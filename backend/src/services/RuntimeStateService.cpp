#include "services/RuntimeStateService.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <spdlog/spdlog.h>

namespace sfc {
namespace {

std::string read_all_from_fd(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) break;
        out.append(buf.data(), static_cast<size_t>(n));
    }
    return out;
}

bool write_all_to_fd(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

double json_number(const nlohmann::json& obj, const char* key, double fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj[key];
    if (v.is_number_float()) return v.get<double>();
    if (v.is_number_integer()) return static_cast<double>(v.get<int64_t>());
    if (v.is_number_unsigned()) return static_cast<double>(v.get<uint64_t>());
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

int json_int(const nlohmann::json& obj, const char* key, int fallback) {
    return static_cast<int>(std::llround(json_number(obj, key, static_cast<double>(fallback))));
}

std::string json_string(const nlohmann::json& obj, const char* key, const std::string& fallback = "") {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
    if (v.is_number_float()) {
        std::ostringstream oss;
        oss << v.get<double>();
        return oss.str();
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return fallback;
}

}  // namespace

RuntimeStateService::RuntimeStateService(RuntimeDBConfig config)
    : config_(std::move(config)) {}

bool RuntimeStateService::init_schema() {
    std::string stderr_out;
    const std::string state_sql =
        "CREATE TABLE IF NOT EXISTS runtime_state ("
        "state_key VARCHAR(64) PRIMARY KEY,"
        "value_json LONGTEXT NOT NULL,"
        "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
    if (!exec_sql(state_sql, nullptr, &stderr_out)) {
        spdlog::error("init runtime_state failed: {}", stderr_out);
        return false;
    }

    const std::string events_sql =
        "CREATE TABLE IF NOT EXISTS runtime_events ("
        "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
        "event_type VARCHAR(64) NOT NULL,"
        "sim_time VARCHAR(64) NOT NULL DEFAULT '',"
        "payload_json LONGTEXT NOT NULL,"
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "INDEX idx_runtime_events_created_at (created_at),"
        "INDEX idx_runtime_events_event_type (event_type)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
    if (!exec_sql(events_sql, nullptr, &stderr_out)) {
        spdlog::error("init runtime_events failed: {}", stderr_out);
        return false;
    }

    const std::string seed_deployments_sql =
        "INSERT IGNORE INTO runtime_state (state_key, value_json) VALUES ('deployments', '[]')";
    if (!exec_sql(seed_deployments_sql, nullptr, &stderr_out)) {
        spdlog::error("seed runtime_state deployments failed: {}", stderr_out);
        return false;
    }
    const std::string seed_control_sql =
        "INSERT IGNORE INTO runtime_state (state_key, value_json) VALUES ('control_config', '{}')";
    if (!exec_sql(seed_control_sql, nullptr, &stderr_out)) {
        spdlog::error("seed runtime_state control_config failed: {}", stderr_out);
        return false;
    }
    return true;
}

bool RuntimeStateService::save_topology(const Topology& topology, const std::string& constellation_template) {
    std::string template_name = constellation_template;
    if (template_name.empty()) {
        const nlohmann::json current = get_state_json("topology_snapshot");
        if (current.is_object()) {
            template_name = json_string(current, "constellation_template", "");
        }
        if (template_name.empty()) template_name = "custom";
    }
    nlohmann::json payload = topology.to_json();
    payload["constellation_template"] = template_name;
    return set_state_json("topology_snapshot", payload);
}

bool RuntimeStateService::load_topology(Topology* topology, std::string* constellation_template) const {
    if (!topology) return false;
    const nlohmann::json payload = get_state_json("topology_snapshot");
    if (!payload.is_object() || payload.empty()) {
        return false;
    }
    if (constellation_template) {
        *constellation_template = json_string(payload, "constellation_template", "");
    }
    return parse_topology_json(payload, topology);
}

bool RuntimeStateService::save_deployments_json(const nlohmann::json& deployments) {
    if (!deployments.is_array()) return false;
    return set_state_json("deployments", deployments);
}

nlohmann::json RuntimeStateService::load_deployments_json() const {
    const nlohmann::json value = get_state_json("deployments");
    return value.is_array() ? value : nlohmann::json::array();
}

bool RuntimeStateService::save_control_config(const nlohmann::json& config) {
    if (!config.is_object()) return false;
    return set_state_json("control_config", config);
}

nlohmann::json RuntimeStateService::load_control_config() const {
    const nlohmann::json value = get_state_json("control_config");
    return value.is_object() ? value : nlohmann::json::object();
}

bool RuntimeStateService::append_runtime_event(const nlohmann::json& event) {
    if (!event.is_object()) return false;
    const std::string event_type = json_string(event, "type", "event");
    const std::string sim_time = json_string(event, "sim_time", "");
    std::string stderr_out;
    const std::string sql =
        "INSERT INTO runtime_events (event_type, sim_time, payload_json) VALUES (" +
        sql_quote(event_type) + ", " + sql_quote(sim_time) + ", " + sql_quote(event.dump()) + ")";
    if (!exec_sql(sql, nullptr, &stderr_out)) {
        spdlog::warn("append_runtime_event failed: {}", stderr_out);
        return false;
    }

    // Keep the event table bounded to avoid unbounded growth.
    // This keeps the latest 1200 runtime events.
    const std::string trim_sql =
        "DELETE FROM runtime_events WHERE id NOT IN ("
        "SELECT id FROM (SELECT id FROM runtime_events ORDER BY id DESC LIMIT 1200) AS t"
        ")";
    if (!exec_sql(trim_sql, nullptr, &stderr_out)) {
        spdlog::warn("trim runtime_events failed: {}", stderr_out);
    }
    return true;
}

nlohmann::json RuntimeStateService::list_runtime_events(int limit) const {
    const int bounded_limit = std::max(1, std::min(limit, 600));
    std::string stdout_out;
    std::string stderr_out;
    const std::string sql =
        "SELECT id,event_type,sim_time,payload_json FROM runtime_events "
        "ORDER BY id DESC LIMIT " + std::to_string(bounded_limit);
    if (!exec_sql(sql, &stdout_out, &stderr_out)) {
        spdlog::warn("list_runtime_events failed: {}", stderr_out);
        return nlohmann::json::array();
    }

    nlohmann::json out = nlohmann::json::array();
    for (const auto& line : split_lines(stdout_out)) {
        const auto fields = split_tab_line(line);
        if (fields.size() < 4) continue;

        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(fields[3]);
        } catch (...) {
            payload = nlohmann::json::object();
        }
        payload["db_id"] = fields[0];
        if (!payload.contains("type")) payload["type"] = fields[1];
        if (!payload.contains("sim_time")) payload["sim_time"] = fields[2];
        out.push_back(payload);
    }
    return out;
}

bool RuntimeStateService::clear_runtime_events() {
    std::string stderr_out;
    return exec_sql("DELETE FROM runtime_events", nullptr, &stderr_out);
}

bool RuntimeStateService::clear_deployments() {
    return set_state_json("deployments", nlohmann::json::array());
}

bool RuntimeStateService::set_state_json(const std::string& key, const nlohmann::json& value) {
    std::string stderr_out;
    const std::string sql =
        "INSERT INTO runtime_state (state_key, value_json) VALUES (" +
        sql_quote(key) + ", " + sql_quote(value.dump()) + ") "
        "ON DUPLICATE KEY UPDATE value_json = VALUES(value_json)";
    if (!exec_sql(sql, nullptr, &stderr_out)) {
        spdlog::warn("set_state_json({}, ...) failed: {}", key, stderr_out);
        return false;
    }
    return true;
}

nlohmann::json RuntimeStateService::get_state_json(const std::string& key) const {
    std::string stdout_out;
    std::string stderr_out;
    const std::string sql =
        "SELECT value_json FROM runtime_state WHERE state_key=" + sql_quote(key) + " LIMIT 1";
    if (!exec_sql(sql, &stdout_out, &stderr_out)) {
        spdlog::warn("get_state_json({}) failed: {}", key, stderr_out);
        return nlohmann::json();
    }
    const auto lines = split_lines(stdout_out);
    if (lines.empty()) return nlohmann::json();
    const auto fields = split_tab_line(lines.front());
    if (fields.empty()) return nlohmann::json();
    try {
        return nlohmann::json::parse(fields[0]);
    } catch (...) {
        return nlohmann::json();
    }
}

bool RuntimeStateService::exec_sql(const std::string& sql, std::string* stdout_out, std::string* stderr_out) const {
    std::lock_guard<std::mutex> lock(exec_mutex_);
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || pipe(in_pipe) != 0) {
        if (stderr_out) *stderr_out = "pipe_failed";
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        close(in_pipe[0]);
        close(in_pipe[1]);
        if (stderr_out) *stderr_out = "fork_failed";
        return false;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);

        const std::string user_arg = "-u" + config_.user;
        const std::string pass_arg = "-p" + config_.password;
        std::vector<std::string> args = {
            "docker", "exec", "-i", config_.container_name, "mysql",
            "-N", "-B", user_arg, pass_arg, "-D", config_.database,
        };
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp("docker", argv.data());
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    const std::string sql_with_terminator = sql + ";\n";
    const bool write_ok = write_all_to_fd(in_pipe[1], sql_with_terminator);
    close(in_pipe[1]);
    const std::string out = read_all_from_fd(out_pipe[0]);
    const std::string err = read_all_from_fd(err_pipe[0]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (stdout_out) *stdout_out = out;
    if (stderr_out) {
        if (write_ok) *stderr_out = err;
        else *stderr_out = err.empty() ? "write_sql_stdin_failed" : err;
    }
    return write_ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string RuntimeStateService::sql_quote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'') out.append("''");
        else if (c == '\\') out.append("\\\\");
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::vector<std::string> RuntimeStateService::split_tab_line(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, '\t')) {
        fields.push_back(item);
    }
    return fields;
}

