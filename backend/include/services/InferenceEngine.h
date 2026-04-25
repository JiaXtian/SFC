#pragma once
#include "models/types.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace sfc {

struct CandidateSearchTuning {
    int offline_target_min = 16;
    int offline_target_multiplier = 4;
    int offline_target_cap = 96;
    int realtime_target_min = 4;
    int realtime_target_multiplier = 2;
    int realtime_target_cap = 12;

    int strict_attempt_base = 72;
    int strict_attempt_per_target = 10;
    int relaxed_attempt_base = 56;
    int relaxed_attempt_per_target = 8;
    int extra_attempt_base = 120;
    int extra_attempt_per_target = 16;

    int offline_default_attempt_cap = 520;
    int offline_min_attempt_cap = 260;
    int realtime_default_attempt_cap = 64;
    int realtime_min_attempt_cap = 24;

    double offline_default_time_budget_ms = 5200.0;
    double offline_min_time_budget_ms = 2200.0;
    double realtime_default_time_budget_ms = 850.0;
    double realtime_min_time_budget_ms = 380.0;

    double relax_disable_min_reliability = 0.55;
    double relax_min_reliability_floor = 0.45;
    std::vector<double> reliability_relax_levels = {1.0, 0.98, 0.95, 0.92, 0.88, 0.84, 0.80, 0.74, 0.68, 0.62};

    int trace_attempts_offline = 18;
    int trace_attempts_realtime = 10;

    int offline_return_topk_floor = 6;
    int realtime_return_topk_floor = 1;
};

class InferenceEngine {
public:
    InferenceEngine(const std::string& gnn_model_path,
                   const std::string& actor_model_path,
                   int num_threads = 2);
    ~InferenceEngine();
    
    std::vector<DeploymentCandidate> inference(
        const SFCRequest& request,
        const Topology& topology,
        nlohmann::json* decision_trace = nullptr
    );
    
private:
    Ort::Env env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> gnn_session_;
    std::unique_ptr<Ort::Session> actor_session_;
    Ort::MemoryInfo memory_info_;
    size_t node_feature_dim_ = 8;
    size_t vnf_feature_dim_ = 8;
    size_t context_feature_dim_ = 48;
    size_t node_embedding_dim_ = 192;
    CandidateSearchTuning candidate_tuning_;
    std::string candidate_tuning_config_path_;
    
    std::pair<std::vector<float>, std::vector<int64_t>> prepare_graph_inputs(const Topology& topology);
    std::vector<float> run_gnn_encoder(
        const std::vector<float>& node_features,
        const std::vector<int64_t>& edge_index,
        size_t num_nodes
    );
    std::vector<float> run_actor_policy(
        const std::vector<float>& node_embeddings,
        const std::vector<int64_t>& candidate_indices,
        const std::vector<float>& vnf_features,
        const std::vector<float>& context_features
    );
    
    std::vector<DeploymentCandidate> generate_gha_drl_candidates(
        const SFCRequest& request,
        const Topology& topology,
        nlohmann::json* decision_trace = nullptr
    );
    
    DeploymentCandidate generate_single_deployment(
        const SFCRequest& request,
        const Topology& topology,
        int seed,
        const std::vector<float>& node_embeddings,
        const std::unordered_map<std::string, int64_t>& node_id_to_idx,
        nlohmann::json* candidate_trace = nullptr
    );
    
    std::vector<std::string> filter_candidate_nodes(
        const SFCRequest& request,
        const VNF& vnf,
        const std::string& prev_node,
        size_t current_vnf_idx,
        double remaining_latency,
        double accumulated_reliability,
        int accumulated_hops,
        const std::unordered_set<std::string>& deployed_node_set,
        const Topology& topology
    );
    
    std::vector<std::pair<std::string, double>> rank_nodes_by_cost(
        const std::vector<std::string>& candidates,
        const VNF& vnf,
        const std::string& prev_node,
        const Topology& topology
    );
    
    std::vector<std::string> find_shortest_path(
        const std::string& src,
        const std::string& dst,
        const Topology& topology,
        double required_bandwidth_gbps = 0.0,
        int max_hops = -1,
        bool log_missing = true
    );
    
    std::vector<DeploymentCandidate::LinkDetail> generate_path_links(
        const std::vector<std::string>& path,
        const Topology& topology,
        double bandwidth_required_gbps
    );
    
    const Link* find_link(
        const std::string& src,
        const std::string& dst,
        const Topology& topology
    ) const;

    double estimate_node_reliability(
        const std::string& node_id,
        const Topology& topology
    ) const;

    double estimate_link_reliability(const Link& link) const;
};

} // namespace sfc
