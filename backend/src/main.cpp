#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "controllers/TopologyController.h"
#include "controllers/SFCController.h"
#include "controllers/UERANSIMController.h"
#include "controllers/AuthController.h"
#include "controllers/UserController.h"
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"
#include "services/InferenceEngine.h"
#include "services/DynamicSimulationService.h"
#include "services/DynamicInferenceService.h"
#include "services/AuthGlobals.h"
#include "services/AuthService.h"
#include "services/UserService.h"
#include "services/RuntimeStateService.h"
#include "services/DeploymentOrchestratorService.h"
#include "services/DeploymentStateStore.h"
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <limits>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

using namespace drogon;
using json = nlohmann::json;

// 全局服务实例定义
namespace sfc {
    std::shared_ptr<TopologyManager> g_topo_mgr;
    std::shared_ptr<ResourceManager> g_res_mgr;
    std::shared_ptr<InferenceEngine> g_inference_engine;
    std::shared_ptr<DynamicSimulationService> g_dynamic_sim;
    std::shared_ptr<DynamicInferenceService> g_dynamic_inference;
    std::shared_ptr<AuthService> g_auth_service;
    std::shared_ptr<UserService> g_user_service;
    std::shared_ptr<RuntimeStateService> g_runtime_state_service;
    std::shared_ptr<DeploymentOrchestratorService> g_deployment_orchestrator;
}

struct Config {
    struct {
        std::string host = "0.0.0.0";
        int port = 8080;
        int threads = 4;
    } server;
    
    struct {
        std::string gnn_model = "../models/exported/gnn_encoder.onnx";
        std::string actor_model = "../models/exported/actor.onnx";
        int num_threads = 2;
    } onnx;
    
    struct {
        std::string level = "info";
    } logging;

    struct {
        std::string container_name = "sfc-mysql";
        std::string name = "sfc_runtime";
        std::string user = "sfc";
        std::string password = "sfc123456";
    } database;

    struct {
        std::string jwt_secret = "sfc-default-jwt-secret-change-this";
        int token_expire_hours = 24;
    } auth;
};

