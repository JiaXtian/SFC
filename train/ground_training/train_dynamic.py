import argparse
import csv
import heapq
import json
import math
import os
import random
import sys
import time
from pathlib import Path

import torch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT_ROOT = Path(__file__).resolve().parents[2]
TRAIN_ROOT = PROJECT_ROOT / "train"

from ground_training.models.drl_agent import DRLAgent
from ground_training.models.gnn_encoder import GNNEncoder
from ground_training.models.model_export import export_models
from ground_training.data_generation.generate_dynamic_multiscale_data import (
    generate_multiscale_dynamic_dataset,
    _parse_scale_plan as parse_dynamic_scale_plan,
)
from ground_training.training.dynamic_dataset import discover_dynamic_pairs, load_dynamic_sequences
from ground_training.training.trainer import SFCTrainer


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
    def _active_graph(G, bw_req=0.0):
        import networkx as nx

        active = nx.DiGraph()
        active.add_nodes_from(G.nodes(data=True))
        for u, v, d in G.edges(data=True):
            if int(d.get("link_status", 1)) != 1:
                continue
            if float(d.get("bandwidth_available_gbps", 0.0)) + 1e-9 < float(bw_req):
                continue
            active.add_edge(u, v, **d)
        return active

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
        blend = (0.7 * softened_product + 0.3 * geometric_mean) if hops <= 6 else (0.82 * softened_product + 0.18 * geometric_mean)
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
        hop_cap = self._compute_hop_cap(current_hops, current_vnf_idx, total_vnfs)
        path, delay, rel, hops = self._dijkstra_constrained(G, source, target, bw_req=float(bw_req), max_hops=hop_cap)
        if path:
            return path, delay, rel, hops
        relaxed_cap = self._compute_relaxed_hop_cap(hop_cap)
        return self._dijkstra_constrained(G, source, target, bw_req=float(bw_req), max_hops=relaxed_cap)

    def prune(
        self,
        G,
        vnf,
        prev_node,
        dest_node,
        remaining_delay,
        top_m=None,
        bandwidth_demand_gbps=0.0,
        current_vnf_idx=0,
        total_vnfs=1,
        accumulated_reliability=1.0,
        reliability_requirement=0.0,
        accumulated_hops=0,
    ):
        import networkx as nx

        if top_m is None:
            top_m = self.top_m

        bw_req = max(float(vnf.get("bandwidth_required_gbps", 0.0)), float(bandwidth_demand_gbps))
        active_graph = self._active_graph(G, bw_req=bw_req)
        candidates = []
        try:
            dist_from_prev = nx.single_source_dijkstra_path_length(active_graph, prev_node, weight="latency_ms")
            reverse_graph = active_graph.reverse(copy=False)
            dist_to_dest = nx.single_source_dijkstra_path_length(reverse_graph, dest_node, weight="latency_ms")
            hop_from_prev = nx.single_source_shortest_path_length(active_graph, prev_node)
            hop_to_dest = nx.single_source_shortest_path_length(reverse_graph, dest_node)
        except Exception:
            return []

        cpu_req = float(vnf.get("cpu_required", 0.0))
        mem_req = float(vnf.get("mem_required", 0.0))
        disk_req = float(vnf.get("disk_required_gb", 0.0))
        delay_cap = float(remaining_delay)
        hop_cap = self._compute_hop_cap(accumulated_hops, current_vnf_idx, total_vnfs)
        relaxed_hop_cap = self._compute_relaxed_hop_cap(hop_cap)
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

        scan_limit = len(valid_nodes)
        for node in valid_nodes[:scan_limit]:
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
            if h1 > relaxed_hop_cap:
                continue
            projected_total_hops = int(accumulated_hops + h1 + max(1, h2))
            if projected_total_hops > self.HARD_TOTAL_HOPS:
                continue
            if projected_total_hops > self.TARGET_TOTAL_HOPS + 4:
                continue

            node_rel = float(G.nodes[node].get("node_reliability", 0.98))
            optimistic_rel = float(accumulated_reliability) * max(1e-9, min(1.0, node_rel)) * future_rel
            if optimistic_rel + 1e-9 < float(reliability_requirement) * 0.72:
                continue

            hop_cost = float(h1) / max(1.0, float(hop_cap))
            hop_over = max(0, projected_total_hops - self.TARGET_TOTAL_HOPS)
            hop_ratio = float(projected_total_hops) / max(1.0, float(self.TARGET_TOTAL_HOPS))
            hop_pressure = max(0.0, hop_ratio - 1.0)
            score = total_d + self.HOP_PENALTY_MS * hop_cost + 3.0 * float(hop_over) + 5.0 * float(hop_pressure)
            candidates.append((node, score))
            if len(candidates) >= top_m * 3:
                break

        candidates.sort(key=lambda x: x[1])
        return [c[0] for c in candidates[:top_m]]


