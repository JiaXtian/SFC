#include "sfc_orchestrator.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <cctype>
#include <unordered_map>

namespace sfc {

static std::string path_to_string(const std::vector<std::string>& path) {
    if (path.empty()) {
        return "<none>";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            oss << " -> ";
        }
        oss << path[i];
    }
    return oss.str();
}

static std::string normalize_nf_type(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '-' || ch == ' ') out.push_back('_');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

static std::vector<float> build_core_nf_features(const VNFRequirement& vnf) {
    const std::string nf_type = normalize_nf_type(
        !vnf.nf_type.empty() ? vnf.nf_type : (!vnf.core_nf_type.empty() ? vnf.core_nf_type : vnf.vnf_type)
    );
    const float is_user_plane = (nf_type == "upf") ? 1.0f : 0.0f;
    const float is_control_plane = is_user_plane > 0.5f ? 0.0f : 1.0f;
    const float stateful = vnf.stateful ? 1.0f : 0.0f;
    const float processing_weight = std::max(0.3f, std::min(3.0f, vnf.processing_weight));
    return {
        vnf.cpu_required,
        vnf.mem_required,
        vnf.bandwidth_required_gbps,
        vnf.disk_required_gb,
        is_user_plane,
        is_control_plane,
        stateful,
        processing_weight,
    };
}

SFCOrchestrator::SFCOrchestrator(std::shared_ptr<HeuristicPruner> pruner, std::shared_ptr<DRLInference> drl)
    : pruner_(pruner), drl_(drl) {}

std::vector<std::string> SFCOrchestrator::find_shortest_path(
    const NetworkGraph& graph,
    const std::string& from,
    const std::string& to,
    float required_bw) const {
    std::unordered_map<std::string, float> distances;
    std::unordered_map<std::string, std::string> previous;
    std::priority_queue<std::pair<float, std::string>, std::vector<std::pair<float, std::string>>, std::greater<>> pq;

    for (const auto& node : graph.get_nodes()) {
        distances[node.id] = std::numeric_limits<float>::infinity();
    }
    distances[from] = 0.0f;
    pq.emplace(0.0f, from);

    while (!pq.empty()) {
        auto [current_dist, current_node] = pq.top();
        pq.pop();

        if (current_node == to) {
            break;
        }
        if (current_dist > distances[current_node]) {
            continue;
        }

        const Node* node = graph.get_node(current_node);
        if (!node) {
            continue;
        }

        for (size_t neighbor_idx : node->neighbors) {
            const auto& neighbor = graph.get_node_by_index(neighbor_idx);
            const Link* link = graph.get_link(current_node, neighbor.id);
            if (!link || link->link_status == 0) {
                continue;
            }
            if (required_bw > 0.0f && link->bandwidth_available_gbps + 1e-6f < required_bw) {
                continue;
            }

            float new_dist = current_dist + link->latency_ms;
            if (new_dist < distances[neighbor.id]) {
                distances[neighbor.id] = new_dist;
                previous[neighbor.id] = current_node;
                pq.emplace(new_dist, neighbor.id);
            }
        }
    }

    std::vector<std::string> path;
    if (distances[to] == std::numeric_limits<float>::infinity()) {
        return path;
    }

    std::string current = to;
    while (current != from) {
        path.push_back(current);
        auto it = previous.find(current);
        if (it == previous.end()) {
            return {};
        }
        current = it->second;
    }
    path.push_back(from);
    std::reverse(path.begin(), path.end());
    return path;
}

float SFCOrchestrator::calculate_path_delay(const NetworkGraph& graph, const std::vector<std::string>& path) const {
    float total_delay = 0.0f;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* link = graph.get_link(path[i], path[i + 1]);
        if (link) {
            total_delay += link->latency_ms;
        }
    }
    return total_delay;
}

bool SFCOrchestrator::check_path_bandwidth(
    const NetworkGraph& graph,
    const std::vector<std::string>& path,
    float required_bw) const {
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* link = graph.get_link(path[i], path[i + 1]);
        if (!link || link->link_status == 0 || link->bandwidth_available_gbps < required_bw) {
            return false;
        }
    }
    return true;
}

static float path_reliability(const NetworkGraph& graph, const std::vector<std::string>& path) {
    if (path.size() < 2) {
        return 1.0f;
    }
    float sum_log_rel = 0.0f;
    int hops = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* link = graph.get_link(path[i], path[i + 1]);
        if (!link || link->link_status == 0) {
            return 0.0f;
        }
        float rel = std::max(1e-6f, link->link_reliability);
        sum_log_rel += std::log(rel);
        hops += 1;
    }
    return std::exp(sum_log_rel / std::max(1, hops));
}

static std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt = *std::localtime(&timer);

    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