Config load_config(const std::string& config_file) {
    Config config;
    
    try {
        std::ifstream ifs(config_file);
        if (ifs.is_open()) {
            json j;
            ifs >> j;
            
            if (j.contains("server")) {
                auto& s = j["server"];
                config.server.host = s.value("host", config.server.host);
                config.server.port = s.value("port", config.server.port);
                config.server.threads = s.value("threads", config.server.threads);
            }
            
            if (j.contains("onnx")) {
                auto& o = j["onnx"];
                config.onnx.gnn_model = o.value("gnn_model", config.onnx.gnn_model);
                config.onnx.actor_model = o.value("actor_model", config.onnx.actor_model);
                config.onnx.num_threads = o.value("num_threads", config.onnx.num_threads);
            }
            
            if (j.contains("logging")) {
                config.logging.level = j["logging"].value("level", config.logging.level);
            }

            if (j.contains("database")) {
                auto& d = j["database"];
                config.database.container_name =
                    d.value("container_name", d.value("host", config.database.container_name));
                config.database.name =
                    d.value("name", d.value("database", config.database.name));
                config.database.user = d.value("user", config.database.user);
                config.database.password = d.value("password", config.database.password);
            }

            if (j.contains("auth")) {
                auto& a = j["auth"];
                config.auth.jwt_secret = a.value("jwt_secret", config.auth.jwt_secret);
                config.auth.token_expire_hours =
                    a.value("token_expire_hours", config.auth.token_expire_hours);
            }
            
            spdlog::info("Config loaded from {}", config_file);
        } else {
            spdlog::warn("Config file {} not found, using defaults", config_file);
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config: {}, using defaults", e.what());
    }
    
    return config;
}

std::string trim_copy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string extract_bearer_token(const HttpRequestPtr& req) {
    const auto auth = req->getHeader("Authorization");
    constexpr const char* kPrefix = "Bearer ";
    if (auth.size() <= 7 || auth.rfind(kPrefix, 0) != 0) return {};
    return trim_copy(auth.substr(7));
}

bool is_public_api_path(const HttpRequestPtr& req) {
    const std::string path = req->path();
    const auto method = req->method();
    if (path == "/api/v1/health" ||
        path == "/api/v1/auth/login" ||
        path == "/api/v1/auth/register") {
        return true;
    }

    if (method != Get) {
        return false;
    }

    if (path == "/api/v1/topology" ||
        path == "/api/v1/satellites" ||
        path == "/api/v1/topology/dynamic/status" ||
        path == "/api/v1/deployments" ||
        path == "/api/v1/runtime/events" ||
        path == "/api/v1/runtime/config") {
        return true;
    }
    if (starts_with(path, "/api/v1/satellite/")) {
        return true;
    }
    return false;
}

bool is_admin_only_path(const HttpRequestPtr& req) {
    const std::string path = req->path();
    const auto method = req->method();
    if (starts_with(path, "/api/v1/users")) return true;
    if (starts_with(path, "/api/v1/sfc")) return true;
    if (starts_with(path, "/api/v1/ueransim")) return true;
    if (path == "/api/v1/topology/generate") return true;
    if (starts_with(path, "/api/v1/topology/dynamic/") && path != "/api/v1/topology/dynamic/status") {
        return true;
    }
    if (path == "/api/v1/runtime/config" && method != Get) return true;
    if (starts_with(path, "/api/v1/satellite/") && method == Delete) return true;
    return false;
}

HttpResponsePtr build_auth_error(HttpStatusCode status, const std::string& message) {
    Json::Value err;
    err["code"] = static_cast<int>(status);
    err["message"] = message;
    auto resp = HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(status);
    return resp;
}

std::filesystem::path find_config_file() {
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("config.json"),
        std::filesystem::path("../config.json"),
        std::filesystem::path("backend/config.json"),
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && !ec) {
            return std::filesystem::absolute(c).lexically_normal();
        }
    }
    return std::filesystem::absolute(std::filesystem::path("config.json")).lexically_normal();
}

std::string resolve_with_base(const std::filesystem::path& base_dir, const std::string& raw_path) {
    if (raw_path.empty()) return raw_path;
    std::filesystem::path p(raw_path);
    if (p.is_absolute()) {
        return p.lexically_normal().string();
    }
    return (base_dir / p).lexically_normal().string();
}

