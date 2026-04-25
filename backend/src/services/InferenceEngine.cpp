#include "services/InferenceEngine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <queue>
#include <limits>
#include <unordered_set>
#include <unordered_map>
#include <cmath>
#include <numeric>
#include <cstdint>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>

namespace sfc {
namespace {

double clamp01(double x);
bool link_is_down(const Link& link);
constexpr size_t kBaseNodeFeatureDim = 8;
constexpr size_t kBaseVnfFeatureDim = 8;
constexpr size_t kBaseContextFeatureDim = 48;
constexpr size_t kDefaultNodeEmbeddingDim = 192;

std::optional<size_t> parse_expected_dim_from_error(
    const std::string& err,
    const std::string& tensor_name
) {
    const size_t tensor_pos = err.find(tensor_name);
    if (tensor_pos == std::string::npos) return std::nullopt;
    const size_t expected_pos = err.find("Expected:", tensor_pos);
    if (expected_pos == std::string::npos) return std::nullopt;
    size_t pos = expected_pos + 9;
    while (pos < err.size() && std::isspace(static_cast<unsigned char>(err[pos]))) ++pos;
    size_t end = pos;
    while (end < err.size() && std::isdigit(static_cast<unsigned char>(err[end]))) ++end;
    if (end <= pos) return std::nullopt;
    try {
        return static_cast<size_t>(std::stoul(err.substr(pos, end - pos)));
    } catch (...) {
        return std::nullopt;
    }
}

size_t positive_last_dim_or(const std::vector<int64_t>& shape, size_t fallback) {
    for (auto it = shape.rbegin(); it != shape.rend(); ++it) {
        if (*it > 0) return static_cast<size_t>(*it);
    }
    return fallback;
}

std::vector<float> fit_feature_dim(const std::vector<float>& base, size_t target_dim) {
    if (target_dim == 0) return {};
    if (target_dim == base.size()) return base;
    std::vector<float> out(target_dim, 0.0f);
    const size_t copy_n = std::min(target_dim, base.size());
    if (copy_n > 0) {
        std::copy_n(base.data(), copy_n, out.data());
    }
    return out;
}

void maybe_load_model_io_meta(
    const std::string& actor_model_path,
    size_t* node_feature_dim,
    size_t* vnf_feature_dim,
    size_t* context_feature_dim,
    size_t* node_embedding_dim
) {
    if (!node_feature_dim || !vnf_feature_dim || !context_feature_dim || !node_embedding_dim) return;
    try {
        const std::filesystem::path actor_path(actor_model_path);
        const std::filesystem::path meta_path = actor_path.parent_path() / "model_io_meta.json";
        if (!std::filesystem::exists(meta_path)) return;
        std::ifstream ifs(meta_path);
        if (!ifs.is_open()) return;
        nlohmann::json meta;
        ifs >> meta;
        if (!meta.is_object()) return;
        if (meta.contains("node_feature_dim")) {
            *node_feature_dim = std::max<size_t>(1, static_cast<size_t>(meta.value("node_feature_dim", *node_feature_dim)));
        }
        if (meta.contains("vnf_feature_dim")) {
            *vnf_feature_dim = std::max<size_t>(1, static_cast<size_t>(meta.value("vnf_feature_dim", *vnf_feature_dim)));
        }
        if (meta.contains("context_feature_dim")) {
            *context_feature_dim = std::max<size_t>(1, static_cast<size_t>(meta.value("context_feature_dim", *context_feature_dim)));
        }
        if (meta.contains("gnn_output_dim")) {
            *node_embedding_dim = std::max<size_t>(1, static_cast<size_t>(meta.value("gnn_output_dim", *node_embedding_dim)));
        }
    } catch (...) {
        // Ignore malformed metadata and rely on model introspection / defaults.
    }
}

std::string normalize_nf_type(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '-' || ch == ' ') out.push_back('_');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::vector<float> build_vnf_features(const VNF& vnf, size_t vnf_feature_dim) {
    const double bw_required = std::max(vnf.bw_in, vnf.bw_out);
    const std::string nf_type = normalize_nf_type(vnf.nf_type.empty() ? vnf.name : vnf.nf_type);
    const bool is_user_plane = (nf_type == "upf");
    const bool is_control_plane = !is_user_plane;
    const double stateful = vnf.stateful ? 1.0 : 0.0;
    const double processing_weight = std::max(0.3, std::min(3.0, vnf.processing_weight));
    const std::vector<float> base = {
        static_cast<float>(vnf.cpu),
        static_cast<float>(vnf.mem),
        static_cast<float>(bw_required),
        static_cast<float>(vnf.disk),
        static_cast<float>(is_user_plane ? 1.0 : 0.0),
        static_cast<float>(is_control_plane ? 1.0 : 0.0),
        static_cast<float>(stateful),
        static_cast<float>(processing_weight),
    };
    return fit_feature_dim(base, vnf_feature_dim);
}

std::vector<float> build_context_features(
    const SFCRequest& request,
    size_t current_vnf_idx,
    double remaining_delay,
    double accumulated_delay,
    double accumulated_reliability,
    double active_node_ratio,
    double active_link_ratio,
    double avg_latency_ms,
    double avg_bandwidth_utilization,
    size_t context_feature_dim
) {
    std::vector<float> ctx(kBaseContextFeatureDim, 0.0f);
    const double total_vnfs = std::max<size_t>(1, request.vnfs.size());
    const double reliability_req = request.constraints.min_reliability;
    const double bandwidth_demand = request.constraints.min_bandwidth_gbps;
    const double request_business_index = 0.0;

    ctx[0] = static_cast<float>(remaining_delay / 300.0);
    ctx[1] = static_cast<float>(static_cast<double>(current_vnf_idx) / total_vnfs);
    // Deprecated: request-level core_network_load removed, keep slot for model compatibility.
    ctx[2] = static_cast<float>(request_business_index);
    ctx[3] = static_cast<float>(bandwidth_demand / 10.0);
    ctx[4] = static_cast<float>(reliability_req);
    ctx[5] = static_cast<float>(accumulated_reliability);
    // Deprecated: priority_weight/load_level removed, keep zero placeholders for compatibility.
    ctx[6] = 0.0f;
    ctx[7] = 0.0f;
    ctx[8] = static_cast<float>(accumulated_delay / 300.0);
    ctx[9] = static_cast<float>(accumulated_reliability - reliability_req);
    ctx[10] = static_cast<float>(std::max(0, request.topology_version) / 10000.0);
    ctx[11] = static_cast<float>(request.sim_time.empty() ? 0.0 : 1.0);
    ctx[12] = static_cast<float>(clamp01(active_node_ratio));
    ctx[13] = static_cast<float>(clamp01(active_link_ratio));
    ctx[14] = static_cast<float>(std::max(0.0, avg_latency_ms) / 80.0);
    ctx[15] = static_cast<float>(clamp01(avg_bandwidth_utilization));
    ctx[16] = 0.0f;
    ctx[17] = 0.0f;
    ctx[18] = 0.0f;
    ctx[19] = 0.0f;
    ctx[20] = 0.0f;
    ctx[21] = 0.0f;
    return fit_feature_dim(ctx, context_feature_dim);
}

struct DynamicTopologyFeatures {
    double active_node_ratio = 1.0;
    double active_link_ratio = 1.0;
    double avg_latency_ms = 0.0;
    double avg_bandwidth_utilization = 0.0;
};

DynamicTopologyFeatures build_dynamic_topology_features(const Topology& topology) {
    DynamicTopologyFeatures f{};
    const double node_total = static_cast<double>(std::max<size_t>(1, topology.nodes.size()));
    const double link_total = static_cast<double>(std::max<size_t>(1, topology.links.size()));

    int active_nodes = 0;
    for (const auto& n : topology.nodes) {
        if (n.status != "down") active_nodes += 1;
    }
    f.active_node_ratio = static_cast<double>(active_nodes) / node_total;

    int active_links = 0;
    double latency_sum = 0.0;
    double util_sum = 0.0;
    int util_cnt = 0;
    for (const auto& l : topology.links) {
        if (!link_is_down(l)) active_links += 1;
        latency_sum += std::max(0.0, l.latency_ms);
        if (l.bandwidth_gbps > 1e-9) {
            const double bw_ratio = clamp01(l.bandwidth_available_gbps / l.bandwidth_gbps);
            util_sum += 1.0 - bw_ratio;
            util_cnt += 1;
        }
    }
    f.active_link_ratio = static_cast<double>(active_links) / link_total;
    f.avg_latency_ms = latency_sum / link_total;
    f.avg_bandwidth_utilization = util_cnt > 0 ? util_sum / static_cast<double>(util_cnt) : 0.0;
    return f;
}

std::string build_candidate_signature(const DeploymentCandidate& candidate) {
    std::string sig;
    sig.reserve(candidate.per_vnf.size() * 24);
    for (const auto& pv : candidate.per_vnf) {
        sig.append(pv.vnf);
        sig.push_back('@');
        sig.append(pv.node);
        sig.push_back('|');
    }
    return sig;
}

std::string make_pair_key(const std::string& a, const std::string& b) {
    std::string key;
    key.reserve(a.size() + b.size() + 1);
    key.append(a);
    key.push_back('\n');
    key.append(b);
    return key;
}

bool link_is_down(const Link& link) {
    return link.status == "down";
}

double clamp01(double x) {
    return std::max(0.0, std::min(1.0, x));
}

constexpr int kMaxActorCandidatePool = 72;
constexpr int kMaxProbePerVnf = 48;
constexpr int kRealtimeActorCandidatePool = 28;
constexpr int kRealtimeProbePerVnf = 18;
constexpr double kHopPenaltyMs = 2.5;
constexpr double kPathSofteningExponent = 0.40;
constexpr double kExcessHopReliabilityPenalty = 0.9993;
constexpr double kFutureStepReliabilityDecay = 0.9995;
constexpr int kTargetDeploymentHops = 50;
constexpr int kHardTotalHopLimit = 50;
constexpr int kMinLegHopCap = 3;
constexpr int kMaxLegHopCap = 14;

CandidateSearchTuning default_candidate_search_tuning() {
    return CandidateSearchTuning{};
}

int read_int_with_bounds(
    const nlohmann::json& obj,
    const char* key,
    int current,
    int lo,
    int hi
) {
    if (!obj.is_object() || !obj.contains(key)) return current;
    const auto& v = obj.at(key);
    if (!v.is_number_integer() && !v.is_number_unsigned()) return current;
    const int parsed = v.get<int>();
    return std::max(lo, std::min(hi, parsed));
}

double read_double_with_bounds(
    const nlohmann::json& obj,
    const char* key,
    double current,
    double lo,
    double hi
) {
    if (!obj.is_object() || !obj.contains(key)) return current;
    const auto& v = obj.at(key);
    if (!v.is_number()) return current;
    const double parsed = v.get<double>();
    return std::max(lo, std::min(hi, parsed));
}

std::vector<double> read_relax_levels_with_bounds(
    const nlohmann::json& obj,
    const char* key,
    const std::vector<double>& current,
    double lo,
    double hi
) {
    if (!obj.is_object() || !obj.contains(key)) return current;
    const auto& arr = obj.at(key);
    if (!arr.is_array()) return current;
    std::vector<double> out;
    out.reserve(arr.size());
    for (const auto& item : arr) {
        if (!item.is_number()) continue;
        const double v = std::max(lo, std::min(hi, item.get<double>()));
        out.push_back(v);
    }
    if (out.empty()) return current;
    std::sort(out.begin(), out.end(), std::greater<double>());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) {
        return std::abs(a - b) <= 1e-6;
    }), out.end());
    if (out.front() < 0.999) {
        out.insert(out.begin(), 1.0);
    } else {
        out.front() = 1.0;
    }
    return out;
}

std::vector<std::filesystem::path> build_candidate_tuning_paths(const std::string& actor_model_path) {
    std::vector<std::filesystem::path> out;
    const auto push = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        const auto normalized = std::filesystem::absolute(p).lexically_normal();
        if (std::find(out.begin(), out.end(), normalized) == out.end()) {
            out.push_back(normalized);
        }
    };

    if (const char* env = std::getenv("SFC_CANDIDATE_SEARCH_CONFIG"); env && *env) {
        push(std::filesystem::path(env));
    }
    push(std::filesystem::path("config/inference_candidate_config.json"));
    push(std::filesystem::path("inference_candidate_config.json"));
    push(std::filesystem::path("../config/inference_candidate_config.json"));
    push(std::filesystem::path("backend/config/inference_candidate_config.json"));
    push(std::filesystem::path("../backend/config/inference_candidate_config.json"));

    if (!actor_model_path.empty()) {
        const auto actor = std::filesystem::path(actor_model_path).lexically_normal();
        const auto actor_parent = actor.parent_path();
        if (!actor_parent.empty()) {
            push(actor_parent / "inference_candidate_config.json");
            // models/exported/actor.onnx -> project root -> backend/config/...
            push(actor_parent / "../../backend/config/inference_candidate_config.json");
            push(actor_parent / "../../../backend/config/inference_candidate_config.json");
        }
    }

    return out;
}

