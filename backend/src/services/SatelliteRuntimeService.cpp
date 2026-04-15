#include "services/SatelliteRuntimeService.h"
#include "utils/CommandRunner.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <future>
#include <iomanip>
#include <sstream>
#include <thread>
#include <spdlog/spdlog.h>

namespace sfc {
namespace {

constexpr const char* kContainerPrefix = "sfc-sat-";

std::string trim_copy(const std::string& raw) {
    size_t l = 0;
    while (l < raw.size() && std::isspace(static_cast<unsigned char>(raw[l]))) ++l;
    size_t r = raw.size();
    while (r > l && std::isspace(static_cast<unsigned char>(raw[r - 1]))) --r;
    return raw.substr(l, r - l);
}

double clamp(double value, double lo, double hi) {
    return std::max(lo, std::min(hi, value));
}

template <typename Fn>
void run_parallel_indexed(size_t total, int parallelism, Fn&& fn) {
    if (total == 0) return;
    const int workers = std::max(1, std::min<int>(parallelism, static_cast<int>(total)));
    std::atomic<size_t> cursor{0};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        pool.emplace_back([&]() {
            while (true) {
                const size_t idx = cursor.fetch_add(1);
                if (idx >= total) break;
                fn(idx);
            }
        });
    }
    for (auto& t : pool) {
        if (t.joinable()) t.join();
    }
}

} // namespace

SatelliteRuntimeService::SatelliteRuntimeService()
    : rng_(std::random_device{}()),
      last_collect_wall_(std::chrono::steady_clock::now() - std::chrono::hours(1)) {
    init_templates();
}

void SatelliteRuntimeService::configure(const Config& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
}

SatelliteRuntimeService::Config SatelliteRuntimeService::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool SatelliteRuntimeService::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.enabled;
}

void SatelliteRuntimeService::init_templates() {
    templates_.clear();
    templates_["starlink_v1"] = TemplateProfile{
        "starlink_v1", "Starlink V1", 550.0, 53.0, {12.0, 24.0}, {24.0, 48.0}, {120.0, 360.0}, 0.01, 8
    };
    templates_["starlink_v2"] = TemplateProfile{
        "starlink_v2", "Starlink V2", 530.0, 43.0, {20.0, 32.0}, {48.0, 96.0}, {220.0, 560.0}, 0.012, 10
    };
    templates_["oneweb"] = TemplateProfile{
        "oneweb", "OneWeb", 1200.0, 87.9, {8.0, 16.0}, {16.0, 32.0}, {100.0, 220.0}, 0.01, 8
    };
    templates_["iridium"] = TemplateProfile{
        "iridium", "Iridium NEXT", 780.0, 86.4, {4.0, 10.0}, {8.0, 18.0}, {80.0, 180.0}, 0.008, 6
    };
    templates_["telesat"] = TemplateProfile{
        "telesat", "Telesat Lightspeed", 1015.0, 98.98, {12.0, 20.0}, {24.0, 48.0}, {140.0, 280.0}, 0.01, 8
    };
    templates_["kuiper"] = TemplateProfile{
        "kuiper", "Amazon Kuiper", 630.0, 51.9, {12.0, 24.0}, {24.0, 48.0}, {160.0, 300.0}, 0.01, 8
    };
    templates_["polar"] = TemplateProfile{
        "polar", "Polar Walker-Star", 600.0, 90.0, {8.0, 16.0}, {16.0, 32.0}, {110.0, 210.0}, 0.01, 8
    };
    templates_["qianfan"] = TemplateProfile{
        "qianfan", "Qianfan", 650.0, 53.0, {14.0, 26.0}, {28.0, 64.0}, {220.0, 420.0}, 0.01, 8
    };
}

const SatelliteRuntimeService::TemplateProfile& SatelliteRuntimeService::resolve_template(
    const std::string& template_id
) const {
    auto it = templates_.find(template_id);
    if (it != templates_.end()) return it->second;
    auto def = templates_.find("starlink_v1");
    if (def != templates_.end()) return def->second;
    return templates_.begin()->second;
}

