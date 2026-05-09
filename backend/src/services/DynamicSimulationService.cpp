#include "services/DynamicSimulationService.h"
#include "utils/Sgp4Propagator.h"
#include "services/AuthGlobals.h"
#include "services/DeploymentOrchestratorService.h"
#include "services/RuntimeStateService.h"
#include "websocket/WSHandler.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unordered_set>

namespace sfc {
namespace {

constexpr double kMinSamplingIntervalSec = 10.0;
constexpr double kMaxSamplingIntervalSec = 30.0;

constexpr double kEarthRadiusKm = 6371.0;
constexpr double kLightSpeedKmPerSec = 299792.458;

const std::vector<std::string> kNodeFaultTypes = {
    "power_failure",
    "cpu_overload",
    "thermal_shutdown",
    "control_plane_sync_loss",
    "software_crash",
    "clock_drift"
};

const std::vector<std::string> kLinkFaultTypes = {
    "optical_signal_loss",
    "beam_misalignment",
    "interference_jamming",
    "routing_blackhole",
    "transceiver_failure",
    "line_degradation"
};

std::unordered_set<std::string> make_catalog_set(const std::vector<std::string>& values) {
    return std::unordered_set<std::string>(values.begin(), values.end());
}

const std::unordered_set<std::string> kNodeFaultTypeSet = make_catalog_set(kNodeFaultTypes);
const std::unordered_set<std::string> kLinkFaultTypeSet = make_catalog_set(kLinkFaultTypes);

std::string canonical_link_key(const std::string& a, const std::string& b) {
    if (a <= b) return a + "|" + b;
    return b + "|" + a;
}

std::pair<std::string, std::string> split_link_key(const std::string& key) {
    const size_t pos = key.find('|');
    if (pos == std::string::npos) return {"", ""};
    return {key.substr(0, pos), key.substr(pos + 1)};
}

double json_number_or(const nlohmann::json& req, const char* key, double fallback) {
    if (!req.contains(key) || !req[key].is_number()) return fallback;
    return req[key].get<double>();
}

int ttl_ticks_from_seconds(double seconds, double sampling_interval_sec) {
    return static_cast<int>(std::max(
        1.0,
        std::ceil(std::max(0.0, seconds) / std::max(1.0, sampling_interval_sec))
    ));
}

double remaining_fault_seconds(
    const DynamicSimulationService::FaultState& state,
    std::chrono::steady_clock::time_point now
) {
    if (state.expires_at == std::chrono::steady_clock::time_point{}) {
        return std::max(0.0, state.duration_sec);
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(state.expires_at - now).count();
    return std::max(0.0, static_cast<double>(remaining_ms) / 1000.0);
}

double parse_fault_duration_seconds(
    const nlohmann::json& req,
    double sampling_interval_sec,
    int fallback_ttl_ticks
) {
    double seconds = 0.0;
    if (req.contains("ttl_seconds")) {
        seconds = json_number_or(req, "ttl_seconds", 0.0);
    } else if (req.contains("duration_sec")) {
        seconds = json_number_or(req, "duration_sec", 0.0);
    } else if (req.contains("duration_seconds")) {
        seconds = json_number_or(req, "duration_seconds", 0.0);
    } else {
        seconds = static_cast<double>(fallback_ttl_ticks) * std::max(1.0, sampling_interval_sec);
    }
    return std::max(1.0, std::min(3600.0, seconds));
}

double parse_fault_extension_seconds(
    const nlohmann::json& req,
    double sampling_interval_sec
) {
    double seconds = 0.0;
    if (req.contains("delta_seconds")) {
        seconds = json_number_or(req, "delta_seconds", 0.0);
    } else if (req.contains("delta_ttl_ticks")) {
        seconds = json_number_or(req, "delta_ttl_ticks", 0.0) * std::max(1.0, sampling_interval_sec);
    }
    return std::max(1.0, std::min(3600.0, seconds));
}

std::vector<std::pair<std::string, std::string>> parse_target_links(const nlohmann::json& req) {
    std::vector<std::pair<std::string, std::string>> out;
    auto push_pair = [&](std::string a, std::string b) {
        if (a.empty() || b.empty() || a == b) return;
        out.emplace_back(std::move(a), std::move(b));
    };

    if (req.contains("links") && req["links"].is_array()) {
        for (const auto& item : req["links"]) {
            if (!item.is_object()) continue;
            const std::string src = item.value("source", item.value("src", std::string("")));
            const std::string dst = item.value("target", item.value("dst", std::string("")));
            push_pair(src, dst);
        }
    }
    const std::string src = req.value("source", req.value("src", std::string("")));
    const std::string dst = req.value("target", req.value("dst", std::string("")));
    if (!src.empty() || !dst.empty()) {
        push_pair(src, dst);
    }

    auto parse_link_text = [&](const std::string& value) {
        if (value.empty()) return;
        const std::string normalized = [&]() {
            std::string s = value;
            for (char& ch : s) {
                if (ch == '>' || ch == '-' || ch == '<') ch = '|';
            }
            return s;
        }();
        const size_t split = normalized.find('|');
        if (split == std::string::npos) return;
        const std::string a = normalized.substr(0, split);
        const std::string b = normalized.substr(split + 1);
        push_pair(a, b);
    };

    if (req.contains("link_ids") && req["link_ids"].is_array()) {
        for (const auto& item : req["link_ids"]) {
            if (!item.is_string()) continue;
            parse_link_text(item.get<std::string>());
        }
    }
    if (req.contains("link_id") && req["link_id"].is_string()) {
        parse_link_text(req["link_id"].get<std::string>());
    }

    std::unordered_set<std::string> dedup;
    std::vector<std::pair<std::string, std::string>> unique;
    unique.reserve(out.size());
    for (const auto& p : out) {
        const std::string key = canonical_link_key(p.first, p.second);
        if (!dedup.insert(key).second) continue;
        unique.push_back(p);
    }
    return unique;
}

std::vector<std::string> parse_target_ids(const nlohmann::json& req, const char* array_key, const char* scalar_key) {
    std::vector<std::string> out;
    if (req.contains(array_key) && req[array_key].is_array()) {
        for (const auto& item : req[array_key]) {
            if (item.is_string()) {
                const std::string id = item.get<std::string>();
                if (!id.empty()) out.push_back(id);
            }
        }
    }
    if (out.empty() && req.contains(scalar_key) && req[scalar_key].is_string()) {
        const std::string id = req[scalar_key].get<std::string>();
        if (!id.empty()) out.push_back(id);
    }
    return out;
}

} // namespace

DynamicSimulationService::DynamicSimulationService(
    std::shared_ptr<TopologyManager> topo_mgr,
    std::shared_ptr<ResourceManager> res_mgr
) : topo_mgr_(std::move(topo_mgr)),
    res_mgr_(std::move(res_mgr)),
    running_(false),
    sampling_interval_sec_(15.0),
    simulation_speed_(1.0),
    enable_faults_(false),
    node_fault_prob_per_tick_(0.0),
    link_fault_prob_per_tick_(0.0),
    topology_version_(0),
    sim_elapsed_sec_(0.0),
    sim_epoch_(std::chrono::system_clock::now()),
    last_wall_tick_(std::chrono::steady_clock::now()),
    rng_(std::random_device{}()) {}

DynamicSimulationService::~DynamicSimulationService() {
    stop(false);
}

const std::vector<std::string>& DynamicSimulationService::node_fault_catalog() {
    return kNodeFaultTypes;
}

const std::vector<std::string>& DynamicSimulationService::link_fault_catalog() {
    return kLinkFaultTypes;
}

std::string DynamicSimulationService::pick_node_fault_type() {
    if (kNodeFaultTypes.empty()) return "node_fault";
    std::uniform_int_distribution<size_t> dist(0, kNodeFaultTypes.size() - 1);
    return kNodeFaultTypes[dist(rng_)];
}

std::string DynamicSimulationService::pick_link_fault_type() {
    if (kLinkFaultTypes.empty()) return "link_fault";
    std::uniform_int_distribution<size_t> dist(0, kLinkFaultTypes.size() - 1);
    return kLinkFaultTypes[dist(rng_)];
}

Topology DynamicSimulationService::load_working_topology_locked() const {
    Topology topology = res_mgr_->export_current_topology();
    if (topology.nodes.empty()) {
        topology = topo_mgr_->get_current_topology();
    }
    return topology;
}

std::string DynamicSimulationService::current_sim_time_iso_locked() const {
    return iso_time_from_system_clock(
        sim_epoch_ + std::chrono::milliseconds(static_cast<int64_t>(sim_elapsed_sec_ * 1000.0))
    );
}

bool DynamicSimulationService::start(
    double sampling_interval_sec,
    double simulation_speed,
    bool enable_faults,
    double node_fault_prob_per_tick,
    double link_fault_prob_per_tick
) {
    std::lock_guard<std::mutex> lock(mutex_);
    sampling_interval_sec_ = clamp(sampling_interval_sec, kMinSamplingIntervalSec, kMaxSamplingIntervalSec);
    simulation_speed_ = clamp(simulation_speed, 0.1, 8.0);
    (void)enable_faults;
    (void)node_fault_prob_per_tick;
    enable_faults_ = false;
    node_fault_prob_per_tick_ = 0.0;
    (void)link_fault_prob_per_tick;
    link_fault_prob_per_tick_ = 0.0;

    if (running_) {
        spdlog::info(
            "Dynamic simulation already running; updated config interval={}s speed={}x",
            sampling_interval_sec_,
            simulation_speed_
        );
        return true;
    }

    running_ = true;
    sim_epoch_ = std::chrono::system_clock::now();
    sim_elapsed_sec_ = 0.0;
    topology_version_ = 0;
    node_fault_states_.clear();
    link_fault_states_.clear();
    last_wall_tick_ = std::chrono::steady_clock::now();
    loop_thread_ = std::thread(&DynamicSimulationService::run_loop, this);
    spdlog::info(
        "Dynamic simulation started: interval={}s, speed={}x, faults=manual_only",
        sampling_interval_sec_,
        simulation_speed_
    );
    if (g_runtime_state_service) {
        nlohmann::json cfg = g_runtime_state_service->load_control_config();
        if (!cfg.is_object()) cfg = nlohmann::json::object();
        cfg["resource_sampling_interval_sec"] = sampling_interval_sec_;
        cfg["simulation_speed"] = simulation_speed_;
        cfg["running"] = true;
        g_runtime_state_service->save_control_config(cfg);
    }
    return true;
}

void DynamicSimulationService::stop(bool persist_running_state) {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    if (persist_running_state && g_runtime_state_service) {
        nlohmann::json cfg = g_runtime_state_service->load_control_config();
        if (!cfg.is_object()) cfg = nlohmann::json::object();
        cfg["running"] = false;
        g_runtime_state_service->save_control_config(cfg);
    }
    spdlog::info("Dynamic simulation stopped");
}

bool DynamicSimulationService::is_running() const {
    return running_.load();
}

TopologySnapshot DynamicSimulationService::step_once() {
    TopologySnapshot snapshot;
    std::unordered_map<std::string, SnapshotListener> listeners;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = advance_one_tick_locked(sampling_interval_sec_ * simulation_speed_, true);
        listeners = snapshot_listeners_;
    }
    for (const auto& kv : listeners) {
        try {
            kv.second(snapshot);
        } catch (const std::exception& e) {
            spdlog::warn("Snapshot listener {} failed: {}", kv.first, e.what());
        }
    }
    return snapshot;
}

TopologySnapshot DynamicSimulationService::get_latest_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_snapshot_;
}

