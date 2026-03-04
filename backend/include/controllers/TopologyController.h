#pragma once
#include <drogon/HttpController.h>
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"

using namespace drogon;

namespace sfc {

// 全局服务声明
extern std::shared_ptr<TopologyManager> g_topo_mgr;
extern std::shared_ptr<ResourceManager> g_res_mgr;

class TopologyController : public HttpController<TopologyController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TopologyController::getTopology, "/api/v1/topology", Get);
    ADD_METHOD_TO(TopologyController::generateTopology, "/api/v1/topology/generate", Post);
    ADD_METHOD_TO(TopologyController::getSatellites, "/api/v1/satellites", Get);
    ADD_METHOD_TO(TopologyController::getSatellite, "/api/v1/satellite/{id}", Get);
    METHOD_LIST_END
    
    void getTopology(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
    
    void generateTopology(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback);
    
    void getSatellites(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
    
    void getSatellite(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& id);
};

} // namespace sfc