def _save_metrics(history, output_dir="logs"):
    os.makedirs(output_dir, exist_ok=True)
    json_path = os.path.join(output_dir, "dynamic_training_metrics.json")
    with open(json_path, "w") as f:
        json.dump(history, f, indent=2)

    csv_path = os.path.join(output_dir, "dynamic_training_metrics.csv")
    if history:
        keys = list(history[0].keys())
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=keys)
            writer.writeheader()
            for row in history:
                writer.writerow(row)
    return json_path, csv_path


def _split_train_val(sequences, train_ratio=0.8, seed=42):
    data = list(sequences)
    rng = random.Random(seed)
    rng.shuffle(data)
    train_count = max(1, int(len(data) * train_ratio))
    train = data[:train_count]
    val = data[train_count:] if train_count < len(data) else data[-1:]
    return train, val


def _resolve_data_dir(path: str) -> str:
    p = Path(path)
    if p.is_absolute():
        return str(p)
    normalized = str(path).replace("\\", "/")
    candidates: list[Path] = []
    if normalized.startswith("train/"):
        candidates.append(PROJECT_ROOT / p)
    else:
        if normalized.startswith("data/"):
            candidates.append(TRAIN_ROOT / p)
        candidates.append(PROJECT_ROOT / p)
        candidates.append(TRAIN_ROOT / p)
    seen = set()
    uniq_candidates = []
    for c in candidates:
        key = str(c)
        if key in seen:
            continue
        seen.add(key)
        uniq_candidates.append(c)
    for c in uniq_candidates:
        if c.is_dir():
            return str(c)
    return str(uniq_candidates[0]) if uniq_candidates else str(PROJECT_ROOT / p)


def _maybe_expand_dynamic_data(args, current_pair_count: int) -> int:
    if not args.auto_expand_multiscale_data:
        return current_pair_count
    if current_pair_count >= args.min_dynamic_sequences:
        return current_pair_count

    scale_plan = parse_dynamic_scale_plan(args.dynamic_scale_plan)
    print(
        "[dynamic_data] 当前动态序列不足，触发多规模扩展: "
        f"existing={current_pair_count}, min_required={args.min_dynamic_sequences}, "
        f"plan={scale_plan}"
    )
    summary = generate_multiscale_dynamic_dataset(
        scale_plan=scale_plan,
        topology_dir=Path(args.dynamic_topology_dir),
        request_dir=Path(args.dynamic_request_dir),
        step_sec=args.dynamic_step_sec,
        duration_sec=args.dynamic_duration_sec,
        base_requests_per_step=args.dynamic_requests_per_step,
        seed=args.seed + 100000,
        isl_max_distance_km=args.dynamic_isl_max_distance_km,
    )
    print(
        "[dynamic_data] 扩展完成: "
        f"new_sequences={summary['total_sequences']} total_steps={summary['total_steps']} "
        f"estimated_requests={summary['estimated_total_requests']}"
    )

    pairs = discover_dynamic_pairs(args.dynamic_topology_dir, args.dynamic_request_dir)
    return len(pairs)