std::string lower_ascii_copy(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool is_active_deployment_status(const std::string& raw_status) {
    const std::string status = lower_ascii_copy(raw_status);
    return !(status == "failed" || status == "rolled_back" || status == "rollbacked" || status == "deleted");
}

sfc::DeploymentCandidate candidate_from_deployment_record(const nlohmann::json& dep) {
    sfc::DeploymentCandidate candidate{};
    candidate.score = dep.value("score", 0.0);
    candidate.total_latency_ms = dep.value("total_latency_ms", 0.0);
    candidate.registration_latency_ms = dep.value("registration_latency_ms", 0.0);
    candidate.pdu_session_latency_ms = dep.value("pdu_session_latency_ms", 0.0);
    candidate.estimated_reliability = dep.value("estimated_reliability", 0.0);
    candidate.bottleneck_bandwidth_gbps = dep.value("bottleneck_bandwidth_gbps", 0.0);
    candidate.satisfies_constraints = dep.value("satisfies_constraints", true);
    candidate.reason = dep.value("reason", std::string(""));

    if (dep.contains("deployed_nodes") && dep["deployed_nodes"].is_array()) {
        for (const auto& node : dep["deployed_nodes"]) {
            if (node.is_string()) {
                const auto id = node.get<std::string>();
                if (!id.empty()) candidate.deployed_nodes.push_back(id);
            }
        }
    }

    const auto per = dep.contains("per_vnf") && dep["per_vnf"].is_array()
        ? dep["per_vnf"]
        : (dep.contains("per_core_nf") && dep["per_core_nf"].is_array() ? dep["per_core_nf"] : nlohmann::json::array());
    std::unordered_set<std::string> node_set(candidate.deployed_nodes.begin(), candidate.deployed_nodes.end());
    for (const auto& item : per) {
        if (!item.is_object()) continue;
        sfc::DeploymentCandidate::PerVNF pv{};
        pv.vnf = item.value("vnf", std::string(""));
        pv.core_nf = item.value("core_nf", pv.vnf);
        pv.nf_type = item.value("nf_type", pv.core_nf.empty() ? pv.vnf : pv.core_nf);
        pv.nf_role = item.value("nf_role", std::string(""));
        pv.node = item.value("node", std::string(""));
        pv.cpu_used = item.value("cpu_used", 0.0);
        pv.mem_used = item.value("mem_used", 0.0);
        pv.disk_used = item.value("disk_used", 0.0);
        if (pv.node.empty()) continue;
        candidate.per_vnf.push_back(pv);
        node_set.insert(pv.node);
    }
    if (candidate.deployed_nodes.empty() && !node_set.empty()) {
        candidate.deployed_nodes.assign(node_set.begin(), node_set.end());
        std::sort(candidate.deployed_nodes.begin(), candidate.deployed_nodes.end());
    }

    const auto links = dep.contains("link_details") && dep["link_details"].is_array()
        ? dep["link_details"] : nlohmann::json::array();
    for (const auto& item : links) {
        if (!item.is_object()) continue;
        sfc::DeploymentCandidate::LinkDetail ld{};
        ld.src = item.value("src", std::string(""));
        ld.dst = item.value("dst", std::string(""));
        ld.dependency_source_nf = item.value("dependency_source_nf", std::string(""));
        ld.dependency_target_nf = item.value("dependency_target_nf", std::string(""));
        ld.latency_ms = item.value("latency_ms", 0.0);
        ld.bandwidth_gbps = item.value("bandwidth_gbps", 0.0);
        ld.bandwidth_available_gbps = item.value("bandwidth_available_gbps", ld.bandwidth_gbps);
        ld.bandwidth_required_gbps = item.value("bandwidth_required_gbps", 0.0);
        ld.status = item.value("status", std::string("active"));
        ld.reliability = item.value("reliability", 0.999);
        if (!ld.src.empty() && !ld.dst.empty()) {
            candidate.link_details.push_back(ld);
        }
    }
    return candidate;
}

sfc::SFCRequest request_from_deployment_record(const nlohmann::json& dep, const sfc::DeploymentCandidate& candidate) {
    sfc::SFCRequest request{};
    request.request_id = dep.value("request_id", dep.value("deployment_id", std::string("")));
    request.service_type = dep.value("service_type", std::string("open5gs_core"));
    request.network_domain = dep.value("network_domain", std::string("open5gs"));
    request.custom_core_graph = dep.value("custom_core_graph", false);
    request.allow_partial_core_nfs = dep.value("allow_partial_core_nfs", request.custom_core_graph);
    request.optimize = dep.value("optimize", std::string("latency"));
    request.topk = 1;
    request.realtime_mode = true;
    request.max_planning_attempts = 20;
    request.planning_time_budget_ms = 450.0;
    request.source_node = dep.value("source_node", std::string(""));
    request.destination_node = dep.value("destination_node", std::string(""));
    request.constraints.max_latency_ms = 150.0;
    request.constraints.registration_latency_ms = 120.0;
    request.constraints.registration_access_latency_ms = 8.0;
    request.constraints.pdu_session_latency_ms = 100.0;
    request.constraints.pdu_access_latency_ms = 10.0;
    request.constraints.min_bandwidth_gbps = 0.5;
    request.constraints.min_reliability = 0.95;

    if (dep.contains("score_constraints") && dep["score_constraints"].is_object()) {
        const auto& c = dep["score_constraints"];
        request.constraints.max_latency_ms = c.value("max_latency_ms", request.constraints.max_latency_ms);
        request.constraints.registration_latency_ms = c.value("registration_latency_ms", request.constraints.registration_latency_ms);
        request.constraints.registration_access_latency_ms = c.value("registration_access_latency_ms", request.constraints.registration_access_latency_ms);
        request.constraints.pdu_session_latency_ms = c.value("pdu_session_latency_ms", request.constraints.pdu_session_latency_ms);
        request.constraints.pdu_access_latency_ms = c.value("pdu_access_latency_ms", request.constraints.pdu_access_latency_ms);
        request.constraints.min_bandwidth_gbps = c.value("min_bandwidth_gbps", request.constraints.min_bandwidth_gbps);
        request.constraints.min_reliability = c.value("min_reliability", request.constraints.min_reliability);
    }

    request.vnfs.reserve(candidate.per_vnf.size());
    for (const auto& pv : candidate.per_vnf) {
        sfc::VNF vnf{};
        vnf.name = pv.core_nf.empty() ? pv.vnf : pv.core_nf;
        if (vnf.name.empty()) vnf.name = pv.nf_type;
        vnf.nf_type = pv.nf_type.empty() ? vnf.name : pv.nf_type;
        vnf.nf_role = pv.nf_role.empty() ? "control_plane" : pv.nf_role;
        vnf.resource_profile = "standard";
        vnf.processing_weight = 1.0;
        vnf.stateful = true;
        vnf.cpu = std::max(0.0, pv.cpu_used);
        vnf.mem = std::max(0.0, pv.mem_used);
        vnf.disk = std::max(0.0, pv.disk_used);
        if (vnf.disk <= 1e-9 && vnf.mem > 1e-9) vnf.disk = vnf.mem * 2.0;
        vnf.bw_in = request.constraints.min_bandwidth_gbps;
        vnf.bw_out = request.constraints.min_bandwidth_gbps;
        request.vnfs.push_back(vnf);
    }

    if (dep.contains("core_nf_dependencies") && dep["core_nf_dependencies"].is_array()) {
        for (const auto& item : dep["core_nf_dependencies"]) {
            if (!item.is_object()) continue;
            sfc::CoreNFDependency edge{};
            edge.source = item.value("source", item.value("src", std::string("")));
            edge.target = item.value("target", item.value("dst", std::string("")));
            edge.criticality = item.value("criticality", 1.0);
            edge.bandwidth_scale = item.value("bandwidth_scale", 0.5);
            edge.latency_weight = item.value("latency_weight", 1.0);
            edge.reliability_weight = item.value("reliability_weight", 1.0);
            edge.bandwidth_required_gbps = item.value("bandwidth_required_gbps", 0.0);
            if (!edge.source.empty() && !edge.target.empty()) {
                request.core_nf_dependencies.push_back(edge);
            }
        }
    }
    if (dep.contains("independent_core_nfs") && dep["independent_core_nfs"].is_array()) {
        std::unordered_set<std::string> seen;
        for (const auto& item : dep["independent_core_nfs"]) {
            if (!item.is_string()) continue;
            std::string token = lower_ascii_copy(item.get<std::string>());
            token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
                return std::isspace(ch);
            }), token.end());
            if (!token.empty() && seen.insert(token).second) {
                request.independent_core_nfs.push_back(token);
            }
        }
    }
    return request;
}