CandidateSearchTuning load_candidate_search_tuning(
    const std::string& actor_model_path,
    std::string* loaded_path
) {
    auto tuning = default_candidate_search_tuning();
    if (loaded_path) loaded_path->clear();

    const auto candidates = build_candidate_tuning_paths(actor_model_path);
    for (const auto& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) continue;

        try {
            std::ifstream ifs(path);
            if (!ifs.is_open()) continue;
            nlohmann::json j;
            ifs >> j;
            if (!j.is_object()) continue;

            const auto general = j.value("general", nlohmann::json::object());
            const auto offline = j.value("offline", nlohmann::json::object());
            const auto realtime = j.value("realtime", nlohmann::json::object());
            const auto attempts = j.value("attempts", nlohmann::json::object());
            const auto reliability = j.value("reliability_relaxation", nlohmann::json::object());
            const auto trace = j.value("trace", nlohmann::json::object());
            const auto returning = j.value("returning", nlohmann::json::object());

            tuning.offline_target_min = read_int_with_bounds(offline, "target_feasible_min", tuning.offline_target_min, 1, 256);
            tuning.offline_target_multiplier = read_int_with_bounds(offline, "target_feasible_multiplier", tuning.offline_target_multiplier, 1, 32);
            tuning.offline_target_cap = read_int_with_bounds(offline, "target_feasible_cap", tuning.offline_target_cap, 1, 512);
            tuning.offline_default_attempt_cap = read_int_with_bounds(offline, "default_attempt_cap", tuning.offline_default_attempt_cap, 0, 100000);
            tuning.offline_min_attempt_cap = read_int_with_bounds(offline, "min_attempt_cap", tuning.offline_min_attempt_cap, 0, 100000);
            tuning.offline_default_time_budget_ms = read_double_with_bounds(offline, "default_time_budget_ms", tuning.offline_default_time_budget_ms, 0.0, 120000.0);
            tuning.offline_min_time_budget_ms = read_double_with_bounds(offline, "min_time_budget_ms", tuning.offline_min_time_budget_ms, 0.0, 120000.0);

            tuning.realtime_target_min = read_int_with_bounds(realtime, "target_feasible_min", tuning.realtime_target_min, 1, 64);
            tuning.realtime_target_multiplier = read_int_with_bounds(realtime, "target_feasible_multiplier", tuning.realtime_target_multiplier, 1, 16);
            tuning.realtime_target_cap = read_int_with_bounds(realtime, "target_feasible_cap", tuning.realtime_target_cap, 1, 128);
            tuning.realtime_default_attempt_cap = read_int_with_bounds(realtime, "default_attempt_cap", tuning.realtime_default_attempt_cap, 0, 100000);
            tuning.realtime_min_attempt_cap = read_int_with_bounds(realtime, "min_attempt_cap", tuning.realtime_min_attempt_cap, 0, 100000);
            tuning.realtime_default_time_budget_ms = read_double_with_bounds(realtime, "default_time_budget_ms", tuning.realtime_default_time_budget_ms, 0.0, 120000.0);
            tuning.realtime_min_time_budget_ms = read_double_with_bounds(realtime, "min_time_budget_ms", tuning.realtime_min_time_budget_ms, 0.0, 120000.0);

            tuning.strict_attempt_base = read_int_with_bounds(attempts, "strict_base", tuning.strict_attempt_base, 1, 100000);
            tuning.strict_attempt_per_target = read_int_with_bounds(attempts, "strict_per_target", tuning.strict_attempt_per_target, 1, 100000);
            tuning.relaxed_attempt_base = read_int_with_bounds(attempts, "relaxed_base", tuning.relaxed_attempt_base, 1, 100000);
            tuning.relaxed_attempt_per_target = read_int_with_bounds(attempts, "relaxed_per_target", tuning.relaxed_attempt_per_target, 1, 100000);
            tuning.extra_attempt_base = read_int_with_bounds(attempts, "extra_base", tuning.extra_attempt_base, 1, 100000);
            tuning.extra_attempt_per_target = read_int_with_bounds(attempts, "extra_per_target", tuning.extra_attempt_per_target, 1, 100000);

            tuning.relax_disable_min_reliability = read_double_with_bounds(
                reliability, "disable_when_min_reliability_below", tuning.relax_disable_min_reliability, 0.30, 0.99
            );
            tuning.relax_min_reliability_floor = read_double_with_bounds(
                reliability, "min_reliability_floor", tuning.relax_min_reliability_floor, 0.30, 0.99
            );
            tuning.reliability_relax_levels = read_relax_levels_with_bounds(
                reliability, "levels", tuning.reliability_relax_levels, 0.30, 1.0
            );

            tuning.trace_attempts_offline = read_int_with_bounds(trace, "max_attempts_offline", tuning.trace_attempts_offline, 0, 1000);
            tuning.trace_attempts_realtime = read_int_with_bounds(trace, "max_attempts_realtime", tuning.trace_attempts_realtime, 0, 1000);
            tuning.offline_return_topk_floor = read_int_with_bounds(
                returning, "offline_min_return_topk", tuning.offline_return_topk_floor, 1, 64
            );
            tuning.realtime_return_topk_floor = read_int_with_bounds(
                returning, "realtime_min_return_topk", tuning.realtime_return_topk_floor, 1, 32
            );

            const int min_attempt_floor = read_int_with_bounds(general, "global_min_attempt_floor", -1, 0, 100000);
            if (min_attempt_floor >= 0) {
                tuning.offline_min_attempt_cap = std::max(tuning.offline_min_attempt_cap, min_attempt_floor);
                tuning.realtime_min_attempt_cap = std::max(tuning.realtime_min_attempt_cap, min_attempt_floor);
            }

            if (tuning.offline_target_cap < tuning.offline_target_min) {
                tuning.offline_target_cap = tuning.offline_target_min;
            }
            if (tuning.realtime_target_cap < tuning.realtime_target_min) {
                tuning.realtime_target_cap = tuning.realtime_target_min;
            }
            if (tuning.offline_default_attempt_cap > 0 && tuning.offline_default_attempt_cap < tuning.offline_min_attempt_cap) {
                tuning.offline_default_attempt_cap = tuning.offline_min_attempt_cap;
            }
            if (tuning.realtime_default_attempt_cap > 0 && tuning.realtime_default_attempt_cap < tuning.realtime_min_attempt_cap) {
                tuning.realtime_default_attempt_cap = tuning.realtime_min_attempt_cap;
            }
            if (tuning.offline_default_time_budget_ms > 0.0 && tuning.offline_default_time_budget_ms < tuning.offline_min_time_budget_ms) {
                tuning.offline_default_time_budget_ms = tuning.offline_min_time_budget_ms;
            }
            if (tuning.realtime_default_time_budget_ms > 0.0 && tuning.realtime_default_time_budget_ms < tuning.realtime_min_time_budget_ms) {
                tuning.realtime_default_time_budget_ms = tuning.realtime_min_time_budget_ms;
            }

            if (loaded_path) *loaded_path = path.string();
            return tuning;
        } catch (const std::exception& e) {
            spdlog::warn("Failed to parse candidate tuning config {}: {}", path.string(), e.what());
        }
    }

    return tuning;
}

struct PathMetrics {
    double latency_ms = 0.0;
    double reliability = 1.0;
    double bottleneck_bandwidth_gbps = std::numeric_limits<double>::infinity();
    int hops = 0;
    bool feasible = true;
};

struct SearchTree {
    std::vector<double> latency;
    std::vector<double> reliability;
    std::vector<int> hops;
    std::vector<int64_t> prev;
};

int compute_hop_cap(size_t remaining_vnfs, int accumulated_hops = 0) {
    const int used_hops = std::max(0, accumulated_hops);
    const int remaining_legs = std::max(1, static_cast<int>(remaining_vnfs) + 1); // include final leg to destination
    const int remaining_budget = std::max(kMinLegHopCap, kTargetDeploymentHops - used_hops);
    const int per_leg_budget = remaining_budget / remaining_legs;
    const int adaptive_cap = per_leg_budget + 2;
    return std::max(kMinLegHopCap, std::min(kMaxLegHopCap, adaptive_cap));
}

int compute_relaxed_hop_cap(int hop_cap) {
    if (hop_cap <= 0) return kMaxLegHopCap + 6;
    return std::min(hop_cap + 5, kMaxLegHopCap + 6);
}

std::string make_local_path_cache_key(const std::string& node_id, int hop_cap) {
    return node_id + '\n' + std::to_string(hop_cap);
}

double soften_path_reliability(double raw_reliability, int hops) {
    if (hops <= 0) {
        return 1.0;
    }
    const double raw = clamp01(std::max(1e-9, raw_reliability));
    const double geometric_mean = std::pow(raw, 1.0 / static_cast<double>(std::max(1, hops)));
    const double softened_product = std::pow(raw, kPathSofteningExponent);
    const double blend = (hops <= 6)
        ? (0.7 * softened_product + 0.3 * geometric_mean)
        : (0.82 * softened_product + 0.18 * geometric_mean);
    const int excess_hops = std::max(0, hops - 6);
    return clamp01(blend * std::pow(kExcessHopReliabilityPenalty, static_cast<double>(excess_hops)));
}

double effective_reliability_target(double request_min_reliability, int estimated_hops) {
    const double base = clamp01(std::max(0.45, request_min_reliability));
    if (estimated_hops <= 0) return base;
    // For practical long paths (<=50 hops), relax reliability target to
    // avoid over-pruning while still keeping reliability as a hard factor.
    if (estimated_hops <= kTargetDeploymentHops) {
        const int extra = std::max(0, estimated_hops - 6);
        const double relax = std::max(0.58, 1.0 - 0.0085 * static_cast<double>(extra));
        return std::max(0.45, std::min(base, base * relax));
    }
    return std::max(0.45, base * 0.58);
}

int estimate_total_hops(
    int accumulated_hops,
    int next_leg_hops,
    size_t remaining_steps
) {
    const int optimistic_future = static_cast<int>(remaining_steps) * 3;
    return std::max(0, accumulated_hops) + std::max(0, next_leg_hops) + optimistic_future;
}

PathMetrics evaluate_path_links(
    const std::vector<DeploymentCandidate::LinkDetail>& path_links,
    double required_bandwidth_gbps
) {
    PathMetrics metrics;
    for (const auto& ld : path_links) {
        metrics.latency_ms += ld.latency_ms;
        metrics.reliability *= std::max(1e-9, ld.reliability);
        metrics.hops += 1;
        metrics.bottleneck_bandwidth_gbps =
            std::min(metrics.bottleneck_bandwidth_gbps, ld.bandwidth_available_gbps);

        if (ld.status == "down" ||
            ld.bandwidth_available_gbps + 1e-9 < required_bandwidth_gbps) {
            metrics.feasible = false;
            return metrics;
        }
    }

    if (path_links.empty()) {
        metrics.bottleneck_bandwidth_gbps = std::numeric_limits<double>::infinity();
    } else {
        metrics.reliability = soften_path_reliability(metrics.reliability, metrics.hops);
    }
    return metrics;
}

struct TopologyIndexCache {
    const void* nodes_ptr = nullptr;
    const void* links_ptr = nullptr;
    size_t node_count = 0;
    size_t link_count = 0;
    uint64_t signature = 0;

    std::unordered_map<std::string, size_t> node_index;
    std::vector<std::string> index_to_node;
    std::vector<std::vector<std::pair<size_t, double>>> adjacency;
    std::unordered_map<std::string, size_t> directed_link_index;
    std::unordered_map<std::string, std::vector<std::string>> shortest_path_cache;
    std::unordered_map<size_t, std::vector<double>> sssp_cache;
    std::unordered_map<std::string, SearchTree> constrained_tree_cache;
};

uint64_t hash_combine_u64(uint64_t h, uint64_t v) {
    return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}

uint64_t hash_string_u64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t topology_signature(const Topology& topology) {
    uint64_t h = 1469598103934665603ULL;
    h = hash_combine_u64(h, static_cast<uint64_t>(topology.nodes.size()));
    h = hash_combine_u64(h, static_cast<uint64_t>(topology.links.size()));
    h = hash_combine_u64(h, static_cast<uint64_t>(topology.metadata.total_sats));
    h = hash_combine_u64(h, static_cast<uint64_t>(topology.metadata.num_planes));

    if (!topology.nodes.empty()) {
        const auto& first = topology.nodes.front();
        const auto& last = topology.nodes.back();
        h = hash_combine_u64(h, hash_string_u64(first.id));
        h = hash_combine_u64(h, hash_string_u64(last.id));
        h = hash_combine_u64(h, static_cast<uint64_t>(std::llround(first.coordinates.x)));
        h = hash_combine_u64(h, static_cast<uint64_t>(std::llround(last.coordinates.y)));
    }
    if (!topology.links.empty()) {
        const auto& first = topology.links.front();
        const auto& last = topology.links.back();
        h = hash_combine_u64(h, hash_string_u64(first.source));
        h = hash_combine_u64(h, hash_string_u64(first.target));
        h = hash_combine_u64(h, hash_string_u64(last.source));
        h = hash_combine_u64(h, hash_string_u64(last.target));
    }
    return h;
}

TopologyIndexCache& get_topology_cache(const Topology& topology) {
    static thread_local TopologyIndexCache cache;

    const void* nodes_ptr = topology.nodes.empty() ? nullptr : static_cast<const void*>(topology.nodes.data());
    const void* links_ptr = topology.links.empty() ? nullptr : static_cast<const void*>(topology.links.data());
    const uint64_t sig = topology_signature(topology);

    const bool rebuild =
        cache.nodes_ptr != nodes_ptr ||
        cache.links_ptr != links_ptr ||
        cache.node_count != topology.nodes.size() ||
        cache.link_count != topology.links.size() ||
        cache.signature != sig;

    if (!rebuild) {
        return cache;
    }

    cache = TopologyIndexCache{};
    cache.nodes_ptr = nodes_ptr;
    cache.links_ptr = links_ptr;
    cache.node_count = topology.nodes.size();
    cache.link_count = topology.links.size();
    cache.signature = sig;

    cache.node_index.reserve(topology.nodes.size() * 2);
    cache.index_to_node.reserve(topology.nodes.size());
    cache.adjacency.assign(topology.nodes.size(), {});

    for (size_t i = 0; i < topology.nodes.size(); ++i) {
        cache.node_index[topology.nodes[i].id] = i;
        cache.index_to_node.push_back(topology.nodes[i].id);
    }

    cache.directed_link_index.reserve(topology.links.size() * 2);
    for (size_t i = 0; i < topology.links.size(); ++i) {
        const auto& link = topology.links[i];
        if (link_is_down(link) || link.bandwidth_available_gbps <= 1e-9) {
            continue;
        }
        auto src_it = cache.node_index.find(link.source);
        auto dst_it = cache.node_index.find(link.target);
        if (src_it == cache.node_index.end() || dst_it == cache.node_index.end()) {
            continue;
        }

        const size_t src_idx = src_it->second;
        const size_t dst_idx = dst_it->second;
        cache.adjacency[src_idx].emplace_back(dst_idx, link.latency_ms);
        cache.adjacency[dst_idx].emplace_back(src_idx, link.latency_ms);

        cache.directed_link_index[make_pair_key(link.source, link.target)] = i;
        cache.directed_link_index[make_pair_key(link.target, link.source)] = i;
    }

    return cache;
}

