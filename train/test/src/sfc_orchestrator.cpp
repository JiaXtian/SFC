#include "sfc_orchestrator.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace sfc {

static std::string normalize_nf_type(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '-' || ch == ' ') out.push_back('_');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

static std::string nf_type_of(const VNFRequirement& vnf) {
    return normalize_nf_type(!vnf.nf_type.empty() ? vnf.nf_type : (!vnf.core_nf_type.empty() ? vnf.core_nf_type : vnf.vnf_type));
}

static std::string path_to_string(const std::vector<std::string>& path) {
    if (path.empty()) return "<none>";
    std::ostringstream oss;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0) oss << " -> ";
        oss << path[i];
    }
    return oss.str();
}

static std::vector<float> build_core_nf_features(const VNFRequirement& vnf) {
    static const std::vector<std::string> nf_order = {
        "nrf", "scp", "sepp", "amf", "smf", "upf", "ausf", "udm", "udr", "pcf", "nssf", "bsf"
    };
    const std::string nf_type = nf_type_of(vnf);
    std::vector<float> features(24, 0.0f);
    for (size_t i = 0; i < nf_order.size(); ++i) {
        if (nf_order[i] == nf_type) {
            features[i] = 1.0f;
            break;
        }
    }
    features[12] = vnf.cpu_required / 8.0f;
    features[13] = vnf.mem_required / 16.0f;
    features[14] = vnf.disk_required_gb / 64.0f;
    features[15] = vnf.bandwidth_required_gbps / 4.0f;
    features[16] = nf_type == "upf" ? 1.0f : 0.0f;
    features[17] = vnf.stateful ? 1.0f : 0.0f;
    features[18] = vnf.business_load_demand.signaling_load;
    features[19] = vnf.business_load_demand.session_load;
    features[20] = vnf.business_load_demand.user_plane_load;
    features[21] = vnf.business_load_demand.mobility_load;
    features[22] = vnf.business_load_demand.policy_load;
    features[23] = vnf.business_load_demand.auth_load;
    return features;
}

static float path_reliability(const NetworkGraph& graph, const std::vector<std::string>& path) {
    if (path.size() < 2) return 1.0f;
    float sum_log_rel = 0.0f;
    int hops = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* link = graph.get_link(path[i], path[i + 1]);
        if (!link || link->link_status == 0) return 0.0f;
        sum_log_rel += std::log(std::max(1e-6f, link->link_reliability));
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

SFCOrchestrator::SFCOrchestrator(std::shared_ptr<HeuristicPruner> pruner, std::shared_ptr<DRLInference> drl)
    : pruner_(pruner), drl_(drl) {}

std::vector<std::string> SFCOrchestrator::find_shortest_path(
    const NetworkGraph& graph,
    const std::string& from,
    const std::string& to,
    float required_bw) const {
    if (from == to) return {from};
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
        if (current_node == to) break;
        if (current_dist > distances[current_node]) continue;
        const Node* node = graph.get_node(current_node);
        if (!node) continue;

        for (size_t neighbor_idx : node->neighbors) {
            const auto& neighbor = graph.get_node_by_index(neighbor_idx);
            const Link* link = graph.get_link(current_node, neighbor.id);
            if (!link || link->link_status == 0) continue;
            if (required_bw > 0.0f && link->bandwidth_available_gbps + 1e-6f < required_bw) continue;
            float new_dist = current_dist + link->latency_ms + 2.0f;
            if (new_dist < distances[neighbor.id]) {
                distances[neighbor.id] = new_dist;
                previous[neighbor.id] = current_node;
                pq.emplace(new_dist, neighbor.id);
            }
        }
    }

    if (!distances.count(to) || distances[to] == std::numeric_limits<float>::infinity()) return {};
    std::vector<std::string> path;
    std::string current = to;
    while (current != from) {
        path.push_back(current);
        auto it = previous.find(current);
        if (it == previous.end()) return {};
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
        if (link) total_delay += link->latency_ms;
    }
    return total_delay;
}

bool SFCOrchestrator::check_path_bandwidth(const NetworkGraph& graph, const std::vector<std::string>& path, float required_bw) const {
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* link = graph.get_link(path[i], path[i + 1]);
        if (!link || link->link_status == 0 || link->bandwidth_available_gbps + 1e-6f < required_bw) return false;
    }
    return true;
}

