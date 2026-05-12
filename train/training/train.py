
import argparse
import csv
import glob
import heapq
import json
import math
import os
from pathlib import Path
import random
import sys
import time

os.environ.setdefault("MPLCONFIGDIR", os.path.join(os.getcwd(), "logs", ".mplconfig"))
os.environ.setdefault("MPLBACKEND", "Agg")

import matplotlib.pyplot as plt
import networkx as nx
import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT_ROOT = Path(__file__).resolve().parents[2]
TRAIN_ROOT = PROJECT_ROOT / "train"

from training.models.drl_agent import DRLAgent
from training.models.gnn_encoder import GNNEncoder
from training.training.trainer import SFCTrainer
from training.open5gs_profile import (
    BUSINESS_DIMENSIONS,
    CONTEXT_FEATURE_DIM,
    CORE_NF_FEATURE_DIM,
    GNN_EMBEDDING_DIM,
    NODE_FEATURE_DIM,
    CORE_NF_TYPES,
)


class HeuristicPruner:
    HOP_PENALTY_MS = 2.0

    def __init__(self, top_m=80):
        self.top_m = int(max(16, top_m))
        self.fast_prefilter_limit = max(96, self.top_m * 3)

    def _prefilter_limit(self, graph_size, top_m):
        top_m = int(max(16, top_m))
        if graph_size >= 4000:
            return min(self.fast_prefilter_limit, max(top_m + 8, int(top_m * 1.10)))
        if graph_size >= 2000:
            return min(self.fast_prefilter_limit, max(top_m + 16, int(top_m * 1.25)))
        return self.fast_prefilter_limit

    @staticmethod
    def _nf_type(vnf: dict) -> str:
        return str(vnf.get("nf_type", vnf.get("core_nf_type", vnf.get("vnf_type", "")))).lower().replace("-", "_").replace(" ", "_")

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
    def _dijkstra_constrained(cls, G, source, target, bw_req: float, max_hops: int):
        if source == target:
            return [source], 0.0, 0
        if source not in G or target not in G:
            return [], float("inf"), 0
        dist = {source: 0.0}
        latency = {source: 0.0}
        hops = {source: 0}
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
                if next_cost + 1e-9 >= dist.get(v, float("inf")):
                    continue
                dist[v] = next_cost
                latency[v] = next_latency
                hops[v] = next_hops
                prev[v] = u
                heapq.heappush(pq, (next_cost, v))
        if target not in prev:
            return [], float("inf"), 0
        path = [target]
        while path[-1] != source:
            p = prev.get(path[-1])
            if p is None:
                return [], float("inf"), 0
            path.append(p)
        path.reverse()
        return path, float(latency.get(target, 0.0)), max(0, len(path) - 1)

    def _score_dependency_paths(self, G, node, nf_type, deployed_by_type, dependencies, max_hops):
        total_delay = 0.0
        total_hops = 0
        checked = 0
        worst_bottleneck_pressure = 0.0
        for dep in dependencies:
            src_type = str(dep.get("source", "")).lower()
            dst_type = str(dep.get("target", "")).lower()
            if src_type == nf_type and dst_type in deployed_by_type:
                src_node = node
                dst_node = deployed_by_type[dst_type]["node"]
            elif dst_type == nf_type and src_type in deployed_by_type:
                src_node = deployed_by_type[src_type]["node"]
                dst_node = node
            else:
                continue
            bw_req = float(dep.get("bandwidth_required_gbps", 0.0))
            path, delay, hops = self._dijkstra_constrained(G, src_node, dst_node, bw_req, max_hops)
            if not path:
                return None
            checked += 1
            total_delay += delay * float(dep.get("latency_weight", 1.0))
            total_hops += hops
            for i in range(len(path) - 1):
                edge = G[path[i]][path[i + 1]]
                total_bw = max(float(edge.get("bandwidth_gbps", 0.0)), 1e-6)
                pressure = 1.0 - float(edge.get("bandwidth_available_gbps", 0.0)) / total_bw
                worst_bottleneck_pressure = max(worst_bottleneck_pressure, pressure)
        return total_delay, total_hops, checked, worst_bottleneck_pressure

    def prune(
        self,
        G,
        vnf,
        deployed_by_type=None,
        dependencies=None,
        remaining_delay=float("inf"),
        top_m=None,
        current_nf_idx=0,
        total_core_nfs=12,
        accumulated_delay=0.0,
        reliability_requirement=0.0,
        accumulated_hops=0,
        max_dependency_hops=16,
        **_kwargs,
    ):
        del current_nf_idx, total_core_nfs, accumulated_delay, reliability_requirement, accumulated_hops
        if top_m is None:
            top_m = self.top_m
        deployed_by_type = deployed_by_type or {}
        dependencies = dependencies or []
        nf_type = self._nf_type(vnf)
        cpu_req = float(vnf.get("cpu_required", 0.0))
        mem_req = float(vnf.get("mem_required", 0.0))
        disk_req = float(vnf.get("disk_required_gb", 0.0))
        business = vnf.get("business_load_demand", {})
        fast_candidates = []

        for node_id, node in G.nodes(data=True):
            if (
                float(node.get("cpu_available", 0.0)) + 1e-9 < cpu_req
                or float(node.get("mem_available", 0.0)) + 1e-9 < mem_req
                or float(node.get("disk_available", 0.0)) + 1e-9 < disk_req
            ):
                continue
            cpu_total = max(float(node.get("cpu_total", 1.0)), 1e-6)
            mem_total = max(float(node.get("mem_total", 1.0)), 1e-6)
            disk_total = max(float(node.get("disk_total", 1.0)), 1e-6)
            projected_resource_util = np.mean(
                [
                    1.0 - (float(node.get("cpu_available", 0.0)) - cpu_req) / cpu_total,
                    1.0 - (float(node.get("mem_available", 0.0)) - mem_req) / mem_total,
                    1.0 - (float(node.get("disk_available", 0.0)) - disk_req) / disk_total,
                ]
            )
            projected_resource_peak = max(
                [
                    1.0 - (float(node.get("cpu_available", 0.0)) - cpu_req) / cpu_total,
                    1.0 - (float(node.get("mem_available", 0.0)) - mem_req) / mem_total,
                    1.0 - (float(node.get("disk_available", 0.0)) - disk_req) / disk_total,
                ]
            )
            load = node.get("core_business_load", {})
            projected_business_max = max(
                float(load.get(dim, node.get(dim, 0.0))) + float(business.get(dim, 0.0))
                for dim in BUSINESS_DIMENSIONS
            )
            co_location = float(node.get("deployed_core_nf_count", 0))
            upf_hotspot_penalty = 5.0 * max(0.0, projected_business_max - 0.55) if nf_type == "upf" else 0.0
            fast_score = (
                5.0 * projected_business_max
                + 4.0 * projected_resource_peak
                + 2.0 * projected_resource_util
                + 22.0 * max(0.0, projected_business_max - 0.72)
                + 10.0 * max(0.0, projected_resource_peak - 0.72)
                + 2.0 * co_location
                + upf_hotspot_penalty
                - 0.25 * float(G.out_degree(node_id))
            )
            fast_candidates.append(
                (
                    node_id,
                    float(fast_score),
                    projected_resource_util,
                    projected_resource_peak,
                    projected_business_max,
                    co_location,
                    upf_hotspot_penalty,
                )
            )

        if not fast_candidates:
            return []
        if not deployed_by_type:
            best_fast = heapq.nsmallest(int(top_m), fast_candidates, key=lambda item: item[1])
            return [node for node, *_ in best_fast]

        candidates = []
        prefilter_limit = min(len(fast_candidates), self._prefilter_limit(len(G), top_m))
        best_fast = heapq.nsmallest(prefilter_limit, fast_candidates, key=lambda item: item[1])
        for (
            node_id,
            _fast_score,
            projected_resource_util,
            projected_resource_peak,
            projected_business_max,
            co_location,
            upf_hotspot_penalty,
        ) in best_fast:
            dep_score = self._score_dependency_paths(G, node_id, nf_type, deployed_by_type, dependencies, int(max_dependency_hops))
            if dep_score is None:
                continue
            dep_delay, dep_hops, dep_checked, bottleneck_pressure = dep_score
            if dep_delay > float(remaining_delay) * 1.08:
                continue
            dependency_bonus = -2.0 * dep_checked
            score = (
                dep_delay
                + 1.7 * dep_hops
                + 6.0 * projected_business_max
                + 4.0 * projected_resource_peak
                + 2.0 * projected_resource_util
                + 18.0 * max(0.0, projected_business_max - 0.78)
                + 7.0 * max(0.0, projected_resource_peak - 0.78)
                + 6.0 * bottleneck_pressure
                + 2.0 * co_location
                + upf_hotspot_penalty
                + dependency_bonus
            )
            candidates.append((node_id, float(score)))

        candidates.sort(key=lambda item: item[1])
        return [node for node, _ in candidates[: int(top_m)]]


