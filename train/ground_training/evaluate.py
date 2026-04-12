"""验证集评估脚本：使用训练得到的checkpoint做确定性推理评估。"""
from __future__ import annotations

import argparse
import glob
import heapq
import json
import math
import os
from pathlib import Path
from collections import Counter

import networkx as nx
import torch

from ground_training.environment.sfc_env import SFCEnvironment
from ground_training.models.drl_agent import DRLAgent
from ground_training.models.gnn_encoder import GNNEncoder
from ground_training.training.trainer import SFCTrainer

PROJECT_ROOT = Path(__file__).resolve().parents[2]
TRAIN_ROOT = PROJECT_ROOT / "train"


class HeuristicPruner:
    TARGET_TOTAL_HOPS = 25
    HARD_TOTAL_HOPS = 30
    MIN_LEG_HOP_CAP = 3
    MAX_LEG_HOP_CAP = 10
    RELAXED_LEG_HOP_CAP = 14
    HOP_PENALTY_MS = 2.5
    PATH_SOFTENING_EXPONENT = 0.46
    EXCESS_HOP_RELIABILITY_PENALTY = 0.9988
    FUTURE_STEP_RELIABILITY_DECAY = 0.9990

    def __init__(self, top_m=80):
        self.top_m = top_m

    @staticmethod
    def _clamp01(value: float) -> float:
        return max(0.0, min(1.0, float(value)))

    @classmethod
    def _softened_path_reliability(cls, raw_reliability: float, hops: int) -> float:
        if hops <= 0:
            return 1.0
        raw = max(1e-9, cls._clamp01(raw_reliability))
        geometric_mean = raw ** (1.0 / max(1, hops))
        softened_product = raw ** cls.PATH_SOFTENING_EXPONENT
        if hops <= 6:
            blend = 0.7 * softened_product + 0.3 * geometric_mean
        else:
            blend = 0.82 * softened_product + 0.18 * geometric_mean
        excess_hops = max(0, hops - 6)
        return cls._clamp01(blend * (cls.EXCESS_HOP_RELIABILITY_PENALTY ** excess_hops))

    @classmethod
    def _compute_hop_cap(cls, current_hops: int, current_vnf_idx: int, total_vnfs: int) -> int:
        remaining_vnfs = max(0, int(total_vnfs) - int(current_vnf_idx))
        remaining_legs = max(1, remaining_vnfs + 1)
        remaining_budget = max(cls.MIN_LEG_HOP_CAP, cls.TARGET_TOTAL_HOPS - max(0, int(current_hops)))
        per_leg = remaining_budget // remaining_legs
        return max(cls.MIN_LEG_HOP_CAP, min(cls.MAX_LEG_HOP_CAP, per_leg + 2))

    @classmethod
    def _compute_relaxed_hop_cap(cls, hop_cap: int) -> int:
        return min(max(hop_cap, cls.MIN_LEG_HOP_CAP) + 3, cls.RELAXED_LEG_HOP_CAP)

    @staticmethod
    def _active_graph(G, bw_req=0.0):
        active = nx.DiGraph()
        active.add_nodes_from(G.nodes(data=True))
        for u, v, d in G.edges(data=True):
            if int(d.get("link_status", 1)) != 1:
                continue
            if float(d.get("bandwidth_available_gbps", 0.0)) + 1e-9 < float(bw_req):
                continue
            active.add_edge(u, v, **d)
        return active

    @classmethod
    def _estimate_link_reliability(cls, edge: dict) -> float:
        if int(edge.get("link_status", 1)) != 1:
            return 0.0
        status = str(edge.get("status", "active")).lower()
        if status == "down":
            return 0.0
        bw_total = float(edge.get("bandwidth_gbps", 0.0))
        bw_avail = float(edge.get("bandwidth_available_gbps", 0.0))
        bw_ratio = cls._clamp01(bw_avail / bw_total) if bw_total > 1e-9 else 0.0
        base = cls._clamp01(float(edge.get("link_reliability", edge.get("reliability", 0.98))))
        bandwidth_factor = 0.98 + 0.02 * bw_ratio
        status_penalty = 0.985 if status == "congested" else 1.0
        return cls._clamp01(base * bandwidth_factor * status_penalty)

    @classmethod
    def _dijkstra_constrained(cls, G, source, target, bw_req: float, max_hops: int):
        if source == target:
            return [source], 0.0, 1.0, 0
        if source not in G or target not in G:
            return [], float("inf"), 0.0, 0

        dist = {source: 0.0}
        latency = {source: 0.0}
        hops = {source: 0}
        reliability_raw = {source: 1.0}
        prev = {}
        pq = [(0.0, source)]

        while pq:
            cur_cost, u = heapq.heappop(pq)
            if cur_cost > dist.get(u, float("inf")) + 1e-9:
                continue
            if u == target:
                break
            for v, edge in G[u].items():
                if int(edge.get("link_status", 1)) != 1:
                    continue
                if float(edge.get("bandwidth_available_gbps", 0.0)) + 1e-9 < bw_req:
                    continue

                next_hops = hops[u] + 1
                if max_hops > 0 and next_hops > max_hops:
                    continue
                next_latency = latency[u] + float(edge.get("latency_ms", 0.0))
                next_cost = next_latency + cls.HOP_PENALTY_MS * next_hops
                edge_rel = max(1e-9, cls._estimate_link_reliability(edge))
                next_rel = reliability_raw[u] * edge_rel

                old_cost = dist.get(v, float("inf"))
                old_latency = latency.get(v, float("inf"))
                old_rel = reliability_raw.get(v, 0.0)
                should_update = (
                    next_cost + 1e-9 < old_cost
                    or (
                        abs(next_cost - old_cost) <= 1e-9
                        and (
                            next_latency + 1e-9 < old_latency
                            or (abs(next_latency - old_latency) <= 1e-9 and next_rel > old_rel + 1e-9)
                        )
                    )
                )
                if not should_update:
                    continue
                dist[v] = next_cost
                latency[v] = next_latency
                hops[v] = next_hops
                reliability_raw[v] = next_rel
                prev[v] = u
                heapq.heappush(pq, (next_cost, v))

        if target not in prev and target != source:
            return [], float("inf"), 0.0, 0

        path = [target]
        while path[-1] != source:
            nxt = prev.get(path[-1])
            if nxt is None:
                return [], float("inf"), 0.0, 0
            path.append(nxt)
        path.reverse()
        hop_count = max(0, len(path) - 1)
        softened_rel = cls._softened_path_reliability(reliability_raw.get(target, 1.0), hop_count)
        return path, float(latency.get(target, 0.0)), float(softened_rel), hop_count

    def find_path(
        self,
        G,
        source,
        target,
        bw_req=0.0,
        current_hops=0,
        current_vnf_idx=0,
        total_vnfs=1,
    ):
        active_graph = self._active_graph(G, bw_req=float(bw_req))
        hop_cap = self._compute_hop_cap(current_hops, current_vnf_idx, total_vnfs)
        path, delay, rel, hops = self._dijkstra_constrained(
            active_graph, source, target, bw_req=float(bw_req), max_hops=hop_cap
        )
        if path:
            return path, delay, rel, hops
        relaxed_cap = self._compute_relaxed_hop_cap(hop_cap)
        return self._dijkstra_constrained(
            active_graph, source, target, bw_req=float(bw_req), max_hops=relaxed_cap
        )

    def prune(
        self,
        G,
        vnf,
        prev_node,
        dest_node,
        remaining_delay,
        bandwidth_demand_gbps=0.0,
        current_vnf_idx=0,
        total_vnfs=1,
        accumulated_reliability=1.0,
        reliability_requirement=0.0,
        accumulated_hops=0,
        **_kwargs,
    ):
        bw_req = max(float(vnf.get("bandwidth_required_gbps", 0.0)), float(bandwidth_demand_gbps))
        active_graph = self._active_graph(G, bw_req=bw_req)
        candidates = []
        try:
            dist_from_prev = nx.single_source_dijkstra_path_length(
                active_graph, prev_node, weight="latency_ms"
            )
            reverse_graph = active_graph.reverse(copy=False)
            dist_to_dest = nx.single_source_dijkstra_path_length(
                reverse_graph, dest_node, weight="latency_ms"
            )
            hop_from_prev = nx.single_source_shortest_path_length(active_graph, prev_node)
            hop_to_dest = nx.single_source_shortest_path_length(reverse_graph, dest_node)
        except Exception:
            return []

        cpu_req = float(vnf.get("cpu_required", 0.0))
        mem_req = float(vnf.get("mem_required", 0.0))
        disk_req = float(vnf.get("disk_required_gb", 0.0))
        delay_cap = float(remaining_delay)
        hop_cap = self._compute_hop_cap(accumulated_hops, current_vnf_idx, total_vnfs)
        relaxed_cap = self._compute_relaxed_hop_cap(hop_cap)
        remaining_steps = max(0, int(total_vnfs) - int(current_vnf_idx))
        future_rel = self.FUTURE_STEP_RELIABILITY_DECAY ** remaining_steps

        valid_nodes = [
            n
            for n in G.nodes()
            if (
                G.nodes[n].get("cpu_available", 0.0) >= cpu_req
                and G.nodes[n].get("mem_available", 0.0) >= mem_req
                and G.nodes[n].get("disk_available", 0.0) >= disk_req
            )
        ]

        for node in valid_nodes:
            d1 = dist_from_prev.get(node, float("inf"))
            d2 = dist_to_dest.get(node, float("inf"))
            if d1 == float("inf") or d2 == float("inf"):
                continue

            total_d = d1 + d2
            if total_d > delay_cap:
                continue
            h1 = hop_from_prev.get(node, math.inf)
            h2 = hop_to_dest.get(node, math.inf)
            if h1 == math.inf or h2 == math.inf:
                continue
            if h1 > relaxed_cap:
                continue
            projected_hops = int(accumulated_hops + h1 + max(1, h2))
            if projected_hops > self.HARD_TOTAL_HOPS:
                continue
            if projected_hops > self.TARGET_TOTAL_HOPS + 4:
                continue

            node_rel = float(G.nodes[node].get("node_reliability", 0.98))
            optimistic_rel = float(accumulated_reliability) * max(1e-9, min(1.0, node_rel)) * future_rel
            if optimistic_rel + 1e-9 < float(reliability_requirement) * 0.72:
                continue

            hop_over = max(0, projected_hops - self.TARGET_TOTAL_HOPS)
            hop_ratio = float(projected_hops) / max(1.0, float(self.TARGET_TOTAL_HOPS))
            hop_pressure = max(0.0, hop_ratio - 1.0)
            score = (
                total_d
                + self.HOP_PENALTY_MS * float(h1) / max(1.0, float(hop_cap))
                + 3.0 * float(hop_over)
                + 5.0 * float(hop_pressure)
            )
            candidates.append((node, score))
            if len(candidates) >= self.top_m * 3:
                break

        candidates.sort(key=lambda x: x[1])
        return [c[0] for c in candidates[: self.top_m]]


