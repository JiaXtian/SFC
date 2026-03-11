"""验证集评估脚本：使用训练得到的checkpoint做确定性推理评估。"""
from __future__ import annotations

import argparse
import glob
import json
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
    def __init__(self, top_m=80):
        self.top_m = top_m

    @staticmethod
    def _active_graph(G):
        active = nx.DiGraph()
        active.add_nodes_from(G.nodes(data=True))
        for u, v, d in G.edges(data=True):
            if int(d.get("link_status", 1)) == 1:
                active.add_edge(u, v, **d)
        return active

    def prune(self, G, vnf, prev_node, dest_node, remaining_delay):
        active_graph = self._active_graph(G)
        candidates = []
        try:
            dist_from_prev = nx.single_source_dijkstra_path_length(
                active_graph, prev_node, weight="latency_ms"
            )
            reverse_graph = active_graph.reverse(copy=False)
            dist_to_dest = nx.single_source_dijkstra_path_length(
                reverse_graph, dest_node, weight="latency_ms"
            )
        except Exception:
            return []

        checked = 0
        cpu_req = float(vnf.get("cpu_required", 0.0))
        mem_req = float(vnf.get("mem_required", 0.0))
        disk_req = float(vnf.get("disk_required_gb", 0.0))
        delay_cap = float(remaining_delay)

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
            checked += 1
            if checked > 1800:
                break

            d1 = dist_from_prev.get(node, float("inf"))
            d2 = dist_to_dest.get(node, float("inf"))
            if d1 == float("inf") or d2 == float("inf"):
                continue

            total_d = d1 + d2
            if total_d > delay_cap:
                continue
            candidates.append((node, total_d))
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

    max_steps = max(12, len(request.get("vnf_sequence", [])) + 2)
    for _ in range(max_steps):
        vnf = state.get("vnf")
        prev_node = state.get("prev_node")
        dest_node = state.get("dest_node")
        remaining_delay = state.get("remaining_delay", float("inf"))

        candidates = heuristic.prune(env.topology, vnf, prev_node, dest_node, remaining_delay)
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

        active_graph = trainer._build_active_graph(env.topology)
        try:
            path = nx.shortest_path(active_graph, prev_node, selected, weight="latency_ms")
            delay = float(sum(active_graph[path[i]][path[i + 1]]["latency_ms"] for i in range(len(path) - 1)))
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

    agent = DRLAgent(node_dim=192, vnf_dim=4, context_dim=48, device=args.device)
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
