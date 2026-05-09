"""卫星星座拓扑生成器（增强指标版）"""
import json
import math
from datetime import datetime

import networkx as nx
import numpy as np

try:
    from training.open5gs_profile import BUSINESS_DIMENSIONS, NODE_FEATURE_DIM, zero_business_load
except ImportError:  # pragma: no cover - direct script execution from train/training
    from open5gs_profile import BUSINESS_DIMENSIONS, NODE_FEATURE_DIM, zero_business_load


class SatelliteConstellationGenerator:
    """Walker-Delta LEO 星座生成器，使用 SGP4/TLE 风格平均根数生成节点/链路指标。"""

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
        self.sgp4_earth_radius_km = 6378.135
        self.sgp4_mu_km3_s2 = 398600.8
        self.sgp4_j2 = 1.082616e-3
        self.orbit_radius_km = self.sgp4_earth_radius_km + self.altitude_km

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
                "constellation_type": "Walker-Delta-SGP4-Enhanced",
                "propagation_model": "SGP4",
                "total_satellites": self.total_sats,
                "num_planes": self.num_planes,
                "sats_per_plane": self.sats_per_plane,
                "altitude_km": self.altitude_km,
                "inclination_deg": self.inclination_deg,
                "isl_count": len(links),
                "feature_schema": {
                    "node": [
                        "cpu_available/cpu_total",
                        "mem_available/mem_total",
                        "disk_available/disk_total",
                        "cpu_total",
                        "cpu_available",
                        "mem_total",
                        "mem_available",
                        "disk_total",
                        "disk_available",
                        "node_reliability",
                        "degree_norm",
                        "active_link_ratio",
                        "avg_bandwidth_available_ratio",
                        "avg_latency_norm",
                        "core_business_load.signaling_load",
                        "core_business_load.session_load",
                        "core_business_load.user_plane_load",
                        "core_business_load.mobility_load",
                        "core_business_load.policy_load",
                        "core_business_load.auth_load",
                        "deployed_core_nf_count_norm",
                    ],
                    "node_feature_dim": NODE_FEATURE_DIM,
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
        epoch = datetime.utcnow()
        epoch_jd = self._julian_date(epoch)

        for plane_idx in range(self.num_planes):
            raan = plane_idx * phase_diff
            for sat_idx in range(self.sats_per_plane):
                true_anomaly = sat_idx * (360.0 / self.sats_per_plane)
                sat_id = f"SAT_{plane_idx:03d}_{sat_idx:03d}"
                orbital_params = self._make_sgp4_params(
                    plane_idx,
                    sat_idx,
                    raan,
                    self.inclination_deg,
                    true_anomaly,
                    epoch,
                    epoch_jd,
                )
                coords = self._propagate_sgp4(orbital_params, 0.0)["coordinates"]
                satellites.append(
                    {
                        "id": sat_id,
                        "orbital_params": orbital_params,
                        "coordinates": coords,
                    }
                )
        return satellites

    @staticmethod
    def _wrap_deg(value):
        return float(value % 360.0)

    @staticmethod
    def _julian_date(dt):
        return 2440587.5 + dt.timestamp() / 86400.0

    def _mean_motion_from_altitude(self, altitude_km):
        semi_major = self.sgp4_earth_radius_km + max(100.0, float(altitude_km))
        n_rad_s = math.sqrt(self.sgp4_mu_km3_s2 / (semi_major**3))
        return n_rad_s * 86400.0 / (2.0 * math.pi)

    def _semi_major_axis(self, mean_motion_rev_day):
        n_rad_s = max(1e-9, float(mean_motion_rev_day)) * 2.0 * math.pi / 86400.0
        return (self.sgp4_mu_km3_s2 / (n_rad_s**2)) ** (1.0 / 3.0)

    def _tle_epoch(self, dt):
        start = datetime(dt.year, 1, 1)
        doy = (dt - start).days + 1
        seconds = dt.hour * 3600 + dt.minute * 60 + dt.second + dt.microsecond / 1_000_000
        fraction = f"{seconds / 86400.0:.8f}"[1:]
        return f"{dt.year % 100:02d}{doy:03d}{fraction}"

    def _make_tle(self, satnum, params, epoch):
        sat = f"{satnum % 100000:05d}"
        ecc7 = int(round(max(0.0, min(0.9999999, params["eccentricity"])) * 10_000_000))
        line1 = f"1 {sat}U 26001A   {self._tle_epoch(epoch)}  .00000000  00000-0  5000-4 0  9990"
        line2 = (
            f"2 {sat} {params['inclination_deg']:8.4f} {params['raan']:8.4f} "
            f"{ecc7:07d} {params['argument_of_perigee_deg']:8.4f} "
            f"{params['mean_anomaly_deg']:8.4f} {params['mean_motion_rev_per_day']:11.8f}00000"
        )
        return line1, line2

    def _make_sgp4_params(self, plane_idx, sat_idx, raan, inclination, true_anomaly, epoch, epoch_jd):
        mean_motion = self._mean_motion_from_altitude(self.altitude_km)
        semi_major = self._semi_major_axis(mean_motion)
        params = {
            "propagation_model": "SGP4",
            "plane": int(plane_idx),
            "position_in_plane": int(sat_idx),
            "raan": self._wrap_deg(raan),
            "inclination": float(inclination),
            "inclination_deg": float(inclination),
            "true_anomaly": self._wrap_deg(true_anomaly),
            "altitude_km": float(self.altitude_km),
            "eccentricity": 0.0001,
            "argument_of_perigee_deg": 0.0,
            "mean_anomaly_deg": self._wrap_deg(true_anomaly),
            "mean_motion_rev_per_day": float(mean_motion),
            "bstar": 0.00005,
            "epoch_jd": float(epoch_jd),
            "epoch_iso": epoch.isoformat() + "Z",
            "propagation_minutes": 0.0,
            "semi_major_axis_km": float(semi_major),
            "period_minutes": float(1440.0 / mean_motion),
        }
        line1, line2 = self._make_tle(plane_idx * 1000 + sat_idx + 1, params, epoch)
        params["tle_line1"] = line1
        params["tle_line2"] = line2
        return params

    @staticmethod
    def _solve_kepler(mean_anomaly_rad, eccentricity):
        ecc_anomaly = mean_anomaly_rad
        for _ in range(10):
            f = ecc_anomaly - eccentricity * math.sin(ecc_anomaly) - mean_anomaly_rad
            fp = 1.0 - eccentricity * math.cos(ecc_anomaly)
            if abs(fp) < 1e-12:
                break
            step = f / fp
            ecc_anomaly -= step
            if abs(step) < 1e-12:
                break
        return ecc_anomaly

    def _propagate_sgp4(self, params, minutes_since_epoch):
        eccentricity = max(0.0, min(0.25, float(params.get("eccentricity", 0.0001))))
        mean_motion = float(params.get("mean_motion_rev_per_day") or self._mean_motion_from_altitude(self.altitude_km))
        semi_major = self._semi_major_axis(mean_motion)
        inclination = math.radians(float(params.get("inclination_deg", params.get("inclination", self.inclination_deg))))
        p = semi_major * (1.0 - eccentricity**2)
        n_rad_min = mean_motion * 2.0 * math.pi / 1440.0
        coeff = 1.5 * self.sgp4_j2 * (self.sgp4_earth_radius_km**2) / (p**2) * n_rad_min
        raan_rate = -coeff * math.cos(inclination)
        argp_rate = 0.5 * coeff * (5.0 * math.cos(inclination) ** 2 - 1.0)
        mean_rate = n_rad_min + 0.5 * coeff * math.sqrt(max(1e-9, 1.0 - eccentricity**2)) * (
            3.0 * math.cos(inclination) ** 2 - 1.0
        )

        raan = math.radians(float(params.get("raan", 0.0))) + raan_rate * minutes_since_epoch
        argp = math.radians(float(params.get("argument_of_perigee_deg", 0.0))) + argp_rate * minutes_since_epoch
        mean = math.radians(float(params.get("mean_anomaly_deg", params.get("true_anomaly", 0.0)))) + mean_rate * minutes_since_epoch
        ecc_anomaly = self._solve_kepler(mean % (2.0 * math.pi), eccentricity)
        radius = semi_major * (1.0 - eccentricity * math.cos(ecc_anomaly))
        true_anomaly = math.atan2(
            math.sqrt(max(0.0, 1.0 - eccentricity**2)) * math.sin(ecc_anomaly),
            math.cos(ecc_anomaly) - eccentricity,
        )

        u = argp + true_anomaly
        x = radius * (math.cos(raan) * math.cos(u) - math.sin(raan) * math.sin(u) * math.cos(inclination))
        y = radius * (math.sin(raan) * math.cos(u) + math.cos(raan) * math.sin(u) * math.cos(inclination))
        z = radius * (math.sin(u) * math.sin(inclination))

        return {
            "coordinates": {
                "x": float(x),
                "y": float(y),
                "z": float(z),
                "lat": float(math.degrees(math.asin(max(-1.0, min(1.0, z / max(1.0, radius)))))),
                "lon": float(math.degrees(math.atan2(y, x))),
            },
            "true_anomaly": self._wrap_deg(math.degrees(true_anomaly)),
            "raan": self._wrap_deg(math.degrees(raan)),
            "argument_of_perigee_deg": self._wrap_deg(math.degrees(argp)),
            "altitude_km": float(radius - self.sgp4_earth_radius_km),
        }

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
                    "coordinates": sat["coordinates"],
                    "cpu_total": round(float(cpu_total), 2),
                    "cpu_available": round(float(cpu_available), 2),
                    "mem_total": round(float(mem_total), 2),
                    "mem_available": round(float(mem_available), 2),
                    "disk_total": round(float(disk_total), 2),
                    "disk_available": round(float(disk_available), 2),
                    "core_business_load": zero_business_load(),
                    "core_network_load": 0.0,
                    "deployed_core_nf_count": 0,
                    "node_reliability": round(
                        float(np.random.uniform(*self.node_reliability_range)), 5
                    ),
                }
            )

        return nodes

    def _build_graph(self, nodes, links):
        graph = nx.DiGraph()

        for node in nodes:
            business_load = node.get("core_business_load", {})
            business_values = {
                dim: float(business_load.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS
            }
            graph.add_node(
                node["id"],
                cpu_total=node["cpu_total"],
                cpu_available=node["cpu_available"],
                mem_total=node["mem_total"],
                mem_available=node["mem_available"],
                disk_total=node["disk_total"],
                disk_available=node["disk_available"],
                core_network_load=float(node.get("core_network_load", 0.0)),
                core_business_load=business_values,
                signaling_load=business_values["signaling_load"],
                session_load=business_values["session_load"],
                user_plane_load=business_values["user_plane_load"],
                mobility_load=business_values["mobility_load"],
                policy_load=business_values["policy_load"],
                auth_load=business_values["auth_load"],
                deployed_core_nf_count=int(node.get("deployed_core_nf_count", 0)),
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
