#pragma once
#include <drogon/HttpController.h>
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"
#include "services/DynamicSimulationService.h"

using namespace drogon;

namespace sfc {

// 全局服务声明
extern std::shared_ptr<TopologyManager> g_topo_mgr;
extern std::shared_ptr<ResourceManager> g_res_mgr;
extern std::shared_ptr<DynamicSimulationService> g_dynamic_sim;

class TopologyController : public HttpController<TopologyController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TopologyController::getTopology, "/api/v1/topology", Get);
    ADD_METHOD_TO(TopologyController::generateTopology, "/api/v1/topology/generate", Post);
    ADD_METHOD_TO(TopologyController::getSatellites, "/api/v1/satellites", Get);
    ADD_METHOD_TO(TopologyController::getSatellite, "/api/v1/satellite/{id}", Get);
    ADD_METHOD_TO(TopologyController::startDynamicSimulation, "/api/v1/topology/dynamic/start", Post);
    ADD_METHOD_TO(TopologyController::stopDynamicSimulation, "/api/v1/topology/dynamic/stop", Post);
    ADD_METHOD_TO(TopologyController::stepDynamicSimulation, "/api/v1/topology/dynamic/step", Post);
    ADD_METHOD_TO(TopologyController::injectDynamicFaults, "/api/v1/topology/dynamic/faults/inject", Post);
    ADD_METHOD_TO(TopologyController::getDynamicSimulationStatus, "/api/v1/topology/dynamic/status", Get);
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

    void startDynamicSimulation(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback);

    void stopDynamicSimulation(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback);

    void stepDynamicSimulation(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback);

    void injectDynamicFaults(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback);

    void getDynamicSimulationStatus(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace sfc
