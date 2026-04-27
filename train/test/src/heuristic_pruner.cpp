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

float HeuristicPruner::compute_score(const Node& node, const VNFRequirement&, float dist_from_prev, float dist_to_dest) const {
    float cpu_util = 1.0f - (node.resources.cpu_total > 1e-6f ? node.resources.cpu_available / node.resources.cpu_total : 0.0f);
    float mem_util = 1.0f - (node.resources.mem_total > 1e-6f ? node.resources.mem_available / node.resources.mem_total : 0.0f);
    float disk_util = 1.0f - (node.resources.disk_total > 1e-6f ? node.resources.disk_available / node.resources.disk_total : 0.0f);

    float resource_score = 0.5f * cpu_util + 0.3f * mem_util + 0.2f * disk_util;
    float latency_score = (dist_from_prev + dist_to_dest) / 1000.0f;

    return config_.w_res * resource_score + config_.w_lat * latency_score;
}

std::vector<std::string> HeuristicPruner::get_candidate_nodes(
    const NetworkGraph& graph,
    const VNFRequirement& vnf,
    const std::string& prev_node,
    const std::string& dest_node,
    float remaining_delay) {
    auto dist_from_prev = dijkstra_single_source(graph, prev_node, false);
    auto dist_to_dest = dijkstra_single_source(graph, dest_node, true);

    std::vector<std::pair<float, std::string>> scored_candidates;

    for (const auto& node : graph.get_nodes()) {
        if (!node.resources.has_sufficient_resources(vnf.cpu_required, vnf.mem_required, vnf.disk_required_gb)) {
            continue;
        }

        float d1 = dist_from_prev[node.id];
        float d2 = dist_to_dest[node.id];

        if (d1 == std::numeric_limits<float>::infinity() || d2 == std::numeric_limits<float>::infinity()) {
            continue;
        }
        if (d1 + d2 > remaining_delay) {
            continue;
        }

        float score = compute_score(node, vnf, d1, d2);
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