std::string SatelliteRuntimeService::make_container_name(const std::string& satellite_id) const {
    std::string out = kContainerPrefix;
    out.reserve(out.size() + satellite_id.size());
    for (char ch : satellite_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
            out.push_back('-');
        }
    }
    return out;
}

std::string SatelliteRuntimeService::iso_now() {
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

std::vector<std::string> SatelliteRuntimeService::split_lines(const std::string& raw) {
    std::vector<std::string> out;
    std::stringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        const auto cleaned = trim_copy(line);
        if (!cleaned.empty()) out.push_back(cleaned);
    }
    return out;
}

std::vector<std::string> SatelliteRuntimeService::split_by(const std::string& raw, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, delim)) {
        out.push_back(token);
    }
    return out;
}

void SatelliteRuntimeService::apply_template_to_topology(
    Topology& topology,
    const std::string& template_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& tpl = resolve_template(template_id);
    std::uniform_real_distribution<double> cpu_dist(tpl.cpu_range.first, tpl.cpu_range.second);
    std::uniform_real_distribution<double> mem_dist(tpl.mem_range.first, tpl.mem_range.second);
    std::uniform_real_distribution<double> disk_dist(tpl.disk_range.first, tpl.disk_range.second);

    for (auto& sat : topology.nodes) {
        sat.template_id = tpl.id;
        sat.podman_container_name = make_container_name(sat.id);
        sat.cpu_total = cpu_dist(rng_);
        sat.mem_total = mem_dist(rng_);
        sat.disk_total = disk_dist(rng_);
        sat.cpu_available = sat.cpu_total;
        sat.mem_available = sat.mem_total;
        sat.disk_available = sat.disk_total;

        auto& rt = runtime_[sat.id];
        rt.container_name = sat.podman_container_name;
        rt.template_id = tpl.id;
        if (rt.status.empty()) rt.status = "not_created";
        rt.last_collected_at = iso_now();

        apply_runtime_state_to_satellite(&sat, rt);
    }
}

nlohmann::json SatelliteRuntimeService::ensure_image_available_locked() {
    if (!config_.enabled) {
        return {{"ok", true}, {"enabled", false}};
    }
    const std::string image_escaped = shell_escape_single_quotes(config_.default_image);
    const std::string exists_cmd = config_.podman_bin + " image exists '" + image_escaped + "'";
    const auto exists = run_command(exists_cmd);
    if (exists.exit_code == 0) {
        return {{"ok", true}, {"pulled", false}};
    }
    if (!config_.auto_pull_image) {
        return {
            {"ok", false},
            {"message", "Container image not found and auto_pull_image=false"},
            {"image", config_.default_image}
        };
    }
    spdlog::info("Podman image not found, pulling {}", config_.default_image);
    const std::string pull_cmd = config_.podman_bin + " pull '" + image_escaped + "'";
    const auto pull = run_command(pull_cmd);
    if (pull.exit_code != 0) {
        spdlog::warn("Failed to pull image {}: {}", config_.default_image, pull.output);
        return {
            {"ok", false},
            {"message", "Failed to pull container image"},
            {"image", config_.default_image},
            {"details", pull.output}
        };
    }
    return {{"ok", true}, {"pulled", true}};
}

