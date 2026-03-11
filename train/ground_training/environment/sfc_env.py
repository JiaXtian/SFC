"""SFC编排环境（完整指标输入 + SLA约束版）"""
import copy
from typing import Dict, List, Optional

import networkx as nx
import numpy as np
import torch


class SFCEnvironment:
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
        self.shared_resources = shared_resources
        self.strict_reliability = strict_reliability

        self.reward_config = reward_config or {
            "step_success": 12,
            "sla_bonus": 26,
            "resource_bonus": 22,
            "load_balance_bonus": 12,
            "completion_bonus": 120,
            "step_penalty": -2,
            "resource_fail": -45,
            "delay_fail": -35,
            "path_fail": -30,
            "reliability_fail": -40,
            "hotspot_penalty": -12,
        }

        self.sfc_request = None
        self.current_vnf_idx = 0
        self.prev_node = None
        self.accumulated_delay = 0.0
        self.accumulated_reliability = 1.0
        self.deployed_vnfs = []
        self.resource_snapshots = []

    @staticmethod
    def _softened_path_reliability(raw_reliability: float, hops: int) -> float:
        if hops <= 0:
            return 1.0
        raw = max(1e-9, min(1.0, float(raw_reliability)))
        geometric_mean = raw ** (1.0 / max(1, hops))
        softened_product = raw ** 0.46
        blend = (0.7 * softened_product + 0.3 * geometric_mean) if hops <= 6 else (0.82 * softened_product + 0.18 * geometric_mean)
        excess_hops = max(0, hops - 6)
        return float(max(0.0, min(1.0, blend * (0.9992 ** excess_hops))))

    def reset(self, sfc_request: Dict, reset_resources=None) -> Dict:
        should_reset = reset_resources if reset_resources is not None else (not self.shared_resources)
        if should_reset:
            self.topology = copy.deepcopy(self.original_topology)

        self.sfc_request = sfc_request
        self.current_vnf_idx = 0
        self.prev_node = sfc_request["source_node"]
        self.accumulated_delay = 0.0
        self.accumulated_reliability = 1.0
        self.deployed_vnfs = []
        self.resource_snapshots = []
        return self._build_state()

    def get_state(self) -> Optional[Dict]:
        if self.sfc_request is None:
            return None
        if self.current_vnf_idx >= len(self.sfc_request.get("vnf_sequence", [])):
            return None
        return self._build_state()

    def advance_topology(self, next_topology: nx.DiGraph, reapply_allocations: bool = True) -> Dict:
        """推进到下一时刻拓扑，并尽量重放已部署资源占用。"""
        self.topology = copy.deepcopy(next_topology)
        if self.sfc_request is None:
            self.original_topology = copy.deepcopy(next_topology)
            return {"reapplied": 0, "dropped": 0}

        if self.prev_node not in self.topology.nodes:
            source = self.sfc_request.get("source_node")
            self.prev_node = source if source in self.topology.nodes else next(iter(self.topology.nodes), None)

        if not reapply_allocations or not self.deployed_vnfs:
            self.resource_snapshots = []
            return {"reapplied": len(self.deployed_vnfs), "dropped": 0}

        valid_allocations = []
        dropped = 0
        for alloc in self.deployed_vnfs:
            node = alloc.get("node")
            path = alloc.get("path", [])
            cpu_req = float(alloc.get("cpu_required", 0.0))
            mem_req = float(alloc.get("mem_required", 0.0))
            disk_req = float(alloc.get("disk_required_gb", 0.0))
            bw_req = float(alloc.get("bandwidth_required_gbps", 0.0))

            if node not in self.topology.nodes:
                dropped += 1
                continue

            node_data = self.topology.nodes[node]
            if (
                float(node_data.get("cpu_available", 0.0)) < cpu_req
                or float(node_data.get("mem_available", 0.0)) < mem_req
                or float(node_data.get("disk_available", 0.0)) < disk_req
            ):
                dropped += 1
                continue

            path_ok = True
            for i in range(len(path) - 1):
                u, v = path[i], path[i + 1]
                if not self.topology.has_edge(u, v):
                    path_ok = False
                    break
                edge = self.topology[u][v]
                if int(edge.get("link_status", 1)) == 0:
                    path_ok = False
                    break
                if float(edge.get("bandwidth_available_gbps", 0.0)) < bw_req:
                    path_ok = False
                    break

            if not path_ok:
                dropped += 1
                continue

            node_data["cpu_available"] = float(node_data.get("cpu_available", 0.0)) - cpu_req
            node_data["mem_available"] = float(node_data.get("mem_available", 0.0)) - mem_req
            node_data["disk_available"] = float(node_data.get("disk_available", 0.0)) - disk_req
            for i in range(len(path) - 1):
                u, v = path[i], path[i + 1]
                self.topology[u][v]["bandwidth_available_gbps"] = (
                    float(self.topology[u][v].get("bandwidth_available_gbps", 0.0)) - bw_req
                )

            valid_allocations.append(alloc)

        self.deployed_vnfs = valid_allocations
        self.resource_snapshots = []
        return {"reapplied": len(valid_allocations), "dropped": dropped}

    def _extract_sla(self):
        sla = self.sfc_request.get("sla", {})
        base_reliability_req = float(
            sla.get("reliability_requirement", self.sfc_request.get("reliability_requirement", 0.97))
        )
        reliability_scale = float(self.sfc_request.get("reliability_scale", 1.0))
        effective_reliability_req = min(0.999, max(0.0, base_reliability_req * reliability_scale))
        strict_reliability = bool(self.sfc_request.get("strict_reliability", self.strict_reliability))
        return {
            "latency_requirement_ms": float(
                sla.get("latency_requirement_ms", self.sfc_request.get("max_latency_ms", 100.0))
            ),
            "bandwidth_demand_gbps": float(
                sla.get("bandwidth_demand_gbps", self.sfc_request.get("bandwidth_demand_gbps", 0.1))
            ),
            "reliability_requirement": effective_reliability_req,
            "base_reliability_requirement": base_reliability_req,
            "reliability_scale": reliability_scale,
            "strict_reliability": strict_reliability,
        }

    def _build_state(self) -> Dict:
        vnf = self.sfc_request["vnf_sequence"][self.current_vnf_idx]
        sla = self._extract_sla()
        remaining_delay = sla["latency_requirement_ms"] - self.accumulated_delay
        remaining_reliability_margin = self.accumulated_reliability - sla["reliability_requirement"]

        return {
            "topology": self.topology,
            "vnf": vnf,
            "prev_node": self.prev_node,
            "dest_node": self.sfc_request["destination_node"],
            "remaining_delay": remaining_delay,
            "current_vnf_idx": self.current_vnf_idx,
            "total_vnfs": len(self.sfc_request["vnf_sequence"]),
            "accumulated_delay": self.accumulated_delay,
            "accumulated_reliability": self.accumulated_reliability,
            "remaining_reliability_margin": remaining_reliability_margin,
            "core_network_load": float(self.sfc_request.get("core_network_load", 0.5)),
            "bandwidth_demand_gbps": sla["bandwidth_demand_gbps"],
            "reliability_requirement": sla["reliability_requirement"],
            "base_reliability_requirement": sla["base_reliability_requirement"],
            "reliability_scale": sla["reliability_scale"],
            "strict_reliability": sla["strict_reliability"],
            "priority_weight": float(self.sfc_request.get("priority_weight", 1.0)),
            "load_level": self.sfc_request.get("load_level", "medium"),
        }

    def _save_resource_snapshot(self, selected_node: str, path_to_node: List[str]):
        node_data = self.topology.nodes[selected_node]
        snapshot = {
            "node": selected_node,
            "cpu": node_data.get("cpu_available", 0.0),
            "mem": node_data.get("mem_available", 0.0),
            "disk": node_data.get("disk_available", 0.0),
            "links": [],
        }
        for i in range(len(path_to_node) - 1):
            u, v = path_to_node[i], path_to_node[i + 1]
            edge_data = self.topology[u][v]
            snapshot["links"].append(
                {
                    "u": u,
                    "v": v,
                    "bw": edge_data.get("bandwidth_available_gbps", 0.0),
                }
            )
        self.resource_snapshots.append(snapshot)

    def _rollback_resources(self):
        for snapshot in self.resource_snapshots:
            node = snapshot["node"]
            if node not in self.topology.nodes:
                continue
            self.topology.nodes[node]["cpu_available"] = snapshot["cpu"]
            self.topology.nodes[node]["mem_available"] = snapshot["mem"]
            self.topology.nodes[node]["disk_available"] = snapshot["disk"]

            for link in snapshot["links"]:
                if self.topology.has_edge(link["u"], link["v"]):
                    self.topology[link["u"]][link["v"]]["bandwidth_available_gbps"] = link["bw"]

    @staticmethod
    def _path_reliability(topology: nx.DiGraph, path: List[str]):
        if len(path) < 2:
            return 1.0
        raw_product = 1.0
        for i in range(len(path) - 1):
            edge_data = topology[path[i]][path[i + 1]]
            raw_product *= max(1e-9, float(edge_data.get("link_reliability", 0.98)))
        return SFCEnvironment._softened_path_reliability(raw_product, len(path) - 1)

    def step(self, selected_node: str, path_to_node: List[str], path_delay: float):
        vnf = self.sfc_request["vnf_sequence"][self.current_vnf_idx]
        sla = self._extract_sla()

        info = {
            "vnf_id": vnf["vnf_id"],
            "selected_node": selected_node,
            "success": False,
            "failure_reason": None,
        }

        if selected_node not in self.topology.nodes:
            self._rollback_resources()
            return None, self.reward_config["resource_fail"], True, {**info, "failure_reason": "node_not_exist"}

        node_data = self.topology.nodes[selected_node]
        cpu_req = float(vnf.get("cpu_required", 0.0))
        mem_req = float(vnf.get("mem_required", 0.0))
        disk_req = float(vnf.get("disk_required_gb", 0.0))
        bw_req = max(
            float(vnf.get("bandwidth_required_gbps", 0.0)),
            float(sla.get("bandwidth_demand_gbps", 0.0)),
        )

        if (
            node_data.get("cpu_available", 0.0) < cpu_req
            or node_data.get("mem_available", 0.0) < mem_req
            or node_data.get("disk_available", 0.0) < disk_req
        ):
            self._rollback_resources()
            return None, self.reward_config["resource_fail"], True, {
                **info,
                "failure_reason": "resource_insufficient",
            }

        same_node_deploy = bool(selected_node == self.prev_node)
        if not path_to_node:
            if same_node_deploy:
                path_to_node = [selected_node]
            else:
                self._rollback_resources()
                return None, self.reward_config["path_fail"], True, {**info, "failure_reason": "invalid_path"}
        if len(path_to_node) < 2 and (not same_node_deploy):
            self._rollback_resources()
            return None, self.reward_config["path_fail"], True, {**info, "failure_reason": "invalid_path"}

        for i in range(len(path_to_node) - 1):
            u, v = path_to_node[i], path_to_node[i + 1]
            if not self.topology.has_edge(u, v):
                self._rollback_resources()
                return None, self.reward_config["path_fail"], True, {**info, "failure_reason": "link_not_exist"}

            edge_data = self.topology[u][v]
            if int(edge_data.get("link_status", 1)) == 0:
                self._rollback_resources()
                return None, self.reward_config["path_fail"], True, {**info, "failure_reason": "link_down"}

            if edge_data.get("bandwidth_available_gbps", 0.0) < bw_req:
                self._rollback_resources()
                return None, self.reward_config["path_fail"], True, {
                    **info,
                    "failure_reason": "bandwidth_insufficient",
                }

        processing_delay = 0.25 + 2.5 * float(self.sfc_request.get("core_network_load", 0.5))
        total_delay = path_delay + processing_delay
        new_accumulated_delay = self.accumulated_delay + total_delay
        if new_accumulated_delay > sla["latency_requirement_ms"]:
            self._rollback_resources()
            return None, self.reward_config["delay_fail"], True, {
                **info,
                "failure_reason": "delay_exceeded",
            }

        node_rel = float(node_data.get("node_reliability", 0.98))
        path_rel = self._path_reliability(self.topology, path_to_node)
        new_acc_rel = self.accumulated_reliability * node_rel * path_rel

        # 训练阶段默认不做硬性可靠性剪枝，避免“几乎全部样本不可行”导致学不到策略。
        # 如果启用 strict_reliability，则保留硬约束。
        is_last_vnf = self.current_vnf_idx + 1 >= len(self.sfc_request["vnf_sequence"])
        if sla["strict_reliability"] and is_last_vnf and new_acc_rel < sla["reliability_requirement"]:
            self._rollback_resources()
            return None, self.reward_config["reliability_fail"], True, {
                **info,
                "failure_reason": "reliability_not_met",
            }

        self._save_resource_snapshot(selected_node, path_to_node)

        node_data["cpu_available"] -= cpu_req
        node_data["mem_available"] -= mem_req
        node_data["disk_available"] -= disk_req
        for i in range(len(path_to_node) - 1):
            u, v = path_to_node[i], path_to_node[i + 1]
            self.topology[u][v]["bandwidth_available_gbps"] -= bw_req

        self.deployed_vnfs.append(
            {
                "vnf_id": vnf["vnf_id"],
                "node": selected_node,
                "path": path_to_node,
                "delay": total_delay,
                "path_reliability": path_rel,
                "cpu_required": cpu_req,
                "mem_required": mem_req,
                "disk_required_gb": disk_req,
                "bandwidth_required_gbps": bw_req,
            }
        )

        self.accumulated_delay = new_accumulated_delay
        self.accumulated_reliability = new_acc_rel
        self.prev_node = selected_node
        self.current_vnf_idx += 1

        cpu_total = max(node_data.get("cpu_total", 1.0), 1e-6)
        mem_total = max(node_data.get("mem_total", 1.0), 1e-6)
        disk_total = max(node_data.get("disk_total", 1.0), 1e-6)
        node_util = np.mean(
            [
                1.0 - node_data.get("cpu_available", 0.0) / cpu_total,
                1.0 - node_data.get("mem_available", 0.0) / mem_total,
                1.0 - node_data.get("disk_available", 0.0) / disk_total,
            ]
        )

        delay_ratio = self.accumulated_delay / max(sla["latency_requirement_ms"], 1e-6)
        reliability_gap = self.accumulated_reliability - sla["reliability_requirement"]

        reward = self.reward_config["step_success"]
        reward += self.reward_config["resource_bonus"] * (1.0 - min(1.0, node_util)) * 0.4
        reward += self.reward_config["sla_bonus"] * max(0.0, 1.0 - delay_ratio) * 0.4
        reward += self.reward_config["sla_bonus"] * max(-0.5, min(0.5, reliability_gap)) * 0.8

        node_usage_count = sum(1 for dep in self.deployed_vnfs if dep["node"] == selected_node)
        if node_usage_count > 1:
            reward += self.reward_config["hotspot_penalty"] * node_usage_count
        else:
            reward += self.reward_config["load_balance_bonus"]

        reward += self.reward_config["step_penalty"]

        done = False
        if self.current_vnf_idx >= len(self.sfc_request["vnf_sequence"]):
            try:
                active_graph = nx.DiGraph()
                active_graph.add_nodes_from(self.topology.nodes(data=True))
                for u, v, d in self.topology.edges(data=True):
                    if int(d.get("link_status", 1)) == 1:
                        active_graph.add_edge(u, v, **d)
                final_path = nx.shortest_path(
                    active_graph,
                    self.prev_node,
                    self.sfc_request["destination_node"],
                    weight="latency_ms",
                )
            except (nx.NetworkXNoPath, nx.NodeNotFound):
                self._rollback_resources()
                return None, self.reward_config["path_fail"], True, {
                    **info,
                    "failure_reason": "no_path_to_dest",
                }

            if any(int(self.topology[final_path[i]][final_path[i + 1]].get("link_status", 1)) == 0 for i in range(len(final_path) - 1)):
                self._rollback_resources()
                return None, self.reward_config["path_fail"], True, {
                    **info,
                    "failure_reason": "final_path_link_down",
                }
            final_bw_req = float(sla.get("bandwidth_demand_gbps", 0.0))
            for i in range(len(final_path) - 1):
                bw_avail = float(self.topology[final_path[i]][final_path[i + 1]].get("bandwidth_available_gbps", 0.0))
                if bw_avail + 1e-9 < final_bw_req:
                    self._rollback_resources()
                    return None, self.reward_config["path_fail"], True, {
                        **info,
                        "failure_reason": "final_bandwidth_insufficient",
                    }

            final_delay = float(
                sum(self.topology[final_path[i]][final_path[i + 1]].get("latency_ms", 0.0) for i in range(len(final_path) - 1))
            )
            final_total_delay = self.accumulated_delay + final_delay
            final_rel = self.accumulated_reliability * self._path_reliability(self.topology, final_path)

            if final_total_delay > sla["latency_requirement_ms"]:
                self._rollback_resources()
                return None, self.reward_config["delay_fail"], True, {
                    **info,
                    "failure_reason": "final_delay_exceeded",
                }

            if sla["strict_reliability"] and final_rel < sla["reliability_requirement"]:
                self._rollback_resources()
                return None, self.reward_config["reliability_fail"], True, {
                    **info,
                    "failure_reason": "final_reliability_not_met",
                }

            done = True
            self.accumulated_delay = final_total_delay
            self.accumulated_reliability = final_rel
            reward += self.reward_config["completion_bonus"]
            reward += self.reward_config["completion_bonus"] * max(0.0, 1.0 - final_total_delay / max(sla["latency_requirement_ms"], 1e-6)) * 0.3
            info["success"] = True

        next_state = None if done else self._build_state()
        info.update(
            {
                "reward": float(reward),
                "accumulated_delay": float(self.accumulated_delay),
                "accumulated_reliability": float(self.accumulated_reliability),
                "deployed_vnfs": len(self.deployed_vnfs),
                "sla_latency_target": sla["latency_requirement_ms"],
                "sla_reliability_target": sla["reliability_requirement"],
                "sla_reliability_target_base": sla["base_reliability_requirement"],
                "reliability_scale": sla["reliability_scale"],
                "strict_reliability": sla["strict_reliability"],
            }
        )
        return next_state, reward, done, info

    def get_topology_state(self):
        nodes = list(self.topology.nodes())
        node_index = {n: i for i, n in enumerate(nodes)}

        node_features = []
        for n in nodes:
            node_data = self.topology.nodes[n]
            cpu_total = max(float(node_data.get("cpu_total", 1.0)), 1e-6)
            mem_total = max(float(node_data.get("mem_total", 1.0)), 1e-6)
            disk_total = max(float(node_data.get("disk_total", 1.0)), 1e-6)

            cpu_ratio = float(node_data.get("cpu_available", 0.0)) / cpu_total
            mem_ratio = float(node_data.get("mem_available", 0.0)) / mem_total
            disk_ratio = float(node_data.get("disk_available", 0.0)) / disk_total

            out_edges = list(self.topology.out_edges(n, data=True))
            if out_edges:
                active_ratio = float(np.mean([float(e[2].get("link_status", 1)) for e in out_edges]))
                bw_ratio = float(
                    np.mean(
                        [
                            float(e[2].get("bandwidth_available_gbps", 0.0))
                            / max(float(e[2].get("bandwidth_gbps", 1.0)), 1e-6)
                            for e in out_edges
                        ]
                    )
                )
                latency_norm = float(np.mean([float(e[2].get("latency_ms", 0.0)) for e in out_edges]) / 50.0)
            else:
                active_ratio = 0.0
                bw_ratio = 0.0
                latency_norm = 1.0

            node_features.append(
                [
                    cpu_ratio,
                    mem_ratio,
                    disk_ratio,
                    float(node_data.get("core_network_load", 0.5)),
                    float(node_data.get("node_reliability", 0.98)),
                    active_ratio,
                    bw_ratio,
                    min(latency_norm, 5.0),
                ]
            )

        edge_index = [[node_index[u], node_index[v]] for u, v in self.topology.edges()]

        return (
            torch.tensor(node_features, dtype=torch.float32, device=self.device),
            torch.tensor(edge_index, dtype=torch.long, device=self.device).t()
            if edge_index
            else torch.zeros((2, 0), dtype=torch.long, device=self.device),
        )

    def get_node_id_map(self):
        nodes = list(self.topology.nodes())
        return {idx: node_id for idx, node_id in enumerate(nodes)}

    def get_resource_utilization(self):
        total_cpu = sum(float(self.topology.nodes[n].get("cpu_total", 0.0)) for n in self.topology.nodes())
        used_cpu = sum(
            float(self.topology.nodes[n].get("cpu_total", 0.0)) - float(self.topology.nodes[n].get("cpu_available", 0.0))
            for n in self.topology.nodes()
        )

        total_mem = sum(float(self.topology.nodes[n].get("mem_total", 0.0)) for n in self.topology.nodes())
        used_mem = sum(
            float(self.topology.nodes[n].get("mem_total", 0.0)) - float(self.topology.nodes[n].get("mem_available", 0.0))
            for n in self.topology.nodes()
        )

        total_disk = sum(float(self.topology.nodes[n].get("disk_total", 0.0)) for n in self.topology.nodes())
        used_disk = sum(
            float(self.topology.nodes[n].get("disk_total", 0.0)) - float(self.topology.nodes[n].get("disk_available", 0.0))
            for n in self.topology.nodes()
        )

        return {
            "cpu_utilization": used_cpu / total_cpu if total_cpu > 0 else 0.0,
            "mem_utilization": used_mem / total_mem if total_mem > 0 else 0.0,
            "disk_utilization": used_disk / total_disk if total_disk > 0 else 0.0,
        }
