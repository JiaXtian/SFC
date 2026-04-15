#pragma once
#include <drogon/HttpController.h>
#include "services/TopologyManager.h"
#include "services/ResourceManager.h"
#include "services/DynamicSimulationService.h"
#include "services/DynamicInferenceService.h"
#include "services/SatelliteRuntimeService.h"
#include "services/PersistenceService.h"

using namespace drogon;

namespace sfc {

// 全局服务声明
extern std::shared_ptr<TopologyManager> g_topo_mgr;
extern std::shared_ptr<ResourceManager> g_res_mgr;
extern std::shared_ptr<DynamicSimulationService> g_dynamic_sim;
extern std::shared_ptr<DynamicInferenceService> g_dynamic_inference;
extern std::shared_ptr<SatelliteRuntimeService> g_sat_runtime;
extern std::shared_ptr<PersistenceService> g_persistence;

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
    ADD_METHOD_TO(TopologyController::collectSatelliteTelemetry, "/api/v1/satellites/runtime/collect", Post);
    ADD_METHOD_TO(TopologyController::startSatellitePods, "/api/v1/satellites/pods/start", Post);
    ADD_METHOD_TO(TopologyController::stopSatellitePods, "/api/v1/satellites/pods/stop", Post);
    ADD_METHOD_TO(TopologyController::stopAllSatellitePods, "/api/v1/satellites/pods/stop_all", Post);
    ADD_METHOD_TO(TopologyController::deleteSatelliteNodes, "/api/v1/satellites/nodes/delete", Post);
    ADD_METHOD_TO(TopologyController::getRuntimeStatus, "/api/v1/satellites/runtime/status", Get);
    ADD_METHOD_TO(TopologyController::getPersistenceStatus, "/api/v1/persistence/status", Get);
    ADD_METHOD_TO(TopologyController::resetPersistenceData, "/api/v1/persistence/reset", Post);
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

    void collectSatelliteTelemetry(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback);

    void startSatellitePods(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback);

    void stopSatellitePods(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback);

    void stopAllSatellitePods(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback);

    void deleteSatelliteNodes(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback);

    void getRuntimeStatus(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback);

    void getPersistenceStatus(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback);

    void resetPersistenceData(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace sfc
