#include "controllers/UERANSIMController.h"
#include "controllers/SFCController.h"
#include "services/AuthGlobals.h"
#include "services/DeploymentOrchestratorService.h"
#include "utils/json_converter.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfc {
namespace {

using json = nlohmann::json;

struct CommandResult {
    int code = -1;
    std::string output;
};

struct UEStatus {
    std::string key;
    std::string label;
    std::string imsi;
    std::string container;
    std::string ip;
    bool registered = false;
    bool pdu_session = false;
    bool receiver_ready = false;
    std::string log_tail;
    std::string received_messages;
};

struct ValidationStep {
    std::string key;
    std::string label;
    std::string status = "pending";
    std::string detail;
};

struct ValidationJob {
    std::string job_id;
    std::string deployment_id;
    std::string mode = "smoke";
    std::string status = "queued";
    int progress = 0;
    std::string message;
    std::string started_at;
    std::string finished_at;
    std::string image;
    std::string network;
    std::string tmp_dir;
    bool stop_requested = false;
    std::string amf_container;
    std::string amf_ip;
    std::string gnb_container;
    std::string gnb_ip;
    bool data_plane_verified = false;
    json deployment;
    json containers = json::array();
    json reschedule_report = json::object();
    std::vector<ValidationStep> steps;
    std::vector<UEStatus> ues;
    std::vector<std::string> logs;
};

struct RuntimeHealth {
    int containers_total = 0;
    int containers_running = 0;
    int core_nfs_total = 0;
    int core_nfs_running = 0;
    bool ready = false;
};

std::mutex g_jobs_mutex;
std::unordered_map<std::string, std::shared_ptr<ValidationJob>> g_jobs;
std::atomic<uint64_t> g_job_seq{1};

std::string iso_now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

std::string normalize_nf_token(std::string raw) {
    raw = trim_copy(raw);
    std::string out;
    out.reserve(raw.size());
    bool sep = false;
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            sep = false;
        } else if (!sep && !out.empty()) {
            out.push_back('_');
            sep = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.rfind("open5gs_", 0) == 0) out = out.substr(std::string("open5gs_").size());
    if (out.size() > 1 && out.back() == 'd') {
        const std::string maybe_nf = out.substr(0, out.size() - 1);
        static const std::unordered_set<std::string> known = {
            "nrf", "amf", "smf", "upf", "ausf", "udm", "udr", "pcf", "nssf", "scp", "bsf", "sepp"
        };
        if (known.count(maybe_nf)) out = maybe_nf;
    }
    return out;
}

std::string normalize_token(const std::string& raw) {
    std::string out;
    bool dash = false;
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            dash = false;
        } else if (!dash && !out.empty()) {
            out.push_back('-');
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "x" : out;
}

std::string digits_only(const std::string& raw) {
    std::string out;
    for (char ch : raw) {
        if (std::isdigit(static_cast<unsigned char>(ch))) out.push_back(ch);
    }
    return out;
}

std::string hex_encode(const std::string& raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char ch : raw) {
        out.push_back(kHex[ch >> 4]);
        out.push_back(kHex[ch & 0xf]);
    }
    return out;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

CommandResult run_cmd(const std::string& cmd) {
    CommandResult result;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) {
        result.output = "popen_failed";
        return result;
    }
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) result.output += buffer;
    const int rc = pclose(pipe);
    if (WIFEXITED(rc)) result.code = WEXITSTATUS(rc);
    else result.code = rc;
    return result;
}

bool run_ok(const std::string& cmd, std::string* output = nullptr) {
    const auto res = run_cmd(cmd);
    if (output) *output = res.output;
    return res.code == 0;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

void append_log(const std::shared_ptr<ValidationJob>& job, const std::string& line) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    job->logs.push_back("[" + iso_now() + "] " + line);
    if (job->logs.size() > 900) {
        job->logs.erase(job->logs.begin(), job->logs.begin() + static_cast<long>(job->logs.size() - 900));
    }
    job->message = line;
}

void set_job_status(const std::shared_ptr<ValidationJob>& job, const std::string& status, int progress, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    if (job->stop_requested && status != "stopped") return;
    job->status = status;
    job->progress = std::max(0, std::min(100, progress));
    job->message = message;
    if (status == "success" || status == "failed" || status == "stopped") {
        job->finished_at = iso_now();
    }
}

bool job_stop_requested(const std::shared_ptr<ValidationJob>& job) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    return job->stop_requested;
}

void throw_if_stopped(const std::shared_ptr<ValidationJob>& job) {
    if (job_stop_requested(job)) {
        throw std::runtime_error("verification_stopped");
    }
}

void set_step(const std::shared_ptr<ValidationJob>& job, const std::string& key, const std::string& status, const std::string& detail) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    for (auto& step : job->steps) {
        if (step.key == key) {
            step.status = status;
            step.detail = detail;
            return;
        }
    }
}

std::string nf_daemon(const std::string& raw) {
    const std::string nf = normalize_nf_token(raw);
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

std::string node_container_name(const std::string& deployment_id, const std::string& node_id) {
    return "sfc-sat-" + normalize_token(deployment_id) + "-" + normalize_token(node_id);
}

std::string legacy_container_name(const std::string& node_id) {
    return "sfc-sat-" + normalize_token(node_id);
}

std::vector<std::string> deployment_container_prefixes(const json& dep) {
    std::vector<std::string> prefixes;
    const std::string dep_id = dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")));
    const std::string backend_dep_id = dep.value("backend_deployment_id", dep_id);
    if (!dep_id.empty()) prefixes.push_back("sfc-sat-" + normalize_token(dep_id) + "-");
    if (!backend_dep_id.empty() && backend_dep_id != dep_id) {
        prefixes.push_back("sfc-sat-" + normalize_token(backend_dep_id) + "-");
    }
    std::sort(prefixes.begin(), prefixes.end());
    prefixes.erase(std::unique(prefixes.begin(), prefixes.end()), prefixes.end());
    return prefixes;
}

bool container_matches_prefixes(const std::string& container, const std::vector<std::string>& prefixes) {
    if (container.empty() || prefixes.empty()) return true;
    return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& prefix) {
        return container.rfind(prefix, 0) == 0;
    });
}

bool is_container_running(const std::string& container) {
    if (container.empty()) return false;
    return run_ok("docker inspect -f '{{.State.Running}}' " + shell_quote(container) + " | grep -q true");
}

json inspect_container(const std::string& container) {
    const auto res = run_cmd("docker inspect " + shell_quote(container));
    if (res.code != 0) return json::array();
    try {
        return json::parse(res.output);
    } catch (...) {
        return json::array();
    }
}

std::string container_ip_on_network(const std::string& container, const std::string& network = "") {
    const auto info = inspect_container(container);
    if (!info.is_array() || info.empty()) return "";
    const auto networks = info[0].value("NetworkSettings", json::object()).value("Networks", json::object());
    if (!network.empty() && networks.contains(network)) {
        return networks[network].value("IPAddress", std::string(""));
    }
    for (const auto& item : networks.items()) {
        const std::string ip = item.value().value("IPAddress", std::string(""));
        if (!ip.empty()) return ip;
    }
    return "";
}

std::string first_network_for_container(const std::string& container) {
    const auto info = inspect_container(container);
    if (!info.is_array() || info.empty()) return "";
    const auto networks = info[0].value("NetworkSettings", json::object()).value("Networks", json::object());
    const char* env_network = std::getenv("SFC_OPEN5GS_NETWORK");
    if (env_network && networks.contains(env_network)) return env_network;
    if (networks.contains("sfc-open5gs-net")) return "sfc-open5gs-net";
    for (const auto& item : networks.items()) return item.key();
    return "";
}

bool process_running_in_container(const std::string& container, const std::string& daemon) {
    if (container.empty() || daemon.empty()) return false;
    return run_ok(
        "docker exec " + shell_quote(container) +
        " sh -lc " + shell_quote("pgrep -x " + shell_quote(daemon) + " >/dev/null 2>&1 || pgrep -f " + shell_quote(daemon) + " >/dev/null 2>&1")
    );
}

std::string find_container_by_daemon(
    const std::string& daemon,
    const std::vector<std::string>& deployment_prefixes = {}
) {
    const auto ps = run_cmd("docker ps --format '{{.Names}}'");
    for (const auto& name : lines_of(ps.output)) {
        if (!container_matches_prefixes(name, deployment_prefixes)) continue;
        if (name.rfind("sfc-sat-", 0) == 0 && process_running_in_container(name, daemon)) {
            return name;
        }
    }
    return "";
}

std::string find_container_for_node(const std::string& deployment_id, const std::string& node_id) {
    const std::vector<std::string> candidates = {
        node_container_name(deployment_id, node_id),
        legacy_container_name(node_id),
    };
    for (const auto& c : candidates) {
        if (is_container_running(c)) return c;
    }

    const std::string suffix = "-" + normalize_token(node_id);
    const auto ps = run_cmd("docker ps --format '{{.Names}}'");
    for (const auto& name : lines_of(ps.output)) {
        if (name.rfind("sfc-sat-", 0) == 0 && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return name;
        }
    }
    return "";
}

json per_nf_array(const json& dep) {
    auto pick = [](const json& obj, const std::string& key) -> std::optional<json> {
        if (obj.contains(key) && obj[key].is_array() && !obj[key].empty()) return obj[key];
        return std::nullopt;
    };
    for (const auto& key : {"per_core_nf", "per_vnf", "core_nfs", "vnfs"}) {
        if (auto arr = pick(dep, key)) return *arr;
    }
    if (dep.contains("candidate") && dep["candidate"].is_object()) {
        const auto& c = dep["candidate"];
        for (const auto& key : {"per_core_nf", "per_vnf", "core_nfs", "vnfs"}) {
            if (auto arr = pick(c, key)) return *arr;
        }
    }
    if (dep.contains("decision_process") && dep["decision_process"].is_object()) {
        const auto& d = dep["decision_process"];
        for (const auto& key : {"per_core_nf", "per_vnf", "core_nfs", "vnfs"}) {
            if (auto arr = pick(d, key)) return *arr;
        }
    }
    return json::array();
}

std::string json_string_any(const json& j, const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
        if (j.contains(key) && j[key].is_number_integer()) return std::to_string(j[key].get<int64_t>());
        if (j.contains(key) && j[key].is_number_unsigned()) return std::to_string(j[key].get<uint64_t>());
    }
    return "";
}

