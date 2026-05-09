#include "services/DeploymentOrchestratorService.h"

#include "services/DeploymentStateStore.h"
#include "websocket/WSHandler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sfc {
namespace {

constexpr const char* kSatelliteNetwork = "sfc-open5gs-net";
constexpr const char* kMirrorSatelliteImage = "docker.1ms.run/gradiant/open5gs:2.7.7";
constexpr const char* kDefaultSatelliteImage = "gradiant/open5gs:2.7.7";
constexpr const char* kFallbackSatelliteImage = "ghcr.io/open5gs/open5gs:latest";
constexpr const char* kLocalSatelliteImage = "sfc-open5gs-satellite:local";
constexpr const char* kLocalSatelliteImageLatest = "sfc-open5gs-satellite:latest";
constexpr const char* kMongoContainer = "sfc-open5gs-mongo";
constexpr const char* kMongoImage = "mongo:6";
constexpr const char* kMongoUri = "mongodb://mongo/open5gs";

constexpr int kSbiPortNrf = 7777;
constexpr int kSbiPortAmf = 7778;
constexpr int kSbiPortSmf = 7779;
constexpr int kSbiPortAusf = 7780;
constexpr int kSbiPortUdm = 7781;
constexpr int kSbiPortUdr = 7782;
constexpr int kSbiPortPcf = 7783;
constexpr int kSbiPortNssf = 7784;
constexpr int kSbiPortScp = 7785;
constexpr int kSbiPortBsf = 7786;
constexpr int kSbiPortSepp = 7787;

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

std::string trim_copy(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    return s;
}

std::string sanitize_for_filename(std::string s) {
    for (auto& ch : s) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) {
            ch = '_';
        }
    }
    if (s.empty()) s = "x";
    return s;
}

std::string getenv_str(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) return "";
    return std::string(v);
}

std::string default_satellite_platform() {
    const std::string configured = getenv_str("SFC_SATELLITE_PLATFORM");
    if (!configured.empty()) return configured;
#if defined(__aarch64__) || defined(__arm64__)
    return "linux/amd64";
#else
    return "";
#endif
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

int decode_exit_code(int status) {
    if (status < 0) return status;
#if defined(WIFEXITED) && defined(WEXITSTATUS)
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
#endif
    return status;
}

std::vector<std::string> split_lines(std::string s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) out.push_back(line);
    return out;
}

std::string tail_lines(const std::string& text, size_t max_lines) {
    const auto lines = split_lines(text);
    if (lines.size() <= max_lines) return text;
    std::ostringstream oss;
    const size_t start = lines.size() - max_lines;
    for (size_t i = start; i < lines.size(); ++i) {
        oss << lines[i] << '\n';
    }
    return oss.str();
}

std::string upper_copy(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

void collect_nf_types_from_json(
    const nlohmann::json& node,
    std::unordered_set<std::string>* out
) {
    if (!out) return;

    if (node.is_object()) {
        auto it = node.find("nfType");
        if (it != node.end() && it->is_string()) {
            out->insert(upper_copy(it->get<std::string>()));
        }
        for (const auto& kv : node.items()) {
            collect_nf_types_from_json(kv.value(), out);
        }
        return;
    }

    if (node.is_array()) {
        for (const auto& item : node) {
            collect_nf_types_from_json(item, out);
        }
    }
}

std::string join_sorted(const std::unordered_set<std::string>& values) {
    if (values.empty()) return "";
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    std::ostringstream oss;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) oss << ',';
        oss << sorted[i];
    }
    return oss.str();
}

