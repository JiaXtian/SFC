#include "heuristic_pruner.h"
#include <algorithm>
#include <limits>
#include <queue>

namespace sfc {

HeuristicPruner::HeuristicPruner(const HeuristicConfig& config) : config_(config) {}

std::unordered_map<std::string, float> HeuristicPruner::dijkstra_single_source(
    const NetworkGraph& graph,
    const std::string& source,
    bool reverse) {
    std::unordered_map<std::string, float> distances;
    std::priority_queue<std::pair<float, std::string>, std::vector<std::pair<float, std::string>>, std::greater<>> pq;

    for (const auto& node : graph.get_nodes()) {
        distances[node.id] = std::numeric_limits<float>::infinity();
    }
    distances[source] = 0.0f;
    pq.emplace(0.0f, source);

    while (!pq.empty()) {
        auto [current_dist, current_node] = pq.top();
        pq.pop();

        if (current_dist > distances[current_node]) {
            continue;
        }

        const Node* node = graph.get_node(current_node);
        if (!node) {
            continue;
        }

        const auto& adjacent = reverse ? node->predecessors : node->neighbors;
        for (size_t adjacent_idx : adjacent) {
            const auto& next_node = graph.get_node_by_index(adjacent_idx);
            const Link* link = reverse
                ? graph.get_link(next_node.id, current_node)
                : graph.get_link(current_node, next_node.id);
            if (!link || link->link_status == 0) {
                continue;
            }

            float new_dist = current_dist + link->latency_ms;
            if (new_dist < distances[next_node.id]) {
                distances[next_node.id] = new_dist;
                pq.emplace(new_dist, next_node.id);
            }
        }
    }

    return distances;
}

static std::string nf_type_of(const VNFRequirement& vnf) {
    return !vnf.nf_type.empty() ? vnf.nf_type : (!vnf.core_nf_type.empty() ? vnf.core_nf_type : vnf.vnf_type);
}

float HeuristicPruner::compute_score(
    const Node& node,
    const VNFRequirement& vnf,
    float dependency_delay,
    float dependency_hops,
    float dependency_count) const {
    float cpu_util = 1.0f - (node.resources.cpu_total > 1e-6f ? node.resources.cpu_available / node.resources.cpu_total : 0.0f);
    float mem_util = 1.0f - (node.resources.mem_total > 1e-6f ? node.resources.mem_available / node.resources.mem_total : 0.0f);
    float disk_util = 1.0f - (node.resources.disk_total > 1e-6f ? node.resources.disk_available / node.resources.disk_total : 0.0f);

    float resource_score = 0.5f * cpu_util + 0.3f * mem_util + 0.2f * disk_util;
    float business_pressure = std::max({
        node.core_business_load.signaling_load + vnf.business_load_demand.signaling_load,
        node.core_business_load.session_load + vnf.business_load_demand.session_load,
        node.core_business_load.user_plane_load + vnf.business_load_demand.user_plane_load,
        node.core_business_load.mobility_load + vnf.business_load_demand.mobility_load,
        node.core_business_load.policy_load + vnf.business_load_demand.policy_load,
        node.core_business_load.auth_load + vnf.business_load_demand.auth_load,
    });
    float hotspot_penalty = std::max(0.0f, business_pressure - 0.78f) * 5.0f
        + std::max(0, node.deployed_core_nf_count - 2) * 1.5f;
    if (nf_type_of(vnf) == "upf") {
        hotspot_penalty += std::max(0.0f, business_pressure - 0.55f) * 4.0f;
    }
    float latency_score = (dependency_delay + 1.7f * dependency_hops) / 120.0f;
    float dependency_bonus = -0.08f * dependency_count;

    return config_.w_res * resource_score + config_.w_lat * latency_score + hotspot_penalty + dependency_bonus;
}

std::vector<std::string> HeuristicPruner::get_candidate_nodes(
    const NetworkGraph& graph,
    const VNFRequirement& vnf,
    const std::unordered_map<std::string, std::string>& deployed_by_type,
    const std::vector<CoreDependency>& dependencies,
    float remaining_delay) {
    std::vector<std::pair<float, std::string>> scored_candidates;
    const std::string current_type = nf_type_of(vnf);
    struct DependencyDistance {
        CoreDependency dependency;
        std::unordered_map<std::string, float> distances;
    };
    std::vector<DependencyDistance> dependency_distances;

    for (const auto& dep : dependencies) {
        if (dep.source == current_type) {
            auto it = deployed_by_type.find(dep.target);
            if (it == deployed_by_type.end()) continue;
            dependency_distances.push_back({dep, dijkstra_single_source(graph, it->second, true)});
        } else if (dep.target == current_type) {
            auto it = deployed_by_type.find(dep.source);
            if (it == deployed_by_type.end()) continue;
            dependency_distances.push_back({dep, dijkstra_single_source(graph, it->second, false)});
        }
    }

    for (const auto& node : graph.get_nodes()) {
        if (!node.resources.has_sufficient_resources(vnf.cpu_required, vnf.mem_required, vnf.disk_required_gb)) {
            continue;
        }

        float dependency_delay = 0.0f;
        float dependency_hops = 0.0f;
        float dependency_count = 0.0f;
        bool feasible = true;
        for (const auto& dep_dist : dependency_distances) {
            const auto& dep = dep_dist.dependency;
            auto it_dist = dep_dist.distances.find(node.id);
            if (it_dist == dep_dist.distances.end() || it_dist->second == std::numeric_limits<float>::infinity()) {
                feasible = false;
                break;
            }
            dependency_delay += it_dist->second * dep.latency_weight;
            dependency_hops += std::max(1.0f, it_dist->second / 3.0f);
            dependency_count += 1.0f;
        }
        if (!feasible || dependency_delay > remaining_delay * 1.08f) {
            continue;
        }

        float score = compute_score(node, vnf, dependency_delay, dependency_hops, dependency_count);
        scored_candidates.emplace_back(score, node.id);
    }

    std::sort(scored_candidates.begin(), scored_candidates.end());

    std::vector<std::string> result;
    int limit = std::min(config_.top_m, static_cast<int>(scored_candidates.size()));
    for (int i = 0; i < limit; ++i) {
        result.push_back(scored_candidates[i].second);
    }
    return result;
}

} // namespace sfc
