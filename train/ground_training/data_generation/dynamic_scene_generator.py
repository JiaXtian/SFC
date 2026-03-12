"""动态卫星场景生成器（SGP4驱动，支持可配置采样周期与故障注入）"""
from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, List, Tuple

from sgp4.api import Satrec, WGS72, jday
from skyfield.api import load


EARTH_RADIUS_KM = 6371.0
LIGHT_SPEED_KMPS = 299792.458
EARTH_MU = 398600.4418
TS = load.timescale()
PROJECT_ROOT = Path(__file__).resolve().parents[3]


@dataclass
class DynamicSceneConfig:
    total_sats: int = 4800
    num_planes: int = 60
    altitude_km: float = 550.0
    inclination_deg: float = 53.0
    step_sec: int = 5
    duration_sec: int = 300
    isl_max_distance_km: float = 5500.0
    node_fault_prob_per_tick: float = 0.0002
    link_fault_prob_per_tick: float = 0.0005
    seed: int = 42


def _mean_motion_rad_per_min(altitude_km: float) -> float:
    a = EARTH_RADIUS_KM + altitude_km
    n_rad_s = math.sqrt(EARTH_MU / (a ** 3))
    return n_rad_s * 60.0


def _build_walker_satrec(config: DynamicSceneConfig) -> Tuple[List[Dict], List[Satrec]]:
    random.seed(config.seed)
    sats_per_plane = max(1, config.total_sats // config.num_planes)
    rem = max(0, config.total_sats - sats_per_plane * config.num_planes)
    mean_motion = _mean_motion_rad_per_min(config.altitude_km)

    meta: List[Dict] = []
    records: List[Satrec] = []
    epoch = datetime.now(timezone.utc)
    jd, fr = jday(epoch.year, epoch.month, epoch.day, epoch.hour, epoch.minute, epoch.second + epoch.microsecond / 1e6)

    satnum = 10000
    for plane in range(config.num_planes):
        plane_count = sats_per_plane + (1 if plane < rem else 0)
        for pos in range(plane_count):
            sat_id = f"SAT_{plane:03d}_{pos:03d}"
            raan_deg = (360.0 / max(1, config.num_planes)) * plane
            phase_deg = (360.0 / max(1, plane_count)) * pos + (360.0 / max(1, config.total_sats)) * plane
            sat = Satrec()
            sat.sgp4init(
                WGS72,
                "i",
                satnum,
                (jd - 2433281.5) + fr,  # epoch as days since 1949-12-31 00:00 UT
                0.0,                    # bstar
                0.0,                    # ndot
                0.0,                    # nddot
                0.0,                    # ecco
                0.0,                    # argpo
                math.radians(config.inclination_deg),
                math.radians(phase_deg),
                mean_motion,
                math.radians(raan_deg),
            )
            satnum += 1
            meta.append({
                "id": sat_id,
                "plane": plane,
                "position_in_plane": pos,
                "raan": raan_deg,
                "altitude_km": config.altitude_km,
                "inclination_deg": config.inclination_deg,
                "cpu_total": round(random.uniform(14.0, 28.0), 2),
                "mem_total": round(random.uniform(24.0, 64.0), 2),
                "disk_total": round(random.uniform(160.0, 520.0), 2),
                "node_reliability": round(random.uniform(0.97, 0.999), 5),
            })
            records.append(sat)
    return meta, records


def _propagate_satellite(sat: Satrec, sat_meta: Dict, dt: datetime) -> Dict:
    jd, fr = jday(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second + dt.microsecond / 1e6)
    err, pos, _vel = sat.sgp4(jd, fr)
    if err != 0:
        # 出错时返回保守占位，避免整批生成失败
        x = y = z = 0.0
    else:
        x, y, z = pos

    r = math.sqrt(x * x + y * y + z * z) or 1.0
    lat = math.degrees(math.asin(max(-1.0, min(1.0, z / r))))
    lon = math.degrees(math.atan2(y, x))
    true_anomaly = (sat.mo + sat.no_kozai * ((jd + fr) - sat.jdsatepoch - sat.jdsatepochF) * 1440.0) % (2.0 * math.pi)

    cpu_load = random.uniform(0.2, 0.85)
    mem_load = random.uniform(0.2, 0.85)
    disk_load = random.uniform(0.15, 0.8)
    return {
        "id": sat_meta["id"],
        "type": "satellite",
        "status": "active",
        "fault_tag": "",
        "orbital_params": {
            "plane": sat_meta["plane"],
            "position_in_plane": sat_meta["position_in_plane"],
            "raan": sat_meta["raan"],
            "true_anomaly": math.degrees(true_anomaly),
            "altitude_km": sat_meta["altitude_km"],
            "inclination_deg": sat_meta["inclination_deg"],
        },
        "coordinates": {"x": x, "y": y, "z": z, "lat": lat, "lon": lon},
        "cpu_total": sat_meta["cpu_total"],
        "cpu_available": round(sat_meta["cpu_total"] * (1.0 - cpu_load), 4),
        "mem_total": sat_meta["mem_total"],
        "mem_available": round(sat_meta["mem_total"] * (1.0 - mem_load), 4),
        "disk_total": sat_meta["disk_total"],
        "disk_available": round(sat_meta["disk_total"] * (1.0 - disk_load), 4),
        "core_network_load": round((cpu_load + mem_load + disk_load) / 3.0, 4),
        "node_reliability": sat_meta["node_reliability"],
        "vnfs": [],
    }


def _distance(a: Dict, b: Dict) -> float:
    dx = a["coordinates"]["x"] - b["coordinates"]["x"]
    dy = a["coordinates"]["y"] - b["coordinates"]["y"]
    dz = a["coordinates"]["z"] - b["coordinates"]["z"]
    return math.sqrt(dx * dx + dy * dy + dz * dz)


def _line_of_sight(a: Dict, b: Dict) -> bool:
    ax, ay, az = a["coordinates"]["x"], a["coordinates"]["y"], a["coordinates"]["z"]
    bx, by, bz = b["coordinates"]["x"], b["coordinates"]["y"], b["coordinates"]["z"]
    dx, dy, dz = bx - ax, by - ay, bz - az
    d2 = dx * dx + dy * dy + dz * dz
    if d2 == 0:
        return False
    t = -(ax * dx + ay * dy + az * dz) / d2
    if t < 0 or t > 1:
        return True
    cx = ax + t * dx
    cy = ay + t * dy
    cz = az + t * dz
    return (cx * cx + cy * cy + cz * cz) > (EARTH_RADIUS_KM + 40.0) ** 2


def _build_links(nodes: List[Dict], config: DynamicSceneConfig) -> List[Dict]:
    by_plane: Dict[int, List[Dict]] = {}
    for n in nodes:
        by_plane.setdefault(int(n["orbital_params"]["plane"]), []).append(n)
    for p in by_plane:
        by_plane[p].sort(key=lambda x: int(x["orbital_params"]["position_in_plane"]))

    planes = sorted(by_plane.keys())
    links: List[Dict] = []

    def add_link(a: Dict, b: Dict, link_type: str):
        d = _distance(a, b)
        if d > config.isl_max_distance_km or not _line_of_sight(a, b):
            return
        bw_total = random.uniform(8.0, 40.0)
        bw_avail = bw_total * random.uniform(0.3, 0.95)
        dist_factor = max(0.0, min(1.0, 1.0 - d / max(1.0, float(config.isl_max_distance_km))))
        reliability = max(0.95, min(0.9997, 0.985 + 0.014 * dist_factor))
        links.append({
            "source": a["id"],
            "target": b["id"],
            "link_type": link_type,
            "status": "active",
            "latency_ms": round((d / LIGHT_SPEED_KMPS) * 1000.0, 5),
            "reliability": round(reliability, 5),
            "bandwidth_gbps": round(bw_total, 4),
            "bandwidth_available_gbps": round(bw_avail, 4),
        })

    for p in planes:
        sats = by_plane[p]
        if not sats:
            continue
        for i, a in enumerate(sats):
            b = sats[(i + 1) % len(sats)]
            add_link(a, b, "intra_orbit")

    for idx, p in enumerate(planes):
        p2 = planes[(idx + 1) % len(planes)]
        sats_a = by_plane[p]
        sats_b = by_plane[p2]
        n = min(len(sats_a), len(sats_b))
        for i in range(n):
            add_link(sats_a[i], sats_b[i], "inter_orbit")

    return links


def _inject_sparse_faults(nodes: List[Dict], links: List[Dict], config: DynamicSceneConfig) -> Tuple[List[Dict], List[Dict], List[Dict]]:
    events = []
    for n in nodes:
        if random.random() < config.node_fault_prob_per_tick:
            n["status"] = "down"
            n["fault_tag"] = "random_node_fault"
            events.append({
                "type": "fault_event",
                "entity_type": "node",
                "entity_id": n["id"],
                "reason": "random_node_fault",
            })
    down_nodes = {n["id"] for n in nodes if n["status"] == "down"}
    for l in links:
        if l["source"] in down_nodes or l["target"] in down_nodes or random.random() < config.link_fault_prob_per_tick:
            l["status"] = "down"
            l["bandwidth_available_gbps"] = 0.0
            events.append({
                "type": "fault_event",
                "entity_type": "link",
                "entity_id": f"{l['source']}->{l['target']}",
                "reason": "random_link_fault_or_endpoint_down",
            })
    return nodes, links, events


def _build_metrics(nodes: List[Dict], links: List[Dict]) -> Dict:
    total_nodes = len(nodes)
    down_nodes = sum(1 for n in nodes if n.get("status") == "down")
    total_links = len(links)
    down_links = sum(1 for l in links if l.get("status") == "down")
    congested_links = sum(
        1
        for l in links
        if l.get("status") == "active"
        and float(l.get("bandwidth_available_gbps", 0.0)) < 0.2 * max(1e-6, float(l.get("bandwidth_gbps", 1.0)))
    )
    avg_latency = sum(float(l.get("latency_ms", 0.0)) for l in links) / max(1, total_links)
    util = 0.0
    c = 0
    for l in links:
        bw = float(l.get("bandwidth_gbps", 0.0))
        if bw <= 1e-9:
            continue
        avail = float(l.get("bandwidth_available_gbps", 0.0))
        util += max(0.0, min(1.0, 1.0 - avail / bw))
        c += 1
    avg_util = util / max(1, c)
    return {
        "total_nodes": total_nodes,
        "active_nodes": total_nodes - down_nodes,
        "down_nodes": down_nodes,
        "total_links": total_links,
        "active_links": total_links - down_links,
        "down_links": down_links,
        "congested_links": congested_links,
        "avg_latency_ms": round(avg_latency, 5),
        "avg_bandwidth_utilization": round(avg_util, 6),
    }


def generate_dynamic_scene(config: DynamicSceneConfig, output_file: Path) -> Dict:
    random.seed(config.seed)
    sat_meta, satrecs = _build_walker_satrec(config)

    start = datetime.now(timezone.utc)
    snapshots = []
    for step_idx in range(0, max(1, config.duration_sec // max(1, config.step_sec)) + 1):
        ts = start + timedelta(seconds=step_idx * config.step_sec)
        sf_t = TS.from_datetime(ts)
        nodes = [_propagate_satellite(satrec, meta, ts) for satrec, meta in zip(satrecs, sat_meta)]
        links = _build_links(nodes, config)
        nodes, links, events = _inject_sparse_faults(nodes, links, config)
        metrics = _build_metrics(nodes, links)
        snapshots.append({
            "sim_time": ts.isoformat(),
            "skyfield_tt": sf_t.tt,
            "topology_version": step_idx,
            "sampling_interval_sec": config.step_sec,
            "metadata": {
                "total_sats": len(nodes),
                "num_planes": config.num_planes,
                "altitude_km": config.altitude_km,
                "inclination_deg": config.inclination_deg,
                "sim_time": ts.isoformat(),
                "topology_version": step_idx,
                "sampling_interval_sec": config.step_sec,
            },
            "topology": {"nodes": nodes, "links": links},
            "metrics": metrics,
            "events": events,
        })

    payload = {
        "schema_version": "dynamic_topology_v1",
        "generator": "sgp4_dynamic_scene_generator",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "config": config.__dict__,
        "snapshots": snapshots,
    }
    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return payload


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate dynamic satellite scenes with SGP4 propagation.")
    p.add_argument("--output", type=Path, default=PROJECT_ROOT / "train" / "data" / "dynamic" / "scenes" / "scene_4800_v1.json")
    p.add_argument("--total-sats", type=int, default=4800)
    p.add_argument("--num-planes", type=int, default=60)
    p.add_argument("--altitude-km", type=float, default=550.0)
    p.add_argument("--inclination-deg", type=float, default=53.0)
    p.add_argument("--step-sec", type=int, default=5)
    p.add_argument("--duration-sec", type=int, default=300)
    p.add_argument("--isl-max-distance-km", type=float, default=5500.0)
    p.add_argument("--node-fault-prob-per-tick", type=float, default=0.0002)
    p.add_argument("--link-fault-prob-per-tick", type=float, default=0.0005)
    p.add_argument("--seed", type=int, default=42)
    return p.parse_args()


def main():
    args = parse_args()
    config = DynamicSceneConfig(
        total_sats=args.total_sats,
        num_planes=args.num_planes,
        altitude_km=args.altitude_km,
        inclination_deg=args.inclination_deg,
        step_sec=args.step_sec,
        duration_sec=args.duration_sec,
        isl_max_distance_km=args.isl_max_distance_km,
        node_fault_prob_per_tick=args.node_fault_prob_per_tick,
        link_fault_prob_per_tick=args.link_fault_prob_per_tick,
        seed=args.seed,
    )
    payload = generate_dynamic_scene(config, args.output)
    print(f"[dynamic_scene_generator] snapshots={len(payload['snapshots'])} output={args.output}")


if __name__ == "__main__":
    main()