const std::vector<double>& get_shortest_distances_from(const Topology& topology, size_t src_idx) {
    auto& cache = get_topology_cache(topology);
    auto it = cache.sssp_cache.find(src_idx);
    if (it != cache.sssp_cache.end()) return it->second;

    const size_t n = topology.nodes.size();
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    std::vector<uint8_t> visited(n, 0);
    dist[src_idx] = 0.0;

    using PQ = std::priority_queue<
        std::pair<double, size_t>,
        std::vector<std::pair<double, size_t>>,
        std::greater<std::pair<double, size_t>>
    >;
    PQ pq;
    pq.emplace(0.0, src_idx);

    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (visited[u]) continue;
        visited[u] = 1;

        for (const auto& [v, w] : cache.adjacency[u]) {
            const double nd = d + w;
            if (nd < dist[v]) {
                dist[v] = nd;
                pq.emplace(nd, v);
            }
        }
    }

    auto [inserted_it, _] = cache.sssp_cache.emplace(src_idx, std::move(dist));
    return inserted_it->second;
}

double estimate_link_reliability_inline(const Link& link) {
    if (link_is_down(link)) return 0.0;
    const double bw_ratio = link.bandwidth_gbps > 0.0
        ? clamp01(link.bandwidth_available_gbps / link.bandwidth_gbps)
        : 0.0;
    const double base = clamp01(link.reliability);
    // Keep residual-bandwidth influence mild: as long as the link still has usable headroom,
    // reliability should not collapse on moderately loaded long-but-feasible paths.
    const double bandwidth_factor = 0.992 + 0.008 * bw_ratio;
    const double status_penalty = (link.status == "congested") ? 0.992 : 1.0;
    return clamp01(base * bandwidth_factor * status_penalty);
}

std::string make_tree_key(const std::string& src, double bw_required, int hop_cap) {
    const int bw_bucket = static_cast<int>(std::ceil(std::max(0.0, bw_required) * 4.0));
    return src + '\n' + std::to_string(bw_bucket) + '\n' + std::to_string(hop_cap);
}

const SearchTree& get_constrained_tree(
    const Topology& topology,
    const std::string& src,
    double bw_required,
    int hop_cap
) {
    auto& cache = get_topology_cache(topology);
    const std::string key = make_tree_key(src, bw_required, hop_cap);
    const auto cached = cache.constrained_tree_cache.find(key);
    if (cached != cache.constrained_tree_cache.end()) {
        return cached->second;
    }

    SearchTree tree;
    const size_t n = topology.nodes.size();
    tree.latency.assign(n, std::numeric_limits<double>::infinity());
    tree.reliability.assign(n, 0.0);
    tree.hops.assign(n, std::numeric_limits<int>::max());
    tree.prev.assign(n, -1);

    const auto src_it = cache.node_index.find(src);
    if (src_it == cache.node_index.end()) {
        auto [inserted, _] = cache.constrained_tree_cache.emplace(key, std::move(tree));
        return inserted->second;
    }

    const size_t src_idx = src_it->second;
    std::vector<double> cost(n, std::numeric_limits<double>::infinity());
    using PQ = std::priority_queue<
        std::pair<double, size_t>,
        std::vector<std::pair<double, size_t>>,
        std::greater<std::pair<double, size_t>>
    >;
    PQ pq;
    cost[src_idx] = 0.0;
    tree.latency[src_idx] = 0.0;
    tree.reliability[src_idx] = 1.0;
    tree.hops[src_idx] = 0;
    pq.emplace(0.0, src_idx);

    while (!pq.empty()) {
        const auto [cur_cost, u] = pq.top();
        pq.pop();
        if (cur_cost > cost[u] + 1e-9) continue;

        const std::string& current = cache.index_to_node[u];
        for (const auto& [v, _] : cache.adjacency[u]) {
            const std::string& next = cache.index_to_node[v];
            const auto link_it = cache.directed_link_index.find(make_pair_key(current, next));
            if (link_it == cache.directed_link_index.end()) continue;
            const Link& link = topology.links[link_it->second];
            if (link_is_down(link) || link.bandwidth_available_gbps + 1e-9 < bw_required) continue;

            const int next_hops = tree.hops[u] + 1;
            if (hop_cap > 0 && next_hops > hop_cap) continue;

            const double next_latency = tree.latency[u] + link.latency_ms;
            const double next_cost = next_latency + kHopPenaltyMs * static_cast<double>(next_hops);
            const double next_rel = tree.reliability[u] * estimate_link_reliability_inline(link);

            if (next_cost + 1e-9 < cost[v] ||
                (std::abs(next_cost - cost[v]) <= 1e-9 &&
                 (next_latency + 1e-9 < tree.latency[v] ||
                  (std::abs(next_latency - tree.latency[v]) <= 1e-9 &&
                   next_rel > tree.reliability[v] + 1e-9)))) {
                cost[v] = next_cost;
                tree.latency[v] = next_latency;
                tree.reliability[v] = next_rel;
                tree.hops[v] = next_hops;
                tree.prev[v] = static_cast<int64_t>(u);
                pq.emplace(next_cost, v);
            }
        }
    }

    auto [inserted, _] = cache.constrained_tree_cache.emplace(key, std::move(tree));
    return inserted->second;
}

} // namespace

InferenceEngine::InferenceEngine(
    const std::string& gnn_model_path,
    const std::string& actor_model_path,
    int num_threads
) : env_(ORT_LOGGING_LEVEL_WARNING, "sfc"),
    memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    spdlog::info("Initializing InferenceEngine");
    candidate_tuning_ = load_candidate_search_tuning(actor_model_path, &candidate_tuning_config_path_);
    if (!candidate_tuning_config_path_.empty()) {
        spdlog::info("Loaded candidate tuning config: {}", candidate_tuning_config_path_);
    } else {
        spdlog::warn("Candidate tuning config not found, using built-in defaults");
    }
    spdlog::info(
        "Candidate tuning: offline[target_min={} x{} cap={} min_attempt={} default_attempt={} min_budget_ms={} default_budget_ms={} min_return_topk={}], "
        "realtime[target_min={} x{} cap={} min_attempt={} default_attempt={} min_budget_ms={} default_budget_ms={} min_return_topk={}]",
        candidate_tuning_.offline_target_min,
        candidate_tuning_.offline_target_multiplier,
        candidate_tuning_.offline_target_cap,
        candidate_tuning_.offline_min_attempt_cap,
        candidate_tuning_.offline_default_attempt_cap,
        candidate_tuning_.offline_min_time_budget_ms,
        candidate_tuning_.offline_default_time_budget_ms,
        candidate_tuning_.offline_return_topk_floor,
        candidate_tuning_.realtime_target_min,
        candidate_tuning_.realtime_target_multiplier,
        candidate_tuning_.realtime_target_cap,
        candidate_tuning_.realtime_min_attempt_cap,
        candidate_tuning_.realtime_default_attempt_cap,
        candidate_tuning_.realtime_min_time_budget_ms,
        candidate_tuning_.realtime_default_time_budget_ms,
        candidate_tuning_.realtime_return_topk_floor
    );
    
    session_options_.SetIntraOpNumThreads(num_threads);
    session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    
    try {
        gnn_session_ = std::make_unique<Ort::Session>(env_, gnn_model_path.c_str(), session_options_);
        actor_session_ = std::make_unique<Ort::Session>(env_, actor_model_path.c_str(), session_options_);

        node_feature_dim_ = kBaseNodeFeatureDim;
        vnf_feature_dim_ = kBaseVnfFeatureDim;
        context_feature_dim_ = kBaseContextFeatureDim;
        node_embedding_dim_ = kDefaultNodeEmbeddingDim;
        maybe_load_model_io_meta(
            actor_model_path,
            &node_feature_dim_,
            &vnf_feature_dim_,
            &context_feature_dim_,
            &node_embedding_dim_
        );

        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < gnn_session_->GetInputCount(); ++i) {
            const auto name_alloc = gnn_session_->GetInputNameAllocated(i, allocator);
            const std::string name = name_alloc ? name_alloc.get() : "";
            if (name != "node_features") continue;
            const auto shape = gnn_session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            node_feature_dim_ = positive_last_dim_or(shape, node_feature_dim_);
        }
        for (size_t i = 0; i < actor_session_->GetInputCount(); ++i) {
            const auto name_alloc = actor_session_->GetInputNameAllocated(i, allocator);
            const std::string name = name_alloc ? name_alloc.get() : "";
            const auto shape = actor_session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            if (name == "node_embeddings") {
                node_embedding_dim_ = positive_last_dim_or(shape, node_embedding_dim_);
            } else if (name == "vnf_features") {
                vnf_feature_dim_ = positive_last_dim_or(shape, vnf_feature_dim_);
            } else if (name == "context_features") {
                context_feature_dim_ = positive_last_dim_or(shape, context_feature_dim_);
            }
        }

        spdlog::info(
            "ONNX model dims: node_features={} vnf_features={} context_features={} node_embedding={}",
            node_feature_dim_,
            vnf_feature_dim_,
            context_feature_dim_,
            node_embedding_dim_
        );
        spdlog::info("ONNX models loaded successfully");
    } catch (const Ort::Exception& e) {
        spdlog::error("Failed to load ONNX models: {}", e.what());
        throw;
    }
}

InferenceEngine::~InferenceEngine() {}

std::vector<DeploymentCandidate> InferenceEngine::inference(
    const SFCRequest& request,
    const Topology& topology,
    nlohmann::json* decision_trace
) {
    try {
        return generate_gha_drl_candidates(request, topology, decision_trace);
    } catch (const std::exception& e) {
        spdlog::error("Inference failed: {}", e.what());
        return {};
    }
}