nlohmann::json SatelliteRuntimeService::create_or_start_node(
    const std::string& sat_id,
    const TemplateProfile& tpl,
    bool recreate,
    bool ensure_running
) {
    Config cfg;
    std::string container_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = runtime_[sat_id];
        if (rt.container_name.empty()) rt.container_name = make_container_name(sat_id);
        rt.template_id = tpl.id;
        cfg = config_;
        container_name = rt.container_name;
        if (!cfg.enabled) {
            rt.status = "simulated";
            rt.last_collected_at = iso_now();
            return {{"ok", true}, {"sat_id", sat_id}, {"mode", "simulated"}};
        }
    }

    const std::string name = shell_escape_single_quotes(container_name);
    auto exists = run_command(cfg.podman_bin + " container exists '" + name + "'");
    if (exists.exit_code == 0 && recreate) {
        auto rm = run_command(cfg.podman_bin + " rm -f --ignore '" + name + "'");
        if (rm.exit_code != 0) {
            return {
                {"ok", false},
                {"sat_id", sat_id},
                {"message", "Failed to recreate existing container"},
                {"details", rm.output}
            };
        }
        exists = run_command(cfg.podman_bin + " container exists '" + name + "'");
    }

    if (exists.exit_code == 0) {
        if (!ensure_running) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& rt = runtime_[sat_id];
            rt.status = "created";
            rt.last_collected_at = iso_now();
            return {{"ok", true}, {"sat_id", sat_id}, {"action", "keep_created"}};
        }
        auto start = run_command(cfg.podman_bin + " start '" + name + "'");
        if (start.exit_code != 0) {
            return {
                {"ok", false},
                {"sat_id", sat_id},
                {"message", "Failed to start existing container"},
                {"details", start.output}
            };
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = runtime_[sat_id];
        rt.status = "running";
        rt.container_id = trim_copy(start.output);
        rt.last_collected_at = iso_now();
        return {{"ok", true}, {"sat_id", sat_id}, {"action", "start"}};
    }

    const double cpu_limit = std::max(0.001, tpl.container_cpu_limit > 0.0 ? tpl.container_cpu_limit : cfg.cpu_limit_per_sat);
    const int mem_limit_mb = std::max(2, tpl.container_mem_limit_mb > 0 ? tpl.container_mem_limit_mb : cfg.mem_limit_mb_per_sat);
    const int tmpfs_mb = std::max(1, cfg.tmpfs_limit_mb_per_sat);
    const int pids_limit = std::max(8, cfg.pids_limit_per_sat);

    std::ostringstream create_cmd;
    create_cmd << cfg.podman_bin
               << " create"
               << " --name '" << name << "'"
               << " --hostname '" << name << "'"
               << " --label sfc.satellite=true"
               << " --label sfc.satellite_id='" << shell_escape_single_quotes(sat_id) << "'"
               << " --label sfc.template='" << shell_escape_single_quotes(tpl.id) << "'"
               << " --cpus " << std::fixed << std::setprecision(3) << cpu_limit
               << " --memory " << mem_limit_mb << "m"
               << " --pids-limit " << pids_limit
               << " --network none"
               << " --read-only"
               << " --tmpfs /tmp:size=" << tmpfs_mb << "m,mode=1777"
               << " '" << shell_escape_single_quotes(cfg.default_image) << "'"
               << " sh -c 'sleep infinity'";

    const auto created = run_command(create_cmd.str());
    if (created.exit_code != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = runtime_[sat_id];
        rt.status = "create_failed";
        rt.last_collected_at = iso_now();
        return {
            {"ok", false},
            {"sat_id", sat_id},
            {"message", "Failed to create podman node"},
            {"details", created.output}
        };
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = runtime_[sat_id];
        rt.container_id = trim_copy(created.output);
        rt.status = "created";
        rt.last_collected_at = iso_now();
    }

    if (!ensure_running) {
        return {{"ok", true}, {"sat_id", sat_id}, {"action", "create_only"}};
    }

    auto start = run_command(cfg.podman_bin + " start '" + name + "'");
    if (start.exit_code != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = runtime_[sat_id];
        rt.status = "created";
        rt.last_collected_at = iso_now();
        return {
            {"ok", false},
            {"sat_id", sat_id},
            {"message", "Container created but failed to start"},
            {"details", start.output}
        };
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rt = runtime_[sat_id];
        rt.status = "running";
        rt.container_id = trim_copy(start.output);
        rt.last_collected_at = iso_now();
    }
    return {{"ok", true}, {"sat_id", sat_id}, {"action", "create_and_start"}};
}

