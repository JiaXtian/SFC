#include "services/DynamicInferenceService.h"
#include "websocket/WSHandler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <spdlog/spdlog.h>

namespace sfc {
namespace {

constexpr size_t kLatencyWindowLimit = 512;

bool better_candidate(const DeploymentCandidate& a, const DeploymentCandidate& b) {
    if (a.satisfies_constraints != b.satisfies_constraints) {
        return a.satisfies_constraints;
    }
    if (std::abs(a.score - b.score) > 1e-9) {
        return a.score > b.score;
    }
    return a.total_latency_ms < b.total_latency_ms;
}

std::vector<DeploymentCandidate::PerVNF> ensure_per_vnf_filled(
    const DeploymentCandidate& candidate,
    const SFCRequest& request
) {
    std::vector<DeploymentCandidate::PerVNF> out = candidate.per_vnf;
    if (out.empty()) {
        const size_t count = std::min(candidate.deployed_nodes.size(), request.vnfs.size());
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            DeploymentCandidate::PerVNF pv;
            pv.vnf = request.vnfs[i].name;
            pv.node = candidate.deployed_nodes[i];
            pv.cpu_used = request.vnfs[i].cpu;
            pv.mem_used = request.vnfs[i].mem;
            pv.disk_used = request.vnfs[i].disk;
            out.push_back(std::move(pv));
        }
        return out;
    }

    const size_t limit = std::min(out.size(), request.vnfs.size());
    for (size_t i = 0; i < limit; ++i) {
        auto& pv = out[i];
        if (pv.vnf.empty()) pv.vnf = request.vnfs[i].name;
        if (pv.node.empty() && i < candidate.deployed_nodes.size()) pv.node = candidate.deployed_nodes[i];
        if (pv.cpu_used <= 0.0) pv.cpu_used = request.vnfs[i].cpu;
        if (pv.mem_used <= 0.0) pv.mem_used = request.vnfs[i].mem;
        if (pv.disk_used <= 0.0) pv.disk_used = request.vnfs[i].disk;
    }
    return out;
}

nlohmann::json request_vnfs_json(const SFCRequest& request) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : request.vnfs) {
        arr.push_back({
            {"name", v.name},
            {"cpu", v.cpu},
            {"mem", v.mem},
            {"disk", v.disk},
            {"bw_in", v.bw_in},
            {"bw_out", v.bw_out}
        });
    }
    return arr;
}

} // namespace

DynamicInferenceService::DynamicInferenceService(
    std::shared_ptr<InferenceEngine> inference_engine,
    std::shared_ptr<ResourceManager> res_mgr,
    std::shared_ptr<TopologyManager> topo_mgr,
    std::shared_ptr<DynamicSimulationService> dynamic_sim
) : inference_engine_(std::move(inference_engine)),
    res_mgr_(std::move(res_mgr)),
    topo_mgr_(std::move(topo_mgr)),
    dynamic_sim_(std::move(dynamic_sim)) {}

std::string DynamicInferenceService::make_session_id() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(1000, 9999);
    return "sess_" + std::to_string(ms) + "_" + std::to_string(dist(rng));
}

std::string DynamicInferenceService::candidate_signature(const DeploymentCandidate& candidate) {
    std::string sig;
    sig.reserve(candidate.deployed_nodes.size() * 16 + candidate.link_details.size() * 24);
    for (const auto& node : candidate.deployed_nodes) {
        sig.append(node);
        sig.push_back('|');
    }
    sig.push_back('#');
    for (const auto& ld : candidate.link_details) {
        sig.append(ld.src);
        sig.append("->");
        sig.append(ld.dst);
        sig.push_back('|');
    }
    return sig;
}

std::vector<std::string> DynamicInferenceService::build_constraint_violations(
    const DeploymentCandidate& candidate,
    const SFCRequest& request
) {
    std::vector<std::string> violations;
    if (candidate.total_latency_ms > request.constraints.max_latency_ms) {
        violations.push_back("latency_exceeded");
    }
    if (candidate.bottleneck_bandwidth_gbps + 1e-9 < request.constraints.min_bandwidth_gbps) {
        violations.push_back("bandwidth_insufficient");
    }
    if (candidate.estimated_reliability + 1e-9 < request.constraints.min_reliability) {
        violations.push_back("reliability_insufficient");
    }
    if (violations.empty() && !candidate.reason.empty()) {
        violations.push_back(candidate.reason);
    }
    return violations;
}