DeploymentResult SFCOrchestrator::deploy_sfc(
    NetworkGraph& graph,
    const SFCRequest& request,
    const std::vector<float>& node_embeddings,
    bool verbose) {
    DeploymentResult result;
    result.request_id = request.request_id;
    result.success = false;
    result.total_delay_ms = 0.0f;

    if (verbose) {
        std::cout << "\n└---------------------------------------------------------------------┘" << std::endl;
        std::cout << "│ " << std::setw(68) << std::left << ("SFC " + request.request_id + " (" + request.service_type + ")") << " │" << std::endl;
        std::cout << "├---------------------------------------------------------------------┤" << std::endl;
        std::cout << "│ 源节点: " << std::setw(58) << std::left << request.source_node << " │" << std::endl;
        std::cout << "│ 目的节点: " << std::setw(56) << std::left << request.destination_node << " │" << std::endl;
        std::cout << "│ 核心网网元数量: " << std::setw(53) << std::left << request.vnf_sequence.size() << " │" << std::endl;
        std::cout << "│ SLA时延: " << std::setw(56) << std::left << (std::to_string(request.max_latency_ms) + " ms") << " │" << std::endl;
        std::cout << "│ SLA可靠性: " << std::setw(54) << std::left << request.reliability_requirement << " │" << std::endl;
        std::cout << "│ 优先级: " << std::setw(58) << std::left << request.priority << " │" << std::endl;
        std::cout << "│ 开始时间: " << std::setw(56) << std::left << get_timestamp() << " │" << std::endl;
        std::cout << "└---------------------------------------------------------------------┘" << std::endl;
    }

    std::string prev_node = request.source_node;
    float accumulated_delay = 0.0f;
    float accumulated_reliability = 1.0f;

    for (size_t vnf_idx = 0; vnf_idx < request.vnf_sequence.size(); ++vnf_idx) {
        const auto& vnf = request.vnf_sequence[vnf_idx];
        float remaining_delay = request.max_latency_ms - accumulated_delay;
        VNFDeploymentTrace vnf_trace;
        vnf_trace.vnf_id = vnf.vnf_id;
        vnf_trace.vnf_type = !vnf.nf_type.empty() ? vnf.nf_type : vnf.vnf_type;
        vnf_trace.remaining_delay_before_ms = remaining_delay;
        vnf_trace.accumulated_delay_before_ms = accumulated_delay;
        vnf_trace.accumulated_reliability_before = accumulated_reliability;

        if (verbose) {
            std::cout << "\n┌- Core NF " << (vnf_idx + 1) << "/" << request.vnf_sequence.size() << ": " << vnf.vnf_type
                      << " " << std::string(52 - std::min<size_t>(52, vnf.vnf_type.length()), '-') << "┐" << std::endl;
            std::cout << "│ Core NF ID: " << vnf.vnf_id << std::endl;
            std::cout << "│ 资源需求: CPU=" << vnf.cpu_required << " cores, MEM=" << vnf.mem_required
                      << " GB, DISK=" << vnf.disk_required_gb << " GB, BW=" << vnf.bandwidth_required_gbps << " Gbps" << std::endl;
            std::cout << "│ 剩余时延预算: " << remaining_delay << " ms, 当前累计可靠性: " << accumulated_reliability << std::endl;
        }

        auto candidates = pruner_->get_candidate_nodes(graph, vnf, prev_node, request.destination_node, remaining_delay);
        vnf_trace.candidate_count = static_cast<int>(candidates.size());
        if (verbose) {
            std::cout << "│ 候选节点数: " << candidates.size() << std::endl;
        }
        if (candidates.empty()) {
            vnf_trace.failure_reason = "No candidate nodes after heuristic pruning";
            result.failure_reason = "No candidate nodes for core NF " + vnf.vnf_id;
            result.vnf_traces.push_back(vnf_trace);
            if (verbose) {
                std::cout << "│ ✗ 失败: " << vnf_trace.failure_reason << std::endl;
            }
            return result;
        }

        std::vector<int> candidate_indices;
        for (const auto& cand : candidates) {
            try {
                candidate_indices.push_back(static_cast<int>(graph.get_node_index(cand)));
            } catch (...) {
                continue;
            }
        }
        if (candidate_indices.empty()) {
            vnf_trace.failure_reason = "All heuristic candidates were invalid in graph index lookup";
            result.failure_reason = "Invalid candidates for core NF " + vnf.vnf_id;
            result.vnf_traces.push_back(vnf_trace);
            if (verbose) {
                std::cout << "│ ✗ 失败: " << vnf_trace.failure_reason << std::endl;
            }
            return result;
        }

        std::vector<float> vnf_features = build_core_nf_features(vnf);
        std::vector<float> context_features(48, 0.0f);
        const float request_business_index = (
            request.core_business_load.signaling_load +
            request.core_business_load.session_load +
            request.core_business_load.user_plane_load +
            request.core_business_load.mobility_load +
            request.core_business_load.policy_load +
            request.core_business_load.auth_load
        ) / 6.0f;
        context_features[0] = remaining_delay / 300.0f;
        context_features[1] = static_cast<float>(vnf_idx) / std::max<size_t>(1, request.vnf_sequence.size());
        context_features[2] = request_business_index;
        context_features[3] = request.bandwidth_demand_gbps / 10.0f;
        context_features[4] = request.reliability_requirement;
        context_features[5] = accumulated_reliability;
        context_features[6] = 0.0f;
        context_features[7] = 0.0f;
        context_features[8] = accumulated_delay / 300.0f;
        context_features[9] = accumulated_reliability - request.reliability_requirement;
        context_features[16] = request.core_business_load.signaling_load;
        context_features[17] = request.core_business_load.session_load;
        context_features[18] = request.core_business_load.user_plane_load;
        context_features[19] = request.core_business_load.mobility_load;
        context_features[20] = request.core_business_load.policy_load;
        context_features[21] = request.core_business_load.auth_load;

        int action_idx = drl_->select_action(node_embeddings, candidate_indices, vnf_features, context_features);
        if (action_idx < 0 || action_idx >= static_cast<int>(candidates.size())) {
            action_idx = 0;
        }
        vnf_trace.actor_choice_index = action_idx;
        vnf_trace.actor_choice_node = candidates[action_idx];
        if (verbose) {
            std::cout << "│ DRL首选候选: #" << action_idx << " -> " << vnf_trace.actor_choice_node << std::endl;
        }

        std::vector<int> probe_order;
        probe_order.reserve(candidates.size());
        probe_order.push_back(action_idx);
        for (int offset = 1; offset < static_cast<int>(candidates.size()); ++offset) {
            int idx = (action_idx + offset) % static_cast<int>(candidates.size());
            probe_order.push_back(idx);
        }

        const int max_probe = std::min<int>(10, static_cast<int>(probe_order.size()));
        bool deployed = false;
        for (int p = 0; p < max_probe; ++p) {
            const std::string& selected_node = candidates[probe_order[p]];
            CandidateAttemptTrace attempt;
            attempt.node_id = selected_node;
            const Node* selected_node_info = graph.get_node(selected_node);
            attempt.has_resources = selected_node_info &&
                selected_node_info->resources.has_sufficient_resources(
                    vnf.cpu_required, vnf.mem_required, vnf.disk_required_gb);
            if (!attempt.has_resources) {
                attempt.reject_reason = "Insufficient node resources";
                vnf_trace.attempts.push_back(attempt);
                if (verbose) {
                    std::cout << "│   尝试 " << (p + 1) << "/" << max_probe << ": " << selected_node
                              << " -> 资源不足" << std::endl;
                }
                continue;
            }

            auto path = find_shortest_path(graph, prev_node, selected_node, vnf.bandwidth_required_gbps);
            attempt.path_found = !path.empty();
            attempt.path = path;
            if (path.empty()) {
                attempt.reject_reason = "No feasible path satisfying bandwidth";
                vnf_trace.attempts.push_back(attempt);
                if (verbose) {
                    std::cout << "│   尝试 " << (p + 1) << "/" << max_probe << ": " << selected_node
                              << " -> 找不到满足带宽的路径" << std::endl;
                }
                continue;
            }
            attempt.bandwidth_ok = true;

            float path_delay = calculate_path_delay(graph, path);
            float processing_delay = 0.25f + 2.5f * request_business_index;
            float total_delay = path_delay + processing_delay;
            float next_delay = accumulated_delay + total_delay;
            attempt.path_delay_ms = path_delay;
            attempt.processing_delay_ms = processing_delay;
            attempt.projected_total_delay_ms = next_delay;
            attempt.delay_ok = next_delay <= request.max_latency_ms;
            if (next_delay > request.max_latency_ms) {
                attempt.reject_reason = "Projected latency exceeds SLA";
                vnf_trace.attempts.push_back(attempt);
                if (verbose) {
                    std::cout << "│   尝试 " << (p + 1) << "/" << max_probe << ": " << selected_node
                              << " -> 时延超限 (" << next_delay << " ms > " << request.max_latency_ms << " ms)" << std::endl;
                }
                continue;
            }

            float next_reliability =
                accumulated_reliability * selected_node_info->node_reliability * path_reliability(graph, path);
            size_t remaining_steps = request.vnf_sequence.size() - (vnf_idx + 1);
            float optimistic_future_rel = std::pow(0.9985f, static_cast<float>(remaining_steps));
            attempt.projected_reliability = next_reliability;
            attempt.reliability_ok = next_reliability * optimistic_future_rel >= request.reliability_requirement;
            if (next_reliability * optimistic_future_rel < request.reliability_requirement) {
                attempt.reject_reason = "Projected reliability below SLA";
                vnf_trace.attempts.push_back(attempt);
                if (verbose) {
                    std::cout << "│   尝试 " << (p + 1) << "/" << max_probe << ": " << selected_node
                              << " -> 可靠性不足 (" << next_reliability * optimistic_future_rel
                              << " < " << request.reliability_requirement << ")" << std::endl;
                }
                continue;
            }

            accumulated_delay = next_delay;
            accumulated_reliability = next_reliability;

            graph.update_node_resources(selected_node, -vnf.cpu_required, -vnf.mem_required, -vnf.disk_required_gb);
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                graph.update_link_bandwidth(path[i], path[i + 1], -vnf.bandwidth_required_gbps);
            }

            result.deployed_nodes.push_back(selected_node);
            result.paths.push_back(path);
            prev_node = selected_node;
            deployed = true;
            attempt.reject_reason = "accepted";
            vnf_trace.attempts.push_back(attempt);
            vnf_trace.success = true;
            vnf_trace.selected_node = selected_node;
            vnf_trace.selected_path = path;
            vnf_trace.selected_path_delay_ms = path_delay;
            vnf_trace.selected_processing_delay_ms = processing_delay;
            vnf_trace.accumulated_delay_after_ms = accumulated_delay;
            vnf_trace.accumulated_reliability_after = accumulated_reliability;
            if (verbose) {
                std::cout << "│   尝试 " << (p + 1) << "/" << max_probe << ": " << selected_node
                          << " -> 通过" << std::endl;
                std::cout << "│   部署节点: " << selected_node << std::endl;
                std::cout << "│   路径: " << path_to_string(path) << std::endl;
                std::cout << "│   路径时延: " << path_delay << " ms, 处理时延: " << processing_delay
                          << " ms, 累计时延: " << accumulated_delay << " ms" << std::endl;
                std::cout << "│   累计可靠性: " << accumulated_reliability << std::endl;
            }
            break;
        }

        if (!deployed) {
            vnf_trace.failure_reason = "No feasible node/path in fallback probes";
            result.vnf_traces.push_back(vnf_trace);
            result.failure_reason = "No feasible node/path in fallback probes for core NF " + vnf.vnf_id;
            if (verbose) {
                std::cout << "│ ✗ 核心网网元部署失败: " << vnf_trace.failure_reason << std::endl;
            }
            return result;
        }

        result.vnf_traces.push_back(vnf_trace);
    }

    auto final_path = find_shortest_path(graph, prev_node, request.destination_node);
    if (final_path.empty()) {
        result.failure_reason = "No path to destination";
        if (verbose) {
            std::cout << "│ ✗ 收尾失败: 无法从最后一个核心网网元节点到达目的节点" << std::endl;
        }
        return result;
    }

    float final_delay = calculate_path_delay(graph, final_path);
    accumulated_delay += final_delay;
    accumulated_reliability *= path_reliability(graph, final_path);
    if (verbose) {
        std::cout << "│ 终点收尾路径: " << path_to_string(final_path) << std::endl;
        std::cout << "│ 终点收尾时延: " << final_delay << " ms" << std::endl;
    }

    if (accumulated_delay > request.max_latency_ms) {
        result.failure_reason = "Final latency exceeded";
        if (verbose) {
            std::cout << "│ ✗ SFC失败: 最终累计时延 " << accumulated_delay << " ms 超过 SLA "
                      << request.max_latency_ms << " ms" << std::endl;
        }
        return result;
    }

    if (accumulated_reliability < request.reliability_requirement) {
        result.failure_reason = "Final reliability not met";
        if (verbose) {
            std::cout << "│ ✗ SFC失败: 最终可靠性 " << accumulated_reliability << " 低于 SLA "
                      << request.reliability_requirement << std::endl;
        }
        return result;
    }

    result.success = true;
    result.total_delay_ms = accumulated_delay;
    result.final_reliability = accumulated_reliability;
    result.paths.push_back(final_path);
    if (verbose) {
        std::cout << "│ ✓ SFC部署成功" << std::endl;
        std::cout << "│ 最终部署节点序列: " << path_to_string(result.deployed_nodes) << std::endl;
        std::cout << "│ 最终累计时延: " << accumulated_delay << " ms" << std::endl;
        std::cout << "│ 最终累计可靠性: " << accumulated_reliability << std::endl;
    }
    return result;
}

} // namespace sfc