nlohmann::json DynamicSimulationService::status_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    nlohmann::json node_fault_details = nlohmann::json::array();
    for (const auto& kv : node_fault_states_) {
        const double remaining_sec = remaining_fault_seconds(kv.second, now);
        node_fault_details.push_back({
            {"node_id", kv.first},
            {"fault_type", kv.second.fault_type},
            {"injection_mode", kv.second.injection_mode},
            {"ttl_ticks", ttl_ticks_from_seconds(remaining_sec, sampling_interval_sec_)},
            {"duration_sec", kv.second.duration_sec},
            {"remaining_sec", remaining_sec}
        });
    }
    nlohmann::json link_fault_details = nlohmann::json::array();
    for (const auto& kv : link_fault_states_) {
        const auto [source, target] = split_link_key(kv.first);
        const double remaining_sec = remaining_fault_seconds(kv.second, now);
        link_fault_details.push_back({
            {"link_key", kv.first},
            {"source", source},
            {"target", target},
            {"fault_type", kv.second.fault_type},
            {"injection_mode", kv.second.injection_mode},
            {"ttl_ticks", ttl_ticks_from_seconds(remaining_sec, sampling_interval_sec_)},
            {"duration_sec", kv.second.duration_sec},
            {"remaining_sec", remaining_sec}
        });
    }
    return {
        {"running", running_.load()},
        {"sampling_interval_sec", sampling_interval_sec_},
        {"simulation_speed", simulation_speed_},
        {"enable_faults", false},
        {"node_fault_prob_per_tick", 0.0},
        {"link_fault_prob_per_tick", 0.0},
        {"auto_fault_injection_enabled", false},
        {"topology_version", topology_version_},
        {"sim_time", latest_snapshot_.sim_time},
        {"active_faults", {
            {"node", node_fault_states_.size()},
            {"link", link_fault_states_.size()}
        }},
        {"active_fault_details", {
            {"node", node_fault_details},
            {"link", link_fault_details}
        }},
        {"fault_catalog", {
            {"node", node_fault_catalog()},
            {"link", link_fault_catalog()}
        }},
        {"metrics", latest_snapshot_.to_json().value("metrics", nlohmann::json::object())}
    };
}