double DynamicInferenceService::percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    if (values.size() == 1) return values.front();
    std::sort(values.begin(), values.end());
    const double pos = std::max(0.0, std::min(1.0, p)) * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = static_cast<size_t>(std::ceil(pos));
    const double alpha = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - alpha) + values[hi] * alpha;
}

TopologySnapshot DynamicInferenceService::build_snapshot_fallback(const Topology& topology) const {
    TopologySnapshot snapshot;
    snapshot.topology = topology;
    snapshot.topology_version = topology.metadata.topology_version;
    snapshot.sim_time = topology.metadata.sim_time;
    snapshot.sampling_interval_sec = topology.metadata.sampling_interval_sec;
    snapshot.metrics.total_nodes = static_cast<int>(topology.nodes.size());
    snapshot.metrics.total_links = static_cast<int>(topology.links.size());
    for (const auto& sat : topology.nodes) {
        if (sat.status == "down") snapshot.metrics.down_nodes += 1;
        else snapshot.metrics.active_nodes += 1;
    }
    for (const auto& link : topology.links) {
        if (link.status == "down") snapshot.metrics.down_links += 1;
        else snapshot.metrics.active_links += 1;
        if (link.status == "congested") snapshot.metrics.congested_links += 1;
    }
    return snapshot;
}

void DynamicInferenceService::trim_latency_window_locked() {
    if (latency_window_ms_.size() <= kLatencyWindowLimit) return;
    const size_t overflow = latency_window_ms_.size() - kLatencyWindowLimit;
    latency_window_ms_.erase(latency_window_ms_.begin(), latency_window_ms_.begin() + static_cast<std::ptrdiff_t>(overflow));
}

nlohmann::json DynamicInferenceService::build_metrics_payload_locked(
    int decisions_this_tick,
    int redeploys_this_tick,
    int failures_this_tick,
    int recovery_attempts_this_tick,
    int recovery_success_this_tick,
    int recovery_failures_this_tick,
    int topology_version,
    const std::string& sim_time
) const {
    const double mean = latency_window_ms_.empty()
        ? 0.0
        : (std::accumulate(latency_window_ms_.begin(), latency_window_ms_.end(), 0.0) /
           static_cast<double>(latency_window_ms_.size()));
    const nlohmann::json payload = {
        {"type", "orchestration_metrics_tick"},
        {"sim_time", sim_time},
        {"topology_version", topology_version},
        {"active_sessions", static_cast<int>(std::count_if(
            sessions_.begin(), sessions_.end(), [](const auto& kv) { return kv.second.active; }
        ))},
        {"decisions_this_tick", decisions_this_tick},
        {"redeploys_this_tick", redeploys_this_tick},
        {"failures_this_tick", failures_this_tick},
        {"recovery_attempts_this_tick", recovery_attempts_this_tick},
        {"recovery_success_this_tick", recovery_success_this_tick},
        {"recovery_failures_this_tick", recovery_failures_this_tick},
        {"total_decisions", total_decisions_},
        {"total_redeploys", total_redeploys_},
        {"total_failures", total_failures_},
        {"total_recovery_attempts", total_recovery_attempts_},
        {"total_recovery_success", total_recovery_success_},
        {"total_recovery_failures", total_recovery_failures_},
        {"recovery_success_rate", total_recovery_attempts_ == 0
            ? 0.0
            : static_cast<double>(total_recovery_success_) / static_cast<double>(total_recovery_attempts_)},
        {"latency_mean_ms", mean},
        {"latency_p50_ms", percentile(latency_window_ms_, 0.50)},
        {"latency_p95_ms", percentile(latency_window_ms_, 0.95)},
        {"latency_p99_ms", percentile(latency_window_ms_, 0.99)}
    };
    return payload;
}

