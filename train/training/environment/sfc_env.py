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
        self.last_quality_score = 0.0

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
        self.last_quality_score = 0.0
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
            "max_dependency_hops": int(sla.get("max_dependency_hops", self.request.get("max_dependency_hops", 16))),
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
        return path, float(latency.get(target, 0.0)), rel, hop_count, float(bottleneck.get(target, 0.0))

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
        if projected_delay > sla["latency_requirement_ms"] * 1.08:
            return None
        score = (
            total_delay
            + 1.8 * total_hops
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
            for i in range(len(path) - 1):
                u, v = path[i], path[i + 1]
                self.topology[u][v]["bandwidth_available_gbps"] = (
                    float(self.topology[u][v].get("bandwidth_available_gbps", 0.0)) - bw_req
                )
            self.satisfied_dependency_keys.add(item["key"])

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
        score = (
            0.20 * dep_ratio
            + 0.22 * self._resource_balance_score()
            + 0.18 * self._business_balance_score()
            + 0.18 * (1.0 - self._link_congestion_score())
            + 0.14 * delay_score
            + 0.08 * rel_score
        )
        return self._clamp01(score)

    def _resource_balance_score(self) -> float:
        utils = []
        for _, node in self.topology.nodes(data=True):
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
        for _, node in self.topology.nodes(data=True):
            load = node.get("core_business_load", {})
            values.append(max(float(load.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS))
        if not values:
            return 1.0
        return self._clamp01(1.0 - float(np.std(values)) * 2.5 - max(0.0, max(values) - 0.86))

    def _link_congestion_score(self) -> float:
        ratios = []
        for _, _, edge in self.topology.edges(data=True):
            total = float(edge.get("bandwidth_gbps", 0.0))
            if total <= 1e-9:
                continue
            ratios.append(1.0 - float(edge.get("bandwidth_available_gbps", 0.0)) / total)
        if not ratios:
            return 0.0
        return self._clamp01(float(np.percentile(ratios, 95)))

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