// 🔥 性能优化：Dijkstra最短路径算法（带路径缓存）
std::vector<std::string> InferenceEngine::find_shortest_path(
    const std::string& src,
    const std::string& dst,
    const Topology& topology,
    double required_bandwidth_gbps,
    int max_hops,
    bool log_missing
) {
    if (src == dst) {
        return {src};
    }

    auto& cache = get_topology_cache(topology);

    const auto src_it = cache.node_index.find(src);
    const auto dst_it = cache.node_index.find(dst);
    if (src_it == cache.node_index.end() || dst_it == cache.node_index.end()) {
        if (log_missing) {
            spdlog::warn("Source or destination node not in graph: {} -> {}", src, dst);
        } else {
            spdlog::debug("Source or destination node not in graph: {} -> {}", src, dst);
        }
        return {};
    }

    const bool use_cache = required_bandwidth_gbps <= 1e-9 && max_hops < 0;
    const std::string cache_key = make_pair_key(src, dst);
    if (use_cache) {
        const auto cache_hit = cache.shortest_path_cache.find(cache_key);
        if (cache_hit != cache.shortest_path_cache.end()) {
            return cache_hit->second;
        }
    }

    if (!use_cache) {
        const auto& tree = get_constrained_tree(topology, src, required_bandwidth_gbps, max_hops);
        const size_t src_idx = src_it->second;
        const size_t dst_idx = dst_it->second;
        if (dst_idx >= tree.prev.size() || !std::isfinite(tree.latency[dst_idx])) {
            if (log_missing) {
                spdlog::warn("No constrained path found from {} to {}", src, dst);
            } else {
                spdlog::debug("No constrained path found from {} to {}", src, dst);
            }
            return {};
        }

        std::vector<std::string> path;
        for (int64_t cur = static_cast<int64_t>(dst_idx); cur >= 0; cur = tree.prev[static_cast<size_t>(cur)]) {
            path.push_back(cache.index_to_node[static_cast<size_t>(cur)]);
            if (static_cast<size_t>(cur) == src_idx) break;
        }
        if (path.empty() || path.back() != src) {
            if (log_missing) {
                spdlog::warn("Constrained path reconstruction failed from {} to {}", src, dst);
            } else {
                spdlog::debug("Constrained path reconstruction failed from {} to {}", src, dst);
            }
            return {};
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    const size_t src_idx = src_it->second;
    const size_t dst_idx = dst_it->second;
    std::vector<double> dist(topology.nodes.size(), std::numeric_limits<double>::infinity());
    std::vector<double> pure_latency(topology.nodes.size(), std::numeric_limits<double>::infinity());
    std::vector<int64_t> prev(topology.nodes.size(), -1);

    using PQ = std::priority_queue<
        std::pair<double, size_t>,
        std::vector<std::pair<double, size_t>>,
        std::greater<std::pair<double, size_t>>
    >;
    PQ pq;
    dist[src_idx] = 0.0;
    pure_latency[src_idx] = 0.0;
    pq.emplace(0.0, src_idx);

    while (!pq.empty()) {
        const auto [cur_cost, u] = pq.top();
        pq.pop();
        if (cur_cost > dist[u] + 1e-9) continue;
        if (u == dst_idx) break;

        const std::string& current_node_id = cache.index_to_node[u];
        for (const auto& [v, _] : cache.adjacency[u]) {
            const std::string& next_node_id = cache.index_to_node[v];
            const Link* link = find_link(current_node_id, next_node_id, topology);
            if (!link || link_is_down(*link)) continue;
            if (required_bandwidth_gbps > 1e-9 &&
                link->bandwidth_available_gbps + 1e-9 < required_bandwidth_gbps) {
                continue;
            }

            const double next_latency = pure_latency[u] + link->latency_ms;
            const double next_cost = cur_cost + link->latency_ms + kHopPenaltyMs;
            if (next_cost + 1e-9 < dist[v] ||
                (std::abs(next_cost - dist[v]) <= 1e-9 &&
                 next_latency + 1e-9 < pure_latency[v])) {
                dist[v] = next_cost;
                pure_latency[v] = next_latency;
                prev[v] = static_cast<int64_t>(u);
                pq.emplace(next_cost, v);
            }
        }
    }

    if (!std::isfinite(dist[dst_idx])) {
        if (log_missing) {
            spdlog::warn("No path found from {} to {}", src, dst);
        } else {
            spdlog::debug("No path found from {} to {}", src, dst);
        }
        if (use_cache) {
            cache.shortest_path_cache.emplace(cache_key, std::vector<std::string>{});
        }
        return {};
    }

    std::vector<std::string> path;
    for (int64_t cur = static_cast<int64_t>(dst_idx); cur >= 0; cur = prev[static_cast<size_t>(cur)]) {
        path.push_back(cache.index_to_node[static_cast<size_t>(cur)]);
        if (static_cast<size_t>(cur) == src_idx) break;
    }
    if (path.empty() || path.back() != src) {
        if (log_missing) {
            spdlog::warn("Path reconstruction failed from {} to {}", src, dst);
        } else {
            spdlog::debug("Path reconstruction failed from {} to {}", src, dst);
        }
        return {};
    }
    std::reverse(path.begin(), path.end());

    if (max_hops > 0 && static_cast<int>(path.size()) - 1 > max_hops) {
        if (log_missing) {
            spdlog::warn("Path {} -> {} exceeds hop cap {}", src, dst, max_hops);
        } else {
            spdlog::debug("Path {} -> {} exceeds hop cap {}", src, dst, max_hops);
        }
        return {};
    }

    for (size_t i = 0; i + 1 < path.size(); ++i) {
        if (cache.directed_link_index.find(make_pair_key(path[i], path[i + 1])) == cache.directed_link_index.end()) {
            spdlog::error("Path discontinuity between {} and {}", path[i], path[i + 1]);
            return {};
        }
    }

    if (use_cache) {
        cache.shortest_path_cache.emplace(cache_key, path);
    }

    return path;
}

std::vector<DeploymentCandidate::LinkDetail> InferenceEngine::generate_path_links(
    const std::vector<std::string>& path,
    const Topology& topology,
    double bandwidth_required_gbps
) {
    std::vector<DeploymentCandidate::LinkDetail> link_details;
    
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const std::string& src = path[i];
        const std::string& dst = path[i + 1];
        
        const Link* link = find_link(src, dst, topology);
        if (link) {
            DeploymentCandidate::LinkDetail ld;
            ld.src = src;
            ld.dst = dst;
            ld.latency_ms = link->latency_ms;
            ld.bandwidth_gbps = link->bandwidth_gbps;
            ld.bandwidth_available_gbps = link->bandwidth_available_gbps;
            ld.bandwidth_required_gbps = bandwidth_required_gbps;
            ld.status = link->status;
            ld.reliability = estimate_link_reliability(*link);
            link_details.push_back(ld);
        } else {
            spdlog::error("Link not found in generate_path_links: {} -> {}", src, dst);
        }
    }
    
    return link_details;
}

const Link* InferenceEngine::find_link(
    const std::string& src,
    const std::string& dst,
    const Topology& topology
) const {
    auto& cache = get_topology_cache(topology);
    const auto it = cache.directed_link_index.find(make_pair_key(src, dst));
    if (it != cache.directed_link_index.end()) {
        return &topology.links[it->second];
    }
    return nullptr;
}

double InferenceEngine::estimate_node_reliability(
    const std::string& node_id,
    const Topology& topology
) const {
    auto& cache = get_topology_cache(topology);
    const auto it = cache.node_index.find(node_id);
    if (it == cache.node_index.end()) return 0.985;
    const auto& node = topology.nodes[it->second];
    const double cpu_ratio = node.cpu_total > 0.0 ? node.cpu_available / node.cpu_total : 0.0;
    const double mem_ratio = node.mem_total > 0.0 ? node.mem_available / node.mem_total : 0.0;
    const double disk_ratio = node.disk_total > 0.0 ? node.disk_available / node.disk_total : 0.0;
    const double health = clamp01((cpu_ratio + mem_ratio + disk_ratio) / 3.0);
    const double base = clamp01(node.node_reliability);
    const double rel = clamp01(base * (0.96 + 0.04 * health));
    return std::max(0.90, rel);
}

double InferenceEngine::estimate_link_reliability(const Link& link) const {
    return estimate_link_reliability_inline(link);
}

std::vector<DeploymentCandidate> InferenceEngine::generate_gha_drl_candidates(
    const SFCRequest& request,
    const Topology& topology,
    nlohmann::json* decision_trace
) {
    spdlog::info("Using GHA-DRL algorithm for SFC planning");
    
    // 🔥 性能优化：检查拓扑是否为空
    if (topology.nodes.empty()) {
        spdlog::error("Empty topology - no nodes available");
        return {};
    }
    
    if (decision_trace) {
        *decision_trace = {
            {"algorithm", "GHA-DRL"},
            {"request_id", request.request_id},
            {"source_node", request.source_node},
            {"destination_node", request.destination_node},
            {"requested_topk", request.topk},
            {"topology_version", request.topology_version},
            {"sim_time", request.sim_time},
            {"steps", nlohmann::json::array()}
        };
    }

    const auto [node_features, edge_index] = prepare_graph_inputs(topology);
    const auto node_embeddings = run_gnn_encoder(node_features, edge_index, topology.nodes.size());
    if (node_embeddings.empty()) {
        spdlog::error("Failed to encode topology with GNN");
        return {};
    }

    std::unordered_map<std::string, int64_t> node_id_to_idx;
    node_id_to_idx.reserve(topology.nodes.size() * 2);
    for (size_t i = 0; i < topology.nodes.size(); ++i) {
        node_id_to_idx[topology.nodes[i].id] = static_cast<int64_t>(i);
    }

    const bool realtime_mode = request.realtime_mode;
    const int req_topk = std::max(1, request.topk);
    const int target_feasible_count = realtime_mode
        ? std::min(
            std::max(
                candidate_tuning_.realtime_target_min,
                req_topk * candidate_tuning_.realtime_target_multiplier
            ),
            candidate_tuning_.realtime_target_cap
          )
        : std::min(
            std::max(
                candidate_tuning_.offline_target_min,
                req_topk * candidate_tuning_.offline_target_multiplier
            ),
            candidate_tuning_.offline_target_cap
          );

    int hard_attempt_cap = request.max_planning_attempts > 0
        ? request.max_planning_attempts
        : (realtime_mode ? candidate_tuning_.realtime_default_attempt_cap : candidate_tuning_.offline_default_attempt_cap);
    const int min_attempt_floor = realtime_mode
        ? candidate_tuning_.realtime_min_attempt_cap
        : candidate_tuning_.offline_min_attempt_cap;
    if (hard_attempt_cap > 0) {
        hard_attempt_cap = std::max(hard_attempt_cap, min_attempt_floor);
    } else if (min_attempt_floor > 0) {
        hard_attempt_cap = min_attempt_floor;
    }

    double planning_time_budget_ms = request.planning_time_budget_ms > 0.0
        ? request.planning_time_budget_ms
        : (realtime_mode ? candidate_tuning_.realtime_default_time_budget_ms : candidate_tuning_.offline_default_time_budget_ms);
    const double min_budget_floor = realtime_mode
        ? candidate_tuning_.realtime_min_time_budget_ms
        : candidate_tuning_.offline_min_time_budget_ms;
    if (planning_time_budget_ms > 0.0) {
        planning_time_budget_ms = std::max(planning_time_budget_ms, min_budget_floor);
    } else if (min_budget_floor > 0.0) {
        planning_time_budget_ms = min_budget_floor;
    }
    const auto search_start = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() -> double {
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - search_start
            ).count()
        ) / 1000.0;
    };
    auto budget_exceeded = [&]() -> bool {
        return planning_time_budget_ms > 0.0 && elapsed_ms() >= planning_time_budget_ms;
    };

    std::vector<DeploymentCandidate> feasible_candidates;
    feasible_candidates.reserve(std::max(1, target_feasible_count));
    std::vector<DeploymentCandidate> fallback_candidates;
    fallback_candidates.reserve(std::max(1, target_feasible_count * 3));
    std::unordered_set<std::string> seen_signatures;
    nlohmann::json trace_steps = nlohmann::json::array();
    const size_t kMaxTraceAttempts = static_cast<size_t>(
        std::max(0, realtime_mode ? candidate_tuning_.trace_attempts_realtime : candidate_tuning_.trace_attempts_offline)
    );
    int attempts_done = 0;
    bool stop_search = false;

    std::vector<double> relax_levels = candidate_tuning_.reliability_relax_levels;
    if (relax_levels.empty()) {
        relax_levels = {1.0};
    }
    if (request.constraints.min_reliability <= candidate_tuning_.relax_disable_min_reliability) {
        relax_levels = {1.0};
    }

    for (size_t relax_idx = 0; relax_idx < relax_levels.size(); ++relax_idx) {
        const double relax_factor = relax_levels[relax_idx];
        SFCRequest planning_request = request;
        planning_request.constraints.min_reliability = std::max(
            candidate_tuning_.relax_min_reliability_floor,
            std::min(request.constraints.min_reliability, request.constraints.min_reliability * relax_factor)
        );
        if (relax_factor < 0.999) {
            spdlog::warn(
                "No enough strict candidates yet, retry with reliability factor {:.2f} (target {:.4f} -> {:.4f})",
                relax_factor,
                request.constraints.min_reliability,
                planning_request.constraints.min_reliability
            );
        }

        const bool strict_phase = relax_idx == 0;
        int level_attempts = strict_phase
            ? std::max(
                target_feasible_count * candidate_tuning_.strict_attempt_per_target,
                candidate_tuning_.strict_attempt_base
              )
            : std::max(
                target_feasible_count * candidate_tuning_.relaxed_attempt_per_target,
                candidate_tuning_.relaxed_attempt_base
              );
        if (hard_attempt_cap > 0) {
            const int remaining_attempts = hard_attempt_cap - attempts_done;
            if (remaining_attempts <= 0) {
                stop_search = true;
                break;
            }
            level_attempts = std::min(level_attempts, remaining_attempts);
        }
        for (int k = 0; k < level_attempts; ++k) {
            if (budget_exceeded()) {
                stop_search = true;
                break;
            }
            if (hard_attempt_cap > 0 && attempts_done >= hard_attempt_cap) {
                stop_search = true;
                break;
            }
            attempts_done += 1;
            nlohmann::json single_trace;
            DeploymentCandidate candidate = generate_single_deployment(
                planning_request,
                topology,
                static_cast<int>(relax_idx) * level_attempts + k,
                node_embeddings,
                node_id_to_idx,
                decision_trace ? &single_trace : nullptr
            );
            if (candidate.deployed_nodes.empty()) continue;

            if (decision_trace && trace_steps.size() < kMaxTraceAttempts) {
                trace_steps.push_back({
                    {"attempt_id", static_cast<int>(relax_idx) * level_attempts + k},
                    {"relax_factor", relax_factor},
                    {"satisfies_constraints", candidate.satisfies_constraints},
                    {"score", candidate.score},
                    {"latency_ms", candidate.total_latency_ms},
                    {"estimated_reliability", candidate.estimated_reliability},
                    {"bottleneck_bandwidth_gbps", candidate.bottleneck_bandwidth_gbps},
                    {"reason", candidate.reason},
                    {"deployment_nodes", candidate.deployed_nodes},
                    {"decision_process", single_trace}
                });
            }

            const std::string sig = build_candidate_signature(candidate);
            if (!seen_signatures.insert(sig).second) continue;

            if (candidate.satisfies_constraints) {
                if (relax_factor < 0.999 && candidate.reason.empty()) {
                    candidate.reason = "Reliability target relaxed for fallback planning";
                }
                feasible_candidates.push_back(std::move(candidate));
            } else {
                fallback_candidates.push_back(std::move(candidate));
            }

            if (static_cast<int>(feasible_candidates.size()) >= target_feasible_count) {
                break;
            }
        }

        if (stop_search || static_cast<int>(feasible_candidates.size()) >= target_feasible_count) {
            break;
        }
    }

    if (!stop_search && static_cast<int>(feasible_candidates.size()) < target_feasible_count) {
        int extra_attempts = std::max(
            target_feasible_count * candidate_tuning_.extra_attempt_per_target,
            candidate_tuning_.extra_attempt_base
        );
        const int seed_base = static_cast<int>(relax_levels.size()) * 1000;
        if (hard_attempt_cap > 0) {
            extra_attempts = std::min(extra_attempts, std::max(0, hard_attempt_cap - attempts_done));
        }
        spdlog::warn(
            "Feasible candidates still below target ({} < {}), running diversification fallback",
            feasible_candidates.size(),
            target_feasible_count
        );
        for (int extra = 0; extra < extra_attempts; ++extra) {
            if (budget_exceeded()) break;
            if (hard_attempt_cap > 0 && attempts_done >= hard_attempt_cap) break;
            attempts_done += 1;
            nlohmann::json single_trace;
            DeploymentCandidate candidate = generate_single_deployment(
                request,
                topology,
                seed_base + extra,
                node_embeddings,
                node_id_to_idx,
                decision_trace ? &single_trace : nullptr
            );
            if (candidate.deployed_nodes.empty()) continue;

            if (decision_trace && trace_steps.size() < kMaxTraceAttempts) {
                trace_steps.push_back({
                    {"attempt_id", seed_base + extra},
                    {"relax_factor", 1.0},
                    {"satisfies_constraints", candidate.satisfies_constraints},
                    {"score", candidate.score},
                    {"latency_ms", candidate.total_latency_ms},
                    {"estimated_reliability", candidate.estimated_reliability},
                    {"bottleneck_bandwidth_gbps", candidate.bottleneck_bandwidth_gbps},
                    {"reason", candidate.reason},
                    {"deployment_nodes", candidate.deployed_nodes},
                    {"decision_process", single_trace}
                });
            }

            const std::string sig = build_candidate_signature(candidate);
            if (!seen_signatures.insert(sig).second) continue;

            if (candidate.satisfies_constraints) {
                feasible_candidates.push_back(std::move(candidate));
            } else {
                fallback_candidates.push_back(std::move(candidate));
            }

            if (static_cast<int>(feasible_candidates.size()) >= target_feasible_count) {
                break;
            }
        }
    }

    std::sort(feasible_candidates.begin(), feasible_candidates.end(), [](const DeploymentCandidate& a, const DeploymentCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.total_latency_ms < b.total_latency_ms;
    });
    const int min_return_topk_floor = realtime_mode
        ? candidate_tuning_.realtime_return_topk_floor
        : candidate_tuning_.offline_return_topk_floor;
    const int effective_return_topk = std::max(1, std::max(request.topk, min_return_topk_floor));
    if (static_cast<int>(feasible_candidates.size()) > effective_return_topk) {
        feasible_candidates.resize(static_cast<size_t>(effective_return_topk));
    }

    if (decision_trace) {
        (*decision_trace)["steps"] = trace_steps;
        (*decision_trace)["feasible_candidate_count"] = static_cast<int>(feasible_candidates.size());
        (*decision_trace)["fallback_candidate_count"] = static_cast<int>(fallback_candidates.size());
        (*decision_trace)["target_feasible_count"] = target_feasible_count;
        (*decision_trace)["realtime_mode"] = realtime_mode;
        (*decision_trace)["attempts_done"] = attempts_done;
        (*decision_trace)["attempt_cap"] = hard_attempt_cap;
        (*decision_trace)["planning_time_budget_ms"] = planning_time_budget_ms;
        (*decision_trace)["planning_elapsed_ms"] = elapsed_ms();
        (*decision_trace)["stopped_by_budget"] = budget_exceeded();
        (*decision_trace)["candidate_tuning_config_path"] = candidate_tuning_config_path_;
        (*decision_trace)["effective_request_topk"] = req_topk;
        (*decision_trace)["effective_return_topk"] = effective_return_topk;
        (*decision_trace)["effective_relax_levels"] = relax_levels;
        (*decision_trace)["effective_min_attempt_floor"] = min_attempt_floor;
        (*decision_trace)["effective_min_budget_floor_ms"] = min_budget_floor;
    }

    if (!feasible_candidates.empty()) {
        return feasible_candidates;
    }

    std::sort(fallback_candidates.begin(), fallback_candidates.end(), [](const DeploymentCandidate& a, const DeploymentCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.total_latency_ms < b.total_latency_ms;
    });
    if (static_cast<int>(fallback_candidates.size()) > effective_return_topk) {
        fallback_candidates.resize(static_cast<size_t>(effective_return_topk));
    }

    if (fallback_candidates.empty()) {
        spdlog::error("No feasible deployment found");
    } else {
        spdlog::warn("No SLA-feasible candidates, returning {} fallback candidates", fallback_candidates.size());
    }

    return fallback_candidates;
}