nlohmann::json DynamicInferenceService::evaluate_session(
    SessionState& session,
    const TopologySnapshot& snapshot,
    const std::string& trigger
) {
    Topology topology = snapshot.topology.nodes.empty()
        ? res_mgr_->export_current_topology()
        : snapshot.topology;
    if (topology.nodes.empty()) {
        topology = topo_mgr_->get_current_topology();
    }
    if (topology.nodes.empty()) {
        return {
            {"session_id", session.session_id},
            {"request_id", session.request.request_id},
            {"status", "failed"},
            {"reason", "empty_topology"}
        };
    }

    session.request.topology_version = snapshot.topology_version > 0
        ? snapshot.topology_version
        : topology.metadata.topology_version;
    session.request.sim_time = !snapshot.sim_time.empty()
        ? snapshot.sim_time
        : topology.metadata.sim_time;

    nlohmann::json decision_process;
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto candidates = inference_engine_->inference(session.request, topology, &decision_process);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double inference_ms =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0;

    std::sort(candidates.begin(), candidates.end(), better_candidate);
    std::vector<DeploymentCandidate> response_candidates = candidates;
    if (static_cast<int>(response_candidates.size()) > session.request.topk) {
        response_candidates.resize(static_cast<size_t>(session.request.topk));
    }
    const int deployable_count = static_cast<int>(std::count_if(
        response_candidates.begin(),
        response_candidates.end(),
        [](const DeploymentCandidate& c) { return c.satisfies_constraints; }
    ));

    nlohmann::json trace_candidates = nlohmann::json::array();
    for (const auto& c : response_candidates) {
        const auto filled_per_vnf = ensure_per_vnf_filled(c, session.request);
        nlohmann::json per_vnf = nlohmann::json::array();
        for (const auto& pv : filled_per_vnf) {
            per_vnf.push_back({
                {"vnf", pv.vnf},
                {"node", pv.node},
                {"cpu_used", pv.cpu_used},
                {"mem_used", pv.mem_used},
                {"disk_used", pv.disk_used}
            });
        }
        nlohmann::json link_details = nlohmann::json::array();
        for (const auto& ld : c.link_details) {
            link_details.push_back({
                {"src", ld.src},
                {"dst", ld.dst},
                {"latency_ms", ld.latency_ms},
                {"bandwidth_gbps", ld.bandwidth_gbps},
                {"bandwidth_available_gbps", ld.bandwidth_available_gbps},
                {"bandwidth_required_gbps", ld.bandwidth_required_gbps},
                {"status", ld.status},
                {"reliability", ld.reliability}
            });
        }
        trace_candidates.push_back({
            {"score", c.score},
            {"satisfies_constraints", c.satisfies_constraints},
            {"total_latency_ms", c.total_latency_ms},
            {"estimated_reliability", c.estimated_reliability},
            {"bottleneck_bandwidth_gbps", c.bottleneck_bandwidth_gbps},
            {"deployed_nodes", c.deployed_nodes},
            {"per_vnf", per_vnf},
            {"link_details", link_details},
            {"violation_details", build_constraint_violations(c, session.request)},
            {"reason", c.reason}
        });
    }

    nlohmann::json trace_payload = {
        {"type", "decision_trace"},
        {"mode", "session_continuous"},
        {"trigger", trigger},
        {"session_id", session.session_id},
        {"request_id", session.request.request_id},
        {"topology_version", session.request.topology_version},
        {"sim_time", session.request.sim_time},
        {"source_node", session.request.source_node},
        {"destination_node", session.request.destination_node},
        {"inference_time_ms", inference_ms},
        {"requested_topk", session.request.topk},
        {"returned_topk", static_cast<int>(response_candidates.size())},
        {"deployable_count", deployable_count},
        {"fallback_only", deployable_count == 0},
        {"request_vnfs", request_vnfs_json(session.request)},
        {"candidates", trace_candidates},
        {"decision_process", decision_process}
    };

    session.decisions_total += 1;
    session.last_topology_version = session.request.topology_version;
    session.last_sim_time = session.request.sim_time;
    session.last_inference_time_ms = inference_ms;
    session.last_decision_trace = trace_payload;

    latency_window_ms_.push_back(inference_ms);
    trim_latency_window_locked();
    total_decisions_ += 1;

    if (!response_candidates.empty()) {
        auto chosen = response_candidates.front();
        chosen.per_vnf = ensure_per_vnf_filled(chosen, session.request);
        const std::string sig = candidate_signature(chosen);
        const bool changed = (!session.has_last_candidate) || (sig != session.last_candidate_signature);
        const std::string status = changed ? (session.has_last_candidate ? "redeployed" : "deployed") : "stable";
        if (changed) {
            session.redeploy_total += 1;
            total_redeploys_ += 1;
        }
        session.last_candidate = chosen;
        session.last_candidate_signature = sig;
        session.has_last_candidate = true;

        WSHandler::broadcast_json(trace_payload);
        WSHandler::broadcast_json({
            {"type", "session_update"},
            {"session_id", session.session_id},
            {"request_id", session.request.request_id},
            {"status", status},
            {"topology_version", session.last_topology_version},
            {"sim_time", session.last_sim_time},
            {"path_changed", changed},
            {"reason", chosen.reason}
        });
        return {
            {"session_id", session.session_id},
            {"status", status},
            {"topology_version", session.last_topology_version},
            {"sim_time", session.last_sim_time},
            {"inference_time_ms", inference_ms}
        };
    }

    session.failures_total += 1;
    total_failures_ += 1;
    WSHandler::broadcast_json(trace_payload);
    WSHandler::broadcast_json({
        {"type", "session_update"},
        {"session_id", session.session_id},
        {"request_id", session.request.request_id},
        {"status", "decision_failed"},
        {"topology_version", session.last_topology_version},
        {"sim_time", session.last_sim_time},
        {"path_changed", false},
        {"reason", "no_candidate"}
    });
    return {
        {"session_id", session.session_id},
        {"status", "decision_failed"},
        {"topology_version", session.last_topology_version},
        {"sim_time", session.last_sim_time},
        {"inference_time_ms", inference_ms}
    };
}