int restore_active_deployments_after_boot() {
    if (!sfc::g_deployment_orchestrator) return 0;
    int queued = 0;
    const nlohmann::json deployments = sfc::list_deployment_records();
    if (!deployments.is_array()) return 0;

    for (const auto& dep : deployments) {
        if (!dep.is_object()) continue;
        if (!is_active_deployment_status(dep.value("status", std::string("completed")))) continue;
        const bool runtime_enabled = dep.value("runtime_enabled", false);

        const std::string deployment_id = dep.value(
            "backend_deployment_id",
            dep.value("deployment_id", std::string(""))
        );
        if (deployment_id.empty()) continue;

        const auto candidate = candidate_from_deployment_record(dep);
        if (candidate.deployed_nodes.empty() || candidate.per_vnf.empty()) continue;
        const auto request = request_from_deployment_record(dep, candidate);
        if (sfc::g_res_mgr) {
            sfc::g_res_mgr->restore_allocation_snapshot(deployment_id, candidate, request.vnfs);
        }

        const std::string request_id = dep.value(
            "request_id",
            dep.value("sfc_name", dep.value("name", deployment_id))
        );
        if (runtime_enabled) {
            sfc::g_deployment_orchestrator->enqueue_deployment(
                deployment_id,
                request_id,
                candidate,
                {},
                "bootstrap_restore",
                "system_restart_restore"
            );
        }
        if (sfc::g_dynamic_inference && !request.vnfs.empty()) {
            (void)sfc::g_dynamic_inference->start_session(
                request,
                dep.value("auto_redeploy", true),
                &candidate,
                deployment_id,
                dep.value("inference_latency_ms", 0.0)
            );
        }
        queued += 1;
    }
    return queued;
}

