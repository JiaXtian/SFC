#include "services/DynamicSimulationService.h"
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

constexpr double kEarthRadiusKm = 6371.0;
constexpr double kEarthMuKm3PerSec2 = 398600.4418;
constexpr double kLightSpeedKmPerSec = 299792.458;
constexpr double kPi = 3.14159265358979323846;

const std::vector<std::string> kNodeFaultTypes = {
    "power_failure",
    "cpu_overload",
    "thermal_shutdown",
    "control_plane_sync_loss",
    "software_crash",
    "clock_drift"
};

const std::vector<std::string> kLinkFaultTypes = {
    // Link fault injection is disabled. Keep placeholder for backward compatibility.
};

std::unordered_set<std::string> make_catalog_set(const std::vector<std::string>& values) {
    return std::unordered_set<std::string>(values.begin(), values.end());
}

const std::unordered_set<std::string> kNodeFaultTypeSet = make_catalog_set(kNodeFaultTypes);

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
    sampling_interval_sec_(5.0),
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
    stop();
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
    sampling_interval_sec_ = clamp(sampling_interval_sec, 1.0, 30.0);
    simulation_speed_ = clamp(simulation_speed, 0.1, 20.0);
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
    return true;
}

void DynamicSimulationService::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
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
    nlohmann::json node_fault_details = nlohmann::json::array();
    for (const auto& kv : node_fault_states_) {
        node_fault_details.push_back({
            {"node_id", kv.first},
            {"fault_type", kv.second.fault_type},
            {"injection_mode", kv.second.injection_mode},
            {"ttl_ticks", kv.second.ttl_ticks},
            {"remaining_sec", kv.second.ttl_ticks * sampling_interval_sec_}
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
            {"link", 0}
        }},
        {"active_fault_details", {
            {"node", node_fault_details},
            {"link", nlohmann::json::array()}
        }},
        {"fault_catalog", {
            {"node", node_fault_catalog()},
            {"link", nlohmann::json::array()}
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
        const int ttl_ticks = static_cast<int>(clamp(static_cast<double>(requested_ttl), 1.0, 120.0));
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

                    node_fault_states_[node_id] = FaultState{ttl_ticks, fault_type, "manual"};
                    injected_count += 1;
                    events.push_back({
                        {"type", "fault_event"},
                        {"entity_type", "node"},
                        {"entity_id", node_id},
                        {"fault_type", fault_type},
                        {"reason", fault_type},
                        {"injection_mode", "manual"},
                        {"ttl_ticks", ttl_ticks},
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
                    int delta_ttl_ticks = static_cast<int>(request.value("delta_ttl_ticks", 0));
                    if (delta_ttl_ticks <= 0 && request.contains("delta_seconds")) {
                        const double delta_seconds = std::max(0.0, request.value("delta_seconds", 0.0));
                        delta_ttl_ticks = static_cast<int>(std::ceil(delta_seconds / std::max(1.0, sampling_interval_sec_)));
                    }
                    delta_ttl_ticks = static_cast<int>(clamp(static_cast<double>(delta_ttl_ticks), 1.0, 3600.0));
                    it->second.ttl_ticks = static_cast<int>(clamp(
                        static_cast<double>(it->second.ttl_ticks + delta_ttl_ticks),
                        1.0,
                        7200.0
                    ));
                    extended_count += 1;
                    events.push_back({
                        {"type", "fault_update_event"},
                        {"entity_type", "node"},
                        {"entity_id", node_id},
                        {"fault_type", it->second.fault_type},
                        {"reason", "ttl_extended"},
                        {"injection_mode", "manual_extend"},
                        {"delta_ttl_ticks", delta_ttl_ticks},
                        {"ttl_ticks", it->second.ttl_ticks},
                        {"sim_time", sim_time},
                        {"topology_version", topology_version_ + 1}
                    });
                    continue;
                }
            }
        } else if (entity_type == "link") {
            return {
                {"ok", false},
                {"message", "Link fault injection is disabled; node faults only"},
                {"action", action},
                {"entity_type", entity_type},
                {"injected", 0}
            };
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

    const double inclination_deg = topology.metadata.inclination_deg;
    for (auto& sat : topology.nodes) {
        update_satellite_position(sat, inclination_deg, sim_dt_sec);
        // Rebuild node fault view from source-of-truth `node_fault_states_` each tick.
        // This prevents stale "down/fault_tag" residue after manual fault removal.
        sat.status = "active";
        sat.fault_tag.clear();
    }

    std::vector<nlohmann::json> change_events;

    for (auto it = node_fault_states_.begin(); it != node_fault_states_.end();) {
        if (advance_fault_timers) {
            it->second.ttl_ticks -= 1;
        }
        if (advance_fault_timers && it->second.ttl_ticks <= 0) {
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

    (void)allow_random_fault_generation;

    for (auto& sat : topology.nodes) {
        const auto fault_it = node_fault_states_.find(sat.id);
        if (fault_it != node_fault_states_.end()) {
            sat.status = "down";
            sat.fault_tag = fault_it->second.fault_type;
        }
    }

    link_fault_states_.clear();

    std::unordered_map<std::string, const Satellite*> node_map;
    node_map.reserve(topology.nodes.size() * 2);
    for (const auto& sat : topology.nodes) node_map[sat.id] = &sat;

    for (auto& link : topology.links) {
        link.fault_tag.clear();

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
    for (const auto& sat : snapshot.topology.nodes) {
        if (sat.status == "down") snapshot.metrics.down_nodes += 1;
        else snapshot.metrics.active_nodes += 1;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<int64_t>(50, sleep_ms)));

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

void DynamicSimulationService::update_satellite_position(
    Satellite& sat,
    double inclination_deg,
    double dt_sec
) {
    const double altitude = std::max(100.0, sat.orbital_params.altitude_km);
    const double orbit_radius = kEarthRadiusKm + altitude;
    const double period_sec = 2.0 * kPi * std::sqrt(
        (orbit_radius * orbit_radius * orbit_radius) / kEarthMuKm3PerSec2
    );
    const double delta_deg = 360.0 * dt_sec / std::max(1.0, period_sec);
    sat.orbital_params.true_anomaly = std::fmod(sat.orbital_params.true_anomaly + delta_deg, 360.0);
    if (sat.orbital_params.true_anomaly < 0.0) {
        sat.orbital_params.true_anomaly += 360.0;
    }
    sat.orbital_params.inclination_deg = inclination_deg;

    const double raan_rad = sat.orbital_params.raan * kPi / 180.0;
    const double inc_rad = inclination_deg * kPi / 180.0;
    const double ta_rad = sat.orbital_params.true_anomaly * kPi / 180.0;

    const double x_orb = orbit_radius * std::cos(ta_rad);
    const double y_orb = orbit_radius * std::sin(ta_rad);

    sat.coordinates.x = std::cos(raan_rad) * x_orb - std::sin(raan_rad) * std::cos(inc_rad) * y_orb;
    sat.coordinates.y = std::sin(raan_rad) * x_orb + std::cos(raan_rad) * std::cos(inc_rad) * y_orb;
    sat.coordinates.z = std::sin(inc_rad) * y_orb;

    sat.coordinates.lat = std::asin(clamp(sat.coordinates.z / orbit_radius, -1.0, 1.0)) * 180.0 / kPi;
    sat.coordinates.lon = std::atan2(sat.coordinates.y, sat.coordinates.x) * 180.0 / kPi;
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
