"""训练器（完整指标统计 + 阶段性能日志）"""
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


class SFCTrainer:
    def __init__(
        self,
        gnn,
        agent,
        device="cpu",
        log_dir="logs",
        context_dim=48,
        history_window=0,
        backend_align_context=True,
        max_probe_candidates=24,
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
            business_load = node.get("core_business_load", {}) if isinstance(node, dict) else {}
            fallback_load = float(node.get("core_network_load", 0.5))
            graph.add_node(
                node["id"],
                cpu_total=node.get("cpu_total", 0.0),
                cpu_available=cpu_available,
                mem_total=node.get("mem_total", 0.0),
                mem_available=mem_available,
                disk_total=node.get("disk_total", 256.0),
                disk_available=disk_available,
                core_network_load=node.get("core_network_load", 0.5),
                core_business_load=business_load,
                signaling_load=float(business_load.get("signaling_load", fallback_load)),
                session_load=float(business_load.get("session_load", fallback_load)),
                user_plane_load=float(business_load.get("user_plane_load", fallback_load)),
                mobility_load=float(business_load.get("mobility_load", fallback_load)),
                policy_load=float(business_load.get("policy_load", fallback_load)),
                auth_load=float(business_load.get("auth_load", fallback_load)),
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
        ctx = torch.zeros(self.context_dim)
        total_vnfs = max(1, state.get("total_vnfs", 1))
        business_load = state.get("core_business_load", {}) if isinstance(state, dict) else {}
        business_index = float(
            (
                float(business_load.get("signaling_load", 0.5))
                + float(business_load.get("session_load", 0.5))
                + float(business_load.get("user_plane_load", 0.5))
                + float(business_load.get("mobility_load", 0.5))
                + float(business_load.get("policy_load", 0.5))
                + float(business_load.get("auth_load", 0.5))
            )
            / 6.0
        )

        remaining_delay = float(state.get("remaining_delay", 0.0))
        accumulated_delay = float(state.get("accumulated_delay", 0.0))
        reliability_req = float(state.get("reliability_requirement", 0.97))
        accumulated_reliability = float(state.get("accumulated_reliability", 1.0))

        topology_version = float(state.get("topology_version", 0.0))
        sim_time_flag = 1.0 if str(state.get("sim_time", "")).strip() else 0.0
        active_node_ratio = float(state.get("active_node_ratio", 1.0))
        active_link_ratio = float(state.get("active_link_ratio", 1.0))
        avg_latency_ms = float(state.get("avg_latency_ms", 0.0))
        avg_bw_util = float(state.get("avg_bandwidth_utilization", 0.0))

        # Backend-aligned layout:
        # [0..11] exactly match backend/src/services/InferenceEngine.cpp::build_context_features
        # [12..15] dynamic topology health stats (also injected in backend runtime now).
        base_features = [
            remaining_delay / 300.0,
            float(state.get("current_vnf_idx", 0)) / total_vnfs,
            business_index,
            float(state.get("bandwidth_demand_gbps", 0.1)) / 10.0,
            reliability_req,
            accumulated_reliability,
            0.0,
            0.0,
            accumulated_delay / 300.0,
            accumulated_reliability - reliability_req,
            topology_version / 10000.0,
            sim_time_flag,
            active_node_ratio,
            active_link_ratio,
            avg_latency_ms / 80.0,
            avg_bw_util,
            float(state.get("core_business_load", {}).get("signaling_load", business_index)),
            float(state.get("core_business_load", {}).get("session_load", business_index)),
            float(state.get("core_business_load", {}).get("user_plane_load", business_index)),
            float(state.get("core_business_load", {}).get("mobility_load", business_index)),
            float(state.get("core_business_load", {}).get("policy_load", business_index)),
            float(state.get("core_business_load", {}).get("auth_load", business_index)),
        ]
        for idx, value in enumerate(base_features):
            if idx >= self.context_dim:
                break
            ctx[idx] = float(value)

        if self.backend_align_context:
            return ctx

        if (not history_stats) or self.history_window <= 0:
            return ctx

        offset = len(base_features)
        recent_history = list(history_stats)[-self.history_window :]
        for hist in recent_history:
            history_features = [
                float(hist.get("active_node_ratio", 1.0)),
                float(hist.get("active_link_ratio", 1.0)),
                float(hist.get("avg_latency_ms", 0.0)) / 80.0,
                float(hist.get("avg_bandwidth_utilization", 0.0)),
                float(hist.get("down_node_ratio", 0.0)),
                float(hist.get("down_link_ratio", 0.0)),
                float(hist.get("congested_link_ratio", 0.0)),
                float(hist.get("topology_version_norm", 0.0)),
            ]
            for value in history_features:
                if offset >= self.context_dim:
                    break
                ctx[offset] = float(value)
                offset += 1
            if offset >= self.context_dim:
                break
        return ctx

    @staticmethod
    def _build_vnf_features(vnf):
        nf_type = str(
            vnf.get(
                "core_nf_type",
                vnf.get("nf_type", vnf.get("vnf_type", vnf.get("name", ""))),
            )
        ).lower().replace("-", "_").replace(" ", "_")
        is_user_plane = 1.0 if nf_type == "upf" else 0.0
        is_control_plane = 0.0 if is_user_plane > 0.5 else 1.0
        stateful = 1.0 if bool(vnf.get("stateful", True)) else 0.0
        processing_weight = float(vnf.get("processing_weight", 1.0))
        return torch.tensor(
            [
                float(vnf.get("cpu_required", 0.0)),
                float(vnf.get("mem_required", 0.0)),
                float(vnf.get("bandwidth_required_gbps", 0.0)),
                float(vnf.get("disk_required_gb", 0.0)),
                is_user_plane,
                is_control_plane,
                stateful,
                processing_weight,
            ],
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
                    float(node_data.get("core_network_load", 0.5)),
                    float(node_data.get("node_reliability", 0.98)),
                    active_ratio,
                    bw_ratio,
                    min(latency_norm, 5.0),
                    float(node_data.get("signaling_load", node_data.get("core_network_load", 0.5))),
                    float(node_data.get("session_load", node_data.get("core_network_load", 0.5))),
                    float(node_data.get("user_plane_load", node_data.get("core_network_load", 0.5))),
                    float(node_data.get("mobility_load", node_data.get("core_network_load", 0.5))),
                    float(node_data.get("policy_load", node_data.get("core_network_load", 0.5))),
                    float(node_data.get("auth_load", node_data.get("core_network_load", 0.5))),
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

            episode_delay_ms_local = float(
                info_obj.get("accumulated_delay", state.get("accumulated_delay", 0.0))
            )
            sla_delay_local = float(
                info_obj.get("sla_latency_target", request.get("max_latency_ms", 1e9))
            )
            sla_rel_local = float(
                info_obj.get("sla_reliability_target", request.get("reliability_requirement", 0.0))
            )
            base_sla_rel_local = float(
                info_obj.get("sla_reliability_target_base", request.get("reliability_requirement", 0.0))
            )
            final_rel_local = float(
                info_obj.get("accumulated_reliability", state.get("accumulated_reliability", 0.0))
            )

            return {
                "episode_reward": episode_reward,
                "steps": len(trajectories),
                "success": bool(success),
                "failure_reason": failure_reason_local,
                "episode_delay_ms": episode_delay_ms_local,
                "decision_latency_ms_mean": float(np.mean(decision_latencies_ms)) if decision_latencies_ms else 0.0,
                "decision_latency_ms_p95": float(np.percentile(decision_latencies_ms, 95)) if decision_latencies_ms else 0.0,
                "sla_met": bool(success and episode_delay_ms_local <= sla_delay_local and final_rel_local >= sla_rel_local),
                "full_sla_met": bool(success and episode_delay_ms_local <= sla_delay_local and final_rel_local >= base_sla_rel_local),
                "updated": did_update,
            }

        max_steps = max(12, len(request.get("vnf_sequence", request.get("core_nf_sequence", []))) + 2)

        for step in range(max_steps):
            vnf = state.get("vnf")
            prev_node = state.get("prev_node")
            dest_node = state.get("dest_node")
            remaining_delay = state.get("remaining_delay", float("inf"))

            t0 = time.perf_counter()
            candidates = heuristic_pruner.prune(
                env.topology,
                vnf,
                prev_node,
                dest_node,
                remaining_delay,
                bandwidth_demand_gbps=float(state.get("bandwidth_demand_gbps", 0.0)),
                current_vnf_idx=int(state.get("current_vnf_idx", 0)),
                total_vnfs=int(state.get("total_vnfs", 1)),
                accumulated_reliability=float(state.get("accumulated_reliability", 1.0)),
                reliability_requirement=float(state.get("reliability_requirement", 0.0)),
                accumulated_hops=int(state.get("accumulated_hops", 0)),
            )

            if not candidates:
                failure_reason = "no_candidates"
                return finish_episode(False, failure_reason)

            node_features, edge_index, nodes_list, node_index = self._get_graph_data(env.topology)
            candidate_indices = [node_index[c] for c in candidates if c in node_index]
            if not candidate_indices:
                failure_reason = "invalid_candidates"
                return finish_episode(False, failure_reason)

            node_embeddings = self.gnn(node_features, edge_index)

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
                    deterministic=True,
                )

            action_idx = int(max(0, min(action_idx, len(candidate_indices) - 1)))
            bw_req = max(
                float(vnf.get("bandwidth_required_gbps", 0.0)),
                float(state.get("bandwidth_demand_gbps", 0.0)),
            )
            probe_order = [action_idx] + [idx for idx in range(len(candidate_indices)) if idx != action_idx]
            max_probe = min(self.max_probe_candidates, len(probe_order))
            probe_candidates = []
            selected = None
            path = []
            delay = 0.0
            acc_hops = int(state.get("accumulated_hops", 0))
            target_hops = int(state.get("max_total_hops", 25))
            remain_vnfs_after_step = max(
                0,
                int(state.get("total_vnfs", 1)) - int(state.get("current_vnf_idx", 0)) - 1,
            )
            for probe_idx in probe_order[:max_probe]:
                selected_node = nodes_list[candidate_indices[probe_idx]]
                try:
                    p, d, _, h = heuristic_pruner.find_path(
                        env.topology,
                        prev_node,
                        selected_node,
                        bw_req=bw_req,
                        current_hops=int(state.get("accumulated_hops", 0)),
                        current_vnf_idx=int(state.get("current_vnf_idx", 0)),
                        total_vnfs=int(state.get("total_vnfs", 1)),
                    )
                except Exception:
                    p = []
                    d = 0.0
                    h = 0
                if p:
                    path_hops = int(max(h, len(p) - 1))
                    projected_hops = acc_hops + path_hops + 1 + remain_vnfs_after_step * 2
                    hop_over_penalty = max(0, projected_hops - target_hops)
                    probe_score = float(d) + 2.2 * float(path_hops) + 6.0 * float(hop_over_penalty)
                    probe_candidates.append((probe_score, probe_idx, selected_node, p, float(d)))
            if probe_candidates:
                probe_candidates.sort(key=lambda x: x[0])
                _score, action_idx, selected, path, delay = probe_candidates[0]
            if not path or selected is None:
                failure_reason = "path_error"
                return finish_episode(False, failure_reason)

            next_state, reward, done, info = env.step(selected, path, delay)
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

            if not done and next_state is not None and next_state.get("vnf") is not None:
                # 速度优化：复用当前步图嵌入作为bootstrap近似，避免每步二次GNN前向。
                traj["next_node_embeddings"] = node_embeddings.detach()
                traj["next_vnf_features"] = self._build_vnf_features(next_state["vnf"]).to(self.device)
                traj["next_context_features"] = self._build_context_features(next_state).to(self.device)
                _, _, _, next_node_index = self._get_graph_data(env.topology)
                next_candidates = heuristic_pruner.prune(
                    env.topology,
                    next_state["vnf"],
                    next_state.get("prev_node"),
                    next_state.get("dest_node"),
                    next_state.get("remaining_delay", float("inf")),
                    bandwidth_demand_gbps=float(next_state.get("bandwidth_demand_gbps", 0.0)),
                    current_vnf_idx=int(next_state.get("current_vnf_idx", 0)),
                    total_vnfs=int(next_state.get("total_vnfs", 1)),
                    accumulated_reliability=float(next_state.get("accumulated_reliability", 1.0)),
                    reliability_requirement=float(next_state.get("reliability_requirement", 0.0)),
                    accumulated_hops=int(next_state.get("accumulated_hops", 0)),
                )
                traj["next_candidate_indices"] = [
                    next_node_index[c] for c in next_candidates if c in next_node_index
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
        from ground_training.environment.sfc_env import SFCEnvironment

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

        metrics = {
            "epoch": epoch,
            "avg_reward": avg_reward,
            "success_rate": success_rate,
            "sla_satisfaction_rate": sla_rate,
            "full_sla_satisfaction_rate": full_sla_rate,
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
