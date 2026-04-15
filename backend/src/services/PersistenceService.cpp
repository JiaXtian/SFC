#include "services/PersistenceService.h"
#include "utils/CommandRunner.h"
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <spdlog/spdlog.h>

namespace sfc {
namespace {

constexpr size_t kMySQLBatchSize = 200;

std::string bool_to_sql(bool v) {
    return v ? "1" : "0";
}

std::string sql_escape_local(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    return out;
}

std::string sql_quote(const std::string& raw) {
    return "'" + sql_escape_local(raw) + "'";
}

} // namespace

PersistenceService::PersistenceService() = default;

void PersistenceService::configure(const Config& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
}

PersistenceService::Config PersistenceService::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

std::string PersistenceService::iso_now() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string PersistenceService::sql_escape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    return out;
}

std::string PersistenceService::json_escape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string PersistenceService::canonical_link_key(const std::string& src, const std::string& dst) {
    if (src <= dst) return src + "<->" + dst;
    return dst + "<->" + src;
}

void PersistenceService::set_constellation_id(const std::string& constellation_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    constellation_id_ = constellation_id;
}

std::string PersistenceService::constellation_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return constellation_id_;
}

std::string PersistenceService::mysql_exec_cmd_locked(const std::string& sql) const {
    std::string runtime = config_.mysql.container_runtime;
    if (runtime.empty()) runtime = "docker";
    for (auto& ch : runtime) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string container_name = config_.mysql.container_name.empty()
        ? config_.mysql.podman_container
        : config_.mysql.container_name;

    std::ostringstream cmd;
    cmd << runtime << " exec " << container_name << " mysql"
        << " -u" << config_.mysql.user;
    if (!config_.mysql.password.empty()) {
        cmd << " -p'" << shell_escape_single_quotes(config_.mysql.password) << "'";
    }
    cmd << " --default-character-set=utf8mb4 -e '"
        << shell_escape_single_quotes(sql) << "'";
    return cmd.str();
}

