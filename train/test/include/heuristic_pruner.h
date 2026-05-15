#pragma once
#include "network_graph.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace sfc {

struct HeuristicConfig {
    float w_res = 0.6f;
    float w_lat = 0.4f;
    float alpha = 0.7f;
    int top_m = 20;
};

class HeuristicPruner {
public:
    explicit HeuristicPruner(const HeuristicConfig& config = HeuristicConfig());
    
    std::vector<std::string> get_candidate_nodes(
        const NetworkGraph& graph,
        const VNFRequirement& vnf,
        const std::unordered_map<std::string, std::string>& deployed_by_type,
        const std::vector<CoreDependency>& dependencies,
        float remaining_delay
    );
    
private:
    HeuristicConfig config_;
    std::unordered_map<std::string, float> dijkstra_single_source(
        const NetworkGraph& graph,
        const std::string& source,
        bool reverse = false
    );
    
    float compute_score(
        const Node& node,
        const VNFRequirement& vnf,
        float dependency_delay,
        float dependency_hops,
        float dependency_count
    ) const;
};

} // namespace sfc