std::vector<std::string> build_image_candidates() {
    std::vector<std::string> out;
    const auto push_if = [&](const std::string& v) {
        if (v.empty()) return;
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    };
    push_if(getenv_str("SFC_SATELLITE_IMAGE"));
#if defined(__aarch64__) || defined(__arm64__)
    push_if(getenv_str("SFC_SATELLITE_IMAGE_ARM64"));
#elif defined(__x86_64__)
    push_if(getenv_str("SFC_SATELLITE_IMAGE_AMD64"));
    push_if(getenv_str("SFC_SATELLITE_IMAGE_X86_64"));
#endif
    push_if(kMirrorSatelliteImage);
    push_if(kDefaultSatelliteImage);
    push_if(kLocalSatelliteImage);
    push_if(kLocalSatelliteImageLatest);
    push_if(kFallbackSatelliteImage);
    return out;
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
    size_t dropped_stale = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto before = queue_.size();
        queue_.erase(
            std::remove_if(
                queue_.begin(),
                queue_.end(),
                [&](const OrchestrationTask& queued) { return queued.deployment_id == deployment_id; }
            ),
            queue_.end()
        );
        dropped_stale = before - queue_.size();

        task.sequence = ++seq_;
        task.deployment_id = deployment_id;
        task.request_id = request_id;
        task.candidate = candidate;
        task.request_vnfs = request_vnfs;
        task.mode = mode;
        task.trigger = trigger;
        queue_.push_back(task);
    }

    if (dropped_stale > 0) {
        spdlog::info(
            "Orchestrator dedup dropped {} stale queued task(s) for deployment {}",
            dropped_stale,
            deployment_id
        );
    }

    const int containers_total = static_cast<int>(unique_nf_types(task.candidate.deployed_nodes).size());
    const int core_nfs_total = static_cast<int>(task.candidate.per_vnf.size());
    const nlohmann::json candidate_json = task.candidate.to_json();
    nlohmann::json patch = {
        {"orchestration_phase", "queued"},
        {"orchestration_progress", 5},
        {"orchestration_mode", mode},
        {"orchestration_trigger", trigger},
        {"deployed_nodes", candidate_json.value("deployed_nodes", nlohmann::json::array())},
        {"per_vnf", candidate_json.value("per_vnf", nlohmann::json::array())},
        {"per_core_nf", candidate_json.value("per_core_nf", nlohmann::json::array())},
        {"total_latency_ms", candidate_json.value("total_latency_ms", 0.0)},
        {"registration_latency_ms", candidate_json.value("registration_latency_ms", 0.0)},
        {"pdu_session_latency_ms", candidate_json.value("pdu_session_latency_ms", 0.0)},
        {"link_details", candidate_json.value("link_details", nlohmann::json::array())},
        {"estimated_reliability", candidate_json.value("estimated_reliability", 0.0)},
        {"bottleneck_bandwidth_gbps", candidate_json.value("bottleneck_bandwidth_gbps", 0.0)},
        {"reason", candidate_json.value("reason", std::string(""))},
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

bool DeploymentOrchestratorService::rollback_deployment(
    const std::string& deployment_id,
    const std::vector<std::string>& nodes_hint
) {
    if (deployment_id.empty()) return false;

    std::vector<std::string> containers_to_stop;
    std::unordered_set<std::string> nodes_to_clear(nodes_hint.begin(), nodes_hint.end());
    bool touched_runtime = false;

    auto normalize_token = [](const std::string& input) {
        std::string out;
        out.reserve(input.size());
        bool last_dash = false;
        for (char ch : input) {
            const unsigned char u = static_cast<unsigned char>(ch);
            if (std::isalnum(u)) {
                out.push_back(static_cast<char>(std::tolower(u)));
                last_dash = false;
            } else if (!last_dash) {
                out.push_back('-');
                last_dash = true;
            }
        }
        while (!out.empty() && out.back() == '-') out.pop_back();
        if (out.empty()) out = "x";
        return out;
    };
    const std::string deployment_prefix = "sfc-sat-" + normalize_token(deployment_id) + "-";

    {
        std::lock_guard<std::mutex> lock(mutex_);

        queue_.erase(
            std::remove_if(
                queue_.begin(),
                queue_.end(),
                [&](const OrchestrationTask& task) { return task.deployment_id == deployment_id; }
            ),
            queue_.end()
        );

        auto it = deployment_runtime_.find(deployment_id);
        if (it != deployment_runtime_.end()) {
            for (const auto& node : it->second.active_nodes) {
                if (!node.empty()) nodes_to_clear.insert(node);
            }
            for (const auto& c : it->second.active_containers) {
                if (!c.empty()) containers_to_stop.push_back(c);
            }
            deployment_runtime_.erase(it);
            touched_runtime = true;
        } else {
            for (const auto& kv : node_runtime_) {
                const auto& snap = kv.second;
                if (snap.container_name.rfind(deployment_prefix, 0) != 0) continue;
                nodes_to_clear.insert(kv.first);
                if (!snap.container_name.empty()) containers_to_stop.push_back(snap.container_name);
                touched_runtime = true;
            }
        }

        for (const auto& node : nodes_to_clear) {
            auto rt_it = node_runtime_.find(node);
            if (rt_it == node_runtime_.end()) continue;
            rt_it->second.deployed = false;
            rt_it->second.container_state = "stopped";
            rt_it->second.running_core_nf_types.clear();
            rt_it->second.service_probe_ok = false;
            rt_it->second.core_business_load = zero_business_load();
            rt_it->second.core_network_load = 0.0;
            touched_runtime = true;
        }
    }

    std::unordered_set<std::string> dedup(containers_to_stop.begin(), containers_to_stop.end());
    for (const auto& node : nodes_to_clear) {
        dedup.insert(node_to_container_name(deployment_id, node));
        dedup.insert(legacy_node_container_name(node));
    }

    if (dedup.empty()) {
        std::string list_output;
        int code = 0;
        const std::string list_cmd =
            "docker ps -a --format '{{.Names}}' | grep '^" + deployment_prefix + "' || true";
        run_shell_command_capture(list_cmd, &list_output, &code);
        for (const auto& line : split_lines(list_output)) {
            const std::string c = trim_copy(line);
            if (!c.empty()) dedup.insert(c);
        }
    }

    bool stopped_any = false;
    for (const auto& c : dedup) {
        if (c.empty()) continue;
        stopped_any = stop_container(c) || stopped_any;
    }

    return touched_runtime || !dedup.empty() || stopped_any;
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
    std::vector<std::string> old_containers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = deployment_runtime_.find(task.deployment_id);
        if (it != deployment_runtime_.end()) {
            old_nodes = it->second.active_nodes;
            old_containers = it->second.active_containers;
        }
    }
    for (const auto& c : old_containers) {
        (void)stop_container(c);
    }
    if (old_containers.empty()) {
        for (const auto& node : old_nodes) {
            const std::string c = node_to_container_name(task.deployment_id, node);
            (void)stop_container(c);
            (void)stop_container(legacy_node_container_name(node));
        }
    }
    clear_nodes_for_deployment(old_nodes);

    const auto unique_nodes = unique_nf_types(task.candidate.deployed_nodes);
    std::unordered_map<std::string, std::vector<std::string>> nfs_by_node;
    for (const auto& pv : task.candidate.per_vnf) {
        if (pv.node.empty()) continue;
        nfs_by_node[pv.node].push_back(normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type));
    }

    auto fail_deployment = [&](const std::string& reason) {
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
            {"last_error", reason},
            {"last_update_at", iso_now()}
        }, true);
    };

    if (!ensure_network()) {
        fail_deployment("docker_network_unavailable");
        return;
    }

    bool has_db_nf = false;
    for (const auto& kv : nfs_by_node) {
        for (const auto& nf : kv.second) {
            if (nf_uses_mongo(nf)) {
                has_db_nf = true;
                break;
            }
        }
        if (has_db_nf) break;
    }
    if (has_db_nf) {
        std::string mongo_reason;
        if (!ensure_mongo_container(&mongo_reason)) {
            fail_deployment(mongo_reason.empty() ? "mongo_unavailable" : mongo_reason);
            return;
        }
    }

    std::string satellite_image;
    std::string image_reason;
    if (!ensure_satellite_image_available(&satellite_image, &image_reason)) {
        fail_deployment(image_reason.empty() ? "satellite_image_unavailable" : image_reason);
        return;
    }
    const std::string platform = default_satellite_platform();

    update_deployment_runtime_state(task.deployment_id, {
        {"orchestration_phase", "starting_containers"},
        {"orchestration_progress", 28},
        {"containers_total", static_cast<int>(unique_nodes.size())},
        {"core_nfs_total", static_cast<int>(task.candidate.per_vnf.size())},
        {"last_error", ""},
        {"last_update_at", iso_now()}
    }, true);

    int containers_running = 0;
    int containers_failed = 0;
    int core_nfs_running = 0;
    int core_nfs_failed = 0;

    std::vector<std::string> active_nodes;
    std::vector<std::string> active_containers;
    std::unordered_map<std::string, std::string> node_container_by_id;
    std::unordered_map<std::string, std::string> node_ip_by_id;
    std::vector<std::string> errors;
    active_nodes.reserve(unique_nodes.size());
    active_containers.reserve(unique_nodes.size());

    for (const auto& node : unique_nodes) {
        const std::string container_name = node_to_container_name(task.deployment_id, node);
        NodeRuntimeSnapshot node_state;
        node_state.node_id = node;
        node_state.container_name = container_name;
        node_state.container_state = "starting";
        node_state.deployed = true;
        update_node_runtime_state(node, node_state);

        const bool container_ready = ensure_satellite_container(container_name, satellite_image, platform);
        if (!container_ready) {
            node_state.container_state = "failed";
            node_state.service_probe_ok = false;
            node_state.running_core_nf_types.clear();
            node_state.core_business_load = zero_business_load();
            node_state.core_network_load = 0.0;
            update_node_runtime_state(node, node_state);
            containers_failed += 1;
            core_nfs_failed += static_cast<int>(nfs_by_node[node].size());
            errors.push_back("container_start_failed:" + node);
            continue;
        }

        const std::string node_ip = inspect_container_ip(container_name);
        if (node_ip.empty()) {
            node_state.container_state = "failed";
            node_state.service_probe_ok = false;
            node_state.running_core_nf_types.clear();
            node_state.core_business_load = zero_business_load();
            node_state.core_network_load = 0.0;
            update_node_runtime_state(node, node_state);
            containers_failed += 1;
            core_nfs_failed += static_cast<int>(nfs_by_node[node].size());
            errors.push_back("container_ip_unavailable:" + node);
            (void)stop_container(container_name);
            continue;
        }

        containers_running += 1;
        node_state.container_state = "running";
        node_state.service_probe_ok = false;
        node_state.running_core_nf_types.clear();
        node_state.core_business_load = zero_business_load();
        node_state.core_network_load = 0.0;
        update_node_runtime_state(node, node_state);
        node_ip_by_id[node] = node_ip;
        node_container_by_id[node] = container_name;
        active_nodes.push_back(node);
        active_containers.push_back(container_name);
    }

    std::string nrf_node;
    std::string smf_node;
    std::string upf_node;
    std::string scp_node;
    for (const auto& pv : task.candidate.per_vnf) {
        const std::string nf = normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type);
        if (nf == "nrf" && nrf_node.empty()) nrf_node = pv.node;
        if (nf == "smf" && smf_node.empty()) smf_node = pv.node;
        if (nf == "upf" && upf_node.empty()) upf_node = pv.node;
        if (nf == "scp" && scp_node.empty()) scp_node = pv.node;
    }

    const std::string nrf_ip = (!nrf_node.empty() && node_ip_by_id.count(nrf_node))
        ? node_ip_by_id[nrf_node]
        : "";
    const std::string upf_ip = (!upf_node.empty() && node_ip_by_id.count(upf_node))
        ? node_ip_by_id[upf_node]
        : "";
    const std::string scp_ip = (!scp_node.empty() && node_ip_by_id.count(scp_node))
        ? node_ip_by_id[scp_node]
        : "";
    const bool smf_upf_colocated =
        !smf_node.empty() &&
        !upf_node.empty() &&
        smf_node == upf_node;
    const std::string nrf_uri = nrf_ip.empty()
        ? ""
        : ("http://" + nrf_ip + ":" + std::to_string(sbi_port_for_nf_type("nrf")));
    const std::string scp_uri = scp_ip.empty()
        ? ""
        : ("http://" + scp_ip + ":" + std::to_string(sbi_port_for_nf_type("scp")));

    std::unordered_set<std::string> started_nfs_set;
    for (const auto& node : unique_nodes) {
        const auto ip_it = node_ip_by_id.find(node);
        if (ip_it == node_ip_by_id.end()) continue;
        const std::string local_ip = ip_it->second;
        const auto c_it = node_container_by_id.find(node);
        if (c_it == node_container_by_id.end()) continue;
        const std::string container_name = c_it->second;

        std::vector<std::string> running_nfs;
        for (const auto& nf : nfs_by_node[node]) {
            const std::string cfg = render_nf_config(
                nf,
                local_ip,
                nrf_uri,
                upf_ip.empty() ? local_ip : upf_ip,
                kMongoUri,
                scp_uri,
                smf_upf_colocated
            );
            if (start_nf_in_container(container_name, nf, cfg, smf_upf_colocated)) {
                running_nfs.push_back(nf);
                started_nfs_set.insert(nf);
                core_nfs_running += 1;
            } else {
                core_nfs_failed += 1;
                errors.push_back("nf_start_failed:" + nf + "@" + node);
            }
        }

        NodeRuntimeSnapshot node_state;
        node_state.node_id = node;
        node_state.container_name = container_name;
        node_state.container_state = "running";
        node_state.deployed = true;
        node_state.running_core_nf_types = unique_nf_types(running_nfs);
        node_state.service_probe_ok =
            !node_state.running_core_nf_types.empty() &&
            node_state.running_core_nf_types.size() == unique_nf_types(nfs_by_node[node]).size();
        node_state.core_business_load = compute_business_load_for_nfs(
            node_state.running_core_nf_types,
            node_state.service_probe_ok
        );
        node_state.core_network_load = node_state.core_business_load.load_index();
        update_node_runtime_state(node, node_state);
    }

    bool registration_ok = true;
    if (!nrf_node.empty()) {
        if (nrf_ip.empty()) {
            registration_ok = false;
            errors.push_back("nrf_ip_unavailable");
        } else {
            const std::string nrf_container = node_container_by_id.count(nrf_node)
                ? node_container_by_id[nrf_node]
                : node_to_container_name(task.deployment_id, nrf_node);
            std::vector<std::string> started_nfs(started_nfs_set.begin(), started_nfs_set.end());
            registration_ok = check_nrf_registration(nrf_container, nrf_ip, started_nfs);
            if (!registration_ok) {
                errors.push_back("nrf_registration_incomplete");
            }
        }
    }

    const bool has_amf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "amf";
    });
    const bool has_smf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "smf";
    });
    const bool has_upf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "upf";
    });
    const bool has_nrf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "nrf";
    });
    const bool has_ausf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "ausf";
    });
    const bool has_udm = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "udm";
    });
    const bool has_udr = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "udr";
    });
    const bool has_pcf = std::any_of(task.candidate.per_vnf.begin(), task.candidate.per_vnf.end(), [](const auto& pv) {
        return normalize_nf_type(pv.nf_type.empty() ? pv.vnf : pv.nf_type) == "pcf";
    });

    bool pfcp_ready = true;
    if (has_smf && has_upf) {
        const std::string smf_container = (!smf_node.empty() && node_container_by_id.count(smf_node))
            ? node_container_by_id[smf_node]
            : "";
        const std::string upf_container = (!upf_node.empty() && node_container_by_id.count(upf_node))
            ? node_container_by_id[upf_node]
            : "";
        if (smf_container.empty() || upf_container.empty()) {
            pfcp_ready = false;
        } else {
            pfcp_ready = check_smf_upf_pfcp_ready(smf_container, upf_container, 20);
        }
        if (!pfcp_ready) {
            errors.push_back("smf_upf_pfcp_not_ready");
        }
    }

    const bool min_chain_ready = has_amf && has_smf && has_upf && has_nrf;
    const bool service_ready =
        min_chain_ready &&
        containers_failed == 0 &&
        core_nfs_failed == 0 &&
        registration_ok &&
        pfcp_ready;
    const bool ue_validation_ready =
        service_ready &&
        has_ausf &&
        has_udm &&
        has_udr &&
        has_pcf;
    const std::string phase = service_ready ? "running" : "degraded";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = deployment_runtime_[task.deployment_id];
        rt.deployment_id = task.deployment_id;
        rt.active_nodes = active_nodes;
        rt.active_containers = active_containers;
    }

    const std::string err = errors.empty()
        ? (service_ready
            ? (ue_validation_ready ? "" : "ueransim_prerequisites_not_met")
            : "open5gs_nf_not_fully_running")
        : errors.front();

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
        {"ready_for_ueransim", ue_validation_ready},
        {"last_error", err},
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
        if (nf == "bsf") return {0.48, 0.68, 0.10, 0.36, 0.88, 0.38};
        if (nf == "sepp") return {0.86, 0.60, 0.18, 0.34, 0.72, 0.94};
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
    // Attenuate single-NF saturation so one NF does not easily show 100% in a dimension.
    // 1 NF -> 0.60, 2 -> 0.70, 3 -> 0.80, 4 -> 0.90, >=5 -> 1.00
    const double density_factor = 0.60 + 0.40 * std::min(1.0, (static_cast<double>(nf_types.size()) - 1.0) / 4.0);
    const double final_factor = health_factor * density_factor;
    load.signaling_load = clamp01(load.signaling_load * scale * final_factor);
    load.session_load = clamp01(load.session_load * scale * final_factor);
    load.user_plane_load = clamp01(load.user_plane_load * scale * final_factor);
    load.mobility_load = clamp01(load.mobility_load * scale * final_factor);
    load.policy_load = clamp01(load.policy_load * scale * final_factor);
    load.auth_load = clamp01(load.auth_load * scale * final_factor);
    load.normalize_inplace();
    return load;
}

