#pragma once
#include "network_graph.h"
#include "heuristic_pruner.h"
#include "drl_inference.h"
#include <vector>
#include <string>
#include <memory>

namespace sfc {

struct CandidateAttemptTrace {
    std::string node_id;
    bool has_resources = false;
    bool path_found = false;
    bool bandwidth_ok = false;
    bool delay_ok = false;
    bool reliability_ok = false;
    float path_delay_ms = 0.0f;
    float processing_delay_ms = 0.0f;
    float projected_total_delay_ms = 0.0f;
    float projected_reliability = 0.0f;
    std::vector<std::string> path;
    std::string reject_reason;
};

struct VNFDeploymentTrace {
    std::string vnf_id;
    std::string vnf_type;
    float remaining_delay_before_ms = 0.0f;
    float accumulated_delay_before_ms = 0.0f;
    float accumulated_reliability_before = 1.0f;
    int candidate_count = 0;
    int actor_choice_index = -1;
    std::string actor_choice_node;
    std::vector<CandidateAttemptTrace> attempts;
    bool success = false;
    std::string selected_node;
    std::vector<std::string> selected_path;
    float selected_path_delay_ms = 0.0f;
    float selected_processing_delay_ms = 0.0f;
    float accumulated_delay_after_ms = 0.0f;
    float accumulated_reliability_after = 0.0f;
    std::string failure_reason;
};

struct DeploymentResult {
    std::string request_id;
    bool success = false;
    float total_delay_ms = 0.0f;
    std::vector<std::string> deployed_nodes;
    std::vector<std::vector<std::string>> paths;
    std::string failure_reason;
    float final_reliability = 0.0f;
    std::vector<VNFDeploymentTrace> vnf_traces;
};

class SFCOrchestrator {
public:
    SFCOrchestrator(
        std::shared_ptr<HeuristicPruner> pruner,
        std::shared_ptr<DRLInference> drl
    );
    
    DeploymentResult deploy_sfc(
        NetworkGraph& graph,
        const SFCRequest& request,
        const std::vector<float>& node_embeddings,
        bool verbose = true
    );
    
private:
    std::shared_ptr<HeuristicPruner> pruner_;
    std::shared_ptr<DRLInference> drl_;
    
    std::vector<std::string> find_shortest_path(
        const NetworkGraph& graph,
        const std::string& from,
        const std::string& to,
        float required_bw = 0.0f
    ) const;
    
    float calculate_path_delay(
        const NetworkGraph& graph,
        const std::vector<std::string>& path
    ) const;
    
    bool check_path_bandwidth(
        const NetworkGraph& graph,
        const std::vector<std::string>& path,
        float required_bw
    ) const;
};

} // namespace sfc