std::vector<std::string> sorted_strings(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::vector<std::string> deployed_nodes_from_dep(const json& dep) {
    std::vector<std::string> nodes;
    if (dep.contains("deployed_nodes") && dep["deployed_nodes"].is_array()) {
        for (const auto& n : dep["deployed_nodes"]) {
            if (n.is_string()) nodes.push_back(n.get<std::string>());
        }
    }
    for (const auto& nf : per_nf_array(dep)) {
        const std::string node = json_string_any(nf, {"node", "node_id", "satellite", "satellite_id", "selected_node", "target_node", "placement_node"});
        if (!node.empty()) nodes.push_back(node);
    }
    nodes = sorted_strings(std::move(nodes));
    return nodes;
}

std::unordered_map<std::string, DeploymentOrchestratorService::NodeRuntimeSnapshot> runtime_snapshot_by_node() {
    if (!g_deployment_orchestrator) return {};
    return g_deployment_orchestrator->snapshot_node_runtime();
}

bool runtime_has_nf(const DeploymentOrchestratorService::NodeRuntimeSnapshot& snap, const std::string& nf_type) {
    const std::string wanted = normalize_nf_token(nf_type);
    for (const auto& nf : snap.running_core_nf_types) {
        if (normalize_nf_token(nf) == wanted) return true;
    }
    return false;
}

json infer_nf_array_from_runtime(
    const json& dep,
    const std::unordered_map<std::string, DeploymentOrchestratorService::NodeRuntimeSnapshot>& runtime
) {
    json inferred = json::array();
    for (const auto& node : deployed_nodes_from_dep(dep)) {
        auto it = runtime.find(node);
        if (it == runtime.end() || !it->second.deployed) continue;
        for (const auto& nf : it->second.running_core_nf_types) {
            const std::string nf_type = normalize_nf_token(nf);
            if (nf_type.empty()) continue;
            inferred.push_back({{"nf_type", nf_type}, {"node", node}});
        }
    }
    return inferred;
}

json collect_deployment_runtime(const json& dep) {
    const std::string dep_id = dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")));
    const auto runtime = runtime_snapshot_by_node();
    const std::vector<std::string> deployment_prefixes = deployment_container_prefixes(dep);
    std::unordered_map<std::string, std::string> runtime_node_by_nf;
    std::unordered_map<std::string, std::string> runtime_container_by_nf;
    for (const auto& kv : runtime) {
        const auto& snap = kv.second;
        if (!snap.deployed || snap.container_state != "running") continue;
        if (!container_matches_prefixes(snap.container_name, deployment_prefixes)) continue;
        for (const auto& nf : snap.running_core_nf_types) {
            const std::string nf_type = normalize_nf_token(nf);
            if (nf_type.empty()) continue;
            runtime_node_by_nf[nf_type] = kv.first;
            runtime_container_by_nf[nf_type] = snap.container_name;
        }
    }
    json per_nfs = per_nf_array(dep);
    if (per_nfs.empty()) per_nfs = infer_nf_array_from_runtime(dep, runtime);
    json containers = json::array();
    for (const auto& nf : per_nfs) {
        const std::string nf_type = normalize_nf_token(json_string_any(nf, {
            "nf_type", "core_nf_type", "vnf_type", "core_nf", "vnf", "name", "type", "core_nf_id", "vnf_id"
        }));
        if (nf_type.empty()) continue;
        std::string node = json_string_any(nf, {
            "node", "node_id", "satellite", "satellite_id", "selected_node", "target_node", "placement_node"
        });
        const bool explicit_placement_node = !node.empty();
        const auto runtime_node_it = runtime_node_by_nf.find(nf_type);
        if (runtime_node_it != runtime_node_by_nf.end() && !runtime_node_it->second.empty()) {
            node = runtime_node_it->second;
        }
        if (node.empty() && nf_type.size() > 0) {
            std::string unique_node;
            int matches = 0;
            for (const auto& candidate_node : deployed_nodes_from_dep(dep)) {
                auto it = runtime.find(candidate_node);
                if (it != runtime.end() && it->second.deployed && runtime_has_nf(it->second, nf_type)) {
                    unique_node = candidate_node;
                    matches += 1;
                }
            }
            if (matches == 1) node = unique_node;
        }
        const std::string daemon = nf_daemon(nf_type);
        auto rt_it = runtime.find(node);
        const bool runtime_running = rt_it != runtime.end() && rt_it->second.deployed && rt_it->second.container_state == "running";
        const bool runtime_nf_ok = rt_it != runtime.end() && runtime_has_nf(rt_it->second, nf_type);
        const auto runtime_container_it = runtime_container_by_nf.find(nf_type);
        std::string container = runtime_container_it != runtime_container_by_nf.end()
            ? runtime_container_it->second
            : (rt_it != runtime.end() ? rt_it->second.container_name : "");
        bool container_running = is_container_running(container);
        if (container.empty() || (!container_running && !runtime_running)) {
            container = find_container_for_node(dep_id, node);
            container_running = is_container_running(container);
        }
        bool daemon_ok = container_running && process_running_in_container(container, daemon);
        if (!daemon_ok && runtime_nf_ok && container.empty()) daemon_ok = true;
        if (!daemon_ok && !explicit_placement_node) {
            const std::string fallback = find_container_by_daemon(daemon, deployment_prefixes);
            if (!fallback.empty()) {
                container = fallback;
                container_running = is_container_running(container);
                daemon_ok = true;
            }
        }
        containers.push_back({
            {"nf_type", nf_type},
            {"node", node},
            {"daemon", daemon},
            {"container", container},
            {"running", container_running || (container.empty() && runtime_running)},
            {"daemon_running", daemon_ok},
            {"ip", container_ip_on_network(container)},
            {"source", runtime_nf_ok ? "orchestrator_runtime" : "docker_probe"}
        });
    }
    return containers;
}

RuntimeHealth summarize_runtime_health(const json& runtime) {
    RuntimeHealth health;
    if (!runtime.is_array()) return health;

    std::unordered_set<std::string> containers;
    std::unordered_set<std::string> running_containers;
    for (const auto& item : runtime) {
        const std::string container = item.value("container", std::string(""));
        if (!container.empty()) {
            containers.insert(container);
            if (item.value("running", false)) running_containers.insert(container);
        }
        health.core_nfs_total += 1;
        if (item.value("daemon_running", false)) health.core_nfs_running += 1;
    }

    health.containers_total = static_cast<int>(containers.size());
    health.containers_running = static_cast<int>(running_containers.size());
    health.ready =
        health.containers_total > 0 &&
        health.core_nfs_total > 0 &&
        health.containers_running == health.containers_total &&
        health.core_nfs_running == health.core_nfs_total;
    return health;
}

bool deployment_effectively_ready(const json& dep, const json& runtime) {
    const RuntimeHealth health = summarize_runtime_health(runtime);
    const int containers_running = dep.value("containers_running", 0);
    const int containers_total = dep.value("containers_total", 0);
    const int core_nfs_running = dep.value("core_nfs_running", 0);
    const int core_nfs_total = dep.value("core_nfs_total", 0);
    const bool persisted_ready =
        dep.value("service_ready", false) &&
        dep.value("ready_for_ueransim", false) &&
        containers_total > 0 &&
        core_nfs_total > 0 &&
        containers_running == containers_total &&
        core_nfs_running == core_nfs_total;
    return persisted_ready || health.ready;
}

json deployment_runtime_summary(const json& dep) {
    return {
        {"deployment_id", dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")))},
        {"sfc_name", dep.value("sfc_name", dep.value("name", std::string("")))},
        {"orchestration_phase", dep.value("orchestration_phase", std::string(""))},
        {"service_ready", dep.value("service_ready", false)},
        {"ready_for_ueransim", dep.value("ready_for_ueransim", false)},
        {"containers_running", dep.value("containers_running", 0)},
        {"containers_total", dep.value("containers_total", 0)},
        {"core_nfs_running", dep.value("core_nfs_running", 0)},
        {"core_nfs_total", dep.value("core_nfs_total", 0)},
        {"last_error", dep.value("last_error", std::string(""))},
        {"last_update_at", dep.value("last_update_at", dep.value("deployed_at", std::string("")))}
    };
}

std::string deployment_nf_signature(const json& dep) {
    std::vector<std::string> parts;
    for (const auto& nf : per_nf_array(dep)) {
        const std::string nf_type = normalize_nf_token(json_string_any(nf, {
            "nf_type", "core_nf_type", "vnf_type", "core_nf", "vnf", "name", "type", "core_nf_id", "vnf_id"
        }));
        const std::string node = json_string_any(nf, {
            "node", "node_id", "satellite", "satellite_id", "selected_node", "target_node", "placement_node"
        });
        if (!nf_type.empty()) parts.push_back(nf_type + "@" + node);
    }
    parts = sorted_strings(std::move(parts));
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) oss << ",";
        oss << parts[i];
    }
    return oss.str();
}

std::string deployment_nodes_signature(const json& dep) {
    std::vector<std::string> nodes;
    if (dep.contains("deployed_nodes") && dep["deployed_nodes"].is_array()) {
        for (const auto& node : dep["deployed_nodes"]) {
            if (node.is_string()) nodes.push_back(node.get<std::string>());
        }
    }
    nodes = sorted_strings(std::move(nodes));
    std::ostringstream oss;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i) oss << ",";
        oss << nodes[i];
    }
    return oss.str();
}