DeploymentCandidate InferenceEngine::generate_single_deployment(
    const SFCRequest& request,
    const Topology& topology,
    int seed,
    const std::vector<float>& node_embeddings,
    const std::unordered_map<std::string, int64_t>& node_id_to_idx,
    nlohmann::json* candidate_trace
) {
    DeploymentCandidate candidate;
    candidate.score = 0.9 - seed * 0.1;
    if (candidate_trace) {
        *candidate_trace = {
            {"seed", seed},
            {"status", "running"},
            {"source_node", request.source_node},
            {"destination_node", request.destination_node},
            {"vnf_count", static_cast<int>(request.vnfs.size())},
            {"core_nf_count", static_cast<int>(request.vnfs.size())},
            {"per_vnf", nlohmann::json::array()},
            {"per_core_nf", nlohmann::json::array()},
            {"final", nlohmann::json::object()}
        };
    }

    auto append_step_trace = [&](const nlohmann::json& step) {
        if (!candidate_trace) return;
        (*candidate_trace)["per_vnf"].push_back(step);
        (*candidate_trace)["per_core_nf"].push_back(step);
    };

    if (node_embeddings.empty()) {
        candidate.satisfies_constraints = false;
        candidate.reason = "Failed to encode topology with GNN";
        return candidate;
    }

    if (request.source_node.empty() || request.destination_node.empty()) {
        candidate.satisfies_constraints = false;
        candidate.reason = "source_node/destination_node is required";
        return candidate;
    }

    auto& cache = get_topology_cache(topology);
    if (cache.node_index.find(request.source_node) == cache.node_index.end() ||
        cache.node_index.find(request.destination_node) == cache.node_index.end()) {
        candidate.satisfies_constraints = false;
        candidate.reason = "source_node or destination_node not found in topology";
        return candidate;
    }

    std::string prev_node = request.source_node;
    bool has_prev_vnf = true;
    double accumulated_latency = 0.0;
    double remaining_latency = request.constraints.max_latency_ms;
    double accumulated_reliability = 1.0;
    int accumulated_hops = 0;
    double bottleneck_bandwidth = std::numeric_limits<double>::infinity();
    std::unordered_set<std::string> deployed_node_set;
    const DynamicTopologyFeatures topo_features = build_dynamic_topology_features(topology);
    spdlog::debug("Max latency: {}ms", request.constraints.max_latency_ms);

    auto finalize_failure = [&](const std::string& reason) {
        candidate.total_latency_ms = accumulated_latency;
        candidate.estimated_reliability = accumulated_reliability;
        candidate.bottleneck_bandwidth_gbps =
            std::isfinite(bottleneck_bandwidth) ? bottleneck_bandwidth : request.constraints.min_bandwidth_gbps;
        candidate.satisfies_constraints = false;
        candidate.reason = reason;
        if (candidate_trace) {
            (*candidate_trace)["status"] = "failed";
            (*candidate_trace)["failure_reason"] = reason;
            (*candidate_trace)["final"] = {
                {"total_latency_ms", candidate.total_latency_ms},
                {"estimated_reliability", candidate.estimated_reliability},
                {"bottleneck_bandwidth_gbps", candidate.bottleneck_bandwidth_gbps}
            };
        }
        return candidate;
    };

    // 迭代放置每个VNF
    for (size_t i = 0; i < request.vnfs.size(); ++i) {
        const auto& vnf = request.vnfs[i];
        nlohmann::json step_trace = {
            {"vnf_index", static_cast<int>(i)},
            {"vnf_name", vnf.name},
            {"core_nf", vnf.name},
            {"nf_type", vnf.nf_type.empty() ? vnf.name : vnf.nf_type},
            {"nf_role", vnf.nf_role},
            {"prev_node", prev_node},
            {"remaining_latency_before", remaining_latency},
            {"accumulated_latency_before", accumulated_latency},
            {"accumulated_reliability_before", accumulated_reliability}
        };
        const double required_bandwidth_gbps = std::max(
            request.constraints.min_bandwidth_gbps,
            std::max(vnf.bw_in, vnf.bw_out)
        );
        const int hop_cap = compute_hop_cap(request.vnfs.size() - i, accumulated_hops);
        const int relaxed_hop_cap = compute_relaxed_hop_cap(hop_cap);

        auto candidates_set = filter_candidate_nodes(
            request,
            vnf,
            prev_node,
            i,
            remaining_latency,
            accumulated_reliability,
            accumulated_hops,
            deployed_node_set,
            topology
        );
        step_trace["pruned_candidate_count"] = static_cast<int>(candidates_set.size());

        if (candidates_set.empty()) {
            spdlog::error("No feasible node for core NF {}: {}", i, vnf.name);
            if (candidate_trace) {
                step_trace["status"] = "failed";
                step_trace["failure_reason"] = "no_candidate_after_pruning";
                append_step_trace(step_trace);
            }
            return finalize_failure("No feasible node for core NF: " + vnf.name);
        }

        auto ranked_nodes = rank_nodes_by_cost(candidates_set, vnf, prev_node, topology);
        nlohmann::json ranked_top = nlohmann::json::array();
        for (size_t ridx = 0; ridx < std::min<size_t>(10, ranked_nodes.size()); ++ridx) {
            ranked_top.push_back({
                {"node", ranked_nodes[ridx].first},
                {"heuristic_cost", ranked_nodes[ridx].second}
            });
        }
        step_trace["ranked_candidates_top"] = ranked_top;
        const int search_target = request.realtime_mode
            ? std::max(1, std::min(4, request.topk))
            : std::max(request.topk, 10);
        const int target_actor_fanout = std::max(
            request.realtime_mode ? 8 : 16,
            std::min(
                request.realtime_mode ? kRealtimeActorCandidatePool : kMaxActorCandidatePool,
                search_target * (request.realtime_mode ? 5 : 8)
            )
        );
        const int actor_top_k = std::min(target_actor_fanout, static_cast<int>(ranked_nodes.size()));
        if (actor_top_k <= 0) {
            if (candidate_trace) {
                step_trace["status"] = "failed";
                step_trace["failure_reason"] = "no_actor_candidate";
                append_step_trace(step_trace);
            }
            return finalize_failure("No candidate node index available");
        }

        std::string selected_node;
        std::vector<DeploymentCandidate::LinkDetail> selected_path_links;
        double selected_path_latency = 0.0;
        double selected_path_reliability = 1.0;
        double selected_path_bottleneck = std::numeric_limits<double>::infinity();
        bool found_feasible_node = false;

        std::vector<std::string> candidate_node_ids;
        candidate_node_ids.reserve(actor_top_k);
        std::vector<int64_t> actor_candidate_indices;
        actor_candidate_indices.reserve(actor_top_k);
        for (int rank_idx = 0; rank_idx < actor_top_k; ++rank_idx) {
            const auto& node_id = ranked_nodes[rank_idx].first;
            auto it = node_id_to_idx.find(node_id);
            if (it == node_id_to_idx.end()) continue;
            candidate_node_ids.push_back(node_id);
            actor_candidate_indices.push_back(it->second);
        }
        if (!prev_node.empty() &&
            deployed_node_set.find(prev_node) == deployed_node_set.end()) {
            const auto cache_it = cache.node_index.find(prev_node);
            const auto embed_it = node_id_to_idx.find(prev_node);
            if (cache_it != cache.node_index.end() && embed_it != node_id_to_idx.end()) {
                const auto& prev_sat = topology.nodes[cache_it->second];
                const bool resource_ok =
                    prev_sat.cpu_available + 1e-9 >= vnf.cpu &&
                    prev_sat.mem_available + 1e-9 >= vnf.mem &&
                    prev_sat.disk_available + 1e-9 >= vnf.disk;
                const bool already_present = std::find(candidate_node_ids.begin(), candidate_node_ids.end(), prev_node) != candidate_node_ids.end();
                if (resource_ok && !already_present) {
                    candidate_node_ids.push_back(prev_node);
                    actor_candidate_indices.push_back(embed_it->second);
                }
            }
        }
        if (candidate_node_ids.empty()) {
            if (candidate_trace) {
                step_trace["status"] = "failed";
                step_trace["failure_reason"] = "candidate_embedding_index_miss";
                append_step_trace(step_trace);
            }
            return finalize_failure("Candidate nodes are not indexed in embeddings");
        }

        auto vnf_features = build_vnf_features(vnf, vnf_feature_dim_);
        auto context_features = build_context_features(
            request,
            i,
            remaining_latency,
            accumulated_latency,
            accumulated_reliability,
            topo_features.active_node_ratio,
            topo_features.active_link_ratio,
            topo_features.avg_latency_ms,
            topo_features.avg_bandwidth_utilization,
            context_feature_dim_
        );

        auto probs = run_actor_policy(node_embeddings, actor_candidate_indices, vnf_features, context_features);
        if (probs.size() != actor_candidate_indices.size()) {
            probs.assign(actor_candidate_indices.size(), 0.0f);
            if (!probs.empty()) {
                probs[static_cast<size_t>(seed) % probs.size()] = 1.0f;
            }
        }

        nlohmann::json actor_top = nlohmann::json::array();
        for (size_t aidx = 0; aidx < std::min<size_t>(10, candidate_node_ids.size()); ++aidx) {
            actor_top.push_back({
                {"node", candidate_node_ids[aidx]},
                {"actor_prob", aidx < probs.size() ? probs[aidx] : 0.0}
            });
        }
        step_trace["actor_candidates"] = actor_top;

        std::vector<size_t> probe_order(probs.size());
        std::iota(probe_order.begin(), probe_order.end(), 0);
        std::vector<double> probe_scores(probs.size(), 0.0);
        for (size_t idx = 0; idx < probe_order.size(); ++idx) {
            const double actor_score = idx < probs.size() ? probs[idx] : 0.0;
            const double heuristic_bonus = 1.0 - (static_cast<double>(idx) / std::max<size_t>(1, probe_order.size()));
            const double prev_node_bonus = candidate_node_ids[idx] == prev_node ? 0.05 : 0.0;
            probe_scores[idx] = actor_score + 0.12 * heuristic_bonus + prev_node_bonus;
        }
        std::stable_sort(probe_order.begin(), probe_order.end(), [&](size_t a, size_t b) {
            return probe_scores[a] > probe_scores[b];
        });
        nlohmann::json actor_rank_top = nlohmann::json::array();
        for (size_t pidx = 0; pidx < std::min<size_t>(10, probe_order.size()); ++pidx) {
            const size_t probe_idx = probe_order[pidx];
            actor_rank_top.push_back({
                {"rank", static_cast<int>(pidx + 1)},
                {"node", candidate_node_ids[probe_idx]},
                {"actor_prob", probe_idx < probs.size() ? probs[probe_idx] : 0.0},
                {"blended_score", probe_idx < probe_scores.size() ? probe_scores[probe_idx] : 0.0}
            });
        }
        step_trace["actor_ranking_top"] = actor_rank_top;
        const auto prev_probe_it = std::find(candidate_node_ids.begin(), candidate_node_ids.end(), prev_node);
        if (prev_probe_it != candidate_node_ids.end()) {
            const size_t prev_probe_idx = static_cast<size_t>(std::distance(candidate_node_ids.begin(), prev_probe_it));
            const auto order_it = std::find(probe_order.begin(), probe_order.end(), prev_probe_idx);
            if (order_it != probe_order.end()) {
                std::rotate(probe_order.begin(), order_it, order_it + 1);
            }
        }
        if (probe_order.size() > 1) {
            const size_t keep_head = std::min<size_t>(4, probe_order.size());
            const size_t tail_size = probe_order.size() - keep_head;
            if (tail_size > 1) {
                const size_t offset = static_cast<size_t>((seed + static_cast<int>(i)) % static_cast<int>(tail_size));
                std::rotate(
                    probe_order.begin() + static_cast<std::ptrdiff_t>(keep_head),
                    probe_order.begin() + static_cast<std::ptrdiff_t>(keep_head + offset),
                    probe_order.end()
                );
            }
        }
        const size_t max_probe_count = static_cast<size_t>(std::max(
            request.realtime_mode ? 6 : 10,
            std::min(
                request.realtime_mode ? kRealtimeProbePerVnf : kMaxProbePerVnf,
                search_target * (request.realtime_mode ? 3 : 5)
            )
        ));
        if (probe_order.size() > max_probe_count) {
            probe_order.resize(max_probe_count);
        }

        std::unordered_map<std::string, std::vector<std::string>> local_path_cache;
        local_path_cache.reserve(probe_order.size());
        std::unordered_map<std::string, int> reject_counters;
        const size_t desired_feasible_rank = static_cast<size_t>((seed + static_cast<int>(i) * 3) % 4);
        size_t feasible_rank = 0;
        bool has_backup_choice = false;
        std::string backup_node;
        std::vector<DeploymentCandidate::LinkDetail> backup_path_links;
        double backup_path_latency = 0.0;
        double backup_path_reliability = 1.0;
        double backup_path_bottleneck = std::numeric_limits<double>::infinity();
        for (size_t probe : probe_order) {
            const auto& node_id = candidate_node_ids[probe];
            std::vector<std::string> path = {node_id};
            int effective_hop_cap = hop_cap;
            if (has_prev_vnf && prev_node != node_id) {
                auto pit = local_path_cache.find(make_local_path_cache_key(node_id, effective_hop_cap));
                if (pit == local_path_cache.end()) {
                    pit = local_path_cache.emplace(
                        make_local_path_cache_key(node_id, effective_hop_cap),
                        find_shortest_path(
                            prev_node,
                            node_id,
                            topology,
                            required_bandwidth_gbps,
                            effective_hop_cap,
                            false
                        )
                    ).first;
                }
                path = pit->second;
                if ((path.empty() || path.size() < 2) && relaxed_hop_cap > effective_hop_cap) {
                    effective_hop_cap = relaxed_hop_cap;
                    auto relaxed_it = local_path_cache.find(make_local_path_cache_key(node_id, effective_hop_cap));
                    if (relaxed_it == local_path_cache.end()) {
                        relaxed_it = local_path_cache.emplace(
                            make_local_path_cache_key(node_id, effective_hop_cap),
                            find_shortest_path(
                                prev_node,
                                node_id,
                                topology,
                                required_bandwidth_gbps,
                                effective_hop_cap,
                                false
                            )
                        ).first;
                    }
                    path = relaxed_it->second;
                }
                if (path.empty() || path.size() < 2) {
                    reject_counters["path_unreachable"] += 1;
                    continue;
                }
            }

            auto path_links = generate_path_links(path, topology, required_bandwidth_gbps);
            const PathMetrics path_metrics = evaluate_path_links(path_links, required_bandwidth_gbps);
            if (!path_metrics.feasible) {
                reject_counters["link_or_bandwidth_violation"] += 1;
                continue;
            }
            if (effective_hop_cap > 0 && path_metrics.hops > effective_hop_cap) {
                reject_counters["hop_cap_exceeded"] += 1;
                continue;
            }
            if (accumulated_latency + path_metrics.latency_ms > request.constraints.max_latency_ms) {
                reject_counters["latency_constraint"] += 1;
                continue;
            }

            const double node_rel =
                deployed_node_set.find(node_id) == deployed_node_set.end()
                    ? estimate_node_reliability(node_id, topology)
                    : 1.0;
            const double tentative_reliability = accumulated_reliability * path_metrics.reliability * node_rel;
            const size_t remaining_steps = request.vnfs.size() - (i + 1) + 1; // +1 for final leg to destination
            const double optimistic_future_rel =
                std::pow(kFutureStepReliabilityDecay, static_cast<double>(remaining_steps));
            const int estimated_total_hops =
                estimate_total_hops(accumulated_hops, path_metrics.hops, remaining_steps);
            if (estimated_total_hops > kHardTotalHopLimit) {
                reject_counters["total_hop_projection"] += 1;
                continue;
            }
            const double rel_target = effective_reliability_target(
                request.constraints.min_reliability,
                estimated_total_hops
            );
            if (tentative_reliability * optimistic_future_rel < rel_target) {
                reject_counters["reliability_projection"] += 1;
                continue;
            }

            if (!has_backup_choice) {
                backup_node = node_id;
                backup_path_links = path_links;
                backup_path_latency = path_metrics.latency_ms;
                backup_path_reliability = path_metrics.reliability * node_rel;
                backup_path_bottleneck = path_metrics.bottleneck_bandwidth_gbps;
                has_backup_choice = true;
            }
            if (feasible_rank++ < desired_feasible_rank) {
                continue;
            }

            selected_node = node_id;
            selected_path_links = std::move(path_links);
            selected_path_latency = path_metrics.latency_ms;
            selected_path_reliability = path_metrics.reliability * node_rel;
            selected_path_bottleneck = path_metrics.bottleneck_bandwidth_gbps;
            found_feasible_node = true;
            break;
        }

        if (!found_feasible_node) {
            // 覆盖所有已排序候选，降低“存在可行节点但被扫描上限截断”导致的漏解概率。
            const int fallback_scan_limit = static_cast<int>(ranked_nodes.size());
            for (int rank_idx = actor_top_k; rank_idx < fallback_scan_limit; ++rank_idx) {
                const auto& node_id = ranked_nodes[rank_idx].first;
                std::vector<std::string> path = {node_id};
                int effective_hop_cap = hop_cap;
                if (has_prev_vnf && prev_node != node_id) {
                    auto pit = local_path_cache.find(make_local_path_cache_key(node_id, effective_hop_cap));
                    if (pit == local_path_cache.end()) {
                        pit = local_path_cache.emplace(
                            make_local_path_cache_key(node_id, effective_hop_cap),
                            find_shortest_path(
                                prev_node,
                                node_id,
                                topology,
                                required_bandwidth_gbps,
                                effective_hop_cap,
                                false
                            )
                        ).first;
                    }
                    path = pit->second;
                    if ((path.empty() || path.size() < 2) && relaxed_hop_cap > effective_hop_cap) {
                        effective_hop_cap = relaxed_hop_cap;
                        auto relaxed_it = local_path_cache.find(make_local_path_cache_key(node_id, effective_hop_cap));
                        if (relaxed_it == local_path_cache.end()) {
                            relaxed_it = local_path_cache.emplace(
                                make_local_path_cache_key(node_id, effective_hop_cap),
                                find_shortest_path(
                                    prev_node,
                                    node_id,
                                    topology,
                                    required_bandwidth_gbps,
                                    effective_hop_cap,
                                    false
                                )
                            ).first;
                        }
                        path = relaxed_it->second;
                    }
                    if (path.empty() || path.size() < 2) {
                        reject_counters["fallback_path_unreachable"] += 1;
                        continue;
                    }
                }

                auto path_links = generate_path_links(path, topology, required_bandwidth_gbps);
                const PathMetrics path_metrics = evaluate_path_links(path_links, required_bandwidth_gbps);
                if (!path_metrics.feasible) {
                    reject_counters["fallback_link_or_bandwidth_violation"] += 1;
                    continue;
                }
                if (effective_hop_cap > 0 && path_metrics.hops > effective_hop_cap) {
                    reject_counters["fallback_hop_cap_exceeded"] += 1;
                    continue;
                }
                if (accumulated_latency + path_metrics.latency_ms > request.constraints.max_latency_ms) {
                    reject_counters["fallback_latency_constraint"] += 1;
                    continue;
                }

                const double node_rel =
                    deployed_node_set.find(node_id) == deployed_node_set.end()
                        ? estimate_node_reliability(node_id, topology)
                        : 1.0;
                const double tentative_reliability =
                    accumulated_reliability * path_metrics.reliability * node_rel;
                const size_t remaining_steps = request.vnfs.size() - (i + 1) + 1;
                const double optimistic_future_rel =
                    std::pow(kFutureStepReliabilityDecay, static_cast<double>(remaining_steps));
                const int estimated_total_hops =
                    estimate_total_hops(accumulated_hops, path_metrics.hops, remaining_steps);
                if (estimated_total_hops > kHardTotalHopLimit) {
                    reject_counters["fallback_total_hop_projection"] += 1;
                    continue;
                }
                const double rel_target = effective_reliability_target(
                    request.constraints.min_reliability,
                    estimated_total_hops
                );
                if (tentative_reliability * optimistic_future_rel < rel_target) {
                    reject_counters["fallback_reliability_projection"] += 1;
                    continue;
                }

                if (!has_backup_choice) {
                    backup_node = node_id;
                    backup_path_links = path_links;
                    backup_path_latency = path_metrics.latency_ms;
                    backup_path_reliability = path_metrics.reliability * node_rel;
                    backup_path_bottleneck = path_metrics.bottleneck_bandwidth_gbps;
                    has_backup_choice = true;
                }
                if (feasible_rank++ < desired_feasible_rank) {
                    continue;
                }

                selected_node = node_id;
                selected_path_links = std::move(path_links);
                selected_path_latency = path_metrics.latency_ms;
                selected_path_reliability = path_metrics.reliability * node_rel;
                selected_path_bottleneck = path_metrics.bottleneck_bandwidth_gbps;
                found_feasible_node = true;
                break;
            }
        }

        if (!found_feasible_node && has_backup_choice) {
            selected_node = backup_node;
            selected_path_links = std::move(backup_path_links);
            selected_path_latency = backup_path_latency;
            selected_path_reliability = backup_path_reliability;
            selected_path_bottleneck = backup_path_bottleneck;
            found_feasible_node = true;
        }

        if (!found_feasible_node) {
            spdlog::error("No SLA-feasible node for core NF {}: {}", i, vnf.name);
            if (candidate_trace) {
                step_trace["status"] = "failed";
                step_trace["failure_reason"] = "no_sla_feasible_node";
                step_trace["reject_counters"] = reject_counters;
                append_step_trace(step_trace);
            }
            return finalize_failure("No SLA-feasible node for core NF: " + vnf.name);
        }

        spdlog::debug("Core NF {} ({}) -> Node {}", i, vnf.name, selected_node);

        for (const auto& link : selected_path_links) {
            candidate.link_details.push_back(link);
        }

        accumulated_latency += selected_path_latency;
        remaining_latency -= selected_path_latency;
        accumulated_reliability *= selected_path_reliability;
        accumulated_hops += static_cast<int>(selected_path_links.size());
        bottleneck_bandwidth = std::min(bottleneck_bandwidth, selected_path_bottleneck);
        if (accumulated_hops > kHardTotalHopLimit) {
            return finalize_failure("Hop budget exceeded during core NF placement");
        }
        
        // 检查时延约束
        if (accumulated_latency > request.constraints.max_latency_ms) {
            spdlog::error("Latency exceeded: {:.2f} > {:.2f}", 
                         accumulated_latency, request.constraints.max_latency_ms);
            return finalize_failure("Latency constraint violated");
        }
        
        // 添加部署信息
        DeploymentCandidate::PerVNF pv;
        pv.vnf = vnf.name;
        pv.core_nf = vnf.name;
        pv.nf_type = vnf.nf_type.empty() ? vnf.name : vnf.nf_type;
        pv.nf_role = vnf.nf_role;
        pv.node = selected_node;
        pv.cpu_used = vnf.cpu;
        pv.mem_used = vnf.mem;
        pv.disk_used = vnf.disk;
        candidate.per_vnf.push_back(pv);
        candidate.deployed_nodes.push_back(selected_node);
        deployed_node_set.insert(selected_node);
        if (candidate_trace) {
            step_trace["status"] = "selected";
            step_trace["selected_node"] = selected_node;
            step_trace["selected_path_latency_ms"] = selected_path_latency;
            step_trace["selected_path_reliability"] = selected_path_reliability;
            step_trace["selected_path_bottleneck_gbps"] = selected_path_bottleneck;
            step_trace["reject_counters"] = reject_counters;
            append_step_trace(step_trace);
        }
        
        prev_node = selected_node;
        has_prev_vnf = true;
    }

    std::vector<std::string> final_path;
    bool final_path_relaxed_bw = false;
    if (prev_node != request.destination_node) {
        const int final_hop_cap = compute_hop_cap(0, accumulated_hops);
        final_path = find_shortest_path(
            prev_node,
            request.destination_node,
            topology,
            request.constraints.min_bandwidth_gbps,
            final_hop_cap,
            false
        );
        if (final_path.empty() || final_path.size() < 2) {
            final_path = find_shortest_path(
                prev_node,
                request.destination_node,
                topology,
                0.0,
                compute_relaxed_hop_cap(final_hop_cap),
                false
            );
            if (!final_path.empty() && final_path.size() >= 2) {
                final_path_relaxed_bw = true;
            }
        }
        if (final_path.empty() || final_path.size() < 2) {
            return finalize_failure("No path to destination node");
        }
    } else {
        final_path = {request.destination_node};
    }

    auto final_links = generate_path_links(final_path, topology, request.constraints.min_bandwidth_gbps);
    const PathMetrics final_metrics = evaluate_path_links(final_links, request.constraints.min_bandwidth_gbps);
    if (!final_metrics.feasible) {
        return finalize_failure(
            final_path_relaxed_bw
                ? "Final path exists but does not meet bandwidth/status constraints"
                : "Final path does not meet bandwidth/status constraints"
        );
    }
    if (accumulated_latency + final_metrics.latency_ms > request.constraints.max_latency_ms) {
        return finalize_failure("Final path latency exceeds SLA");
    }

    for (const auto& ld : final_links) {
        candidate.link_details.push_back(ld);
    }
    accumulated_latency += final_metrics.latency_ms;
    accumulated_reliability *= final_metrics.reliability;
    accumulated_hops += static_cast<int>(final_links.size());
    if (accumulated_hops > kHardTotalHopLimit) {
        return finalize_failure("Final path exceeds hop budget");
    }
    bottleneck_bandwidth = std::min(
        bottleneck_bandwidth,
        final_metrics.bottleneck_bandwidth_gbps
    );
    if (candidate_trace) {
        (*candidate_trace)["final_path"] = final_path;
        (*candidate_trace)["final_path_latency_ms"] = final_metrics.latency_ms;
        (*candidate_trace)["final_path_reliability"] = final_metrics.reliability;
        (*candidate_trace)["final_path_bottleneck_gbps"] = final_metrics.bottleneck_bandwidth_gbps;
    }

    std::vector<std::string> unique_nodes;
    unique_nodes.reserve(candidate.deployed_nodes.size());
    for (const auto& node_id : candidate.deployed_nodes) {
        if (unique_nodes.empty() || unique_nodes.back() != node_id) {
            unique_nodes.push_back(node_id);
        }
    }
    std::unordered_set<std::string> seen_nodes;
    std::vector<std::string> deduped_nodes;
    deduped_nodes.reserve(unique_nodes.size());
    for (const auto& node_id : unique_nodes) {
        if (seen_nodes.insert(node_id).second) deduped_nodes.push_back(node_id);
    }
    candidate.deployed_nodes = std::move(deduped_nodes);

    candidate.total_latency_ms = accumulated_latency;
    candidate.estimated_reliability = accumulated_reliability;
    const double final_rel_target = effective_reliability_target(
        request.constraints.min_reliability,
        accumulated_hops
    );
    candidate.bottleneck_bandwidth_gbps =
        std::isfinite(bottleneck_bandwidth) ? bottleneck_bandwidth : request.constraints.min_bandwidth_gbps;
    candidate.satisfies_constraints =
        (accumulated_latency <= request.constraints.max_latency_ms) &&
        (candidate.bottleneck_bandwidth_gbps >= request.constraints.min_bandwidth_gbps) &&
        (accumulated_reliability >= final_rel_target) &&
        (accumulated_hops <= kTargetDeploymentHops);
    
    if (!candidate.satisfies_constraints) {
        if (accumulated_latency > request.constraints.max_latency_ms) {
            candidate.reason = "Latency constraint violated";
        } else if (candidate.bottleneck_bandwidth_gbps < request.constraints.min_bandwidth_gbps) {
            candidate.reason = "Bandwidth constraint violated";
        } else if (accumulated_reliability < final_rel_target) {
            candidate.reason = "Reliability constraint violated";
        } else if (accumulated_hops > kTargetDeploymentHops) {
            candidate.reason = "Hop target exceeded";
        } else {
            candidate.reason = "SLA constraints violated";
        }
    }

    const double latency_score = request.constraints.max_latency_ms > 0.0
        ? std::min(1.0, request.constraints.max_latency_ms / std::max(1.0, accumulated_latency))
        : 1.0;
    const double bw_score = request.constraints.min_bandwidth_gbps > 0.0
        ? std::min(1.0, candidate.bottleneck_bandwidth_gbps / request.constraints.min_bandwidth_gbps)
        : 1.0;
    const double rel_score = final_rel_target > 0.0
        ? std::min(1.0, accumulated_reliability / final_rel_target)
        : 1.0;
    const double dispersion_score = request.vnfs.empty()
        ? 1.0
        : std::min(1.0, static_cast<double>(candidate.deployed_nodes.size()) /
                          static_cast<double>(request.vnfs.size()));

    double resource_score = 0.0;
    if (!candidate.deployed_nodes.empty()) {
        for (const auto& node_id : candidate.deployed_nodes) {
            const auto it = cache.node_index.find(node_id);
            if (it == cache.node_index.end()) continue;
            const auto& node = topology.nodes[it->second];
            const double cpu = node.cpu_total > 0.0 ? node.cpu_available / node.cpu_total : 0.0;
            const double mem = node.mem_total > 0.0 ? node.mem_available / node.mem_total : 0.0;
            const double disk = node.disk_total > 0.0 ? node.disk_available / node.disk_total : 0.0;
            resource_score += (cpu + mem + disk) / 3.0;
        }
        resource_score /= static_cast<double>(candidate.deployed_nodes.size());
    } else {
        resource_score = 1.0;
    }

    const bool custom_weights =
        request.score_weights.latency >= 0.0 &&
        request.score_weights.resource >= 0.0 &&
        request.score_weights.reliability >= 0.0 &&
        request.score_weights.bandwidth >= 0.0 &&
        request.score_weights.dispersion >= 0.0;

    if (custom_weights) {
        const double sum_w =
            request.score_weights.latency +
            request.score_weights.resource +
            request.score_weights.reliability +
            request.score_weights.bandwidth +
            request.score_weights.dispersion;
        const double norm = sum_w > 1e-9 ? sum_w : 1.0;
        const double w_lat = request.score_weights.latency / norm;
        const double w_res = request.score_weights.resource / norm;
        const double w_rel = request.score_weights.reliability / norm;
        const double w_bw = request.score_weights.bandwidth / norm;
        const double w_disp = request.score_weights.dispersion / norm;
        candidate.score = w_lat * latency_score +
                          w_res * resource_score +
                          w_rel * rel_score +
                          w_bw * bw_score +
                          w_disp * dispersion_score;
    } else if (request.optimize == "resource") {
        candidate.score = 0.4 * resource_score + 0.25 * rel_score + 0.2 * latency_score + 0.15 * bw_score;
    } else if (request.optimize == "balanced") {
        candidate.score = 0.25 * resource_score + 0.25 * rel_score + 0.25 * latency_score + 0.25 * bw_score;
    } else {
        candidate.score = 0.45 * latency_score + 0.25 * rel_score + 0.2 * bw_score + 0.1 * resource_score;
    }
    candidate.score = std::max(0.0, std::min(1.0, candidate.score));
    
    if (!request.realtime_mode) {
        spdlog::info(
            "Candidate {}: {} core NFs, {} links, {:.2f}ms, feasible: {}",
            seed + 1,
            candidate.per_vnf.size(),
            candidate.link_details.size(),
            candidate.total_latency_ms,
            candidate.satisfies_constraints
        );
    } else {
        spdlog::debug(
            "[realtime] Candidate {}: score={:.3f}, latency={:.2f}ms, feasible={}",
            seed + 1,
            candidate.score,
            candidate.total_latency_ms,
            candidate.satisfies_constraints
        );
    }
    if (candidate_trace) {
        (*candidate_trace)["status"] = candidate.satisfies_constraints ? "success" : "failed";
        (*candidate_trace)["final"] = {
            {"score", candidate.score},
            {"satisfies_constraints", candidate.satisfies_constraints},
            {"reason", candidate.reason},
            {"total_latency_ms", candidate.total_latency_ms},
            {"estimated_reliability", candidate.estimated_reliability},
            {"bottleneck_bandwidth_gbps", candidate.bottleneck_bandwidth_gbps},
            {"total_hops", accumulated_hops},
            {"deployed_nodes", candidate.deployed_nodes}
        };
    }
    
    return candidate;
}

