#pragma once

#include "models/types.h"
#include "services/ResourceManager.h"
#include "services/TopologyManager.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace sfc {

class DynamicSimulationService {
public:
    using SnapshotListener = std::function<void(const TopologySnapshot&)>;

    DynamicSimulationService(
        std::shared_ptr<TopologyManager> topo_mgr,
        std::shared_ptr<ResourceManager> res_mgr
    );
    ~DynamicSimulationService();

    bool start(
        double sampling_interval_sec = 5.0,
        double simulation_speed = 1.0,
        bool enable_faults = true,
        double node_fault_prob_per_tick = 0.0002,
        double link_fault_prob_per_tick = 0.0005
    );

    void stop();
    bool is_running() const;

    TopologySnapshot step_once();
    TopologySnapshot get_latest_snapshot() const;

    nlohmann::json status_json() const;
    void register_snapshot_listener(const std::string& listener_id, SnapshotListener listener);
    void unregister_snapshot_listener(const std::string& listener_id);

private:
    TopologySnapshot advance_one_tick_locked(double sim_dt_sec, bool emit_events);
    void run_loop();

    static std::string iso_time_from_system_clock(const std::chrono::system_clock::time_point& tp);
    static void update_satellite_position(
        Satellite& sat,
        double inclination_deg,
        double dt_sec
    );
    static double link_distance_km(const Coordinates& a, const Coordinates& b);
    static double max_isl_range_km(double altitude_km);
    static double clamp(double v, double lo, double hi);

    std::shared_ptr<TopologyManager> topo_mgr_;
    std::shared_ptr<ResourceManager> res_mgr_;

    mutable std::mutex mutex_;
    std::atomic<bool> running_;
    std::thread loop_thread_;

    double sampling_interval_sec_;
    double simulation_speed_;
    bool enable_faults_;
    double node_fault_prob_per_tick_;
    double link_fault_prob_per_tick_;
    int topology_version_;
    double sim_elapsed_sec_;
    std::chrono::system_clock::time_point sim_epoch_;
    std::chrono::steady_clock::time_point last_wall_tick_;

    TopologySnapshot latest_snapshot_;
    std::unordered_map<std::string, int> node_fault_ttl_;
    std::unordered_map<std::string, int> link_fault_ttl_;
    std::unordered_map<std::string, SnapshotListener> snapshot_listeners_;
    mutable std::mt19937 rng_;
};

} // namespace sfc