json build_reschedule_diff(
    const json& before,
    const json& after,
    const json& fault_runtime,
    const json& before_runtime = json::array(),
    const json& after_runtime = json::array()
) {
    std::unordered_map<std::string, json> before_by_nf;
    std::unordered_map<std::string, json> after_by_nf;
    for (const auto& nf : per_nf_array(before)) {
        const std::string nf_type = normalize_nf_token(json_string_any(nf, {
            "nf_type", "core_nf_type", "vnf_type", "core_nf", "vnf", "name", "type", "core_nf_id", "vnf_id"
        }));
        if (!nf_type.empty()) before_by_nf[nf_type] = nf;
    }
    for (const auto& nf : per_nf_array(after)) {
        const std::string nf_type = normalize_nf_token(json_string_any(nf, {
            "nf_type", "core_nf_type", "vnf_type", "core_nf", "vnf", "name", "type", "core_nf_id", "vnf_id"
        }));
        if (!nf_type.empty()) after_by_nf[nf_type] = nf;
    }
    auto overlay_runtime = [](std::unordered_map<std::string, json>& by_nf, const json& runtime) {
        if (!runtime.is_array()) return;
        for (const auto& item : runtime) {
            const bool usable =
                item.value("daemon_running", false) ||
                (item.value("running", false) && item.value("source", std::string("")) == "orchestrator_runtime");
            if (!usable) continue;
            const std::string nf_type = normalize_nf_token(item.value("nf_type", std::string("")));
            const std::string node = item.value("node", std::string(""));
            if (nf_type.empty() || node.empty()) continue;
            json placement = by_nf.count(nf_type) ? by_nf[nf_type] : json::object();
            placement["nf_type"] = nf_type;
            placement["node"] = node;
            if (item.contains("container")) placement["container"] = item["container"];
            by_nf[nf_type] = placement;
        }
    };
    overlay_runtime(before_by_nf, before_runtime);
    overlay_runtime(after_by_nf, after_runtime);

    json moves = json::array();
    int moved_count = 0;
    int total_count = 0;
    const std::string before_dep_id = before.value("deployment_id", before.value("backend_deployment_id", std::string("")));
    const std::string after_dep_id = after.value("deployment_id", after.value("backend_deployment_id", std::string("")));
    for (const auto& kv : before_by_nf) {
        const std::string nf_type = kv.first;
        const std::string from_node = json_string_any(kv.second, {
            "node", "node_id", "satellite", "satellite_id", "selected_node", "target_node", "placement_node"
        });
        const auto it = after_by_nf.find(nf_type);
        const std::string to_node = it == after_by_nf.end() ? "" : json_string_any(it->second, {
            "node", "node_id", "satellite", "satellite_id", "selected_node", "target_node", "placement_node"
        });
        const bool moved = !to_node.empty() && from_node != to_node;
        if (moved) ++moved_count;
        ++total_count;
        const std::string from_container = json_string_any(kv.second, {"container", "container_name"});
        const std::string to_container = it == after_by_nf.end()
            ? ""
            : json_string_any(it->second, {"container", "container_name"});
        moves.push_back({
            {"nf_type", nf_type},
            {"from_node", from_node},
            {"to_node", to_node},
            {"status", moved ? "moved" : (to_node.empty() ? "missing_after" : "unchanged")},
            {"from_container", from_container.empty() ? node_container_name(before_dep_id, from_node) : from_container},
            {"to_container", to_container.empty() && !to_node.empty() ? node_container_name(after_dep_id, to_node) : to_container}
        });
    }

    std::vector<std::string> before_nodes_vec;
    std::vector<std::string> after_nodes_vec;
    if (before.contains("deployed_nodes") && before["deployed_nodes"].is_array()) {
        for (const auto& node : before["deployed_nodes"]) if (node.is_string()) before_nodes_vec.push_back(node.get<std::string>());
    }
    if (after.contains("deployed_nodes") && after["deployed_nodes"].is_array()) {
        for (const auto& node : after["deployed_nodes"]) if (node.is_string()) after_nodes_vec.push_back(node.get<std::string>());
    }
    before_nodes_vec = sorted_strings(std::move(before_nodes_vec));
    after_nodes_vec = sorted_strings(std::move(after_nodes_vec));

    json removed_nodes = json::array();
    json added_nodes = json::array();
    for (const auto& n : before_nodes_vec) {
        if (!std::binary_search(after_nodes_vec.begin(), after_nodes_vec.end(), n)) removed_nodes.push_back(n);
    }
    for (const auto& n : after_nodes_vec) {
        if (!std::binary_search(before_nodes_vec.begin(), before_nodes_vec.end(), n)) added_nodes.push_back(n);
    }

    json stopped_nfs = json::array();
    if (fault_runtime.is_array()) {
        for (const auto& item : fault_runtime) {
            if (!item.value("running", false) || !item.value("daemon_running", false)) {
                stopped_nfs.push_back(item);
            }
        }
    }

    auto recovery_text = [](const json& dep) {
        std::ostringstream oss;
        for (const auto& key : {"orchestration_trigger", "recovery_strategy", "decision_trigger"}) {
            if (dep.contains(key) && dep[key].is_string()) oss << dep[key].get<std::string>() << " ";
        }
        if (dep.contains("decision_process") && dep["decision_process"].is_object()) {
            for (const auto& key : {"recovery_algorithm", "recovery_reason", "detail", "algorithm"}) {
                if (dep["decision_process"].contains(key) && dep["decision_process"][key].is_string()) {
                    oss << dep["decision_process"][key].get<std::string>() << " ";
                }
            }
        }
        return oss.str();
    };
    const std::string after_recovery = normalize_token(recovery_text(after));
    const bool full_redeploy_strategy =
        after_recovery.find("full-redeploy") != std::string::npos ||
        after_recovery.find("multi-node") != std::string::npos ||
        after_recovery.find("overall") != std::string::npos;

    std::string scope = "none";
    if (full_redeploy_strategy || (total_count > 0 && moved_count == total_count)) scope = "overall";
    else if (moved_count > 0 || !removed_nodes.empty() || !added_nodes.empty()) scope = "partial";

    return {
        {"scope", scope},
        {"strategy_hint", recovery_text(after)},
        {"nf_total", total_count},
        {"nf_moved", moved_count},
        {"nf_moves", moves},
        {"removed_nodes", removed_nodes},
        {"added_nodes", added_nodes},
        {"stopped_nfs", stopped_nfs}
    };
}

std::optional<json> find_deployment(const std::string& deployment_id) {
    const json all = list_deployment_records();
    if (!all.is_array()) return std::nullopt;
    for (const auto& dep : all) {
        const std::string dep_id = dep.value("deployment_id", std::string(""));
        const std::string backend_id = dep.value("backend_deployment_id", dep_id);
        if (dep_id == deployment_id || backend_id == deployment_id) return dep;
    }
    return std::nullopt;
}

std::string normalize_sd(std::string raw) {
    raw.erase(std::remove_if(raw.begin(), raw.end(), [](unsigned char c) { return std::isspace(c); }), raw.end());
    if (raw.rfind("0x", 0) == 0 || raw.rfind("0X", 0) == 0) raw = raw.substr(2);
    if (raw.empty()) return "";
    for (char ch : raw) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return "000001";
    }
    unsigned long value = std::stoul(raw, nullptr, 16);
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << (value & 0xffffffUL);
    return oss.str();
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;
    ofs << content;
    return ofs.good();
}

bool ensure_tun_device(const std::string& container) {
    return run_ok(
        "docker exec " + shell_quote(container) + " sh -lc " +
        shell_quote("mkdir -p /dev/net; if [ ! -c /dev/net/tun ]; then mknod /dev/net/tun c 10 200 >/dev/null 2>&1 || true; fi; chmod 666 /dev/net/tun >/dev/null 2>&1 || true; test -c /dev/net/tun")
    );
}

bool start_ueransim_container(
    const std::string& name,
    const std::string& image,
    const std::string& network,
    const std::string& mount
) {
    run_ok("docker rm -f " + shell_quote(name) + " >/dev/null");
    const std::string base =
        "docker run -d --name " + shell_quote(name) +
        " --network " + shell_quote(network) +
        " -v " + shell_quote(mount) + " ";
    const std::string idle = shell_quote("while true; do sleep 3600; done");
    if (run_ok(base + "--cap-add=NET_ADMIN --device=/dev/net/tun " + shell_quote(image) + " sh -lc " + idle) &&
        ensure_tun_device(name)) {
        return true;
    }
    run_ok("docker rm -f " + shell_quote(name) + " >/dev/null");
    if (run_ok(base + "--privileged " + shell_quote(image) + " sh -lc " + idle) && ensure_tun_device(name)) {
        return true;
    }
    run_ok("docker rm -f " + shell_quote(name) + " >/dev/null");
    return false;
}

std::string resolve_bin_path(const std::string& container, const std::vector<std::string>& candidates) {
    std::ostringstream script;
    script << "for p in";
    for (const auto& c : candidates) script << " " << shell_quote(c);
    script << "; do if [ -x \"$p\" ]; then echo \"$p\"; exit 0; fi; q=$(command -v \"$p\" 2>/dev/null || true); if [ -n \"$q\" ]; then echo \"$q\"; exit 0; fi; done; exit 1";
    std::string out;
    if (!run_ok("docker exec " + shell_quote(container) + " sh -lc " + shell_quote(script.str()), &out)) return "";
    return trim_copy(out);
}