nlohmann::json SatelliteRuntimeService::refresh_runtime_state_from_podman_locked() {
    if (!config_.enabled) {
        return {{"ok", true}, {"enabled", false}};
    }

    const std::string cmd = config_.podman_bin
        + " ps -a --format '{{.Names}}|{{.ID}}|{{.State}}' --filter label=sfc.satellite=true";
    const auto res = run_command(cmd);
    if (res.exit_code != 0) {
        return {
            {"ok", false},
            {"message", "Failed to query podman container state"},
            {"details", res.output}
        };
    }

    std::unordered_map<std::string, std::pair<std::string, std::string>> state_by_name;
    for (const auto& line : split_lines(res.output)) {
        const auto parts = split_by(line, '|');
        if (parts.size() < 3) continue;
        state_by_name[trim_copy(parts[0])] = {trim_copy(parts[1]), trim_copy(parts[2])};
    }

    for (auto& kv : runtime_) {
        auto& rt = kv.second;
        auto it = state_by_name.find(rt.container_name);
        if (it == state_by_name.end()) {
            if (rt.status != "not_created" && rt.status != "simulated") {
                rt.status = "missing";
            }
            continue;
        }
        rt.container_id = it->second.first;
        const std::string state = it->second.second;
        if (state == "running" || state == "up") rt.status = "running";
        else if (state == "exited") rt.status = "exited";
        else if (state == "created") rt.status = "created";
        else if (state == "paused") rt.status = "paused";
        else rt.status = state.empty() ? "unknown" : state;
    }

    return {{"ok", true}, {"containers", state_by_name.size()}};
}

void SatelliteRuntimeService::simulate_node_resource_usage_locked(Topology& topology, const std::string& ts) {
    for (auto& sat : topology.nodes) {
        auto& rt = runtime_[sat.id];
        if (rt.container_name.empty()) rt.container_name = make_container_name(sat.id);

        const std::size_t hv = std::hash<std::string>{}(
            sat.id + "|" + ts + "|" + std::to_string(sat.orbital_params.plane)
        );
        const double phase = static_cast<double>(hv % 8192) / 777.0;

        const bool node_down = (
            sat.status == "down" ||
            rt.status == "stopped" ||
            rt.status == "exited" ||
            rt.status == "missing"
        );
        if (node_down) {
            rt.cpu_utilization_ratio = 0.99;
            rt.mem_utilization_ratio = 0.99;
            rt.disk_utilization_ratio = 0.99;
            rt.last_collected_at = ts;
            continue;
        }

        // Simulated occupancy (not real pod usage): deterministic pseudo-random wave per node.
        const double cpu = clamp(0.28 + 0.33 * std::sin(phase) + 0.09 * std::cos(phase * 0.37), 0.06, 0.86);
        const double mem = clamp(0.31 + 0.29 * std::cos(phase * 0.83 + 0.3), 0.08, 0.88);
        const double disk = clamp(0.18 + 0.22 * std::sin(phase * 0.51 + 1.1), 0.05, 0.72);

        rt.cpu_utilization_ratio = cpu;
        rt.mem_utilization_ratio = mem;
        rt.disk_utilization_ratio = disk;
        rt.last_collected_at = ts;
    }
}

void SatelliteRuntimeService::apply_runtime_state_to_satellite(
    Satellite* sat,
    const NodeRuntimeState& rt
) const {
    if (!sat) return;
    sat->podman_container_name = rt.container_name;
    sat->podman_container_id = rt.container_id;
    sat->podman_status = rt.status;
    sat->fault_injected = rt.fault_injected;
    if (!rt.fault_tag.empty()) sat->fault_tag = rt.fault_tag;
    sat->core_nf_policy_applied = rt.core_nf_policy_applied;
    sat->core_nf_policy = rt.core_nf_policy;
    sat->deployment_state = rt.deployment_state;
    sat->deployment_detail = rt.deployment_detail;
    sat->cpu_utilization_ratio = clamp(rt.cpu_utilization_ratio, 0.0, 1.0);
    sat->mem_utilization_ratio = clamp(rt.mem_utilization_ratio, 0.0, 1.0);
    sat->disk_utilization_ratio = clamp(rt.disk_utilization_ratio, 0.0, 1.0);
    sat->last_collected_at = rt.last_collected_at;

    sat->cpu_available = clamp(sat->cpu_total * (1.0 - sat->cpu_utilization_ratio), 0.0, sat->cpu_total);
    sat->mem_available = clamp(sat->mem_total * (1.0 - sat->mem_utilization_ratio), 0.0, sat->mem_total);
    sat->disk_available = clamp(sat->disk_total * (1.0 - sat->disk_utilization_ratio), 0.0, sat->disk_total);

    if (rt.status == "running") {
        if (sat->fault_tag == "container_stopped") {
            sat->fault_tag.clear();
            sat->fault_injected = false;
            sat->status = "active";
        }
    } else if (rt.status == "exited" || rt.status == "stopped" || rt.status == "missing") {
        if (sat->fault_tag.empty()) {
            sat->fault_tag = "container_stopped";
            sat->fault_injected = true;
        }
        sat->status = "down";
    }
}