nlohmann::json DynamicInferenceService::start_session(
    const SFCRequest& request,
    bool auto_redeploy,
    const DeploymentCandidate* initial_candidate
) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionState session;
    session.session_id = make_session_id();
    session.request = request;
    session.request.realtime_mode = true;
    if (session.request.max_planning_attempts <= 0) {
        session.request.max_planning_attempts = std::max(16, session.request.topk * 6);
    }
    if (session.request.planning_time_budget_ms <= 0.0) {
        session.request.planning_time_budget_ms = 450.0;
    }
    if (session.request.request_id.empty()) {
        session.request.request_id = "req_" + session.session_id;
    }
    session.auto_redeploy = auto_redeploy;
    session.active = true;
    sessions_[session.session_id] = session;

    TopologySnapshot snapshot = dynamic_sim_ ? dynamic_sim_->get_latest_snapshot() : TopologySnapshot{};
    if (snapshot.topology.nodes.empty()) {
        snapshot = build_snapshot_fallback(res_mgr_->export_current_topology());
    }
    if (initial_candidate && (!initial_candidate->deployed_nodes.empty() || !initial_candidate->per_vnf.empty())) {
        DeploymentCandidate chosen = *initial_candidate;
        chosen.per_vnf = ensure_per_vnf_filled(chosen, sessions_[session.session_id].request);
        sessions_[session.session_id].last_candidate = chosen;
        sessions_[session.session_id].last_candidate_signature = candidate_signature(chosen);
        sessions_[session.session_id].has_last_candidate = true;
        sessions_[session.session_id].last_topology_version =
            snapshot.topology_version > 0 ? snapshot.topology_version : snapshot.topology.metadata.topology_version;
        sessions_[session.session_id].last_sim_time = !snapshot.sim_time.empty()
            ? snapshot.sim_time
            : snapshot.topology.metadata.sim_time;
        sessions_[session.session_id].last_inference_time_ms = 0.0;
        sessions_[session.session_id].redeploy_total += 1;
        total_redeploys_ += 1;

        nlohmann::json per_vnf = nlohmann::json::array();
        for (const auto& pv : chosen.per_vnf) {
            per_vnf.push_back({
                {"vnf", pv.vnf},
                {"node", pv.node},
                {"cpu_used", pv.cpu_used},
                {"mem_used", pv.mem_used},
                {"disk_used", pv.disk_used}
            });
        }
        nlohmann::json link_details = nlohmann::json::array();
        for (const auto& ld : chosen.link_details) {
            link_details.push_back({
                {"src", ld.src},
                {"dst", ld.dst},
                {"latency_ms", ld.latency_ms},
                {"bandwidth_gbps", ld.bandwidth_gbps},
                {"bandwidth_available_gbps", ld.bandwidth_available_gbps},
                {"bandwidth_required_gbps", ld.bandwidth_required_gbps},
                {"status", ld.status},
                {"reliability", ld.reliability}
            });
        }
        const auto violations = build_constraint_violations(chosen, sessions_[session.session_id].request);
        nlohmann::json chosen_json = {
            {"score", chosen.score},
            {"satisfies_constraints", chosen.satisfies_constraints},
            {"total_latency_ms", chosen.total_latency_ms},
            {"estimated_reliability", chosen.estimated_reliability},
            {"bottleneck_bandwidth_gbps", chosen.bottleneck_bandwidth_gbps},
            {"deployed_nodes", chosen.deployed_nodes},
            {"per_vnf", per_vnf},
            {"link_details", link_details},
            {"violation_details", violations},
            {"reason", chosen.reason}
        };

        nlohmann::json trace_payload = {
            {"type", "decision_trace"},
            {"mode", "session_continuous"},
            {"trigger", "manual_initial_candidate"},
            {"session_id", session.session_id},
            {"request_id", sessions_[session.session_id].request.request_id},
            {"topology_version", sessions_[session.session_id].last_topology_version},
            {"sim_time", sessions_[session.session_id].last_sim_time},
            {"source_node", sessions_[session.session_id].request.source_node},
            {"destination_node", sessions_[session.session_id].request.destination_node},
            {"inference_time_ms", 0.0},
            {"requested_topk", sessions_[session.session_id].request.topk},
            {"returned_topk", 1},
            {"deployable_count", chosen.satisfies_constraints ? 1 : 0},
            {"fallback_only", !chosen.satisfies_constraints},
            {"request_vnfs", request_vnfs_json(sessions_[session.session_id].request)},
            {"candidates", nlohmann::json::array({chosen_json})},
            {"decision_process", nlohmann::json::object()}
        };

        sessions_[session.session_id].last_decision_trace = trace_payload;
        sessions_[session.session_id].decisions_total += 1;
        total_decisions_ += 1;
        latency_window_ms_.push_back(0.0);
        trim_latency_window_locked();

        WSHandler::broadcast_json(trace_payload);
        WSHandler::broadcast_json({
            {"type", "session_update"},
            {"session_id", session.session_id},
            {"request_id", sessions_[session.session_id].request.request_id},
            {"status", "deployed"},
            {"topology_version", sessions_[session.session_id].last_topology_version},
            {"sim_time", sessions_[session.session_id].last_sim_time},
            {"path_changed", true},
            {"reason", chosen.reason}
        });

        return {
            {"session_id", session.session_id},
            {"active", true},
            {"auto_redeploy", auto_redeploy},
            {"request_id", sessions_[session.session_id].request.request_id},
            {"initial_result", {
                {"session_id", session.session_id},
                {"status", "deployed"},
                {"topology_version", sessions_[session.session_id].last_topology_version},
                {"sim_time", sessions_[session.session_id].last_sim_time},
                {"inference_time_ms", 0.0}
            }}
        };
    }

    auto result = evaluate_session(sessions_[session.session_id], snapshot, "session_start");
    return {
        {"session_id", session.session_id},
        {"active", true},
        {"auto_redeploy", auto_redeploy},
        {"request_id", sessions_[session.session_id].request.request_id},
        {"initial_result", result}
    };
}

