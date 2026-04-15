#pragma once

#include "models/types.h"
#include <mutex>
#include <string>

namespace sfc {

class PersistenceService {
public:
    struct MySQLConfig {
        bool enabled = true;
        std::string container_runtime = "docker";
        std::string container_name = "sfc-mysql";
        std::string podman_container = "sfc-mysql";
        std::string user = "sfc";
        std::string password = "sfc123456";
        std::string database = "sfc_runtime";
    };

    struct Config {
        bool enabled = true;
        bool strict_startup = true;
        int snapshot_interval_ticks = 1;
        std::string fallback_dir = "data/persistence_fallback";
        MySQLConfig mysql;
    };

    PersistenceService();

    void configure(const Config& cfg);
    Config config() const;

    nlohmann::json initialize_schema();
    nlohmann::json persist_topology_snapshot(
        const Topology& topology,
        const std::string& trigger,
        bool force = false
    );
    nlohmann::json persist_event(
        const std::string& event_type,
        const std::string& entity_id,
        const std::string& detail
    );
    nlohmann::json reset_all_data();

    void set_constellation_id(const std::string& constellation_id);
    std::string constellation_id() const;

    nlohmann::json status() const;

private:
    static std::string iso_now();
    static std::string sql_escape(const std::string& raw);
    static std::string json_escape(const std::string& raw);
    static std::string canonical_link_key(const std::string& src, const std::string& dst);

    std::string mysql_exec_cmd_locked(const std::string& sql) const;
    nlohmann::json persist_mysql_locked(const Topology& topology, const std::string& trigger);
    nlohmann::json persist_file_locked(const Topology& topology, const std::string& trigger) const;

    mutable std::mutex mutex_;
    Config config_;
    bool initialized_ = false;
    std::string constellation_id_;
    int tick_counter_ = 0;
    std::string last_error_;
};

} // namespace sfc
