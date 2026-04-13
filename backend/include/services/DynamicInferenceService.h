#pragma once

#include "models/types.h"
#include "services/DynamicSimulationService.h"
#include "services/InferenceEngine.h"
#include "services/ResourceManager.h"
#include "services/TopologyManager.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfc {

class DynamicInferenceService {
public:
    DynamicInferenceService(
        std::shared_ptr<InferenceEngine> inference_engine,
        std::shared_ptr<ResourceManager> res_mgr,
        std::shared_ptr<TopologyManager> topo_mgr,
        std::shared_ptr<DynamicSimulationService> dynamic_sim
    );

    nlohmann::json start_session(
        const SFCRequest& request,
        bool auto_redeploy,
        const DeploymentCandidate* initial_candidate = nullptr,
        const std::string& initial_deployment_id = "",
        double initial_inference_time_ms = -1.0
    );
    bool stop_session(const std::string& session_id);
    nlohmann::json list_sessions() const;
    nlohmann::json get_session_status(const std::string& session_id) const;
    nlohmann::json force_recompute(const std::string& session_id, const std::string& trigger = "manual");

    void on_topology_tick(const TopologySnapshot& snapshot);

private:
    struct SessionState {
        std::string session_id;
        SFCRequest request;
        bool active = true;
        bool auto_redeploy = true;
        int decisions_total = 0;
        int redeploy_total = 0;
        int failures_total = 0;
        int last_topology_version = -1;
        std::string last_sim_time;
        double last_inference_time_ms = 0.0;
        DeploymentCandidate last_candidate;
        bool has_last_candidate = false;
        std::string last_candidate_signature;
        std::string last_required_recompute_signature;
        int last_required_recompute_topology_version = -1;
        nlohmann::json last_decision_trace;
        std::string active_resource_deployment_id;
    };

    static std::string make_session_id();
    static std::string candidate_signature(const DeploymentCandidate& candidate);
    static std::vector<std::string> build_constraint_violations(
        const DeploymentCandidate& candidate,
        const SFCRequest& request
    );
    static double percentile(std::vector<double> values, double p);

    TopologySnapshot build_snapshot_fallback(const Topology& topology) const;
    nlohmann::json evaluate_session(
        SessionState& session,
        const TopologySnapshot& snapshot,
        const std::string& trigger
    );
    std::unordered_set<std::string> collect_down_nodes(const Topology& topology) const;
    std::unordered_map<std::string, std::vector<std::string>> build_active_adjacency(
        const Topology& topology,
        const std::unordered_set<std::string>& down_nodes
    ) const;
    bool has_path_between_nodes(
        const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
        const std::string& src,
        const std::string& dst
    ) const;
    std::string infer_required_recompute_trigger(
        const SessionState& session,
        const TopologySnapshot& snapshot,
        const std::unordered_set<std::string>& down_nodes,
        const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
        std::string* disconnected_from = nullptr,
        std::string* disconnected_to = nullptr
    ) const;
    void trim_latency_window_locked();
    nlohmann::json build_metrics_payload_locked(
        int decisions_this_tick,
        int redeploys_this_tick,
        int failures_this_tick,
        int recovery_attempts_this_tick,
        int recovery_success_this_tick,
        int recovery_failures_this_tick,
        int topology_version,
        const std::string& sim_time
    ) const;

    std::shared_ptr<InferenceEngine> inference_engine_;
    std::shared_ptr<ResourceManager> res_mgr_;
    std::shared_ptr<TopologyManager> topo_mgr_;
    std::shared_ptr<DynamicSimulationService> dynamic_sim_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionState> sessions_;
    std::vector<double> latency_window_ms_;
    uint64_t total_decisions_ = 0;
    uint64_t total_redeploys_ = 0;
    uint64_t total_failures_ = 0;
    uint64_t total_recovery_attempts_ = 0;
    uint64_t total_recovery_success_ = 0;
    uint64_t total_recovery_failures_ = 0;
};

} // namespace sfc
