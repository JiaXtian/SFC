"""多规模动态训练集生成器。

生成内容：
1) 动态拓扑时序（topology_timeline）
2) 对齐拓扑步的请求时序（request_timeline）

目标：增强模型在不同规模（如 800/1600/2500/4000/4800）场景下的泛化能力。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List, Tuple

try:
    from .dynamic_scene_generator import DynamicSceneConfig, generate_dynamic_scene
    from .sfc_generator import build_sfc_requests_payload
except ImportError:
    from dynamic_scene_generator import DynamicSceneConfig, generate_dynamic_scene
    from sfc_generator import build_sfc_requests_payload

PROJECT_ROOT = Path(__file__).resolve().parents[3]


def _parse_scale_plan(text: str) -> List[Tuple[int, int]]:
    # format: "800:3,1600:3,2500:4"
    plan: List[Tuple[int, int]] = []
    for token in (text or "").split(","):
        t = token.strip()
        if not t or ":" not in t:
            continue
        a, b = t.split(":", 1)
        try:
            scale = int(a.strip())
            count = int(b.strip())
        except ValueError:
            continue
        if scale > 0 and count > 0:
            plan.append((scale, count))
    if plan:
        return plan
    return [(800, 2), (1600, 2), (2500, 2), (4000, 2), (4800, 2)]


def _infer_num_planes(total_sats: int) -> int:
    if total_sats <= 1000:
        return max(8, total_sats // 32)
    if total_sats <= 2500:
        return max(20, total_sats // 48)
    return max(36, total_sats // 72)


def _load_profile_for_step(step_idx: int) -> str:
    profiles = ["low", "medium", "high", "mixed", "mixed"]
    return profiles[step_idx % len(profiles)]


def _requests_per_step(total_sats: int, base_count: int, ref_scale: int = 2500) -> int:
    scale_factor = (max(200, total_sats) / max(200, ref_scale)) ** 0.38
    dynamic_count = int(round(base_count * scale_factor))
    return max(24, min(180, dynamic_count))


def _snapshot_to_topology_timeline(scene_payload: Dict) -> List[Dict]:
    snapshots = scene_payload.get("snapshots", [])
    timeline: List[Dict] = []
    for idx, snap in enumerate(snapshots):
        sim_time = snap.get("sim_time", "")
        timeline.append(
            {
                "step_index": int(snap.get("topology_version", idx)),
                "timestamp": sim_time,
                "sim_time": sim_time,
                "topology_version": int(snap.get("topology_version", idx)),
                "sampling_interval_sec": float(snap.get("sampling_interval_sec", 5.0)),
                "topology": snap.get("topology", {}),
                "metrics": snap.get("metrics", {}),
                "events": snap.get("events", []),
            }
        )
    return timeline


def _build_request_timeline(
    topology_timeline: List[Dict],
    topology_name: str,
    total_sats: int,
    base_requests_per_step: int,
    seed_base: int,
) -> Dict:
    request_timeline: List[Dict] = []
    req_per_step = _requests_per_step(total_sats, base_requests_per_step)

    for step_idx, step in enumerate(topology_timeline):
        nodes = step.get("topology", {}).get("nodes", [])
        active_nodes = [n.get("id") for n in nodes if n.get("id") and str(n.get("status", "active")).lower() not in {"down", "inactive", "failed"}]
        if len(active_nodes) < 2:
            request_timeline.append(
                {
                    "step_index": int(step.get("step_index", step_idx)),
                    "requests": [],
                    "active_instances": [],
                }
            )
            continue

        payload = build_sfc_requests_payload(
            num_requests=req_per_step,
            node_list=active_nodes,
            topology_data=step.get("topology", {}),
            load_profile=_load_profile_for_step(step_idx),
            seed=seed_base + step_idx,
            topology_scale=total_sats,
            topology_file=topology_name,
            request_id_prefix=f"{Path(topology_name).stem}_t{step_idx}",
        )
        request_timeline.append(
            {
                "step_index": int(step.get("step_index", step_idx)),
                "requests": payload.get("requests", []),
                "active_instances": [],
            }
        )

    return {
        "metadata": {
            "generator": "dynamic_multiscale_request_generator",
            "num_steps": len(topology_timeline),
            "requests_per_step": req_per_step,
            "topology_scale": total_sats,
            "topology_file": topology_name,
            "generation_seed": seed_base,
        },
        "request_timeline": request_timeline,
    }


def _next_index(topology_dir: Path, request_dir: Path) -> int:
    max_idx = -1
    for p in list(topology_dir.glob("*.json")) + list(request_dir.glob("*.json")):
        stem = p.stem
        token = stem.split("_")[-1]
        if token.isdigit():
            max_idx = max(max_idx, int(token))
    return max_idx + 1


def generate_multiscale_dynamic_dataset(
    scale_plan: List[Tuple[int, int]],
    topology_dir: Path,
    request_dir: Path,
    step_sec: int,
    duration_sec: int,
    base_requests_per_step: int,
    seed: int,
    isl_max_distance_km: float,
) -> Dict:
    topology_dir.mkdir(parents=True, exist_ok=True)
    request_dir.mkdir(parents=True, exist_ok=True)

    index = _next_index(topology_dir, request_dir)
    created = []

    altitudes = [550.0, 600.0, 700.0, 900.0, 1200.0]
    inclinations = [53.0, 70.0, 97.6]

    seq_counter = 0
    for scale, count in scale_plan:
        for local_idx in range(count):
            seq_id = index + seq_counter
            seq_seed = seed + seq_id * 97
            num_planes = _infer_num_planes(scale)
            altitude_km = altitudes[(seq_id + local_idx) % len(altitudes)]
            inclination_deg = inclinations[(seq_id + scale) % len(inclinations)]

            scene_cfg = DynamicSceneConfig(
                total_sats=scale,
                num_planes=num_planes,
                altitude_km=altitude_km,
                inclination_deg=inclination_deg,
                step_sec=step_sec,
                duration_sec=duration_sec,
                isl_max_distance_km=isl_max_distance_km,
                node_fault_prob_per_tick=0.00015 + 0.00005 * (seq_id % 4),
                link_fault_prob_per_tick=0.00045 + 0.00010 * (seq_id % 3),
                seed=seq_seed,
            )

            topo_file = topology_dir / f"dynamic_topology_{seq_id:03d}.json"
            req_file = request_dir / f"dynamic_requests_{seq_id:03d}.json"
            scene_payload = generate_dynamic_scene(scene_cfg, topo_file)
            topology_timeline = _snapshot_to_topology_timeline(scene_payload)

            topo_payload = {
                "metadata": {
                    "generator": "dynamic_topology_sequence_generator",
                    "orbit_model": "sgp4_skyfield",
                    "steps": len(topology_timeline),
                    "step_seconds": float(step_sec),
                    "seed": seq_seed,
                    "fault_probability": round(scene_cfg.link_fault_prob_per_tick, 6),
                    "created_at": scene_payload.get("generated_at", ""),
                    "total_sats": scale,
                    "num_planes": num_planes,
                    "altitude_km": altitude_km,
                    "inclination_deg": inclination_deg,
                },
                "topology_timeline": topology_timeline,
            }
            topo_file.write_text(json.dumps(topo_payload, ensure_ascii=False, indent=2), encoding="utf-8")

            req_payload = _build_request_timeline(
                topology_timeline=topology_timeline,
                topology_name=topo_file.name,
                total_sats=scale,
                base_requests_per_step=base_requests_per_step,
                seed_base=seq_seed + 100000,
            )
            req_file.write_text(json.dumps(req_payload, ensure_ascii=False, indent=2), encoding="utf-8")

            created.append(
                {
                    "index": seq_id,
                    "scale": scale,
                    "num_planes": num_planes,
                    "topology_file": str(topo_file),
                    "request_file": str(req_file),
                    "steps": len(topology_timeline),
                    "requests_per_step": int(req_payload["metadata"]["requests_per_step"]),
                }
            )
            seq_counter += 1

    return {
        "generator": "dynamic_multiscale_dataset_builder_v1",
        "scale_plan": scale_plan,
        "created_sequences": created,
        "total_sequences": len(created),
        "total_steps": int(sum(x["steps"] for x in created)),
        "estimated_total_requests": int(sum(x["steps"] * x["requests_per_step"] for x in created)),
    }


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate multi-scale dynamic datasets for dynamic training.")
    p.add_argument("--train-scale-plan", type=str, default="800:2,1600:2,2500:2,4000:2,4800:2")
    p.add_argument("--topology-dir", type=Path, default=PROJECT_ROOT / "train" / "data" / "train" / "dynamic" / "topologies")
    p.add_argument("--request-dir", type=Path, default=PROJECT_ROOT / "train" / "data" / "train" / "dynamic" / "requests")
    p.add_argument("--step-sec", type=int, default=5)
    p.add_argument("--duration-sec", type=int, default=180)
    p.add_argument("--base-requests-per-step", type=int, default=64)
    p.add_argument("--isl-max-distance-km", type=float, default=4200.0)
    p.add_argument("--seed", type=int, default=11000)
    p.add_argument("--summary-file", type=Path, default=PROJECT_ROOT / "logs" / "dynamic_multiscale_generation_summary.json")
    return p.parse_args()


def main():
    args = parse_args()
    scale_plan = _parse_scale_plan(args.train_scale_plan)
    summary = generate_multiscale_dynamic_dataset(
        scale_plan=scale_plan,
        topology_dir=args.topology_dir,
        request_dir=args.request_dir,
        step_sec=args.step_sec,
        duration_sec=args.duration_sec,
        base_requests_per_step=args.base_requests_per_step,
        seed=args.seed,
        isl_max_distance_km=args.isl_max_distance_km,
    )
    args.summary_file.parent.mkdir(parents=True, exist_ok=True)
    args.summary_file.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

    print(
        f"[dynamic_multiscale] sequences={summary['total_sequences']} "
        f"steps={summary['total_steps']} "
        f"estimated_requests={summary['estimated_total_requests']}"
    )
    print(f"[dynamic_multiscale] summary -> {args.summary_file}")


if __name__ == "__main__":
    main()