bool DeploymentOrchestratorService::ensure_satellite_image_available(
    std::string* image_out,
    std::string* reason_out
) {
    if (!image_out) return false;

    const auto candidates = build_image_candidates();
    const std::string platform = default_satellite_platform();
    int code = 0;

    for (const auto& image : candidates) {
        const std::string inspect_cmd = "docker image inspect " + image + " >/dev/null 2>&1";
        if (run_shell_command(inspect_cmd, &code) && code == 0) {
            std::string invalid_reason;
            if (validate_satellite_image(image, &invalid_reason)) {
                *image_out = image;
                if (reason_out) *reason_out = "";
                spdlog::info("Selected satellite image {}", image);
                return true;
            }
            spdlog::warn(
                "Skipping satellite image {} due to missing Open5GS daemons: {}",
                image,
                invalid_reason
            );
        }
    }

    for (const auto& image : candidates) {
        std::string output;
        const std::string pull_cmd =
            "docker pull " + (platform.empty() ? "" : ("--platform " + platform + " ")) + image;
        run_shell_command_capture(pull_cmd, &output, &code);
        if (code == 0) {
            std::string invalid_reason;
            if (validate_satellite_image(image, &invalid_reason)) {
                *image_out = image;
                if (reason_out) *reason_out = "";
                spdlog::info("Pulled and selected satellite image {}", image);
                return true;
            }
            spdlog::warn(
                "Pulled satellite image {} but validation failed: {}",
                image,
                invalid_reason
            );
            continue;
        }
        spdlog::warn("Failed to pull image {}: {}", image, trim_copy(tail_lines(output, 8)));
    }

    const std::string explicit_df = getenv_str("SFC_SATELLITE_DOCKERFILE");
    const std::string explicit_ctx = getenv_str("SFC_SATELLITE_BUILD_CONTEXT");
    std::vector<std::pair<std::string, std::string>> build_attempts;
    if (!explicit_df.empty()) {
        build_attempts.emplace_back(explicit_df, explicit_ctx.empty() ? "." : explicit_ctx);
    }
    build_attempts.emplace_back("docker/open5gs-satellite.Dockerfile", ".");
    build_attempts.emplace_back("../docker/open5gs-satellite.Dockerfile", "..");

    for (const auto& attempt : build_attempts) {
        const auto& dockerfile = attempt.first;
        const auto& context = attempt.second;
        if (!std::filesystem::exists(dockerfile)) continue;

        std::string output;
        const std::string build_cmd =
            "docker build -f " + dockerfile + " -t " + std::string(kLocalSatelliteImage) + " " + context;
        run_shell_command_capture(build_cmd, &output, &code);
        if (code == 0) {
            std::string invalid_reason;
            if (validate_satellite_image(kLocalSatelliteImage, &invalid_reason)) {
                *image_out = kLocalSatelliteImage;
                if (reason_out) *reason_out = "";
                spdlog::info("Built and selected satellite image {}", kLocalSatelliteImage);
                return true;
            }
            spdlog::warn(
                "Built satellite image {} but validation failed: {}",
                kLocalSatelliteImage,
                invalid_reason
            );
        }
        spdlog::warn(
            "Failed to build {} from {}: {}",
            kLocalSatelliteImage,
            dockerfile,
            trim_copy(tail_lines(output, 12))
        );
    }

    if (reason_out) {
        *reason_out = "satellite_image_unavailable";
    }
    return false;
}

