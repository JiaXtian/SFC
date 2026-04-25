#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "controllers/TopologyController.h"
#include "controllers/SFCController.h"
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
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <stdexcept>

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
    
    try {
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
        if (const nlohmann::json control_config = sfc::g_runtime_state_service->load_control_config();
            control_config.is_object()) {
            if (control_config.contains("resource_sampling_interval_sec")) {
                boot_sampling_interval_sec = std::max(
                    10.0,
                    std::min(30.0, control_config.value("resource_sampling_interval_sec", 15.0))
                );
            }
            if (control_config.contains("simulation_speed")) {
                boot_simulation_speed = std::max(
                    0.1,
                    std::min(20.0, control_config.value("simulation_speed", 1.0))
                );
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
        if (sfc::g_deployment_orchestrator) {
            sfc::g_deployment_orchestrator->stop();
        }
        
    } catch (const std::exception& e) {
        if (sfc::g_deployment_orchestrator) {
            sfc::g_deployment_orchestrator->stop();
        }
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }
    
    return 0;
}
