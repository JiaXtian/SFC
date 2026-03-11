"""基于动态拓扑场景构建训练/验证数据清单（v2）"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List

from dynamic_scene_generator import DynamicSceneConfig, generate_dynamic_scene


def split_indices(total: int, train_ratio: float) -> Dict[str, List[int]]:
    train_count = max(1, int(total * train_ratio))
    train_ids = list(range(train_count))
    val_ids = list(range(train_count, total))
    if not val_ids:
        val_ids = [max(0, train_count - 1)]
    return {"train": train_ids, "val": val_ids}


def build_dataset(scene_file: Path, out_file: Path, train_ratio: float = 0.8):
    scene = json.loads(scene_file.read_text(encoding="utf-8"))
    snapshots = scene.get("snapshots", [])
    split = split_indices(len(snapshots), train_ratio)

    payload = {
        "schema_version": "dynamic_training_dataset_v2",
        "source_scene": str(scene_file),
        "total_snapshots": len(snapshots),
        "train_snapshot_indices": split["train"],
        "val_snapshot_indices": split["val"],
        "metadata": {
            "description": "用于动态拓扑训练/验证的数据索引清单，训练与推理字段口径保持与后端 TopologySnapshot 一致。",
            "topology_snapshot_fields": [
                "sim_time",
                "topology_version",
                "sampling_interval_sec",
                "topology.nodes",
                "topology.links",
                "metrics",
                "events",
            ],
        },
    }
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return payload


def parse_args():
    p = argparse.ArgumentParser(description="Generate dynamic dataset index for training v2")
    p.add_argument("--scene-file", type=Path, default=Path("../../data/dynamic/scenes/scene_4800_v1.json"))
    p.add_argument("--dataset-file", type=Path, default=Path("../../data/dynamic/dataset_v2_index.json"))
    p.add_argument("--train-ratio", type=float, default=0.8)
    p.add_argument("--generate-scene-if-missing", action="store_true")
    return p.parse_args()


def main():
    args = parse_args()
    if args.generate_scene_if_missing and not args.scene_file.exists():
        cfg = DynamicSceneConfig()
        generate_dynamic_scene(cfg, args.scene_file)
    if not args.scene_file.exists():
        raise FileNotFoundError(f"scene file not found: {args.scene_file}")
    payload = build_dataset(args.scene_file, args.dataset_file, args.train_ratio)
    print(f"[generate_dynamic_dataset] total={payload['total_snapshots']} output={args.dataset_file}")


if __name__ == "__main__":
    main()