def save_metrics(history, output_dir="logs"):
    os.makedirs(output_dir, exist_ok=True)

    json_path = os.path.join(output_dir, "training_metrics.json")
    with open(json_path, "w") as f:
        json.dump(history, f, indent=2)

    csv_path = os.path.join(output_dir, "training_metrics.csv")
    if history:
        keys = list(history[0].keys())
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=keys)
            writer.writeheader()
            for row in history:
                writer.writerow(row)

    plot_path = os.path.join(output_dir, "training_performance.png")
    if history:
        epochs = [h["epoch"] for h in history]

        fig, axes = plt.subplots(2, 2, figsize=(14, 10))

        axes[0, 0].plot(epochs, [h["success_rate"] for h in history], label="Deploy Success%", color="#1f77b4")
        axes[0, 0].plot(epochs, [h["sla_satisfaction_rate"] for h in history], label="SLA Satisfaction%", color="#ff7f0e")
        axes[0, 0].plot(
            epochs,
            [h.get("full_sla_satisfaction_rate", h["sla_satisfaction_rate"]) for h in history],
            label="Full SLA Satisfaction%",
            color="#2ca02c",
        )
        axes[0, 0].set_title("Success / SLA")
        axes[0, 0].set_xlabel("Epoch")
        axes[0, 0].set_ylabel("Rate (%)")
        axes[0, 0].legend()
        axes[0, 0].grid(alpha=0.3)

        axes[0, 1].plot(epochs, [h["avg_reward"] for h in history], label="Train Reward", color="#2ca02c")
        if "eval_avg_reward" in history[0]:
            axes[0, 1].plot(epochs, [h.get("eval_avg_reward", 0.0) for h in history], label="Eval Reward", color="#1f77b4")
        if "eval_quality_best_so_far" in history[0]:
            axes[0, 1].plot(
                epochs,
                [100.0 * h.get("eval_quality_best_so_far", 0.0) for h in history],
                label="Eval Quality Best x100",
                color="#9467bd",
            )
        axes[0, 1].set_title("Average Reward")
        axes[0, 1].set_xlabel("Epoch")
        axes[0, 1].set_ylabel("Reward")
        axes[0, 1].legend()
        axes[0, 1].grid(alpha=0.3)

        axes[1, 0].plot(epochs, [h["avg_episode_delay_ms"] for h in history], label="Deployment Delay", color="#9467bd")
        axes[1, 0].set_title("Deployment Delay")
        axes[1, 0].set_xlabel("Epoch")
        axes[1, 0].set_ylabel("ms")
        axes[1, 0].grid(alpha=0.3)

        axes[1, 1].plot(epochs, [h["avg_algorithm_latency_ms"] for h in history], label="Avg Algorithm Latency", color="#d62728")
        axes[1, 1].plot(epochs, [h["p95_algorithm_latency_ms"] for h in history], label="P95 Algorithm Latency", color="#8c564b")
        if "eval_p95_algorithm_latency_ms" in history[0]:
            axes[1, 1].plot(
                epochs,
                [h.get("eval_p95_algorithm_latency_ms", 0.0) for h in history],
                label="Eval P95 Algorithm Latency",
                color="#ff7f0e",
            )
        axes[1, 1].axhline(500.0, color="#111111", linestyle="--", linewidth=1.0, alpha=0.5)
        axes[1, 1].set_title("Algorithm Processing Latency")
        axes[1, 1].set_xlabel("Epoch")
        axes[1, 1].set_ylabel("ms")
        axes[1, 1].legend()
        axes[1, 1].grid(alpha=0.3)

        plt.tight_layout()
        plt.savefig(plot_path, dpi=180)
        plt.close(fig)

    return json_path, csv_path, plot_path


