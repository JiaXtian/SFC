#include "services/DynamicInferenceService.h"
#include "services/AuthGlobals.h"
#include "services/DeploymentOrchestratorService.h"
#include "websocket/WSHandler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace sfc {
namespace {

constexpr size_t kLatencyWindowLimit = 512;

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool node_is_down(const Satellite& sat) {
    const std::string status = to_lower(sat.status);
    if (status == "down" || status == "fault" || status == "failed" || status == "inactive") {
        return true;
    }
    const std::string fault_tag = to_lower(sat.fault_tag);
    return !fault_tag.empty() && fault_tag != "none";
}

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
            pv.core_nf = request.vnfs[i].name;
            pv.nf_type = request.vnfs[i].nf_type.empty() ? request.vnfs[i].name : request.vnfs[i].nf_type;
            pv.nf_role = request.vnfs[i].nf_role;
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
        if (pv.core_nf.empty()) pv.core_nf = request.vnfs[i].name;
        if (pv.nf_type.empty()) pv.nf_type = request.vnfs[i].nf_type.empty() ? request.vnfs[i].name : request.vnfs[i].nf_type;
        if (pv.nf_role.empty()) pv.nf_role = request.vnfs[i].nf_role;
        if (pv.node.empty() && i < candidate.deployed_nodes.size()) pv.node = candidate.deployed_nodes[i];
        if (pv.cpu_used <= 0.0) pv.cpu_used = request.vnfs[i].cpu;
        if (pv.mem_used <= 0.0) pv.mem_used = request.vnfs[i].mem;
        if (pv.disk_used <= 0.0) pv.disk_used = request.vnfs[i].disk;
    }
    return out;
}

double default_required_bandwidth(const SFCRequest& request) {
    double required = std::max(0.01, request.constraints.min_bandwidth_gbps);
    for (const auto& v : request.vnfs) {
        required = std::max(required, std::max(v.bw_in, v.bw_out));
    }
    return required;
}

void normalize_candidate_bandwidth_requirements(DeploymentCandidate* candidate, const SFCRequest& request) {
    if (!candidate) return;
    const double fallback = default_required_bandwidth(request);
    for (auto& ld : candidate->link_details) {
        if (ld.bandwidth_required_gbps <= 1e-9) {
            ld.bandwidth_required_gbps = fallback;
        }
    }
}

std::string make_session_resource_deployment_id(const std::string& session_id) {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "sess_alloc_" + session_id + "_" + std::to_string(ms);
}

nlohmann::json request_vnfs_json(const SFCRequest& request) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : request.vnfs) {
        arr.push_back({
            {"name", v.name},
            {"core_nf", v.name},
            {"nf_type", v.nf_type.empty() ? v.name : v.nf_type},
            {"nf_role", v.nf_role},
            {"resource_profile", v.resource_profile},
            {"processing_weight", v.processing_weight},
            {"stateful", v.stateful},
            {"cpu", v.cpu},
            {"mem", v.mem},
            {"disk", v.disk},
            {"bw_in", v.bw_in},
            {"bw_out", v.bw_out}
        });
    }
    return arr;
}