nlohmann::json SatelliteRuntimeService::provision_constellation(
    Topology& topology,
    const std::string& template_id,
    bool recreate_existing
) {
    apply_template_to_topology(topology, template_id);
    TemplateProfile tpl;
    Config cfg;
    const int requested = static_cast<int>(topology.nodes.size());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tpl = resolve_template(template_id);
        cfg = config_;
        progress_.active = true;
        progress_.phase = "provisioning";
        progress_.requested = requested;
        progress_.processed = 0;
        progress_.started = 0;
        progress_.failed = 0;
        progress_.concurrency = std::max(1, std::min(cfg.provision_parallelism, requested));
        progress_.started_at = iso_now();
        progress_.updated_at = progress_.started_at;
        progress_.recent_logs.clear();
    }

    nlohmann::json image_ready = {{"ok", true}, {"enabled", cfg.enabled}};
    if (cfg.enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready = ensure_image_available_locked();
    }

    const bool podman_ready = !cfg.enabled || image_ready.value("ok", false);
    if (cfg.enabled && !podman_ready) {
        spdlog::warn("Provisioning fallback to simulated runtime: {}", image_ready.value("message", ""));
    }

    std::vector<std::string> sat_ids;
    sat_ids.reserve(topology.nodes.size());
    for (const auto& sat : topology.nodes) sat_ids.push_back(sat.id);

    std::atomic<int> created_or_started{0};
    std::atomic<int> failed{0};
    std::atomic<int> processed{0};
    std::mutex failures_mutex;
    std::vector<std::string> failures;
    failures.reserve(16);

    const bool ensure_running = cfg.enabled && !cfg.lightweight_create_only;
    const int parallelism = std::max(1, std::min(cfg.provision_parallelism, requested));

    run_parallel_indexed(sat_ids.size(), parallelism, [&](size_t idx) {
        const std::string& sat_id = sat_ids[idx];
        nlohmann::json result;
        if (!podman_ready) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& rt = runtime_[sat_id];
                if (rt.container_name.empty()) rt.container_name = make_container_name(sat_id);
                rt.status = "simulated";
                rt.last_collected_at = iso_now();
            }
            result = {{"ok", true}, {"sat_id", sat_id}, {"mode", "simulated_fallback"}};
        } else {
            result = create_or_start_node(
                sat_id,
                tpl,
                recreate_existing || cfg.recreate_existing,
                ensure_running
            );
        }

        const bool ok = result.value("ok", false);
        if (ok) {
            created_or_started.fetch_add(1);
        } else {
            failed.fetch_add(1);
            std::lock_guard<std::mutex> f_lock(failures_mutex);
            if (failures.size() < 24) {
                failures.push_back(sat_id + ": " + result.value("message", std::string("unknown error")));
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        progress_.processed = processed.fetch_add(1) + 1;
        progress_.started = created_or_started.load();
        progress_.failed = failed.load();
        progress_.updated_at = iso_now();
        if (progress_.recent_logs.size() < 300) {
            std::ostringstream line;
            line << sat_id << " -> " << (ok ? (ensure_running ? "running" : "created") : "failed");
            append_progress_log_locked(line.str());
        }
    });

    nlohmann::json state_result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_result = refresh_runtime_state_from_podman_locked();
        const std::string collected_ts = iso_now();
        simulate_node_resource_usage_locked(topology, collected_ts);
        for (auto& sat : topology.nodes) {
            auto it = runtime_.find(sat.id);
            if (it != runtime_.end()) apply_runtime_state_to_satellite(&sat, it->second);
        }
        progress_.active = false;
        progress_.phase = "idle";
        progress_.updated_at = iso_now();
    }

    return {
        {"template_id", tpl.id},
        {"requested", requested},
        {"created_or_started", created_or_started.load()},
        {"failed", failed.load()},
        {"failures", failures},
        {"state_result", state_result},
        {"resource_mode", "simulated"},
        {"runtime", runtime_status()}
    };
}

