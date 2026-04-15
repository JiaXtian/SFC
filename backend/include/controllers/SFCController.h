#pragma once
#include <drogon/HttpController.h>
#include "services/InferenceEngine.h"
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"
#include "services/DynamicSimulationService.h"
#include "services/DynamicInferenceService.h"
#include "services/SatelliteRuntimeService.h"
#include "services/PersistenceService.h"

using namespace drogon;

namespace sfc {

// 全局服务声明
extern std::shared_ptr<InferenceEngine> g_inference_engine;
extern std::shared_ptr<TopologyManager> g_topo_mgr;
extern std::shared_ptr<ResourceManager> g_res_mgr;
extern std::shared_ptr<DynamicSimulationService> g_dynamic_sim;
extern std::shared_ptr<DynamicInferenceService> g_dynamic_inference;
extern std::shared_ptr<SatelliteRuntimeService> g_sat_runtime;
extern std::shared_ptr<PersistenceService> g_persistence;

class SFCController : public HttpController<SFCController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SFCController::plan, "/api/v1/sfc/plan", Post);
    ADD_METHOD_TO(SFCController::deploy, "/api/v1/sfc/deploy", Post);
    ADD_METHOD_TO(SFCController::rollback, "/api/v1/sfc/rollback", Post);
    ADD_METHOD_TO(SFCController::getDeployments, "/api/v1/deployments", Get);
    ADD_METHOD_TO(SFCController::startSession, "/api/v1/sfc/session/start", Post);
    ADD_METHOD_TO(SFCController::stopSession, "/api/v1/sfc/session/stop", Post);
    ADD_METHOD_TO(SFCController::listSessions, "/api/v1/sfc/sessions", Get);
    ADD_METHOD_TO(SFCController::getSessionStatus, "/api/v1/sfc/session/{session_id}", Get);
    ADD_METHOD_TO(SFCController::recomputeSession, "/api/v1/sfc/session/{session_id}/recompute", Post);
    METHOD_LIST_END
    
    void plan(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);
    
    void deploy(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback);
    
    void rollback(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
    
    void getDeployments(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

    void startSession(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

    void stopSession(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

    void listSessions(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

    void getSessionStatus(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         const std::string& session_id);

    void recomputeSession(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& session_id);
    
private:
    SFCRequest parse_sfc_request(const Json::Value& json);
};

} // namespace sfc