bool wait_for_log(const std::string& container, const std::string& logfile, const std::string& pattern, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (run_ok(
                "docker exec " + shell_quote(container) + " sh -lc " +
                shell_quote("test -f " + shell_quote(logfile) + " && grep -Eiq " + shell_quote(pattern) + " " + shell_quote(logfile)))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

bool wait_for_amf_ngap_probe(const std::string& amf_container, int timeout_sec, std::string* detail = nullptr) {
    if (amf_container.empty()) return false;
    const std::string probe =
        "if command -v ss >/dev/null 2>&1; then "
        "  ss -H -l -n -A sctp 2>/dev/null | grep -Eq '(^|[[:space:]:])38412([[:space:]]|$)' && exit 0; "
        "  ss -H -l -n 2>/dev/null | grep -Eq '(^|[[:space:]:])38412([[:space:]]|$)' && exit 0; "
        "fi; "
        "if command -v netstat >/dev/null 2>&1; then "
        "  netstat -an 2>/dev/null | grep -E '38412' | grep -Eiq 'listen|sctp|0\\.0\\.0\\.0|::|172\\.' && exit 0; "
        "fi; "
        "grep -Eiq 'ngap|38412|SCTP' /tmp/open5gs/open5gs-amfd.log 2>/dev/null && exit 0; "
        "exit 1";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
        if (run_ok("docker exec " + shell_quote(amf_container) + " sh -lc " + shell_quote(probe), &last)) {
            if (detail) *detail = trim_copy(last);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (detail) {
        const auto res = run_cmd(
            "docker exec " + shell_quote(amf_container) + " sh -lc " +
            shell_quote("tail -n 40 /tmp/open5gs/open5gs-amfd.log 2>/dev/null || true")
        );
        *detail = res.output;
    }
    return false;
}

bool start_gnb_and_wait_ng_setup(
    const std::shared_ptr<ValidationJob>& job,
    const std::string& phase_tag,
    const std::string& gnb_container,
    const std::string& gnb_bin,
    const std::string& config_path,
    int attempts,
    int wait_each_sec,
    std::string* last_log = nullptr
) {
    const int safe_attempts = std::max(1, attempts);
    auto read_gnb_tail = [&]() {
        const auto res = run_cmd(
            "docker exec " + shell_quote(gnb_container) + " sh -lc " +
            shell_quote("tail -n 80 /tmp/gnb.log 2>/dev/null || true")
        );
        return res.output;
    };
    for (int attempt = 1; attempt <= safe_attempts; ++attempt) {
        throw_if_stopped(job);
        run_ok(
            "docker exec " + shell_quote(gnb_container) + " sh -lc " +
            shell_quote("pkill -f " + shell_quote(gnb_bin) + " >/dev/null 2>&1 || true; rm -f /tmp/gnb.log")
        );
        run_ok(
            "docker exec -d " + shell_quote(gnb_container) + " sh -lc " +
            shell_quote(shell_quote(gnb_bin) + " -c " + shell_quote(config_path) + " > /tmp/gnb.log 2>&1")
        );
        if (wait_for_log(gnb_container, "/tmp/gnb.log", "NG Setup procedure is successful|NG setup successful|NG Setup Response", wait_each_sec)) {
            if (last_log) *last_log = read_gnb_tail();
            return true;
        }
        const std::string tail = read_gnb_tail();
        if (last_log) *last_log = tail;
        const bool retryable =
            tail.find("Connection refused") != std::string::npos ||
            tail.find("SCTP could not connect") != std::string::npos ||
            tail.find("Trying to establish SCTP connection") != std::string::npos ||
            tail.find("timeout") != std::string::npos ||
            tail.find("Timeout") != std::string::npos;
        append_log(
            job,
            phase_tag + ": gNB NG Setup 第 " + std::to_string(attempt) + "/" + std::to_string(safe_attempts) +
            " 次未完成" + (retryable ? "，等待 AMF SCTP 就绪后重试" : "，继续重试")
        );
        if (attempt < safe_attempts) {
            std::this_thread::sleep_for(std::chrono::seconds(retryable ? 5 : 3));
        }
    }
    return false;
}

std::string wait_for_ue_tun_ip(const std::string& container, const std::string& iface, int timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto res = run_cmd(
            "docker exec " + shell_quote(container) + " sh -lc " +
            shell_quote("ip -o -4 addr show dev " + shell_quote(iface) + " 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1")
        );
        const std::string ip = trim_copy(res.output);
        if (!ip.empty()) return ip;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return "";
}

std::string tail_file(const std::string& container, const std::string& file, int lines = 80) {
    const auto res = run_cmd(
        "docker exec " + shell_quote(container) + " sh -lc " +
        shell_quote("tail -n " + std::to_string(lines) + " " + shell_quote(file) + " 2>/dev/null || true")
    );
    return res.output;
}

bool provision_subscriber(
    const std::filesystem::path& dir,
    const std::string& mongo_container,
    const std::string& imsi,
    const std::string& sst,
    const std::string& sd,
    const std::string& apn,
    const std::string& key,
    const std::string& opc
) {
    if (!is_container_running(mongo_container)) return false;
    const std::filesystem::path js_path = dir / ("subscriber-" + imsi + ".js");
    std::ostringstream js;
    js << "const dbx = db.getSiblingDB('open5gs');\n"
       << "const imsi = '" << imsi << "';\n"
       << "const subscriber = {\n"
       << "  imsi, subscriber_status: 0, operator_determined_barring: 0, network_access_mode: 0,\n"
       << "  subscribed_rau_tau_timer: 12, access_restriction_data: 0,\n"
       << "  slice: [{ sst: Number('" << sst << "'), default_indicator: true, ";
    if (!sd.empty()) js << "sd: '" << sd << "', ";
    js << "session: [{ name: '" << apn << "', type: 1, pcc_rule: [], ambr: { uplink: { value: 1, unit: 3 }, downlink: { value: 1, unit: 3 } }, qos: { index: 9, arp: { priority_level: 8, pre_emption_capability: 1, pre_emption_vulnerability: 1 } } }] }],\n"
       << "  ambr: { uplink: { value: 1, unit: 3 }, downlink: { value: 1, unit: 3 } },\n"
       << "  security: { k: '" << key << "', amf: '8000', op: null, opc: '" << opc << "' }, schema_version: 1, __v: 0\n"
       << "};\n"
       << "dbx.subscribers.updateOne({ imsi }, { $set: subscriber }, { upsert: true });\n"
       << "const found = dbx.subscribers.findOne({ imsi }); if (!found) throw new Error('subscriber_upsert_failed'); printjson({imsi: found.imsi, slice: found.slice});\n";
    if (!write_file(js_path, js.str())) return false;
    const std::string remote = "/tmp/" + js_path.filename().string();
    return run_ok("docker cp " + shell_quote(js_path.string()) + " " + shell_quote(mongo_container + ":" + remote)) &&
           run_ok("docker exec " + shell_quote(mongo_container) + " mongosh --quiet " + shell_quote(remote));
}

bool start_receiver(const std::string& container) {
    const std::string script =
        "rm -f /tmp/sfc_ue_rx.log /tmp/sfc_ue_rx.pid; touch /tmp/sfc_ue_rx.log; "
        "if command -v python3 >/dev/null 2>&1; then "
        "nohup python3 -u /config/udp_receiver.py 9090 /tmp/sfc_ue_rx.log >/tmp/sfc_ue_rx_runner.log 2>&1 & echo $! >/tmp/sfc_ue_rx.pid; "
        "elif command -v nc >/dev/null 2>&1; then "
        "nohup sh -lc 'while true; do nc -ul -p 9090 >>/tmp/sfc_ue_rx.log 2>&1; done' >/tmp/sfc_ue_rx_runner.log 2>&1 & echo $! >/tmp/sfc_ue_rx.pid; "
        "else echo '[receiver] no python3/nc available; manual messages will use fallback log' >>/tmp/sfc_ue_rx.log; fi";
    return run_ok("docker exec " + shell_quote(container) + " sh -lc " + shell_quote(script));
}

std::pair<bool, std::string> send_udp_message(
    const std::string& from_container,
    const std::string& to_ip,
    const std::string& message
) {
    const std::string hex = hex_encode(message);
    std::string cmd =
        "if command -v python3 >/dev/null 2>&1; then "
        "python3 /config/udp_send.py " + shell_quote(to_ip) + " 9090 " + shell_quote(hex) + "; "
        "elif command -v nc >/dev/null 2>&1; then "
        "printf %s " + shell_quote(message) + " | nc -u -w1 " + shell_quote(to_ip) + " 9090; "
        "elif command -v bash >/dev/null 2>&1; then "
        "bash -lc " + shell_quote("printf %s " + shell_quote(message) + " > /dev/udp/" + to_ip + "/9090") + "; "
        "else exit 42; fi";
    const auto res = run_cmd("docker exec " + shell_quote(from_container) + " sh -lc " + shell_quote(cmd));
    return {res.code == 0, res.output};
}

void append_ue_message_panel(
    const std::string& container,
    const std::string& line
) {
    if (container.empty() || line.empty()) return;
    run_ok(
        "docker exec " + shell_quote(container) + " sh -lc " +
        shell_quote("printf '%s\n' " + shell_quote(line) + " >> /tmp/sfc_ue_messages.log")
    );
}

bool wait_for_panel_message(
    const std::string& container,
    const std::string& needle,
    int timeout_ms = 1800
) {
    if (container.empty() || needle.empty()) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string merged =
            tail_file(container, "/tmp/sfc_ue_rx.log", 80) +
            tail_file(container, "/tmp/sfc_ue_messages.log", 80);
        if (merged.find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
    }
    return false;
}

void refresh_ue_logs_locked(ValidationJob& job) {
    for (auto& ue : job.ues) {
        ue.log_tail = tail_file(ue.container, "/tmp/ue.log", 90);
        ue.received_messages = tail_file(ue.container, "/tmp/sfc_ue_rx.log", 80) + tail_file(ue.container, "/tmp/sfc_ue_messages.log", 80);
    }
}

json job_to_json(const ValidationJob& job) {
    json steps = json::array();
    for (const auto& s : job.steps) {
        steps.push_back({{"key", s.key}, {"label", s.label}, {"status", s.status}, {"detail", s.detail}});
    }
    json ues = json::array();
    for (const auto& ue : job.ues) {
        ues.push_back({
            {"key", ue.key},
            {"label", ue.label},
            {"imsi", ue.imsi},
            {"container", ue.container},
            {"ip", ue.ip},
            {"registered", ue.registered},
            {"pdu_session", ue.pdu_session},
            {"receiver_ready", ue.receiver_ready},
            {"log_tail", ue.log_tail},
            {"received_messages", ue.received_messages}
        });
    }
    return {
        {"job_id", job.job_id},
        {"deployment_id", job.deployment_id},
        {"mode", job.mode},
        {"status", job.status},
        {"progress", job.progress},
        {"message", job.message},
        {"started_at", job.started_at},
        {"finished_at", job.finished_at},
        {"image", job.image},
        {"network", job.network},
        {"stop_requested", job.stop_requested},
        {"amf_container", job.amf_container},
        {"amf_ip", job.amf_ip},
        {"gnb_container", job.gnb_container},
        {"gnb_ip", job.gnb_ip},
        {"data_plane_verified", job.data_plane_verified},
        {"deployment", job.deployment},
        {"containers", job.containers},
        {"reschedule_report", job.reschedule_report},
        {"steps", steps},
        {"ues", ues},
        {"logs", job.logs}
    };
}

void write_ueransim_helpers(const std::filesystem::path& dir) {
    write_file(dir / "udp_receiver.py",
        "import datetime, socket, sys\n"
        "port=int(sys.argv[1]); path=sys.argv[2]\n"
        "sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(('0.0.0.0', port))\n"
        "open(path,'a').write('[receiver] listening udp/{}\\n'.format(port))\n"
        "while True:\n"
        "    data, addr = sock.recvfrom(65535)\n"
        "    text = data.decode('utf-8', 'replace')\n"
        "    ts = datetime.datetime.now().isoformat(timespec='seconds')\n"
        "    with open(path, 'a') as f: f.write('[{}] from {}:{} {}\\n'.format(ts, addr[0], addr[1], text)); f.flush()\n"
    );
    write_file(dir / "udp_send.py",
        "import socket, sys\n"
        "host=sys.argv[1]; port=int(sys.argv[2]); payload=bytes.fromhex(sys.argv[3])\n"
        "sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.sendto(payload, (host, port)); print('sent', len(payload), 'bytes')\n"
    );
}

void cleanup_job_ueransim_containers(const std::shared_ptr<ValidationJob>& job) {
    std::vector<std::string> containers;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (!job->gnb_container.empty()) containers.push_back(job->gnb_container);
        for (const auto& ue : job->ues) {
            if (!ue.container.empty()) containers.push_back(ue.container);
        }
    }
    for (const auto& c : containers) {
        run_ok("docker rm -f " + shell_quote(c) + " >/dev/null");
    }
}

json run_single_ue_smoke_phase(
    const std::shared_ptr<ValidationJob>& job,
    const json& dep,
    const std::string& phase_tag,
    const std::string& step_key,
    int progress_start,
    int progress_end,
    bool cleanup_after
) {
    const std::string dep_id = dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")));
    auto pct = [&](int step) {
        return progress_start + ((progress_end - progress_start) * step / 5);
    };
    set_job_status(job, "running", progress_start, phase_tag + ": 核对核心网运行态");
    throw_if_stopped(job);

    json runtime = collect_deployment_runtime(dep);
    int nf_total = 0;
    int nf_ok = 0;
    std::string amf_container;
    for (const auto& item : runtime) {
        ++nf_total;
        if (item.value("daemon_running", false)) ++nf_ok;
        if (item.value("nf_type", std::string("")) == "amf") amf_container = item.value("container", std::string(""));
    }
    if (nf_total == 0 || nf_ok != nf_total) {
        throw std::runtime_error(phase_tag + " NF process verification failed: " + std::to_string(nf_ok) + "/" + std::to_string(nf_total));
    }
    if (amf_container.empty()) amf_container = find_container_by_daemon("open5gs-amfd", deployment_container_prefixes(dep));
    if (amf_container.empty()) throw std::runtime_error(phase_tag + " unable to locate AMF container");

    const std::string network = first_network_for_container(amf_container);
    const std::string amf_ip = container_ip_on_network(amf_container, network);
    if (network.empty() || amf_ip.empty()) throw std::runtime_error(phase_tag + " unable to resolve AMF network/IP");
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        job->deployment = dep;
        job->containers = runtime;
        job->amf_container = amf_container;
        job->amf_ip = amf_ip;
        job->network = network;
    }
    append_log(job, phase_tag + ": 核心网容器核验通过 NF=" + std::to_string(nf_ok) + "/" + std::to_string(nf_total) + ", AMF=" + amf_container);

    const std::string image = std::getenv("UERANSIM_IMAGE") ? std::getenv("UERANSIM_IMAGE") : "docker.io/free5gc/ueransim:latest";
    const std::string mcc = std::getenv("UERANSIM_MCC") ? std::getenv("UERANSIM_MCC") : "999";
    const std::string mnc = std::getenv("UERANSIM_MNC") ? std::getenv("UERANSIM_MNC") : "70";
    const std::string tac = std::getenv("UERANSIM_TAC") ? std::getenv("UERANSIM_TAC") : "1";
    const std::string sst = std::getenv("UERANSIM_SST") ? std::getenv("UERANSIM_SST") : "1";
    const std::string sd = normalize_sd(std::getenv("UERANSIM_SD") ? std::getenv("UERANSIM_SD") : "000001");
    const std::string apn = std::getenv("UERANSIM_APN") ? std::getenv("UERANSIM_APN") : "internet";
    const std::string key = std::getenv("UERANSIM_KEY") ? std::getenv("UERANSIM_KEY") : "465B5CE8B199B49FAA5F0A2EE238A6BC";
    const std::string opc = std::getenv("UERANSIM_OPC") ? std::getenv("UERANSIM_OPC") : "E8ED289DEBA952E4283B54E88E6183CA";
    const std::string mongo = std::getenv("SFC_OPEN5GS_MONGO_CONTAINER") ? std::getenv("SFC_OPEN5GS_MONGO_CONTAINER") : "sfc-open5gs-mongo";
    std::string imsi = digits_only(std::getenv("UERANSIM_IMSI") ? std::getenv("UERANSIM_IMSI") : "999700000000001");
    if (imsi.empty()) imsi = "999700000000001";

    const std::string short_id = normalize_token(job->job_id).substr(0, 18);
    const std::string suffix = normalize_token(phase_tag).substr(0, 14);
    const std::string name_base = "sfc-ueransim-" + normalize_token(dep_id).substr(0, 22) + "-" + short_id + "-" + suffix;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name_base;
    std::filesystem::create_directories(dir);
    write_ueransim_helpers(dir);
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        job->image = image;
        job->tmp_dir = dir.string();
        job->gnb_container = name_base + "-gnb";
        job->ues = {{"ue1", "UE-1", imsi, name_base + "-ue1", "", false, false, false, "", ""}};
    }

    set_job_status(job, "running", pct(1), phase_tag + ": 写入 UE 订阅数据");
    throw_if_stopped(job);
    if (!provision_subscriber(dir, mongo, imsi, sst, sd, apn, key, opc)) {
        throw std::runtime_error(phase_tag + " failed to provision subscriber IMSI=" + imsi + " on " + mongo);
    }

    set_job_status(job, "running", pct(2), phase_tag + ": 启动 UERANSIM 容器");
    if (!run_ok("docker image inspect " + shell_quote(image) + " >/dev/null")) {
        if (!run_ok("docker pull " + shell_quote(image) + " >/dev/null")) {
            throw std::runtime_error(phase_tag + " failed to pull UERANSIM image: " + image);
        }
    }
    const std::string mount = dir.string() + ":/config";
    std::string gnb_container;
    std::string ue_container;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        gnb_container = job->gnb_container;
        ue_container = job->ues.front().container;
    }
    if (!start_ueransim_container(gnb_container, image, network, mount)) {
        throw std::runtime_error(phase_tag + " failed to start gNB container");
    }
    throw_if_stopped(job);
    if (!start_ueransim_container(ue_container, image, network, mount)) {
        throw std::runtime_error(phase_tag + " failed to start UE container");
    }

    const std::string gnb_bin = resolve_bin_path(gnb_container, {"/ueransim/nr-gnb", "/UERANSIM/build/nr-gnb", "nr-gnb"});
    const std::string ue_bin = resolve_bin_path(ue_container, {"/ueransim/nr-ue", "/UERANSIM/build/nr-ue", "nr-ue"});
    if (gnb_bin.empty() || ue_bin.empty()) throw std::runtime_error(phase_tag + " failed to locate UERANSIM binaries");
    const std::string gnb_ip = container_ip_on_network(gnb_container, network);
    if (gnb_ip.empty()) throw std::runtime_error(phase_tag + " failed to resolve gNB IP");
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        job->gnb_ip = gnb_ip;
    }

    std::ostringstream gnb_yaml;
    gnb_yaml << "gnbId: 1\nmcc: '" << mcc << "'\nmnc: '" << mnc
             << "'\nnci: '0x000000010'\nidLength: 32\ntac: " << tac
             << "\nignoreStreamIds: true\nlinkIp: " << gnb_ip
             << "\nngapIp: " << gnb_ip << "\ngtpIp: " << gnb_ip
             << "\namfConfigs:\n  - address: " << amf_ip << "\n    port: 38412\nslices:\n  - sst: " << sst << "\n";
    if (!sd.empty()) gnb_yaml << "    sd: " << sd << "\n";
    write_file(dir / "gnb.yaml", gnb_yaml.str());

    std::ostringstream ue_yaml;
    ue_yaml << "supi: 'imsi-" << imsi << "'\nmcc: '" << mcc << "'\nmnc: '" << mnc
            << "'\nkey: '" << key << "'\nopType: 'OPC'\nop: '" << opc
            << "'\namf: '8000'\nimei: '356938035643803'\nimeiSv: '4370816125816151'\nintegrity:\n  IA1: true\n  IA2: true\n  IA3: true\nciphering:\n  EA1: true\n  EA2: true\n  EA3: true\nintegrityMaxRate:\n  uplink: full\n  downlink: full\nuacAic:\n  mps: false\n  mcs: false\nuacAcc:\n  normalClass: 0\n  class11: false\n  class12: false\n  class13: false\n  class14: false\n  class15: false\nconfigured-nssai:\n  - sst: " << sst << "\n";
    if (!sd.empty()) ue_yaml << "    sd: " << sd << "\n";
    ue_yaml << "default-nssai:\n  - sst: " << sst << "\n";
    if (!sd.empty()) ue_yaml << "    sd: " << sd << "\n";
    ue_yaml << "gnbSearchList:\n  - " << gnb_ip << "\nsessions:\n  - type: 'IPv4'\n    apn: '" << apn << "'\n    slice:\n      sst: " << sst << "\n";
    if (!sd.empty()) ue_yaml << "      sd: " << sd << "\n";
    write_file(dir / "ue1.yaml", ue_yaml.str());

    set_job_status(job, "running", pct(3), phase_tag + ": gNB NG Setup");
    std::string amf_probe_detail;
    if (wait_for_amf_ngap_probe(amf_container, 45, &amf_probe_detail)) {
        append_log(job, phase_tag + ": AMF NGAP/SCTP 就绪，开始 gNB 接入");
    } else {
        append_log(job, phase_tag + ": AMF NGAP/SCTP 探测未确认，进入 gNB 重试等待");
    }
    std::string gnb_log;
    if (!start_gnb_and_wait_ng_setup(job, phase_tag, gnb_container, gnb_bin, "/config/gnb.yaml", 6, 25, &gnb_log)) {
        throw std::runtime_error(phase_tag + " gNB NG Setup failed after retries: " + gnb_log);
    }

    set_job_status(job, "running", pct(4), phase_tag + ": UE 注册与 PDU Session");
    run_ok("docker exec -d " + shell_quote(ue_container) + " sh -lc " + shell_quote(shell_quote(ue_bin) + " -c /config/ue1.yaml > /tmp/ue.log 2>&1"));
    if (!wait_for_log(ue_container, "/tmp/ue.log", "Registration complete|Initial Registration is successful|5GMM-REGISTERED", 135)) {
        throw std::runtime_error(phase_tag + " UE registration failed: " + tail_file(ue_container, "/tmp/ue.log", 90));
    }
    if (!wait_for_log(ue_container, "/tmp/ue.log", "PDU Session establishment is successful|PDU Session Establishment Accept", 45)) {
        throw std::runtime_error(phase_tag + " PDU session failed: " + tail_file(ue_container, "/tmp/ue.log", 90));
    }
    const std::string ue_ip = wait_for_ue_tun_ip(ue_container, "uesimtun0", 45);
    if (ue_ip.empty()) throw std::runtime_error(phase_tag + " UE did not obtain IPv4 on uesimtun0");

    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        job->ues.front().registered = true;
        job->ues.front().pdu_session = true;
        job->ues.front().ip = ue_ip;
        job->ues.front().log_tail = tail_file(ue_container, "/tmp/ue.log", 120);
        job->ues.front().received_messages = tail_file(ue_container, "/tmp/sfc_ue_rx.log", 20);
    }
    append_log(job, phase_tag + ": 单 UE 注册/PDU/隧道地址验证通过，UE IP=" + ue_ip);
    set_step(job, step_key, "success", phase_tag + "通过，UE IP=" + ue_ip);
    set_job_status(job, "running", progress_end, phase_tag + ": 单 UE 功能验证通过");

    json result = {
        {"deployment", dep},
        {"runtime", deployment_runtime_summary(dep)},
        {"containers", runtime},
        {"ue_ip", ue_ip},
        {"amf_container", amf_container},
        {"amf_ip", amf_ip},
        {"gnb_ip", gnb_ip}
    };

    if (cleanup_after) {
        cleanup_job_ueransim_containers(job);
        append_log(job, phase_tag + ": 已清理本阶段 UERANSIM 容器");
    }
    return result;
}