def _read_topology_scale(topo_path):
    try:
        with open(topo_path) as f:
            topo = json.load(f)
        return int(topo.get("metadata", {}).get("total_satellites", 0))
    except Exception:
        return 0


def _read_request_scale(req_path):
    try:
        with open(req_path) as f:
            req = json.load(f)
        return int(req.get("metadata", {}).get("topology_scale", 0))
    except Exception:
        return 0


def _read_request_meta(req_path):
    try:
        with open(req_path) as f:
            req = json.load(f)
        meta = req.get("metadata", {})
        return {
            "topology_file": meta.get("topology_file"),
            "generation_seed": meta.get("generation_seed"),
            "topology_scale": meta.get("topology_scale", 0),
        }
    except Exception:
        return {
            "topology_file": None,
            "generation_seed": None,
            "topology_scale": 0,
        }


def _load_topology_nodes(topo_path):
    try:
        with open(topo_path) as f:
            topo = json.load(f)
        return {n.get("id") for n in topo.get("topology", {}).get("nodes", []) if n.get("id")}
    except Exception:
        return set()


def _pair_request_to_topology(req_file, topo_files, topo_by_name, topo_nodes_cache):
    meta = _read_request_meta(req_file)

    topo_name = meta.get("topology_file")
    if topo_name:
        mapped = topo_by_name.get(topo_name)
        if mapped:
            return mapped

    return topo_files[0] if topo_files else None