nlohmann::json PersistenceService::initialize_schema() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::create_directories(config_.fallback_dir);

    nlohmann::json result{
        {"ok", true},
        {"mysql", nlohmann::json::object()}
    };

    if (!config_.enabled) {
        initialized_ = true;
        result["message"] = "Persistence disabled";
        return result;
    }

    if (config_.mysql.enabled) {
        std::ostringstream sql;
        sql
            << "CREATE DATABASE IF NOT EXISTS " << config_.mysql.database << " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
            << "USE " << config_.mysql.database << ";"
            << "CREATE TABLE IF NOT EXISTS constellation_runs ("
            << "constellation_id VARCHAR(64) PRIMARY KEY,"
            << "template_id VARCHAR(64),"
            << "total_sats INT,"
            << "num_planes INT,"
            << "altitude_km DOUBLE,"
            << "inclination_deg DOUBLE,"
            << "created_at DATETIME,"
            << "updated_at DATETIME"
            << ");"
            << "CREATE TABLE IF NOT EXISTS satellite_state ("
            << "constellation_id VARCHAR(64) NOT NULL,"
            << "sat_id VARCHAR(64) NOT NULL,"
            << "template_id VARCHAR(64),"
            << "plane_idx INT,"
            << "position_in_plane INT,"
            << "raan DOUBLE,"
            << "true_anomaly DOUBLE,"
            << "altitude_km DOUBLE,"
            << "inclination_deg DOUBLE,"
            << "latitude DOUBLE,"
            << "longitude DOUBLE,"
            << "x DOUBLE,y DOUBLE,z DOUBLE,"
            << "cpu_total DOUBLE,cpu_available DOUBLE,"
            << "mem_total DOUBLE,mem_available DOUBLE,"
            << "disk_total DOUBLE,disk_available DOUBLE,"
            << "cpu_utilization DOUBLE,mem_utilization DOUBLE,disk_utilization DOUBLE,"
            << "node_status VARCHAR(32),"
            << "fault_tag VARCHAR(128),"
            << "fault_injected TINYINT,"
            << "podman_container_name VARCHAR(128),"
            << "podman_container_id VARCHAR(128),"
            << "podman_status VARCHAR(32),"
            << "deployment_state VARCHAR(64),"
            << "deployment_detail VARCHAR(255),"
            << "core_nf_policy VARCHAR(128),"
            << "core_nf_policy_applied TINYINT,"
            << "collected_at DATETIME,"
            << "updated_at DATETIME,"
            << "PRIMARY KEY (constellation_id, sat_id),"
            << "INDEX idx_sat_status (node_status),"
            << "INDEX idx_sat_podman (podman_status)"
            << ");"
            << "CREATE TABLE IF NOT EXISTS link_state ("
            << "constellation_id VARCHAR(64) NOT NULL,"
            << "link_key VARCHAR(160) NOT NULL,"
            << "source_id VARCHAR(64) NOT NULL,"
            << "target_id VARCHAR(64) NOT NULL,"
            << "link_type VARCHAR(32),"
            << "link_status VARCHAR(32),"
            << "fault_tag VARCHAR(128),"
            << "latency_ms DOUBLE,"
            << "reliability DOUBLE,"
            << "bandwidth_gbps DOUBLE,"
            << "bandwidth_available_gbps DOUBLE,"
            << "updated_at DATETIME,"
            << "PRIMARY KEY (constellation_id, link_key),"
            << "INDEX idx_link_status (link_status)"
            << ");"
            << "CREATE TABLE IF NOT EXISTS event_log ("
            << "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            << "constellation_id VARCHAR(64),"
            << "event_type VARCHAR(64),"
            << "entity_id VARCHAR(128),"
            << "detail VARCHAR(512),"
            << "created_at DATETIME"
            << ");";
        const auto schema = run_command(mysql_exec_cmd_locked(sql.str()));
        if (schema.exit_code != 0) {
            result["ok"] = false;
            result["mysql"] = {
                {"ok", false},
                {"message", "Failed to initialize MySQL schema"},
                {"details", schema.output}
            };
            last_error_ = schema.output;
        } else {
            result["mysql"] = {{"ok", true}};
        }
    } else {
        result["mysql"] = {{"ok", true}, {"enabled", false}};
    }

    initialized_ = true;
    return result;
}