void persist_runtime_snapshot_on_shutdown(const std::string& reason) {
    if (!sfc::g_runtime_state_service) return;
    if (!sfc::g_res_mgr || !sfc::g_topo_mgr) return;

    sfc::Topology topo = sfc::g_res_mgr->export_current_topology();
    if (topo.nodes.empty()) {
        topo = sfc::g_topo_mgr->get_current_topology();
    }
    if (topo.nodes.empty()) {
        spdlog::warn("Skip runtime snapshot persistence on shutdown: topology is empty (reason={})", reason);
        return;
    }
    const bool ok = sfc::g_runtime_state_service->save_topology(topo, "");
    spdlog::info(
        "Shutdown snapshot persisted (reason={}): success={} nodes={} links={}",
        reason,
        ok ? "true" : "false",
        topo.nodes.size(),
        topo.links.size()
    );
}

bool process_exists(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

bool acquire_instance_lock_file(const std::string& lock_file, std::string* err) {
    constexpr int kMaxAttempts = 2;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            const std::string pid_line = std::to_string(::getpid()) + "\n";
            (void)::write(fd, pid_line.data(), pid_line.size());
            (void)::close(fd);
            return true;
        }

        if (errno != EEXIST) {
            if (err) *err = "create_lock_file_failed";
            return false;
        }

        std::ifstream ifs(lock_file);
        pid_t existing_pid = -1;
        if (ifs.is_open()) {
            long long raw = -1;
            ifs >> raw;
            if (raw > 0 && raw <= std::numeric_limits<pid_t>::max()) {
                existing_pid = static_cast<pid_t>(raw);
            }
        }
        if (process_exists(existing_pid) && existing_pid != ::getpid()) {
            if (err) *err = "lock_held_by_pid_" + std::to_string(existing_pid);
            return false;
        }

        // Stale lock file; remove and retry.
        (void)::unlink(lock_file.c_str());
    }

    if (err) *err = "lock_retry_exhausted";
    return false;
}

void release_instance_lock_file(const std::string& lock_file) {
    std::ifstream ifs(lock_file);
    pid_t existing_pid = -1;
    if (ifs.is_open()) {
        long long raw = -1;
        ifs >> raw;
        if (raw > 0 && raw <= std::numeric_limits<pid_t>::max()) {
            existing_pid = static_cast<pid_t>(raw);
        }
    }
    if (existing_pid == ::getpid() || !process_exists(existing_pid)) {
        (void)::unlink(lock_file.c_str());
    }
}

