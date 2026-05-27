#include "services/DynamicInferenceService.h"
#include "services/AuthGlobals.h"
#include "services/DeploymentOrchestratorService.h"
#include "services/DeploymentStateStore.h"
#include "websocket/WSHandler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
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

std::string canonical_link_key(const std::string& source, const std::string& target) {
    if (source <= target) return source + "|" + target;
    return target + "|" + source;
}

bool link_is_down(const Link& link) {
    const std::string status = to_lower(link.status);
    if (status == "down" || status == "fault" || status == "failed" || status == "inactive") {
        return true;
    }
    const std::string fault_tag = to_lower(link.fault_tag);
    return !fault_tag.empty() && fault_tag != "none";
}

bool deployment_runtime_enabled_for_inference(const std::string& deployment_id) {
    if (deployment_id.empty()) return false;
    const nlohmann::json records = list_deployment_records();
    if (!records.is_array()) return true;
    for (const auto& dep : records) {
        if (!dep.is_object()) continue;
        const std::string dep_id = dep.value("deployment_id", std::string(""));
        const std::string backend_id = dep.value("backend_deployment_id", dep_id);
        if (dep_id == deployment_id || backend_id == deployment_id) {
            return dep.value("runtime_enabled", false);
        }
    }
    return true;
}

std::pair<std::string, int> down_link_signature(const Topology& topology) {
    std::vector<std::string> keys;
    keys.reserve(topology.links.size());
    for (const auto& link : topology.links) {
        if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
        if (!link_is_down(link)) continue;
        keys.push_back(canonical_link_key(link.source, link.target));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::string sig;
    for (const auto& key : keys) {
        if (!sig.empty()) sig.push_back(',');
        sig += key;
    }
    return {sig, static_cast<int>(keys.size())};
}

std::string candidate_placement_signature(const DeploymentCandidate& candidate) {
    std::vector<std::string> parts;
    parts.reserve(candidate.per_vnf.empty() ? candidate.deployed_nodes.size() : candidate.per_vnf.size());
    if (!candidate.per_vnf.empty()) {
        for (const auto& pv : candidate.per_vnf) {
            std::string nf = pv.nf_type.empty() ? (pv.core_nf.empty() ? pv.vnf : pv.core_nf) : pv.nf_type;
            std::transform(nf.begin(), nf.end(), nf.begin(), [](unsigned char ch) {
                if (ch == '-' || ch == ' ') return '_';
                return static_cast<char>(std::tolower(ch));
            });
            parts.push_back(nf + "@" + pv.node);
        }
    } else {
        for (const auto& node : candidate.deployed_nodes) {
            parts.push_back("node@" + node);
        }
    }
    std::sort(parts.begin(), parts.end());
    std::ostringstream oss;
    for (const auto& part : parts) {
        oss << part << "|";
    }
    return oss.str();
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

struct RepairPathMetrics {
    double latency_ms = 0.0;
    double reliability = 1.0;
    double bottleneck_bandwidth_gbps = std::numeric_limits<double>::infinity();
    int hops = 0;
    bool feasible = true;
};

double soften_repair_reliability(double raw_reliability, int hops) {
    if (hops <= 0) return 1.0;
    const double raw = std::max(1e-9, std::min(1.0, raw_reliability));
    const double geometric_mean = std::pow(raw, 1.0 / static_cast<double>(std::max(1, hops)));
    const double softened_product = std::pow(raw, 0.28);
    const int excess_hops = std::max(0, hops - 6);
    return std::max(0.0, std::min(1.0, (0.78 * softened_product + 0.22 * geometric_mean) * std::pow(0.9993, excess_hops)));
}

RepairPathMetrics evaluate_repair_links(
    const std::vector<DeploymentCandidate::LinkDetail>& links,
    double required_bw
) {
    RepairPathMetrics metrics;
    for (const auto& ld : links) {
        metrics.latency_ms += ld.latency_ms;
        metrics.reliability *= std::max(1e-9, ld.reliability);
        metrics.bottleneck_bandwidth_gbps = std::min(metrics.bottleneck_bandwidth_gbps, ld.bandwidth_available_gbps);
        metrics.hops += 1;
        if (to_lower(ld.status) == "down" || ld.bandwidth_available_gbps + 1e-9 < required_bw) {
            metrics.feasible = false;
            return metrics;
        }
    }
    if (links.empty()) {
        metrics.bottleneck_bandwidth_gbps = std::numeric_limits<double>::infinity();
        metrics.reliability = 1.0;
    } else {
        metrics.reliability = soften_repair_reliability(metrics.reliability, metrics.hops);
    }
    return metrics;
}

std::string nf_key_from_pv(const DeploymentCandidate::PerVNF& pv) {
    return to_lower(pv.nf_type.empty() ? (pv.core_nf.empty() ? pv.vnf : pv.core_nf) : pv.nf_type);
}

double coordinate_distance_km(const Coordinates& a, const Coordinates& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::unordered_set<std::string> collect_repair_neighborhood(
    const Topology& topology,
    const std::unordered_set<std::string>& anchors,
    const std::unordered_set<std::string>& down_nodes,
    int max_hops
) {
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& sat : topology.nodes) {
        if (down_nodes.find(sat.id) == down_nodes.end() && !node_is_down(sat)) {
            adjacency[sat.id] = {};
        }
    }
    for (const auto& link : topology.links) {
        if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
        if (down_nodes.find(link.source) != down_nodes.end() || down_nodes.find(link.target) != down_nodes.end()) continue;
        if (to_lower(link.status) == "down") continue;
        if (link.bandwidth_available_gbps <= 0.0) continue;
        if (adjacency.find(link.source) == adjacency.end() || adjacency.find(link.target) == adjacency.end()) continue;
        adjacency[link.source].push_back(link.target);
        adjacency[link.target].push_back(link.source);
    }

    std::queue<std::pair<std::string, int>> q;
    std::unordered_set<std::string> visited;
    for (const auto& anchor : anchors) {
        if (adjacency.find(anchor) == adjacency.end()) continue;
        if (visited.insert(anchor).second) q.push({anchor, 0});
    }

    while (!q.empty()) {
        const auto [node, depth] = q.front();
        q.pop();
        if (depth >= max_hops) continue;
        const auto it = adjacency.find(node);
        if (it == adjacency.end()) continue;
        for (const auto& next : it->second) {
            if (visited.find(next) != visited.end()) continue;
            visited.insert(next);
            q.push({next, depth + 1});
        }
    }
    return visited;
}

std::vector<std::string> repair_shortest_path(
    const std::string& src,
    const std::string& dst,
    const Topology& topology,
    double required_bw
) {
    if (src.empty() || dst.empty()) return {};
    if (src == dst) return {src};

    std::unordered_set<std::string> down_nodes;
    for (const auto& sat : topology.nodes) {
        if (node_is_down(sat)) down_nodes.insert(sat.id);
    }
    if (down_nodes.find(src) != down_nodes.end() || down_nodes.find(dst) != down_nodes.end()) return {};

    struct Edge { std::string to; double weight = 0.0; };
    std::unordered_map<std::string, std::vector<Edge>> adj;
    for (const auto& sat : topology.nodes) {
        if (down_nodes.find(sat.id) == down_nodes.end()) adj[sat.id] = {};
    }
    for (const auto& link : topology.links) {
        if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
        if (down_nodes.find(link.source) != down_nodes.end() || down_nodes.find(link.target) != down_nodes.end()) continue;
        if (to_lower(link.status) == "down") continue;
        if (link.bandwidth_available_gbps + 1e-9 < required_bw) continue;
        const double congestion_penalty = to_lower(link.status) == "congested" ? 1.35 : 1.0;
        const double weight = std::max(0.001, link.latency_ms) * congestion_penalty;
        adj[link.source].push_back({link.target, weight});
        adj[link.target].push_back({link.source, weight});
    }
    if (adj.find(src) == adj.end() || adj.find(dst) == adj.end()) return {};

    using Item = std::pair<double, std::string>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    std::unordered_map<std::string, double> dist;
    std::unordered_map<std::string, std::string> prev;
    dist[src] = 0.0;
    pq.push({0.0, src});
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u] + 1e-9) continue;
        if (u == dst) break;
        for (const auto& e : adj[u]) {
            const double nd = d + e.weight;
            auto it = dist.find(e.to);
            if (it == dist.end() || nd < it->second) {
                dist[e.to] = nd;
                prev[e.to] = u;
                pq.push({nd, e.to});
            }
        }
    }
    if (dist.find(dst) == dist.end()) return {};

    std::vector<std::string> path;
    for (std::string cur = dst; !cur.empty();) {
        path.push_back(cur);
        if (cur == src) break;
        auto it = prev.find(cur);
        if (it == prev.end()) return {};
        cur = it->second;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<DeploymentCandidate::LinkDetail> repair_path_links(
    const std::vector<std::string>& path,
    const Topology& topology,
    double required_bw
) {
    std::vector<DeploymentCandidate::LinkDetail> out;
    if (path.size() < 2) return out;
    out.reserve(path.size() - 1);
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const std::string& a = path[i];
        const std::string& b = path[i + 1];
        const Link* found = nullptr;
        for (const auto& link : topology.links) {
            const bool same = (link.source == a && link.target == b) || (link.source == b && link.target == a);
            if (same) {
                found = &link;
                break;
            }
        }
        if (!found) return {};
        DeploymentCandidate::LinkDetail ld;
        ld.src = a;
        ld.dst = b;
        ld.latency_ms = found->latency_ms;
        ld.bandwidth_gbps = found->bandwidth_gbps;
        ld.bandwidth_available_gbps = found->bandwidth_available_gbps;
        ld.bandwidth_required_gbps = required_bw;
        ld.status = found->status;
        ld.reliability = found->reliability;
        out.push_back(std::move(ld));
    }
    return out;
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

Topology DynamicInferenceService::build_planning_topology_for_session(
    const SessionState& session,
    const Topology& topology,
    const std::unordered_set<std::string>& down_nodes
) const {
    Topology planning = topology;
    if (!session.has_last_candidate) return planning;

    const auto per_vnf = ensure_per_vnf_filled(session.last_candidate, session.request);
    for (size_t i = 0; i < per_vnf.size() && i < session.request.vnfs.size(); ++i) {
        const auto& pv = per_vnf[i];
        if (pv.node.empty()) continue;
        for (auto& sat : planning.nodes) {
            if (sat.id != pv.node) continue;
            if (down_nodes.find(sat.id) != down_nodes.end() || node_is_down(sat)) break;
            sat.cpu_available = std::min(sat.cpu_total, sat.cpu_available + session.request.vnfs[i].cpu);
            sat.mem_available = std::min(sat.mem_total, sat.mem_available + session.request.vnfs[i].mem);
            sat.disk_available = std::min(sat.disk_total, sat.disk_available + session.request.vnfs[i].disk);
            break;
        }
    }

    for (const auto& ld : session.last_candidate.link_details) {
        const double bw = ld.bandwidth_required_gbps > 1e-9
            ? ld.bandwidth_required_gbps
            : default_required_bandwidth(session.request);
        for (auto& link : planning.links) {
            const bool same =
                (link.source == ld.src && link.target == ld.dst) ||
                (link.source == ld.dst && link.target == ld.src);
            if (!same) continue;
            if (to_lower(link.status) == "down") break;
            link.bandwidth_available_gbps = std::min(link.bandwidth_gbps, link.bandwidth_available_gbps + bw);
            if (link.bandwidth_available_gbps > link.bandwidth_gbps * 0.15 && to_lower(link.status) == "congested") {
                link.status = "active";
            }
            break;
        }
    }
    return planning;
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

bool DynamicInferenceService::rebuild_candidate_paths_and_sla(
    DeploymentCandidate* candidate,
    const SFCRequest& request,
    const Topology& topology,
    std::string* reason
) {
    if (!candidate) return false;
    candidate->per_vnf = ensure_per_vnf_filled(*candidate, request);
    std::unordered_map<std::string, std::string> node_by_nf;
    for (const auto& pv : candidate->per_vnf) {
        const std::string nf = nf_key_from_pv(pv);
        if (!nf.empty() && !pv.node.empty()) node_by_nf[nf] = pv.node;
    }

    std::vector<DeploymentCandidate::LinkDetail> dependency_links;
    double dependency_latency = 0.0;
    double dependency_reliability = 1.0;
    double dependency_bottleneck = std::numeric_limits<double>::infinity();

    auto pair_metrics = [&](const std::string& src_nf_raw, const std::string& dst_nf_raw, double required_bw,
                            std::vector<DeploymentCandidate::LinkDetail>* out_links) -> std::optional<RepairPathMetrics> {
        const std::string src_nf = to_lower(src_nf_raw);
        const std::string dst_nf = to_lower(dst_nf_raw);
        const auto src_it = node_by_nf.find(src_nf);
        const auto dst_it = node_by_nf.find(dst_nf);
        if (src_it == node_by_nf.end() || dst_it == node_by_nf.end()) return std::nullopt;
        std::vector<std::string> path = {src_it->second};
        if (src_it->second != dst_it->second) {
            path = repair_shortest_path(src_it->second, dst_it->second, topology, required_bw);
            if (path.empty() || path.size() < 2) {
                path = repair_shortest_path(src_it->second, dst_it->second, topology, 0.0);
            }
            if (path.empty() || path.size() < 2) return std::nullopt;
        }
        auto links = repair_path_links(path, topology, required_bw);
        const auto metrics = evaluate_repair_links(links, required_bw);
        if (!metrics.feasible) return std::nullopt;
        if (out_links) {
            for (auto& ld : links) {
                ld.dependency_source_nf = src_nf;
                ld.dependency_target_nf = dst_nf;
                out_links->push_back(std::move(ld));
            }
        }
        return metrics;
    };

    for (const auto& dep : request.core_nf_dependencies) {
        const double required_bw = std::max(0.01, dep.bandwidth_required_gbps);
        std::vector<DeploymentCandidate::LinkDetail> dep_links;
        auto metrics = pair_metrics(dep.source, dep.target, required_bw, &dep_links);
        if (!metrics.has_value()) {
            if (reason) *reason = "partial_redeploy_dependency_path_unavailable:" + dep.source + "->" + dep.target;
            return false;
        }
        dependency_latency += metrics->latency_ms * std::max(0.1, dep.latency_weight);
        dependency_reliability = std::min(
            dependency_reliability,
            std::pow(std::max(1e-9, metrics->reliability), std::max(0.1, dep.reliability_weight))
        );
        dependency_bottleneck = std::min(dependency_bottleneck, metrics->bottleneck_bandwidth_gbps);
        dependency_links.insert(dependency_links.end(), dep_links.begin(), dep_links.end());
    }

    auto flow_latency = [&](const std::vector<std::pair<std::string, std::string>>& hops, double fixed_ms) -> std::optional<double> {
        double total = fixed_ms;
        for (const auto& hop : hops) {
            auto metrics = pair_metrics(hop.first, hop.second, std::max(0.01, request.constraints.min_bandwidth_gbps), nullptr);
            if (!metrics.has_value()) return std::nullopt;
            total += metrics->latency_ms;
        }
        return total;
    };

    const auto reg_latency = flow_latency({{"amf", "ausf"}, {"ausf", "udm"}}, request.constraints.registration_access_latency_ms);
    if (!reg_latency.has_value()) {
        if (reason) *reason = "partial_redeploy_registration_sla_path_unavailable";
        return false;
    }
    const auto pdu_latency = flow_latency({{"amf", "smf"}, {"smf", "upf"}}, request.constraints.pdu_access_latency_ms);
    if (!pdu_latency.has_value()) {
        if (reason) *reason = "partial_redeploy_pdu_sla_path_unavailable";
        return false;
    }

    std::vector<std::string> dedup_nodes;
    std::unordered_set<std::string> seen;
    for (const auto& pv : candidate->per_vnf) {
        if (!pv.node.empty() && seen.insert(pv.node).second) dedup_nodes.push_back(pv.node);
    }

    candidate->deployed_nodes = std::move(dedup_nodes);
    candidate->link_details = std::move(dependency_links);
    candidate->total_latency_ms = dependency_latency;
    candidate->registration_latency_ms = *reg_latency;
    candidate->pdu_session_latency_ms = *pdu_latency;
    candidate->estimated_reliability = dependency_reliability;
    candidate->bottleneck_bandwidth_gbps =
        std::isfinite(dependency_bottleneck) ? dependency_bottleneck : request.constraints.min_bandwidth_gbps;
    const double rel_target = std::max(0.45, request.constraints.min_reliability * 0.92);
    candidate->satisfies_constraints =
        candidate->total_latency_ms <= request.constraints.max_latency_ms &&
        candidate->registration_latency_ms <= request.constraints.registration_latency_ms &&
        candidate->pdu_session_latency_ms <= request.constraints.pdu_session_latency_ms &&
        candidate->bottleneck_bandwidth_gbps + 1e-9 >= request.constraints.min_bandwidth_gbps &&
        candidate->estimated_reliability + 1e-9 >= rel_target;
    if (!candidate->satisfies_constraints) {
        if (reason) *reason = "partial_redeploy_sla_violation";
        candidate->reason = reason ? *reason : "partial_redeploy_sla_violation";
        return false;
    }
    candidate->reason.clear();
    candidate->score = std::max(0.0, std::min(1.0, 1.0 - candidate->total_latency_ms / std::max(1.0, request.constraints.max_latency_ms) * 0.35));
    return true;
}

std::optional<DeploymentCandidate> DynamicInferenceService::try_partial_node_redeploy(
    const SessionState& session,
    const Topology& planning_topology,
    const std::unordered_set<std::string>& down_nodes,
    std::string* detail
) {
    if (!session.has_last_candidate) return std::nullopt;
    auto per_vnf = ensure_per_vnf_filled(session.last_candidate, session.request);
    std::vector<size_t> affected;
    std::unordered_set<std::string> affected_nodes;
    for (size_t i = 0; i < per_vnf.size(); ++i) {
        if (!per_vnf[i].node.empty() && down_nodes.find(per_vnf[i].node) != down_nodes.end()) {
            affected.push_back(i);
            affected_nodes.insert(per_vnf[i].node);
        }
    }
    if (affected.empty()) {
        if (detail) *detail = "no_affected_core_nf";
        return std::nullopt;
    }
    if (down_nodes.size() >= 2) {
        if (detail) {
            *detail = "escalate_full_redeploy_multi_node_fault:down_nodes=" + std::to_string(down_nodes.size()) +
                ",affected_nodes=" + std::to_string(affected_nodes.size()) +
                ",affected_nfs=" + std::to_string(affected.size());
        }
        return std::nullopt;
    }
    if (affected_nodes.size() >= 2) {
        if (detail) {
            *detail = "escalate_full_redeploy_multi_node_fault:affected_nodes=" + std::to_string(affected_nodes.size()) +
                ",affected_nfs=" + std::to_string(affected.size());
        }
        return std::nullopt;
    }
    auto dependency_degree = [&](size_t idx) {
        if (idx >= per_vnf.size()) return 0;
        const std::string nf = nf_key_from_pv(per_vnf[idx]);
        int degree = 0;
        for (const auto& dep : session.request.core_nf_dependencies) {
            if (to_lower(dep.source) == nf || to_lower(dep.target) == nf) degree += 1;
        }
        return degree;
    };
    std::sort(affected.begin(), affected.end(), [&](size_t a, size_t b) {
        return dependency_degree(a) > dependency_degree(b);
    });

    std::unordered_map<std::string, const Satellite*> sat_by_id;
    std::unordered_map<std::string, double> cpu;
    std::unordered_map<std::string, double> mem;
    std::unordered_map<std::string, double> disk;
    for (const auto& sat : planning_topology.nodes) {
        sat_by_id[sat.id] = &sat;
        cpu[sat.id] = sat.cpu_available;
        mem[sat.id] = sat.mem_available;
        disk[sat.id] = sat.disk_available;
    }

    DeploymentCandidate repaired = session.last_candidate;
    repaired.per_vnf = per_vnf;
    repaired.reason.clear();

    std::unordered_map<std::string, std::string> placed_nf;
    for (size_t i = 0; i < repaired.per_vnf.size(); ++i) {
        if (std::find(affected.begin(), affected.end(), i) != affected.end()) continue;
        const auto& pv = repaired.per_vnf[i];
        const std::string nf = nf_key_from_pv(pv);
        if (!nf.empty()) placed_nf[nf] = pv.node;
        if (i < session.request.vnfs.size() && !pv.node.empty() && down_nodes.find(pv.node) == down_nodes.end()) {
            const auto& vnf = session.request.vnfs[i];
            cpu[pv.node] -= vnf.cpu;
            mem[pv.node] -= vnf.mem;
            disk[pv.node] -= vnf.disk;
        }
    }

    std::unordered_set<std::string> healthy_deployed_nodes;
    std::unordered_set<std::string> replacement_nodes;
    for (const auto& node : session.last_candidate.deployed_nodes) {
        if (!node.empty() && down_nodes.find(node) == down_nodes.end()) {
            healthy_deployed_nodes.insert(node);
        }
    }
    if (healthy_deployed_nodes.empty()) {
        if (detail) *detail = "partial_redeploy_no_healthy_anchor_for_scope";
        return std::nullopt;
    }

    constexpr int kLocalRepairHopLimit = 5;
    constexpr int kMaxLocalRepairValidations = 36;
    const auto nearby_nodes = collect_repair_neighborhood(
        planning_topology,
        healthy_deployed_nodes,
        down_nodes,
        kLocalRepairHopLimit
    );
    if (nearby_nodes.empty()) {
        if (detail) *detail = "partial_redeploy_empty_5hop_scope";
        return std::nullopt;
    }

    std::unordered_map<std::string, std::vector<std::pair<std::string, double>>> latency_adj;
    for (const auto& sat : planning_topology.nodes) {
        if (down_nodes.find(sat.id) == down_nodes.end() && !node_is_down(sat)) {
            latency_adj[sat.id] = {};
        }
    }
    for (const auto& link : planning_topology.links) {
        if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
        if (down_nodes.find(link.source) != down_nodes.end() || down_nodes.find(link.target) != down_nodes.end()) continue;
        if (to_lower(link.status) == "down") continue;
        if (link.bandwidth_available_gbps <= 0.0) continue;
        if (latency_adj.find(link.source) == latency_adj.end() || latency_adj.find(link.target) == latency_adj.end()) continue;
        const double weight = std::max(0.001, link.latency_ms) * (to_lower(link.status) == "congested" ? 1.35 : 1.0);
        latency_adj[link.source].push_back({link.target, weight});
        latency_adj[link.target].push_back({link.source, weight});
    }
    std::unordered_map<std::string, double> latency_cache;
    auto path_latency_ms = [&](const std::string& src, const std::string& dst) -> std::optional<double> {
        if (src.empty() || dst.empty()) return std::nullopt;
        if (src == dst) return 0.0;
        const std::string key = src < dst ? src + ">" + dst : dst + ">" + src;
        const auto cached = latency_cache.find(key);
        if (cached != latency_cache.end()) return cached->second;
        if (latency_adj.find(src) == latency_adj.end() || latency_adj.find(dst) == latency_adj.end()) return std::nullopt;

        using Item = std::pair<double, std::string>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        std::unordered_map<std::string, double> dist;
        dist[src] = 0.0;
        pq.push({0.0, src});
        while (!pq.empty()) {
            const auto [d, u] = pq.top();
            pq.pop();
            if (d > dist[u] + 1e-9) continue;
            if (u == dst) {
                latency_cache[key] = d;
                return d;
            }
            const auto adj_it = latency_adj.find(u);
            if (adj_it == latency_adj.end()) continue;
            for (const auto& edge : adj_it->second) {
                const double nd = d + edge.second;
                const auto it = dist.find(edge.first);
                if (it == dist.end() || nd < it->second) {
                    dist[edge.first] = nd;
                    pq.push({nd, edge.first});
                }
            }
        }
        return std::nullopt;
    };

    struct Choice { std::string node; double cost = std::numeric_limits<double>::infinity(); };
    std::vector<DeploymentCandidate::PerVNF> working_per_vnf = repaired.per_vnf;
    std::string last_reason;
    int validation_attempts = 0;

    auto make_choices = [&](size_t idx, bool allow_existing_core_nodes) {
        std::vector<Choice> choices;
        if (idx >= session.request.vnfs.size()) return choices;
        const auto& vnf = session.request.vnfs[idx];
        const std::string nf = nf_key_from_pv(working_per_vnf[idx]);
        choices.reserve(nearby_nodes.size());

        for (const auto& node_id : nearby_nodes) {
            const auto sat_it = sat_by_id.find(node_id);
            if (sat_it == sat_by_id.end()) continue;
            const auto& sat = *sat_it->second;
            if (down_nodes.find(sat.id) != down_nodes.end() || node_is_down(sat)) continue;
            const bool already_hosts_this_core = healthy_deployed_nodes.find(sat.id) != healthy_deployed_nodes.end();
            const bool is_new_replacement_node = replacement_nodes.find(sat.id) != replacement_nodes.end();
            if (already_hosts_this_core && !is_new_replacement_node && !allow_existing_core_nodes) continue;
            if (cpu[sat.id] + 1e-9 < vnf.cpu || mem[sat.id] + 1e-9 < vnf.mem || disk[sat.id] + 1e-9 < vnf.disk) continue;

            double locality_cost = 0.0;
            int linked_neighbors = 0;
            for (const auto& dep : session.request.core_nf_dependencies) {
                std::string other_nf;
                if (to_lower(dep.source) == nf) other_nf = to_lower(dep.target);
                else if (to_lower(dep.target) == nf) other_nf = to_lower(dep.source);
                else continue;
                const auto other_it = placed_nf.find(other_nf);
                if (other_it == placed_nf.end() || other_it->second.empty() || other_it->second == sat.id) continue;
                const auto neighbor_sat = sat_by_id.find(other_it->second);
                if (neighbor_sat == sat_by_id.end()) continue;
                const auto latency = path_latency_ms(sat.id, other_it->second);
                locality_cost += latency.has_value()
                    ? *latency * 120.0 * std::max(0.1, dep.latency_weight)
                    : coordinate_distance_km(sat.coordinates, neighbor_sat->second->coordinates) *
                        std::max(0.1, dep.latency_weight);
                linked_neighbors += 1;
            }
            if (linked_neighbors == 0) {
                double nearest_anchor_latency = std::numeric_limits<double>::infinity();
                double nearest_anchor_distance = std::numeric_limits<double>::infinity();
                for (const auto& node : healthy_deployed_nodes) {
                    const auto anchor_it = sat_by_id.find(node);
                    if (anchor_it == sat_by_id.end()) continue;
                    const auto latency = path_latency_ms(sat.id, node);
                    if (latency.has_value()) nearest_anchor_latency = std::min(nearest_anchor_latency, *latency);
                    nearest_anchor_distance = std::min(
                        nearest_anchor_distance,
                        coordinate_distance_km(sat.coordinates, anchor_it->second->coordinates)
                    );
                }
                if (std::isfinite(nearest_anchor_latency)) {
                    locality_cost += nearest_anchor_latency * 120.0;
                } else if (std::isfinite(nearest_anchor_distance)) {
                    locality_cost += nearest_anchor_distance * 0.35;
                }
            }

            const double cpu_after = sat.cpu_total > 0 ? (cpu[sat.id] - vnf.cpu) / sat.cpu_total : 0.0;
            const double mem_after = sat.mem_total > 0 ? (mem[sat.id] - vnf.mem) / sat.mem_total : 0.0;
            const double disk_after = sat.disk_total > 0 ? (disk[sat.id] - vnf.disk) / sat.disk_total : 0.0;
            const double resource_headroom = std::max(0.0, std::min(1.0, (cpu_after + mem_after + disk_after) / 3.0));
            const double resource_cost = (1.0 - resource_headroom) * 900.0;
            const double existing_core_penalty = already_hosts_this_core && !is_new_replacement_node ? 820.0 : 0.0;
            const double replacement_reuse_bonus = is_new_replacement_node ? 220.0 : 0.0;
            const double fresh_node_bonus = already_hosts_this_core ? 0.0 : 420.0;
            choices.push_back({
                sat.id,
                locality_cost + resource_cost + existing_core_penalty - replacement_reuse_bonus - fresh_node_bonus - linked_neighbors * 75.0
            });
        }

        std::sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) {
            return a.cost < b.cost;
        });
        const size_t limit = affected.size() <= 2 ? 18 : 12;
        if (choices.size() > limit) choices.resize(limit);
        return choices;
    };

    std::function<bool(size_t)> dfs = [&](size_t pos) -> bool {
        if (validation_attempts >= kMaxLocalRepairValidations) return false;
        if (pos >= affected.size()) {
            DeploymentCandidate candidate = session.last_candidate;
            candidate.per_vnf = working_per_vnf;
            candidate.reason.clear();
            validation_attempts += 1;
            std::string reason;
            if (rebuild_candidate_paths_and_sla(&candidate, session.request, planning_topology, &reason)) {
                repaired = std::move(candidate);
                return true;
            }
            last_reason = reason.empty() ? "partial_redeploy_validation_failed" : reason;
            return false;
        }

        const size_t idx = affected[pos];
        if (idx >= session.request.vnfs.size()) return dfs(pos + 1);
        const auto& vnf = session.request.vnfs[idx];
        const std::string nf = nf_key_from_pv(working_per_vnf[idx]);
        auto choices = make_choices(idx, false);
        if (choices.empty()) {
            choices = make_choices(idx, true);
        }
        if (choices.empty()) {
            last_reason = "partial_redeploy_no_5hop_target_for_" + nf;
            return false;
        }

        for (const auto& choice : choices) {
            if (validation_attempts >= kMaxLocalRepairValidations) break;
            const std::string old_node = working_per_vnf[idx].node;
            const auto placed_it = placed_nf.find(nf);
            const bool had_placed = placed_it != placed_nf.end();
            const std::string old_placed = had_placed ? placed_it->second : "";
            const bool inserted_replacement =
                healthy_deployed_nodes.find(choice.node) == healthy_deployed_nodes.end() &&
                replacement_nodes.insert(choice.node).second;

            working_per_vnf[idx].node = choice.node;
            placed_nf[nf] = choice.node;
            cpu[choice.node] -= vnf.cpu;
            mem[choice.node] -= vnf.mem;
            disk[choice.node] -= vnf.disk;

            const bool ok = dfs(pos + 1);

            cpu[choice.node] += vnf.cpu;
            mem[choice.node] += vnf.mem;
            disk[choice.node] += vnf.disk;
            working_per_vnf[idx].node = old_node;
            if (had_placed) placed_nf[nf] = old_placed;
            else placed_nf.erase(nf);
            if (inserted_replacement) replacement_nodes.erase(choice.node);

            if (ok) return true;
        }
        return false;
    };

    if (!dfs(0)) {
        if (detail) {
            *detail = last_reason.empty()
                ? "partial_redeploy_5hop_search_exhausted"
                : last_reason;
        }
        return std::nullopt;
    }
    if (detail) {
        *detail = "partial_redeploy_success:affected_nfs=" + std::to_string(affected.size()) +
            ",scope_hops=" + std::to_string(kLocalRepairHopLimit) +
            ",scope_nodes=" + std::to_string(nearby_nodes.size()) +
            ",validated=" + std::to_string(validation_attempts);
    }
    return repaired;
}