std::vector<std::string> RuntimeStateService::split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

bool RuntimeStateService::parse_topology_json(const nlohmann::json& root, Topology* topology) {
    if (!topology || !root.is_object()) return false;

    const nlohmann::json* meta = nullptr;
    const nlohmann::json* top = nullptr;
    if (root.contains("metadata") && root["metadata"].is_object()) meta = &root["metadata"];
    if (root.contains("topology") && root["topology"].is_object()) top = &root["topology"];
    if (!top) top = &root;

    const nlohmann::json* nodes = nullptr;
    const nlohmann::json* links = nullptr;
    if (top->contains("nodes") && (*top)["nodes"].is_array()) nodes = &(*top)["nodes"];
    if (top->contains("links") && (*top)["links"].is_array()) links = &(*top)["links"];
    if (!nodes && root.contains("nodes") && root["nodes"].is_array()) nodes = &root["nodes"];
    if (!links && root.contains("links") && root["links"].is_array()) links = &root["links"];

    Topology parsed;
    parsed.metadata.total_sats = meta ? json_int(*meta, "total_sats", 0) : 0;
    parsed.metadata.num_planes = meta ? json_int(*meta, "num_planes", 0) : 0;
    parsed.metadata.altitude_km = meta ? json_number(*meta, "altitude_km", 0.0) : 0.0;
    parsed.metadata.inclination_deg = meta ? json_number(*meta, "inclination_deg", 53.0) : 53.0;
    parsed.metadata.topology_version = meta ? json_int(*meta, "topology_version", 0) : 0;
    parsed.metadata.sampling_interval_sec = meta ? json_number(*meta, "sampling_interval_sec", 5.0) : 5.0;
    parsed.metadata.sim_time = meta ? json_string(*meta, "sim_time", "") : "";
    parsed.metadata.timestamp = meta ? json_string(*meta, "timestamp", "") : "";

    if (nodes && nodes->is_array()) {
        for (const auto& n : *nodes) {
            if (!n.is_object()) continue;
            Satellite sat;
            sat.id = json_string(n, "id", "");
            if (sat.id.empty()) continue;
            sat.type = json_string(n, "type", "satellite");

            const auto op = n.contains("orbital_params") && n["orbital_params"].is_object()
                                ? n["orbital_params"]
                                : nlohmann::json::object();
            sat.orbital_params.plane = json_int(op, "plane", 0);
            sat.orbital_params.position_in_plane = json_int(op, "position_in_plane", 0);
            sat.orbital_params.raan = json_number(op, "raan", 0.0);
            sat.orbital_params.true_anomaly = json_number(op, "true_anomaly", 0.0);
            sat.orbital_params.altitude_km = json_number(op, "altitude_km", parsed.metadata.altitude_km);
            sat.orbital_params.inclination_deg = json_number(
                op,
                "inclination_deg",
                json_number(op, "inclination", parsed.metadata.inclination_deg));

            const auto coord = n.contains("coordinates") && n["coordinates"].is_object()
                                   ? n["coordinates"]
                                   : nlohmann::json::object();
            sat.coordinates.x = json_number(coord, "x", 0.0);
            sat.coordinates.y = json_number(coord, "y", 0.0);
            sat.coordinates.z = json_number(coord, "z", 0.0);
            sat.coordinates.lat = json_number(coord, "lat", 0.0);
            sat.coordinates.lon = json_number(coord, "lon", 0.0);

            sat.cpu_total = json_number(n, "cpu_total", 0.0);
            sat.cpu_available = json_number(n, "cpu_available", sat.cpu_total);
            sat.mem_total = json_number(n, "mem_total", 0.0);
            sat.mem_available = json_number(n, "mem_available", sat.mem_total);
            sat.disk_total = json_number(n, "disk_total", 0.0);
            sat.disk_available = json_number(n, "disk_available", sat.disk_total);
            sat.core_network_load = std::max(0.0, std::min(1.0, json_number(n, "core_network_load", 0.0)));
            sat.core_business_load = CoreBusinessLoad{};
            if (n.contains("core_business_load") && n["core_business_load"].is_object()) {
                const auto& cb = n["core_business_load"];
                sat.core_business_load.signaling_load = json_number(cb, "signaling_load", sat.core_network_load);
                sat.core_business_load.session_load = json_number(cb, "session_load", sat.core_network_load);
                sat.core_business_load.user_plane_load = json_number(cb, "user_plane_load", sat.core_network_load);
                sat.core_business_load.mobility_load = json_number(cb, "mobility_load", sat.core_network_load);
                sat.core_business_load.policy_load = json_number(cb, "policy_load", sat.core_network_load);
                sat.core_business_load.auth_load = json_number(cb, "auth_load", sat.core_network_load);
            } else {
                sat.core_business_load.signaling_load = sat.core_network_load;
                sat.core_business_load.session_load = sat.core_network_load;
                sat.core_business_load.user_plane_load = sat.core_network_load;
                sat.core_business_load.mobility_load = sat.core_network_load;
                sat.core_business_load.policy_load = sat.core_network_load;
                sat.core_business_load.auth_load = sat.core_network_load;
            }
            sat.core_business_load.normalize_inplace();
            if (sat.core_network_load <= 1e-9) {
                sat.core_network_load = sat.core_business_load.load_index();
            }

            sat.node_reliability = json_number(n, "node_reliability", 0.98);
            sat.status = json_string(n, "status", "active");
            sat.fault_tag = json_string(n, "fault_tag", "");

            const auto vnfs = n.contains("core_nfs") && n["core_nfs"].is_array()
                                  ? n["core_nfs"]
                                  : (n.contains("vnfs") && n["vnfs"].is_array()
                                         ? n["vnfs"]
                                         : nlohmann::json::array());
            for (const auto& item : vnfs) {
                if (!item.is_object()) continue;
                VNFDeployment d;
                d.vnf_id = json_string(item, "vnf_id", json_string(item, "core_nf_id", ""));
                d.vnf_type = json_string(item, "vnf_type", json_string(item, "core_nf_type", ""));
                d.core_nf_id = json_string(item, "core_nf_id", d.vnf_id);
                d.core_nf_type = json_string(item, "core_nf_type", d.vnf_type);
                d.nf_role = json_string(item, "nf_role", "");
                d.resource_profile = json_string(item, "resource_profile", "");
                d.node = json_string(item, "node", sat.id);
                d.cpu_used = json_number(item, "cpu_used", 0.0);
                d.mem_used = json_number(item, "mem_used", 0.0);
                d.disk_used = json_number(item, "disk_used", 0.0);
                d.sfc_id = json_string(item, "sfc_id", "");
                sat.vnfs.push_back(std::move(d));
            }

            parsed.nodes.push_back(std::move(sat));
        }
    }

    if (links && links->is_array()) {
        for (const auto& l : *links) {
            if (!l.is_object()) continue;
            Link link;
            link.source = json_string(l, "source", json_string(l, "src", ""));
            link.target = json_string(l, "target", json_string(l, "dst", ""));
            if (link.source.empty() || link.target.empty()) continue;
            link.link_type = json_string(l, "link_type", "inter_orbit");
            link.status = json_string(l, "status", "active");
            link.fault_tag = json_string(l, "fault_tag", "");
            link.latency_ms = json_number(l, "latency_ms", 0.0);
            link.reliability = json_number(l, "reliability", json_number(l, "link_reliability", 0.999));
            link.bandwidth_gbps = json_number(l, "bandwidth_gbps", 0.0);
            link.bandwidth_available_gbps =
                json_number(l, "bandwidth_available_gbps", link.bandwidth_gbps);
            parsed.links.push_back(std::move(link));
        }
    }

    parsed.metadata.total_sats = static_cast<int>(parsed.nodes.size());
    if (parsed.metadata.num_planes <= 0) {
        int max_plane = -1;
        for (const auto& sat : parsed.nodes) {
            max_plane = std::max(max_plane, sat.orbital_params.plane);
        }
        parsed.metadata.num_planes = max_plane + 1;
    }
    *topology = std::move(parsed);
    return true;
}

}  // namespace sfc