bool DynamicInferenceService::stop_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second.active = false;
    return true;
}

nlohmann::json DynamicInferenceService::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& kv : sessions_) {
        const auto& s = kv.second;
        arr.push_back({
            {"session_id", s.session_id},
            {"request_id", s.request.request_id},
            {"active", s.active},
            {"auto_redeploy", s.auto_redeploy},
            {"realtime_mode", s.request.realtime_mode},
            {"max_planning_attempts", s.request.max_planning_attempts},
            {"planning_time_budget_ms", s.request.planning_time_budget_ms},
            {"source_node", s.request.source_node},
            {"destination_node", s.request.destination_node},
            {"last_topology_version", s.last_topology_version},
            {"last_sim_time", s.last_sim_time},
            {"last_inference_time_ms", s.last_inference_time_ms},
            {"decisions_total", s.decisions_total},
            {"redeploy_total", s.redeploy_total},
            {"failures_total", s.failures_total}
        });
    }
    return arr;
}

nlohmann::json DynamicInferenceService::get_session_status(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return {{"found", false}, {"session_id", session_id}};
    }
    const auto& s = it->second;
    return {
        {"found", true},
        {"session_id", s.session_id},
        {"request_id", s.request.request_id},
        {"active", s.active},
        {"auto_redeploy", s.auto_redeploy},
        {"realtime_mode", s.request.realtime_mode},
        {"max_planning_attempts", s.request.max_planning_attempts},
        {"planning_time_budget_ms", s.request.planning_time_budget_ms},
        {"source_node", s.request.source_node},
        {"destination_node", s.request.destination_node},
        {"last_topology_version", s.last_topology_version},
        {"last_sim_time", s.last_sim_time},
        {"last_inference_time_ms", s.last_inference_time_ms},
        {"decisions_total", s.decisions_total},
        {"redeploy_total", s.redeploy_total},
        {"failures_total", s.failures_total},
        {"last_decision_trace", s.last_decision_trace}
    };
}

