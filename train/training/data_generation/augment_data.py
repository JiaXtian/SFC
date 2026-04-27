"""数据增强脚本（大规模训练集 + 完整指标体系）"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.append(os.path.dirname(os.path.dirname(__file__)))

from data_generation.sfc_generator import generate_sfc_requests
from data_generation.topology_generator import generate_scaled_topologies

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DATA_ROOT = PROJECT_ROOT / "train" / "data"


def _parse_scales(scale_text):
    parts = [p.strip() for p in str(scale_text).split(",") if p.strip()]
    values = []
    for p in parts:
        try:
            values.append(int(p))
        except ValueError:
            continue
    return values or [2500, 6000]


def _distribute_counts(total_count, scales, weights_text):
    weights = [max(1, int(d.strip())) for d in str(weights_text).split(",") if d.strip().isdigit()]
    if len(weights) != len(scales):
        weights = [1 for _ in scales]

    if total_count <= 0:
        return {scale: weights[idx] for idx, scale in enumerate(scales)}

    weight_sum = sum(weights)
    scale_to_count = {}
    assigned = 0
    for idx, scale in enumerate(scales):
        if idx == len(scales) - 1:
            count = max(1, total_count - assigned)
        else:
            count = max(1, int(round(total_count * weights[idx] / max(1, weight_sum))))
            assigned += count
        scale_to_count[scale] = count
    return scale_to_count


def augment_training_data(
    train_topologies=8,
    train_groups_per_topology=5,
    train_requests_per_group=700,
    val_topologies=3,
    val_requests_per_topology=500,
    train_scales="2500,6000",
    scale_distribution="2,4,2",
):
    print("=== 数据模拟 ===")
    train_topology_dir = DATA_ROOT / "train" / "topologies"
    train_request_dir = DATA_ROOT / "train" / "requests"
    val_topology_dir = DATA_ROOT / "val" / "topologies"
    val_request_dir = DATA_ROOT / "val" / "requests"
    train_topology_dir.mkdir(parents=True, exist_ok=True)
    train_request_dir.mkdir(parents=True, exist_ok=True)
    val_topology_dir.mkdir(parents=True, exist_ok=True)
    val_request_dir.mkdir(parents=True, exist_ok=True)

    print("\n[1/3] 生成训练拓扑...")
    parsed_scales = _parse_scales(train_scales)
    scale_to_count = _distribute_counts(train_topologies, parsed_scales, scale_distribution)

    train_topos = generate_scaled_topologies(scale_to_count, output_dir=str(train_topology_dir))

    print("\n[2/3] 生成训练请求...")
    total_train_requests = 0
    load_profiles = ["low", "medium", "high", "mixed", "mixed"]

    for i, topo_file in enumerate(train_topos):
        with open(topo_file) as f:
            topo = json.load(f)
        nodes = [n["id"] for n in topo["topology"]["nodes"]]

        topo_sat_count = int(topo.get("metadata", {}).get("total_satellites", len(nodes)))
        if topo_sat_count <= 1200:
            req_scale = 0.75
        elif topo_sat_count >= 5000:
            req_scale = 1.35
        else:
            req_scale = 1.0

        for j in range(train_groups_per_topology):
            load_profile = load_profiles[j % len(load_profiles)]
            req_count = int(max(120, round(train_requests_per_group * req_scale)))
            generate_sfc_requests(
                num_requests=req_count,
                node_list=nodes,
                topology_data=topo,
                output_file=str(train_request_dir / f"requests_{i:03d}_{j:03d}.json"),
                load_profile=load_profile,
                seed=1000 + i * 100 + j,
                topology_scale=topo_sat_count,
                topology_file=os.path.basename(topo_file),
            )
            total_train_requests += req_count

    print("\n[3/3] 生成验证集...")
    val_scales = [scale for scale in parsed_scales if scale > 1200] or parsed_scales
    val_scale_plan = _distribute_counts(val_topologies, val_scales, scale_distribution)
    val_topos = generate_scaled_topologies(
        val_scale_plan,
        output_dir=str(val_topology_dir),
    )

    total_val_requests = 0
    for i, topo_file in enumerate(val_topos):
        with open(topo_file) as f:
            topo = json.load(f)
        nodes = [n["id"] for n in topo["topology"]["nodes"]]

        profile = "high" if i % 2 == 0 else "mixed"
        generate_sfc_requests(
            num_requests=val_requests_per_topology,
            node_list=nodes,
            topology_data=topo,
            output_file=str(val_request_dir / f"requests_{i:03d}.json"),
            load_profile=profile,
            seed=9000 + i,
            topology_scale=int(topo.get("metadata", {}).get("total_satellites", len(nodes))),
            topology_file=os.path.basename(topo_file),
        )
        total_val_requests += val_requests_per_topology

    print("\n数据模拟生成完成:")
    print(
        f"  训练: {len(train_topos)} 拓扑({scale_to_count}) × {train_groups_per_topology}组 ≈ {total_train_requests} 条"
    )
    print(f"  验证: {len(val_topos)} 拓扑 × {val_requests_per_topology}请求 = {total_val_requests} 条")
    print(f"  总计: {total_train_requests + total_val_requests} 条")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--train_topologies", type=int, default=8)
    parser.add_argument("--train_groups_per_topology", type=int, default=5)
    parser.add_argument("--train_requests_per_group", type=int, default=700)
    parser.add_argument("--val_topologies", type=int, default=10)
    parser.add_argument("--val_requests_per_topology", type=int, default=500)
    parser.add_argument("--train_scales", type=str, default="2500,6000")
    parser.add_argument("--scale_distribution", type=str, default="2,4,2")
    args = parser.parse_args()

    augment_training_data(
        train_topologies=args.train_topologies,
        train_groups_per_topology=args.train_groups_per_topology,
        train_requests_per_group=args.train_requests_per_group,
        val_topologies=args.val_topologies,
        val_requests_per_topology=args.val_requests_per_topology,
        train_scales=args.train_scales,
        scale_distribution=args.scale_distribution,
    )