void update_reschedule_report(const std::shared_ptr<ValidationJob>& job, const json& patch) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        job->reschedule_report[it.key()] = it.value();
    }
}

bool deployment_recovered_for_ueransim(const json& dep) {
    const std::string phase = dep.value("orchestration_phase", std::string(""));
    if (phase != "running") return false;
    if (!dep.value("service_ready", false) || !dep.value("ready_for_ueransim", false)) return false;
    return deployment_effectively_ready(dep, collect_deployment_runtime(dep));
}

void run_reschedule_validation_job(const std::shared_ptr<ValidationJob>& job) {
    constexpr int kWaitFaultTimeoutSec = 600;
    constexpr int kRecoveryTimeoutSec = 240;
    constexpr int kPollSec = 2;

    try {
        set_job_status(job, "running", 2, "准备重调度恢复验证");
        append_log(job, "开始重调度恢复验证：先执行单 UE 基线验证");

        auto dep_opt = find_deployment(job->deployment_id);
        if (!dep_opt) throw std::runtime_error("deployment_not_found");
        json baseline_dep = *dep_opt;
        const json baseline_runtime = collect_deployment_runtime(baseline_dep);
        if (!deployment_effectively_ready(baseline_dep, baseline_runtime)) {
            throw std::runtime_error("deployment is not service_ready && ready_for_ueransim");
        }
        baseline_dep["service_ready"] = true;
        baseline_dep["ready_for_ueransim"] = true;

        const std::string baseline_nf_sig = deployment_nf_signature(baseline_dep);
        const std::string baseline_nodes_sig = deployment_nodes_signature(baseline_dep);
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->deployment = baseline_dep;
            job->containers = baseline_runtime;
            job->reschedule_report = {
                {"baseline", deployment_runtime_summary(baseline_dep)},
                {"baseline_containers", baseline_runtime},
                {"wait_fault_timeout_sec", kWaitFaultTimeoutSec},
                {"recovery_timeout_sec", kRecoveryTimeoutSec},
                {"poll_sec", kPollSec},
                {"state", "baseline_verifying"}
            };
        }

        set_step(job, "baseline", "running", "对当前就绪核心网执行单 UE 注册/PDU 验证");
        const json baseline_verify = run_single_ue_smoke_phase(job, baseline_dep, "基线验证", "baseline", 5, 25, true);
        update_reschedule_report(job, {{"baseline_verify", baseline_verify}, {"state", "waiting_manual_fault"}});

        set_step(job, "wait_fault", "running", "请在故障控制页手动注入卫星/链路故障，系统会自动检测原核心网停止服务");
        set_job_status(job, "running", 32, "基线验证通过，请手动注入故障");
        append_log(job, "基线验证通过。请在故障控制页手动注入故障，等待窗口 " + std::to_string(kWaitFaultTimeoutSec) + "s");

        const int64_t wait_deadline_ms = now_ms() + static_cast<int64_t>(kWaitFaultTimeoutSec) * 1000;
        bool detected = false;
        int64_t recovery_start_ms = 0;
        json fault_dep = json::object();
        json fault_runtime = json::array();
        std::string detection_reason;

        while (now_ms() < wait_deadline_ms) {
            throw_if_stopped(job);
            auto current_opt = find_deployment(job->deployment_id);
            if (!current_opt) {
                std::this_thread::sleep_for(std::chrono::seconds(kPollSec));
                continue;
            }
            json current = *current_opt;
            const std::string phase = current.value("orchestration_phase", std::string(""));
            const bool phase_hint =
                phase == "stopping_old" || phase == "starting_containers" ||
                phase == "starting_core_nfs" || phase == "health_check" ||
                phase == "starting_nfs" || phase == "probing" || phase == "degraded";
            const bool signature_changed =
                deployment_nf_signature(current) != baseline_nf_sig ||
                deployment_nodes_signature(current) != baseline_nodes_sig;
            const json current_runtime = collect_deployment_runtime(current);
            const bool service_lost = !deployment_effectively_ready(current, current_runtime);

            {
                std::lock_guard<std::mutex> lock(g_jobs_mutex);
                job->deployment = current;
                job->containers = current_runtime;
                job->reschedule_report["latest"] = deployment_runtime_summary(current);
                job->reschedule_report["state"] = "waiting_manual_fault";
            }

            if (phase_hint || signature_changed || service_lost) {
                detected = true;
                recovery_start_ms = now_ms();
                fault_dep = current;
                fault_runtime = collect_deployment_runtime(current);
                if (service_lost) detection_reason = "service_not_ready";
                else if (phase_hint) detection_reason = "orchestration_phase_" + phase;
                else detection_reason = "placement_signature_changed";
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(kPollSec));
        }

        if (!detected) {
            throw std::runtime_error("no reschedule detected within wait window");
        }

        set_step(job, "wait_fault", "success", "已检测到故障/重调度信号: " + detection_reason);
        set_step(job, "detect", "running", "原核心网停止服务或部署签名变化，开始等待恢复");
        set_step(job, "recover", "running", "等待容器与核心网网元全部恢复运行");
        set_job_status(job, "running", 45, "已检测到重调度，等待新核心网恢复");
        append_log(job, "检测到重调度: " + detection_reason);
        update_reschedule_report(job, {
            {"state", "recovering"},
            {"detection_reason", detection_reason},
            {"recovery_start_at", iso_now()},
            {"fault_snapshot", deployment_runtime_summary(fault_dep)},
            {"fault_containers", fault_runtime}
        });

        const int64_t recovery_deadline_ms = now_ms() + static_cast<int64_t>(kRecoveryTimeoutSec) * 1000;
        json recovered_dep = json::object();
        while (now_ms() < recovery_deadline_ms) {
            throw_if_stopped(job);
            auto current_opt = find_deployment(job->deployment_id);
            if (!current_opt) {
                std::this_thread::sleep_for(std::chrono::seconds(kPollSec));
                continue;
            }
            json current = *current_opt;
            {
                std::lock_guard<std::mutex> lock(g_jobs_mutex);
                job->deployment = current;
                job->containers = collect_deployment_runtime(current);
                job->reschedule_report["latest"] = deployment_runtime_summary(current);
            }
            if (deployment_recovered_for_ueransim(current)) {
                recovered_dep = current;
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(kPollSec));
        }

        if (recovered_dep.empty()) {
            throw std::runtime_error("service did not recover within timeout");
        }

        set_step(job, "detect", "success", "新核心网已恢复 service_ready/ready_for_ueransim");
        set_step(job, "recover", "success", "容器与核心网网元全部恢复运行");
        set_step(job, "recovery_verify", "running", "对恢复后核心网执行单 UE 重新接入验证");
        set_job_status(job, "running", 76, "恢复态已观察到，执行 UE 重新接入验证");
        append_log(job, "恢复态已观察到，开始恢复后 UE 验证");

        const json recovery_verify = run_single_ue_smoke_phase(job, recovered_dep, "恢复验证", "recovery_verify", 78, 92, true);
        const int64_t recovery_done_ms = now_ms();
        const int64_t recovery_cost_ms = std::max<int64_t>(0, recovery_done_ms - recovery_start_ms);
        const json recovered_runtime = collect_deployment_runtime(recovered_dep);
        const json diff = build_reschedule_diff(
            baseline_dep,
            recovered_dep,
            fault_runtime,
            baseline_runtime,
            recovered_runtime
        );

        update_reschedule_report(job, {
            {"state", "success"},
            {"recovered", deployment_runtime_summary(recovered_dep)},
            {"recovered_containers", recovered_runtime},
            {"recovery_verify", recovery_verify},
            {"recovery_done_at", iso_now()},
            {"recovery_time_ms", recovery_cost_ms},
            {"diff", diff}
        });
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->deployment = recovered_dep;
            job->containers = recovered_runtime;
            job->data_plane_verified = true;
        }
        set_step(job, "report", "success", "恢复耗时 " + std::to_string(recovery_cost_ms) + " ms，重调度范围=" + diff.value("scope", std::string("unknown")));
        set_job_status(job, "success", 100, "重调度恢复验证通过，恢复耗时 " + std::to_string(recovery_cost_ms) + " ms");
        cleanup_job_ueransim_containers(job);
        append_log(job, "重调度验证流程已结束，已清理本次 UERANSIM gNB/UE 容器");
        append_log(job, "重调度恢复验证通过，recovery_time_ms=" + std::to_string(recovery_cost_ms));
    } catch (const std::exception& e) {
        cleanup_job_ueransim_containers(job);
        if (job_stop_requested(job) || std::string(e.what()) == "verification_stopped") {
            append_log(job, "重调度恢复验证已由用户停止");
            set_job_status(job, "stopped", job->progress, "已停止并清理 UERANSIM 容器");
            return;
        }
        spdlog::error("UERANSIM reschedule validation job {} failed: {}", job->job_id, e.what());
        append_log(job, std::string("重调度恢复验证失败: ") + e.what());
        set_step(job, "report", "failed", e.what());
        set_job_status(job, "failed", std::max(1, job->progress), e.what());
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        refresh_ue_logs_locked(*job);
    }
}