std::vector<std::string> InferenceEngine::filter_candidate_nodes(
    const SFCRequest& request,
    const VNF& vnf,
    const std::string& prev_node,
    size_t current_vnf_idx,
    double remaining_latency,
    double accumulated_reliability,
    int accumulated_hops,
    const std::unordered_set<std::string>& deployed_node_set,
    const Topology& topology
) {
    std::vector<std::string> candidates;
    auto& cache = get_topology_cache(topology);
    const double required_bw = std::max(request.constraints.min_bandwidth_gbps, std::max(vnf.bw_in, vnf.bw_out));
    const int hop_cap = compute_hop_cap(request.vnfs.size() - current_vnf_idx, accumulated_hops);
    const std::vector<double>* prev_distances = nullptr;
    const std::vector<double>* dest_distances = nullptr;
    const SearchTree* prev_tree = nullptr;
    const SearchTree* dest_tree = nullptr;
    if (!prev_node.empty()) {
        auto it = cache.node_index.find(prev_node);
        if (it != cache.node_index.end()) {
            prev_distances = &get_shortest_distances_from(topology, it->second);
            prev_tree = &get_constrained_tree(topology, prev_node, required_bw, hop_cap);
        }
    }
    if (!request.destination_node.empty()) {
        auto it = cache.node_index.find(request.destination_node);
        if (it != cache.node_index.end()) {
            dest_distances = &get_shortest_distances_from(topology, it->second);
            dest_tree = &get_constrained_tree(topology, request.destination_node, required_bw, -1);
        }
    }

    auto pass = [&](bool enforce_dest_budget, double delay_relax_factor) {
        std::vector<std::string> pass_candidates;
        pass_candidates.reserve(topology.nodes.size());
        const double reliability_threshold_factor = enforce_dest_budget ? 0.55 : 0.35;
        for (size_t idx = 0; idx < topology.nodes.size(); ++idx) {
            const auto& node = topology.nodes[idx];
            if (node.cpu_available < vnf.cpu ||
                node.mem_available < vnf.mem ||
                node.disk_available < vnf.disk) {
                continue;
            }
            const double delay_budget = remaining_latency * delay_relax_factor;
            if (prev_distances && idx < prev_distances->size()) {
                double path_delay = (*prev_distances)[idx];
                if (!std::isfinite(path_delay) || path_delay > delay_budget) {
                    continue;
                }
                if (!prev_tree || idx >= prev_tree->latency.size() ||
                    !std::isfinite(prev_tree->latency[idx]) ||
                    prev_tree->hops[idx] == std::numeric_limits<int>::max()) {
                    continue;
                }
                if (enforce_dest_budget && dest_distances && idx < dest_distances->size()) {
                    const double to_dest = (*dest_distances)[idx];
                    if (!std::isfinite(to_dest) || (path_delay + to_dest) > delay_budget) {
                        continue;
                    }
                }
            } else if (enforce_dest_budget && dest_distances && idx < dest_distances->size() && current_vnf_idx + 1 >= request.vnfs.size()) {
                const double to_dest = (*dest_distances)[idx];
                if (!std::isfinite(to_dest) || to_dest > delay_budget) {
                    continue;
                }
            }

            const double node_rel =
                deployed_node_set.find(node.id) == deployed_node_set.end()
                    ? estimate_node_reliability(node.id, topology)
                    : 1.0;
            const size_t remaining_steps = request.vnfs.size() > current_vnf_idx
                ? request.vnfs.size() - current_vnf_idx
                : 0;
            const double prev_path_rel =
                (prev_tree && idx < prev_tree->reliability.size() &&
                 prev_tree->hops[idx] != std::numeric_limits<int>::max())
                    ? soften_path_reliability(prev_tree->reliability[idx], prev_tree->hops[idx])
                    : 1.0;
            double optimistic_rel =
                accumulated_reliability * node_rel * prev_path_rel *
                std::pow(kFutureStepReliabilityDecay, static_cast<double>(remaining_steps));
            const int projected_total_hops = estimate_total_hops(
                accumulated_hops,
                (prev_tree && idx < prev_tree->hops.size() &&
                 prev_tree->hops[idx] != std::numeric_limits<int>::max())
                    ? prev_tree->hops[idx]
                    : 0,
                remaining_steps
            );
            if (projected_total_hops > kHardTotalHopLimit) {
                continue;
            }
            if (enforce_dest_budget && dest_tree && idx < dest_tree->reliability.size() &&
                dest_tree->reliability[idx] > 0.0 &&
                dest_tree->hops[idx] != std::numeric_limits<int>::max()) {
                const double dest_path_rel = soften_path_reliability(
                    dest_tree->reliability[idx],
                    dest_tree->hops[idx]
                );
                optimistic_rel *= std::pow(dest_path_rel, 0.20);
            }
            if (optimistic_rel < request.constraints.min_reliability * reliability_threshold_factor) {
                continue;
            }

            pass_candidates.push_back(node.id);
        }
        return pass_candidates;
    };

    // 第一阶段：严格执行入口+出口可达性与可靠性预筛选
    candidates = pass(true, 1.0);
    // 第二阶段（兜底）：放宽剪枝，避免可行解在前置过滤阶段被全部剪空
    if (candidates.empty()) {
        candidates = pass(false, 1.45);
        if (!candidates.empty()) {
            spdlog::warn(
                "Relaxed candidate filtering at core NF index {} (prev={}, remaining_latency={:.2f}ms), recovered {} candidates",
                current_vnf_idx, prev_node, remaining_latency, candidates.size()
            );
        }
    }

    // 第三阶段（全量恢复）：前两级剪枝仍无候选时，执行全局可达性恢复，避免“有解被剪空”。
    if (candidates.empty()) {
        const SearchTree* exhaustive_prev_tree = nullptr;
        const SearchTree* exhaustive_dest_tree = nullptr;
        if (!prev_node.empty()) {
            const auto it = cache.node_index.find(prev_node);
            if (it != cache.node_index.end()) {
                exhaustive_prev_tree = &get_constrained_tree(topology, prev_node, required_bw, -1);
            }
        }
        if (!request.destination_node.empty()) {
            const auto it = cache.node_index.find(request.destination_node);
            if (it != cache.node_index.end()) {
                exhaustive_dest_tree = &get_constrained_tree(topology, request.destination_node, required_bw, -1);
            }
        }

        const bool is_last_vnf = current_vnf_idx + 1 >= request.vnfs.size();
        const size_t remaining_steps = request.vnfs.size() > current_vnf_idx
            ? request.vnfs.size() - current_vnf_idx
            : 0;
        std::vector<std::string> recovered;
        recovered.reserve(topology.nodes.size() / 2);

        for (size_t idx = 0; idx < topology.nodes.size(); ++idx) {
            const auto& node = topology.nodes[idx];
            if (node.cpu_available < vnf.cpu ||
                node.mem_available < vnf.mem ||
                node.disk_available < vnf.disk) {
                continue;
            }
            double path_delay = 0.0;
            double path_reliability = 1.0;
            if (!prev_node.empty() && prev_node != node.id) {
                if (!exhaustive_prev_tree ||
                    idx >= exhaustive_prev_tree->latency.size() ||
                    !std::isfinite(exhaustive_prev_tree->latency[idx]) ||
                    exhaustive_prev_tree->hops[idx] == std::numeric_limits<int>::max()) {
                    continue;
                }
                path_delay = exhaustive_prev_tree->latency[idx];
                path_reliability = soften_path_reliability(
                    exhaustive_prev_tree->reliability[idx],
                    exhaustive_prev_tree->hops[idx]
                );
            }
            if (path_delay > remaining_latency + 1e-9) {
                continue;
            }

            if (is_last_vnf && !request.destination_node.empty() && request.destination_node != node.id) {
                if (!exhaustive_dest_tree ||
                    idx >= exhaustive_dest_tree->latency.size() ||
                    !std::isfinite(exhaustive_dest_tree->latency[idx]) ||
                    exhaustive_dest_tree->hops[idx] == std::numeric_limits<int>::max()) {
                    continue;
                }
                const double final_leg_delay = exhaustive_dest_tree->latency[idx];
                if (path_delay + final_leg_delay > remaining_latency * 1.08 + 1e-9) {
                    continue;
                }
            }

            const double node_rel =
                deployed_node_set.find(node.id) == deployed_node_set.end()
                    ? estimate_node_reliability(node.id, topology)
                    : 1.0;
            const double optimistic_rel =
                accumulated_reliability * node_rel * path_reliability *
                std::pow(kFutureStepReliabilityDecay, static_cast<double>(remaining_steps));
            const int projected_total_hops = estimate_total_hops(
                accumulated_hops,
                (exhaustive_prev_tree && idx < exhaustive_prev_tree->hops.size() &&
                 exhaustive_prev_tree->hops[idx] != std::numeric_limits<int>::max())
                    ? exhaustive_prev_tree->hops[idx]
                    : 0,
                remaining_steps
            );
            if (projected_total_hops > kHardTotalHopLimit) {
                continue;
            }
            if (optimistic_rel + 1e-9 < request.constraints.min_reliability * 0.30) {
                continue;
            }

            recovered.push_back(node.id);
        }

        if (!recovered.empty()) {
            spdlog::warn(
                "Global recovery restored {} candidates at core NF index {} (prev={}, remaining_latency={:.2f}ms)",
                recovered.size(), current_vnf_idx, prev_node, remaining_latency
            );
            candidates = std::move(recovered);
        }
    }

    return candidates;
}