def main():
    os.chdir(PROJECT_ROOT)
    parser = argparse.ArgumentParser(description="动态拓扑时序训练（阶段2）")
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--heuristic_top_m", type=int, default=110)
    parser.add_argument("--history_window", type=int, default=0)
    parser.add_argument("--context_dim", type=int, default=48)
    parser.add_argument("--max_sequences", type=int, default=0)
    parser.add_argument("--max_steps_per_sequence", type=int, default=0)
    parser.add_argument("--max_requests_per_step", type=int, default=24)
    parser.add_argument("--dynamic_topology_dir", default="data/train/dynamic/topologies")
    parser.add_argument("--dynamic_request_dir", default="data/train/dynamic/requests")
    parser.add_argument("--train_ratio", type=float, default=0.8)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--skip_onnx_export", action="store_true")
    parser.add_argument("--auto_expand_multiscale_data", dest="auto_expand_multiscale_data", action="store_true")
    parser.add_argument("--no_auto_expand_multiscale_data", dest="auto_expand_multiscale_data", action="store_false")
    parser.set_defaults(auto_expand_multiscale_data=True)
    parser.add_argument("--min_dynamic_sequences", type=int, default=12)
    parser.add_argument("--dynamic_scale_plan", type=str, default="800:2,1600:2,2500:3,4000:3,4800:2")
    parser.add_argument("--dynamic_step_sec", type=int, default=5)
    parser.add_argument("--dynamic_duration_sec", type=int, default=180)
    parser.add_argument("--dynamic_requests_per_step", type=int, default=64)
    parser.add_argument("--dynamic_isl_max_distance_km", type=float, default=4200.0)
    parser.add_argument("--backend_align_context", dest="backend_align_context", action="store_true")
    parser.add_argument("--no_backend_align_context", dest="backend_align_context", action="store_false")
    parser.set_defaults(backend_align_context=True)
    args = parser.parse_args()

    random.seed(args.seed)
    torch.manual_seed(args.seed)

    if args.device == "auto":
        if torch.cuda.is_available():
            args.device = "cuda"
        elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            args.device = "mps"
        else:
            args.device = "cpu"

    if args.backend_align_context:
        args.history_window = 0
        min_context_dim = 48
    else:
        min_context_dim = 16 + max(0, args.history_window) * 8
    if args.context_dim < min_context_dim:
        args.context_dim = min_context_dim
        print(f"context_dim 自动提升到 {args.context_dim} (history_window={args.history_window})")

    os.makedirs("models/checkpoints", exist_ok=True)
    os.makedirs("logs", exist_ok=True)
    os.makedirs("models/exported", exist_ok=True)
    os.makedirs("models/exported_dynamic", exist_ok=True)

    args.dynamic_topology_dir = _resolve_data_dir(args.dynamic_topology_dir)
    args.dynamic_request_dir = _resolve_data_dir(args.dynamic_request_dir)

    pairs = discover_dynamic_pairs(args.dynamic_topology_dir, args.dynamic_request_dir)
    pair_count = _maybe_expand_dynamic_data(args, len(pairs))
    if pair_count != len(pairs):
        pairs = discover_dynamic_pairs(args.dynamic_topology_dir, args.dynamic_request_dir)
    if not pairs:
        raise RuntimeError(
            f"未找到动态拓扑/请求对，请检查目录: {args.dynamic_topology_dir} 与 {args.dynamic_request_dir}"
        )
    sequences = load_dynamic_sequences(pairs, max_sequences=args.max_sequences)
    if not sequences:
        raise RuntimeError("动态时序样本为空")

    train_sequences, val_sequences = _split_train_val(sequences, train_ratio=args.train_ratio, seed=args.seed)
    print(f"动态样本序列: total={len(sequences)} train={len(train_sequences)} val={len(val_sequences)}")

    gnn = GNNEncoder(input_dim=8, hidden_dim=192, num_layers=4)
    agent = DRLAgent(node_dim=192, vnf_dim=8, context_dim=args.context_dim, device=args.device)
    agent.actor_optimizer.add_param_group({"params": gnn.parameters(), "lr": 3e-5, "weight_decay": 1e-5})

    trainer = SFCTrainer(
        gnn=gnn,
        agent=agent,
        device=args.device,
        context_dim=args.context_dim,
        history_window=args.history_window,
        backend_align_context=args.backend_align_context,
        log_dir="logs",
    )
    heuristic = HeuristicPruner(top_m=args.heuristic_top_m)
    actor_scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(agent.actor_optimizer, T_max=args.epochs, eta_min=1e-5)
    critic_scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(agent.critic_optimizer, T_max=args.epochs, eta_min=2e-5)

    reliability_curriculum = {
        "start_epoch": 1,
        "end_epoch": max(8, int(args.epochs * 0.75)),
        "min_scale": 0.8,
        "strict_enable_ratio": 0.7,
        "strict_ramp_ratio": 0.2,
    }

    epsilon = 0.28
    best_val_score = -1e9
    history = []
    start_time = time.time()

    for epoch in range(1, args.epochs + 1):
        random.shuffle(train_sequences)
        train_metrics = trainer.train_dynamic_epoch(
            epoch=epoch,
            sequence_data=train_sequences,
            epsilon=epsilon,
            heuristic_pruner=heuristic,
            max_steps_per_sequence=args.max_steps_per_sequence,
            max_requests_per_step=args.max_requests_per_step,
            total_epochs=args.epochs,
            reliability_curriculum=reliability_curriculum,
            update_policy=True,
            deterministic_policy=False,
        )
        val_metrics = trainer.train_dynamic_epoch(
            epoch=epoch,
            sequence_data=val_sequences,
            epsilon=0.0,
            heuristic_pruner=heuristic,
            max_steps_per_sequence=args.max_steps_per_sequence,
            max_requests_per_step=max(8, args.max_requests_per_step // 2),
            total_epochs=args.epochs,
            reliability_curriculum=reliability_curriculum,
            update_policy=False,
            deterministic_policy=True,
        )

        merged = {
            "epoch": epoch,
            "epsilon": epsilon,
            "train_success_rate": train_metrics["success_rate"],
            "train_sla_satisfaction_rate": train_metrics["sla_satisfaction_rate"],
            "train_full_sla_satisfaction_rate": train_metrics["full_sla_satisfaction_rate"],
            "train_avg_algorithm_latency_ms": train_metrics["avg_algorithm_latency_ms"],
            "train_avg_episode_delay_ms": train_metrics["avg_episode_delay_ms"],
            "val_success_rate": val_metrics["success_rate"],
            "val_sla_satisfaction_rate": val_metrics["sla_satisfaction_rate"],
            "val_full_sla_satisfaction_rate": val_metrics["full_sla_satisfaction_rate"],
            "val_avg_algorithm_latency_ms": val_metrics["avg_algorithm_latency_ms"],
            "val_avg_episode_delay_ms": val_metrics["avg_episode_delay_ms"],
            "train_total_requests": train_metrics["total_requests"],
            "val_total_requests": val_metrics["total_requests"],
        }
        history.append(merged)

        val_score = (
            val_metrics["success_rate"] * 0.45
            + val_metrics["sla_satisfaction_rate"] * 0.35
            + val_metrics.get("full_sla_satisfaction_rate", 0.0) * 0.25
            - val_metrics["avg_algorithm_latency_ms"] * 0.2
            - val_metrics["avg_episode_delay_ms"] * 0.05
        )
        if val_score > best_val_score:
            best_val_score = val_score
            agent.save("models/checkpoints/model_dynamic_best.pth")
            torch.save(gnn.state_dict(), "models/checkpoints/gnn_dynamic_best.pth")
            with open("models/checkpoints/model_dynamic_meta.json", "w") as f:
                json.dump(
                    {
                        "context_dim": args.context_dim,
                        "vnf_feature_dim": 8,
                        "history_window": args.history_window,
                        "backend_align_context": args.backend_align_context,
                        "best_actor_checkpoint": "models/checkpoints/model_dynamic_best.pth",
                        "best_gnn_checkpoint": "models/checkpoints/gnn_dynamic_best.pth",
                    },
                    f,
                    indent=2,
                )
            print(
                f"[epoch {epoch}] 新最佳动态模型 "
                f"ValSuccess={val_metrics['success_rate']:.2f}% "
                f"ValSLA={val_metrics['sla_satisfaction_rate']:.2f}% Score={val_score:.2f}"
            )

        if train_metrics.get("updated_episodes", 0) > 0:
            actor_scheduler.step()
            critic_scheduler.step()
        epsilon = max(0.02, epsilon * 0.985)

    agent.save("models/checkpoints/model_dynamic_final.pth")
    torch.save(gnn.state_dict(), "models/checkpoints/gnn_dynamic_final.pth")
    with open("models/checkpoints/model_dynamic_meta.json", "w") as f:
        json.dump(
            {
                "context_dim": args.context_dim,
                "vnf_feature_dim": 8,
                "history_window": args.history_window,
                "backend_align_context": args.backend_align_context,
                "best_actor_checkpoint": "models/checkpoints/model_dynamic_best.pth",
                "best_gnn_checkpoint": "models/checkpoints/gnn_dynamic_best.pth",
                "final_actor_checkpoint": "models/checkpoints/model_dynamic_final.pth",
                "final_gnn_checkpoint": "models/checkpoints/gnn_dynamic_final.pth",
            },
            f,
            indent=2,
        )

    json_path, csv_path = _save_metrics(history, output_dir="logs")
    val_best = max(history, key=lambda x: x["val_success_rate"])
    validation_report = {
        "summary": {
            "best_val_success_rate": val_best["val_success_rate"],
            "best_val_sla_rate": val_best["val_sla_satisfaction_rate"],
            "best_val_full_sla_rate": val_best["val_full_sla_satisfaction_rate"],
            "best_val_algorithm_latency_ms": val_best["val_avg_algorithm_latency_ms"],
            "best_epoch": val_best["epoch"],
            "total_runtime_min": round((time.time() - start_time) / 60.0, 2),
        },
        "history": history,
    }
    validation_report_path = "logs/dynamic_validation_report.json"
    with open(validation_report_path, "w") as f:
        json.dump(validation_report, f, indent=2)

    if not args.skip_onnx_export:
        best_actor = "models/checkpoints/model_dynamic_best.pth"
        best_gnn = "models/checkpoints/gnn_dynamic_best.pth"
        if os.path.exists(best_actor) and os.path.exists(best_gnn):
            export_models(
                gnn_path=best_gnn,
                agent_path=best_actor,
                output_dir="models/exported",
                context_dim=args.context_dim,
            )
            export_models(
                gnn_path=best_gnn,
                agent_path=best_actor,
                output_dir="models/exported_dynamic",
                context_dim=args.context_dim,
            )

    print("\n动态训练完成")
    print(f"  指标JSON: {json_path}")
    print(f"  指标CSV: {csv_path}")
    print(f"  验证报告: {validation_report_path}")


if __name__ == "__main__":
    main()