def _read_request_meta(req_path):
    try:
        with open(req_path) as f:
            req = json.load(f)
        meta = req.get("metadata", {})
        return {
            "topology_file": meta.get("topology_file"),
        }
    except Exception:
        return {"topology_file": None}


def _pair_val_data(topologies, requests):
    topo_by_name = {os.path.basename(p): p for p in topologies}
    pairs = []
    for idx, req_file in enumerate(requests):
        meta = _read_request_meta(req_file)
        topo_file = topo_by_name.get(meta.get("topology_file"))
        if topo_file is None:
            topo_file = topologies[idx % len(topologies)] if topologies else None
        if topo_file:
            pairs.append((topo_file, req_file))
    return pairs


def _episode_infer(env, trainer, gnn, agent, heuristic, request):
    state = env.reset(request, reset_resources=True)
    info = {}

    max_steps = max(12, len(request.get("vnf_sequence", request.get("core_nf_sequence", []))) + 2)
    for _ in range(max_steps):
        vnf = state.get("vnf")
        prev_node = state.get("prev_node")
        dest_node = state.get("dest_node")
        remaining_delay = state.get("remaining_delay", float("inf"))

        candidates = heuristic.prune(
            env.topology,
            vnf,
            prev_node,
            dest_node,
            remaining_delay,
            current_vnf_idx=int(state.get("current_vnf_idx", 0)),
            total_vnfs=int(state.get("total_vnfs", 1)),
            accumulated_reliability=float(state.get("accumulated_reliability", 1.0)),
            reliability_requirement=float(state.get("reliability_requirement", 0.0)),
            accumulated_hops=int(state.get("accumulated_hops", 0)),
        )
        if not candidates:
            return False, "no_candidates", float(state.get("accumulated_delay", 0.0)), float(state.get("accumulated_reliability", 0.0))

        node_features, edge_index, nodes_list, node_index = trainer._get_graph_data(env.topology)
        candidate_indices = [node_index[c] for c in candidates if c in node_index]
        if not candidate_indices:
            return False, "invalid_candidates", float(state.get("accumulated_delay", 0.0)), float(state.get("accumulated_reliability", 0.0))

        with torch.no_grad():
            node_embeddings = gnn(node_features, edge_index)

        vnf_feat = trainer._build_vnf_features(vnf).to(trainer.device)
        ctx_feat = trainer._build_context_features(state).to(trainer.device)

        action_idx, _ = agent.select_action(
            node_embeddings,
            candidate_indices,
            vnf_feat,
            ctx_feat,
            deterministic=True,
        )
        action_idx = int(max(0, min(action_idx, len(candidate_indices) - 1)))
        selected = nodes_list[candidate_indices[action_idx]]

        try:
            path, delay, _, _ = heuristic.find_path(
                env.topology,
                prev_node,
                selected,
                bw_req=float(state.get("bandwidth_demand_gbps", 0.0)),
                current_hops=int(state.get("accumulated_hops", 0)),
                current_vnf_idx=int(state.get("current_vnf_idx", 0)),
                total_vnfs=int(state.get("total_vnfs", 1)),
            )
            if not path:
                raise RuntimeError("no_constrained_path")
        except Exception:
            return False, "path_error", float(state.get("accumulated_delay", 0.0)), float(state.get("accumulated_reliability", 0.0))

        next_state, _, done, info = env.step(selected, path, delay)
        if done:
            break
        state = next_state

    success = bool(info.get("success", False)) if isinstance(info, dict) else False
    reason = str(info.get("failure_reason", "")) if isinstance(info, dict) else "unknown"
    delay = float(info.get("accumulated_delay", state.get("accumulated_delay", 0.0))) if isinstance(info, dict) else 0.0
    reliability = float(info.get("accumulated_reliability", state.get("accumulated_reliability", 0.0))) if isinstance(info, dict) else 0.0
    return success, reason, delay, reliability