nlohmann::json DynamicInferenceService::force_recompute(const std::string& session_id, const std::string& trigger) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return {{"ok", false}, {"reason", "session_not_found"}, {"session_id", session_id}};
    }
    TopologySnapshot snapshot = dynamic_sim_ ? dynamic_sim_->get_latest_snapshot() : TopologySnapshot{};
    if (snapshot.topology.nodes.empty()) {
        snapshot = build_snapshot_fallback(res_mgr_->export_current_topology());
    }
    auto res = evaluate_session(it->second, snapshot, trigger);
    return {{"ok", true}, {"session_id", session_id}, {"result", res}};
}

void DynamicInferenceService::on_topology_tick(const TopologySnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    int decisions_this_tick = 0;
    int redeploys_before = static_cast<int>(total_redeploys_);
    int failures_before = static_cast<int>(total_failures_);
    int recovery_attempts_this_tick = 0;
    int recovery_success_this_tick = 0;
    int recovery_failures_this_tick = 0;
    const bool fault_metrics_changed =
        (last_down_nodes_ != snapshot.metrics.down_nodes) ||
        (last_down_links_ != snapshot.metrics.down_links) ||
        (last_congested_links_ != snapshot.metrics.congested_links);

    for (auto& kv : sessions_) {
        auto& session = kv.second;
        if (!session.active) continue;
        const bool needs_recompute =
            !session.has_last_candidate ||
            (fault_metrics_changed && snapshot.topology_version != session.last_topology_version);
        if (!needs_recompute) continue;
        const bool had_prev = session.has_last_candidate;
        const DeploymentCandidate prev_candidate = session.last_candidate;
        const std::string trigger = fault_metrics_changed ? "fault_or_anomaly_tick" : "topology_tick";
        auto result = evaluate_session(session, snapshot, trigger);
        if (fault_metrics_changed) {
            recovery_attempts_this_tick += 1;
            total_recovery_attempts_ += 1;
            const std::string status = result.value("status", "");
            const bool success =
                status == "deployed" || status == "redeployed" || status == "stable";
            if (success) {
                recovery_success_this_tick += 1;
                total_recovery_success_ += 1;
            } else {
                recovery_failures_this_tick += 1;
                total_recovery_failures_ += 1;
            }

            std::string strategy = "degraded_fallback";
            if (success) {
                if (!had_prev) {
                    strategy = "initial_recovery";
                } else {
                    const bool nodes_changed = prev_candidate.deployed_nodes != session.last_candidate.deployed_nodes;
                    const bool path_changed = candidate_signature(prev_candidate) != candidate_signature(session.last_candidate);
                    if (nodes_changed) {
                        strategy = "cross_node_redeploy";
                    } else if (path_changed) {
                        strategy = "local_reroute";
                    } else {
                        strategy = "hold_and_observe";
                    }
                }
            }

            WSHandler::broadcast_json({
                {"type", "recovery_event"},
                {"entity_type", "session"},
                {"entity_id", session.session_id},
                {"request_id", session.request.request_id},
                {"sim_time", snapshot.sim_time},
                {"topology_version", snapshot.topology_version},
                {"strategy", strategy},
                {"result", status},
                {"success", success},
                {"affected_services", 1}
            });
        }
        decisions_this_tick += 1;
    }

    const int redeploys_this_tick = static_cast<int>(total_redeploys_) - redeploys_before;
    const int failures_this_tick = static_cast<int>(total_failures_) - failures_before;
    WSHandler::broadcast_json(
        build_metrics_payload_locked(
            decisions_this_tick,
            redeploys_this_tick,
            failures_this_tick,
            recovery_attempts_this_tick,
            recovery_success_this_tick,
            recovery_failures_this_tick,
            snapshot.topology_version,
            snapshot.sim_time
        )
    );
    last_down_nodes_ = snapshot.metrics.down_nodes;
    last_down_links_ = snapshot.metrics.down_links;
    last_congested_links_ = snapshot.metrics.congested_links;
}

} // namespace sfc