int main() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    
    const std::filesystem::path config_path = find_config_file();
    Config config = load_config(config_path.string());
    if (const char* env_secret = std::getenv("SFC_JWT_SECRET"); env_secret && *env_secret) {
        config.auth.jwt_secret = env_secret;
    }
    const std::filesystem::path config_dir = config_path.parent_path();
    const std::string upload_tmp_path =
        (std::filesystem::temp_directory_path() / "sfc_drogon_upload").lexically_normal().string();
    config.onnx.gnn_model = resolve_with_base(config_dir, config.onnx.gnn_model);
    config.onnx.actor_model = resolve_with_base(config_dir, config.onnx.actor_model);
    
    if (config.logging.level == "debug") {
        spdlog::set_level(spdlog::level::debug);
    } else if (config.logging.level == "warn") {
        spdlog::set_level(spdlog::level::warn);
    } else {
        spdlog::set_level(spdlog::level::info);
    }
    
    spdlog::info("==============================================");
    spdlog::info("  SFC Visualization Backend Starting...");
    spdlog::info("==============================================");

    std::string instance_lock_path;
    bool instance_lock_acquired = false;
    try {
        instance_lock_path = []() {
            if (const char* v = std::getenv("SFC_BACKEND_INSTANCE_LOCK_FILE"); v && *v) {
                return std::string(v);
            }
            return std::string("/tmp/sfc_runtime_backend.lock");
        }();
        std::string lock_err;
        instance_lock_acquired = acquire_instance_lock_file(instance_lock_path, &lock_err);
        if (!instance_lock_acquired) {
            throw std::runtime_error(
                "Another backend instance is already running (lock file: " + instance_lock_path + "). "
                "Please stop the old process before starting a new one."
            );
        }

        // 创建全局服务实例
        sfc::g_topo_mgr = std::make_shared<sfc::TopologyManager>();
        sfc::g_res_mgr = std::make_shared<sfc::ResourceManager>();
        sfc::g_inference_engine = std::make_shared<sfc::InferenceEngine>(
            config.onnx.gnn_model,
            config.onnx.actor_model,
            config.onnx.num_threads
        );
        sfc::g_dynamic_sim = std::make_shared<sfc::DynamicSimulationService>(
            sfc::g_topo_mgr,
            sfc::g_res_mgr
        );
        sfc::g_dynamic_inference = std::make_shared<sfc::DynamicInferenceService>(
            sfc::g_inference_engine,
            sfc::g_res_mgr,
            sfc::g_topo_mgr,
            sfc::g_dynamic_sim
        );
        sfc::g_auth_service = std::make_shared<sfc::AuthService>(
            config.auth.jwt_secret,
            config.auth.token_expire_hours
        );
        sfc::g_dynamic_sim->register_snapshot_listener(
            "dynamic_orchestrator",
            [service = sfc::g_dynamic_inference](const sfc::TopologySnapshot& snapshot) {
                if (service) service->on_topology_tick(snapshot);
            }
        );
        
        // 配置Drogon
        app()
            .setLogLevel(trantor::Logger::kInfo)
            .addListener(config.server.host, config.server.port)
            .setThreadNum(config.server.threads)
            .setClientMaxBodySize(100 * 1024 * 1024)
            .setMaxConnectionNum(10000)
            .setIdleConnectionTimeout(60)
            .enableSession(3600)
            .setUploadPath(upload_tmp_path)
            .setDocumentRoot("./public");

        sfc::g_user_service = std::make_shared<sfc::UserService>(sfc::UserDBConfig{
            config.database.container_name,
            config.database.name,
            config.database.user,
            config.database.password,
        });
        bool user_schema_ready = false;
        for (int attempt = 1; attempt <= 45; ++attempt) {
            if (sfc::g_user_service->init_schema()) {
                user_schema_ready = true;
                break;
            }
            spdlog::warn(
                "Database not ready for users schema (attempt {}/45), waiting...",
                attempt
            );
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!user_schema_ready) {
            throw std::runtime_error("Failed to initialize users table: database not ready");
        }
        if (!sfc::g_user_service->seed_default_accounts()) {
            throw std::runtime_error("Failed to seed default users");
        }

        sfc::g_runtime_state_service = std::make_shared<sfc::RuntimeStateService>(sfc::RuntimeDBConfig{
            config.database.container_name,
            config.database.name,
            config.database.user,
            config.database.password,
        });
        sfc::g_deployment_orchestrator = std::make_shared<sfc::DeploymentOrchestratorService>();
        bool runtime_schema_ready = false;
        for (int attempt = 1; attempt <= 45; ++attempt) {
            if (sfc::g_runtime_state_service->init_schema()) {
                runtime_schema_ready = true;
                break;
            }
            spdlog::warn(
                "Database not ready for runtime schema (attempt {}/45), waiting...",
                attempt
            );
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!runtime_schema_ready) {
            throw std::runtime_error("Failed to initialize runtime_state tables: database not ready");
        }
        sfc::g_deployment_orchestrator->start();

        sfc::Topology persisted_topology;
        std::string persisted_template;
        bool restored_topology = false;
        if (sfc::g_runtime_state_service->load_topology(&persisted_topology, &persisted_template) &&
            !persisted_topology.nodes.empty()) {
            sfc::g_topo_mgr->save_current_topology(persisted_topology);
            sfc::g_res_mgr->reset_all_allocations();
            sfc::g_res_mgr->load_topology(persisted_topology);
            restored_topology = true;
            spdlog::info(
                "Restored topology from DB: {} nodes, {} links, template={}",
                persisted_topology.nodes.size(),
                persisted_topology.links.size(),
                persisted_template.empty() ? "unknown" : persisted_template
            );
        }

        double boot_sampling_interval_sec = 15.0;
        double boot_simulation_speed = 1.0;
        bool boot_sim_should_run = true;
        if (const nlohmann::json control_config = sfc::g_runtime_state_service->load_control_config();
            control_config.is_object()) {
            if (control_config.contains("resource_sampling_interval_sec")) {
                boot_sampling_interval_sec = std::max(
                    10.0,
                    std::min(120.0, control_config.value("resource_sampling_interval_sec", 15.0))
                );
            }
            if (control_config.contains("simulation_speed")) {
                boot_simulation_speed = std::max(
                    0.1,
                    std::min(20.0, control_config.value("simulation_speed", 1.0))
                );
            }
            if (control_config.contains("running") && control_config["running"].is_boolean()) {
                boot_sim_should_run = control_config.value("running", true);
            }
        }
        if (!restored_topology) {
            spdlog::info("No topology in DB at startup; waiting for /api/v1/topology/generate");
        }

        app().registerPreRoutingAdvice(
            [](const HttpRequestPtr& req,
               AdviceCallback&& callback,
               AdviceChainCallback&& chain_callback) {
                const std::string path = req->path();
                if (!starts_with(path, "/api/v1/")) {
                    chain_callback();
                    return;
                }
                if (req->method() == Options || is_public_api_path(req)) {
                    chain_callback();
                    return;
                }
                if (!sfc::g_auth_service) {
                    callback(build_auth_error(k500InternalServerError, "auth_service_unavailable"));
                    return;
                }
                const std::string token = extract_bearer_token(req);
                if (token.empty()) {
                    callback(build_auth_error(k401Unauthorized, "missing_token"));
                    return;
                }
                const auto claims = sfc::g_auth_service->verify_token(token);
                if (!claims) {
                    callback(build_auth_error(k401Unauthorized, "invalid_or_expired_token"));
                    return;
                }
                if (is_admin_only_path(req) && !claims->is_admin()) {
                    callback(build_auth_error(k403Forbidden, "admin_required"));
                    return;
                }
                chain_callback();
            }
        );
        
        // 启用CORS
        app().registerPostHandlingAdvice(
            [](const HttpRequestPtr&, const HttpResponsePtr& resp) {
                resp->addHeader("Access-Control-Allow-Origin", "*");
                resp->addHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
                resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            }
        );
        
        // 健康检查
        app().registerHandler(
            "/api/v1/health",
            [](const HttpRequestPtr&,
               std::function<void(const HttpResponsePtr&)>&& callback) {
                Json::Value resp;
                resp["status"] = "ok";
                resp["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
                auto http_resp = HttpResponse::newHttpJsonResponse(resp);
                callback(http_resp);
            },
            {Get}
        );

        // 第三阶段：会话级连续推理接口（显式注册，确保动态编排路由稳定可用）
        static sfc::SFCController sfc_controller;
        app().registerHandler(
            "/api/v1/sfc/session/start",
            [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
                sfc_controller.startSession(req, std::move(callback));
            },
            {Post}
        );
        app().registerHandler(
            "/api/v1/sfc/session/stop",
            [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
                sfc_controller.stopSession(req, std::move(callback));
            },
            {Post}
        );
        app().registerHandler(
            "/api/v1/sfc/sessions",
            [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
                sfc_controller.listSessions(req, std::move(callback));
            },
            {Get}
        );
        app().registerHandler(
            "/api/v1/sfc/session/{1}",
            [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& session_id) {
                sfc_controller.getSessionStatus(req, std::move(callback), session_id);
            },
            {Get}
        );
        app().registerHandler(
            "/api/v1/sfc/session/{1}/recompute",
            [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& session_id) {
                sfc_controller.recomputeSession(req, std::move(callback), session_id);
            },
            {Post}
        );

        if (restored_topology) {
            const int restored_deployments = restore_active_deployments_after_boot();
            if (restored_deployments > 0) {
                spdlog::info(
                    "Queued {} persisted deployment(s) for runtime restore after restart",
                    restored_deployments
                );
            } else {
                spdlog::info("No active persisted deployments to restore at startup");
            }
        }

        if (restored_topology && boot_sim_should_run) {
            const bool started = sfc::g_dynamic_sim->start(
                boot_sampling_interval_sec,
                boot_simulation_speed,
                false,
                0.0,
                0.0
            );
            spdlog::info(
                "Dynamic simulation bootstrap on persisted topology: started={}, interval={}s speed={}x",
                started ? "true" : "false",
                boot_sampling_interval_sec,
                boot_simulation_speed
            );
        } else if (restored_topology) {
            spdlog::info(
                "Dynamic simulation remained paused from persisted state: interval={}s speed={}x",
                boot_sampling_interval_sec,
                boot_simulation_speed
            );
        }
        
        spdlog::info("Server configured:");
        spdlog::info("  - Address: {}:{}", config.server.host, config.server.port);
        spdlog::info("  - Threads: {}", config.server.threads);
        spdlog::info("  - GNN Model: {}", config.onnx.gnn_model);
        spdlog::info("  - Actor Model: {}", config.onnx.actor_model);
        spdlog::info("  - Database: docker:{} / {}", config.database.container_name, config.database.name);
        spdlog::info("  - JWT Expire: {}h", config.auth.token_expire_hours);
        spdlog::info("  - Upload Temp Path: {}", upload_tmp_path);
        spdlog::info("");
        spdlog::info("Starting server...");
        
        app().run();
        const bool sim_was_running = sfc::g_dynamic_sim && sfc::g_dynamic_sim->is_running();
        persist_runtime_snapshot_on_shutdown("app_run_exit");
        if (sfc::g_runtime_state_service) {
            nlohmann::json cfg = sfc::g_runtime_state_service->load_control_config();
            if (!cfg.is_object()) cfg = nlohmann::json::object();
            cfg["running"] = sim_was_running;
            sfc::g_runtime_state_service->save_control_config(cfg);
        }
        if (sfc::g_dynamic_sim) {
            sfc::g_dynamic_sim->stop(false);
        }
        if (sfc::g_deployment_orchestrator) {
            sfc::g_deployment_orchestrator->stop();
        }
        if (instance_lock_acquired) {
            release_instance_lock_file(instance_lock_path);
            instance_lock_acquired = false;
        }
        
    } catch (const std::exception& e) {
        persist_runtime_snapshot_on_shutdown("fatal_exception");
        if (sfc::g_dynamic_sim) {
            sfc::g_dynamic_sim->stop(false);
        }
        if (sfc::g_deployment_orchestrator) {
            sfc::g_deployment_orchestrator->stop();
        }
        if (instance_lock_acquired) {
            release_instance_lock_file(instance_lock_path);
            instance_lock_acquired = false;
        }
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }
    
    return 0;
}