nlohmann::json PersistenceService::persist_mysql_locked(
    const Topology& topology,
    const std::string& trigger
) {
    if (!config_.mysql.enabled) {
        return {{"ok", true}, {"enabled", false}};
    }

    const std::string now = iso_now();
    const std::string cid = constellation_id_;
    std::string template_id = "starlink_v1";
    if (!topology.nodes.empty()) template_id = topology.nodes.front().template_id;

    std::ostringstream upsert_run;
    upsert_run
        << "USE " << config_.mysql.database << ";"
        << "INSERT INTO constellation_runs (constellation_id, template_id, total_sats, num_planes, altitude_km, inclination_deg, created_at, updated_at) VALUES ("
        << sql_quote(cid) << ","
        << sql_quote(template_id) << ","
        << static_cast<int>(topology.nodes.size()) << ","
        << topology.metadata.num_planes << ","
        << topology.metadata.altitude_km << ","
        << topology.metadata.inclination_deg << ","
        << "STR_TO_DATE(" << sql_quote(now) << ", '%Y-%m-%dT%H:%i:%sZ'),"
        << "NOW()"
        << ") ON DUPLICATE KEY UPDATE "
        << "template_id=VALUES(template_id), total_sats=VALUES(total_sats), num_planes=VALUES(num_planes), "
        << "altitude_km=VALUES(altitude_km), inclination_deg=VALUES(inclination_deg), updated_at=NOW();";
    auto run_res = run_command(mysql_exec_cmd_locked(upsert_run.str()));
    if (run_res.exit_code != 0) {
        return {
            {"ok", false},
            {"message", "Failed to upsert constellation_runs"},
            {"details", run_res.output}
        };
    }

    size_t sat_written = 0;
    for (size_t start = 0; start < topology.nodes.size(); start += kMySQLBatchSize) {
        const size_t end = std::min(topology.nodes.size(), start + kMySQLBatchSize);
        std::ostringstream sql;
        sql << "USE " << config_.mysql.database << ";"
            << "INSERT INTO satellite_state ("
            << "constellation_id,sat_id,template_id,plane_idx,position_in_plane,raan,true_anomaly,altitude_km,inclination_deg,"
            << "latitude,longitude,x,y,z,cpu_total,cpu_available,mem_total,mem_available,disk_total,disk_available,"
            << "cpu_utilization,mem_utilization,disk_utilization,node_status,fault_tag,fault_injected,"
            << "podman_container_name,podman_container_id,podman_status,deployment_state,deployment_detail,core_nf_policy,core_nf_policy_applied,"
            << "collected_at,updated_at"
            << ") VALUES ";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            const auto& sat = topology.nodes[i];
            if (!first) sql << ",";
            first = false;
            const std::string collected = sat.last_collected_at.empty() ? now : sat.last_collected_at;
            sql
                << "("
                << sql_quote(cid) << ","
                << sql_quote(sat.id) << ","
                << sql_quote(sat.template_id) << ","
                << sat.orbital_params.plane << ","
                << sat.orbital_params.position_in_plane << ","
                << sat.orbital_params.raan << ","
                << sat.orbital_params.true_anomaly << ","
                << sat.orbital_params.altitude_km << ","
                << sat.orbital_params.inclination_deg << ","
                << sat.coordinates.lat << ","
                << sat.coordinates.lon << ","
                << sat.coordinates.x << ","
                << sat.coordinates.y << ","
                << sat.coordinates.z << ","
                << sat.cpu_total << ","
                << sat.cpu_available << ","
                << sat.mem_total << ","
                << sat.mem_available << ","
                << sat.disk_total << ","
                << sat.disk_available << ","
                << sat.cpu_utilization_ratio << ","
                << sat.mem_utilization_ratio << ","
                << sat.disk_utilization_ratio << ","
                << sql_quote(sat.status) << ","
                << sql_quote(sat.fault_tag) << ","
                << bool_to_sql(sat.fault_injected) << ","
                << sql_quote(sat.podman_container_name) << ","
                << sql_quote(sat.podman_container_id) << ","
                << sql_quote(sat.podman_status) << ","
                << sql_quote(sat.deployment_state) << ","
                << sql_quote(sat.deployment_detail) << ","
                << sql_quote(sat.core_nf_policy) << ","
                << bool_to_sql(sat.core_nf_policy_applied) << ","
                << "STR_TO_DATE(" << sql_quote(collected) << ", '%Y-%m-%dT%H:%i:%sZ'),"
                << "NOW()"
                << ")";
        }
        sql << " ON DUPLICATE KEY UPDATE "
            << "template_id=VALUES(template_id), plane_idx=VALUES(plane_idx), position_in_plane=VALUES(position_in_plane),"
            << "raan=VALUES(raan), true_anomaly=VALUES(true_anomaly), altitude_km=VALUES(altitude_km), inclination_deg=VALUES(inclination_deg),"
            << "latitude=VALUES(latitude), longitude=VALUES(longitude), x=VALUES(x), y=VALUES(y), z=VALUES(z),"
            << "cpu_total=VALUES(cpu_total), cpu_available=VALUES(cpu_available), mem_total=VALUES(mem_total), mem_available=VALUES(mem_available),"
            << "disk_total=VALUES(disk_total), disk_available=VALUES(disk_available),"
            << "cpu_utilization=VALUES(cpu_utilization), mem_utilization=VALUES(mem_utilization), disk_utilization=VALUES(disk_utilization),"
            << "node_status=VALUES(node_status), fault_tag=VALUES(fault_tag), fault_injected=VALUES(fault_injected),"
            << "podman_container_name=VALUES(podman_container_name), podman_container_id=VALUES(podman_container_id), podman_status=VALUES(podman_status),"
            << "deployment_state=VALUES(deployment_state), deployment_detail=VALUES(deployment_detail),"
            << "core_nf_policy=VALUES(core_nf_policy), core_nf_policy_applied=VALUES(core_nf_policy_applied),"
            << "collected_at=VALUES(collected_at), updated_at=NOW();";

        const auto r = run_command(mysql_exec_cmd_locked(sql.str()));
        if (r.exit_code != 0) {
            return {
                {"ok", false},
                {"message", "Failed to upsert satellite_state"},
                {"details", r.output}
            };
        }
        sat_written += (end - start);
    }

    size_t link_written = 0;
    for (size_t start = 0; start < topology.links.size(); start += kMySQLBatchSize) {
        const size_t end = std::min(topology.links.size(), start + kMySQLBatchSize);
        std::ostringstream sql;
        sql << "USE " << config_.mysql.database << ";"
            << "INSERT INTO link_state ("
            << "constellation_id,link_key,source_id,target_id,link_type,link_status,fault_tag,latency_ms,reliability,bandwidth_gbps,bandwidth_available_gbps,updated_at"
            << ") VALUES ";
        bool first = true;
        for (size_t i = start; i < end; ++i) {
            const auto& link = topology.links[i];
            if (!first) sql << ",";
            first = false;
            sql
                << "("
                << sql_quote(cid) << ","
                << sql_quote(canonical_link_key(link.source, link.target)) << ","
                << sql_quote(link.source) << ","
                << sql_quote(link.target) << ","
                << sql_quote(link.link_type) << ","
                << sql_quote(link.status) << ","
                << sql_quote(link.fault_tag) << ","
                << link.latency_ms << ","
                << link.reliability << ","
                << link.bandwidth_gbps << ","
                << link.bandwidth_available_gbps << ","
                << "NOW()"
                << ")";
        }
        sql << " ON DUPLICATE KEY UPDATE "
            << "source_id=VALUES(source_id), target_id=VALUES(target_id), link_type=VALUES(link_type), link_status=VALUES(link_status),"
            << "fault_tag=VALUES(fault_tag), latency_ms=VALUES(latency_ms), reliability=VALUES(reliability),"
            << "bandwidth_gbps=VALUES(bandwidth_gbps), bandwidth_available_gbps=VALUES(bandwidth_available_gbps), updated_at=NOW();";
        const auto r = run_command(mysql_exec_cmd_locked(sql.str()));
        if (r.exit_code != 0) {
            return {
                {"ok", false},
                {"message", "Failed to upsert link_state"},
                {"details", r.output}
            };
        }
        link_written += (end - start);
    }

    std::ostringstream event_sql;
    event_sql << "USE " << config_.mysql.database << ";"
              << "INSERT INTO event_log (constellation_id,event_type,entity_id,detail,created_at) VALUES ("
              << sql_quote(cid) << ","
              << sql_quote("snapshot") << ","
              << sql_quote(trigger) << ","
              << sql_quote("nodes=" + std::to_string(sat_written) + ",links=" + std::to_string(link_written)) << ","
              << "NOW()"
              << ");";
    (void)run_command(mysql_exec_cmd_locked(event_sql.str()));

    return {
        {"ok", true},
        {"satellite_rows", sat_written},
        {"link_rows", link_written}
    };
}