nlohmann::json SatelliteRuntimeService::collect_node_telemetry(Topology& topology, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!force) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_collect_wall_).count();
        if (elapsed < std::max(1, config_.telemetry_interval_sec)) {
            return {
                {"ok", true},
                {"skipped", true},
                {"reason", "throttled"},
                {"elapsed_sec", elapsed}
            };
        }
    }
    last_collect_wall_ = now;

    const std::string ts = iso_now();
    auto state_result = refresh_runtime_state_from_podman_locked();
    simulate_node_resource_usage_locked(topology, ts);
    for (auto& sat : topology.nodes) {
        auto& rt = runtime_[sat.id];
        if (!config_.enabled) rt.status = "simulated";
        apply_runtime_state_to_satellite(&sat, rt);
    }

    return {
        {"ok", true},
        {"collected_at", ts},
        {"state_result", state_result},
        {"metric_result", {{"ok", true}, {"mode", "simulated_resource_occupancy"}}},
        {"runtime", runtime_status_locked()}
    };
}

nlohmann::json SatelliteRuntimeService::stop_node(const std::string& sat_id, bool remove_container) {
    Config cfg;
    std::string container_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runtime_.find(sat_id);
        if (it == runtime_.end()) {
            return {{"ok", false}, {"sat_id", sat_id}, {"message", "Unknown satellite id"}};
        }
        if (it->second.container_name.empty()) it->second.container_name = make_container_name(sat_id);
        container_name = it->second.container_name;
        cfg = config_;
        if (!cfg.enabled) {
            it->second.status = remove_container ? "not_created" : "stopped";
            it->second.last_collected_at = iso_now();
            return {{"ok", true}, {"sat_id", sat_id}, {"mode", "simulated_only"}};
        }
    }

    const std::string name = shell_escape_single_quotes(container_name);
    const auto stop_res = run_command(cfg.podman_bin + " stop --ignore '" + name + "'");
    if (stop_res.exit_code != 0) {
        return {
            {"ok", false},
            {"sat_id", sat_id},
            {"message", "Failed to stop container"},
            {"details", stop_res.output}
        };
    }
    if (remove_container) {
        const auto rm_res = run_command(cfg.podman_bin + " rm --ignore '" + name + "'");
        if (rm_res.exit_code != 0) {
            return {
                {"ok", false},
                {"sat_id", sat_id},
                {"message", "Failed to remove container"},
                {"details", rm_res.output}
            };
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtime_.find(sat_id);
    if (it != runtime_.end()) {
        it->second.status = remove_container ? "not_created" : "stopped";
        if (remove_container) it->second.container_id.clear();
        it->second.last_collected_at = iso_now();
    }
    return {{"ok", true}, {"sat_id", sat_id}, {"removed", remove_container}};
}

nlohmann::json SatelliteRuntimeService::stop_satellite_nodes(
    Topology& topology,
    const std::vector<std::string>& node_ids,
    bool remove_containers
) {
    Config cfg;
    std::vector<std::string> valid_ids;
    std::vector<std::string> container_names;
    std::vector<std::string> failures;
    failures.reserve(32);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
        for (const auto& node_id : node_ids) {
            auto it = runtime_.find(node_id);
            if (it == runtime_.end()) {
                if (failures.size() < 20) failures.push_back(node_id + ": unknown satellite id");
                continue;
            }
            if (it->second.container_name.empty()) it->second.container_name = make_container_name(node_id);
            valid_ids.push_back(node_id);
            if (!it->second.container_name.empty()) container_names.push_back(it->second.container_name);
        }
    }

    int command_failed = 0;
    if (cfg.enabled && !container_names.empty()) {
        constexpr size_t kChunk = 120;
        const size_t chunk_count = (container_names.size() + kChunk - 1) / kChunk;
        std::mutex f_lock;
        std::atomic<int> command_failures{0};

        auto run_chunked = [&](const char* action) {
            run_parallel_indexed(chunk_count, std::max(1, cfg.stop_parallelism), [&](size_t cidx) {
                const size_t begin = cidx * kChunk;
                const size_t end = std::min(container_names.size(), begin + kChunk);
                std::ostringstream cmd;
                cmd << cfg.podman_bin << " " << action << " --ignore";
                for (size_t i = begin; i < end; ++i) {
                    cmd << " '" << shell_escape_single_quotes(container_names[i]) << "'";
                }
                const auto res = run_command(cmd.str());
                if (res.exit_code != 0) {
                    command_failures.fetch_add(1);
                    std::lock_guard<std::mutex> lk(f_lock);
                    if (failures.size() < 20) failures.push_back(std::string(action) + " chunk failed: " + trim_copy(res.output));
                }
            });
        };

        run_chunked("stop");
        if (remove_containers) run_chunked("rm");
        command_failed = command_failures.load();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& node_id : valid_ids) {
            auto it = runtime_.find(node_id);
            if (it == runtime_.end()) continue;
            it->second.status = remove_containers ? "not_created" : "stopped";
            if (remove_containers) it->second.container_id.clear();
            it->second.last_collected_at = iso_now();
        }

        for (auto& sat : topology.nodes) {
            auto it = runtime_.find(sat.id);
            if (it == runtime_.end()) continue;
            apply_runtime_state_to_satellite(&sat, it->second);
        }
    }

    const int failed = static_cast<int>(failures.size()) + command_failed;
    return {
        {"ok", failed == 0},
        {"stopped", static_cast<int>(valid_ids.size())},
        {"failed", failed},
        {"failures", failures},
        {"runtime", runtime_status()}
    };
}