void run_validation_job(const std::shared_ptr<ValidationJob>& job) {
    try {
        if (job->mode == "reschedule") {
            run_reschedule_validation_job(job);
            return;
        }
        set_job_status(job, "running", 2, "准备验证任务");
        append_log(job, "开始 UERANSIM 功能验证");

        auto dep_opt = find_deployment(job->deployment_id);
        throw_if_stopped(job);
        if (!dep_opt) throw std::runtime_error("deployment_not_found");
        json dep = *dep_opt;
        const std::string dep_id = dep.value("deployment_id", dep.value("backend_deployment_id", std::string("")));
        json deployment_runtime = collect_deployment_runtime(dep);
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->deployment = dep;
            job->containers = deployment_runtime;
        }
        if (!deployment_effectively_ready(dep, deployment_runtime)) {
            throw std::runtime_error("deployment is not service_ready && ready_for_ueransim");
        }
        dep["service_ready"] = true;
        dep["ready_for_ueransim"] = true;
        set_step(job, "deployment", "success", "核心网部署状态满足 UE 验证条件");
        set_job_status(job, "running", 8, "核对核心网容器与 NF 进程");
        throw_if_stopped(job);

        int nf_total = 0;
        int nf_ok = 0;
        std::string amf_container;
        deployment_runtime = collect_deployment_runtime(dep);
        for (const auto& item : deployment_runtime) {
            ++nf_total;
            if (item.value("daemon_running", false)) ++nf_ok;
            if (item.value("nf_type", std::string("")) == "amf") amf_container = item.value("container", std::string(""));
        }
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->containers = deployment_runtime;
        }
        if (nf_total == 0 || nf_ok != nf_total) {
            throw std::runtime_error("NF process verification failed: " + std::to_string(nf_ok) + "/" + std::to_string(nf_total));
        }
        if (amf_container.empty()) amf_container = find_container_by_daemon("open5gs-amfd", deployment_container_prefixes(dep));
        if (amf_container.empty()) throw std::runtime_error("unable to locate AMF container");
        const std::string network = first_network_for_container(amf_container);
        const std::string amf_ip = container_ip_on_network(amf_container, network);
        if (network.empty() || amf_ip.empty()) throw std::runtime_error("unable to resolve AMF network/IP");
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->amf_container = amf_container;
            job->amf_ip = amf_ip;
            job->network = network;
        }
        set_step(job, "nf", "success", "NF 进程全部运行，AMF=" + amf_container + " " + amf_ip);
        append_log(job, "核心网容器核验通过: NF=" + std::to_string(nf_ok) + "/" + std::to_string(nf_total) + ", AMF=" + amf_container + ", network=" + network);
        throw_if_stopped(job);

        const std::string image = std::getenv("UERANSIM_IMAGE") ? std::getenv("UERANSIM_IMAGE") : "docker.io/free5gc/ueransim:latest";
        const std::string mcc = std::getenv("UERANSIM_MCC") ? std::getenv("UERANSIM_MCC") : "999";
        const std::string mnc = std::getenv("UERANSIM_MNC") ? std::getenv("UERANSIM_MNC") : "70";
        const std::string tac = std::getenv("UERANSIM_TAC") ? std::getenv("UERANSIM_TAC") : "1";
        const std::string sst = std::getenv("UERANSIM_SST") ? std::getenv("UERANSIM_SST") : "1";
        const std::string sd = normalize_sd(std::getenv("UERANSIM_SD") ? std::getenv("UERANSIM_SD") : "000001");
        const std::string apn = std::getenv("UERANSIM_APN") ? std::getenv("UERANSIM_APN") : "internet";
        const std::string key = std::getenv("UERANSIM_KEY") ? std::getenv("UERANSIM_KEY") : "465B5CE8B199B49FAA5F0A2EE238A6BC";
        const std::string opc = std::getenv("UERANSIM_OPC") ? std::getenv("UERANSIM_OPC") : "E8ED289DEBA952E4283B54E88E6183CA";
        const std::string mongo = std::getenv("SFC_OPEN5GS_MONGO_CONTAINER") ? std::getenv("SFC_OPEN5GS_MONGO_CONTAINER") : "sfc-open5gs-mongo";
        std::string imsi_base = digits_only(std::getenv("UERANSIM_IMSI") ? std::getenv("UERANSIM_IMSI") : "999700000000001");
        if (imsi_base.empty()) imsi_base = "999700000000001";
        const unsigned long long imsi0 = std::stoull(imsi_base);

        const std::string short_id = normalize_token(job->job_id).substr(0, 18);
        const std::string name_base = "sfc-ueransim-" + normalize_token(dep_id).substr(0, 28) + "-" + short_id;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / name_base;
        std::filesystem::create_directories(dir);
        write_ueransim_helpers(dir);
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->image = image;
            job->tmp_dir = dir.string();
            job->gnb_container = name_base + "-gnb";
            job->ues = {
                {"ue1", "UE-1", std::to_string(imsi0), name_base + "-ue1", "", false, false, false, "", ""},
                {"ue2", "UE-2", std::to_string(imsi0 + 1), name_base + "-ue2", "", false, false, false, "", ""}
            };
        }

        set_job_status(job, "running", 16, "写入订阅用户");
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            const std::string imsi = std::to_string(imsi0 + static_cast<unsigned long long>(i));
            if (!provision_subscriber(dir, mongo, imsi, sst, sd, apn, key, opc)) {
                throw std::runtime_error("failed to provision subscriber IMSI=" + imsi + " on " + mongo);
            }
        }
        set_step(job, "subscriber", "success", "已向 Mongo 写入 UE-1/UE-2 订阅数据");

        set_job_status(job, "running", 22, "准备 UERANSIM 镜像与容器");
        append_log(job, "检查 UERANSIM 镜像: " + image);
        throw_if_stopped(job);
        if (!run_ok("docker image inspect " + shell_quote(image) + " >/dev/null")) {
            if (!run_ok("docker pull " + shell_quote(image) + " >/dev/null")) {
                throw std::runtime_error("failed to pull UERANSIM image: " + image);
            }
        }
        const std::string mount = dir.string() + ":/config";
        std::string gnb_container;
        std::vector<UEStatus> ues;
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            gnb_container = job->gnb_container;
            ues = job->ues;
        }
        if (!start_ueransim_container(gnb_container, image, network, mount)) {
            throw std::runtime_error("failed to start gNB container with tun support");
        }
        for (const auto& ue : ues) {
            throw_if_stopped(job);
            if (!start_ueransim_container(ue.container, image, network, mount)) {
                throw std::runtime_error("failed to start " + ue.label + " container with tun support");
            }
        }
        set_step(job, "containers", "success", "gNB/UE-1/UE-2 容器已启动并具备 TUN 支持");

        const std::string gnb_bin = resolve_bin_path(gnb_container, {"/ueransim/nr-gnb", "/UERANSIM/build/nr-gnb", "nr-gnb"});
        const std::string ue_bin = resolve_bin_path(ues.front().container, {"/ueransim/nr-ue", "/UERANSIM/build/nr-ue", "nr-ue"});
        if (gnb_bin.empty() || ue_bin.empty()) throw std::runtime_error("failed to locate UERANSIM binaries");
        const std::string gnb_ip = container_ip_on_network(gnb_container, network);
        if (gnb_ip.empty()) throw std::runtime_error("failed to resolve gNB IP");
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->gnb_ip = gnb_ip;
        }
        append_log(job, "UERANSIM 容器就绪: gNB=" + gnb_container + " " + gnb_ip + ", bin=" + gnb_bin + "/" + ue_bin);

        std::ostringstream gnb_yaml;
        gnb_yaml << "gnbId: 1\nmcc: '" << mcc << "'\nmnc: '" << mnc
                 << "'\nnci: '0x000000010'\nidLength: 32\ntac: " << tac
                 << "\nignoreStreamIds: true\nlinkIp: " << gnb_ip
                 << "\nngapIp: " << gnb_ip << "\ngtpIp: " << gnb_ip
                 << "\namfConfigs:\n  - address: " << amf_ip << "\n    port: 38412\nslices:\n  - sst: " << sst << "\n";
        if (!sd.empty()) gnb_yaml << "    sd: " << sd << "\n";
        write_file(dir / "gnb.yaml", gnb_yaml.str());
        for (int i = 0; i < 2; ++i) {
            const std::string imsi = std::to_string(imsi0 + static_cast<unsigned long long>(i));
            std::ostringstream ue_yaml;
            ue_yaml << "supi: 'imsi-" << imsi << "'\nmcc: '" << mcc << "'\nmnc: '" << mnc
                    << "'\nkey: '" << key << "'\nopType: 'OPC'\nop: '" << opc
                    << "'\namf: '8000'\nimei: '35693803564380" << (3 + i)
                    << "'\nimeiSv: '437081612581615" << (1 + i)
                    << "'\nintegrity:\n  IA1: true\n  IA2: true\n  IA3: true\nciphering:\n  EA1: true\n  EA2: true\n  EA3: true\nintegrityMaxRate:\n  uplink: full\n  downlink: full\nuacAic:\n  mps: false\n  mcs: false\nuacAcc:\n  normalClass: 0\n  class11: false\n  class12: false\n  class13: false\n  class14: false\n  class15: false\nconfigured-nssai:\n  - sst: " << sst << "\n";
            if (!sd.empty()) ue_yaml << "    sd: " << sd << "\n";
            ue_yaml << "default-nssai:\n  - sst: " << sst << "\n";
            if (!sd.empty()) ue_yaml << "    sd: " << sd << "\n";
            ue_yaml << "gnbSearchList:\n  - " << gnb_ip << "\nsessions:\n  - type: 'IPv4'\n    apn: '" << apn << "'\n    slice:\n      sst: " << sst << "\n";
            if (!sd.empty()) ue_yaml << "      sd: " << sd << "\n";
            write_file(dir / ("ue" + std::to_string(i + 1) + ".yaml"), ue_yaml.str());
        }

        set_job_status(job, "running", 34, "启动 gNB 并等待 NG Setup");
        throw_if_stopped(job);
        std::string amf_probe_detail;
        if (wait_for_amf_ngap_probe(amf_container, 45, &amf_probe_detail)) {
            append_log(job, "AMF NGAP/SCTP 就绪，开始 gNB 接入");
        } else {
            append_log(job, "AMF NGAP/SCTP 探测未确认，进入 gNB 重试等待");
        }
        std::string gnb_log;
        if (!start_gnb_and_wait_ng_setup(job, "双 UE 验证", gnb_container, gnb_bin, "/config/gnb.yaml", 6, 25, &gnb_log)) {
            throw std::runtime_error("gNB NG Setup failed after retries: " + gnb_log);
        }
        set_step(job, "gnb", "success", "gNB 与 AMF 完成 NG Setup");

        set_job_status(job, "running", 48, "启动两个 UE 并等待注册");
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            run_ok("docker exec -d " + shell_quote(ues[i].container) + " sh -lc " + shell_quote(shell_quote(ue_bin) + " -c /config/ue" + std::to_string(i + 1) + ".yaml > /tmp/ue.log 2>&1"));
        }
        const std::string reg_pattern = "Registration complete|Initial Registration is successful|5GMM-REGISTERED";
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            if (!wait_for_log(ues[i].container, "/tmp/ue.log", reg_pattern, 135)) {
                throw std::runtime_error(ues[i].label + " registration failed: " + tail_file(ues[i].container, "/tmp/ue.log", 90));
            }
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->ues[i].registered = true;
            job->ues[i].log_tail = tail_file(ues[i].container, "/tmp/ue.log", 90);
        }
        set_step(job, "registration", "success", "UE-1/UE-2 注册成功");

        set_job_status(job, "running", 62, "检查 PDU session 与 UE 隧道地址");
        const std::string pdu_pattern = "PDU Session establishment is successful|PDU Session Establishment Accept";
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            if (!wait_for_log(ues[i].container, "/tmp/ue.log", pdu_pattern, 45)) {
                throw std::runtime_error(ues[i].label + " PDU session failed: " + tail_file(ues[i].container, "/tmp/ue.log", 90));
            }
            const std::string ip = wait_for_ue_tun_ip(ues[i].container, "uesimtun0", 45);
            if (ip.empty()) {
                throw std::runtime_error(ues[i].label + " did not obtain IPv4 on uesimtun0");
            }
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->ues[i].pdu_session = true;
            job->ues[i].ip = ip;
            job->ues[i].log_tail = tail_file(ues[i].container, "/tmp/ue.log", 90);
        }
        set_step(job, "pdu", "success", "两个 UE 均完成 PDU Session 建立并获得 uesimtun0 地址");

        set_job_status(job, "running", 78, "执行 UE 间业务面连通性验证");
        throw_if_stopped(job);
        std::vector<UEStatus> fresh_ues;
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            fresh_ues = job->ues;
        }
        const std::string ping12 = "ping -c 2 -W 2 -I uesimtun0 " + shell_quote(fresh_ues[1].ip);
        const std::string ping21 = "ping -c 2 -W 2 -I uesimtun0 " + shell_quote(fresh_ues[0].ip);
        std::string ping_out_12;
        std::string ping_out_21;
        const bool ok12 = run_ok("docker exec " + shell_quote(fresh_ues[0].container) + " sh -lc " + shell_quote(ping12), &ping_out_12);
        const bool ok21 = run_ok("docker exec " + shell_quote(fresh_ues[1].container) + " sh -lc " + shell_quote(ping21), &ping_out_21);
        if (!ok12 || !ok21) {
            throw std::runtime_error("UE-to-UE ping failed\nUE-1->UE-2:\n" + ping_out_12 + "\nUE-2->UE-1:\n" + ping_out_21);
        }
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->data_plane_verified = true;
        }
        set_step(job, "dataplane", "success", "UE-1 与 UE-2 通过核心网业务面互 ping 成功");

        set_job_status(job, "running", 88, "启动 UE 消息接收面板");
        for (int i = 0; i < 2; ++i) {
            throw_if_stopped(job);
            const bool receiver = start_receiver(fresh_ues[i].container);
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            job->ues[i].receiver_ready = receiver;
            job->ues[i].received_messages = tail_file(fresh_ues[i].container, "/tmp/sfc_ue_rx.log", 20);
        }
        set_step(job, "message", "running", "消息接收器已启动，可在页面中发送 UE 间验证消息");

        set_job_status(job, "success", 100, "功能验证完成：核心网、UERANSIM 注册/PDU/业务面/双 UE 通信均已通过");
        set_step(job, "message", "success", "UE 消息面板已就绪");
        append_log(job, "UERANSIM 功能验证完整通过");
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            refresh_ue_logs_locked(*job);
        }
    } catch (const std::exception& e) {
        if (job_stop_requested(job) || std::string(e.what()) == "verification_stopped") {
            append_log(job, "验证任务已由用户停止");
            set_job_status(job, "stopped", job->progress, "已停止并清理 UERANSIM 容器");
            return;
        }
        spdlog::error("UERANSIM validation job {} failed: {}", job->job_id, e.what());
        append_log(job, std::string("验证失败: ") + e.what());
        set_job_status(job, "failed", std::max(1, job->progress), e.what());
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        refresh_ue_logs_locked(*job);
    }
}