nlohmann::json PersistenceService::persist_file_locked(
    const Topology& topology,
    const std::string& trigger
) const {
    try {
        std::filesystem::create_directories(config_.fallback_dir);
        const std::string ts = iso_now();
        std::string file_ts = ts;
        for (auto& ch : file_ts) {
            if (ch == ':' || ch == '.') ch = '-';
        }
        const std::filesystem::path file = std::filesystem::path(config_.fallback_dir)
            / ("snapshot_" + file_ts + "_" + trigger + ".json");
        nlohmann::json payload = topology.to_json();
        payload["persist_meta"] = {
            {"constellation_id", constellation_id_},
            {"trigger", trigger},
            {"saved_at", ts}
        };
        std::ofstream ofs(file);
        ofs << payload.dump(2);
        return {{"ok", true}, {"file", file.string()}};
    } catch (const std::exception& e) {
        return {{"ok", false}, {"message", e.what()}};
    }
}

nlohmann::json PersistenceService::persist_topology_snapshot(
    const Topology& topology,
    const std::string& trigger,
    bool force
) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!initialized_) {
        lock.unlock();
        initialize_schema();
        lock.lock();
    }
    if (constellation_id_.empty()) {
        constellation_id_ = "constellation_" + std::to_string(
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())
        );
    }

    tick_counter_ += 1;
    const int interval = std::max(1, config_.snapshot_interval_ticks);
    if (!force && tick_counter_ % interval != 0) {
        return {
            {"ok", true},
            {"skipped", true},
            {"reason", "interval"},
            {"tick_counter", tick_counter_},
            {"interval", interval}
        };
    }

    nlohmann::json mysql_result = {{"ok", true}, {"enabled", false}};
    nlohmann::json file_result = persist_file_locked(topology, trigger);

    if (config_.enabled && config_.mysql.enabled) {
        mysql_result = persist_mysql_locked(topology, trigger);
    }
    const bool ok = mysql_result.value("ok", true) && file_result.value("ok", false);
    if (!ok) {
        last_error_ = "Persist snapshot failed";
    }
    return {
        {"ok", ok},
        {"constellation_id", constellation_id_},
        {"mysql", mysql_result},
        {"fallback_file", file_result}
    };
}

