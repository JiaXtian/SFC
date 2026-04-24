#include "services/DeploymentOrchestratorService.h"

#include "services/DeploymentStateStore.h"
#include "websocket/WSHandler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <spdlog/spdlog.h>

namespace sfc {
namespace {

constexpr const char* kSatelliteNetwork = "sfc-open5gs-net";
constexpr const char* kDefaultSatelliteImage = "ghcr.io/open5gs/open5gs:latest";

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

std::string getenv_str(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) return "";
    return std::string(v);
}

std::string resolve_satellite_image() {
    const std::string explicit_image = getenv_str("SFC_SATELLITE_IMAGE");
    if (!explicit_image.empty()) return explicit_image;
#if defined(__aarch64__) || defined(__arm64__)
    const std::string arm64_image = getenv_str("SFC_SATELLITE_IMAGE_ARM64");
    if (!arm64_image.empty()) return arm64_image;
#elif defined(__x86_64__)
    const std::string amd64_image = getenv_str("SFC_SATELLITE_IMAGE_AMD64");
    if (!amd64_image.empty()) return amd64_image;
    const std::string x86_64_image = getenv_str("SFC_SATELLITE_IMAGE_X86_64");
    if (!x86_64_image.empty()) return x86_64_image;
#endif
    return std::string(kDefaultSatelliteImage);
}

std::vector<std::string> unique_nf_types(const std::vector<std::string>& values) {
    std::set<std::string> uniq;
    for (const auto& v : values) {
        if (!v.empty()) uniq.insert(v);
    }
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

CoreBusinessLoad zero_business_load() {
    CoreBusinessLoad load{};
    load.signaling_load = 0.0;
    load.session_load = 0.0;
    load.user_plane_load = 0.0;
    load.mobility_load = 0.0;
    load.policy_load = 0.0;
    load.auth_load = 0.0;
    return load;
}

}  // namespace

DeploymentOrchestratorService::DeploymentOrchestratorService() = default;

DeploymentOrchestratorService::~DeploymentOrchestratorService() {
    stop();
}

void DeploymentOrchestratorService::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    stop_requested_ = false;
    running_ = true;
    worker_ = std::thread(&DeploymentOrchestratorService::worker_loop, this);
    spdlog::info("DeploymentOrchestratorService started");
}

void DeploymentOrchestratorService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    queue_.clear();
    spdlog::info("DeploymentOrchestratorService stopped");
}

void DeploymentOrchestratorService::enqueue_deployment(
    const std::string& deployment_id,
    const std::string& request_id,
    const DeploymentCandidate& candidate,
    const std::vector<VNF>& request_vnfs,
    const std::string& mode,
    const std::string& trigger
) {
    if (deployment_id.empty()) return;

    OrchestrationTask task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task.sequence = ++seq_;
        task.deployment_id = deployment_id;
        task.request_id = request_id;
        task.candidate = candidate;
        task.request_vnfs = request_vnfs;
        task.mode = mode;
        task.trigger = trigger;
        queue_.push_back(task);
    }

    const int containers_total = static_cast<int>(unique_nf_types(task.candidate.deployed_nodes).size());
    const int core_nfs_total = static_cast<int>(task.candidate.per_vnf.size());
    const nlohmann::json patch = {
        {"orchestration_phase", "queued"},
        {"orchestration_progress", 5},
        {"orchestration_mode", mode},
        {"orchestration_trigger", trigger},
        {"containers_total", containers_total},
        {"containers_running", 0},
        {"containers_failed", 0},
        {"core_nfs_total", core_nfs_total},
        {"core_nfs_running", 0},
        {"core_nfs_failed", 0},
        {"service_ready", false},
        {"ready_for_ueransim", false},
        {"last_error", ""},
        {"last_update_at", iso_now()}
    };
    update_deployment_runtime_state(deployment_id, patch, true);
    cv_.notify_one();
}

std::unordered_map<std::string, DeploymentOrchestratorService::NodeRuntimeSnapshot>
DeploymentOrchestratorService::snapshot_node_runtime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_runtime_;
}

std::optional<DeploymentOrchestratorService::NodeRuntimeSnapshot>
DeploymentOrchestratorService::get_node_runtime(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = node_runtime_.find(node_id);
    if (it == node_runtime_.end()) return std::nullopt;
    return it->second;
}

void DeploymentOrchestratorService::worker_loop() {
    while (true) {
        OrchestrationTask task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&]() { return stop_requested_ || !queue_.empty(); });
            if (stop_requested_ && queue_.empty()) break;
            task = queue_.front();
            queue_.pop_front();
        }
        process_task(task);
    }
}