nlohmann::json SatelliteRuntimeService::start_satellite_nodes(
    Topology& topology,
    const std::vector<std::string>& node_ids,
    bool recreate_containers
) {
    if (node_ids.empty()) {
        return {{"ok", false}, {"message", "node_ids is empty"}};
    }

    Config cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = config_;
    }

    nlohmann::json image_ready = {{"ok", true}, {"enabled", cfg.enabled}};
    if (cfg.enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready = ensure_image_available_locked();
    }
    if (cfg.enabled && !image_ready.value("ok", false)) {
        return {
            {"ok", false},
            {"message", "container image unavailable"},
            {"details", image_ready}
        };
    }

    std::unordered_map<std::string, std::string> template_by_id;
    template_by_id.reserve(topology.nodes.size());
    for (const auto& sat : topology.nodes) {
        template_by_id[sat.id] = sat.template_id.empty() ? "starlink_v1" : sat.template_id;
    }

    std::atomic<int> started{0};
    std::atomic<int> failed{0};
    std::mutex fail_lock;
    std::vector<std::string> failures;
    failures.reserve(16);

    const int parallelism = std::max(1, std::min(cfg.start_parallelism, static_cast<int>(node_ids.size())));
    run_parallel_indexed(node_ids.size(), parallelism, [&](size_t idx) {
        const std::string& node_id = node_ids[idx];
        auto it = template_by_id.find(node_id);
        if (it == template_by_id.end()) {
            failed.fetch_add(1);
            std::lock_guard<std::mutex> lk(fail_lock);
            if (failures.size() < 20) failures.push_back(node_id + ": not found in topology");
            return;
        }
        const auto& tpl = resolve_template(it->second);
        auto result = create_or_start_node(node_id, tpl, recreate_containers, true);
        if (result.value("ok", false)) {
            started.fetch_add(1);
        } else {
            failed.fetch_add(1);
            std::lock_guard<std::mutex> lk(fail_lock);
            if (failures.size() < 20) failures.push_back(node_id + ": " + result.value("message", std::string("failed")));
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)refresh_runtime_state_from_podman_locked();
        const std::string ts = iso_now();
        simulate_node_resource_usage_locked(topology, ts);
        for (auto& sat : topology.nodes) {
            auto it = runtime_.find(sat.id);
            if (it == runtime_.end()) continue;
            apply_runtime_state_to_satellite(&sat, it->second);
        }
    }

    return {
        {"ok", failed.load() == 0},
        {"started", started.load()},
        {"failed", failed.load()},
        {"failures", failures},
        {"runtime", runtime_status()}
    };
}