std::string DynamicInferenceService::infer_required_recompute_trigger(
    const SessionState& session,
    const TopologySnapshot& snapshot,
    const std::unordered_set<std::string>& down_nodes,
    const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
    std::string* disconnected_from,
    std::string* disconnected_to
) {
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
        Topology topology = snapshot.topology;
        if (topology.nodes.empty()) topology = res_mgr_->export_current_topology();
        if (topology.nodes.empty()) topology = topo_mgr_->get_current_topology();
        if (!topology.nodes.empty()) {
            topology = build_planning_topology_for_session(session, topology, down_nodes);
            DeploymentCandidate current = session.last_candidate;
            std::string sla_reason;
            const bool current_ok = rebuild_candidate_paths_and_sla(&current, session.request, topology, &sla_reason);
            if (!current_ok) {
                const bool latency_violation =
                    current.total_latency_ms > session.request.constraints.max_latency_ms ||
                    current.registration_latency_ms > session.request.constraints.registration_latency_ms ||
                    current.pdu_session_latency_ms > session.request.constraints.pdu_session_latency_ms;
                if (latency_violation) {
                    if (disconnected_from) *disconnected_from = "sla";
                    if (disconnected_to) *disconnected_to = "latency";
                    return "sla_latency_violation";
                }
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

    const auto down_nodes = collect_down_nodes(topology);
    Topology planning_topology = build_planning_topology_for_session(session, topology, down_nodes);
    std::string recovery_strategy = "";
    std::string partial_detail = "";
    if (trigger == "anchor_path_disconnected") {
        recovery_strategy = "local_reroute";
    } else if (trigger == "link_recovery_reroute") {
        recovery_strategy = "link_recovery_path_optimization";
    } else if (trigger == "sla_latency_violation") {
        recovery_strategy = "full_redeploy_sla_latency_violation";
    }
    nlohmann::json decision_process;
    const auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<DeploymentCandidate> candidates;
    if ((trigger == "anchor_path_disconnected" || trigger == "link_recovery_reroute") && session.has_last_candidate) {
        DeploymentCandidate rerouted = session.last_candidate;
        std::string reroute_reason;
        if (rebuild_candidate_paths_and_sla(&rerouted, session.request, planning_topology, &reroute_reason)) {
            decision_process = {
                {"algorithm", "fast_dependency_path_reroute"},
                {"status", "success"},
                {"detail", trigger == "link_recovery_reroute"
                    ? "link_recovered_same_nf_placement_path_optimized"
                    : "same_nf_placement_path_recomputed"}
            };
            candidates.push_back(std::move(rerouted));
        } else {
            partial_detail = reroute_reason.empty() ? "fast_path_reroute_unavailable" : reroute_reason;
            decision_process = {
                {"algorithm", "fast_dependency_path_reroute"},
                {"status", "failed"},
                {"detail", partial_detail}
            };
            recovery_strategy = trigger == "link_recovery_reroute"
                ? "full_redeploy_after_recovery_path_optimization_unavailable"
                : "full_redeploy_after_path_reroute_unavailable";
            candidates = inference_engine_->inference(session.request, planning_topology, &decision_process);
            if (!decision_process.is_object()) {
                decision_process = nlohmann::json::object();
            }
            decision_process["recovery_algorithm"] = "model_full_redeploy";
            decision_process["recovery_reason"] = trigger == "link_recovery_reroute"
                ? "recovered_link_path_optimization_unavailable"
                : "dependency_path_reroute_unavailable";
            decision_process["detail"] = partial_detail;
            if (candidates.empty()) {
                decision_process["status"] = "failed";
            }
        }
    } else if (trigger == "deployment_node_down") {
        auto partial = try_partial_node_redeploy(session, planning_topology, down_nodes, &partial_detail);
        if (partial.has_value()) {
            recovery_strategy = "partial_node_redeploy";
            decision_process = {
                {"algorithm", "partial_node_redeploy"},
                {"status", "success"},
                {"detail", partial_detail}
            };
            candidates.push_back(std::move(*partial));
        } else {
            const bool multi_node_fault =
                partial_detail.find("escalate_full_redeploy_multi_node_fault") != std::string::npos;
            recovery_strategy = multi_node_fault
                ? "full_redeploy_multi_node_fault"
                : "full_redeploy_after_partial_unavailable";
            WSHandler::broadcast_json({
                {"type", "recovery_event"},
                {"entity_type", "session"},
                {"entity_id", session.session_id},
                {"request_id", session.request.request_id},
                {"sim_time", session.request.sim_time},
                {"topology_version", session.request.topology_version},
                {"strategy", multi_node_fault ? "full_redeploy_multi_node_fault" : "partial_node_redeploy_failed_escalate_full"},
                {"trigger", trigger},
                {"result", "escalating"},
                {"success", false},
                {"reason", partial_detail.empty() ? "partial_redeploy_unavailable" : partial_detail},
                {"affected_services", 1}
            });
            candidates = inference_engine_->inference(session.request, planning_topology, &decision_process);
            if (!decision_process.is_object()) {
                decision_process = nlohmann::json::object();
            }
            decision_process["recovery_algorithm"] = "model_full_redeploy";
            decision_process["recovery_reason"] = multi_node_fault
                ? "multi_deployed_satellite_fault"
                : "partial_redeploy_unavailable";
            decision_process["detail"] = partial_detail.empty() ? "escalated_to_model_full_redeploy" : partial_detail;
            if (candidates.empty()) {
                decision_process["status"] = "failed";
            }
        }
    } else if (trigger == "sla_latency_violation") {
        decision_process = {
            {"algorithm", "model_full_redeploy"},
            {"status", "running"},
            {"detail", "dynamic_latency_sla_violation"}
        };
        candidates = inference_engine_->inference(session.request, planning_topology, &decision_process);
        if (!decision_process.is_object()) decision_process = nlohmann::json::object();
        decision_process["recovery_algorithm"] = "model_full_redeploy";
        decision_process["recovery_reason"] = "dynamic_latency_sla_violation";
        decision_process["detail"] = "registration_or_pdu_latency_exceeded_during_dynamic_run";
        if (candidates.empty()) decision_process["status"] = "failed";
    } else {
        candidates = inference_engine_->inference(session.request, planning_topology, &decision_process);
    }
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
            {"registration_latency_ms", c.registration_latency_ms},
            {"pdu_session_latency_ms", c.pdu_session_latency_ms},
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
        {"recovery_strategy", recovery_strategy},
        {"partial_redeploy_detail", partial_detail},
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
    const bool link_fault_reroute_only =
        (trigger == "anchor_path_disconnected" || trigger == "link_recovery_reroute") &&
        session.has_last_candidate;
    if (link_fault_reroute_only) {
        chosen_it = std::find_if(
            response_candidates.begin(),
            response_candidates.end(),
            [&](const DeploymentCandidate& c) {
                return is_deployable_candidate(c) &&
                    c.deployed_nodes == session.last_candidate.deployed_nodes;
            }
        );
    }
    if (chosen_it != response_candidates.end()) {
        auto chosen = *chosen_it;
        chosen.per_vnf = ensure_per_vnf_filled(chosen, session.request);
        normalize_candidate_bandwidth_requirements(&chosen, session.request);
        const std::string sig = candidate_signature(chosen);
        const std::string placement_sig = candidate_placement_signature(chosen);
        const bool placement_changed =
            !session.has_last_candidate ||
            placement_sig != candidate_placement_signature(session.last_candidate);
        const bool changed = (!session.has_last_candidate) || (sig != session.last_candidate_signature);
        const std::string status = changed ? (session.has_last_candidate ? "redeployed" : "deployed") : "stable";
        trace_payload["status"] = status;
        trace_payload["path_changed"] = changed;
        trace_payload["placement_changed"] = placement_changed;

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
                    {"recovery_strategy", recovery_strategy},
                    {"reason", fail_reason}
                });
                return {
                    {"session_id", session.session_id},
                    {"status", "decision_failed"},
                    {"topology_version", session.last_topology_version},
                    {"sim_time", session.last_sim_time},
                    {"inference_time_ms", inference_ms},
                    {"recovery_strategy", recovery_strategy},
                    {"reason", fail_reason}
                };
            }

            session.active_resource_deployment_id = new_allocation_id;
            auto updated_topology = res_mgr_->export_current_topology();
            topo_mgr_->save_current_topology(updated_topology);

            const bool path_only_reroute =
                (trigger == "anchor_path_disconnected" || trigger == "link_recovery_reroute") &&
                !placement_changed;
            if (!path_only_reroute &&
                !session.orchestration_deployment_id.empty() &&
                g_deployment_orchestrator &&
                deployment_runtime_enabled_for_inference(session.orchestration_deployment_id)) {
                const std::string orchestration_trigger = recovery_strategy.empty() ? trigger : recovery_strategy;
                g_deployment_orchestrator->enqueue_deployment(
                    session.orchestration_deployment_id,
                    session.request.request_id,
                    chosen,
                    session.request.vnfs,
                    "session_continuous",
                    orchestration_trigger
                );
            }

            session.redeploy_total += 1;
            total_redeploys_ += 1;
        } else if (
            !session.orchestration_deployment_id.empty() &&
            g_deployment_orchestrator &&
            deployment_runtime_enabled_for_inference(session.orchestration_deployment_id) &&
            trigger != "session_start" &&
            trigger != "topology_tick_bootstrap" &&
            trigger != "anchor_path_disconnected" &&
            trigger != "link_recovery_reroute"
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

        if (!session.orchestration_deployment_id.empty()) {
            nlohmann::json candidate_json = chosen.to_json();
            candidate_json["score_total"] = chosen.score;
            candidate_json["session_id"] = session.session_id;
            candidate_json["strategy_mode"] = "session_continuous";
            candidate_json["request_id"] = session.request.request_id;
            candidate_json["status"] = "completed";
            candidate_json["decision_trigger"] = trigger;
            candidate_json["topology_version_bound"] = session.last_topology_version;
            candidate_json["last_update_at"] = session.last_sim_time;
            (void)patch_deployment_record(session.orchestration_deployment_id, candidate_json, nullptr);
        }

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
            {"recovery_strategy", recovery_strategy},
            {"reason", chosen.reason}
        });
        return {
            {"session_id", session.session_id},
            {"status", status},
            {"topology_version", session.last_topology_version},
            {"sim_time", session.last_sim_time},
            {"inference_time_ms", inference_ms},
            {"recovery_strategy", recovery_strategy}
        };
    }

    session.failures_total += 1;
    total_failures_ += 1;
    const std::string pending_reason = link_fault_reroute_only
        ? "anchor_reroute_and_cross_node_reschedule_unavailable"
        : (response_candidates.empty() ? "no_candidate" : "no_deployable_candidate");
    session.pending_replanning = true;
    session.last_replanning_attempt_topology_version = session.last_topology_version;
    trace_payload["status"] = "replanning";
    trace_payload["path_changed"] = false;
    trace_payload["placement_changed"] = false;
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
        {"recovery_strategy", recovery_strategy},
        {"reason", pending_reason}
    });
    return {
        {"session_id", session.session_id},
        {"status", "replanning"},
        {"topology_version", session.last_topology_version},
        {"sim_time", session.last_sim_time},
        {"inference_time_ms", inference_ms},
        {"recovery_strategy", recovery_strategy},
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

    if (!initial_deployment_id.empty()) {
        for (const auto& kv : sessions_) {
            const auto& existing = kv.second;
            if (!existing.active) continue;
            if (existing.orchestration_deployment_id != initial_deployment_id) continue;
            return {
                {"session_id", existing.session_id},
                {"active", true},
                {"auto_redeploy", existing.auto_redeploy},
                {"request_id", existing.request.request_id},
                {"deduplicated", true},
                {"initial_result", {
                    {"session_id", existing.session_id},
                    {"status", "stable"},
                    {"topology_version", existing.last_topology_version},
                    {"sim_time", existing.last_sim_time},
                    {"inference_time_ms", existing.last_inference_time_ms}
                }}
            };
        }
    }

    sessions_[session.session_id] = session;
    sessions_[session.session_id].orchestration_deployment_id = initial_deployment_id;

    TopologySnapshot snapshot = dynamic_sim_ ? dynamic_sim_->refresh_current_snapshot(false) : TopologySnapshot{};
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

        if (!initial_deployment_id.empty()) {
            nlohmann::json patch = {
                {"session_id", session.session_id},
                {"strategy_mode", "session_continuous"},
                {"request_id", sessions_[session.session_id].request.request_id},
                {"status", "completed"},
                {"auto_redeploy", auto_redeploy}
            };
            (void)patch_deployment_record(initial_deployment_id, patch, nullptr);
        }

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

std::vector<std::string> DynamicInferenceService::stop_sessions_for_deployment(
    const std::string& deployment_id,
    const std::string& request_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> released_ids;
    bool released_any = false;

    for (auto& kv : sessions_) {
        auto& session = kv.second;
        if (!session.active) continue;
        const bool deployment_match =
            !deployment_id.empty() &&
            (session.orchestration_deployment_id == deployment_id ||
             session.active_resource_deployment_id == deployment_id);
        const bool request_match =
            !request_id.empty() && session.request.request_id == request_id;
        if (!deployment_match && !request_match) continue;

        if (!session.active_resource_deployment_id.empty()) {
            const std::string alloc_id = session.active_resource_deployment_id;
            if (res_mgr_->release_resources(alloc_id)) {
                released_ids.push_back(alloc_id);
                released_any = true;
            } else {
                spdlog::debug(
                    "Session {} failed to release allocation {} while stopping deployment {}",
                    session.session_id,
                    alloc_id,
                    deployment_id
                );
            }
            session.active_resource_deployment_id.clear();
        }
        session.active = false;
        session.auto_redeploy = false;
        session.pending_replanning = false;
    }

    if (released_any) {
        auto updated_topology = res_mgr_->export_current_topology();
        topo_mgr_->save_current_topology(updated_topology);
    }
    return released_ids;
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
    TopologySnapshot snapshot = dynamic_sim_ ? dynamic_sim_->refresh_current_snapshot(false) : TopologySnapshot{};
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
    const auto current_down_link_state = down_link_signature(topology);
    const std::string current_down_link_signature = current_down_link_state.first;
    const int current_down_link_count = current_down_link_state.second;

    for (auto& kv : sessions_) {
        auto& session = kv.second;
        if (!session.active) continue;
        if (!session.auto_redeploy) continue;
        auto remember_link_observation = [&]() {
            session.last_observed_down_link_signature = current_down_link_signature;
            session.last_observed_down_link_count = current_down_link_count;
        };
        const bool has_link_observation = session.last_observed_down_link_count >= 0;
        const bool link_recovered_since_last_tick =
            has_link_observation &&
            current_down_link_count < session.last_observed_down_link_count &&
            current_down_link_signature != session.last_observed_down_link_signature;

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
        if (trigger.empty() && session.has_last_candidate && link_recovered_since_last_tick) {
            trigger = "link_recovery_reroute";
            disconnected_from = "recovered_link";
            disconnected_to = "optimized_path";
        }
        if (trigger.empty()) {
            if (session.pending_replanning) {
                if (session.last_replanning_attempt_topology_version == snapshot.topology_version) {
                    remember_link_observation();
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
                remember_link_observation();
                continue;
            }
            session.last_required_recompute_signature.clear();
            session.last_required_recompute_topology_version = -1;
            remember_link_observation();
            continue;
        }

        const bool fault_driven = trigger != "topology_tick_bootstrap";
        const std::string reason_signature = trigger + "|" + disconnected_from + "->" + disconnected_to;
        const bool repeated_same_reason = fault_driven && reason_signature == session.last_required_recompute_signature;
        if (repeated_same_reason &&
            session.last_required_recompute_topology_version == snapshot.topology_version) {
            remember_link_observation();
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
            const std::string explicit_strategy = result.value("recovery_strategy", "");
            if (success) {
                if (!explicit_strategy.empty()) {
                    strategy = explicit_strategy;
                } else if (!had_prev) {
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
        remember_link_observation();
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