bool DeploymentOrchestratorService::validate_satellite_image(
    const std::string& image,
    std::string* reason_out
) const {
    if (reason_out) reason_out->clear();
    if (image.empty()) {
        if (reason_out) *reason_out = "empty_image";
        return false;
    }

    const std::string platform = default_satellite_platform();
    int code = 0;
    std::string output;
    const std::string cmd =
        "docker run --rm " +
        (platform.empty() ? "" : (" --platform " + platform)) +
        " --entrypoint /bin/sh " + image +
        " -lc 'for d in open5gs-nrfd open5gs-amfd open5gs-smfd open5gs-upfd; do "
        "command -v \"$d\" >/dev/null 2>&1 || [ -x \"/opt/open5gs/bin/$d\" ] || exit 10; "
        "done'";
    run_shell_command_capture(cmd, &output, &code);
    if (code == 0) return true;

    if (reason_out) {
        *reason_out = trim_copy(tail_lines(output, 6));
        if (reason_out->empty()) *reason_out = "required_open5gs_daemons_not_found";
    }
    return false;
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

bool DeploymentOrchestratorService::ensure_mongo_container(std::string* reason_out) {
    int code = 0;
    std::string inspect_output;
    bool has_container = false;
    bool attached_to_target_network = false;
    bool has_mongo_alias = false;
    {
        const std::string inspect_cmd =
            "docker inspect -f '{{json .NetworkSettings.Networks}}' " + std::string(kMongoContainer) + " 2>/dev/null";
        run_shell_command_capture(inspect_cmd, &inspect_output, &code);
        has_container = (code == 0);
        if (has_container) {
            attached_to_target_network =
                inspect_output.find("\"" + std::string(kSatelliteNetwork) + "\"") != std::string::npos;
            has_mongo_alias = inspect_output.find("\"mongo\"") != std::string::npos;
        }
    }

    if (!has_container || !attached_to_target_network || !has_mongo_alias) {
        run_shell_command("docker rm -f " + std::string(kMongoContainer) + " >/dev/null 2>&1", &code);
        const std::string run_cmd =
            "docker run -d --name " + std::string(kMongoContainer) +
            " --network " + std::string(kSatelliteNetwork) +
            " --network-alias " + std::string(kMongoContainer) +
            " --network-alias mongo " +
            " " + std::string(kMongoImage) + " >/dev/null 2>&1";
        if (!(run_shell_command(run_cmd, &code) && code == 0)) {
            if (reason_out) *reason_out = "mongo_container_start_failed";
            return false;
        }
    } else {
        const std::string inspect_running =
            "docker inspect -f '{{.State.Running}}' " + std::string(kMongoContainer) + " 2>/dev/null | grep -q true";
        if (!(run_shell_command(inspect_running, &code) && code == 0)) {
            const std::string start_cmd = "docker start " + std::string(kMongoContainer) + " >/dev/null 2>&1";
            if (!(run_shell_command(start_cmd, &code) && code == 0)) {
                if (reason_out) *reason_out = "mongo_container_start_failed";
                return false;
            }
        }
    }

    for (int i = 0; i < 30; ++i) {
        const std::string probe_cmd =
            "docker exec " + std::string(kMongoContainer) +
            " sh -lc 'mongosh --quiet --eval \"db.adminCommand({ping:1}).ok\" >/dev/null 2>&1 || "
            "mongo --quiet --eval \"db.runCommand({ping:1}).ok\" >/dev/null 2>&1'";
        if (run_shell_command(probe_cmd, &code) && code == 0) return true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (reason_out) *reason_out = "mongo_not_ready";
    return false;
}

bool DeploymentOrchestratorService::stop_container(const std::string& container_name) {
    if (container_name.empty()) return false;
    int code = 0;
    const std::string cmd = "docker rm -f " + container_name + " >/dev/null 2>&1";
    run_shell_command(cmd, &code);
    return code == 0;
}

bool DeploymentOrchestratorService::is_container_running(const std::string& container_name) const {
    if (container_name.empty()) return false;
    int code = 0;
    const std::string inspect_running =
        "docker inspect -f '{{.State.Running}}' " + container_name + " 2>/dev/null | grep -q true";
    run_shell_command(inspect_running, &code);
    return code == 0;
}

bool DeploymentOrchestratorService::wait_for_container_running(
    const std::string& container_name,
    int attempts,
    int interval_ms
) const {
    if (container_name.empty()) return false;
    attempts = std::max(1, attempts);
    interval_ms = std::max(50, interval_ms);

    for (int i = 0; i < attempts; ++i) {
        if (is_container_running(container_name)) {
            // Double-check after a short delay to catch fast-exit containers.
            std::this_thread::sleep_for(std::chrono::milliseconds(160));
            if (is_container_running(container_name)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return false;
}

void DeploymentOrchestratorService::log_container_diagnostics(
    const std::string& container_name,
    const std::string& context
) const {
    if (container_name.empty()) return;
    int code = 0;
    std::string inspect_output;
    std::string logs_output;
    const std::string inspect_cmd =
        "docker inspect -f 'state={{.State.Status}} exit={{.State.ExitCode}} err={{.State.Error}} "
        "started={{.State.StartedAt}} finished={{.State.FinishedAt}} image={{.Config.Image}}' " +
        container_name + " 2>/dev/null";
    run_shell_command_capture(inspect_cmd, &inspect_output, &code);
    const std::string logs_cmd = "docker logs --tail 20 " + container_name + " 2>/dev/null || true";
    run_shell_command_capture(logs_cmd, &logs_output, &code);
    spdlog::warn(
        "Container diagnostics [{}] {} inspect={} logs={}",
        context,
        container_name,
        trim_copy(inspect_output),
        trim_copy(tail_lines(logs_output, 8))
    );
}

bool DeploymentOrchestratorService::ensure_satellite_container(
    const std::string& container_name,
    const std::string& image,
    const std::string& platform
) {
    if (container_name.empty() || image.empty()) return false;
    int code = 0;

    if (wait_for_container_running(container_name)) return true;

    const std::string start_cmd = "docker start " + container_name + " >/dev/null 2>&1";
    if (run_shell_command(start_cmd, &code) && code == 0) {
        if (wait_for_container_running(container_name)) return true;
        log_container_diagnostics(container_name, "start_existing_unstable");
    }

    run_shell_command("docker rm -f " + container_name + " >/dev/null 2>&1", &code);

    std::string output;
    auto try_run = [&](const std::string& extra_flags) -> bool {
        const std::string run_cmd =
            "docker run -d --name " + container_name +
            " --network " + std::string(kSatelliteNetwork) +
            (platform.empty() ? "" : (" --platform " + platform)) +
            (extra_flags.empty() ? "" : (" " + extra_flags)) +
            " --entrypoint /bin/sh " +
            " " + image +
            " -c 'while sleep 3600; do :; done'";
        run_shell_command_capture(run_cmd, &output, &code);
        if (!(code == 0)) return false;
        return wait_for_container_running(container_name);
    };

    bool started = try_run("--cap-add=NET_ADMIN --device=/dev/net/tun");
    if (!started) {
        spdlog::warn(
            "Satellite container {} device-start failed, fallback to privileged mode: {}",
            container_name,
            trim_copy(tail_lines(output, 8))
        );
        log_container_diagnostics(container_name, "device_mode_failed");
        run_shell_command("docker rm -f " + container_name + " >/dev/null 2>&1", &code);
        started = try_run("--privileged");
    }
    if (!started) {
        log_container_diagnostics(container_name, "privileged_mode_failed");
        spdlog::warn(
            "Failed to start satellite container {} with image {}: {}",
            container_name,
            image,
            trim_copy(tail_lines(output, 10))
        );
        return false;
    }

    return true;
}

std::string DeploymentOrchestratorService::inspect_container_ip(const std::string& container_name) const {
    if (container_name.empty()) return "";
    int code = 0;
    std::string output;
    const std::string cmd =
        "docker inspect -f '{{(index .NetworkSettings.Networks \"" + std::string(kSatelliteNetwork) +
        "\").IPAddress}}' " + container_name + " 2>/dev/null";
    run_shell_command_capture(cmd, &output, &code);
    if (code != 0) return "";
    return trim_copy(output);
}

std::string DeploymentOrchestratorService::daemon_for_nf_type(const std::string& nf_type) const {
    const std::string nf = normalize_nf_type(nf_type);
    if (nf == "nrf") return "open5gs-nrfd";
    if (nf == "amf") return "open5gs-amfd";
    if (nf == "smf") return "open5gs-smfd";
    if (nf == "upf") return "open5gs-upfd";
    if (nf == "ausf") return "open5gs-ausfd";
    if (nf == "udm") return "open5gs-udmd";
    if (nf == "udr") return "open5gs-udrd";
    if (nf == "pcf") return "open5gs-pcfd";
    if (nf == "nssf") return "open5gs-nssfd";
    if (nf == "scp") return "open5gs-scpd";
    if (nf == "bsf") return "open5gs-bsfd";
    if (nf == "sepp") return "open5gs-seppd";
    return "open5gs-" + nf + "d";
}

std::string DeploymentOrchestratorService::nf_type_to_3gpp(const std::string& nf_type) const {
    const std::string nf = normalize_nf_type(nf_type);
    if (nf == "nrf") return "NRF";
    if (nf == "amf") return "AMF";
    if (nf == "smf") return "SMF";
    if (nf == "upf") return "UPF";
    if (nf == "ausf") return "AUSF";
    if (nf == "udm") return "UDM";
    if (nf == "udr") return "UDR";
    if (nf == "pcf") return "PCF";
    if (nf == "nssf") return "NSSF";
    if (nf == "scp") return "SCP";
    if (nf == "bsf") return "BSF";
    if (nf == "sepp") return "SEPP";
    std::string up = nf;
    for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return up;
}

int DeploymentOrchestratorService::sbi_port_for_nf_type(const std::string& nf_type) const {
    const std::string nf = normalize_nf_type(nf_type);
    if (nf == "nrf") return kSbiPortNrf;
    if (nf == "amf") return kSbiPortAmf;
    if (nf == "smf") return kSbiPortSmf;
    if (nf == "ausf") return kSbiPortAusf;
    if (nf == "udm") return kSbiPortUdm;
    if (nf == "udr") return kSbiPortUdr;
    if (nf == "pcf") return kSbiPortPcf;
    if (nf == "nssf") return kSbiPortNssf;
    if (nf == "scp") return kSbiPortScp;
    if (nf == "bsf") return kSbiPortBsf;
    if (nf == "sepp") return kSbiPortSepp;
    return kSbiPortNrf;
}

bool DeploymentOrchestratorService::nf_uses_mongo(const std::string& nf_type) const {
    const std::string nf = normalize_nf_type(nf_type);
    return nf == "udr" || nf == "udm" || nf == "ausf" || nf == "pcf";
}

bool DeploymentOrchestratorService::nf_registers_to_nrf(const std::string& nf_type) const {
    const std::string nf = normalize_nf_type(nf_type);
    return nf != "upf";
}

std::string DeploymentOrchestratorService::render_nf_config(
    const std::string& nf_type,
    const std::string& local_ip,
    const std::string& nrf_uri,
    const std::string& upf_ip,
    const std::string& mongo_uri,
    const std::string& scp_uri,
    bool smf_upf_colocated
) const {
    const std::string nf = normalize_nf_type(nf_type);
    const int sbi_port = sbi_port_for_nf_type(nf);
    const bool has_nrf_uri = !nrf_uri.empty();
    const bool has_scp_uri = !scp_uri.empty();

    const auto append_common_header = [&](std::ostringstream& oss, const char* log_name) {
        oss << "logger:\n";
        oss << "  level: info\n";
        if (log_name && *log_name) {
            oss << "  file:\n";
            // Use a writable path across heterogeneous Open5GS images.
            oss << "    path: /tmp/open5gs/" << log_name << ".log\n";
        }
        oss << "global:\n";
        oss << "  max:\n";
        oss << "    ue: 2048\n";
    };

    const auto append_sbi_client = [&](std::ostringstream& oss, bool prefer_scp) {
        if (!has_nrf_uri && !has_scp_uri) return;
        oss << "    client:\n";
        if (prefer_scp && has_scp_uri) {
            oss << "      scp:\n";
            oss << "        - uri: " << scp_uri << "\n";
            if (has_nrf_uri) {
                oss << "      nrf:\n";
                oss << "        - uri: " << nrf_uri << "\n";
            }
            return;
        }
        if (has_nrf_uri) {
            oss << "      nrf:\n";
            oss << "        - uri: " << nrf_uri << "\n";
        } else if (has_scp_uri) {
            oss << "      scp:\n";
            oss << "        - uri: " << scp_uri << "\n";
        }
    };

    std::ostringstream oss;

    if (nf == "nrf") {
        append_common_header(oss, "nrf");
        oss << "nrf:\n";
        oss << "  serving:\n";
        oss << "    - plmn_id:\n";
        oss << "        mcc: 999\n";
        oss << "        mnc: 70\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        return oss.str();
    }

    if (nf == "amf") {
        append_common_header(oss, "amf");
        oss << "amf:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        oss << "  ngap:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "  metrics:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: 9090\n";
        oss << "  guami:\n";
        oss << "    - plmn_id:\n";
        oss << "        mcc: 999\n";
        oss << "        mnc: 70\n";
        oss << "      amf_id:\n";
        oss << "        region: 2\n";
        oss << "        set: 1\n";
        oss << "  tai:\n";
        oss << "    - plmn_id:\n";
        oss << "        mcc: 999\n";
        oss << "        mnc: 70\n";
        oss << "      tac: 1\n";
        oss << "  plmn_support:\n";
        oss << "    - plmn_id:\n";
        oss << "        mcc: 999\n";
        oss << "        mnc: 70\n";
        oss << "      s_nssai:\n";
        oss << "        - sst: 1\n";
        oss << "          sd: '000001'\n";
        oss << "  security:\n";
        oss << "    integrity_order: [ NIA2, NIA1, NIA0 ]\n";
        oss << "    ciphering_order: [ NEA0, NEA1, NEA2 ]\n";
        oss << "  network_name:\n";
        oss << "    full: Open5GS\n";
        oss << "    short: Next\n";
        oss << "  amf_name: open5gs-amf0\n";
        oss << "  time:\n";
        oss << "    t3512:\n";
        oss << "      value: 540\n";
        return oss.str();
    }

    if (nf == "smf") {
        append_common_header(oss, "smf");
        const int smf_pfcp_server_port = smf_upf_colocated ? 8806 : 8805;
        oss << "smf:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        oss << "  pfcp:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        // If SMF and UPF are collocated, move SMF PFCP to 8806 to avoid local bind collision.
        oss << "        port: " << smf_pfcp_server_port << "\n";
        oss << "    client:\n";
        oss << "      upf:\n";
        oss << "        - address: " << (upf_ip.empty() ? local_ip : upf_ip) << "\n";
        oss << "          port: 8805\n";
        oss << "  gtpc:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: 2124\n";
        oss << "  gtpu:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: 2153\n";
        oss << "  session:\n";
        oss << "    - subnet: 10.45.0.0/16\n";
        oss << "      gateway: 10.45.0.1\n";
        oss << "    - subnet: 2001:db8:cafe::/48\n";
        oss << "      gateway: 2001:db8:cafe::1\n";
        oss << "  dns:\n";
        oss << "    - 8.8.8.8\n";
        oss << "    - 8.8.4.4\n";
        oss << "  mtu: 1400\n";
        return oss.str();
    }

    if (nf == "upf") {
        append_common_header(oss, "upf");
        oss << "upf:\n";
        oss << "  pfcp:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: 8805\n";
        oss << "  gtpu:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: 2152\n";
        oss << "  session:\n";
        oss << "    - subnet: 10.45.0.0/16\n";
        oss << "      gateway: 10.45.0.1\n";
        oss << "    - subnet: 2001:db8:cafe::/48\n";
        oss << "      gateway: 2001:db8:cafe::1\n";
        return oss.str();
    }

    if (nf == "ausf") {
        append_common_header(oss, "ausf");
        oss << "ausf:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        return oss.str();
    }

    if (nf == "udm") {
        append_common_header(oss, "udm");
        oss << "udm:\n";
        oss << "  hnet:\n";
        oss << "    - id: 1\n";
        oss << "      scheme: 1\n";
        oss << "      key: /etc/open5gs/hnet/curve25519-1.key\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        return oss.str();
    }

    if (nf == "udr") {
        if (!mongo_uri.empty()) {
            oss << "db_uri: " << mongo_uri << "\n";
        }
        append_common_header(oss, "udr");
        oss << "udr:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        return oss.str();
    }

    if (nf == "pcf") {
        if (!mongo_uri.empty()) {
            oss << "db_uri: " << mongo_uri << "\n";
        }
        append_common_header(oss, "pcf");
        oss << "pcf:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        oss << "  metrics:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: 9090\n";
        return oss.str();
    }

    if (nf == "nssf") {
        append_common_header(oss, "nssf");
        oss << "nssf:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        if (has_nrf_uri) {
            oss << "      nsi:\n";
            oss << "        - uri: " << nrf_uri << "\n";
            oss << "          s_nssai:\n";
            oss << "            sst: 1\n";
            oss << "            sd: '000001'\n";
        }
        return oss.str();
    }

    if (nf == "scp") {
        append_common_header(oss, "scp");
        oss << "scp:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, true);
        return oss.str();
    }

    if (nf == "bsf") {
        append_common_header(oss, "bsf");
        oss << "bsf:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        return oss.str();
    }

    if (nf == "sepp") {
        append_common_header(oss, "sepp");
        oss << "sepp:\n";
        oss << "  sbi:\n";
        oss << "    server:\n";
        oss << "      - address: " << local_ip << "\n";
        oss << "        port: " << sbi_port << "\n";
        append_sbi_client(oss, false);
        oss << "  n32:\n";
        oss << "    server:\n";
        oss << "      - sender: sepp.localdomain\n";
        return oss.str();
    }

    // Fallback minimal SBI-style config for unknown NF.
    append_common_header(oss, nf.c_str());
    oss << nf << ":\n";
    oss << "  sbi:\n";
    oss << "    server:\n";
    oss << "      - address: " << local_ip << "\n";
    oss << "        port: " << sbi_port << "\n";
    append_sbi_client(oss, false);
    return oss.str();
}

bool DeploymentOrchestratorService::write_config_to_container(
    const std::string& container_name,
    const std::string& nf_type,
    const std::string& content
) const {
    if (container_name.empty() || nf_type.empty()) return false;
    int code = 0;
    const std::string nf = normalize_nf_type(nf_type);
    const std::string config_in_container = "/tmp/open5gs/" + nf + ".yaml";
    const std::string mkdir_cmd = "docker exec -u 0 " + container_name + " sh -lc 'mkdir -p /tmp/open5gs'";
    if (!(run_shell_command(mkdir_cmd, &code) && code == 0)) return false;

    const auto tmp_dir = std::filesystem::temp_directory_path();
    const auto ts = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const std::string tmp_name =
        "sfc-open5gs-" + sanitize_for_filename(container_name) + "-" + sanitize_for_filename(nf) +
        "-" + std::to_string(ts) + ".yaml";
    const std::filesystem::path tmp_path = tmp_dir / tmp_name;

    {
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs << content;
    }

    std::string cp_cmd = "docker cp " + tmp_path.string() + " " + container_name + ":" + config_in_container;
    const bool cp_ok = run_shell_command(cp_cmd, &code) && code == 0;
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    return cp_ok;
}

std::string DeploymentOrchestratorService::normalize_rendered_nf_config(
    const std::string& nf_type,
    const std::string& raw_config,
    bool smf_upf_colocated
) const {
    std::string out = raw_config;
    if (normalize_nf_type(nf_type) != "smf") {
        return out;
    }

    const int expected_port = smf_upf_colocated ? 8806 : 8805;
    const std::size_t pfcp_anchor = out.find("pfcp:\n    server:\n");
    if (pfcp_anchor == std::string::npos) {
        return out;
    }
    const std::size_t port_pos = out.find("port:", pfcp_anchor);
    if (port_pos == std::string::npos) {
        return out;
    }
    const std::size_t line_end = out.find('\n', port_pos);
    if (line_end == std::string::npos) {
        return out;
    }
    const std::string expected = "port: " + std::to_string(expected_port);
    const std::string current = trim_copy(out.substr(port_pos, line_end - port_pos));
    if (current != expected) {
        out.replace(port_pos, line_end - port_pos, expected);
        spdlog::warn(
            "Auto-migrated SMF PFCP server port to {} (smf_upf_colocated={}) before daemon start",
            expected_port,
            smf_upf_colocated
        );
    }
    return out;
}

bool DeploymentOrchestratorService::start_nf_in_container(
    const std::string& container_name,
    const std::string& nf_type,
    const std::string& config_content,
    bool smf_upf_colocated
) {
    if (container_name.empty() || nf_type.empty()) return false;
    const std::string nf = normalize_nf_type(nf_type);
    const std::string daemon = daemon_for_nf_type(nf);
    const std::string config_path = "/tmp/open5gs/" + nf + ".yaml";
    const std::string log_path = "/tmp/open5gs/" + daemon + ".log";

    if (!wait_for_container_running(container_name, 5, 220)) {
        const std::string restart_cmd = "docker start " + container_name + " >/dev/null 2>&1";
        int restart_code = 0;
        run_shell_command(restart_cmd, &restart_code);
        if (!wait_for_container_running(container_name, 8, 250)) {
            log_container_diagnostics(container_name, "nf_start_container_not_running");
            spdlog::warn("Container {} not running before starting {}", container_name, nf);
            return false;
        }
    }

    {
        int prep_code = 0;
        const std::string prep_cmd =
            "docker exec -u 0 " + container_name +
            " sh -lc 'mkdir -p /tmp/open5gs; chmod 777 /tmp/open5gs >/dev/null 2>&1 || true'";
        run_shell_command(prep_cmd, &prep_code);
    }

    const std::string normalized_config = normalize_rendered_nf_config(nf, config_content, smf_upf_colocated);
    if (!write_config_to_container(container_name, nf, normalized_config)) {
        log_container_diagnostics(container_name, "write_config_failed");
        spdlog::warn("Failed to write config for {} in {}", nf, container_name);
        return false;
    }

    if (nf == "upf") {
        if (!setup_upf_dataplane(container_name)) {
            // Keep UPF start best-effort: some environments (e.g. restricted Docker Desktop)
            // cannot fully provision kernel dataplane devices but daemon can still come up.
            spdlog::warn("UPF dataplane setup not ready in {}, continue with daemon start", container_name);
        }
    }

    int code = 0;
    std::ostringstream oss;
    oss
        << "docker exec -u 0 " << container_name
        << " sh -lc 'DAEMON_BIN=$(command -v " << daemon << " 2>/dev/null || true); "
        << "if [ -z \"$DAEMON_BIN\" ] && [ -x /opt/open5gs/bin/" << daemon << " ]; then "
        << "DAEMON_BIN=/opt/open5gs/bin/" << daemon << "; fi; "
        << "if [ -n \"$DAEMON_BIN\" ]; then "
        << "pkill -x \"" << daemon << "\" >/dev/null 2>&1 || true; "
        << ": > " << log_path << "; "
        << "nohup \"$DAEMON_BIN\" -c " << config_path << " >" << log_path << " 2>&1 & "
        << "sleep 1; "
        << "pgrep -x \"" << daemon << "\" >/dev/null 2>&1; "
        << "else "
        << "exit 2; "
        << "fi'";
    run_shell_command(oss.str(), &code);
    if (code == 0) return true;

    std::string log_tail;
    const std::string tail_cmd =
        "docker exec -u 0 " + container_name + " sh -lc 'tail -n 80 " + log_path + " 2>/dev/null || true'";
    run_shell_command_capture(tail_cmd, &log_tail, &code);
    spdlog::warn(
        "Failed to start {} in {} (nf={}): {}",
        daemon,
        container_name,
        nf,
        trim_copy(tail_lines(log_tail, 12))
    );
    return false;
}

bool DeploymentOrchestratorService::ensure_container_tun_device(const std::string& container_name) const {
    if (container_name.empty()) return false;
    int code = 0;
    const std::string cmd =
        "docker exec -u 0 " + container_name +
        " sh -lc 'mkdir -p /dev/net; "
        "if [ ! -c /dev/net/tun ]; then mknod /dev/net/tun c 10 200 >/dev/null 2>&1 || true; fi; "
        "chmod 666 /dev/net/tun >/dev/null 2>&1 || true; "
        "test -c /dev/net/tun'";
    run_shell_command(cmd, &code);
    return code == 0;
}

bool DeploymentOrchestratorService::setup_upf_dataplane(const std::string& container_name) const {
    if (container_name.empty()) return false;
    if (!ensure_container_tun_device(container_name)) {
        spdlog::warn("UPF dataplane precheck failed: /dev/net/tun unavailable in {}", container_name);
        return false;
    }

    int code = 0;
    const std::string cmd =
        "docker exec -u 0 " + container_name +
        " sh -lc '"
        "set -e; "
        "sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1 || true; "
        "sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null 2>&1 || true; "
        "ip tuntap add name ogstun mode tun >/dev/null 2>&1 || true; "
        "ip link set ogstun up >/dev/null 2>&1 || true; "
        "ip addr add 10.45.0.1/16 dev ogstun >/dev/null 2>&1 || true; "
        "ip -6 addr add 2001:db8:cafe::1/48 dev ogstun >/dev/null 2>&1 || true; "
        "iptables -t nat -C POSTROUTING -s 10.45.0.0/16 ! -o ogstun -j MASQUERADE >/dev/null 2>&1 || "
        "iptables -t nat -A POSTROUTING -s 10.45.0.0/16 ! -o ogstun -j MASQUERADE >/dev/null 2>&1 || true; "
        "iptables -C FORWARD -i ogstun -j ACCEPT >/dev/null 2>&1 || iptables -A FORWARD -i ogstun -j ACCEPT >/dev/null 2>&1 || true; "
        "iptables -C FORWARD -o ogstun -j ACCEPT >/dev/null 2>&1 || iptables -A FORWARD -o ogstun -j ACCEPT >/dev/null 2>&1 || true; "
        "ip6tables -t nat -C POSTROUTING -s 2001:db8:cafe::/48 ! -o ogstun -j MASQUERADE >/dev/null 2>&1 || "
        "ip6tables -t nat -A POSTROUTING -s 2001:db8:cafe::/48 ! -o ogstun -j MASQUERADE >/dev/null 2>&1 || true; "
        "ip -o link show ogstun >/dev/null 2>&1; "
        "'";
    run_shell_command(cmd, &code);
    if (code != 0) {
        std::string debug;
        run_shell_command_capture(
            "docker exec -u 0 " + container_name +
                " sh -lc 'ip -o link show ogstun 2>/dev/null || true; ip addr show ogstun 2>/dev/null || true; "
                "iptables -t nat -S 2>/dev/null | tail -n 30 || true'",
            &debug,
            &code
        );
        spdlog::warn("UPF dataplane setup failed in {} details={}", container_name, trim_copy(tail_lines(debug, 20)));
        return false;
    }
    return true;
}

bool DeploymentOrchestratorService::check_nrf_registration(
    const std::string& nrf_container,
    const std::string& nrf_ip,
    const std::vector<std::string>& started_nfs
) const {
    if (nrf_container.empty() || nrf_ip.empty()) return false;

    std::unordered_set<std::string> wanted;
    for (const auto& nf_raw : started_nfs) {
        const std::string nf = normalize_nf_type(nf_raw);
        if (!nf_registers_to_nrf(nf)) continue;
        if (nf == "nrf") continue;
        wanted.insert(nf_type_to_3gpp(nf));
    }
    if (wanted.empty()) return true;

    auto fetch_url = [&](const std::string& url, std::string* output) -> bool {
        if (output) output->clear();
        int code = 0;
        std::string cmd_output;
        const std::string cmd_h2 =
            "docker exec " + nrf_container +
            " sh -lc 'curl --http2-prior-knowledge -fsS --max-time 3 \"" + url + "\"'";
        run_shell_command_capture(cmd_h2, &cmd_output, &code);
        if (code == 0) {
            if (output) *output = cmd_output;
            return true;
        }

        const std::string cmd_http1 =
            "docker exec " + nrf_container +
            " sh -lc 'curl -fsS --max-time 3 \"" + url + "\"'";
        run_shell_command_capture(cmd_http1, &cmd_output, &code);
        if (output) *output = cmd_output;
        return code == 0;
    };

    const std::vector<std::string> nrf_hosts = {nrf_ip, nrf_container};
    std::string last_probe_output;
    std::unordered_set<std::string> last_found;

    for (int i = 0; i < 20; ++i) {
        std::unordered_set<std::string> found;
        for (const auto& host : nrf_hosts) {
            if (host.empty()) continue;

            const std::string list_url =
                "http://" + host + ":" + std::to_string(sbi_port_for_nf_type("nrf")) +
                "/nnrf-nfm/v1/nf-instances";
            std::string list_output;
            if (!fetch_url(list_url, &list_output)) {
                last_probe_output = list_output;
                continue;
            }

            const nlohmann::json list_json = nlohmann::json::parse(list_output, nullptr, false);
            if (list_json.is_discarded()) {
                last_probe_output = list_output;
                continue;
            }
            collect_nf_types_from_json(list_json, &found);

            const auto links_it = list_json.find("_links");
            if (links_it == list_json.end() || !links_it->is_object()) continue;
            const auto item_it = links_it->find("item");
            if (item_it == links_it->end() || !item_it->is_array()) continue;

            for (const auto& item : *item_it) {
                if (!item.is_object()) continue;
                const auto href_it = item.find("href");
                if (href_it == item.end() || !href_it->is_string()) continue;

                std::string detail_output;
                if (!fetch_url(href_it->get<std::string>(), &detail_output)) {
                    continue;
                }
                const nlohmann::json detail_json = nlohmann::json::parse(detail_output, nullptr, false);
                if (detail_json.is_discarded()) continue;
                collect_nf_types_from_json(detail_json, &found);
            }
        }

        bool all_found = true;
        for (const auto& nf : wanted) {
            if (found.find(nf) == found.end()) {
                all_found = false;
                break;
            }
        }
        if (all_found) return true;

        last_found = std::move(found);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    spdlog::warn(
        "NRF registration incomplete in {} wanted=[{}] found=[{}] probe={}",
        nrf_container,
        join_sorted(wanted),
        join_sorted(last_found),
        trim_copy(tail_lines(last_probe_output, 20))
    );
    return false;
}

bool DeploymentOrchestratorService::check_smf_upf_pfcp_ready(
    const std::string& smf_container,
    const std::string& upf_container,
    int timeout_seconds
) const {
    if (smf_container.empty() || upf_container.empty()) return false;
    timeout_seconds = std::max(1, timeout_seconds);

    int code = 0;
    auto has_pfcp_associated = [&](const std::string& container, const std::string& daemon) -> bool {
        const std::string log_path = "/tmp/open5gs/" + daemon + ".log";
        const std::string cmd =
            "docker exec -u 0 " + container +
            " sh -lc 'test -f " + log_path + " && grep -q \"PFCP associated\" " + log_path + "'";
        run_shell_command(cmd, &code);
        return code == 0;
    };

    for (int i = 0; i < timeout_seconds; ++i) {
        const bool smf_ok = has_pfcp_associated(smf_container, "open5gs-smfd");
        const bool upf_ok = has_pfcp_associated(upf_container, "open5gs-upfd");
        if (smf_ok && upf_ok) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::string smf_tail;
    std::string upf_tail;
    run_shell_command_capture(
        "docker exec -u 0 " + smf_container + " sh -lc 'tail -n 80 /tmp/open5gs/open5gs-smfd.log 2>/dev/null || true'",
        &smf_tail,
        &code
    );
    run_shell_command_capture(
        "docker exec -u 0 " + upf_container + " sh -lc 'tail -n 80 /tmp/open5gs/open5gs-upfd.log 2>/dev/null || true'",
        &upf_tail,
        &code
    );
    spdlog::warn(
        "PFCP association not ready between SMF={} and UPF={} smf_tail={} upf_tail={}",
        smf_container,
        upf_container,
        trim_copy(tail_lines(smf_tail, 10)),
        trim_copy(tail_lines(upf_tail, 10))
    );
    return false;
}

bool DeploymentOrchestratorService::run_shell_command_capture(
    const std::string& cmd,
    std::string* output,
    int* code
) const {
    if (output) output->clear();
    if (cmd.empty()) {
        if (code) *code = -1;
        return false;
    }

    const std::string wrapped = cmd + " 2>&1";
    FILE* fp = ::popen(wrapped.c_str(), "r");
    if (!fp) {
        if (code) *code = -1;
        return false;
    }

    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        if (output) output->append(buf.data());
    }
    const int rc = ::pclose(fp);
    if (code) *code = decode_exit_code(rc);
    return true;
}

bool DeploymentOrchestratorService::run_shell_command(const std::string& cmd, int* code) const {
    if (cmd.empty()) {
        if (code) *code = -1;
        return false;
    }
    const int rc = std::system(cmd.c_str());
    if (code) *code = decode_exit_code(rc);
    return true;
}

std::string DeploymentOrchestratorService::node_to_container_name(
    const std::string& deployment_id,
    const std::string& node_id
) {
    auto norm = [](const std::string& input) {
        std::string out;
        out.reserve(input.size());
        bool last_dash = false;
        for (char ch : input) {
            const unsigned char u = static_cast<unsigned char>(ch);
            if (std::isalnum(u)) {
                out.push_back(static_cast<char>(std::tolower(u)));
                last_dash = false;
                continue;
            }
            if (!last_dash) {
                out.push_back('-');
                last_dash = true;
            }
        }
        while (!out.empty() && out.back() == '-') out.pop_back();
        if (out.empty()) out = "x";
        return out;
    };

    return "sfc-sat-" + norm(deployment_id) + "-" + norm(node_id);
}

std::string DeploymentOrchestratorService::legacy_node_container_name(const std::string& node_id) {
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