void DeploymentOrchestratorService::process_task(const OrchestrationTask& task) {
    const std::string now = iso_now();
    update_deployment_runtime_state(task.deployment_id, {
        {"orchestration_phase", "stopping_old"},
        {"orchestration_progress", 12},
        {"last_error", ""},
        {"last_update_at", now}
    }, true);

    std::vector<std::string> old_nodes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = deployment_runtime_.find(task.deployment_id);
        if (it != deployment_runtime_.end()) {
            old_nodes = it->second.active_nodes;
        }
    }
    for (const auto& node : old_nodes) {
        const std::string c = node_to_container_name(node);
        (void)stop_container(c);
    }
    clear_nodes_for_deployment(old_nodes);

    const auto unique_nodes = unique_nf_types(task.candidate.deployed_nodes);
    std::unordered_map<std::string, std::vector<std::string>> nfs_by_node;
    for (const auto& pv : task.candidate.per_vnf) {
        if (pv.node.empty()) continue;
        nfs_by_node[pv.node].push_back(normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type));
    }

    if (!ensure_network()) {
        update_deployment_runtime_state(task.deployment_id, {
            {"orchestration_phase", "failed"},
            {"orchestration_progress", 100},
            {"containers_total", static_cast<int>(unique_nodes.size())},
            {"containers_running", 0},
            {"containers_failed", static_cast<int>(unique_nodes.size())},
            {"core_nfs_total", static_cast<int>(task.candidate.per_vnf.size())},
            {"core_nfs_running", 0},
            {"core_nfs_failed", static_cast<int>(task.candidate.per_vnf.size())},
            {"service_ready", false},
            {"ready_for_ueransim", false},
            {"last_error", "docker_network_unavailable"},
            {"last_update_at", iso_now()}
        }, true);
        return;
    }

    update_deployment_runtime_state(task.deployment_id, {
        {"orchestration_phase", "starting_containers"},
        {"orchestration_progress", 28},
        {"containers_total", static_cast<int>(unique_nodes.size())},
        {"core_nfs_total", static_cast<int>(task.candidate.per_vnf.size())},
        {"last_update_at", iso_now()}
    }, true);

    int containers_running = 0;
    int containers_failed = 0;
    int core_nfs_running = 0;
    int core_nfs_failed = 0;

    std::vector<std::string> active_nodes;
    active_nodes.reserve(unique_nodes.size());

    for (const auto& node : unique_nodes) {
        const std::string container_name = node_to_container_name(node);
        NodeRuntimeSnapshot node_state;
        node_state.node_id = node;
        node_state.container_name = container_name;
        node_state.container_state = "starting";
        node_state.deployed = true;
        update_node_runtime_state(node, node_state);

        const bool container_ready = ensure_satellite_container(container_name);
        if (!container_ready) {
            node_state.container_state = "failed";
            node_state.service_probe_ok = false;
            node_state.running_core_nf_types.clear();
            node_state.core_business_load = zero_business_load();
            node_state.core_network_load = 0.0;
            update_node_runtime_state(node, node_state);
            containers_failed += 1;
            core_nfs_failed += static_cast<int>(nfs_by_node[node].size());
            continue;
        }

        containers_running += 1;
        node_state.container_state = "running";
        std::vector<std::string> running_nfs;
        for (const auto& nf : nfs_by_node[node]) {
            if (start_nf_in_container(container_name, nf)) {
                running_nfs.push_back(nf);
                core_nfs_running += 1;
            } else {
                core_nfs_failed += 1;
            }
        }
        running_nfs = unique_nf_types(running_nfs);
        node_state.running_core_nf_types = running_nfs;
        node_state.service_probe_ok = !running_nfs.empty();
        node_state.core_business_load = compute_business_load_for_nfs(
            running_nfs,
            node_state.service_probe_ok
        );
        node_state.core_network_load = node_state.core_business_load.load_index();
        update_node_runtime_state(node, node_state);
        active_nodes.push_back(node);
    }

    const bool has_amf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type) == "amf";
    });
    const bool has_smf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type) == "smf";
    });
    const bool has_upf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type) == "upf";
    });
    const bool has_nrf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type) == "nrf";
    });
    const bool min_chain_ready = has_amf && has_smf && has_upf && has_nrf;
    const bool service_ready = min_chain_ready && containers_failed == 0 && core_nfs_failed == 0;
    const std::string phase = service_ready ? "running" : "degraded";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = deployment_runtime_[task.deployment_id];
        rt.deployment_id = task.deployment_id;
        rt.active_nodes = active_nodes;
    }

    update_deployment_runtime_state(task.deployment_id, {
        {"orchestration_phase", phase},
        {"orchestration_progress", 100},
        {"containers_total", static_cast<int>(unique_nodes.size())},
        {"containers_running", containers_running},
        {"containers_failed", containers_failed},
        {"core_nfs_total", static_cast<int>(task.candidate.per_vnf.size())},
        {"core_nfs_running", core_nfs_running},
        {"core_nfs_failed", core_nfs_failed},
        {"service_ready", service_ready},
        {"ready_for_ueransim", service_ready},
        {"last_error", service_ready ? "" : "open5gs_nf_not_fully_running"},
        {"last_update_at", iso_now()}
    }, true);
}

