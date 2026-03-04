#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "controllers/TopologyController.h"
#include "controllers/SFCController.h"
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"
#include "services/InferenceEngine.h"
#include <fstream>

using namespace drogon;
using json = nlohmann::json;

// 全局服务实例定义
namespace sfc {
    std::shared_ptr<TopologyManager> g_topo_mgr;
    std::shared_ptr<ResourceManager> g_res_mgr;
    std::shared_ptr<InferenceEngine> g_inference_engine;
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
            
            spdlog::info("Config loaded from {}", config_file);
        } else {
            spdlog::warn("Config file {} not found, using defaults", config_file);
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config: {}, using defaults", e.what());
    }
    
    return config;
}

int main() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    
    Config config = load_config("config.json");
    
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
        
        // 启动时不再加载默认星座，等待前端显式生成/导入拓扑后再进行部署
        spdlog::info("No default topology loaded at startup; waiting for /api/v1/topology/generate");
        
        // 配置Drogon
        app()
            .setLogPath("./logs")
            .setLogLevel(trantor::Logger::kInfo)
            .addListener(config.server.host, config.server.port)
            .setThreadNum(config.server.threads)
            .setClientMaxBodySize(100 * 1024 * 1024)
            .setMaxConnectionNum(10000)
            .setIdleConnectionTimeout(60)
            .enableSession(3600)
            .setDocumentRoot("./public");
        
        // 启用CORS
        app().registerPostHandlingAdvice(
            [](const HttpRequestPtr&, const HttpResponsePtr& resp) {
                resp->addHeader("Access-Control-Allow-Origin", "*");
                resp->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
                resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
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
        
        spdlog::info("Server configured:");
        spdlog::info("  - Address: {}:{}", config.server.host, config.server.port);
        spdlog::info("  - Threads: {}", config.server.threads);
        spdlog::info("  - GNN Model: {}", config.onnx.gnn_model);
        spdlog::info("  - Actor Model: {}", config.onnx.actor_model);
        spdlog::info("");
        spdlog::info("Starting server...");
        
        app().run();
        
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }
    
    return 0;
}