HttpResponsePtr json_response(const json& payload, HttpStatusCode status = k200OK) {
    auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(payload));
    resp->setStatusCode(status);
    return resp;
}

} // namespace

void UERANSIMController::listDeployments(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        const json deployments = list_deployment_records();
        json items = json::array();
        if (deployments.is_array()) {
            for (const auto& dep : deployments) {
                json item = dep;
                json runtime = collect_deployment_runtime(dep);
                const RuntimeHealth health = summarize_runtime_health(runtime);
                if (health.containers_total > 0) {
                    item["containers_total"] = health.containers_total;
                    item["containers_running"] = health.containers_running;
                }
                if (health.core_nfs_total > 0) {
                    item["core_nfs_total"] = health.core_nfs_total;
                    item["core_nfs_running"] = health.core_nfs_running;
                }
                const bool effectively_ready = deployment_effectively_ready(dep, runtime);
                if (effectively_ready) {
                    item["service_ready"] = true;
                    item["ready_for_ueransim"] = true;
                    item["reason"] = "";
                }
                item["eligible_for_ueransim"] = effectively_ready;
                item["runtime_containers"] = runtime;
                items.push_back(item);
            }
        }
        callback(json_response({{"items", items}}));
    } catch (const std::exception& e) {
        callback(json_response({{"message", e.what()}}, k500InternalServerError));
    }
}

