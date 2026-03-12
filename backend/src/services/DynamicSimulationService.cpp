#include "services/DynamicSimulationService.h"
#include "websocket/WSHandler.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <spdlog/spdlog.h>
#include <sstream>

namespace sfc {
namespace {

constexpr double kEarthRadiusKm = 6371.0;
constexpr double kEarthMuKm3PerSec2 = 398600.4418;
constexpr double kLightSpeedKmPerSec = 299792.458;
constexpr double kPi = 3.14159265358979323846;

std::string make_link_key(const std::string& source, const std::string& target) {
    return source + "->" + target;
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
    enable_faults_(true),
    node_fault_prob_per_tick_(0.0002),
    link_fault_prob_per_tick_(0.0005),
    topology_version_(0),
    sim_elapsed_sec_(0.0),
    sim_epoch_(std::chrono::system_clock::now()),
    last_wall_tick_(std::chrono::steady_clock::now()),
    rng_(std::random_device{}()) {}

DynamicSimulationService::~DynamicSimulationService() {
    stop();
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
    enable_faults_ = enable_faults;
    node_fault_prob_per_tick_ = clamp(node_fault_prob_per_tick, 0.0, 0.1);
    link_fault_prob_per_tick_ = clamp(link_fault_prob_per_tick, 0.0, 0.2);

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
    node_fault_ttl_.clear();
    link_fault_ttl_.clear();
    last_wall_tick_ = std::chrono::steady_clock::now();
    loop_thread_ = std::thread(&DynamicSimulationService::run_loop, this);
    spdlog::info(
        "Dynamic simulation started: interval={}s, speed={}x, faults={}",
        sampling_interval_sec_,
        simulation_speed_,
        enable_faults_ ? "on" : "off"
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
    return {
        {"running", running_.load()},
        {"sampling_interval_sec", sampling_interval_sec_},
        {"simulation_speed", simulation_speed_},
        {"enable_faults", enable_faults_},
        {"node_fault_prob_per_tick", node_fault_prob_per_tick_},
        {"link_fault_prob_per_tick", link_fault_prob_per_tick_},
        {"topology_version", topology_version_},
        {"sim_time", latest_snapshot_.sim_time},
        {"metrics", latest_snapshot_.to_json().value("metrics", nlohmann::json::object())}
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

TopologySnapshot DynamicSimulationService::advance_one_tick_locked(double sim_dt_sec, bool emit_events) {
    Topology topology = res_mgr_->export_current_topology();
    if (topology.nodes.empty()) {
        topology = topo_mgr_->get_current_topology();
    }
    if (topology.nodes.empty()) {
        latest_snapshot_ = {};
        return latest_snapshot_;
    }

    sim_elapsed_sec_ += std::max(0.0, sim_dt_sec);
    topology_version_ += 1;

    const double inclination_deg = topology.metadata.inclination_deg;
    for (auto& sat : topology.nodes) {
        update_satellite_position(sat, inclination_deg, sim_dt_sec);
        if (sat.status.empty()) sat.status = "active";
    }

    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<int> ttl_ticks(2, 6);
    std::vector<nlohmann::json> change_events;

    for (auto it = node_fault_ttl_.begin(); it != node_fault_ttl_.end();) {
        it->second -= 1;
        if (it->second <= 0) {
            for (auto& sat : topology.nodes) {
                if (sat.id == it->first) {
                    sat.status = "active";
                    sat.fault_tag.clear();
                    change_events.push_back({
                        {"type", "recovery_event"},
                        {"entity_type", "node"},
                        {"entity_id", sat.id},
                        {"sim_time", iso_time_from_system_clock(sim_epoch_ + std::chrono::milliseconds(static_cast<int64_t>(sim_elapsed_sec_ * 1000.0)))},
                        {"topology_version", topology_version_}
                    });
                    break;
                }
            }
            it = node_fault_ttl_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = link_fault_ttl_.begin(); it != link_fault_ttl_.end();) {
        it->second -= 1;
        if (it->second <= 0) {
            change_events.push_back({
                {"type", "recovery_event"},
                {"entity_type", "link"},
                {"entity_id", it->first},
                {"sim_time", iso_time_from_system_clock(sim_epoch_ + std::chrono::milliseconds(static_cast<int64_t>(sim_elapsed_sec_ * 1000.0)))},
                {"topology_version", topology_version_}
            });
            it = link_fault_ttl_.erase(it);
        } else {
            ++it;
        }
    }

    if (enable_faults_) {
        for (auto& sat : topology.nodes) {
            if (sat.status == "down") continue;
            if (prob(rng_) < node_fault_prob_per_tick_) {
                sat.status = "down";
                sat.fault_tag = "random_node_fault";
                node_fault_ttl_[sat.id] = ttl_ticks(rng_);
                change_events.push_back({
                    {"type", "fault_event"},
                    {"entity_type", "node"},
                    {"entity_id", sat.id},
                    {"reason", "random_node_fault"},
                    {"sim_time", iso_time_from_system_clock(sim_epoch_ + std::chrono::milliseconds(static_cast<int64_t>(sim_elapsed_sec_ * 1000.0)))},
                    {"topology_version", topology_version_}
                });
            }
        }
    }

    std::unordered_map<std::string, const Satellite*> node_map;
    node_map.reserve(topology.nodes.size() * 2);
    for (const auto& sat : topology.nodes) node_map[sat.id] = &sat;

    for (auto& link : topology.links) {
        const auto src_it = node_map.find(link.source);
        const auto dst_it = node_map.find(link.target);
        if (src_it == node_map.end() || dst_it == node_map.end()) {
            link.status = "down";
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
        const std::string lk = make_link_key(link.source, link.target);

        if (enable_faults_ && !endpoint_down && los_ok && prob(rng_) < link_fault_prob_per_tick_) {
            link_fault_ttl_[lk] = ttl_ticks(rng_);
            change_events.push_back({
                {"type", "fault_event"},
                {"entity_type", "link"},
                {"entity_id", lk},
                {"reason", "random_link_fault"},
                {"sim_time", iso_time_from_system_clock(sim_epoch_ + std::chrono::milliseconds(static_cast<int64_t>(sim_elapsed_sec_ * 1000.0)))},
                {"topology_version", topology_version_}
            });
        }

        const bool link_faulted = link_fault_ttl_.find(lk) != link_fault_ttl_.end();
        if (endpoint_down || !los_ok || link_faulted) {
            link.status = "down";
            link.bandwidth_available_gbps = 0.0;
            continue;
        }

        const size_t hv = std::hash<std::string>{}(link.source + "|" + link.target);
        const double phase = 0.001 * static_cast<double>(topology_version_ * 37 + static_cast<int>(hv % 4096));
        const double dynamic_factor = clamp(0.15, 1.0, 0.62 + 0.24 * std::sin(phase) + 0.14 * std::cos(phase * 0.73));
        link.bandwidth_available_gbps = clamp(
            link.bandwidth_gbps * dynamic_factor,
            0.0,
            link.bandwidth_gbps
        );
        if (link.bandwidth_gbps > 1e-6 && link.bandwidth_available_gbps <= 0.2 * link.bandwidth_gbps) {
            link.status = "congested";
        } else {
            link.status = "active";
        }

        const double dist_factor = clamp(1.0 - d_km / std::max(1.0, max_isl_range_km(alt_km)), 0.0, 1.0);
        // Keep dynamic-link reliability in an engineering-realistic high band.
        // This avoids practical multi-hop paths (target <=25 hops) being rejected too aggressively.
        link.reliability = clamp(0.985 + 0.014 * dist_factor, 0.95, 0.9997);
    }

    TopologySnapshot snapshot;
    snapshot.topology = topology;
    snapshot.sampling_interval_sec = sampling_interval_sec_;
    snapshot.topology_version = topology_version_;
    snapshot.sim_time = iso_time_from_system_clock(
        sim_epoch_ + std::chrono::milliseconds(static_cast<int64_t>(sim_elapsed_sec_ * 1000.0))
    );

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