def main():
    os.chdir(PROJECT_ROOT)
    parser = argparse.ArgumentParser()
    parser.add_argument("--model_checkpoint", default="models/checkpoints/model_best.pth")
    parser.add_argument("--gnn_checkpoint", default="models/checkpoints/gnn_best.pth")
    parser.add_argument("--val_topology_dir", default="data/val/topologies")
    parser.add_argument("--val_requests_dir", default="data/val/requests")
    parser.add_argument("--max_requests_per_file", type=int, default=120)
    parser.add_argument("--top_m", type=int, default=100)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--output_json", default="logs/val_eval.json")
    args = parser.parse_args()

    val_topology_dir = Path(args.val_topology_dir)
    val_requests_dir = Path(args.val_requests_dir)
    if not val_topology_dir.is_absolute():
        if str(val_topology_dir).replace("\\", "/").startswith("data/"):
            val_topology_dir = TRAIN_ROOT / val_topology_dir
        else:
            val_topology_dir = PROJECT_ROOT / val_topology_dir
    if not val_requests_dir.is_absolute():
        if str(val_requests_dir).replace("\\", "/").startswith("data/"):
            val_requests_dir = TRAIN_ROOT / val_requests_dir
        else:
            val_requests_dir = PROJECT_ROOT / val_requests_dir

    model_checkpoint = Path(args.model_checkpoint)
    gnn_checkpoint = Path(args.gnn_checkpoint)
    if not model_checkpoint.is_absolute():
        model_checkpoint = PROJECT_ROOT / model_checkpoint
    if not gnn_checkpoint.is_absolute():
        gnn_checkpoint = PROJECT_ROOT / gnn_checkpoint

    topologies = sorted(glob.glob(str(val_topology_dir / "*.json")))
    requests = sorted(glob.glob(str(val_requests_dir / "*.json")))
    if not topologies or not requests:
        raise RuntimeError("验证数据缺失，请检查 data/val/topologies 与 data/val/requests")

    gnn = GNNEncoder(input_dim=8, hidden_dim=192, num_layers=4).to(args.device)
    gnn.load_state_dict(torch.load(str(gnn_checkpoint), map_location=args.device))
    gnn.eval()

    agent = DRLAgent(node_dim=192, vnf_dim=8, context_dim=48, device=args.device)
    agent.load(str(model_checkpoint), load_optimizer=False)
    agent.actor.eval()
    agent.critic.eval()

    trainer = SFCTrainer(gnn, agent, args.device)
    heuristic = HeuristicPruner(top_m=args.top_m)

    pairs = _pair_val_data(topologies, requests)
    total = 0
    succ = 0
    sla = 0
    full_sla = 0
    fail_counter = Counter()
    delays = []

    for topo_file, req_file in pairs:
        with open(topo_file) as f:
            topo_data = json.load(f)
        graph = trainer._build_graph_from_json(topo_data)

        with open(req_file) as f:
            req_data = json.load(f)

        env = SFCEnvironment(graph, args.device, shared_resources=False)
        for request in req_data.get("requests", [])[: args.max_requests_per_file]:
            total += 1
            success, reason, ep_delay, ep_rel = _episode_infer(env, trainer, gnn, agent, heuristic, request)
            delays.append(ep_delay)
            if success:
                succ += 1
                sla_target = float(request.get("sla", {}).get("latency_requirement_ms", request.get("max_latency_ms", 1e9)))
                rel_target = float(request.get("sla", {}).get("reliability_requirement", request.get("reliability_requirement", 0.0)))
                if ep_delay <= sla_target and ep_rel >= rel_target:
                    sla += 1
                    full_sla += 1
            else:
                fail_counter[reason or "unknown"] += 1

    result = {
        "total_requests": total,
        "success_rate": (100.0 * succ / total) if total else 0.0,
        "sla_satisfaction_rate": (100.0 * sla / total) if total else 0.0,
        "full_sla_satisfaction_rate": (100.0 * full_sla / total) if total else 0.0,
        "avg_episode_delay_ms": (sum(delays) / len(delays)) if delays else 0.0,
        "top_failure_reasons": fail_counter.most_common(8),
    }

    os.makedirs(os.path.dirname(args.output_json) or ".", exist_ok=True)
    with open(args.output_json, "w") as f:
        json.dump(result, f, indent=2)

    print("Validation done.")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
