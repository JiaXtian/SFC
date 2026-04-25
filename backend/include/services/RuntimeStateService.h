#pragma once

#include "models/types.h"

#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace sfc {

struct RuntimeDBConfig {
    std::string container_name = "sfc-mysql";
    std::string database = "sfc_runtime";
    std::string user = "sfc";
    std::string password = "sfc123456";
};

class RuntimeStateService {
  public:
    explicit RuntimeStateService(RuntimeDBConfig config);

    bool init_schema();

    bool save_topology(const Topology& topology, const std::string& constellation_template);
    bool load_topology(Topology* topology, std::string* constellation_template = nullptr) const;

    bool save_deployments_json(const nlohmann::json& deployments);
    nlohmann::json load_deployments_json() const;

    bool save_control_config(const nlohmann::json& config);
    nlohmann::json load_control_config() const;

    bool append_runtime_event(const nlohmann::json& event);
    nlohmann::json list_runtime_events(int limit) const;
    bool clear_runtime_events();

    bool clear_topology();
    bool clear_deployments();

  private:
    bool set_state_json(const std::string& key, const nlohmann::json& value);
    nlohmann::json get_state_json(const std::string& key) const;
    bool exec_sql(const std::string& sql, std::string* stdout_out, std::string* stderr_out) const;
    static std::string sql_quote(const std::string& value);
    static std::vector<std::string> split_tab_line(const std::string& line);
    static std::vector<std::string> split_lines(const std::string& text);
    static bool parse_topology_json(const nlohmann::json& root, Topology* topology);

    RuntimeDBConfig config_;
    mutable std::mutex exec_mutex_;
};

}  // namespace sfc