nlohmann::json request_dependencies_json(const SFCRequest& request) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& dep : request.core_nf_dependencies) {
        arr.push_back(dep.to_json());
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
    if (candidate.registration_latency_ms > request.constraints.registration_latency_ms) {
        violations.push_back("registration_latency_exceeded");
    }
    if (candidate.pdu_session_latency_ms > request.constraints.pdu_session_latency_ms) {
        violations.push_back("pdu_session_latency_exceeded");
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

std::unordered_set<std::string> DynamicInferenceService::collect_down_nodes(const Topology& topology) const {
    std::unordered_set<std::string> down_nodes;
    for (const auto& sat : topology.nodes) {
        if (node_is_down(sat)) {
            down_nodes.insert(sat.id);
        }
    }
    return down_nodes;
}

std::unordered_map<std::string, std::vector<std::string>> DynamicInferenceService::build_active_adjacency(
    const Topology& topology,
    const std::unordered_set<std::string>& down_nodes
) const {
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& sat : topology.nodes) {
        if (down_nodes.find(sat.id) == down_nodes.end()) {
            adjacency[sat.id] = {};
        }
    }

    for (const auto& link : topology.links) {
        if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
        if (down_nodes.find(link.source) != down_nodes.end() || down_nodes.find(link.target) != down_nodes.end()) {
            continue;
        }
        if (to_lower(link.status) == "down") continue;
        if (link.bandwidth_available_gbps <= 0.0) continue;

        adjacency[link.source].push_back(link.target);
        adjacency[link.target].push_back(link.source);
    }
    return adjacency;
}

bool DynamicInferenceService::has_path_between_nodes(
    const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
    const std::string& src,
    const std::string& dst
) const {
    if (src.empty() || dst.empty()) return false;
    if (src == dst) return true;
    if (adjacency.find(src) == adjacency.end() || adjacency.find(dst) == adjacency.end()) return false;

    std::queue<std::string> q;
    std::unordered_set<std::string> visited;
    visited.insert(src);
    q.push(src);

    while (!q.empty()) {
        const std::string cur = q.front();
        q.pop();
        auto it = adjacency.find(cur);
        if (it == adjacency.end()) continue;
        for (const auto& next : it->second) {
            if (visited.find(next) != visited.end()) continue;
            if (next == dst) return true;
            visited.insert(next);
            q.push(next);
        }
    }
    return false;
}

std::string DynamicInferenceService::infer_required_recompute_trigger(
    const SessionState& session,
    const TopologySnapshot&,
    const std::unordered_set<std::string>& down_nodes,
    const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
    std::string* disconnected_from,
    std::string* disconnected_to
) const {
    if (session.has_last_candidate) {
        for (const auto& node : session.last_candidate.deployed_nodes) {
            if (!node.empty() && down_nodes.find(node) != down_nodes.end()) {
                return "deployment_node_down";
            }
        }
    }

    if (!session.request.core_nf_dependencies.empty()) {
        std::unordered_map<std::string, std::string> node_by_nf_type;
        const auto per_vnf = ensure_per_vnf_filled(session.last_candidate, session.request);
        node_by_nf_type.reserve(per_vnf.size() * 2);
        for (const auto& pv : per_vnf) {
            std::string nf_type = pv.nf_type.empty()
                ? (pv.core_nf.empty() ? pv.vnf : pv.core_nf)
                : pv.nf_type;
            nf_type = to_lower(nf_type);
            if (!nf_type.empty() && !pv.node.empty()) {
                node_by_nf_type[nf_type] = pv.node;
            }
        }
        for (const auto& dep : session.request.core_nf_dependencies) {
            const std::string src_nf = to_lower(dep.source);
            const std::string dst_nf = to_lower(dep.target);
            const auto src_it = node_by_nf_type.find(src_nf);
            const auto dst_it = node_by_nf_type.find(dst_nf);
            if (src_it == node_by_nf_type.end() || dst_it == node_by_nf_type.end()) {
                return "core_dependency_endpoint_missing";
            }
            if (!has_path_between_nodes(adjacency, src_it->second, dst_it->second)) {
                if (disconnected_from) *disconnected_from = src_it->second;
                if (disconnected_to) *disconnected_to = dst_it->second;
                return "anchor_path_disconnected";
            }
        }
        return "";
    }

    const std::string source = session.request.source_node;
    const std::string destination = session.request.destination_node;
    std::vector<std::string> anchors;
    anchors.reserve(session.last_candidate.deployed_nodes.size() + 2);
    if (!source.empty()) anchors.push_back(source);
    for (const auto& node : session.last_candidate.deployed_nodes) {
        if (!node.empty() && (anchors.empty() || anchors.back() != node)) {
            anchors.push_back(node);
        }
    }
    if (!destination.empty() && (anchors.empty() || anchors.back() != destination)) {
        anchors.push_back(destination);
    }
    if (anchors.size() < 2) return "";

    for (size_t i = 0; i + 1 < anchors.size(); ++i) {
        const std::string& a = anchors[i];
        const std::string& b = anchors[i + 1];
        if (!has_path_between_nodes(adjacency, a, b)) {
            if (disconnected_from) *disconnected_from = a;
            if (disconnected_to) *disconnected_to = b;
            return "anchor_path_disconnected";
        }
    }
    return "";
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
    for (auto& c : response_candidates) {
        normalize_candidate_bandwidth_requirements(&c, session.request);
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
                {"core_nf", pv.core_nf.empty() ? pv.vnf : pv.core_nf},
                {"nf_type", pv.nf_type},
                {"nf_role", pv.nf_role},
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
                {"dependency_source_nf", ld.dependency_source_nf},
                {"dependency_target_nf", ld.dependency_target_nf},
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
            {"per_core_nf", per_vnf},
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
        {"request_core_nfs", request_vnfs_json(session.request)},
        {"core_nf_dependencies", request_dependencies_json(session.request)},
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

    const auto down_nodes = collect_down_nodes(topology);
    const auto is_deployable_candidate = [&](const DeploymentCandidate& c) {
        if (!c.satisfies_constraints) return false;
        for (const auto& node : c.deployed_nodes) {
            if (!node.empty() && down_nodes.find(node) != down_nodes.end()) {
                return false;
            }
        }
        return true;
    };

    auto chosen_it = std::find_if(
        response_candidates.begin(),
        response_candidates.end(),
        [&](const DeploymentCandidate& c) { return is_deployable_candidate(c); }
    );
    const bool link_fault_reroute_only = trigger == "anchor_path_disconnected" && session.has_last_candidate;
    if (link_fault_reroute_only) {
        chosen_it = std::find_if(
            response_candidates.begin(),
            response_candidates.end(),
            [&](const DeploymentCandidate& c) {
                return is_deployable_candidate(c) &&
                    c.deployed_nodes == session.last_candidate.deployed_nodes;
            }
        );
        if (chosen_it == response_candidates.end()) {
            // Link-local reroute failed; immediately fallback to cross-node redeploy
            // in the same decision cycle to minimize outage recovery time.
            chosen_it = std::find_if(
                response_candidates.begin(),
                response_candidates.end(),
                [&](const DeploymentCandidate& c) { return is_deployable_candidate(c); }
            );
        }
    }
    if (chosen_it != response_candidates.end()) {
        auto chosen = *chosen_it;
        chosen.per_vnf = ensure_per_vnf_filled(chosen, session.request);
        normalize_candidate_bandwidth_requirements(&chosen, session.request);
        const std::string sig = candidate_signature(chosen);
        const bool changed = (!session.has_last_candidate) || (sig != session.last_candidate_signature);
        const std::string status = changed ? (session.has_last_candidate ? "redeployed" : "deployed") : "stable";

        const bool nodes_changed = (!session.has_last_candidate) ||
            (session.last_candidate.deployed_nodes != chosen.deployed_nodes);
        if (changed) {
            const std::string previous_allocation_id = session.active_resource_deployment_id;
            const bool had_previous_allocation = !previous_allocation_id.empty();
            const DeploymentCandidate previous_candidate = session.last_candidate;
            bool released_previous = true;
            if (had_previous_allocation) {
                released_previous = res_mgr_->release_resources(previous_allocation_id);
                if (!released_previous) {
                    spdlog::warn(
                        "Session {} failed to release previous allocation {} before redeploy",
                        session.session_id,
                        previous_allocation_id
                    );
                }
            }

            const std::string new_allocation_id = make_session_resource_deployment_id(session.session_id);
            const bool allocated = res_mgr_->allocate_resources(new_allocation_id, chosen, session.request.vnfs);
            if (!allocated) {
                bool restored_previous = false;
                if (had_previous_allocation && released_previous && session.has_last_candidate) {
                    DeploymentCandidate restore_candidate = previous_candidate;
                    restore_candidate.per_vnf = ensure_per_vnf_filled(restore_candidate, session.request);
                    normalize_candidate_bandwidth_requirements(&restore_candidate, session.request);
                    restored_previous = res_mgr_->allocate_resources(
                        previous_allocation_id,
                        restore_candidate,
                        session.request.vnfs
                    );
                    if (restored_previous) {
                        session.active_resource_deployment_id = previous_allocation_id;
                    }
                }

                session.failures_total += 1;
                total_failures_ += 1;
                session.pending_replanning = true;
                session.last_replanning_attempt_topology_version = session.last_topology_version;
                const std::string fail_reason = restored_previous
                    ? "resource_allocation_failed_restored_previous"
                    : "resource_allocation_failed";
                spdlog::error(
                    "Session {} resource apply failed on trigger {}: {}",
                    session.session_id,
                    trigger,
                    fail_reason
                );

                WSHandler::broadcast_json(trace_payload);
                WSHandler::broadcast_json({
                    {"type", "session_update"},
                    {"session_id", session.session_id},
                    {"request_id", session.request.request_id},
                    {"status", "decision_failed"},
                    {"trigger", trigger},
                    {"topology_version", session.last_topology_version},
                    {"sim_time", session.last_sim_time},
                    {"path_changed", false},
                    {"reason", fail_reason}
                });
                return {
                    {"session_id", session.session_id},
                    {"status", "decision_failed"},
                    {"topology_version", session.last_topology_version},
                    {"sim_time", session.last_sim_time},
                    {"inference_time_ms", inference_ms},
                    {"reason", fail_reason}
                };
            }

            session.active_resource_deployment_id = new_allocation_id;
            auto updated_topology = res_mgr_->export_current_topology();
            topo_mgr_->save_current_topology(updated_topology);

            if (!session.orchestration_deployment_id.empty() && g_deployment_orchestrator && nodes_changed) {
                g_deployment_orchestrator->enqueue_deployment(
                    session.orchestration_deployment_id,
                    session.request.request_id,
                    chosen,
                    session.request.vnfs,
                    "session_continuous",
                    trigger
                );
            }

            session.redeploy_total += 1;
            total_redeploys_ += 1;
        } else if (
            !session.orchestration_deployment_id.empty() &&
            g_deployment_orchestrator &&
            trigger != "session_start" &&
            trigger != "topology_tick_bootstrap" &&
            trigger != "anchor_path_disconnected"
        ) {
            // Fault-driven recomputation may keep the same placement. We still trigger orchestration
            // so container runtime can perform stop/start recovery on the selected satellites.
            g_deployment_orchestrator->enqueue_deployment(
                session.orchestration_deployment_id,
                session.request.request_id,
                chosen,
                session.request.vnfs,
                "session_continuous",
                trigger
            );
        }

        session.last_candidate = chosen;
        session.last_candidate_signature = sig;
        session.has_last_candidate = true;
        session.pending_replanning = false;
        session.last_replanning_attempt_topology_version = -1;

        WSHandler::broadcast_json(trace_payload);
        WSHandler::broadcast_json({
            {"type", "session_update"},
            {"session_id", session.session_id},
            {"request_id", session.request.request_id},
            {"status", status},
            {"trigger", trigger},
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
    const std::string pending_reason = link_fault_reroute_only
        ? "anchor_reroute_and_cross_node_reschedule_unavailable"
        : (response_candidates.empty() ? "no_candidate" : "no_deployable_candidate");
    session.pending_replanning = true;
    session.last_replanning_attempt_topology_version = session.last_topology_version;
    WSHandler::broadcast_json(trace_payload);
    WSHandler::broadcast_json({
        {"type", "session_update"},
        {"session_id", session.session_id},
        {"request_id", session.request.request_id},
        {"status", "replanning"},
        {"trigger", trigger},
        {"topology_version", session.last_topology_version},
        {"sim_time", session.last_sim_time},
        {"path_changed", false},
        {"reason", pending_reason}
    });
    return {
        {"session_id", session.session_id},
        {"status", "replanning"},
        {"topology_version", session.last_topology_version},
        {"sim_time", session.last_sim_time},
        {"inference_time_ms", inference_ms},
        {"reason", pending_reason}
    };
}

nlohmann::json DynamicInferenceService::start_session(
    const SFCRequest& request,
    bool auto_redeploy,
    const DeploymentCandidate* initial_candidate,
    const std::string& initial_deployment_id,
    double initial_inference_time_ms
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
    sessions_[session.session_id].orchestration_deployment_id = initial_deployment_id;

    TopologySnapshot snapshot = dynamic_sim_ ? dynamic_sim_->get_latest_snapshot() : TopologySnapshot{};
    if (snapshot.topology.nodes.empty()) {
        snapshot = build_snapshot_fallback(res_mgr_->export_current_topology());
    }
    const double seeded_inference_ms =
        (std::isfinite(initial_inference_time_ms) && initial_inference_time_ms > 0.0)
            ? initial_inference_time_ms
            : 0.0;
    if (initial_candidate && (!initial_candidate->deployed_nodes.empty() || !initial_candidate->per_vnf.empty())) {
        DeploymentCandidate chosen = *initial_candidate;
        chosen.per_vnf = ensure_per_vnf_filled(chosen, sessions_[session.session_id].request);
        normalize_candidate_bandwidth_requirements(&chosen, sessions_[session.session_id].request);
        sessions_[session.session_id].last_candidate = chosen;
        sessions_[session.session_id].last_candidate_signature = candidate_signature(chosen);
        sessions_[session.session_id].has_last_candidate = true;
        sessions_[session.session_id].active_resource_deployment_id = initial_deployment_id;
        sessions_[session.session_id].orchestration_deployment_id = initial_deployment_id;
        sessions_[session.session_id].last_topology_version =
            snapshot.topology_version > 0 ? snapshot.topology_version : snapshot.topology.metadata.topology_version;
        sessions_[session.session_id].last_sim_time = !snapshot.sim_time.empty()
            ? snapshot.sim_time
            : snapshot.topology.metadata.sim_time;
        sessions_[session.session_id].last_inference_time_ms = seeded_inference_ms;

        if (sessions_[session.session_id].active_resource_deployment_id.empty()) {
            const std::string alloc_id = make_session_resource_deployment_id(session.session_id);
            const bool allocated = res_mgr_->allocate_resources(
                alloc_id,
                chosen,
                sessions_[session.session_id].request.vnfs
            );
            if (!allocated) {
                sessions_[session.session_id].active = false;
                sessions_[session.session_id].failures_total += 1;
                total_failures_ += 1;
                return {
                    {"session_id", session.session_id},
                    {"active", false},
                    {"auto_redeploy", auto_redeploy},
                    {"request_id", sessions_[session.session_id].request.request_id},
                    {"initial_result", {
                        {"session_id", session.session_id},
                        {"status", "decision_failed"},
                        {"topology_version", sessions_[session.session_id].last_topology_version},
                        {"sim_time", sessions_[session.session_id].last_sim_time},
                        {"inference_time_ms", seeded_inference_ms},
                        {"reason", "resource_allocation_failed"}
                    }}
                };
            }
            sessions_[session.session_id].active_resource_deployment_id = alloc_id;
            auto updated_topology = res_mgr_->export_current_topology();
            topo_mgr_->save_current_topology(updated_topology);
        }

        sessions_[session.session_id].redeploy_total += 1;
        total_redeploys_ += 1;

        nlohmann::json per_vnf = nlohmann::json::array();
        for (const auto& pv : chosen.per_vnf) {
            per_vnf.push_back({
                {"vnf", pv.vnf},
                {"core_nf", pv.core_nf.empty() ? pv.vnf : pv.core_nf},
                {"nf_type", pv.nf_type},
                {"nf_role", pv.nf_role},
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
                {"dependency_source_nf", ld.dependency_source_nf},
                {"dependency_target_nf", ld.dependency_target_nf},
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
            {"per_core_nf", per_vnf},
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
            {"inference_time_ms", seeded_inference_ms},
            {"requested_topk", sessions_[session.session_id].request.topk},
            {"returned_topk", 1},
            {"deployable_count", chosen.satisfies_constraints ? 1 : 0},
            {"fallback_only", !chosen.satisfies_constraints},
            {"request_vnfs", request_vnfs_json(sessions_[session.session_id].request)},
            {"request_core_nfs", request_vnfs_json(sessions_[session.session_id].request)},
            {"core_nf_dependencies", request_dependencies_json(sessions_[session.session_id].request)},
            {"candidates", nlohmann::json::array({chosen_json})},
            {"decision_process", nlohmann::json::object()}
        };

        sessions_[session.session_id].last_decision_trace = trace_payload;
        sessions_[session.session_id].decisions_total += 1;
        total_decisions_ += 1;
        if (seeded_inference_ms > 0.0) {
            latency_window_ms_.push_back(seeded_inference_ms);
            trim_latency_window_locked();
        }

        WSHandler::broadcast_json(trace_payload);
        WSHandler::broadcast_json({
            {"type", "session_update"},
            {"session_id", session.session_id},
            {"request_id", sessions_[session.session_id].request.request_id},
            {"status", "deployed"},
            {"trigger", "manual_initial_candidate"},
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
                {"inference_time_ms", seeded_inference_ms}
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
    if (!it->second.active_resource_deployment_id.empty()) {
        const bool released = res_mgr_->release_resources(it->second.active_resource_deployment_id);
        if (!released) {
            spdlog::debug(
                "Session {} failed to release allocation {} on stop",
                session_id,
                it->second.active_resource_deployment_id
            );
        } else {
            auto updated_topology = res_mgr_->export_current_topology();
            topo_mgr_->save_current_topology(updated_topology);
        }
        it->second.active_resource_deployment_id.clear();
    }
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

    Topology topology = snapshot.topology;
    if (topology.nodes.empty()) {
        topology = res_mgr_->export_current_topology();
    }
    if (topology.nodes.empty()) {
        topology = topo_mgr_->get_current_topology();
    }
    const auto down_nodes = collect_down_nodes(topology);
    const auto adjacency = build_active_adjacency(topology, down_nodes);

    for (auto& kv : sessions_) {
        auto& session = kv.second;
        if (!session.active) continue;
        if (!session.auto_redeploy) continue;

        std::string trigger;
        std::string disconnected_from;
        std::string disconnected_to;
        if (!session.has_last_candidate) {
            trigger = "topology_tick_bootstrap";
        } else {
            trigger = infer_required_recompute_trigger(
                session,
                snapshot,
                down_nodes,
                adjacency,
                &disconnected_from,
                &disconnected_to
            );
        }
        if (trigger.empty()) {
            if (session.pending_replanning) {
                if (session.last_replanning_attempt_topology_version == snapshot.topology_version) {
                    continue;
                }
                auto result = evaluate_session(session, snapshot, "recovery_resume");
                session.last_replanning_attempt_topology_version = snapshot.topology_version;
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
                WSHandler::broadcast_json({
                    {"type", "recovery_event"},
                    {"entity_type", "session"},
                    {"entity_id", session.session_id},
                    {"request_id", session.request.request_id},
                    {"sim_time", snapshot.sim_time},
                    {"topology_version", snapshot.topology_version},
                    {"strategy", success ? "resume_replanning" : "continue_replanning"},
                    {"trigger", "recovery_resume"},
                    {"disconnected_from", ""},
                    {"disconnected_to", ""},
                    {"result", status},
                    {"success", success},
                    {"affected_services", 1}
                });
                decisions_this_tick += 1;
                continue;
            }
            session.last_required_recompute_signature.clear();
            session.last_required_recompute_topology_version = -1;
            continue;
        }

        const bool fault_driven = trigger != "topology_tick_bootstrap";
        const std::string reason_signature = trigger + "|" + disconnected_from + "->" + disconnected_to;
        const bool repeated_same_reason = fault_driven && reason_signature == session.last_required_recompute_signature;
        if (repeated_same_reason &&
            session.last_required_recompute_topology_version == snapshot.topology_version) {
            continue;
        }
        const bool had_prev = session.has_last_candidate;
        const DeploymentCandidate prev_candidate = session.last_candidate;
        auto result = evaluate_session(session, snapshot, trigger);
        if (fault_driven) {
            session.last_required_recompute_signature = reason_signature;
            session.last_required_recompute_topology_version = snapshot.topology_version;
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
                {"trigger", trigger},
                {"disconnected_from", disconnected_from},
                {"disconnected_to", disconnected_to},
                {"result", status},
                {"success", success},
                {"affected_services", 1}
            });
        } else {
            session.last_required_recompute_signature.clear();
            session.last_required_recompute_topology_version = -1;
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
}

} // namespace sfc