std::vector<std::pair<std::string, double>> InferenceEngine::rank_nodes_by_cost(
    const std::vector<std::string>& candidates,
    const VNF& vnf,
    const std::string& prev_node,
    const Topology& topology
) {
    const double ALPHA_CPU = 0.5;
    const double ALPHA_MEM = 0.35;
    const double ALPHA_DISK = 0.15;
    const double BETA_RESOURCE = 0.36;
    const double BETA_LATENCY = 0.24;
    const double BETA_HOPS = 0.10;
    const double BETA_RELIABILITY = 0.14;
    
    
    std::vector<std::pair<std::string, double>> ranked;
    auto& cache = get_topology_cache(topology);
    ranked.reserve(candidates.size());
    const std::vector<double>* prev_distances = nullptr;
    const SearchTree* prev_tree = nullptr;
    const double required_bw = std::max(vnf.bw_in, vnf.bw_out);
    if (!prev_node.empty()) {
        const auto prev_it = cache.node_index.find(prev_node);
        if (prev_it != cache.node_index.end()) {
            prev_distances = &get_shortest_distances_from(topology, prev_it->second);
            prev_tree = &get_constrained_tree(topology, prev_node, required_bw, compute_hop_cap(1));
        }
    }
    
    for (const auto& node_id : candidates) {
        const auto it = cache.node_index.find(node_id);
        if (it == cache.node_index.end()) continue;
        const Satellite* node = &topology.nodes[it->second];
        
        // 资源代价
        double cpu_util_after = 1.0 - (node->cpu_available - vnf.cpu) / node->cpu_total;
        double mem_util_after = 1.0 - (node->mem_available - vnf.mem) / node->mem_total;
        double disk_util_after = 1.0 - (node->disk_available - vnf.disk) / node->disk_total;
        double resource_cost = ALPHA_CPU * cpu_util_after + ALPHA_MEM * mem_util_after + ALPHA_DISK * disk_util_after;
        
        // 时延代价（简化计算）
        double latency_cost = 0.0;
        double hop_cost = 0.0;
        double reliability_cost = 0.0;
        if (!prev_node.empty() && prev_node != node_id) {
            if (prev_distances && it->second < prev_distances->size()) {
                const double d = (*prev_distances)[it->second];
                if (std::isfinite(d)) {
                    latency_cost = d;
                }
            }
            latency_cost /= 100.0;  // 归一化
            if (prev_tree && it->second < prev_tree->hops.size() &&
                prev_tree->hops[it->second] != std::numeric_limits<int>::max()) {
                hop_cost = static_cast<double>(prev_tree->hops[it->second]) / 8.0;
            }
            if (prev_tree && it->second < prev_tree->reliability.size() && prev_tree->reliability[it->second] > 0.0) {
                reliability_cost = 1.0 - prev_tree->reliability[it->second];
            }
        }
        
        double total_cost =
            BETA_RESOURCE * resource_cost +
            BETA_LATENCY * latency_cost +
            BETA_HOPS * hop_cost +
            BETA_RELIABILITY * reliability_cost;
        ranked.emplace_back(node_id, total_cost);
    }
    
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    return ranked;
}