std::string DeploymentOrchestratorService::iso_now() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::string DeploymentOrchestratorService::normalize_nf_type(const std::string& nf_type) {
    std::string out;
    out.reserve(nf_type.size());
    for (char ch : nf_type) {
        if (ch == '-' || ch == ' ') out.push_back('_');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

CoreBusinessLoad DeploymentOrchestratorService::compute_business_load_for_nfs(
    const std::vector<std::string>& nf_types,
    bool service_probe_ok
) {
    CoreBusinessLoad load = zero_business_load();
    if (nf_types.empty()) {
        load.normalize_inplace();
        return load;
    }

    struct Weights {
        double signaling;
        double session;
        double user_plane;
        double mobility;
        double policy;
        double auth;
    };

    auto weight_for = [](const std::string& nf) -> Weights {
        if (nf == "amf") return {0.95, 0.70, 0.12, 0.92, 0.46, 0.52};
        if (nf == "smf") return {0.66, 0.96, 0.44, 0.50, 0.92, 0.42};
        if (nf == "upf") return {0.24, 0.62, 1.00, 0.42, 0.26, 0.14};
        if (nf == "nrf") return {0.82, 0.56, 0.10, 0.32, 0.52, 0.30};
        if (nf == "ausf") return {0.58, 0.40, 0.08, 0.22, 0.24, 1.00};
        if (nf == "udm" || nf == "udr") return {0.54, 0.66, 0.10, 0.40, 0.46, 0.82};
        if (nf == "pcf") return {0.50, 0.70, 0.10, 0.30, 1.00, 0.40};
        if (nf == "nssf") return {0.50, 0.42, 0.08, 0.76, 0.80, 0.20};
        if (nf == "scp") return {0.80, 0.56, 0.22, 0.28, 0.52, 0.30};
        return {0.40, 0.40, 0.20, 0.30, 0.30, 0.30};
    };

    for (const auto& raw : nf_types) {
        const auto nf = normalize_nf_type(raw);
        const auto w = weight_for(nf);
        load.signaling_load += w.signaling;
        load.session_load += w.session;
        load.user_plane_load += w.user_plane;
        load.mobility_load += w.mobility;
        load.policy_load += w.policy;
        load.auth_load += w.auth;
    }

    const double scale = 1.0 / static_cast<double>(nf_types.size());
    const double health_factor = service_probe_ok ? 1.0 : 0.45;
    load.signaling_load = clamp01(load.signaling_load * scale * health_factor);
    load.session_load = clamp01(load.session_load * scale * health_factor);
    load.user_plane_load = clamp01(load.user_plane_load * scale * health_factor);
    load.mobility_load = clamp01(load.mobility_load * scale * health_factor);
    load.policy_load = clamp01(load.policy_load * scale * health_factor);
    load.auth_load = clamp01(load.auth_load * scale * health_factor);
    load.normalize_inplace();
    return load;
}

bool DeploymentOrchestratorService::ensure_network() {
    int code = 0;
    const std::string inspect_cmd = "docker network inspect " + std::string(kSatelliteNetwork) + " >/dev/null 2>&1";
    if (run_shell_command(inspect_cmd, &code) && code == 0) return true;
    const std::string create_cmd = "docker network create " + std::string(kSatelliteNetwork) + " >/dev/null 2>&1";
    const bool ok = run_shell_command(create_cmd, &code) && code == 0;
    if (!ok) {
        spdlog::warn("Failed to create docker network {}", kSatelliteNetwork);
    }
    return ok;
}

bool DeploymentOrchestratorService::stop_container(const std::string& container_name) {
    if (container_name.empty()) return false;
    int code = 0;
    const std::string cmd =
        "docker rm -f " + container_name + " >/dev/null 2>&1";
    const bool ok = run_shell_command(cmd, &code);
    (void)ok;
    return code == 0;
}

bool DeploymentOrchestratorService::ensure_satellite_container(const std::string& container_name) {
    if (container_name.empty()) return false;
    const std::string image = resolve_satellite_image();
    const std::string platform = getenv_str("SFC_SATELLITE_PLATFORM");
    int code = 0;

    const std::string inspect_running =
        "docker inspect -f '{{.State.Running}}' " + container_name + " 2>/dev/null | grep -q true";
    if (run_shell_command(inspect_running, &code) && code == 0) return true;

    const std::string start_cmd = "docker start " + container_name + " >/dev/null 2>&1";
    if (run_shell_command(start_cmd, &code) && code == 0) return true;

    run_shell_command("docker rm -f " + container_name + " >/dev/null 2>&1", &code);

    const std::string run_cmd =
        "docker run -d --name " + container_name +
        " --network " + std::string(kSatelliteNetwork) +
        (platform.empty() ? "" : (" --platform " + platform)) +
        " " + image +
        " sh -lc 'while true; do sleep 3600; done' >/dev/null 2>&1";
    const bool ok = run_shell_command(run_cmd, &code) && code == 0;
    if (!ok) {
        spdlog::warn("Failed to start satellite container {} with image {}", container_name, image);
    }
    return ok;
}

bool DeploymentOrchestratorService::start_nf_in_container(
    const std::string& container_name,
    const std::string& nf_type
) {
    if (container_name.empty() || nf_type.empty()) return false;
    const std::string nf = normalize_nf_type(nf_type);
    const std::string daemon = "open5gs-" + nf + "d";

    std::ostringstream oss;
    oss
        << "docker exec " << container_name
        << " sh -lc 'if command -v " << daemon << " >/dev/null 2>&1; then "
        << "nohup " << daemon << " >/tmp/" << daemon << ".log 2>&1 & "
        << "sleep 0.3; "
        << "pgrep -f \"" << daemon << "\" >/dev/null 2>&1; "
        << "else "
        << "exit 2; "
        << "fi' >/dev/null 2>&1";
    int code = 0;
    const bool invoked = run_shell_command(oss.str(), &code);
    if (!invoked) return false;
    if (code == 0) return true;
    // command not available in image.
    return false;
}

bool DeploymentOrchestratorService::run_shell_command(const std::string& cmd, int* code) const {
    if (cmd.empty()) {
        if (code) *code = -1;
        return false;
    }
    const int rc = std::system(cmd.c_str());
    if (code) *code = rc;
    return true;
}

std::string DeploymentOrchestratorService::node_to_container_name(const std::string& node_id) {
    std::string out = "sfc-sat-";
    out.reserve(out.size() + node_id.size());
    for (char ch : node_id) {
        if (ch == '_') out.push_back('-');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

void DeploymentOrchestratorService::update_deployment_runtime_state(
    const std::string& deployment_id,
    const nlohmann::json& patch,
    bool broadcast
) {
    nlohmann::json merged;
    if (!patch_deployment_record(deployment_id, patch, &merged)) {
        return;
    }
    if (!broadcast) return;

    nlohmann::json payload = {
        {"type", "deployment_runtime_update"},
        {"deployment_id", deployment_id},
        {"request_id", merged.value("request_id", "")},
        {"orchestration_phase", merged.value("orchestration_phase", "")},
        {"orchestration_progress", merged.value("orchestration_progress", 0)},
        {"containers_total", merged.value("containers_total", 0)},
        {"containers_running", merged.value("containers_running", 0)},
        {"containers_failed", merged.value("containers_failed", 0)},
        {"core_nfs_total", merged.value("core_nfs_total", 0)},
        {"core_nfs_running", merged.value("core_nfs_running", 0)},
        {"core_nfs_failed", merged.value("core_nfs_failed", 0)},
        {"service_ready", merged.value("service_ready", false)},
        {"ready_for_ueransim", merged.value("ready_for_ueransim", false)},
        {"last_error", merged.value("last_error", "")},
        {"last_update_at", merged.value("last_update_at", "")}
    };
    WSHandler::broadcast_json(payload);
}

void DeploymentOrchestratorService::update_node_runtime_state(
    const std::string& node_id,
    const NodeRuntimeSnapshot& snapshot
) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_runtime_[node_id] = snapshot;
}

void DeploymentOrchestratorService::clear_nodes_for_deployment(const std::vector<std::string>& old_nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& node : old_nodes) {
        auto it = node_runtime_.find(node);
        if (it == node_runtime_.end()) continue;
        it->second.deployed = false;
        it->second.container_state = "stopped";
        it->second.running_core_nf_types.clear();
        it->second.service_probe_ok = false;
        it->second.core_business_load = zero_business_load();
        it->second.core_network_load = 0.0;
    }
}

}  // namespace sfc