nlohmann::json DynamicSimulationService::inject_faults(const nlohmann::json& request) {
    TopologySnapshot snapshot;
    std::unordered_map<std::string, SnapshotListener> listeners;
    std::vector<nlohmann::json> events;
    std::vector<std::string> invalid_targets;
    int injected_count = 0;
    int removed_count = 0;
    int extended_count = 0;
    int skipped_existing = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Topology topology = load_working_topology_locked();
        if (topology.nodes.empty()) {
            return {
                {"ok", false},
                {"message", "No topology loaded"},
                {"injected", 0}
            };
        }

        const std::string entity_type = request.value("entity_type", std::string("node"));
        const std::string action = request.value("action", std::string("inject"));
        const bool overwrite_existing = request.value("overwrite_existing", true);
        const int requested_ttl = static_cast<int>(request.value("ttl_ticks", 4));
        const double duration_sec = parse_fault_duration_seconds(request, sampling_interval_sec_, requested_ttl);
        const int ttl_ticks = ttl_ticks_from_seconds(duration_sec, sampling_interval_sec_);
        const auto now = std::chrono::steady_clock::now();
        const auto expires_at = now + std::chrono::milliseconds(static_cast<int64_t>(duration_sec * 1000.0));
        const std::string sim_time = current_sim_time_iso_locked();

        if (entity_type == "node") {
            std::unordered_set<std::string> existing_ids;
            for (const auto& sat : topology.nodes) existing_ids.insert(sat.id);

            std::vector<std::string> node_ids = parse_target_ids(request, "node_ids", "node_id");
            if (node_ids.empty()) {
                const int batch_count_req = static_cast<int>(request.value("batch_count", 0));
                if (batch_count_req > 0) {
                    const bool only_active = request.value("only_active", true);
                    std::vector<std::string> candidates;
                    candidates.reserve(topology.nodes.size());
                    for (const auto& sat : topology.nodes) {
                        if (only_active && sat.status == "down") continue;
                        candidates.push_back(sat.id);
                    }
                    std::shuffle(candidates.begin(), candidates.end(), rng_);
                    const int batch_count = std::max(0, std::min(batch_count_req, static_cast<int>(candidates.size())));
                    node_ids.assign(candidates.begin(), candidates.begin() + batch_count);
                }
            }

            for (const auto& node_id : node_ids) {
                if (existing_ids.find(node_id) == existing_ids.end()) {
                    invalid_targets.push_back(node_id);
                    continue;
                }

                if (action == "inject") {
                    if (!overwrite_existing && node_fault_states_.find(node_id) != node_fault_states_.end()) {
                        skipped_existing += 1;
                        continue;
                    }

                    const std::string requested_type = request.value("fault_type", std::string(""));
                    const std::string fault_type =
                        (requested_type.empty() || requested_type == "auto" || kNodeFaultTypeSet.find(requested_type) == kNodeFaultTypeSet.end())
                            ? pick_node_fault_type()
                            : requested_type;

                    node_fault_states_[node_id] = FaultState{
                        ttl_ticks,
                        fault_type,
                        "manual",
                        duration_sec,
                        expires_at
                    };
                    injected_count += 1;
                    events.push_back({
                        {"type", "fault_event"},
                        {"entity_type", "node"},
                        {"entity_id", node_id},
                        {"fault_type", fault_type},
                        {"reason", fault_type},
                        {"injection_mode", "manual"},
                        {"ttl_ticks", ttl_ticks},
                        {"duration_sec", duration_sec},
                        {"remaining_sec", duration_sec},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }

                auto it = node_fault_states_.find(node_id);
                if (action == "remove") {
                    if (it == node_fault_states_.end()) {
                        invalid_targets.push_back(node_id);
                        continue;
                    }
                    const std::string fault_type = it->second.fault_type;
                    node_fault_states_.erase(it);
                    for (auto& sat : topology.nodes) {
                        if (sat.id == node_id) {
                            sat.status = "active";
                            sat.fault_tag.clear();
                            break;
                        }
                    }
                    removed_count += 1;
                    events.push_back({
                        {"type", "recovery_event"},
                        {"entity_type", "node"},
                        {"entity_id", node_id},
                        {"fault_type", fault_type},
                        {"reason", fault_type},
                        {"injection_mode", "manual_remove"},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }

                if (action == "extend") {
                    if (it == node_fault_states_.end()) {
                        invalid_targets.push_back(node_id);
                        continue;
                    }
                    const double delta_seconds = parse_fault_extension_seconds(request, sampling_interval_sec_);
                    if (it->second.expires_at == std::chrono::steady_clock::time_point{}) {
                        const double fallback_remaining =
                            std::max(1.0, static_cast<double>(it->second.ttl_ticks) * std::max(1.0, sampling_interval_sec_));
                        it->second.expires_at = now + std::chrono::milliseconds(static_cast<int64_t>(fallback_remaining * 1000.0));
                        it->second.duration_sec = fallback_remaining;
                    }
                    const auto base = it->second.expires_at > now ? it->second.expires_at : now;
                    it->second.expires_at =
                        base + std::chrono::milliseconds(static_cast<int64_t>(delta_seconds * 1000.0));
                    it->second.duration_sec = std::max(
                        it->second.duration_sec,
                        remaining_fault_seconds(it->second, now)
                    ) + delta_seconds;
                    const double remaining_sec = remaining_fault_seconds(it->second, now);
                    it->second.ttl_ticks = ttl_ticks_from_seconds(remaining_sec, sampling_interval_sec_);
                    extended_count += 1;
                    events.push_back({
                        {"type", "fault_update_event"},
                        {"entity_type", "node"},
                        {"entity_id", node_id},
                        {"fault_type", it->second.fault_type},
                        {"reason", "ttl_extended"},
                        {"injection_mode", "manual_extend"},
                        {"delta_seconds", delta_seconds},
                        {"ttl_ticks", it->second.ttl_ticks},
                        {"duration_sec", it->second.duration_sec},
                        {"remaining_sec", remaining_sec},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }
            }
        } else if (entity_type == "link") {
            std::unordered_set<std::string> existing_links;
            for (const auto& link : topology.links) {
                if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
                existing_links.insert(canonical_link_key(link.source, link.target));
            }

            std::vector<std::pair<std::string, std::string>> link_pairs = parse_target_links(request);
            if (link_pairs.empty() && request.contains("batch_count")) {
                const int batch_count_req = static_cast<int>(request.value("batch_count", 0));
                if (batch_count_req > 0) {
                    const bool only_active = request.value("only_active", true);
                    std::vector<std::pair<std::string, std::string>> candidates;
                    candidates.reserve(topology.links.size());
                    for (const auto& link : topology.links) {
                        if (link.source.empty() || link.target.empty() || link.source == link.target) continue;
                        if (only_active && link.status == "down") continue;
                        candidates.emplace_back(link.source, link.target);
                    }
                    std::shuffle(candidates.begin(), candidates.end(), rng_);
                    const int batch_count = std::max(0, std::min(batch_count_req, static_cast<int>(candidates.size())));
                    link_pairs.assign(candidates.begin(), candidates.begin() + batch_count);
                }
            }

            for (const auto& pair : link_pairs) {
                const std::string link_key = canonical_link_key(pair.first, pair.second);
                if (existing_links.find(link_key) == existing_links.end()) {
                    invalid_targets.push_back(link_key);
                    continue;
                }
                const auto [source, target] = split_link_key(link_key);

                if (action == "inject") {
                    if (!overwrite_existing && link_fault_states_.find(link_key) != link_fault_states_.end()) {
                        skipped_existing += 1;
                        continue;
                    }
                    const std::string requested_type = request.value("fault_type", std::string(""));
                    const std::string fault_type =
                        (requested_type.empty() || requested_type == "auto" ||
                        kLinkFaultTypeSet.find(requested_type) == kLinkFaultTypeSet.end())
                            ? pick_link_fault_type()
                            : requested_type;
                    link_fault_states_[link_key] = FaultState{
                        ttl_ticks,
                        fault_type,
                        "manual",
                        duration_sec,
                        expires_at
                    };
                    injected_count += 1;
                    events.push_back({
                        {"type", "fault_event"},
                        {"entity_type", "link"},
                        {"entity_id", link_key},
                        {"source", source},
                        {"target", target},
                        {"fault_type", fault_type},
                        {"reason", fault_type},
                        {"injection_mode", "manual"},
                        {"ttl_ticks", ttl_ticks},
                        {"duration_sec", duration_sec},
                        {"remaining_sec", duration_sec},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }

                auto it = link_fault_states_.find(link_key);
                if (action == "remove") {
                    if (it == link_fault_states_.end()) {
                        invalid_targets.push_back(link_key);
                        continue;
                    }
                    const std::string fault_type = it->second.fault_type;
                    link_fault_states_.erase(it);
                    removed_count += 1;
                    events.push_back({
                        {"type", "recovery_event"},
                        {"entity_type", "link"},
                        {"entity_id", link_key},
                        {"source", source},
                        {"target", target},
                        {"fault_type", fault_type},
                        {"reason", fault_type},
                        {"injection_mode", "manual_remove"},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }

                if (action == "extend") {
                    if (it == link_fault_states_.end()) {
                        invalid_targets.push_back(link_key);
                        continue;
                    }
                    const double delta_seconds = parse_fault_extension_seconds(request, sampling_interval_sec_);
                    if (it->second.expires_at == std::chrono::steady_clock::time_point{}) {
                        const double fallback_remaining =
                            std::max(1.0, static_cast<double>(it->second.ttl_ticks) * std::max(1.0, sampling_interval_sec_));
                        it->second.expires_at = now + std::chrono::milliseconds(static_cast<int64_t>(fallback_remaining * 1000.0));
                        it->second.duration_sec = fallback_remaining;
                    }
                    const auto base = it->second.expires_at > now ? it->second.expires_at : now;
                    it->second.expires_at =
                        base + std::chrono::milliseconds(static_cast<int64_t>(delta_seconds * 1000.0));
                    it->second.duration_sec = std::max(
                        it->second.duration_sec,
                        remaining_fault_seconds(it->second, now)
                    ) + delta_seconds;
                    const double remaining_sec = remaining_fault_seconds(it->second, now);
                    it->second.ttl_ticks = ttl_ticks_from_seconds(remaining_sec, sampling_interval_sec_);
                    extended_count += 1;
                    events.push_back({
                        {"type", "fault_update_event"},
                        {"entity_type", "link"},
                        {"entity_id", link_key},
                        {"source", source},
                        {"target", target},
                        {"fault_type", it->second.fault_type},
                        {"reason", "ttl_extended"},
                        {"injection_mode", "manual_extend"},
                        {"delta_seconds", delta_seconds},
                        {"ttl_ticks", it->second.ttl_ticks},
                        {"duration_sec", it->second.duration_sec},
                        {"remaining_sec", remaining_sec},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }
            }
        } else {
            return {
                {"ok", false},
                {"message", "Unsupported entity_type"},
                {"action", action},
                {"entity_type", entity_type},
                {"injected", 0}
            };
        }

        if (action != "inject" && action != "remove" && action != "extend") {
            return {
                {"ok", false},
                {"message", "Unsupported action"},
                {"action", action},
                {"entity_type", entity_type},
                {"injected", 0}
            };
        }

        const int changed_count = injected_count + removed_count + extended_count;
        if (changed_count > 0) {
            snapshot = advance_one_tick_locked(0.0, false, false, false);
            listeners = snapshot_listeners_;
        } else {
            snapshot = latest_snapshot_;
        }
    }

    const int changed_count = injected_count + removed_count + extended_count;
    if (changed_count > 0 && !snapshot.topology.nodes.empty()) {
        WSHandler::broadcast_json({
            {"type", "topology_tick"},
            {"snapshot", snapshot.to_json()}
        });
        WSHandler::broadcast_json({
            {"type", "metrics_tick"},
            {"sim_time", snapshot.sim_time},
            {"topology_version", snapshot.topology_version},
            {"metrics", snapshot.to_json()["metrics"]}
        });
    }
    for (const auto& evt : events) {
        WSHandler::broadcast_json(evt);
    }

    for (const auto& kv : listeners) {
        try {
            kv.second(snapshot);
        } catch (const std::exception& e) {
            spdlog::warn("Snapshot listener {} failed after manual fault inject: {}", kv.first, e.what());
        }
    }

    return {
        {"ok", true},
        {"action", request.value("action", std::string("inject"))},
        {"entity_type", request.value("entity_type", std::string("node"))},
        {"injected", injected_count},
        {"removed", removed_count},
        {"extended", extended_count},
        {"skipped_existing", skipped_existing},
        {"invalid_targets", invalid_targets},
        {"events", events},
        {"status", status_json()}
    };
}

void DynamicSimulationService::register_snapshot_listener(
    const std::string& listener_id,
    SnapshotListener listener
) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_listeners_[listener_id] = std::move(listener);
}

void DynamicSimulationService::unregister_snapshot_listener(const std::string& listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_listeners_.erase(listener_id);
}

TopologySnapshot DynamicSimulationService::advance_one_tick_locked(
    double sim_dt_sec,
    bool emit_events,
    bool allow_random_fault_generation,
    bool advance_fault_timers
) {
    Topology topology = load_working_topology_locked();
    if (topology.nodes.empty()) {
        latest_snapshot_ = {};
        return latest_snapshot_;
    }

    sim_elapsed_sec_ += std::max(0.0, sim_dt_sec);
    topology_version_ += 1;

    const std::string sim_time = current_sim_time_iso_locked();
    std::unordered_map<std::string, DeploymentOrchestratorService::NodeRuntimeSnapshot> runtime_snapshot;
    if (g_deployment_orchestrator) {
        runtime_snapshot = g_deployment_orchestrator->snapshot_node_runtime();
    }

    for (auto& sat : topology.nodes) {
        update_satellite_position(sat, sim_dt_sec);
        // Rebuild node fault view from source-of-truth `node_fault_states_` each tick.
        // This prevents stale "down/fault_tag" residue after manual fault removal.
        sat.status = "active";
        sat.fault_tag.clear();

        const auto [alloc_cpu, alloc_mem, alloc_disk] = res_mgr_->get_allocated_node_resources(sat.id);
        const size_t hv = std::hash<std::string>{}(sat.id);
        const double phase = 0.001 * static_cast<double>(topology_version_ * 29 + static_cast<int>(hv % 4096));

        const double cpu_factor = clamp(0.78 + 0.16 * std::sin(phase) + 0.08 * std::cos(phase * 0.61), 0.45, 1.0);
        const double mem_factor = clamp(0.80 + 0.14 * std::sin(phase * 1.07 + 0.9) + 0.07 * std::cos(phase * 0.73), 0.48, 1.0);
        const double disk_factor = clamp(0.87 + 0.08 * std::sin(phase * 0.83 + 1.7) + 0.04 * std::cos(phase * 0.57), 0.62, 1.0);

        const double dyn_cpu_cap = clamp(sat.cpu_total * cpu_factor, 0.0, sat.cpu_total);
        const double dyn_mem_cap = clamp(sat.mem_total * mem_factor, 0.0, sat.mem_total);
        const double dyn_disk_cap = clamp(sat.disk_total * disk_factor, 0.0, sat.disk_total);

        sat.cpu_available = clamp(dyn_cpu_cap - alloc_cpu, 0.0, sat.cpu_total);
        sat.mem_available = clamp(dyn_mem_cap - alloc_mem, 0.0, sat.mem_total);
        sat.disk_available = clamp(dyn_disk_cap - alloc_disk, 0.0, sat.disk_total);

        const double cpu_util = sat.cpu_total > 1e-9 ? 1.0 - sat.cpu_available / sat.cpu_total : 1.0;
        const double mem_util = sat.mem_total > 1e-9 ? 1.0 - sat.mem_available / sat.mem_total : 1.0;
        const double disk_util = sat.disk_total > 1e-9 ? 1.0 - sat.disk_available / sat.disk_total : 1.0;
        const double avg_util = clamp((cpu_util + mem_util + disk_util) / 3.0, 0.0, 1.0);
        const auto rt_it = runtime_snapshot.find(sat.id);
        if (rt_it != runtime_snapshot.end() && rt_it->second.deployed) {
            sat.core_business_load = rt_it->second.core_business_load;
            sat.core_business_load.normalize_inplace();
            sat.core_network_load = clamp(rt_it->second.core_network_load, 0.0, 1.0);
        } else {
            sat.core_business_load = CoreBusinessLoad{};
            sat.core_business_load.signaling_load = 0.0;
            sat.core_business_load.session_load = 0.0;
            sat.core_business_load.user_plane_load = 0.0;
            sat.core_business_load.mobility_load = 0.0;
            sat.core_business_load.policy_load = 0.0;
            sat.core_business_load.auth_load = 0.0;
            sat.core_network_load = 0.0;
        }
        sat.node_reliability = clamp(0.997 - 0.045 * avg_util, 0.93, 0.9995);
    }

    std::vector<nlohmann::json> change_events;
    const auto wall_now = std::chrono::steady_clock::now();
    auto fault_expired = [&](FaultState& state) {
        if (!advance_fault_timers) return false;
        if (state.expires_at == std::chrono::steady_clock::time_point{}) {
            const double fallback_remaining =
                std::max(1.0, static_cast<double>(state.ttl_ticks) * std::max(1.0, sampling_interval_sec_));
            state.duration_sec = std::max(state.duration_sec, fallback_remaining);
            state.expires_at =
                wall_now + std::chrono::milliseconds(static_cast<int64_t>(fallback_remaining * 1000.0));
        }
        const double remaining_sec = remaining_fault_seconds(state, wall_now);
        state.ttl_ticks = ttl_ticks_from_seconds(remaining_sec, sampling_interval_sec_);
        return remaining_sec <= 1e-6;
    };

    for (auto it = node_fault_states_.begin(); it != node_fault_states_.end();) {
        if (fault_expired(it->second)) {
            const std::string sat_id = it->first;
            const std::string fault_type = it->second.fault_type;
            for (auto& sat : topology.nodes) {
                if (sat.id == sat_id) {
                    sat.status = "active";
                    sat.fault_tag.clear();
                    break;
                }
            }
            change_events.push_back({
                {"type", "recovery_event"},
                {"entity_type", "node"},
                {"entity_id", sat_id},
                {"fault_type", fault_type},
                {"reason", fault_type},
                {"sim_time", sim_time},
                {"topology_version", topology_version_}
            });
            it = node_fault_states_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = link_fault_states_.begin(); it != link_fault_states_.end();) {
        if (fault_expired(it->second)) {
            const std::string link_key = it->first;
            const std::string fault_type = it->second.fault_type;
            const auto [source, target] = split_link_key(link_key);
            change_events.push_back({
                {"type", "recovery_event"},
                {"entity_type", "link"},
                {"entity_id", link_key},
                {"source", source},
                {"target", target},
                {"fault_type", fault_type},
                {"reason", fault_type},
                {"sim_time", sim_time},
                {"topology_version", topology_version_}
            });
            it = link_fault_states_.erase(it);
        } else {
            ++it;
        }
    }

    (void)allow_random_fault_generation;

    for (auto& sat : topology.nodes) {
        const auto fault_it = node_fault_states_.find(sat.id);
        if (fault_it != node_fault_states_.end()) {
            sat.status = "down";
            sat.fault_tag = fault_it->second.fault_type;
            sat.cpu_available = 0.0;
            sat.mem_available = 0.0;
            sat.disk_available = 0.0;
            sat.core_business_load = CoreBusinessLoad{};
            sat.core_business_load.signaling_load = 0.0;
            sat.core_business_load.session_load = 0.0;
            sat.core_business_load.user_plane_load = 0.0;
            sat.core_business_load.mobility_load = 0.0;
            sat.core_business_load.policy_load = 0.0;
            sat.core_business_load.auth_load = 0.0;
            sat.core_network_load = 0.0;
            sat.node_reliability = 0.0;
        }
    }

    std::unordered_map<std::string, const Satellite*> node_map;
    node_map.reserve(topology.nodes.size() * 2);
    for (const auto& sat : topology.nodes) node_map[sat.id] = &sat;

    for (auto& link : topology.links) {
        link.fault_tag.clear();
        const std::string link_key = canonical_link_key(link.source, link.target);
        const auto link_fault_it = link_fault_states_.find(link_key);
        if (link_fault_it != link_fault_states_.end()) {
            link.status = "down";
            link.fault_tag = link_fault_it->second.fault_type.empty()
                ? "link_fault" : link_fault_it->second.fault_type;
            link.bandwidth_available_gbps = 0.0;
            continue;
        }

        const auto src_it = node_map.find(link.source);
        const auto dst_it = node_map.find(link.target);
        if (src_it == node_map.end() || dst_it == node_map.end()) {
            link.status = "down";
            link.fault_tag.clear();
            link.bandwidth_available_gbps = 0.0;
            continue;
        }

        const Satellite* src = src_it->second;
        const Satellite* dst = dst_it->second;
        const double d_km = link_distance_km(src->coordinates, dst->coordinates);
        link.latency_ms = (d_km / kLightSpeedKmPerSec) * 1000.0;

        const double alt_km = std::max(src->orbital_params.altitude_km, dst->orbital_params.altitude_km);
        const bool los_ok = d_km <= max_isl_range_km(alt_km);
        const bool endpoint_down = src->status == "down" || dst->status == "down";
        (void)allow_random_fault_generation;
        if (endpoint_down) {
            link.status = "down";
            link.fault_tag = "endpoint_node_fault";
            link.bandwidth_available_gbps = 0.0;
            continue;
        }
        if (!los_ok) {
            link.status = "down";
            link.fault_tag.clear();
            link.bandwidth_available_gbps = 0.0;
            continue;
        }

        const size_t hv = std::hash<std::string>{}(link.source + "|" + link.target);
        const double phase = 0.001 * static_cast<double>(topology_version_ * 37 + static_cast<int>(hv % 4096));
        const double dynamic_factor = clamp(0.62 + 0.24 * std::sin(phase) + 0.14 * std::cos(phase * 0.73), 0.15, 1.0);
        const double dynamic_capacity_gbps = clamp(
            link.bandwidth_gbps * dynamic_factor,
            0.0,
            link.bandwidth_gbps
        );
        const double allocated_gbps = res_mgr_->get_allocated_link_bandwidth(link.source, link.target);
        link.bandwidth_available_gbps = clamp(
            dynamic_capacity_gbps - allocated_gbps,
            0.0,
            link.bandwidth_gbps
        );
        if (link.bandwidth_gbps > 1e-6 && link.bandwidth_available_gbps <= 0.2 * link.bandwidth_gbps) {
            link.status = "congested";
        } else {
            link.status = "active";
        }

        const double dist_factor = clamp(1.0 - d_km / std::max(1.0, max_isl_range_km(alt_km)), 0.0, 1.0);
        link.reliability = clamp(0.989 + 0.010 * dist_factor, 0.97, 0.9998);
    }

    TopologySnapshot snapshot;
    snapshot.topology = topology;
    snapshot.sampling_interval_sec = sampling_interval_sec_;
    snapshot.topology_version = topology_version_;
    snapshot.sim_time = sim_time;

    snapshot.topology.metadata.topology_version = snapshot.topology_version;
    snapshot.topology.metadata.sampling_interval_sec = snapshot.sampling_interval_sec;
    snapshot.topology.metadata.sim_time = snapshot.sim_time;
    snapshot.topology.metadata.timestamp = snapshot.sim_time;

    snapshot.metrics.total_nodes = static_cast<int>(snapshot.topology.nodes.size());
    double core_load_sum = 0.0;
    double signaling_sum = 0.0;
    double session_sum = 0.0;
    double user_plane_sum = 0.0;
    double mobility_sum = 0.0;
    double policy_sum = 0.0;
    double auth_sum = 0.0;
    for (const auto& sat : snapshot.topology.nodes) {
        if (sat.status == "down") snapshot.metrics.down_nodes += 1;
        else snapshot.metrics.active_nodes += 1;
        core_load_sum += clamp(sat.core_network_load, 0.0, 1.0);
        signaling_sum += clamp(sat.core_business_load.signaling_load, 0.0, 1.0);
        session_sum += clamp(sat.core_business_load.session_load, 0.0, 1.0);
        user_plane_sum += clamp(sat.core_business_load.user_plane_load, 0.0, 1.0);
        mobility_sum += clamp(sat.core_business_load.mobility_load, 0.0, 1.0);
        policy_sum += clamp(sat.core_business_load.policy_load, 0.0, 1.0);
        auth_sum += clamp(sat.core_business_load.auth_load, 0.0, 1.0);
    }
    if (snapshot.metrics.total_nodes > 0) {
        const double denom = static_cast<double>(snapshot.metrics.total_nodes);
        snapshot.metrics.avg_core_network_load = core_load_sum / denom;
        snapshot.metrics.avg_signaling_load = signaling_sum / denom;
        snapshot.metrics.avg_session_load = session_sum / denom;
        snapshot.metrics.avg_user_plane_load = user_plane_sum / denom;
        snapshot.metrics.avg_mobility_load = mobility_sum / denom;
        snapshot.metrics.avg_policy_load = policy_sum / denom;
        snapshot.metrics.avg_auth_load = auth_sum / denom;
    } else {
        snapshot.metrics.avg_core_network_load = 0.0;
        snapshot.metrics.avg_signaling_load = 0.0;
        snapshot.metrics.avg_session_load = 0.0;
        snapshot.metrics.avg_user_plane_load = 0.0;
        snapshot.metrics.avg_mobility_load = 0.0;
        snapshot.metrics.avg_policy_load = 0.0;
        snapshot.metrics.avg_auth_load = 0.0;
    }

    snapshot.metrics.total_links = static_cast<int>(snapshot.topology.links.size());
    double latency_sum = 0.0;
    double util_sum = 0.0;
    int util_count = 0;
    for (const auto& link : snapshot.topology.links) {
        if (link.status == "down") snapshot.metrics.down_links += 1;
        else snapshot.metrics.active_links += 1;
        if (link.status == "congested") snapshot.metrics.congested_links += 1;
        latency_sum += link.latency_ms;
        if (link.bandwidth_gbps > 1e-9) {
            util_sum += 1.0 - clamp(link.bandwidth_available_gbps / link.bandwidth_gbps, 0.0, 1.0);
            util_count += 1;
        }
    }
    snapshot.metrics.avg_latency_ms = snapshot.metrics.total_links > 0
        ? latency_sum / static_cast<double>(snapshot.metrics.total_links)
        : 0.0;
    snapshot.metrics.avg_bandwidth_utilization = util_count > 0
        ? util_sum / static_cast<double>(util_count)
        : 0.0;

    topo_mgr_->save_current_topology(snapshot.topology);
    res_mgr_->load_topology(snapshot.topology);
    // Do NOT persist every dynamic tick to runtime_state.topology_snapshot.
    // Topology snapshot persistence is handled by explicit control-plane actions
    // (generate/import/delete/redeploy). Persisting here can cause cross-instance
    // overwrite races when multiple backend processes are accidentally running.
    latest_snapshot_ = snapshot;

    if (emit_events) {
        WSHandler::broadcast_json({
            {"type", "topology_tick"},
            {"snapshot", snapshot.to_json()}
        });
        WSHandler::broadcast_json({
            {"type", "metrics_tick"},
            {"sim_time", snapshot.sim_time},
            {"topology_version", snapshot.topology_version},
            {"metrics", snapshot.to_json()["metrics"]}
        });
        for (const auto& evt : change_events) {
            WSHandler::broadcast_json(evt);
        }
    }

    return snapshot;
}

void DynamicSimulationService::run_loop() {
    while (running_.load()) {
        const auto sleep_ms = static_cast<int64_t>(sampling_interval_sec_ * 1000.0);
        int64_t remaining_ms = std::max<int64_t>(50, sleep_ms);
        while (running_.load() && remaining_ms > 0) {
            const int64_t slice_ms = std::min<int64_t>(200, remaining_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            remaining_ms -= slice_ms;
            if (!running_.load()) break;

            TopologySnapshot fault_snapshot;
            std::unordered_map<std::string, SnapshotListener> fault_listeners;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto now = std::chrono::steady_clock::now();
                const auto has_expired = [&](const auto& states) {
                    return std::any_of(states.begin(), states.end(), [&](const auto& kv) {
                        const auto expires_at = kv.second.expires_at;
                        return expires_at != std::chrono::steady_clock::time_point{} && expires_at <= now;
                    });
                };
                if (has_expired(node_fault_states_) || has_expired(link_fault_states_)) {
                    fault_snapshot = advance_one_tick_locked(0.0, true, false, true);
                    fault_listeners = snapshot_listeners_;
                }
            }
            if (!fault_snapshot.topology.nodes.empty()) {
                for (const auto& kv : fault_listeners) {
                    try {
                        kv.second(fault_snapshot);
                    } catch (const std::exception& e) {
                        spdlog::warn("Snapshot listener {} failed after fault expiry: {}", kv.first, e.what());
                    }
                }
            }
        }

        if (!running_.load()) {
            break;
        }
        TopologySnapshot snapshot;
        std::unordered_map<std::string, SnapshotListener> listeners;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = advance_one_tick_locked(sampling_interval_sec_ * simulation_speed_, true);
            listeners = snapshot_listeners_;
        }
        for (const auto& kv : listeners) {
            try {
                kv.second(snapshot);
            } catch (const std::exception& e) {
                spdlog::warn("Snapshot listener {} failed: {}", kv.first, e.what());
            }
        }
    }
}

std::string DynamicSimulationService::iso_time_from_system_clock(const std::chrono::system_clock::time_point& tp) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
    const auto sec = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const std::time_t tt = std::chrono::system_clock::to_time_t(sec);
    const int millis = static_cast<int>(ms.count() % 1000);

    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&tt));
    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << millis << "Z";
    return oss.str();
}

void DynamicSimulationService::update_satellite_position(Satellite& sat, double dt_sec) {
    sgp4::propagate_inplace(sat, std::max(0.0, dt_sec) / 60.0);
}

double DynamicSimulationService::link_distance_km(const Coordinates& a, const Coordinates& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double DynamicSimulationService::max_isl_range_km(double altitude_km) {
    const double h = std::max(50.0, altitude_km);
    const double horizon = std::sqrt(2.0 * kEarthRadiusKm * h + h * h);
    return 1.9 * horizon;
}

double DynamicSimulationService::clamp(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

} // namespace sfc