std::pair<std::vector<float>, std::vector<int64_t>> 
InferenceEngine::prepare_graph_inputs(const Topology& topology) {
    std::vector<float> node_features;
    std::vector<int64_t> edge_index;
    node_features.reserve(topology.nodes.size() * std::max<size_t>(1, node_feature_dim_));
    edge_index.reserve(topology.links.size() * 2);

    auto& cache = get_topology_cache(topology);
    std::vector<double> active_sum(topology.nodes.size(), 0.0);
    std::vector<double> bw_sum(topology.nodes.size(), 0.0);
    std::vector<double> latency_sum(topology.nodes.size(), 0.0);
    std::vector<int> out_count(topology.nodes.size(), 0);

    for (const auto& link : topology.links) {
        const auto src_it = cache.node_index.find(link.source);
        const auto dst_it = cache.node_index.find(link.target);
        if (src_it == cache.node_index.end() || dst_it == cache.node_index.end()) continue;

        const size_t src_idx = src_it->second;
        edge_index.push_back(static_cast<int64_t>(src_idx));
        edge_index.push_back(static_cast<int64_t>(dst_it->second));

        out_count[src_idx] += 1;
        active_sum[src_idx] += link_is_down(link) ? 0.0 : 1.0;
        if (link.bandwidth_gbps > 1e-6) {
            bw_sum[src_idx] += std::max(0.0, link.bandwidth_available_gbps / link.bandwidth_gbps);
        }
        latency_sum[src_idx] += link.latency_ms / 50.0;
    }

    for (size_t i = 0; i < topology.nodes.size(); ++i) {
        const auto& node = topology.nodes[i];
        const double cpu_total = std::max(1e-6, node.cpu_total);
        const double mem_total = std::max(1e-6, node.mem_total);
        const double disk_total = std::max(1e-6, node.disk_total);

        const double cpu_ratio = node.cpu_available / cpu_total;
        const double mem_ratio = node.mem_available / mem_total;
        const double disk_ratio = node.disk_available / disk_total;

        double active_ratio = 0.0;
        double bw_ratio = 0.0;
        double latency_norm = 1.0;
        if (out_count[i] > 0) {
            active_ratio = active_sum[i] / out_count[i];
            bw_ratio = bw_sum[i] / out_count[i];
            latency_norm = latency_sum[i] / out_count[i];
        }

        const std::vector<float> base = {
            static_cast<float>(cpu_ratio),
            static_cast<float>(mem_ratio),
            static_cast<float>(disk_ratio),
            0.0f,
            static_cast<float>(clamp01(node.node_reliability)),
            static_cast<float>(active_ratio),
            static_cast<float>(bw_ratio),
            static_cast<float>(std::min(5.0, std::max(0.0, latency_norm))),
        };
        const auto fitted = fit_feature_dim(base, node_feature_dim_);
        node_features.insert(node_features.end(), fitted.begin(), fitted.end());
    }

    return {node_features, edge_index};
}

std::vector<float> InferenceEngine::run_gnn_encoder(
    const std::vector<float>& node_features,
    const std::vector<int64_t>& edge_index,
    size_t num_nodes
) {
    if (num_nodes == 0) return {};

    std::vector<int64_t> node_shape = {
        static_cast<int64_t>(num_nodes),
        static_cast<int64_t>(std::max<size_t>(1, node_feature_dim_))
    };
    std::vector<int64_t> edge_shape = {2, static_cast<int64_t>(edge_index.size() / 2)};

    try {
        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_,
            const_cast<float*>(node_features.data()),
            node_features.size(),
            node_shape.data(),
            node_shape.size()));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
            memory_info_,
            const_cast<int64_t*>(edge_index.data()),
            edge_index.size(),
            edge_shape.data(),
            edge_shape.size()));

        const char* input_names[] = {"node_features", "edge_index"};
        const char* output_names[] = {"node_embeddings"};

        auto output_tensors = gnn_session_->Run(
            Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 2, output_names, 1);

        const float* output_data = output_tensors[0].GetTensorData<float>();
        const auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
        node_embedding_dim_ = positive_last_dim_or(output_shape, node_embedding_dim_);
        size_t output_size = std::accumulate(
            output_shape.begin(), output_shape.end(), static_cast<size_t>(1), std::multiplies<size_t>());
        return std::vector<float>(output_data, output_data + output_size);
    } catch (const Ort::Exception& e) {
        spdlog::error("GNN inference failed: {}", e.what());
        return {};
    }
}

std::vector<float> InferenceEngine::run_actor_policy(
    const std::vector<float>& node_embeddings,
    const std::vector<int64_t>& candidate_indices,
    const std::vector<float>& vnf_features,
    const std::vector<float>& context_features
) {
    if (candidate_indices.empty()) return {};
    if (node_embeddings.empty()) return {};

    const size_t emb_dim = std::max<size_t>(1, node_embedding_dim_);
    if (node_embeddings.size() % emb_dim != 0) {
        spdlog::warn(
            "Actor input node_embeddings size mismatch: total={} not divisible by emb_dim={}",
            node_embeddings.size(),
            emb_dim
        );
        return {};
    }
    const size_t num_nodes = node_embeddings.size() / emb_dim;
    if (num_nodes == 0) return {};

    const std::vector<float> fitted_vnf = fit_feature_dim(vnf_features, std::max<size_t>(1, vnf_feature_dim_));
    const std::vector<float> fitted_ctx = fit_feature_dim(context_features, std::max<size_t>(1, context_feature_dim_));

    std::vector<int64_t> emb_shape = {static_cast<int64_t>(num_nodes), static_cast<int64_t>(emb_dim)};
    std::vector<int64_t> cand_shape = {static_cast<int64_t>(candidate_indices.size())};

    auto run_once = [&](const std::vector<float>& vnf_in, const std::vector<float>& ctx_in) -> std::vector<float> {
        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_,
            const_cast<float*>(node_embeddings.data()),
            node_embeddings.size(),
            emb_shape.data(),
            emb_shape.size()));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
            memory_info_,
            const_cast<int64_t*>(candidate_indices.data()),
            candidate_indices.size(),
            cand_shape.data(),
            cand_shape.size()));
        std::vector<int64_t> local_vnf_shape = {static_cast<int64_t>(vnf_in.size())};
        std::vector<int64_t> local_ctx_shape = {static_cast<int64_t>(ctx_in.size())};
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_,
            const_cast<float*>(vnf_in.data()),
            vnf_in.size(),
            local_vnf_shape.data(),
            local_vnf_shape.size()));
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_,
            const_cast<float*>(ctx_in.data()),
            ctx_in.size(),
            local_ctx_shape.data(),
            local_ctx_shape.size()));

        const char* input_names[] = {"node_embeddings", "candidate_indices", "vnf_features", "context_features"};
        const char* output_names[] = {"probs", "logits"};
        auto output_tensors = actor_session_->Run(
            Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 4, output_names, 2);

        const float* probs = output_tensors[0].GetTensorData<float>();
        return std::vector<float>(probs, probs + candidate_indices.size());
    };

    try {
        return run_once(fitted_vnf, fitted_ctx);
    } catch (const Ort::Exception& e) {
        const std::string err = e.what();
        auto expected_vnf = parse_expected_dim_from_error(err, "vnf_features");
        auto expected_ctx = parse_expected_dim_from_error(err, "context_features");
        bool can_retry = false;
        std::vector<float> retry_vnf = fitted_vnf;
        std::vector<float> retry_ctx = fitted_ctx;
        if (expected_vnf && *expected_vnf > 0 && *expected_vnf != retry_vnf.size()) {
            retry_vnf = fit_feature_dim(vnf_features, *expected_vnf);
            can_retry = true;
        }
        if (expected_ctx && *expected_ctx > 0 && *expected_ctx != retry_ctx.size()) {
            retry_ctx = fit_feature_dim(context_features, *expected_ctx);
            can_retry = true;
        }
        if (can_retry) {
            try {
                const auto probs = run_once(retry_vnf, retry_ctx);
                if (expected_vnf && *expected_vnf > 0) vnf_feature_dim_ = *expected_vnf;
                if (expected_ctx && *expected_ctx > 0) context_feature_dim_ = *expected_ctx;
                spdlog::warn(
                    "Actor inference recovered by dynamic dim fallback: vnf={} context={}",
                    retry_vnf.size(),
                    retry_ctx.size()
                );
                return probs;
            } catch (const Ort::Exception& retry_err) {
                spdlog::error("Actor inference retry failed: {}", retry_err.what());
            }
        }
        spdlog::error("Actor inference failed: {}", err);
        return {};
    }
}

} // namespace sfc
