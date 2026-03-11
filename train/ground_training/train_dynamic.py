import argparse
import csv
import json
import os
import random
import sys
import time
from pathlib import Path

import torch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ground_training.models.drl_agent import DRLAgent
from ground_training.models.gnn_encoder import GNNEncoder
from ground_training.models.model_export import export_models
from ground_training.training.dynamic_dataset import discover_dynamic_pairs, load_dynamic_sequences
from ground_training.training.hyperparam_report import generate_hyperparam_report
from ground_training.training.trainer import SFCTrainer


class HeuristicPruner:
    def __init__(self, top_m=80):
        self.top_m = top_m

    @staticmethod
    def _active_graph(G):
        import networkx as nx

        active = nx.DiGraph()
        active.add_nodes_from(G.nodes(data=True))
        for u, v, d in G.edges(data=True):
            if int(d.get("link_status", 1)) == 1:
                active.add_edge(u, v, **d)
        return active

    def prune(self, G, vnf, prev_node, dest_node, remaining_delay, top_m=None):
        import networkx as nx

        if top_m is None:
            top_m = self.top_m

        active_graph = self._active_graph(G)
        candidates = []
        try:
            dist_from_prev = nx.single_source_dijkstra_path_length(active_graph, prev_node, weight="latency_ms")
            reverse_graph = active_graph.reverse(copy=False)
            dist_to_dest = nx.single_source_dijkstra_path_length(reverse_graph, dest_node, weight="latency_ms")
        except Exception:
            return []

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

        for node in valid_nodes[:1800]:
            d1 = dist_from_prev.get(node, float("inf"))
            d2 = dist_to_dest.get(node, float("inf"))
            if d1 == float("inf") or d2 == float("inf"):
                continue
            total_d = d1 + d2
            if total_d > delay_cap:
                continue
            candidates.append((node, total_d))
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
    if os.path.isdir(path):
        return path
    prefixed = os.path.join("train", path)
    if os.path.isdir(prefixed):
        return prefixed
    return path


def main():
    parser = argparse.ArgumentParser(description="动态拓扑时序训练（阶段2）")
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--heuristic_top_m", type=int, default=110)
    parser.add_argument("--history_window", type=int, default=4)
    parser.add_argument("--context_dim", type=int, default=80)
    parser.add_argument("--max_sequences", type=int, default=0)
    parser.add_argument("--max_steps_per_sequence", type=int, default=0)
    parser.add_argument("--max_requests_per_step", type=int, default=24)
    parser.add_argument("--dynamic_topology_dir", default="data/train/dynamic/topologies")
    parser.add_argument("--dynamic_request_dir", default="data/train/dynamic/requests")
    parser.add_argument("--train_ratio", type=float, default=0.8)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--skip_onnx_export", action="store_true")
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

    min_context_dim = 16 + max(0, args.history_window) * 8
    if args.context_dim < min_context_dim:
        args.context_dim = min_context_dim
        print(f"context_dim 自动提升到 {args.context_dim} (history_window={args.history_window})")

    os.makedirs("models/checkpoints", exist_ok=True)
    os.makedirs("logs", exist_ok=True)
    os.makedirs("models/exported_dynamic", exist_ok=True)

    args.dynamic_topology_dir = _resolve_data_dir(args.dynamic_topology_dir)
    args.dynamic_request_dir = _resolve_data_dir(args.dynamic_request_dir)

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
    agent = DRLAgent(node_dim=192, vnf_dim=4, context_dim=args.context_dim, device=args.device)
    agent.actor_optimizer.add_param_group({"params": gnn.parameters(), "lr": 3e-5, "weight_decay": 1e-5})

    trainer = SFCTrainer(
        gnn=gnn,
        agent=agent,
        device=args.device,
        context_dim=args.context_dim,
        history_window=args.history_window,
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
                        "history_window": args.history_window,
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
                "history_window": args.history_window,
                "best_actor_checkpoint": "models/checkpoints/model_dynamic_best.pth",
                "best_gnn_checkpoint": "models/checkpoints/gnn_dynamic_best.pth",
                "final_actor_checkpoint": "models/checkpoints/model_dynamic_final.pth",
                "final_gnn_checkpoint": "models/checkpoints/gnn_dynamic_final.pth",
            },
            f,
            indent=2,
        )

    json_path, csv_path = _save_metrics(history, output_dir="logs")
    report_md_path, report_json_path = generate_hyperparam_report(
        history=[
            {
                "epoch": h["epoch"],
                "avg_reward": h["train_success_rate"],
                "success_rate": h["val_success_rate"],
                "sla_satisfaction_rate": h["val_sla_satisfaction_rate"],
                "full_sla_satisfaction_rate": h["val_full_sla_satisfaction_rate"],
                "avg_episode_delay_ms": h["val_avg_episode_delay_ms"],
                "avg_algorithm_latency_ms": h["val_avg_algorithm_latency_ms"],
                "top_failure_reasons": [],
            }
            for h in history
        ],
        current_config={
            "epochs": args.epochs,
            "history_window": args.history_window,
            "context_dim": args.context_dim,
            "heuristic_top_m": args.heuristic_top_m,
            "max_steps_per_sequence": args.max_steps_per_sequence,
            "max_requests_per_step": args.max_requests_per_step,
        },
        markdown_path=Path("logs/dynamic_hyperparam_report.md"),
        json_path=Path("logs/dynamic_hyperparam_report.json"),
    )

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
                output_dir="models/exported_dynamic",
                context_dim=args.context_dim,
            )

    print("\n动态训练完成")
    print(f"  指标JSON: {json_path}")
    print(f"  指标CSV: {csv_path}")
    print(f"  验证报告: {validation_report_path}")
    print(f"  超参报告: {report_md_path}")
    print(f"  超参JSON: {report_json_path}")


if __name__ == "__main__":
    main()