nlohmann::json PersistenceService::persist_event(
    const std::string& event_type,
    const std::string& entity_id,
    const std::string& detail
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled || !config_.mysql.enabled) {
        return {{"ok", true}, {"enabled", false}};
    }
    if (constellation_id_.empty()) {
        constellation_id_ = "constellation_" + std::to_string(
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())
        );
    }
    std::ostringstream sql;
    sql << "USE " << config_.mysql.database << ";"
        << "INSERT INTO event_log (constellation_id,event_type,entity_id,detail,created_at) VALUES ("
        << sql_quote(constellation_id_) << ","
        << sql_quote(event_type) << ","
        << sql_quote(entity_id) << ","
        << sql_quote(detail.substr(0, 500)) << ","
        << "NOW()"
        << ");";
    const auto res = run_command(mysql_exec_cmd_locked(sql.str()));
    if (res.exit_code != 0) {
        return {
            {"ok", false},
            {"message", "Failed to persist event"},
            {"details", res.output}
        };
    }
    return {{"ok", true}};
}

nlohmann::json PersistenceService::reset_all_data() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled || !config_.mysql.enabled) {
        return {{"ok", false}, {"message", "persistence/mysql disabled"}};
    }

    std::ostringstream sql;
    sql << "USE " << config_.mysql.database << ";"
        << "SET FOREIGN_KEY_CHECKS=0;"
        << "TRUNCATE TABLE link_state;"
        << "TRUNCATE TABLE satellite_state;"
        << "TRUNCATE TABLE event_log;"
        << "TRUNCATE TABLE constellation_runs;"
        << "SET FOREIGN_KEY_CHECKS=1;";

    const auto res = run_command(mysql_exec_cmd_locked(sql.str()));
    if (res.exit_code != 0) {
        last_error_ = res.output;
        return {
            {"ok", false},
            {"message", "failed to reset persistence tables"},
            {"details", res.output}
        };
    }

    constellation_id_.clear();
    tick_counter_ = 0;
    last_error_.clear();
    return {
        {"ok", true},
        {"message", "persistence reset complete"}
    };
}

nlohmann::json PersistenceService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"enabled", config_.enabled},
        {"strict_startup", config_.strict_startup},
        {"initialized", initialized_},
        {"constellation_id", constellation_id_},
        {"snapshot_interval_ticks", config_.snapshot_interval_ticks},
        {"mode", "mysql_only"},
        {"mysql", {
            {"enabled", config_.mysql.enabled},
            {"runtime", config_.mysql.container_runtime},
            {"container", config_.mysql.container_name.empty() ? config_.mysql.podman_container : config_.mysql.container_name},
            {"database", config_.mysql.database}
        }},
        {"fallback_dir", config_.fallback_dir},
        {"last_error", last_error_}
    };
}

} // namespace sfc
