"""训练器（完整指标统计 + 阶段性能日志）"""
from __future__ import annotations
import json
import logging
import os
import random
import time
from typing import Dict, Sequence

import networkx as nx
import numpy as np
import torch
from tqdm import tqdm

from training.open5gs_profile import (
    BUSINESS_DIMENSIONS,
    CONTEXT_FEATURE_DIM,
    CORE_NF_FEATURE_DIM,
    CORE_NF_INDEX,
    NODE_FEATURE_DIM,
    zero_business_load,
)


class SFCTrainer:
    def __init__(
        self,
        gnn,
        agent,
        device="cpu",
        log_dir="logs",
        context_dim=CONTEXT_FEATURE_DIM,
        history_window=0,
        backend_align_context=True,
        max_probe_candidates=8,
    ):
        self.gnn = gnn.to(device)
        self.agent = agent
        self.device = device
        self.context_dim = int(context_dim)
        self.history_window = int(max(0, history_window))
        self.backend_align_context = bool(backend_align_context)
        self.max_probe_candidates = int(max(6, max_probe_candidates))

        os.makedirs(log_dir, exist_ok=True)
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s [%(levelname)s] %(message)s",
            handlers=[logging.FileHandler(f"{log_dir}/training.log"), logging.StreamHandler()],
            force=True,
        )
        self.logger = logging.getLogger("sfc.trainer")

    @staticmethod
    def _build_graph_from_json(topo_data):
        topology_data = topo_data.get("topology", topo_data)
        graph = nx.DiGraph()

        def _merge_or_set_edge(u, v, attrs):
            if not graph.has_edge(u, v):
                graph.add_edge(u, v, **attrs)
                return
            cur = graph[u][v]
            cur["latency_ms"] = min(float(cur.get("latency_ms", 1.0)), float(attrs.get("latency_ms", 1.0)))
            cur["bandwidth_gbps"] = max(float(cur.get("bandwidth_gbps", 0.0)), float(attrs.get("bandwidth_gbps", 0.0)))
            cur["bandwidth_available_gbps"] = max(
                float(cur.get("bandwidth_available_gbps", 0.0)),
                float(attrs.get("bandwidth_available_gbps", 0.0)),
            )
            cur["link_status"] = max(int(cur.get("link_status", 0)), int(attrs.get("link_status", 0)))
            cur["link_reliability"] = max(
                float(cur.get("link_reliability", 0.0)),
                float(attrs.get("link_reliability", 0.0)),
            )
            cur["jitter_ms"] = min(float(cur.get("jitter_ms", 0.2)), float(attrs.get("jitter_ms", 0.2)))

        for node in topology_data.get("nodes", []):
            node_status = str(node.get("status", "active")).lower()
            is_active_node = node_status not in {"down", "inactive", "failed"}
            cpu_available = float(node.get("cpu_available", 0.0))
            mem_available = float(node.get("mem_available", 0.0))
            disk_available = float(node.get("disk_available", 0.0))
            if not is_active_node:
                cpu_available = 0.0
                mem_available = 0.0
                disk_available = 0.0
            business_load = node.get("core_business_load", zero_business_load()) if isinstance(node, dict) else zero_business_load()
            business_values = {dim: float(business_load.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS}
            graph.add_node(
                node["id"],
                cpu_total=node.get("cpu_total", 0.0),
                cpu_available=cpu_available,
                mem_total=node.get("mem_total", 0.0),
                mem_available=mem_available,
                disk_total=node.get("disk_total", 256.0),
                disk_available=disk_available,
                core_network_load=float(node.get("core_network_load", 0.0)),
                core_business_load=business_values,
                signaling_load=business_values["signaling_load"],
                session_load=business_values["session_load"],
                user_plane_load=business_values["user_plane_load"],
                mobility_load=business_values["mobility_load"],
                policy_load=business_values["policy_load"],
                auth_load=business_values["auth_load"],
                deployed_core_nf_count=int(node.get("deployed_core_nf_count", 0)),
                node_reliability=node.get("node_reliability", 0.98),
                type=node.get("type", "satellite"),
                status=node_status,
                fault_tag=node.get("fault_tag", ""),
            )
        for link in topology_data.get("links", []):
            raw_status = link.get("link_status")
            if raw_status is None:
                status_text = str(link.get("status", "active")).lower()
                link_status = 1 if status_text in {"active", "up", "healthy"} else 0
            else:
                link_status = int(raw_status)
            edge_attrs = {
                "latency_ms": link.get("latency_ms", 1.0),
                "bandwidth_gbps": link.get("bandwidth_gbps", 1.0),
                "bandwidth_available_gbps": link.get("bandwidth_available_gbps", 1.0),
                "link_status": link_status,
                "link_reliability": link.get("link_reliability", link.get("reliability", 0.98)),
                "jitter_ms": link.get("jitter_ms", 0.2),
                "link_type": link.get("link_type", "isl"),
            }
            src = link["source"]
            dst = link["target"]
            _merge_or_set_edge(src, dst, edge_attrs)
            _merge_or_set_edge(dst, src, edge_attrs)
        return graph

    def _build_context_features(self, state, history_stats: Sequence[Dict] | None = None):
        del history_stats
        ctx = torch.zeros(self.context_dim)
        total_nfs = max(1, int(state.get("total_core_nfs", state.get("total_vnfs", 12))))
        current_idx = int(state.get("current_nf_idx", state.get("current_vnf_idx", 0)))
        deployed_count = len(state.get("deployed_nfs", []))
        dep_total = max(1, int(state.get("total_dependencies", 1)))
        dep_done = int(state.get("satisfied_dependencies", 0))
        latency_target = max(float(state.get("latency_requirement_ms", 120.0)), 1e-6)
        graph = state.get("topology")
        resource_util = {"cpu_utilization": 0.0, "mem_utilization": 0.0, "disk_utilization": 0.0}
        avg_bw_util = 0.0
        max_business_load = 0.0
        avg_business_load = 0.0
        if graph is not None:
            cpu_total = sum(float(graph.nodes[n].get("cpu_total", 0.0)) for n in graph.nodes())
            cpu_used = sum(float(graph.nodes[n].get("cpu_total", 0.0)) - float(graph.nodes[n].get("cpu_available", 0.0)) for n in graph.nodes())
            mem_total = sum(float(graph.nodes[n].get("mem_total", 0.0)) for n in graph.nodes())
            mem_used = sum(float(graph.nodes[n].get("mem_total", 0.0)) - float(graph.nodes[n].get("mem_available", 0.0)) for n in graph.nodes())
            disk_total = sum(float(graph.nodes[n].get("disk_total", 0.0)) for n in graph.nodes())
            disk_used = sum(float(graph.nodes[n].get("disk_total", 0.0)) - float(graph.nodes[n].get("disk_available", 0.0)) for n in graph.nodes())
            resource_util = {
                "cpu_utilization": cpu_used / cpu_total if cpu_total > 0 else 0.0,
                "mem_utilization": mem_used / mem_total if mem_total > 0 else 0.0,
                "disk_utilization": disk_used / disk_total if disk_total > 0 else 0.0,
            }
            bw_ratios = []
            node_business = []
            for _, _, edge in graph.edges(data=True):
                total = float(edge.get("bandwidth_gbps", 0.0))
                if total > 1e-9:
                    bw_ratios.append(1.0 - float(edge.get("bandwidth_available_gbps", 0.0)) / total)
            for _, node in graph.nodes(data=True):
                load = node.get("core_business_load", {})
                node_business.append(max(float(load.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS))
            avg_bw_util = float(np.mean(bw_ratios)) if bw_ratios else 0.0
            max_business_load = float(max(node_business)) if node_business else 0.0
            avg_business_load = float(np.mean(node_business)) if node_business else 0.0

        current_nf = state.get("core_nf", state.get("vnf", {}))
        current_nf_type = str(current_nf.get("nf_type", current_nf.get("core_nf_type", ""))).lower()
        current_nf_business = current_nf.get("business_load_demand", {})
        business_demand = state.get("business_demand", zero_business_load())
        base_features = [
            current_idx / total_nfs,
            max(0, total_nfs - current_idx) / total_nfs,
            deployed_count / total_nfs,
            dep_done / dep_total,
            float(state.get("accumulated_dependency_delay", 0.0)) / latency_target,
            float(state.get("max_dependency_delay", 0.0)) / latency_target,
            (float(state.get("accumulated_dependency_delay", 0.0)) / max(1, dep_done)) / latency_target,
            float(state.get("min_dependency_reliability", 1.0)),
            float(state.get("accumulated_hops", 0)) / 80.0,
            float(np.mean(list(resource_util.values()))),
            resource_util["cpu_utilization"],
            resource_util["mem_utilization"],
            resource_util["disk_utilization"],
            avg_bw_util,
            max_business_load,
            avg_business_load,
            *[float(business_demand.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS],
            float(state.get("open_dependencies_for_current", 0)) / 6.0,
            float(state.get("critical_open_dependencies_for_current", 0)) / 6.0,
            1.0 if current_nf_type == "upf" else 0.0,
            float(np.mean([float(current_nf_business.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS])) if current_nf_business else 0.0,
            float(state.get("remaining_delay", latency_target)) / latency_target,
            float(state.get("reliability_requirement", 0.82)),
            0.0,
            0.0,
            0.0,
        ]
        for idx, value in enumerate(base_features):
            if idx >= self.context_dim:
                break
            ctx[idx] = float(value)
        return ctx

    @staticmethod
    def _build_vnf_features(vnf):
        nf_type = str(
            vnf.get(
                "core_nf_type",
                vnf.get("nf_type", vnf.get("vnf_type", vnf.get("name", ""))),
            )
        ).lower().replace("-", "_").replace(" ", "_")
        one_hot = [0.0] * len(CORE_NF_INDEX)
        if nf_type in CORE_NF_INDEX:
            one_hot[CORE_NF_INDEX[nf_type]] = 1.0
        is_user_plane = 1.0 if nf_type == "upf" else 0.0
        stateful = 1.0 if bool(vnf.get("stateful", True)) else 0.0
        business = vnf.get("business_load_demand", {})
        return torch.tensor(
            one_hot
            + [
                float(vnf.get("cpu_required", 0.0)) / 8.0,
                float(vnf.get("mem_required", 0.0)) / 16.0,
                float(vnf.get("disk_required_gb", 0.0)) / 64.0,
                float(vnf.get("bandwidth_required_gbps", 0.0)) / 4.0,
                is_user_plane,
                stateful,
                *[float(business.get(dim, 0.0)) for dim in BUSINESS_DIMENSIONS],
            ][:CORE_NF_FEATURE_DIM],
            dtype=torch.float32,
        )

    @staticmethod
    def _build_active_graph(graph, bw_req=0.0):
        active_graph = nx.DiGraph()
        active_graph.add_nodes_from(graph.nodes(data=True))
        for u, v, d in graph.edges(data=True):
            if int(d.get("link_status", 1)) != 1:
                continue
            if float(d.get("bandwidth_available_gbps", 0.0)) + 1e-9 < float(bw_req):
                continue
            active_graph.add_edge(u, v, **d)
        return active_graph

    @staticmethod
    def _resolve_reliability_curriculum(epoch, total_epochs, curriculum_cfg):
        if not curriculum_cfg:
            return 1.0, 1.0

        start_epoch = int(curriculum_cfg.get("start_epoch", 1))
        end_epoch = int(curriculum_cfg.get("end_epoch", total_epochs))
        min_scale = float(curriculum_cfg.get("min_scale", 0.75))
        strict_enable_ratio = float(curriculum_cfg.get("strict_enable_ratio", 0.7))
        strict_ramp_ratio = float(curriculum_cfg.get("strict_ramp_ratio", 0.2))

        if end_epoch <= start_epoch:
            return 1.0, 1.0

        if epoch <= start_epoch:
            return min_scale, 0.0

        if epoch >= end_epoch:
            return 1.0, 1.0

        progress = float(epoch - start_epoch) / float(end_epoch - start_epoch)
        scale = min_scale + (1.0 - min_scale) * progress
        if progress < strict_enable_ratio:
            strict_prob = 0.0
        else:
            denom = max(1e-6, strict_ramp_ratio)
            strict_prob = min(1.0, (progress - strict_enable_ratio) / denom)
        return float(scale), float(strict_prob)

    def _get_graph_data(self, graph):
        nodes = list(graph.nodes())
        node_index = {n: i for i, n in enumerate(nodes)}
        max_degree = max([graph.out_degree(n) for n in nodes] or [1])

        node_features = []
        for n in nodes:
            node_data = graph.nodes[n]
            cpu_total = max(float(node_data.get("cpu_total", 1.0)), 1e-6)
            mem_total = max(float(node_data.get("mem_total", 1.0)), 1e-6)
            disk_total = max(float(node_data.get("disk_total", 1.0)), 1e-6)

            cpu_ratio = float(node_data.get("cpu_available", 0.0)) / cpu_total
            mem_ratio = float(node_data.get("mem_available", 0.0)) / mem_total
            disk_ratio = float(node_data.get("disk_available", 0.0)) / disk_total

            out_edges = list(graph.out_edges(n, data=True))
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
                    min(float(node_data.get("cpu_available", 0.0)) / 64.0, 4.0),
                    min(float(node_data.get("mem_available", 0.0)) / 128.0, 4.0),
                    min(float(node_data.get("disk_available", 0.0)) / 1024.0, 4.0),
                    float(node_data.get("node_reliability", 0.98)),
                    float(graph.out_degree(n)) / max(1.0, float(max_degree)),
                    active_ratio,
                    bw_ratio,
                    min(latency_norm, 5.0),
                    *[
                        float(node_data.get("core_business_load", {}).get(dim, node_data.get(dim, 0.0)))
                        for dim in BUSINESS_DIMENSIONS
                    ],
                    min(1.0, float(node_data.get("deployed_core_nf_count", 0)) / 12.0),
                ]
            )

        edge_index = [[node_index[u], node_index[v]] for u, v in graph.edges()]
        edge_index_tensor = (
            torch.tensor(edge_index, dtype=torch.long, device=self.device).t()
            if edge_index
            else torch.zeros((2, 0), dtype=torch.long, device=self.device)
        )

        return (
            torch.tensor(node_features, dtype=torch.float32, device=self.device),
            edge_index_tensor,
            nodes,
            node_index,
        )

    def _train_episode(self, env, graph, request, epsilon, heuristic_pruner, reset_resources=True):
        del graph
        state = env.reset(request, reset_resources=reset_resources)
        trajectories = []

        episode_reward = 0.0
        failure_reason = None
        decision_latencies_ms = []
        info = {}

        def finish_episode(success, failure_reason_local, info_obj=None):
            did_update = False
            if trajectories:
                self.agent.update(trajectories)
                did_update = True

            if info_obj is None:
                info_obj = {}

            episode_delay_ms_local = float(info_obj.get("accumulated_dependency_delay", 0.0))
            sla_delay_local = float(request.get("sla", {}).get("latency_requirement_ms", request.get("max_latency_ms", 1e9)))
            final_rel_local = float(info_obj.get("min_dependency_reliability", 1.0))
            sla_rel_local = float(request.get("sla", {}).get("reliability_requirement", request.get("reliability_requirement", 0.0)))
            dep_done = int(info_obj.get("satisfied_dependencies", 0))
            dep_total = max(1, int(info_obj.get("total_dependencies", len(request.get("core_nf_dependencies", [])) or 1)))

            return {
                "episode_reward": episode_reward,
                "steps": len(trajectories),
                "success": bool(success),
                "failure_reason": failure_reason_local,
                "episode_delay_ms": episode_delay_ms_local,
                "decision_latency_ms_mean": float(np.mean(decision_latencies_ms)) if decision_latencies_ms else 0.0,
                "decision_latency_ms_p95": float(np.percentile(decision_latencies_ms, 95)) if decision_latencies_ms else 0.0,
                "sla_met": bool(success and episode_delay_ms_local <= sla_delay_local and final_rel_local >= sla_rel_local * 0.92),
                "full_sla_met": bool(success and episode_delay_ms_local <= sla_delay_local and final_rel_local >= sla_rel_local),
                "updated": did_update,
                "quality_score": float(info_obj.get("quality_score", 0.0)),
                "dependency_satisfaction_rate": 100.0 * dep_done / dep_total,
                "resource_balance": float(info_obj.get("resource_balance", 0.0)),
                "link_congestion": float(info_obj.get("link_congestion", 0.0)),
                "business_balance": float(info_obj.get("business_balance", 0.0)),
                "max_dependency_delay": float(info_obj.get("max_dependency_delay", 0.0)),
            }

        node_features, edge_index, nodes_list, node_index = self._get_graph_data(env.topology)
        node_embeddings = self.gnn(node_features, edge_index)
        max_steps = max(12, len(request.get("core_nfs", [])))

        for step in range(max_steps):
            vnf = state.get("core_nf", state.get("vnf"))
            if vnf is None:
                break

            t0 = time.perf_counter()
            candidates = heuristic_pruner.prune(
                env.topology,
                vnf,
                deployed_by_type=state.get("deployed_by_type", {}),
                dependencies=state.get("dependencies", []),
                remaining_delay=float(state.get("remaining_delay", float("inf"))),
                current_nf_idx=int(state.get("current_nf_idx", 0)),
                total_core_nfs=int(state.get("total_core_nfs", 12)),
                accumulated_delay=float(state.get("accumulated_dependency_delay", 0.0)),
                reliability_requirement=float(state.get("reliability_requirement", 0.0)),
                accumulated_hops=int(state.get("accumulated_hops", 0)),
                max_dependency_hops=int(state.get("max_dependency_hops", 16)),
            )

            if not candidates:
                failure_reason = "no_candidates"
                return finish_episode(False, failure_reason)

            candidate_indices = [node_index[c] for c in candidates if c in node_index]
            if not candidate_indices:
                failure_reason = "invalid_candidates"
                return finish_episode(False, failure_reason)

            vnf_feat = self._build_vnf_features(vnf).to(self.device)
            ctx_feat = self._build_context_features(state).to(self.device)

            if random.random() < epsilon:
                action_idx = random.randint(0, len(candidate_indices) - 1)
            else:
                action_idx, _ = self.agent.select_action(
                    node_embeddings,
                    candidate_indices,
                    vnf_feat,
                    ctx_feat,
                    deterministic=False,
                )

            action_idx = int(max(0, min(action_idx, len(candidate_indices) - 1)))
            probe_order = [action_idx] + [idx for idx in range(len(candidate_indices)) if idx != action_idx]
            max_probe = min(self.max_probe_candidates, len(probe_order))
            probe_candidates = []
            selected = None
            selected_plan = None
            for probe_idx in probe_order[:max_probe]:
                selected_node = nodes_list[candidate_indices[probe_idx]]
                plan = env.plan_candidate(selected_node)
                if plan:
                    probe_candidates.append((float(plan.get("score", 0.0)), probe_idx, selected_node, plan))
            if probe_candidates:
                probe_candidates.sort(key=lambda x: x[0])
                _score, action_idx, selected, selected_plan = probe_candidates[0]
            if selected is None or selected_plan is None:
                failure_reason = "candidate_infeasible"
                return finish_episode(False, failure_reason)

            next_state, reward, done, info = env.step(selected, selected_plan)
            episode_reward += float(reward)

            step_elapsed_ms = (time.perf_counter() - t0) * 1000.0
            decision_latencies_ms.append(step_elapsed_ms)

            traj = {
                "node_embeddings": node_embeddings,
                "candidate_indices": candidate_indices,
                "vnf_features": vnf_feat,
                "context_features": ctx_feat,
                "action": action_idx,
                "reward": float(reward),
                "done": done,
            }

            if not done and next_state is not None and next_state.get("core_nf") is not None:
                traj["next_node_embeddings"] = node_embeddings.detach()
                traj["next_vnf_features"] = self._build_vnf_features(next_state["core_nf"]).to(self.device)
                traj["next_context_features"] = self._build_context_features(next_state).to(self.device)
                next_candidates = heuristic_pruner.prune(
                    env.topology,
                    next_state["core_nf"],
                    deployed_by_type=next_state.get("deployed_by_type", {}),
                    dependencies=next_state.get("dependencies", []),
                    remaining_delay=float(next_state.get("remaining_delay", float("inf"))),
                    current_nf_idx=int(next_state.get("current_nf_idx", 0)),
                    total_core_nfs=int(next_state.get("total_core_nfs", 12)),
                    accumulated_delay=float(next_state.get("accumulated_dependency_delay", 0.0)),
                    reliability_requirement=float(next_state.get("reliability_requirement", 0.0)),
                    accumulated_hops=int(next_state.get("accumulated_hops", 0)),
                    max_dependency_hops=int(next_state.get("max_dependency_hops", 16)),
                )
                traj["next_candidate_indices"] = [
                    node_index[c] for c in next_candidates if c in node_index
                ]

            trajectories.append(traj)

            if done:
                break
            state = next_state

        success = bool(info.get("success", False)) if isinstance(info, dict) else False
        if not success:
            failure_reason = info.get("failure_reason", "unknown") if isinstance(info, dict) else "unknown"
            if not failure_reason:
                failure_reason = "max_steps_reached"
        return finish_episode(success, failure_reason, info if isinstance(info, dict) else {})

    def train_epoch(
        self,
        epoch,
        train_data,
        epsilon,
        heuristic_pruner,
        shared_resources_prob=0.4,
        max_requests_per_file=100,
        total_epochs=1,
        reliability_curriculum=None,
    ):
        from training.environment.sfc_env import SFCEnvironment

        epoch_start = time.time()
        total_requests = 0
        success_count = 0
        sla_met_count = 0
        full_sla_met_count = 0
        updated_episodes = 0
        resource_fail_count = 0
        failure_reason_counts = {}

        rewards = []
        episode_delays = []
        decision_lat_means = []
        decision_lat_p95s = []
        quality_scores = []
        dependency_rates = []
        resource_balance_scores = []
        link_congestion_scores = []
        business_balance_scores = []
        reliability_scale, strict_reliability_prob = self._resolve_reliability_curriculum(
            epoch, total_epochs, reliability_curriculum
        )

        pbar = tqdm(train_data, desc=f"Epoch {epoch}")
        for topo_idx, (topo_file, req_file) in enumerate(pbar):
            with open(topo_file) as f:
                topo_data = json.load(f)
            graph = self._build_graph_from_json(topo_data)

            with open(req_file) as f:
                req_data = json.load(f)

            use_shared = random.random() < shared_resources_prob
            env = SFCEnvironment(graph, self.device, shared_resources=use_shared)

            requests = req_data.get("requests", [])[:max_requests_per_file]
            for req_idx, request in enumerate(requests):
                total_requests += 1
                request_for_train = dict(request)
                request_for_train["reliability_scale"] = reliability_scale
                request_for_train["strict_reliability"] = random.random() < strict_reliability_prob

                episode_result = self._train_episode(
                    env,
                    graph,
                    request_for_train,
                    epsilon,
                    heuristic_pruner,
                    reset_resources=(not use_shared),
                )

                rewards.append(episode_result["episode_reward"])
                episode_delays.append(episode_result["episode_delay_ms"])
                decision_lat_means.append(episode_result["decision_latency_ms_mean"])
                decision_lat_p95s.append(episode_result["decision_latency_ms_p95"])
                quality_scores.append(float(episode_result.get("quality_score", 0.0)))
                dependency_rates.append(float(episode_result.get("dependency_satisfaction_rate", 0.0)))
                resource_balance_scores.append(float(episode_result.get("resource_balance", 0.0)))
                link_congestion_scores.append(float(episode_result.get("link_congestion", 0.0)))
                business_balance_scores.append(float(episode_result.get("business_balance", 0.0)))

                if episode_result["success"]:
                    success_count += 1
                if episode_result.get("updated", False):
                    updated_episodes += 1
                if episode_result["sla_met"]:
                    sla_met_count += 1
                if episode_result["full_sla_met"]:
                    full_sla_met_count += 1
                failure_reason = str(episode_result.get("failure_reason", ""))
                if not episode_result["success"] and failure_reason and failure_reason.lower() != "none":
                    failure_reason_counts[failure_reason] = failure_reason_counts.get(failure_reason, 0) + 1
                    if "resource" in failure_reason:
                        resource_fail_count += 1

                if req_idx % 10 == 0:
                    succ_rate = 100.0 * success_count / max(1, total_requests)
                    pbar.set_postfix(
                        {
                            "Topo": f"{topo_idx + 1}/{len(train_data)}",
                            "Succ": f"{succ_rate:.1f}%",
                            "Fail": int(total_requests - success_count),
                            "AlgLat": f"{np.mean(decision_lat_means):.2f}ms" if decision_lat_means else "0ms",
                        }
                    )

        epoch_time = time.time() - epoch_start
        avg_reward = float(np.mean(rewards)) if rewards else 0.0
        success_rate = 100.0 * success_count / max(1, total_requests)
        sla_rate = 100.0 * sla_met_count / max(1, total_requests)
        full_sla_rate = 100.0 * full_sla_met_count / max(1, total_requests)
        avg_ep_delay = float(np.mean(episode_delays)) if episode_delays else 0.0
        avg_alg_delay = float(np.mean(decision_lat_means)) if decision_lat_means else 0.0
        p95_alg_delay = float(np.percentile(decision_lat_p95s, 95)) if decision_lat_p95s else 0.0
        avg_quality = float(np.mean(quality_scores)) if quality_scores else 0.0
        avg_dependency_rate = float(np.mean(dependency_rates)) if dependency_rates else 0.0
        avg_resource_balance = float(np.mean(resource_balance_scores)) if resource_balance_scores else 0.0
        avg_link_congestion = float(np.mean(link_congestion_scores)) if link_congestion_scores else 0.0
        avg_business_balance = float(np.mean(business_balance_scores)) if business_balance_scores else 0.0

        metrics = {
            "epoch": epoch,
            "avg_reward": avg_reward,
            "avg_quality_score": avg_quality,
            "success_rate": success_rate,
            "complete_core_deployment_rate": success_rate,
            "sla_satisfaction_rate": sla_rate,
            "full_sla_satisfaction_rate": full_sla_rate,
            "dependency_satisfaction_rate": avg_dependency_rate,
            "resource_balance_score": avg_resource_balance,
            "link_congestion_score": avg_link_congestion,
            "business_balance_score": avg_business_balance,
            "random_baseline_win_rate": 0.0,
            "avg_episode_delay_ms": avg_ep_delay,
            "avg_algorithm_latency_ms": avg_alg_delay,
            "p95_algorithm_latency_ms": p95_alg_delay,
            "resource_fail_count": resource_fail_count,
            "total_requests": total_requests,
            "updated_episodes": updated_episodes,
            "epoch_time_s": epoch_time,
            "epsilon": float(epsilon),
            "reliability_scale": reliability_scale,
            "strict_reliability_prob": strict_reliability_prob,
            "top_failure_reasons": sorted(
                failure_reason_counts.items(), key=lambda kv: kv[1], reverse=True
            )[:5],
        }

        self.logger.info(
            "Epoch %d | Reward=%.2f | Success=%.2f%% | DepDelay=%.2fms | "
            "Fail=%d/%d | TopFail=%s",
            epoch,
            avg_reward,
            success_rate,
            avg_ep_delay,
            int(total_requests - success_count),
            total_requests,
            metrics["top_failure_reasons"],
        )
        return metrics
