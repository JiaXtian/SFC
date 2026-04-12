"""动态时序验证脚本（阶段2）。"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import torch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT_ROOT = Path(__file__).resolve().parents[2]
TRAIN_ROOT = PROJECT_ROOT / "train"

from ground_training.models.drl_agent import DRLAgent
from ground_training.models.gnn_encoder import GNNEncoder
from ground_training.train_dynamic import HeuristicPruner
from ground_training.training.dynamic_dataset import discover_dynamic_pairs, load_dynamic_sequences
from ground_training.training.trainer import SFCTrainer


def _load_meta_if_exists() -> dict:
    for p in ["models/checkpoints/model_dynamic_meta.json", "train/models/checkpoints/model_dynamic_meta.json"]:
        if os.path.exists(p):
            with open(p) as f:
                return json.load(f)
    return {}


def _infer_context_dim(model_checkpoint: str, fallback: int) -> int:
    checkpoint = torch.load(model_checkpoint, map_location="cpu", weights_only=False)
    actor_state = checkpoint.get("actor_state_dict", {})
    fc0_w = actor_state.get("fc.0.weight")
    if fc0_w is None or fc0_w.ndim != 2:
        return fallback
    inferred = int(fc0_w.shape[1]) - 192 - 8
    return inferred if inferred > 0 else fallback


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


def main():
    os.chdir(PROJECT_ROOT)
    parser = argparse.ArgumentParser(description="Evaluate dynamic SFC model")
    parser.add_argument("--model_checkpoint", default="models/checkpoints/model_dynamic_best.pth")
    parser.add_argument("--gnn_checkpoint", default="models/checkpoints/gnn_dynamic_best.pth")
    parser.add_argument("--dynamic_topology_dir", default="data/train/dynamic/topologies")
    parser.add_argument("--dynamic_request_dir", default="data/train/dynamic/requests")
    parser.add_argument("--max_sequences", type=int, default=0)
    parser.add_argument("--max_steps_per_sequence", type=int, default=0)
    parser.add_argument("--max_requests_per_step", type=int, default=12)
    parser.add_argument("--top_m", type=int, default=100)
    parser.add_argument("--history_window", type=int, default=-1)
    parser.add_argument("--context_dim", type=int, default=-1)
    parser.add_argument("--backend_align_context", dest="backend_align_context", action="store_true")
    parser.add_argument("--no_backend_align_context", dest="backend_align_context", action="store_false")
    parser.set_defaults(backend_align_context=None)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--output_json", default="logs/dynamic_eval.json")
    args = parser.parse_args()

    args.dynamic_topology_dir = _resolve_data_dir(args.dynamic_topology_dir)
    args.dynamic_request_dir = _resolve_data_dir(args.dynamic_request_dir)

    meta = _load_meta_if_exists()
    if args.context_dim <= 0:
        args.context_dim = int(meta.get("context_dim", 48))
    if args.backend_align_context is None:
        args.backend_align_context = bool(meta.get("backend_align_context", True))
    if args.history_window < 0:
        if args.backend_align_context:
            args.history_window = 0
        else:
            args.history_window = int(meta.get("history_window", max(0, (args.context_dim - 16) // 8)))
    if args.backend_align_context and args.context_dim < 48:
        args.context_dim = 48
    args.context_dim = _infer_context_dim(args.model_checkpoint, args.context_dim)
    if args.backend_align_context:
        args.history_window = 0
    else:
        args.history_window = max(0, min(args.history_window, max(0, (args.context_dim - 16) // 8)))

    pairs = discover_dynamic_pairs(args.dynamic_topology_dir, args.dynamic_request_dir)
    if not pairs:
        raise RuntimeError("未找到动态验证样本")
    sequences = load_dynamic_sequences(pairs, max_sequences=args.max_sequences)
    if not sequences:
        raise RuntimeError("动态验证样本为空")

    gnn = GNNEncoder(input_dim=8, hidden_dim=192, num_layers=4).to(args.device)
    gnn.load_state_dict(torch.load(args.gnn_checkpoint, map_location=args.device))
    gnn.eval()

    agent = DRLAgent(node_dim=192, vnf_dim=8, context_dim=args.context_dim, device=args.device)
    agent.load(args.model_checkpoint, load_optimizer=False)
    agent.actor.eval()
    agent.critic.eval()

    trainer = SFCTrainer(
        gnn=gnn,
        agent=agent,
        device=args.device,
        context_dim=args.context_dim,
        history_window=args.history_window,
        backend_align_context=args.backend_align_context,
        log_dir="logs",
    )
    heuristic = HeuristicPruner(top_m=args.top_m)

    metrics = trainer.train_dynamic_epoch(
        epoch=0,
        sequence_data=sequences,
        epsilon=0.0,
        heuristic_pruner=heuristic,
        max_steps_per_sequence=args.max_steps_per_sequence,
        max_requests_per_step=args.max_requests_per_step,
        total_epochs=1,
        reliability_curriculum=None,
        update_policy=False,
        deterministic_policy=True,
    )

    os.makedirs(os.path.dirname(args.output_json) or ".", exist_ok=True)
    with open(args.output_json, "w") as f:
        json.dump(metrics, f, indent=2)
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
