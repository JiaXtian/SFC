"""open5gs full-core deployment environment."""

from __future__ import annotations

import copy
import heapq
from typing import Dict, List, Optional

import networkx as nx
import numpy as np
import torch

from training.open5gs_profile import (
    BUSINESS_DIMENSIONS,
    CORE_NF_RESOURCE_RANGES,
    CORE_NF_TYPES,
    NODE_FEATURE_DIM,
    dependencies_for_request,
    nf_profile,
    nf_role,
    normalize_nf_type,
    zero_business_load,
)


class SFCEnvironment:
    """Environment kept under the historical class name for import stability."""

    HOP_PENALTY_MS = 2.0

    def __init__(
        self,
        topology: nx.DiGraph,
        device="cpu",
        reward_config: Optional[Dict] = None,
        shared_resources=True,
        strict_reliability=False,
    ):
        self.device = device
        self.original_topology = copy.deepcopy(topology)
        self.topology = topology
        self.shared_resources = bool(shared_resources)
        self.strict_reliability = bool(strict_reliability)
        self.reward_config = reward_config or {
            "step_success": 0.05,
            "completion_success": 2.0,
            "quality_scale": 24.0,
            "quality_delta_scale": 3.0,
            "resource_fail": -8.0,
            "path_fail": -7.0,
            "sla_fail": -8.0,
            "hotspot_penalty": -2.2,
            "fragment_penalty": -1.6,
            "latency_margin": 1.0,
            "quality_resource_pressure": 1.55,
            "quality_business_pressure": 1.35,
        }
        self.request = None
        self.core_nfs: List[Dict] = []
        self.dependencies: List[Dict] = []
        self.current_nf_idx = 0
        self.deployed_nfs: List[Dict] = []
        self.deployed_by_type: Dict[str, Dict] = {}
        self.request_snapshots: List[Dict] = []
        self.accumulated_dependency_delay = 0.0
        self.max_dependency_delay = 0.0
        self.accumulated_hops = 0
        self.min_dependency_reliability = 1.0
        self.satisfied_dependency_keys = set()
        self.used_dependency_paths: List[List[str]] = []
        self.last_quality_score = 0.0
        self.last_quality_components: Dict[str, float] = {}
        self._path_cache = {}

    @staticmethod
    def _clamp01(value: float) -> float:
        return max(0.0, min(1.0, float(value)))

    @staticmethod
    def _estimate_link_reliability(edge_data: Dict) -> float:
        if int(edge_data.get("link_status", 1)) != 1:
            return 0.0
        base = SFCEnvironment._clamp01(edge_data.get("link_reliability", edge_data.get("reliability", 0.98)))
        bw_total = float(edge_data.get("bandwidth_gbps", 0.0))
        bw_avail = float(edge_data.get("bandwidth_available_gbps", 0.0))
        bw_ratio = SFCEnvironment._clamp01(bw_avail / bw_total) if bw_total > 1e-9 else 0.0
        return SFCEnvironment._clamp01(base * (0.985 + 0.015 * bw_ratio))

    @staticmethod
    def _path_reliability(topology: nx.DiGraph, path: List[str]) -> float:
        if len(path) < 2:
            return 1.0
        raw = 1.0
        for i in range(len(path) - 1):
            raw *= max(1e-9, SFCEnvironment._estimate_link_reliability(topology[path[i]][path[i + 1]]))
        hops = max(1, len(path) - 1)
        return float(raw ** (1.0 / hops))

    @staticmethod
    def _dependency_key(dep: Dict) -> str:
        return f"{normalize_nf_type(dep.get('source'))}->{normalize_nf_type(dep.get('target'))}"

    def reset(self, sfc_request: Dict, reset_resources=None) -> Dict:
        should_reset = reset_resources if reset_resources is not None else (not self.shared_resources)
        if should_reset:
            self.topology = copy.deepcopy(self.original_topology)

        self.request = copy.deepcopy(sfc_request)
        self.core_nfs = self._normalize_core_nfs(self.request)
        self.dependencies = self._normalize_dependencies(
            self.request.get("core_nf_dependencies") or dependencies_for_request()
        )
        self.current_nf_idx = 0
        self.deployed_nfs = []
        self.deployed_by_type = {}
        self.request_snapshots = []
        self.accumulated_dependency_delay = 0.0
        self.max_dependency_delay = 0.0
        self.accumulated_hops = 0
        self.min_dependency_reliability = 1.0
        self.satisfied_dependency_keys = set()
        self.used_dependency_paths = []
        self.last_quality_score = 0.0
        self.last_quality_components = {}
        self._path_cache = {}
        return self._build_state()

    def get_state(self) -> Optional[Dict]:
        if self.request is None or self.current_nf_idx >= len(self.core_nfs):
            return None
        return self._build_state()

    def _default_nf(self, nf_type: str, request_demand: Dict) -> Dict:
        ranges = CORE_NF_RESOURCE_RANGES.get(nf_type, CORE_NF_RESOURCE_RANGES["amf"])
        profile = nf_profile(nf_type)
        midpoint = lambda key: (float(ranges[key][0]) + float(ranges[key][1])) * 0.5
        bw_req = midpoint("bw")
        return {
            "core_nf_id": f"{self.request.get('request_id', 'core')}_{nf_type}",
            "core_nf_type": nf_type,
            "nf_type": nf_type,
            "nf_role": nf_role(nf_type),
            "stateful": nf_type not in {"scp", "nrf"},
            "cpu_required": midpoint("cpu"),
            "mem_required": midpoint("mem"),
            "disk_required_gb": midpoint("disk"),
            "bandwidth_required_gbps": bw_req,
            "business_load_demand": {
                dim: float(profile.get(dim, 0.0)) * float(request_demand.get(dim, 1.0))
                for dim in BUSINESS_DIMENSIONS
            },
        }

    def _normalize_core_nfs(self, request: Dict) -> List[Dict]:
        raw_nfs = request.get("core_nfs", [])
        if not raw_nfs:
            raw_nfs = request.get("core_nf_sequence", request.get("vnf_sequence", []))

        nf_by_type = {}
        request_demand = request.get("business_demand", zero_business_load())
        for idx, nf in enumerate(raw_nfs):
            nf_type = normalize_nf_type(nf.get("nf_type", nf.get("core_nf_type", nf.get("vnf_type", ""))))
            if not nf_type:
                nf_type = CORE_NF_TYPES[min(idx, len(CORE_NF_TYPES) - 1)]
            bw_req = max(float(nf.get("bandwidth_required_gbps", 0.0)), float(nf.get("bw_in", 0.0)), float(nf.get("bw_out", 0.0)))
            business_load = nf.get("business_load_demand", nf.get("business_load_profile", zero_business_load()))
            nf_by_type[nf_type] = {
                "core_nf_id": nf.get("core_nf_id", nf.get("vnf_id", nf.get("name", nf_type))),
                "core_nf_type": nf_type,
                "nf_type": nf_type,
                "nf_role": nf.get("nf_role", "user_plane" if nf_type == "upf" else "control_plane"),
                "stateful": bool(nf.get("stateful", True)),
                "cpu_required": float(nf.get("cpu_required", nf.get("cpu", 0.0))),
                "mem_required": float(nf.get("mem_required", nf.get("mem", 0.0))),
                "disk_required_gb": float(nf.get("disk_required_gb", nf.get("disk", 0.0))),
                "bandwidth_required_gbps": bw_req,
                "business_load_demand": {dim: float(business_load.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS},
            }
        for nf_type in CORE_NF_TYPES:
            if nf_type not in nf_by_type:
                nf_by_type[nf_type] = self._default_nf(nf_type, request_demand)
        return [nf_by_type[nf_type] for nf_type in CORE_NF_TYPES if nf_type in nf_by_type]

    def _normalize_dependencies(self, raw_dependencies: List[Dict]) -> List[Dict]:
        nf_by_type = {nf["nf_type"]: nf for nf in self.core_nfs}
        request_bw = max([float(nf.get("bandwidth_required_gbps", 0.0)) for nf in self.core_nfs] or [0.0])
        dependencies = []
        seen = set()
        for dep in raw_dependencies or dependencies_for_request():
            src = normalize_nf_type(dep.get("source"))
            dst = normalize_nf_type(dep.get("target"))
            if not src or not dst or src not in nf_by_type or dst not in nf_by_type:
                continue
            key = f"{src}->{dst}"
            if key in seen:
                continue
            seen.add(key)
            normalized = dict(dep)
            normalized["source"] = src
            normalized["target"] = dst
            if float(normalized.get("bandwidth_required_gbps", 0.0)) <= 0.0:
                src_bw = float(nf_by_type[src].get("bandwidth_required_gbps", 0.0))
                dst_bw = float(nf_by_type[dst].get("bandwidth_required_gbps", 0.0))
                base_bw = max(src_bw, dst_bw, request_bw * 0.18, 1e-6)
                normalized["bandwidth_required_gbps"] = round(
                    base_bw * float(normalized.get("bandwidth_scale", 0.5)), 4
                )
            dependencies.append(normalized)
        return dependencies or dependencies_for_request()

    def _extract_sla(self) -> Dict:
        sla = self.request.get("sla", {}) if self.request else {}
        return {
            "latency_requirement_ms": float(sla.get("latency_requirement_ms", self.request.get("max_latency_ms", 120.0))),
            "reliability_requirement": float(sla.get("reliability_requirement", self.request.get("reliability_requirement", 0.82))),
            "max_dependency_hops": int(sla.get("max_dependency_hops", self.request.get("max_dependency_hops", 24))),
        }

    def _build_state(self) -> Dict:
        nf = self.core_nfs[self.current_nf_idx]
        sla = self._extract_sla()
        relevant_deps = self._dependencies_touching(nf["nf_type"])
        already_open = [
            dep for dep in relevant_deps
            if normalize_nf_type(dep["source"]) in self.deployed_by_type or normalize_nf_type(dep["target"]) in self.deployed_by_type
        ]
        return {
            "topology": self.topology,
            "core_nf": nf,
            "vnf": nf,
            "current_nf_idx": self.current_nf_idx,
            "current_vnf_idx": self.current_nf_idx,
            "total_core_nfs": len(self.core_nfs),
            "total_vnfs": len(self.core_nfs),
            "deployed_nfs": list(self.deployed_nfs),
            "deployed_by_type": dict(self.deployed_by_type),
            "dependencies": self.dependencies,
            "open_dependencies_for_current": len(already_open),
            "critical_open_dependencies_for_current": sum(1 for dep in already_open if float(dep.get("criticality", 0.0)) >= 0.85),
            "satisfied_dependencies": len(self.satisfied_dependency_keys),
            "total_dependencies": len(self.dependencies),
            "accumulated_dependency_delay": self.accumulated_dependency_delay,
            "max_dependency_delay": self.max_dependency_delay,
            "accumulated_hops": int(self.accumulated_hops),
            "min_dependency_reliability": self.min_dependency_reliability,
            "remaining_delay": sla["latency_requirement_ms"] - self.accumulated_dependency_delay,
            "latency_requirement_ms": sla["latency_requirement_ms"],
            "reliability_requirement": sla["reliability_requirement"],
            "max_dependency_hops": sla["max_dependency_hops"],
            "business_demand": self.request.get("business_demand", zero_business_load()),
        }

    def _dependencies_touching(self, nf_type: str) -> List[Dict]:
        nf_type = normalize_nf_type(nf_type)
        return [
            dep for dep in self.dependencies
            if normalize_nf_type(dep.get("source")) == nf_type or normalize_nf_type(dep.get("target")) == nf_type
        ]

    def _new_dependencies_for_nf(self, nf_type: str, selected_node: str) -> List[Dict]:
        nf_type = normalize_nf_type(nf_type)
        paths = []
        sla = self._extract_sla()
        for dep in self._dependencies_touching(nf_type):
            key = self._dependency_key(dep)
            if key in self.satisfied_dependency_keys:
                continue
            src_type = normalize_nf_type(dep.get("source"))
            dst_type = normalize_nf_type(dep.get("target"))
            if src_type == nf_type and dst_type in self.deployed_by_type:
                src_node = selected_node
                dst_node = self.deployed_by_type[dst_type]["node"]
            elif dst_type == nf_type and src_type in self.deployed_by_type:
                src_node = self.deployed_by_type[src_type]["node"]
                dst_node = selected_node
            else:
                continue
            bw_req = float(dep.get("bandwidth_required_gbps", 0.0))
            path, delay, rel, hops, bottleneck = self._find_constrained_path(
                src_node, dst_node, bw_req=bw_req, max_hops=sla["max_dependency_hops"]
            )
            if not path and sla["max_dependency_hops"] > 0:
                path, delay, rel, hops, bottleneck = self._find_constrained_path(
                    src_node, dst_node, bw_req=bw_req, max_hops=0
                )
            if not path:
                return []
            paths.append(
                {
                    "dependency": dep,
                    "key": key,
                    "path": path,
                    "delay_ms": delay,
                    "reliability": rel,
                    "hops": hops,
                    "bottleneck_bw": bottleneck,
                    "bandwidth_required_gbps": bw_req,
                }
            )
        return paths

    def _find_constrained_path(self, source: str, target: str, bw_req: float, max_hops: int):
        cache_key = (source, target, round(float(bw_req), 4), int(max_hops))
        cached = self._path_cache.get(cache_key)
        if cached is not None:
            path, delay, rel, hops, bottleneck = cached
            return list(path), delay, rel, hops, bottleneck
        if source == target:
            return [source], 0.0, 1.0, 0, float("inf")
        if source not in self.topology.nodes or target not in self.topology.nodes:
            return [], float("inf"), 0.0, 0, 0.0

        dist = {source: 0.0}
        latency = {source: 0.0}
        hops = {source: 0}
        rel_raw = {source: 1.0}
        bottleneck = {source: float("inf")}
        prev = {}
        pq = [(0.0, source)]
        while pq:
            cur_cost, u = heapq.heappop(pq)
            if cur_cost > dist.get(u, float("inf")) + 1e-9:
                continue
            if u == target:
                break
            for v in self.topology.successors(u):
                edge = self.topology[u][v]
                if int(edge.get("link_status", 1)) != 1:
                    continue
                if float(edge.get("bandwidth_available_gbps", 0.0)) + 1e-9 < bw_req:
                    continue
                next_hops = hops[u] + 1
                if max_hops > 0 and next_hops > max_hops:
                    continue
                next_latency = latency[u] + float(edge.get("latency_ms", 0.0))
                next_cost = next_latency + self.HOP_PENALTY_MS * next_hops
                next_rel = rel_raw[u] * max(1e-9, self._estimate_link_reliability(edge))
                old = dist.get(v, float("inf"))
                if next_cost + 1e-9 >= old:
                    continue
                dist[v] = next_cost
                latency[v] = next_latency
                hops[v] = next_hops
                rel_raw[v] = next_rel
                bottleneck[v] = min(bottleneck[u], float(edge.get("bandwidth_available_gbps", 0.0)))
                prev[v] = u
                heapq.heappush(pq, (next_cost, v))

        if target not in prev:
            return [], float("inf"), 0.0, 0, 0.0
        path = [target]
        while path[-1] != source:
            p = prev.get(path[-1])
            if p is None:
                return [], float("inf"), 0.0, 0, 0.0
            path.append(p)
        path.reverse()
        hop_count = len(path) - 1
        rel = float(max(0.0, min(1.0, rel_raw.get(target, 1.0) ** (1.0 / max(1, hop_count)))))
        result = (path, float(latency.get(target, 0.0)), rel, hop_count, float(bottleneck.get(target, 0.0)))
        self._path_cache[cache_key] = (tuple(path), result[1], result[2], result[3], result[4])
        return result

    def plan_candidate(self, selected_node: str) -> Optional[Dict]:
        if self.current_nf_idx >= len(self.core_nfs):
            return None
        nf = self.core_nfs[self.current_nf_idx]
        if selected_node not in self.topology.nodes:
            return None
        node = self.topology.nodes[selected_node]
        if (
            float(node.get("cpu_available", 0.0)) + 1e-9 < float(nf["cpu_required"])
            or float(node.get("mem_available", 0.0)) + 1e-9 < float(nf["mem_required"])
            or float(node.get("disk_available", 0.0)) + 1e-9 < float(nf["disk_required_gb"])
        ):
            return None
        dep_paths = self._new_dependencies_for_nf(nf["nf_type"], selected_node)
        if dep_paths == [] and any(
            (
                normalize_nf_type(dep.get("source")) == nf["nf_type"] and normalize_nf_type(dep.get("target")) in self.deployed_by_type
            )
            or (
                normalize_nf_type(dep.get("target")) == nf["nf_type"] and normalize_nf_type(dep.get("source")) in self.deployed_by_type
            )
            for dep in self._dependencies_touching(nf["nf_type"])
        ):
            return None

        total_delay = sum(float(p["delay_ms"]) * float(p["dependency"].get("latency_weight", 1.0)) for p in dep_paths)
        max_delay = max([float(p["delay_ms"]) for p in dep_paths] or [0.0])
        total_hops = sum(int(p["hops"]) for p in dep_paths)
        min_rel = min([float(p["reliability"]) for p in dep_paths] or [1.0])
        bottleneck = min([float(p["bottleneck_bw"]) for p in dep_paths] or [float("inf")])
        node_load_after = self._project_node_business_load(node, nf)
        node_business_pressure = max(node_load_after.values()) if node_load_after else 0.0
        projected_delay = self.accumulated_dependency_delay + total_delay
        sla = self._extract_sla()
        latency_margin = float(self.reward_config.get("latency_margin", 1.0))
        if projected_delay > sla["latency_requirement_ms"] * latency_margin:
            return None
        score = (
            total_delay
            + 1.8 * total_hops
            + 4.0 * self._projected_resource_pressure(node, nf)
            + 6.0 * node_business_pressure
            + 16.0 * max(0.0, node_business_pressure - 0.72)
            + 4.0 * float(node.get("deployed_core_nf_count", 0))
            - 1.5 * min(1.0, bottleneck / max(float(nf["bandwidth_required_gbps"]), 1e-6))
        )
        return {
            "dependency_paths": dep_paths,
            "path_delay_ms": total_delay,
            "max_dependency_delay_ms": max_delay,
            "path_hops": total_hops,
            "min_dependency_reliability": min_rel,
            "bottleneck_bw": bottleneck,
            "score": float(score),
        }

    def _project_node_business_load(self, node: Dict, nf: Dict) -> Dict[str, float]:
        current = node.get("core_business_load", {})
        return {
            dim: self._clamp01(float(current.get(dim, node.get(dim, 0.0))) + float(nf["business_load_demand"].get(dim, 0.0)))
            for dim in BUSINESS_DIMENSIONS
        }

    def _save_snapshot(self, selected_node: str, dependency_paths: List[Dict]):
        node = self.topology.nodes[selected_node]
        snapshot = {
            "node": selected_node,
            "node_attrs": {
                "cpu_available": float(node.get("cpu_available", 0.0)),
                "mem_available": float(node.get("mem_available", 0.0)),
                "disk_available": float(node.get("disk_available", 0.0)),
                "core_business_load": dict(node.get("core_business_load", zero_business_load())),
                "deployed_core_nf_count": int(node.get("deployed_core_nf_count", 0)),
                "core_network_load": float(node.get("core_network_load", 0.0)),
            },
            "links": [],
        }
        seen = set()
        for item in dependency_paths:
            path = item.get("path", [])
            for i in range(len(path) - 1):
                u, v = path[i], path[i + 1]
                if (u, v) in seen or not self.topology.has_edge(u, v):
                    continue
                seen.add((u, v))
                snapshot["links"].append(
                    {
                        "u": u,
                        "v": v,
                        "bandwidth_available_gbps": float(self.topology[u][v].get("bandwidth_available_gbps", 0.0)),
                    }
                )
        self.request_snapshots.append(snapshot)

    def _rollback_request(self):
        for snapshot in reversed(self.request_snapshots):
            node_id = snapshot["node"]
            if node_id in self.topology.nodes:
                for key, value in snapshot["node_attrs"].items():
                    self.topology.nodes[node_id][key] = copy.deepcopy(value)
                for dim in BUSINESS_DIMENSIONS:
                    self.topology.nodes[node_id][dim] = float(self.topology.nodes[node_id]["core_business_load"].get(dim, 0.0))
            for link in snapshot["links"]:
                if self.topology.has_edge(link["u"], link["v"]):
                    self.topology[link["u"]][link["v"]]["bandwidth_available_gbps"] = link["bandwidth_available_gbps"]
        self.request_snapshots = []

    def step(self, selected_node: str, plan: Optional[Dict] = None):
        nf = self.core_nfs[self.current_nf_idx]
        if plan is None:
            plan = self.plan_candidate(selected_node)
        info = {
            "core_nf_id": nf["core_nf_id"],
            "core_nf_type": nf["nf_type"],
            "selected_node": selected_node,
            "success": False,
            "failure_reason": None,
        }
        if not plan:
            self._rollback_request()
            return None, self.reward_config["path_fail"], True, {**info, "failure_reason": "candidate_infeasible"}

        node = self.topology.nodes[selected_node]
        self._save_snapshot(selected_node, plan["dependency_paths"])
        node["cpu_available"] = float(node.get("cpu_available", 0.0)) - float(nf["cpu_required"])
        node["mem_available"] = float(node.get("mem_available", 0.0)) - float(nf["mem_required"])
        node["disk_available"] = float(node.get("disk_available", 0.0)) - float(nf["disk_required_gb"])
        node["deployed_core_nf_count"] = int(node.get("deployed_core_nf_count", 0)) + 1

        new_business = self._project_node_business_load(node, nf)
        node["core_business_load"] = new_business
        node["core_network_load"] = float(np.mean(list(new_business.values())))
        for dim in BUSINESS_DIMENSIONS:
            node[dim] = float(new_business[dim])

        for item in plan["dependency_paths"]:
            bw_req = float(item["bandwidth_required_gbps"])
            path = item["path"]
            self.used_dependency_paths.append(list(path))
            for i in range(len(path) - 1):
                u, v = path[i], path[i + 1]
                self.topology[u][v]["bandwidth_available_gbps"] = (
                    float(self.topology[u][v].get("bandwidth_available_gbps", 0.0)) - bw_req
                )
            self.satisfied_dependency_keys.add(item["key"])
        self._path_cache = {}

        self.deployed_nfs.append(
            {
                "core_nf_id": nf["core_nf_id"],
                "nf_type": nf["nf_type"],
                "node": selected_node,
                "business_load_demand": dict(nf["business_load_demand"]),
            }
        )
        self.deployed_by_type[nf["nf_type"]] = self.deployed_nfs[-1]
        self.accumulated_dependency_delay += float(plan["path_delay_ms"])
        self.max_dependency_delay = max(self.max_dependency_delay, float(plan["max_dependency_delay_ms"]))
        self.accumulated_hops += int(plan["path_hops"])
        self.min_dependency_reliability = min(self.min_dependency_reliability, float(plan["min_dependency_reliability"]))
        self.current_nf_idx += 1

        done = self.current_nf_idx >= len(self.core_nfs)
        prev_quality = float(self.last_quality_score)
        quality = self._compute_quality_score()
        self.last_quality_score = quality
        quality_components = self.last_quality_components
        reward = (
            self.reward_config["step_success"]
            + self.reward_config["quality_delta_scale"] * (quality - prev_quality)
            + 0.8 * (quality - 0.5)
        )
        reward -= self._hotspot_penalty(selected_node, nf)
        if done:
            sla = self._extract_sla()
            dep_ratio = len(self.satisfied_dependency_keys) / max(1, len(self.dependencies))
            if dep_ratio < 1.0:
                self._rollback_request()
                return None, self.reward_config["path_fail"], True, {**info, "failure_reason": "dependency_incomplete"}
            if self.accumulated_dependency_delay > sla["latency_requirement_ms"]:
                self._rollback_request()
                return None, self.reward_config["sla_fail"], True, {**info, "failure_reason": "latency_not_met"}
            if self.min_dependency_reliability < sla["reliability_requirement"] * 0.92:
                self._rollback_request()
                return None, self.reward_config["sla_fail"], True, {**info, "failure_reason": "reliability_not_met"}
            reward += self.reward_config["completion_success"]
            reward += self.reward_config["quality_scale"] * quality
            info["success"] = True

        next_state = None if done else self._build_state()
        info.update(
            {
                "reward": float(reward),
                "quality_score": float(quality),
                "accumulated_dependency_delay": float(self.accumulated_dependency_delay),
                "max_dependency_delay": float(self.max_dependency_delay),
                "min_dependency_reliability": float(self.min_dependency_reliability),
                "accumulated_hops": int(self.accumulated_hops),
                "satisfied_dependencies": len(self.satisfied_dependency_keys),
                "total_dependencies": len(self.dependencies),
                "deployed_core_nfs": len(self.deployed_nfs),
                "resource_balance": self._resource_balance_score(),
                "link_congestion": self._link_congestion_score(),
                "business_balance": self._business_balance_score(),
                "placement_resource": float(quality_components.get("placement_resource", 0.0)),
                "placement_business": float(quality_components.get("placement_business", 0.0)),
                "future_feasibility": float(quality_components.get("future_feasibility", 0.0)),
                "used_link_congestion": float(quality_components.get("used_link_congestion", 0.0)),
            }
        )
        return next_state, reward, done, info

    def _hotspot_penalty(self, selected_node: str, nf: Dict) -> float:
        node = self.topology.nodes[selected_node]
        count = int(node.get("deployed_core_nf_count", 0))
        business_max = max(float(node.get("core_business_load", {}).get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS)
        penalty = 0.0
        if count > 2:
            penalty += abs(self.reward_config["hotspot_penalty"]) * (count - 2) * 0.25
        if nf["nf_type"] == "upf" and business_max > 0.55:
            penalty += 1.5 * (business_max - 0.55)
        return float(penalty)

    def _compute_quality_score(self) -> float:
        sla = self._extract_sla()
        dep_ratio = len(self.satisfied_dependency_keys) / max(1, len(self.dependencies))
        delay_score = 1.0 - min(1.0, self.accumulated_dependency_delay / max(sla["latency_requirement_ms"], 1e-6))
        rel_score = min(1.0, self.min_dependency_reliability / max(sla["reliability_requirement"], 1e-6))
        placement_resource = self._placement_resource_score()
        placement_business = self._placement_business_score()
        future_feasibility = self._future_feasibility_score()
        used_link_congestion = self._used_link_congestion_score()
        used_link_good = 1.0 - used_link_congestion
        global_resource = self._resource_balance_score()
        score = (
            0.18 * dep_ratio
            + 0.22 * placement_resource
            + 0.16 * future_feasibility
            + 0.14 * placement_business
            + 0.14 * used_link_good
            + 0.10 * delay_score
            + 0.04 * rel_score
            + 0.02 * global_resource
        )
        self.last_quality_components = {
            "dependency_ratio": float(dep_ratio),
            "delay_score": float(delay_score),
            "reliability_score": float(rel_score),
            "placement_resource": float(placement_resource),
            "placement_business": float(placement_business),
            "future_feasibility": float(future_feasibility),
            "used_link_congestion": float(used_link_congestion),
            "resource_balance": float(global_resource),
        }
        return self._clamp01(score)

    def _deployed_node_records(self):
        records = []
        seen = set()
        for item in self.deployed_nfs:
            node_id = item.get("node")
            if node_id in seen or node_id not in self.topology.nodes:
                continue
            seen.add(node_id)
            records.append((node_id, self.topology.nodes[node_id]))
        return records

    @staticmethod
    def _node_resource_utils(node: Dict) -> List[float]:
        values = []
        for avail_key, total_key in [
            ("cpu_available", "cpu_total"),
            ("mem_available", "mem_total"),
            ("disk_available", "disk_total"),
        ]:
            total = max(float(node.get(total_key, 0.0)), 1e-6)
            values.append(SFCEnvironment._clamp01(1.0 - float(node.get(avail_key, 0.0)) / total))
        return values

    def _projected_resource_pressure(self, node: Dict, nf: Dict) -> float:
        projected = []
        for avail_key, total_key, req_key in [
            ("cpu_available", "cpu_total", "cpu_required"),
            ("mem_available", "mem_total", "mem_required"),
            ("disk_available", "disk_total", "disk_required_gb"),
        ]:
            total = max(float(node.get(total_key, 0.0)), 1e-6)
            util = 1.0 - (float(node.get(avail_key, 0.0)) - float(nf.get(req_key, 0.0))) / total
            projected.append(self._clamp01(util))
        return float(max(projected))

    def _placement_resource_score(self) -> float:
        records = self._deployed_node_records()
        if not records:
            return 0.65
        max_utils = []
        mean_utils = []
        colocations = []
        pressure = float(self.reward_config.get("quality_resource_pressure", 1.0))
        for _, node in records:
            utils = self._node_resource_utils(node)
            amplified = [self._clamp01(value * pressure) for value in utils]
            max_utils.append(max(amplified))
            mean_utils.append(float(np.mean(amplified)))
            colocations.append(max(0, int(node.get("deployed_core_nf_count", 0)) - 1))
        mean_pressure = float(np.mean(max_utils))
        peak_pressure = float(max(max_utils))
        spread = float(np.std(max_utils)) if len(max_utils) > 1 else 0.0
        colocation_penalty = min(1.0, float(sum(colocations)) / max(1.0, len(self.core_nfs)))
        score = (
            1.0
            - 0.76 * mean_pressure
            - 0.22 * peak_pressure
            - 0.24 * spread
            - 0.18 * colocation_penalty
            + 0.10 * (1.0 - float(np.mean(mean_utils)))
        )
        return self._clamp01(score)

    def _placement_business_score(self) -> float:
        records = self._deployed_node_records()
        if not records:
            return 0.70
        max_loads = []
        avg_loads = []
        pressure = float(self.reward_config.get("quality_business_pressure", 1.0))
        for _, node in records:
            load = node.get("core_business_load", {})
            values = [
                self._clamp01(float(load.get(dim, node.get(dim, 0.0))) * pressure)
                for dim in BUSINESS_DIMENSIONS
            ]
            max_loads.append(max(values))
            avg_loads.append(float(np.mean(values)))
        mean_load = float(np.mean(max_loads))
        peak_load = float(max(max_loads))
        spread = float(np.std(max_loads)) if len(max_loads) > 1 else 0.0
        score = 1.0 - 0.78 * mean_load - 0.22 * peak_load - 0.24 * spread + 0.08 * (1.0 - float(np.mean(avg_loads)))
        return self._clamp01(score)

    def _future_feasibility_score(self) -> float:
        nodes = self._sampled_nodes(limit=512)
        if not nodes or not self.core_nfs:
            return 0.0
        per_nf_scores = []
        resource_pressure = float(self.reward_config.get("quality_resource_pressure", 1.0))
        business_pressure_scale = float(self.reward_config.get("quality_business_pressure", 1.0))
        for nf in self.core_nfs:
            feasible = 0
            projected_pressures = []
            business_pressures = []
            for _, node in nodes:
                if (
                    float(node.get("cpu_available", 0.0)) + 1e-9 < float(nf.get("cpu_required", 0.0))
                    or float(node.get("mem_available", 0.0)) + 1e-9 < float(nf.get("mem_required", 0.0))
                    or float(node.get("disk_available", 0.0)) + 1e-9 < float(nf.get("disk_required_gb", 0.0))
                ):
                    continue
                projected_business = self._project_node_business_load(node, nf)
                business_pressure = max(projected_business.values()) if projected_business else 0.0
                if business_pressure > 0.92:
                    continue
                feasible += 1
                projected_pressures.append(self._clamp01(self._projected_resource_pressure(node, nf) * resource_pressure))
                business_pressures.append(self._clamp01(business_pressure * business_pressure_scale))
            feasible_ratio = feasible / max(1, len(nodes))
            if feasible == 0:
                per_nf_scores.append(0.0)
                continue
            resource_headroom = 1.0 - float(np.mean(projected_pressures))
            business_headroom = 1.0 - float(np.mean(business_pressures))
            per_nf_scores.append(
                self._clamp01(0.45 * feasible_ratio + 0.35 * resource_headroom + 0.20 * business_headroom)
            )
        return self._clamp01(0.70 * float(np.mean(per_nf_scores)) + 0.30 * float(min(per_nf_scores)))

    def _used_link_congestion_score(self) -> float:
        ratios = []
        seen = set()
        for path in self.used_dependency_paths:
            for i in range(len(path) - 1):
                u, v = path[i], path[i + 1]
                if (u, v) in seen or not self.topology.has_edge(u, v):
                    continue
                seen.add((u, v))
                edge = self.topology[u][v]
                total = float(edge.get("bandwidth_gbps", 0.0))
                if total <= 1e-9:
                    continue
                ratios.append(1.0 - float(edge.get("bandwidth_available_gbps", 0.0)) / total)
        if not ratios:
            return 0.0
        return self._clamp01(float(np.percentile(ratios, 90)))

    def _resource_balance_score(self) -> float:
        utils = []
        nodes = self._sampled_nodes()
        for _, node in nodes:
            for avail_key, total_key in [
                ("cpu_available", "cpu_total"),
                ("mem_available", "mem_total"),
                ("disk_available", "disk_total"),
            ]:
                total = max(float(node.get(total_key, 0.0)), 1e-6)
                utils.append(1.0 - float(node.get(avail_key, 0.0)) / total)
        if not utils:
            return 1.0
        return self._clamp01(1.0 - float(np.std(utils)) * 2.0 - max(0.0, max(utils) - 0.82))

    def _business_balance_score(self) -> float:
        values = []
        nodes = self._sampled_nodes()
        for _, node in nodes:
            load = node.get("core_business_load", {})
            values.append(max(float(load.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS))
        if not values:
            return 1.0
        return self._clamp01(1.0 - float(np.std(values)) * 2.5 - max(0.0, max(values) - 0.86))

    def _link_congestion_score(self) -> float:
        ratios = []
        edges = self._sampled_edges()
        for _, _, edge in edges:
            total = float(edge.get("bandwidth_gbps", 0.0))
            if total <= 1e-9:
                continue
            ratios.append(1.0 - float(edge.get("bandwidth_available_gbps", 0.0)) / total)
        if not ratios:
            return 0.0
        return self._clamp01(float(np.percentile(ratios, 95)))

    def _sampled_nodes(self, limit: int = 768):
        nodes = list(self.topology.nodes(data=True))
        if len(nodes) <= limit:
            return nodes
        stride = max(1, len(nodes) // limit)
        return nodes[::stride][:limit]

    def _sampled_edges(self, limit: int = 1536):
        edges = list(self.topology.edges(data=True))
        if len(edges) <= limit:
            return edges
        stride = max(1, len(edges) // limit)
        return edges[::stride][:limit]

    def get_topology_state(self):
        node_features, edge_index = self._graph_features(self.topology)
        return node_features, edge_index

    def _graph_features(self, graph):
        nodes = list(graph.nodes())
        node_index = {n: i for i, n in enumerate(nodes)}
        max_degree = max([graph.out_degree(n) for n in nodes] or [1])
        node_features = []
        for n in nodes:
            node = graph.nodes[n]
            cpu_total = max(float(node.get("cpu_total", 1.0)), 1e-6)
            mem_total = max(float(node.get("mem_total", 1.0)), 1e-6)
            disk_total = max(float(node.get("disk_total", 1.0)), 1e-6)
            out_edges = list(graph.out_edges(n, data=True))
            if out_edges:
                active_ratio = float(np.mean([float(e[2].get("link_status", 1)) for e in out_edges]))
                bw_ratio = float(np.mean([
                    float(e[2].get("bandwidth_available_gbps", 0.0)) / max(float(e[2].get("bandwidth_gbps", 1.0)), 1e-6)
                    for e in out_edges
                ]))
                latency_norm = float(np.mean([float(e[2].get("latency_ms", 0.0)) for e in out_edges]) / 50.0)
            else:
                active_ratio = 0.0
                bw_ratio = 0.0
                latency_norm = 1.0
            load = node.get("core_business_load", {})
            node_features.append(
                [
                    float(node.get("cpu_available", 0.0)) / cpu_total,
                    float(node.get("mem_available", 0.0)) / mem_total,
                    float(node.get("disk_available", 0.0)) / disk_total,
                    min(float(node.get("cpu_available", 0.0)) / 64.0, 4.0),
                    min(float(node.get("mem_available", 0.0)) / 128.0, 4.0),
                    min(float(node.get("disk_available", 0.0)) / 1024.0, 4.0),
                    float(node.get("node_reliability", 0.98)),
                    float(graph.out_degree(n)) / max(1.0, float(max_degree)),
                    active_ratio,
                    bw_ratio,
                    min(latency_norm, 5.0),
                    *[float(load.get(dim, node.get(dim, 0.0))) for dim in BUSINESS_DIMENSIONS],
                    min(1.0, float(node.get("deployed_core_nf_count", 0)) / 12.0),
                ]
            )
        edge_index = [[node_index[u], node_index[v]] for u, v in graph.edges()]
        return (
            torch.tensor(node_features, dtype=torch.float32, device=self.device),
            torch.tensor(edge_index, dtype=torch.long, device=self.device).t()
            if edge_index
            else torch.zeros((2, 0), dtype=torch.long, device=self.device),
        )

    def get_node_id_map(self):
        return {idx: node_id for idx, node_id in enumerate(self.topology.nodes())}

    def get_resource_utilization(self):
        total_cpu = sum(float(self.topology.nodes[n].get("cpu_total", 0.0)) for n in self.topology.nodes())
        used_cpu = sum(float(self.topology.nodes[n].get("cpu_total", 0.0)) - float(self.topology.nodes[n].get("cpu_available", 0.0)) for n in self.topology.nodes())
        total_mem = sum(float(self.topology.nodes[n].get("mem_total", 0.0)) for n in self.topology.nodes())
        used_mem = sum(float(self.topology.nodes[n].get("mem_total", 0.0)) - float(self.topology.nodes[n].get("mem_available", 0.0)) for n in self.topology.nodes())
        total_disk = sum(float(self.topology.nodes[n].get("disk_total", 0.0)) for n in self.topology.nodes())
        used_disk = sum(float(self.topology.nodes[n].get("disk_total", 0.0)) - float(self.topology.nodes[n].get("disk_available", 0.0)) for n in self.topology.nodes())
        return {
            "cpu_utilization": used_cpu / total_cpu if total_cpu > 0 else 0.0,
            "mem_utilization": used_mem / total_mem if total_mem > 0 else 0.0,
            "disk_utilization": used_disk / total_disk if total_disk > 0 else 0.0,
        }
