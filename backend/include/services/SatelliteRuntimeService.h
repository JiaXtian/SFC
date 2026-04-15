#pragma once

#include "models/types.h"
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfc {

class SatelliteRuntimeService {
public:
    struct Config {
        bool enabled = false;
        std::string podman_bin = "podman";
        std::string default_image = "docker.io/library/alpine:3.20";
        bool auto_pull_image = true;
        bool recreate_existing = false;
        int telemetry_interval_sec = 5;
        double cpu_limit_per_sat = 0.01;
        int mem_limit_mb_per_sat = 8;
        int tmpfs_limit_mb_per_sat = 4;
        int pids_limit_per_sat = 32;
        bool lightweight_create_only = true;
        int provision_parallelism = 24;
        int start_parallelism = 24;
        int stop_parallelism = 48;
    };

    SatelliteRuntimeService();

    void configure(const Config& cfg);
    Config config() const;
    bool enabled() const;

    void apply_template_to_topology(Topology& topology, const std::string& template_id);
    nlohmann::json provision_constellation(
        Topology& topology,
        const std::string& template_id,
        bool recreate_existing
    );
    nlohmann::json collect_node_telemetry(Topology& topology, bool force = false);
    nlohmann::json stop_satellite_nodes(
        Topology& topology,
        const std::vector<std::string>& node_ids,
        bool remove_containers = false
    );
    nlohmann::json start_satellite_nodes(
        Topology& topology,
        const std::vector<std::string>& node_ids,
        bool recreate_containers = false
    );
    nlohmann::json stop_all_satellite_nodes(Topology& topology, bool remove_containers = false);
    void forget_nodes(const std::vector<std::string>& node_ids);

    void mark_fault_state(
        Topology& topology,
        const std::string& node_id,
        bool injected,
        const std::string& fault_tag
    );
    void mark_deployment_policy(
        Topology& topology,
        const std::vector<std::string>& node_ids,
        const std::string& policy_name,
        bool applied,
        const std::string& detail
    );

    nlohmann::json runtime_status() const;

private:
    struct TemplateProfile {
        std::string id;
        std::string display_name;
        double default_altitude_km = 550.0;
        double default_inclination_deg = 53.0;
        std::pair<double, double> cpu_range = {12.0, 24.0};
        std::pair<double, double> mem_range = {24.0, 48.0};
        std::pair<double, double> disk_range = {120.0, 360.0};
        double container_cpu_limit = 0.01;
        int container_mem_limit_mb = 8;
    };

    struct NodeRuntimeState {
        std::string container_name;
        std::string container_id;
        std::string status = "not_created";
        std::string template_id = "starlink_v1";
        bool fault_injected = false;
        std::string fault_tag;
        bool core_nf_policy_applied = false;
        std::string core_nf_policy;
        std::string deployment_state = "none";
        std::string deployment_detail;
        double cpu_utilization_ratio = 0.0;
        double mem_utilization_ratio = 0.0;
        double disk_utilization_ratio = 0.0;
        std::string last_collected_at;
    };

    struct ProvisionProgress {
        bool active = false;
        std::string phase = "idle";
        int requested = 0;
        int processed = 0;
        int started = 0;
        int failed = 0;
        int concurrency = 0;
        std::string started_at;
        std::string updated_at;
        std::vector<std::string> recent_logs;
    };

    void init_templates();
    const TemplateProfile& resolve_template(const std::string& template_id) const;
    std::string make_container_name(const std::string& satellite_id) const;
    static std::string iso_now();
    static std::vector<std::string> split_lines(const std::string& raw);
    static std::vector<std::string> split_by(const std::string& raw, char delim);

    nlohmann::json ensure_image_available_locked();
    nlohmann::json refresh_runtime_state_from_podman_locked();
    nlohmann::json create_or_start_node(
        const std::string& sat_id,
        const TemplateProfile& tpl,
        bool recreate,
        bool ensure_running
    );
    nlohmann::json stop_node(const std::string& sat_id, bool remove_container);
    void append_progress_log_locked(const std::string& line);
    void update_progress_locked(
        const std::string& phase,
        int requested,
        int processed,
        int started,
        int failed,
        bool active
    );
    nlohmann::json runtime_status_locked() const;
    void simulate_node_resource_usage_locked(Topology& topology, const std::string& ts);

    void apply_runtime_state_to_satellite(Satellite* sat, const NodeRuntimeState& rt) const;

    mutable std::mutex mutex_;
    Config config_;
    std::unordered_map<std::string, TemplateProfile> templates_;
    std::unordered_map<std::string, NodeRuntimeState> runtime_;
    ProvisionProgress progress_;
    std::mt19937 rng_;
    std::chrono::steady_clock::time_point last_collect_wall_;
};

} // namespace sfc
