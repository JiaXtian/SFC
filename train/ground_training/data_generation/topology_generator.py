"""卫星星座拓扑生成器（增强指标版）"""
import json
from datetime import datetime

import networkx as nx
import numpy as np


class SatelliteConstellationGenerator:
    """Walker-Delta LEO 星座生成器，包含节点/链路完整指标。"""

    def __init__(
        self,
        total_sats=2500,
        num_planes=12,
        altitude_km=550,
        inclination_deg=53,
        isl_range_km=5016,
        node_cpu_range=(16.0, 36.0),
        node_mem_range=(32.0, 96.0),
        node_disk_range=(128.0, 1024.0),
        node_load_range=(0.15, 0.95),
        node_reliability_range=(0.97, 0.999),
        link_bw_range=(5.0, 12.0),
        link_latency_base=1.0,
        link_up_probability=0.96,
        link_reliability_range=(0.985, 0.9997),
    ):
        self.total_sats = total_sats
        self.num_planes = num_planes
        self.sats_per_plane = max(2, total_sats // num_planes)
        self.altitude_km = altitude_km
        self.inclination_deg = inclination_deg
        self.isl_range_km = isl_range_km

        self.node_cpu_range = node_cpu_range
        self.node_mem_range = node_mem_range
        self.node_disk_range = node_disk_range
        self.node_load_range = node_load_range
        self.node_reliability_range = node_reliability_range

        self.link_bw_range = link_bw_range
        self.link_latency_base = link_latency_base
        self.link_up_probability = link_up_probability
        self.link_reliability_range = link_reliability_range

        self.earth_radius_km = 6371.0
        self.orbit_radius_km = self.earth_radius_km + self.altitude_km

    def generate(self, seed=42):
        np.random.seed(seed)
        print(
            f"[ConstellationGen] 生成星座: {self.total_sats}颗卫星, {self.num_planes}轨道面"
        )

        satellites = self._generate_satellite_positions()
        links = self._build_inter_satellite_links(satellites)
        nodes = self._assign_resources(satellites)
        graph = self._build_graph(nodes, links)

        topology_dict = {
            "metadata": {
                "timestamp": datetime.now().isoformat(),
                "constellation_type": "Walker-Delta-Enhanced",
                "total_satellites": self.total_sats,
                "num_planes": self.num_planes,
                "sats_per_plane": self.sats_per_plane,
                "altitude_km": self.altitude_km,
                "inclination_deg": self.inclination_deg,
                "isl_count": len(links),
                "feature_schema": {
                    "node": [
                        "cpu_total",
                        "cpu_available",
                        "mem_total",
                        "mem_available",
                        "disk_total",
                        "disk_available",
                        "core_network_load",
                        "node_reliability",
                    ],
                    "link": [
                        "link_status",
                        "bandwidth_gbps",
                        "bandwidth_available_gbps",
                        "latency_ms",
                        "link_reliability",
                    ],
                },
            },
            "topology": {"nodes": nodes, "links": links},
        }

        print(f"[ConstellationGen] 完成: {len(nodes)} 节点, {len(links)} 链路")
        return topology_dict, graph

    def _generate_satellite_positions(self):
        satellites = []
        phase_diff = 360.0 / self.num_planes

        for plane_idx in range(self.num_planes):
            raan = plane_idx * phase_diff
            for sat_idx in range(self.sats_per_plane):
                true_anomaly = sat_idx * (360.0 / self.sats_per_plane)
                sat_id = f"SAT_{plane_idx:03d}_{sat_idx:03d}"
                coords = self._orbital_to_cartesian(raan, self.inclination_deg, true_anomaly)
                satellites.append(
                    {
                        "id": sat_id,
                        "orbital_params": {
                            "plane": plane_idx,
                            "position_in_plane": sat_idx,
                            "raan": raan,
                            "inclination": self.inclination_deg,
                            "true_anomaly": true_anomaly,
                            "altitude_km": self.altitude_km,
                        },
                        "coordinates": coords,
                    }
                )
        return satellites

    def _orbital_to_cartesian(self, raan, inclination, true_anomaly):
        raan_rad = np.deg2rad(raan)
        inc_rad = np.deg2rad(inclination)
        ta_rad = np.deg2rad(true_anomaly)

        r = self.orbit_radius_km
        x_orb = r * np.cos(ta_rad)
        y_orb = r * np.sin(ta_rad)

        x1 = x_orb * np.cos(raan_rad) - y_orb * np.sin(raan_rad)
        y1 = x_orb * np.sin(raan_rad) + y_orb * np.cos(raan_rad)

        x2 = x1
        y2 = y1 * np.cos(inc_rad)
        z2 = y1 * np.sin(inc_rad)
        return {"x": float(x2), "y": float(y2), "z": float(z2)}

    @staticmethod
    def _calculate_distance(coords1, coords2):
        dx = coords1["x"] - coords2["x"]
        dy = coords1["y"] - coords2["y"]
        dz = coords1["z"] - coords2["z"]
        return float(np.sqrt(dx**2 + dy**2 + dz**2))

    def _build_inter_satellite_links(self, satellites):
        links = []

        for sat1 in satellites:
            plane1 = sat1["orbital_params"]["plane"]
            pos1 = sat1["orbital_params"]["position_in_plane"]

            # 同轨道前后邻居
            for offset in [1, -1]:
                pos2 = (pos1 + offset) % self.sats_per_plane
                sat2_idx = plane1 * self.sats_per_plane + pos2
                if sat2_idx < len(satellites):
                    sat2 = satellites[sat2_idx]
                    self._try_add_link(links, sat1, sat2, "intra_orbit")

            # 相邻轨道同相位邻居
            for plane_offset in [1, -1]:
                plane2 = (plane1 + plane_offset) % self.num_planes
                sat2_idx = plane2 * self.sats_per_plane + pos1
                if sat2_idx < len(satellites):
                    sat2 = satellites[sat2_idx]
                    self._try_add_link(links, sat1, sat2, "inter_orbit")

        return links

    def _try_add_link(self, links, sat1, sat2, link_type):
        distance = self._calculate_distance(sat1["coordinates"], sat2["coordinates"])
        if distance > self.isl_range_km:
            return

        latency = max(0.1, distance * self.link_latency_base / 1000.0)
        bw_total = np.random.uniform(*self.link_bw_range)
        bw_avail = bw_total * np.random.uniform(0.55, 0.96)
        dist_factor = max(0.0, min(1.0, 1.0 - distance / max(1.0, self.isl_range_km)))
        reliability = float(max(0.95, min(0.9997, 0.985 + 0.014 * dist_factor)))

        link_status = 1 if np.random.random() < self.link_up_probability else 0
        if link_status == 0:
            bw_avail = 0.0

        links.append(
            {
                "source": sat1["id"],
                "target": sat2["id"],
                "link_type": link_type,
                "link_status": int(link_status),
                "latency_ms": round(float(latency * np.random.uniform(0.9, 1.15)), 3),
                "jitter_ms": round(float(np.random.uniform(0.05, 2.0)), 3),
                "bandwidth_gbps": round(float(bw_total), 2),
                "bandwidth_available_gbps": round(float(bw_avail), 2),
                "link_reliability": round(reliability, 5),
            }
        )

    def _assign_resources(self, satellites):
        nodes = []

        for sat in satellites:
            cpu_total = np.random.uniform(*self.node_cpu_range)
            cpu_available = cpu_total * np.random.uniform(0.45, 0.95)

            mem_total = np.random.uniform(*self.node_mem_range)
            mem_available = mem_total * np.random.uniform(0.45, 0.95)

            disk_total = np.random.uniform(*self.node_disk_range)
            disk_available = disk_total * np.random.uniform(0.35, 0.95)

            nodes.append(
                {
                    "id": sat["id"],
                    "type": "satellite",
                    "orbital_params": sat["orbital_params"],
                    "cpu_total": round(float(cpu_total), 2),
                    "cpu_available": round(float(cpu_available), 2),
                    "mem_total": round(float(mem_total), 2),
                    "mem_available": round(float(mem_available), 2),
                    "disk_total": round(float(disk_total), 2),
                    "disk_available": round(float(disk_available), 2),
                    "core_network_load": round(
                        float(np.random.uniform(*self.node_load_range)), 4
                    ),
                    "node_reliability": round(
                        float(np.random.uniform(*self.node_reliability_range)), 5
                    ),
                }
            )

        return nodes

    def _build_graph(self, nodes, links):
        graph = nx.DiGraph()

        for node in nodes:
            graph.add_node(
                node["id"],
                cpu_total=node["cpu_total"],
                cpu_available=node["cpu_available"],
                mem_total=node["mem_total"],
                mem_available=node["mem_available"],
                disk_total=node["disk_total"],
                disk_available=node["disk_available"],
                core_network_load=node["core_network_load"],
                node_reliability=node["node_reliability"],
                type=node["type"],
            )

        for link in links:
            graph.add_edge(
                link["source"],
                link["target"],
                latency_ms=link["latency_ms"],
                bandwidth_gbps=link["bandwidth_gbps"],
                bandwidth_available_gbps=link["bandwidth_available_gbps"],
                link_status=link["link_status"],
                link_reliability=link["link_reliability"],
                jitter_ms=link["jitter_ms"],
            )

        return graph


def _derive_num_planes(total_sats):
    if total_sats <= 1200:
        return 8
    if total_sats <= 3200:
        return 12
    return 20


def generate_multiple_topologies(num_topologies=8, base_config=None, output_dir=".", filename_prefix="topology"):
    import os

    if base_config is None:
        base_config = {"total_sats": 2500, "num_planes": 12, "altitude_km": 550}

    file_paths = []
    for i in range(num_topologies):
        config = base_config.copy()
        total_sats_base = int(base_config["total_sats"])
        sat_jitter = max(24, int(total_sats_base * 0.06))
        config["total_sats"] = int(total_sats_base + np.random.randint(-sat_jitter, sat_jitter + 1))
        config["total_sats"] = max(120, config["total_sats"])
        if "num_planes" not in config or not config["num_planes"]:
            config["num_planes"] = _derive_num_planes(config["total_sats"])
        config["altitude_km"] = float(base_config["altitude_km"] + np.random.uniform(-80, 80))
        config["isl_range_km"] = float(np.random.uniform(4200, 5600))
        config["link_up_probability"] = float(np.random.uniform(0.95, 0.995))

        generator = SatelliteConstellationGenerator(**config)
        topology_dict, _ = generator.generate(seed=42 + i)

        filename = f"{filename_prefix}_{i:03d}.json"
        filepath = os.path.join(output_dir, filename)
        os.makedirs(os.path.dirname(filepath) if os.path.dirname(filepath) else ".", exist_ok=True)

        with open(filepath, "w") as f:
            json.dump(topology_dict, f)

        file_paths.append(filepath)
        print(f"[生成] {filepath}")

    return file_paths


def generate_scaled_topologies(scale_to_count, output_dir="."):
    """
    按卫星规模批量生成拓扑。
    scale_to_count: {800: 4, 2500: 6, 6000: 4}
    """
    all_paths = []
    for scale, count in scale_to_count.items():
        scale_int = int(scale)
        count_int = int(max(1, count))
        base_cfg = {
            "total_sats": scale_int,
            "num_planes": _derive_num_planes(scale_int),
            "altitude_km": 550,
        }
        prefix = f"topology_s{scale_int}"
        paths = generate_multiple_topologies(
            num_topologies=count_int,
            base_config=base_cfg,
            output_dir=output_dir,
            filename_prefix=prefix,
        )
        all_paths.extend(paths)
    return all_paths