void UERANSIMController::startVerification(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    const auto body = req->getJsonObject();
    if (!body || !body->isMember("deployment_id")) {
        callback(json_response({{"message", "deployment_id_required"}}, k400BadRequest));
        return;
    }
    const std::string deployment_id = (*body)["deployment_id"].asString();
    if (deployment_id.empty()) {
        callback(json_response({{"message", "deployment_id_required"}}, k400BadRequest));
        return;
    }

    auto job = std::make_shared<ValidationJob>();
    const auto seq = g_job_seq.fetch_add(1);
    job->job_id = "ueransim-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(seq);
    job->deployment_id = deployment_id;
    if (body->isMember("mode") && (*body)["mode"].isString()) {
        const std::string raw_mode = (*body)["mode"].asString();
        job->mode = raw_mode == "reschedule" ? "reschedule" : "smoke";
    } else if (body->isMember("flow") && (*body)["flow"].isString()) {
        const std::string raw_mode = (*body)["flow"].asString();
        job->mode = raw_mode == "reschedule" ? "reschedule" : "smoke";
    }
    job->started_at = iso_now();
    if (job->mode == "reschedule") {
        job->steps = {
            {"baseline", "基线单 UE 验证", "pending", ""},
            {"wait_fault", "等待人工故障", "pending", ""},
            {"detect", "检测重调度/停服", "pending", ""},
            {"recover", "等待新核心网恢复", "pending", ""},
            {"recovery_verify", "恢复后单 UE 验证", "pending", ""},
            {"report", "恢复报告", "pending", ""}
        };
    } else {
        job->steps = {
            {"deployment", "核心网就绪检查", "pending", ""},
            {"nf", "核心网容器与 NF 进程核验", "pending", ""},
            {"subscriber", "UE 订阅用户写入", "pending", ""},
            {"containers", "UERANSIM 容器启动", "pending", ""},
            {"gnb", "gNB NG Setup", "pending", ""},
            {"registration", "UE 注册", "pending", ""},
            {"pdu", "PDU Session 与 UE IP", "pending", ""},
            {"dataplane", "UE 间业务面验证", "pending", ""},
            {"message", "双 UE 消息面板", "pending", ""}
        };
    }
    json initial_job;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs[job->job_id] = job;
        initial_job = job_to_json(*job);
    }
    std::thread(run_validation_job, job).detach();
    callback(json_response({{"job", initial_job}}));
}

void UERANSIMController::getVerification(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& job_id
) {
    std::shared_ptr<ValidationJob> job;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) {
            callback(json_response({{"message", "job_not_found"}}, k404NotFound));
            return;
        }
        refresh_ue_logs_locked(*it->second);
        job = it->second;
        callback(json_response({{"job", job_to_json(*job)}}));
    }
}

void UERANSIMController::sendMessage(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& job_id
) {
    const auto body = req->getJsonObject();
    if (!body) {
        callback(json_response({{"message", "json_body_required"}}, k400BadRequest));
        return;
    }
    const std::string from_key = body->isMember("from") ? (*body)["from"].asString() : "ue1";
    const std::string to_key = body->isMember("to") ? (*body)["to"].asString() : (from_key == "ue1" ? "ue2" : "ue1");
    std::string message = body->isMember("message") ? (*body)["message"].asString() : "";
    message = trim_copy(message);
    if (message.empty()) {
        callback(json_response({{"message", "message_required"}}, k400BadRequest));
        return;
    }
    if (message.size() > 512) message.resize(512);

    std::shared_ptr<ValidationJob> job;
    UEStatus from;
    UEStatus to;
    bool found_from = false;
    bool found_to = false;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) {
            callback(json_response({{"message", "job_not_found"}}, k404NotFound));
            return;
        }
        job = it->second;
        if (job->status != "success") {
            callback(json_response({{"message", "verification_not_successful"}}, k409Conflict));
            return;
        }
        for (const auto& ue : job->ues) {
            if (ue.key == from_key) {
                from = ue;
                found_from = true;
            }
            if (ue.key == to_key) {
                to = ue;
                found_to = true;
            }
        }
    }
    if (!found_from || !found_to || from.ip.empty() || to.ip.empty()) {
        callback(json_response({{"message", "ue_not_ready"}}, k409Conflict));
        return;
    }

    const std::string wire_message = from.label + " -> " + to.label + ": " + message;
    const auto [udp_ok, udp_out] = send_udp_message(from.container, to.ip, wire_message);
    std::string transport = "udp_over_ue_tunnel";
    const std::string stamped_line = "[" + iso_now() + "] " + wire_message;
    if (udp_ok) {
        if (!wait_for_panel_message(to.container, wire_message)) {
            append_ue_message_panel(to.container, stamped_line + " (UDP sent over UE tunnel; receiver log not flushed in time)");
        }
    } else {
        transport = "fallback_log_after_dataplane_verified";
        append_ue_message_panel(to.container, stamped_line + " (UDP sender unavailable: " + trim_copy(udp_out) + ")");
    }
    append_ue_message_panel(from.container, stamped_line + " (sent)");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    append_log(job, "消息发送: " + wire_message + " transport=" + transport);
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        refresh_ue_logs_locked(*job);
        callback(json_response({{"job", job_to_json(*job)}, {"transport", transport}, {"udp_output", udp_out}}));
    }
}

void UERANSIMController::stopVerification(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& job_id
) {
    std::shared_ptr<ValidationJob> job;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) {
            callback(json_response({{"message", "job_not_found"}}, k404NotFound));
            return;
        }
        job = it->second;
        job->stop_requested = true;
    }
    std::vector<std::string> containers;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (!job->gnb_container.empty()) containers.push_back(job->gnb_container);
        for (const auto& ue : job->ues) {
            if (!ue.container.empty()) containers.push_back(ue.container);
        }
    }
    for (const auto& c : containers) {
        run_ok("docker rm -f " + shell_quote(c) + " >/dev/null");
    }
    set_job_status(job, "stopped", job->progress, "已停止并清理 UERANSIM 容器");
    append_log(job, "已停止验证任务并清理容器");
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    callback(json_response({{"job", job_to_json(*job)}}));
}

} // namespace sfc
