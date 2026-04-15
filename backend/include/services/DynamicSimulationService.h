#pragma once

#include "models/types.h"
#include "services/ResourceManager.h"
#include "services/TopologyManager.h"
#include "services/SatelliteRuntimeService.h"
#include "services/PersistenceService.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sfc {

class DynamicSimulationService {
public:
    using SnapshotListener = std::function<void(const TopologySnapshot&)>;
    struct FaultState {
        int ttl_ticks = 0;
        std::string fault_type;
        std::string injection_mode; // random/manual
    };

    DynamicSimulationService(
        std::shared_ptr<TopologyManager> topo_mgr,
        std::shared_ptr<ResourceManager> res_mgr,
        std::shared_ptr<SatelliteRuntimeService> sat_runtime = nullptr,
        std::shared_ptr<PersistenceService> persistence = nullptr
    );
    ~DynamicSimulationService();

    bool start(
        double sampling_interval_sec = 5.0,
        double simulation_speed = 1.0,
        bool enable_faults = false,
        double node_fault_prob_per_tick = 0.0,
        double link_fault_prob_per_tick = 0.0
    );

    void stop();
    bool is_running() const;

    TopologySnapshot step_once();
    TopologySnapshot get_latest_snapshot() const;

    nlohmann::json status_json() const;
    nlohmann::json inject_faults(const nlohmann::json& request);
    void register_snapshot_listener(const std::string& listener_id, SnapshotListener listener);
    void unregister_snapshot_listener(const std::string& listener_id);

private:
    TopologySnapshot advance_one_tick_locked(
        double sim_dt_sec,
        bool emit_events,
        bool allow_random_fault_generation = false,
        bool advance_fault_timers = true
    );
    void run_loop();
    Topology load_working_topology_locked() const;
    std::string current_sim_time_iso_locked() const;

    static std::string iso_time_from_system_clock(const std::chrono::system_clock::time_point& tp);
    static void update_satellite_position(
        Satellite& sat,
        double inclination_deg,
        double dt_sec
    );
    static double link_distance_km(const Coordinates& a, const Coordinates& b);
    static double max_isl_range_km(double altitude_km);
    static double clamp(double v, double lo, double hi);
    static const std::vector<std::string>& node_fault_catalog();
    static const std::vector<std::string>& link_fault_catalog();
    std::string pick_node_fault_type();
    std::string pick_link_fault_type();

    std::shared_ptr<TopologyManager> topo_mgr_;
    std::shared_ptr<ResourceManager> res_mgr_;
    std::shared_ptr<SatelliteRuntimeService> sat_runtime_;
    std::shared_ptr<PersistenceService> persistence_;

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
    std::unordered_map<std::string, FaultState> node_fault_states_;
    std::unordered_map<std::string, FaultState> link_fault_states_;
    std::unordered_map<std::string, SnapshotListener> snapshot_listeners_;
    mutable std::mt19937 rng_;
};

} // namespace sfc