def _is_request_file_compatible(req_file, topo_nodes, sample_limit=24):
    del topo_nodes
    try:
        with open(req_file) as f:
            req = json.load(f)
        requests = req.get("requests", [])
        if not requests:
            return False
        for r in requests[:sample_limit]:
            core_nfs = r.get("core_nfs", r.get("core_nf_sequence", r.get("vnf_sequence", [])))
            if core_nfs and len(core_nfs) not in {len(CORE_NF_TYPES), 0}:
                # Older partial-chain files are still usable because the
                # environment now completes missing open5gs NFs deterministically.
                continue
            if not isinstance(r, dict):
                return False
        return True
    except Exception:
        return False


def _build_scale_balanced_data(train_topos, train_reqs):
    if not train_topos or not train_reqs:
        return []

    topo_by_name = {os.path.basename(tp): tp for tp in train_topos}
    topo_nodes_cache = {tp: _load_topology_nodes(tp) for tp in train_topos}

    pair_by_scale = {}
    skipped = 0
    for req_file in train_reqs:
        topo_file = _pair_request_to_topology(req_file, train_topos, topo_by_name, topo_nodes_cache)
        if not topo_file:
            skipped += 1
            continue
        if not _is_request_file_compatible(req_file, topo_nodes_cache[topo_file]):
            skipped += 1
            continue
        scale = _read_topology_scale(topo_file)
        scale_key = int(round(scale / 100.0) * 100) if scale > 0 else 0
        pair_by_scale.setdefault(scale_key, []).append((topo_file, req_file))

    balanced = []
    scale_keys = sorted(pair_by_scale.keys())
    max_len = max(len(v) for v in pair_by_scale.values()) if scale_keys else 0
    for idx in range(max_len):
        for scale in scale_keys:
            bucket = pair_by_scale.get(scale, [])
            if idx < len(bucket):
                balanced.append(bucket[idx])
    if skipped > 0:
        print(f"训练数据过滤: 跳过 {skipped} 个空文件或无法读取的请求文件")
    return balanced


def _select_scale_coverage(data_pairs, max_files):
    if not data_pairs or max_files <= 0:
        return []
    by_scale = {}
    for topo_file, req_file in data_pairs:
        scale = _read_topology_scale(topo_file)
        scale_key = int(round(scale / 100.0) * 100) if scale > 0 else 0
        by_scale.setdefault(scale_key, []).append((topo_file, req_file))
    selected = []
    scale_keys = sorted(by_scale.keys())
    cursor = 0
    while len(selected) < max_files and scale_keys:
        progressed = False
        for scale in scale_keys:
            bucket = by_scale[scale]
            if cursor < len(bucket):
                selected.append(bucket[cursor])
                progressed = True
                if len(selected) >= max_files:
                    break
        if not progressed:
            break
        cursor += 1
    return selected


def _training_quality_score(metrics, prefix=""):
    """Scale-independent checkpoint score.

    Raw algorithm latency grows with topology size, so using it directly made
    warmup epochs look better than full-scale epochs.  This score emphasizes
    deployment quality and only applies a bounded speed term.
    """
    success = float(metrics.get(f"{prefix}success_rate", metrics.get("success_rate", 0.0)))
    full_sla = float(
        metrics.get(
            f"{prefix}full_sla_satisfaction_rate",
            metrics.get(f"{prefix}sla_satisfaction_rate", metrics.get("sla_satisfaction_rate", success)),
        )
    )
    dep = float(metrics.get(f"{prefix}dependency_satisfaction_rate", metrics.get("dependency_satisfaction_rate", 0.0)))
    quality = 100.0 * float(metrics.get(f"{prefix}avg_quality_score", metrics.get("avg_quality_score", 0.0)))
    resource = 100.0 * float(
        metrics.get(
            f"{prefix}placement_resource_score",
            metrics.get(f"{prefix}resource_balance_score", metrics.get("resource_balance_score", 0.0)),
        )
    )
    business = 100.0 * float(
        metrics.get(
            f"{prefix}placement_business_score",
            metrics.get(f"{prefix}business_balance_score", metrics.get("business_balance_score", 0.0)),
        )
    )
    future = 100.0 * float(
        metrics.get(
            f"{prefix}future_feasibility_score",
            metrics.get("future_feasibility_score", 0.0),
        )
    )
    link_metric = float(
        metrics.get(
            f"{prefix}used_link_congestion_score",
            metrics.get(f"{prefix}link_congestion_score", metrics.get("link_congestion_score", 0.0)),
        )
    )
    link_good = 100.0 * (1.0 - link_metric)
    alg_latency = float(metrics.get(f"{prefix}avg_algorithm_latency_ms", metrics.get("avg_algorithm_latency_ms", 0.0)))
    speed = 100.0 * max(0.0, 1.0 - min(1.0, alg_latency / 500.0))
    return (
        0.24 * success
        + 0.18 * full_sla
        + 0.10 * dep
        + 0.18 * quality
        + 0.10 * resource
        + 0.07 * future
        + 0.05 * business
        + 0.05 * link_good
        + 0.03 * speed
    )


