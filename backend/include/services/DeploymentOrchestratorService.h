#pragma once

#include "models/types.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sfc {

class DeploymentOrchestratorService {
  public:
    struct NodeRuntimeSnapshot {
        std::string node_id;
        std::string container_name;
        std::string container_state;  // stopped, starting, running, failed
        std::vector<std::string> running_core_nf_types;
        bool service_probe_ok = false;
        bool deployed = false;
        CoreBusinessLoad core_business_load{};
        double core_network_load = 0.0;
    };

    DeploymentOrchestratorService();
    ~DeploymentOrchestratorService();

    void start();
    void stop();

    void enqueue_deployment(
        const std::string& deployment_id,
        const std::string& request_id,
        const DeploymentCandidate& candidate,
        const std::vector<VNF>& request_vnfs,
        const std::string& mode,
        const std::string& trigger
    );
    bool rollback_deployment(
        const std::string& deployment_id,
        const std::vector<std::string>& nodes_hint = {}
    );

    std::unordered_map<std::string, NodeRuntimeSnapshot> snapshot_node_runtime() const;
    std::optional<NodeRuntimeSnapshot> get_node_runtime(const std::string& node_id) const;

  private:
    struct OrchestrationTask {
        uint64_t sequence = 0;
        std::string deployment_id;
        std::string request_id;
        DeploymentCandidate candidate;
        std::vector<VNF> request_vnfs;
        std::string mode;
        std::string trigger;
    };

    struct DeploymentRuntimeState {
        std::string deployment_id;
        std::vector<std::string> active_nodes;
        std::vector<std::string> active_containers;
    };

    void worker_loop();
    void process_task(const OrchestrationTask& task);

    static std::string iso_now();
    static std::string normalize_nf_type(const std::string& nf_type);
    static CoreBusinessLoad compute_business_load_for_nfs(
        const std::vector<std::string>& nf_types,
        bool service_probe_ok
    );

    bool ensure_satellite_image_available(std::string* image_out, std::string* reason_out);
    bool validate_satellite_image(
        const std::string& image,
        std::string* reason_out = nullptr
    ) const;
    bool ensure_network();
    bool ensure_mongo_container(std::string* reason_out = nullptr);
    bool stop_container(const std::string& container_name);
    bool is_container_running(const std::string& container_name) const;
    bool wait_for_container_running(
        const std::string& container_name,
        int attempts = 8,
        int interval_ms = 250
    ) const;
    void log_container_diagnostics(
        const std::string& container_name,
        const std::string& context
    ) const;
    bool ensure_satellite_container(
        const std::string& container_name,
        const std::string& image,
        const std::string& platform
    );
    std::string inspect_container_ip(const std::string& container_name) const;
    bool write_config_to_container(
        const std::string& container_name,
        const std::string& nf_type,
        const std::string& content
    ) const;
    std::string normalize_rendered_nf_config(
        const std::string& nf_type,
        const std::string& raw_config,
        bool smf_upf_colocated
    ) const;
    std::string render_nf_config(
        const std::string& nf_type,
        const std::string& local_ip,
        const std::string& nrf_uri,
        const std::string& upf_ip,
        const std::string& mongo_uri,
        const std::string& scp_uri,
        bool smf_upf_colocated
    ) const;
    std::string daemon_for_nf_type(const std::string& nf_type) const;
    std::string nf_type_to_3gpp(const std::string& nf_type) const;
    int sbi_port_for_nf_type(const std::string& nf_type) const;
    bool nf_uses_mongo(const std::string& nf_type) const;
    bool nf_registers_to_nrf(const std::string& nf_type) const;
    bool start_nf_in_container(
        const std::string& container_name,
        const std::string& nf_type,
        const std::string& config_content,
        bool smf_upf_colocated
    );
    bool ensure_container_tun_device(const std::string& container_name) const;
    bool setup_upf_dataplane(const std::string& container_name) const;
    bool check_nrf_registration(
        const std::string& nrf_container,
        const std::string& nrf_ip,
        const std::vector<std::string>& started_nfs
    ) const;
    bool check_smf_upf_pfcp_ready(
        const std::string& smf_container,
        const std::string& upf_container,
        int timeout_seconds = 20
    ) const;
    bool run_shell_command_capture(
        const std::string& cmd,
        std::string* output,
        int* code = nullptr
    ) const;
    bool run_shell_command(const std::string& cmd, int* code = nullptr) const;
    static std::string node_to_container_name(
        const std::string& deployment_id,
        const std::string& node_id
    );
    static std::string legacy_node_container_name(const std::string& node_id);

    void update_deployment_runtime_state(
        const std::string& deployment_id,
        const nlohmann::json& patch,
        bool broadcast
    );
    void update_node_runtime_state(
        const std::string& node_id,
        const NodeRuntimeSnapshot& snapshot
    );
    void clear_nodes_for_deployment(const std::vector<std::string>& old_nodes);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool stop_requested_ = false;
    uint64_t seq_ = 0;
    std::thread worker_;
    std::deque<OrchestrationTask> queue_;
    std::unordered_map<std::string, DeploymentRuntimeState> deployment_runtime_;
    std::unordered_map<std::string, NodeRuntimeSnapshot> node_runtime_;
};

}  // namespace sfc