nlohmann::json SatelliteRuntimeService::stop_all_satellite_nodes(
    Topology& topology,
    bool remove_containers
) {
    std::vector<std::string> ids;
    ids.reserve(topology.nodes.size());
    for (const auto& sat : topology.nodes) ids.push_back(sat.id);
    return stop_satellite_nodes(topology, ids, remove_containers);
}

void SatelliteRuntimeService::forget_nodes(const std::vector<std::string>& node_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& node_id : node_ids) {
        runtime_.erase(node_id);
    }
}

void SatelliteRuntimeService::mark_fault_state(
    Topology& topology,
    const std::string& node_id,
    bool injected,
    const std::string& fault_tag
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& rt = runtime_[node_id];
    if (rt.container_name.empty()) rt.container_name = make_container_name(node_id);
    rt.fault_injected = injected;
    rt.fault_tag = injected ? fault_tag : "";
    rt.last_collected_at = iso_now();

    for (auto& sat : topology.nodes) {
        if (sat.id != node_id) continue;
        sat.fault_injected = injected;
        if (injected) {
            sat.status = "down";
            sat.fault_tag = fault_tag;
        } else if (sat.fault_tag == fault_tag || sat.fault_tag == "container_stopped") {
            sat.status = "active";
            sat.fault_tag.clear();
        }
        apply_runtime_state_to_satellite(&sat, rt);
        break;
    }
}

void SatelliteRuntimeService::mark_deployment_policy(
    Topology& topology,
    const std::vector<std::string>& node_ids,
    const std::string& policy_name,
    bool applied,
    const std::string& detail
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string now = iso_now();
    for (const auto& node_id : node_ids) {
        auto& rt = runtime_[node_id];
        if (rt.container_name.empty()) rt.container_name = make_container_name(node_id);
        rt.core_nf_policy_applied = applied;
        rt.core_nf_policy = policy_name;
        rt.deployment_state = applied ? "deployed" : "rolled_back";
        rt.deployment_detail = detail;
        rt.last_collected_at = now;
    }
    for (auto& sat : topology.nodes) {
        auto it = runtime_.find(sat.id);
        if (it == runtime_.end()) continue;
        apply_runtime_state_to_satellite(&sat, it->second);
    }
}

void SatelliteRuntimeService::append_progress_log_locked(const std::string& line) {
    if (line.empty()) return;
    if (progress_.recent_logs.size() >= 300) {
        progress_.recent_logs.erase(progress_.recent_logs.begin());
    }
    progress_.recent_logs.push_back(line);
}

void SatelliteRuntimeService::update_progress_locked(
    const std::string& phase,
    int requested,
    int processed,
    int started,
    int failed,
    bool active
) {
    progress_.phase = phase;
    progress_.requested = std::max(0, requested);
    progress_.processed = std::max(0, processed);
    progress_.started = std::max(0, started);
    progress_.failed = std::max(0, failed);
    progress_.active = active;
    progress_.updated_at = iso_now();
}

nlohmann::json SatelliteRuntimeService::runtime_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_status_locked();
}

nlohmann::json SatelliteRuntimeService::runtime_status_locked() const {
    int running = 0;
    int stopped = 0;
    int created = 0;
    int simulated = 0;
    int failed = 0;
    for (const auto& kv : runtime_) {
        const auto& status = kv.second.status;
        if (status == "running") running += 1;
        else if (status == "created") created += 1;
        else if (status == "simulated" || status == "not_created") simulated += 1;
        else if (status == "stopped" || status == "exited") stopped += 1;
        else if (status == "create_failed" || status == "missing") failed += 1;
    }
    return {
        {"enabled", config_.enabled},
        {"total_nodes", runtime_.size()},
        {"running", running},
        {"stopped", stopped},
        {"created", created},
        {"simulated", simulated},
        {"failed", failed},
        {"podman_bin", config_.podman_bin},
        {"image", config_.default_image},
        {"provisioning", {
            {"active", progress_.active},
            {"phase", progress_.phase},
            {"requested", progress_.requested},
            {"processed", progress_.processed},
            {"started", progress_.started},
            {"failed", progress_.failed},
            {"concurrency", progress_.concurrency},
            {"started_at", progress_.started_at},
            {"updated_at", progress_.updated_at},
            {"recent_logs", progress_.recent_logs}
        }}
    };
}

} // namespace sfc
