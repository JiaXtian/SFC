#pragma once
#include <drogon/HttpController.h>
#include "services/InferenceEngine.h"
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"

using namespace drogon;

namespace sfc {

// 全局服务声明
extern std::shared_ptr<InferenceEngine> g_inference_engine;
extern std::shared_ptr<TopologyManager> g_topo_mgr;
extern std::shared_ptr<ResourceManager> g_res_mgr;

class SFCController : public HttpController<SFCController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SFCController::plan, "/api/v1/sfc/plan", Post);
    ADD_METHOD_TO(SFCController::deploy, "/api/v1/sfc/deploy", Post);
    ADD_METHOD_TO(SFCController::rollback, "/api/v1/sfc/rollback", Post);
    ADD_METHOD_TO(SFCController::getDeployments, "/api/v1/deployments", Get);
    METHOD_LIST_END
    
    void plan(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);
    
    void deploy(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback);
    
    void rollback(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
    
    void getDeployments(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);
    
private:
    SFCRequest parse_sfc_request(const Json::Value& json);
};

} // namespace sfc