def main():
    os.chdir(PROJECT_ROOT)
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=80, help="训练轮次")
    parser.add_argument("--device", default="auto", help="训练设备(auto/cpu/cuda/mps)")
    parser.add_argument("--heuristic_top_m", type=int, default=64, help="候选剪枝上限")
    parser.add_argument("--max_requests_per_file", type=int, default=10)
    parser.add_argument("--shared_resources_prob", type=float, default=0.4)
    parser.add_argument("--max_data_files", type=int, default=16, help="每个epoch最多使用的训练文件数，0表示全部")
    parser.add_argument("--warmup_epochs", type=int, default=6, help="热身轮次，使用更小数据子集加速前期收敛")
    parser.add_argument("--time_budget_hours", type=float, default=0.0, help="保留兼容参数；当前训练不按时间预算早停")
    parser.add_argument("--min_epochs", type=int, default=0, help="保留兼容参数；当前训练不按时间预算早停")
    parser.add_argument("--rel_curr_start_epoch", type=int, default=1, help="可靠性课程学习起始epoch")
    parser.add_argument("--rel_curr_end_epoch", type=int, default=32, help="可靠性课程学习结束epoch（到达严格约束）")
    parser.add_argument("--rel_curr_min_scale", type=float, default=0.75, help="课程学习初始可靠性缩放系数")
    parser.add_argument("--rel_curr_strict_ratio", type=float, default=0.7, help="课程进度达到该比例后启用严格可靠性硬约束")
    parser.add_argument("--rel_curr_strict_ramp_ratio", type=float, default=0.2, help="严格可靠性从0到1的渐进区间比例")
    parser.add_argument("--shared_resources_prob_min", type=float, default=0.25, help="训练早期共享资源模式概率")
    parser.add_argument("--shared_resources_prob_max", type=float, default=0.45, help="训练后期共享资源模式概率")
    parser.add_argument("--adaptive_control", action="store_true", default=True, help="启用自适应训练控制")
    parser.add_argument("--collapse_patience", type=int, default=2, help="连续多少轮劣化后触发回退保护")
    parser.add_argument("--eval_data_files", type=int, default=10, help="每轮固定验证使用的文件数")
    parser.add_argument("--eval_requests_per_file", type=int, default=8, help="每个验证文件使用的请求数")
    parser.add_argument("--no_save_checkpoints", action="store_true", help="调试/smoke test时不写入正式checkpoint")
    parser.add_argument("--init_model_checkpoint", type=str, default="", help="初始化Actor/Critic权重路径")
    parser.add_argument("--init_gnn_checkpoint", type=str, default="", help="初始化GNN权重路径")
    args = parser.parse_args()
    current_config = {
        "epochs": args.epochs,
        "max_data_files": args.max_data_files,
        "max_requests_per_file": args.max_requests_per_file,
        "heuristic_top_m": args.heuristic_top_m,
        "warmup_epochs": args.warmup_epochs,
        "rel_curr_start_epoch": args.rel_curr_start_epoch,
        "rel_curr_end_epoch": args.rel_curr_end_epoch,
        "rel_curr_min_scale": args.rel_curr_min_scale,
        "rel_curr_strict_ratio": args.rel_curr_strict_ratio,
        "rel_curr_strict_ramp_ratio": args.rel_curr_strict_ramp_ratio,
        "shared_resources_prob_min": args.shared_resources_prob_min,
        "shared_resources_prob_max": args.shared_resources_prob_max,
    }

    print("=" * 72)
    print("  open5gs星座核心网部署 - 训练")
    print("=" * 72)

    if args.device == "auto":
        if torch.cuda.is_available():
            args.device = "cuda"
        elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            args.device = "mps"
        else:
            args.device = "cpu"

    start_time = time.time()
    gnn = GNNEncoder(input_dim=NODE_FEATURE_DIM, hidden_dim=GNN_EMBEDDING_DIM, num_layers=4)
    agent = DRLAgent(
        node_dim=GNN_EMBEDDING_DIM,
        vnf_dim=CORE_NF_FEATURE_DIM,
        context_dim=CONTEXT_FEATURE_DIM,
        device=args.device,
    )
    if args.init_model_checkpoint and os.path.exists(args.init_model_checkpoint):
        print(f"加载初始化策略模型: {args.init_model_checkpoint}")
        # 继续训练时仅加载网络权重，不恢复旧优化器状态（参数组可能已变化）。
        agent.load(args.init_model_checkpoint, load_optimizer=False)
    if args.init_gnn_checkpoint and os.path.exists(args.init_gnn_checkpoint):
        print(f"加载初始化GNN模型: {args.init_gnn_checkpoint}")
        gnn.load_state_dict(torch.load(args.init_gnn_checkpoint, map_location=args.device))
    # 关键：让GNN参与联合优化，否则策略只能在随机图编码上学习，效果会长期横盘。
    agent.actor_optimizer.add_param_group({"params": gnn.parameters(), "lr": 3e-5, "weight_decay": 1e-5})
    heuristic = HeuristicPruner(top_m=args.heuristic_top_m)
    trainer = SFCTrainer(gnn, agent, args.device)

    actor_scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(agent.actor_optimizer, T_max=args.epochs, eta_min=1e-5)
    critic_scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(agent.critic_optimizer, T_max=args.epochs, eta_min=2e-5)

    train_topos = sorted(glob.glob(str(TRAIN_ROOT / "data" / "train" / "topologies" / "*.json")))
    train_reqs = sorted(glob.glob(str(TRAIN_ROOT / "data" / "train" / "requests" / "*.json")))
    val_topos = sorted(glob.glob(str(TRAIN_ROOT / "data" / "val" / "topologies" / "*.json")))
    val_reqs = sorted(glob.glob(str(TRAIN_ROOT / "data" / "val" / "requests" / "*.json")))

    if not train_topos or not train_reqs:
        print("错误: 训练数据未生成，请先运行数据增强")
        print("python3 train/training/data_generation/augment_data.py")
        sys.exit(1)

    full_train_data = _build_scale_balanced_data(train_topos, train_reqs)
    if not full_train_data:
        full_train_data = list(zip(train_topos * (len(train_reqs) // len(train_topos) + 1), train_reqs))[: len(train_reqs)]
    print(f"训练数据池: {len(full_train_data)} 组 (拓扑: {len(train_topos)}, 请求文件: {len(train_reqs)})")
    fixed_eval_data = _build_scale_balanced_data(val_topos, val_reqs) if val_topos and val_reqs else []
    if not fixed_eval_data:
        fixed_eval_data = _select_scale_coverage(full_train_data, max(1, min(len(full_train_data), args.eval_data_files)))
    else:
        fixed_eval_data = _select_scale_coverage(fixed_eval_data, max(1, min(len(fixed_eval_data), args.eval_data_files)))
    print(
        f"固定验证集: {len(fixed_eval_data)} 组 × 每组 {args.eval_requests_per_file} 请求 "
        "(连续部署口径，用于稳定评估reward/quality趋势)"
    )

    os.makedirs("models/checkpoints", exist_ok=True)
    os.makedirs("logs", exist_ok=True)

    epsilon = 0.35
    best_score = -1e9
    best_success_rate = 0.0
    best_eval_quality = 0.0
    best_eval_reward = -1e9
    history = []
    collapse_count = 0
    reliability_curriculum = {
        "start_epoch": args.rel_curr_start_epoch,
        "end_epoch": args.rel_curr_end_epoch,
        "min_scale": args.rel_curr_min_scale,
        "strict_enable_ratio": args.rel_curr_strict_ratio,
        "strict_ramp_ratio": args.rel_curr_strict_ramp_ratio,
    }

    for epoch in range(1, args.epochs + 1):
        train_progress = epoch / max(1, args.epochs)
        current_shared_resources_prob = (
            args.shared_resources_prob_min
            + (args.shared_resources_prob_max - args.shared_resources_prob_min) * train_progress
        )

        if args.max_data_files > 0:
            target_files = args.max_data_files
        else:
            target_files = len(full_train_data)

        # 热身阶段使用更小样本集，先快速学到可行策略再放大训练难度
        if epoch <= args.warmup_epochs:
            epoch_files = max(2, target_files // 2)
            epoch_requests_per_file = max(1, args.max_requests_per_file // 2)
        else:
            epoch_files = target_files
            epoch_requests_per_file = args.max_requests_per_file

        candidate_pool = full_train_data
        if epoch <= args.warmup_epochs:
            warmup_pool = [
                pair for pair in full_train_data
                if 0 < _read_topology_scale(pair[0]) <= 1200
            ]
            if warmup_pool:
                candidate_pool = warmup_pool

        if candidate_pool:
            rotation = ((epoch - 1) * max(1, epoch_files)) % len(candidate_pool)
            rotated_pool = candidate_pool[rotation:] + candidate_pool[:rotation]
        else:
            rotated_pool = []
        train_data = rotated_pool[: min(len(rotated_pool), epoch_files)]

        epoch_metrics = trainer.train_epoch(
            epoch,
            train_data,
            epsilon,
            heuristic,
            shared_resources_prob=current_shared_resources_prob,
            max_requests_per_file=epoch_requests_per_file,
            total_epochs=args.epochs,
            reliability_curriculum=reliability_curriculum,
        )
        eval_metrics = trainer.evaluate_epoch(
            epoch,
            fixed_eval_data,
            heuristic,
            max_requests_per_file=args.eval_requests_per_file,
            shared_resources=True,
        )
        epoch_metrics.update(eval_metrics)
        best_eval_quality = max(best_eval_quality, float(eval_metrics.get("eval_avg_quality_score", 0.0)))
        best_eval_reward = max(best_eval_reward, float(eval_metrics.get("eval_avg_reward", 0.0)))
        epoch_metrics["eval_quality_best_so_far"] = best_eval_quality
        epoch_metrics["eval_reward_best_so_far"] = best_eval_reward
        history.append(epoch_metrics)

        composite_score = _training_quality_score(epoch_metrics)
        eval_score = _training_quality_score(epoch_metrics, prefix="eval_")
        if epoch == args.warmup_epochs + 1:
            best_score = -1e9
            print("  ℹ 进入完整规模训练阶段，重置最佳模型评分基准")
        total_req = int(epoch_metrics.get("total_requests", 0))
        fail_count = int(max(0, round(total_req * (100.0 - float(epoch_metrics["success_rate"])) / 100.0)))
        print(
            f"Epoch {epoch} | TrainScore={composite_score:.2f} | EvalScore={eval_score:.2f} | "
            f"Reward={epoch_metrics['avg_reward']:.2f} | "
            f"Quality={epoch_metrics.get('avg_quality_score', 0.0):.3f} | "
            f"EvalReward={epoch_metrics.get('eval_avg_reward', 0.0):.2f} | "
            f"EvalQuality={epoch_metrics.get('eval_avg_quality_score', 0.0):.3f} "
            f"(Best={epoch_metrics.get('eval_quality_best_so_far', 0.0):.3f}) | "
            f"PlaceRes={epoch_metrics.get('eval_placement_resource_score', 0.0):.3f} | "
            f"Future={epoch_metrics.get('eval_future_feasibility_score', 0.0):.3f} | "
            f"EvalActorHit={epoch_metrics.get('eval_actor_hit_rate', 0.0):.1f}% | "
            f"EvalFallback={epoch_metrics.get('eval_fallback_count', 0)} | "
            f"Success={epoch_metrics['success_rate']:.2f}% | DepDelay={epoch_metrics['avg_episode_delay_ms']:.2f}ms | "
            f"AlgP95={epoch_metrics.get('p95_algorithm_latency_ms', 0.0):.2f}ms | "
            f"Fail={fail_count}/{total_req} | TopFail={epoch_metrics.get('top_failure_reasons', [])}"
        )

        eligible_for_best = epoch > args.warmup_epochs or best_score <= -1e8
        if eligible_for_best and eval_score > best_score:
            best_score = eval_score
            best_success_rate = max(best_success_rate, epoch_metrics["success_rate"])
            if not args.no_save_checkpoints:
                agent.save("models/checkpoints/model_best.pth")
                torch.save(gnn.state_dict(), "models/checkpoints/gnn_best.pth")
            print(
                "  ✓ 新最佳模型: "
                f"EvalScore={eval_score:.2f}, EvalQuality={epoch_metrics.get('eval_avg_quality_score', 0.0):.3f}, "
                f"Success={epoch_metrics['success_rate']:.2f}%"
            )
            collapse_count = 0

        if epoch % 10 == 0:
            if not args.no_save_checkpoints:
                agent.save(f"models/checkpoints/model_epoch_{epoch}.pth")
                torch.save(gnn.state_dict(), f"models/checkpoints/gnn_epoch_{epoch}.pth")
                print(f"  ✓ 检查点已保存 (epoch {epoch})")

        # 自适应控制：抑制中后期性能崩塌，并平衡成功率/SLA/时延
        if args.adaptive_control:
            recent_window = history[-3:]
            recent_success = sum(h["success_rate"] for h in recent_window) / len(recent_window)
            recent_full_sla = sum(h.get("full_sla_satisfaction_rate", 0.0) for h in recent_window) / len(recent_window)

            # 1) 失败模式驱动的动作
            top_fails = dict(epoch_metrics.get("top_failure_reasons", []))
            max_step_fail = top_fails.get("max_steps_reached", 0)
            no_candidate_fail = top_fails.get("no_candidates", 0) + top_fails.get("invalid_candidates", 0)
            if max_step_fail > 0.5 * max(1, epoch_metrics.get("total_requests", 1)):
                heuristic.top_m = min(96, heuristic.top_m + 4)
                epsilon = max(epsilon, 0.22)
            if no_candidate_fail > 0:
                heuristic.top_m = min(96, heuristic.top_m + 6)
                epsilon = max(epsilon, 0.20)

            # 2) 时延优化：在成功率较高时收紧候选规模，提高推理速度
            latency_pressure = max(
                float(epoch_metrics.get("p95_algorithm_latency_ms", 0.0)),
                float(epoch_metrics.get("eval_p95_algorithm_latency_ms", 0.0)),
            )
            if (
                epoch_metrics["success_rate"] >= 99.0
                and (
                    epoch_metrics["avg_algorithm_latency_ms"] > 300
                    or latency_pressure > 450
                )
                and no_candidate_fail == 0
            ):
                heuristic.top_m = max(48, heuristic.top_m - 6)
                trainer.max_probe_candidates = max(4, trainer.max_probe_candidates - 1)
            elif latency_pressure > 500:
                heuristic.top_m = max(48, heuristic.top_m - 8)
                trainer.max_probe_candidates = max(4, trainer.max_probe_candidates - 1)

            # 3) 崩塌保护：成功率明显低于历史最佳时触发
            collapse_threshold = max(10.0, best_success_rate * 0.55)
            if epoch >= 10 and recent_success < collapse_threshold:
                collapse_count += 1
            else:
                collapse_count = 0

            if collapse_count >= args.collapse_patience:
                print(
                    f"  ⚠ 检测到性能崩塌(最近成功率{recent_success:.1f}% < 阈值{collapse_threshold:.1f}%)，执行恢复策略"
                )
                # 回退到最佳模型，避免策略持续劣化
                best_actor = "models/checkpoints/model_best.pth"
                best_gnn = "models/checkpoints/gnn_best.pth"
                if os.path.exists(best_actor) and os.path.exists(best_gnn):
                    agent.load(best_actor)
                    gnn.load_state_dict(torch.load(best_gnn, map_location=args.device))

                # 放缓课程学习严格化节奏
                reliability_curriculum["strict_enable_ratio"] = min(
                    0.9, reliability_curriculum["strict_enable_ratio"] + 0.05
                )
                reliability_curriculum["strict_ramp_ratio"] = min(
                    0.45, reliability_curriculum["strict_ramp_ratio"] + 0.05
                )
                reliability_curriculum["end_epoch"] = min(
                    args.epochs, reliability_curriculum["end_epoch"] + 6
                )
                reliability_curriculum["min_scale"] = max(
                    0.65, reliability_curriculum["min_scale"] - 0.03
                )

                # 增强探索，扩大候选，帮助跳出局部最优
                epsilon = max(epsilon, 0.28)
                heuristic.top_m = min(96, heuristic.top_m + 8)
                collapse_count = 0

            # 4) Full SLA长期为0时，前期保持宽松可靠性目标，避免无效训练
            if epoch >= 20 and recent_full_sla <= 1.0:
                reliability_curriculum["end_epoch"] = min(args.epochs, reliability_curriculum["end_epoch"] + 2)
                reliability_curriculum["strict_enable_ratio"] = min(
                    0.9, reliability_curriculum["strict_enable_ratio"] + 0.01
                )
                reliability_curriculum["min_scale"] = max(0.65, reliability_curriculum["min_scale"] - 0.01)

        if epoch_metrics.get("updated_episodes", 0) > 0:
            actor_scheduler.step()
            critic_scheduler.step()
        epsilon = max(0.02, epsilon * 0.985)

    best_actor_path = "models/checkpoints/model_best.pth"
    best_gnn_path = "models/checkpoints/gnn_best.pth"
    if not args.no_save_checkpoints and os.path.exists(best_actor_path) and os.path.exists(best_gnn_path):
        agent.load(best_actor_path, load_optimizer=False)
        gnn.load_state_dict(torch.load(best_gnn_path, map_location=args.device))
        print(f"  ✓ 已恢复最佳策略作为最终模型: Score={best_score:.2f}")

    if not args.no_save_checkpoints:
        agent.save("models/checkpoints/model_final.pth")
        torch.save(gnn.state_dict(), "models/checkpoints/gnn_final.pth")

    json_path, csv_path, plot_path = save_metrics(history, output_dir="logs")
    total_time = time.time() - start_time
    best_success = max((h["success_rate"] for h in history), default=0.0)
    best_sla = max((h["sla_satisfaction_rate"] for h in history), default=0.0)

    print("\n✓ 训练完成")
    print(f"  最佳部署成功率: {best_success:.2f}%")
    print(f"  最佳SLA满足率: {best_sla:.2f}%")
    print(f"  总耗时: {total_time / 60:.1f} 分钟")
    print(f"  指标JSON: {json_path}")
    print(f"  指标CSV: {csv_path}")
    print(f"  训练图表: {plot_path}")


if __name__ == "__main__":
    main()