DeploymentResult SFCOrchestrator::deploy_sfc(
    NetworkGraph& graph,
    const SFCRequest& request,
    const std::vector<float>& node_embeddings,
    bool verbose) {
    DeploymentResult result;
    result.request_id = request.request_id;
    result.success = false;

    if (verbose) {
        std::cout << "\n┌---------------------------------------------------------------------┐" << std::endl;
        std::cout << "│ open5gs Core " << request.request_id << " @ " << get_timestamp() << std::endl;
        std::cout << "│ 核心网网元数量: " << request.vnf_sequence.size()
                  << " | 依赖边: " << request.core_dependencies.size()
                  << " | SLA时延: " << request.max_latency_ms << " ms" << std::endl;
        std::cout << "└---------------------------------------------------------------------┘" << std::endl;
    }

    std::unordered_map<std::string, std::string> deployed_by_type;
    float accumulated_delay = 0.0f;
    float min_reliability = 1.0f;
    int satisfied_dependencies = 0;

    for (size_t nf_idx = 0; nf_idx < request.vnf_sequence.size(); ++nf_idx) {
        const auto& vnf = request.vnf_sequence[nf_idx];
        const std::string current_type = nf_type_of(vnf);
        VNFDeploymentTrace trace;
        trace.vnf_id = vnf.core_nf_id.empty() ? vnf.vnf_id : vnf.core_nf_id;
        trace.vnf_type = current_type;
        trace.remaining_delay_before_ms = request.max_latency_ms - accumulated_delay;
        trace.accumulated_delay_before_ms = accumulated_delay;
        trace.accumulated_reliability_before = min_reliability;

        auto candidates = pruner_->get_candidate_nodes(
            graph,
            vnf,
            deployed_by_type,
            request.core_dependencies,
            request.max_latency_ms - accumulated_delay);
        trace.candidate_count = static_cast<int>(candidates.size());
        if (candidates.empty()) {
            trace.failure_reason = "No candidate nodes after dependency-aware pruning";
            result.failure_reason = trace.failure_reason + " for " + current_type;
            result.vnf_traces.push_back(trace);
            return result;
        }

        std::vector<int> candidate_indices;
        for (const auto& candidate : candidates) {
            try {
                candidate_indices.push_back(static_cast<int>(graph.get_node_index(candidate)));
            } catch (...) {
            }
        }
        if (candidate_indices.empty()) {
            trace.failure_reason = "Invalid candidate indices";
            result.failure_reason = trace.failure_reason + " for " + current_type;
            result.vnf_traces.push_back(trace);
            return result;
        }

        std::vector<float> context_features(32, 0.0f);
        context_features[0] = static_cast<float>(nf_idx) / std::max<size_t>(1, request.vnf_sequence.size());
        context_features[1] = static_cast<float>(request.vnf_sequence.size() - nf_idx) / std::max<size_t>(1, request.vnf_sequence.size());
        context_features[2] = static_cast<float>(deployed_by_type.size()) / std::max<size_t>(1, request.vnf_sequence.size());
        context_features[3] = static_cast<float>(satisfied_dependencies) / std::max<size_t>(1, request.core_dependencies.size());
        context_features[4] = accumulated_delay / std::max(1.0f, request.max_latency_ms);
        context_features[7] = min_reliability;
        context_features[16] = request.core_business_load.signaling_load;
        context_features[17] = request.core_business_load.session_load;
        context_features[18] = request.core_business_load.user_plane_load;
        context_features[19] = request.core_business_load.mobility_load;
        context_features[20] = request.core_business_load.policy_load;
        context_features[21] = request.core_business_load.auth_load;
        context_features[25] = current_type == "upf" ? 1.0f : 0.0f;
        context_features[26] = (
            vnf.business_load_demand.signaling_load +
            vnf.business_load_demand.session_load +
            vnf.business_load_demand.user_plane_load +
            vnf.business_load_demand.mobility_load +
            vnf.business_load_demand.policy_load +
            vnf.business_load_demand.auth_load) / 6.0f;
        context_features[27] = (request.max_latency_ms - accumulated_delay) / std::max(1.0f, request.max_latency_ms);
        context_features[28] = request.reliability_requirement;

        int action_idx = drl_->select_action(node_embeddings, candidate_indices, build_core_nf_features(vnf), context_features);
        if (action_idx < 0 || action_idx >= static_cast<int>(candidates.size())) action_idx = 0;
        trace.actor_choice_index = action_idx;
        trace.actor_choice_node = candidates[action_idx];

        std::vector<int> probe_order;
        probe_order.push_back(action_idx);
        for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
            if (i != action_idx) probe_order.push_back(i);
        }

        bool deployed = false;
        const int max_probe = std::min<int>(12, static_cast<int>(probe_order.size()));
        for (int p = 0; p < max_probe; ++p) {
            const std::string selected_node = candidates[probe_order[p]];
            CandidateAttemptTrace attempt;
            attempt.node_id = selected_node;
            const Node* selected_node_info = graph.get_node(selected_node);
            attempt.has_resources = selected_node_info && selected_node_info->resources.has_sufficient_resources(
                vnf.cpu_required, vnf.mem_required, vnf.disk_required_gb);
            if (!attempt.has_resources) {
                attempt.reject_reason = "Insufficient node resources";
                trace.attempts.push_back(attempt);
                continue;
            }

            std::vector<std::pair<std::vector<std::string>, float>> dependency_paths;
            float step_delay = 0.0f;
            float step_reliability = 1.0f;
            bool dependency_ok = true;
            for (const auto& dep : request.core_dependencies) {
                std::string src_node;
                std::string dst_node;
                if (dep.source == current_type) {
                    auto it = deployed_by_type.find(dep.target);
                    if (it == deployed_by_type.end()) continue;
                    src_node = selected_node;
                    dst_node = it->second;
                } else if (dep.target == current_type) {
                    auto it = deployed_by_type.find(dep.source);
                    if (it == deployed_by_type.end()) continue;
                    src_node = it->second;
                    dst_node = selected_node;
                } else {
                    continue;
                }
                auto path = find_shortest_path(graph, src_node, dst_node, dep.bandwidth_required_gbps);
                if (path.empty() || !check_path_bandwidth(graph, path, dep.bandwidth_required_gbps)) {
                    dependency_ok = false;
                    break;
                }
                step_delay += calculate_path_delay(graph, path) * dep.latency_weight;
                step_reliability = std::min(step_reliability, path_reliability(graph, path));
                dependency_paths.emplace_back(path, dep.bandwidth_required_gbps);
            }
            attempt.path_found = dependency_ok;
            attempt.bandwidth_ok = dependency_ok;
            attempt.path_delay_ms = step_delay;
            attempt.projected_total_delay_ms = accumulated_delay + step_delay;
            attempt.projected_reliability = std::min(min_reliability, step_reliability);
            if (!dependency_ok) {
                attempt.reject_reason = "Dependency path infeasible";
                trace.attempts.push_back(attempt);
                continue;
            }
            if (accumulated_delay + step_delay > request.max_latency_ms * 1.08f) {
                attempt.reject_reason = "Projected dependency latency exceeds SLA";
                trace.attempts.push_back(attempt);
                continue;
            }

            graph.update_node_resources(selected_node, -vnf.cpu_required, -vnf.mem_required, -vnf.disk_required_gb);
            graph.update_node_business_load(selected_node, vnf.business_load_demand);
            for (const auto& [path, bw_req] : dependency_paths) {
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    graph.update_link_bandwidth(path[i], path[i + 1], -bw_req);
                }
                result.paths.push_back(path);
            }

            deployed_by_type[current_type] = selected_node;
            result.deployed_nodes.push_back(selected_node);
            accumulated_delay += step_delay;
            min_reliability = std::min(min_reliability, step_reliability);
            satisfied_dependencies += static_cast<int>(dependency_paths.size());
            deployed = true;

            attempt.reject_reason = "accepted";
            trace.success = true;
            trace.selected_node = selected_node;
            trace.selected_path = dependency_paths.empty() ? std::vector<std::string>{selected_node} : dependency_paths.front().first;
            trace.selected_path_delay_ms = step_delay;
            trace.accumulated_delay_after_ms = accumulated_delay;
            trace.accumulated_reliability_after = min_reliability;
            trace.attempts.push_back(attempt);
            if (verbose) {
                std::cout << "│ " << current_type << " -> " << selected_node
                          << " deps=" << dependency_paths.size()
                          << " delay=" << step_delay << "ms" << std::endl;
            }
            break;
        }

        result.vnf_traces.push_back(trace);
        if (!deployed) {
            result.failure_reason = "No feasible node/path for " + current_type;
            return result;
        }
    }

    if (satisfied_dependencies < static_cast<int>(request.core_dependencies.size())) {
        result.failure_reason = "Core dependency graph incomplete";
        return result;
    }
    if (accumulated_delay > request.max_latency_ms) {
        result.failure_reason = "Dependency latency SLA not met";
        return result;
    }
    if (min_reliability < request.reliability_requirement * 0.92f) {
        result.failure_reason = "Dependency reliability SLA not met";
        return result;
    }

    result.success = true;
    result.total_delay_ms = accumulated_delay;
    result.final_reliability = min_reliability;
    if (verbose) {
        std::cout << "│ ✓ open5gs核心网部署成功: delay=" << accumulated_delay
                  << "ms rel=" << min_reliability
                  << " nodes=" << path_to_string(result.deployed_nodes) << std::endl;
    }
    return result;
}

} // namespace sfc
