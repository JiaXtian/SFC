#include "controllers/TopologyController.h"
#include "utils/json_converter.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <ctime>

namespace sfc {

void TopologyController::getTopology(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto topology = g_topo_mgr->get_current_topology();
        auto json_val = nlohmann_to_jsoncpp(topology.to_json());
        auto resp = HttpResponse::newHttpJsonResponse(json_val);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to get topology: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Internal server error";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::generateTopology(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        if (!json) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid JSON";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }
        
        int total_sats = 576;
        int num_planes = 24;
        double altitude = 550.0;
        double inclination = 53.0;

        if (json->isMember("metadata")) {
            const auto& meta = (*json)["metadata"];
            if (meta.isMember("total_satellites")) total_sats = meta["total_satellites"].asInt();
            if (meta.isMember("total_sats")) total_sats = meta["total_sats"].asInt();
            if (meta.isMember("num_planes")) num_planes = meta["num_planes"].asInt(); // 确保前端传了这个
            if (meta.isMember("altitude_km")) altitude = meta["altitude_km"].asDouble();
            if (meta.isMember("inclination_deg")) inclination = meta["inclination_deg"].asDouble();
        } 
        else {
            total_sats = json->get("total_sats", 576).asInt();
            num_planes = json->get("num_planes", 24).asInt();
        }

        const bool has_nodes = json->isMember("nodes") && (*json)["nodes"].isArray() && (*json)["nodes"].size() > 0;
        const bool has_links = json->isMember("links") && (*json)["links"].isArray() && (*json)["links"].size() > 0;

        Topology topology;

        if (has_nodes) {
            // 优先使用前端上传的完整拓扑，避免后续部署/回滚时星座构型发生变化
            topology.metadata.total_sats = total_sats;
            topology.metadata.num_planes = num_planes;
            topology.metadata.altitude_km = altitude;
            topology.metadata.inclination_deg = inclination;
            topology.metadata.topology_version = json->isMember("metadata")
                ? (*json)["metadata"].get("topology_version", 0).asInt()
                : 0;
            topology.metadata.sampling_interval_sec = json->isMember("metadata")
                ? (*json)["metadata"].get("sampling_interval_sec", 5.0).asDouble()
                : 5.0;
            topology.metadata.sim_time = json->isMember("metadata")
                ? (*json)["metadata"].get("sim_time", "").asString()
                : "";
            topology.metadata.timestamp = json->isMember("metadata")
                ? (*json)["metadata"].get("timestamp", "").asString()
                : "";

            if (topology.metadata.timestamp.empty()) {
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                char buf[100];
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t));
                topology.metadata.timestamp = buf;
            }

            int max_plane_idx = 0;
            for (const auto& n : (*json)["nodes"]) {
                Satellite sat;
                sat.id = n.get("id", "").asString();
                sat.type = n.get("type", "satellite").asString();

                if (n.isMember("orbital_params")) {
                    const auto& op = n["orbital_params"];
                    sat.orbital_params.plane = op.get("plane", 0).asInt();
                    sat.orbital_params.position_in_plane = op.get("position_in_plane", 0).asInt();
                    sat.orbital_params.raan = op.get("raan", 0.0).asDouble();
                    sat.orbital_params.true_anomaly = op.get("true_anomaly", 0.0).asDouble();
                    sat.orbital_params.altitude_km = op.get("altitude_km", altitude).asDouble();
                    sat.orbital_params.inclination_deg = op.get("inclination_deg", op.get("inclination", inclination).asDouble()).asDouble();
                    max_plane_idx = std::max(max_plane_idx, sat.orbital_params.plane);
                }

                if (n.isMember("coordinates")) {
                    const auto& c = n["coordinates"];
                    sat.coordinates.x = c.get("x", 0.0).asDouble();
                    sat.coordinates.y = c.get("y", 0.0).asDouble();
                    sat.coordinates.z = c.get("z", 0.0).asDouble();
                    sat.coordinates.lat = c.get("lat", 0.0).asDouble();
                    sat.coordinates.lon = c.get("lon", 0.0).asDouble();
                }

                sat.cpu_total = n.get("cpu_total", 16.0).asDouble();
                sat.cpu_available = n.get("cpu_available", sat.cpu_total).asDouble();
                sat.mem_total = n.get("mem_total", 32.0).asDouble();
                sat.mem_available = n.get("mem_available", sat.mem_total).asDouble();
                sat.disk_total = n.get("disk_total", 200.0).asDouble();
                sat.disk_available = n.get("disk_available", sat.disk_total).asDouble();
                const double cpu_ratio = sat.cpu_total > 1e-9 ? sat.cpu_available / sat.cpu_total : 0.0;
                const double mem_ratio = sat.mem_total > 1e-9 ? sat.mem_available / sat.mem_total : 0.0;
                const double disk_ratio = sat.disk_total > 1e-9 ? sat.disk_available / sat.disk_total : 0.0;
                const double resource_health = std::max(
                    0.0, std::min(1.0, (cpu_ratio + mem_ratio + disk_ratio) / 3.0)
                );
                sat.core_network_load = n.get("core_network_load", 1.0 - resource_health).asDouble();
                sat.node_reliability = n.get("node_reliability", 0.985 + 0.014 * resource_health).asDouble();
                sat.status = n.get("status", "active").asString();
                sat.fault_tag = n.get("fault_tag", "").asString();
                sat.core_network_load = std::max(0.0, std::min(1.0, sat.core_network_load));
                sat.node_reliability = std::max(0.7, std::min(0.999, sat.node_reliability));

                topology.nodes.push_back(sat);
            }

            if (has_links) {
                for (const auto& l : (*json)["links"]) {
                    Link link;
                    link.source = l.get("source", "").asString();
                    link.target = l.get("target", "").asString();
                    link.link_type = l.get("link_type", "inter_orbit").asString();
                    if (l.isMember("status")) {
                        link.status = l["status"].asString();
                    } else {
                        link.status = l.get("link_status", 1).asInt() == 0 ? "down" : "active";
                    }
                    link.fault_tag = l.get("fault_tag", "").asString();
                    link.latency_ms = l.get("latency_ms", 1.0).asDouble();
                    if (l.isMember("reliability")) {
                        link.reliability = l["reliability"].asDouble();
                    } else {
                        link.reliability = l.get("link_reliability", 0.999).asDouble();
                    }
                    link.bandwidth_gbps = l.get("bandwidth_gbps", 10.0).asDouble();
                    link.bandwidth_available_gbps = l.get("bandwidth_available_gbps", link.bandwidth_gbps).asDouble();
                    topology.links.push_back(link);
                }
            }

            topology.metadata.total_sats = static_cast<int>(topology.nodes.size());
            if (json->isMember("metadata") && (*json)["metadata"].isMember("num_planes")) {
                topology.metadata.num_planes = (*json)["metadata"].get("num_planes", num_planes).asInt();
            } else {
                topology.metadata.num_planes = max_plane_idx + 1;
            }

            g_topo_mgr->save_current_topology(topology);
            g_res_mgr->reset_all_allocations();
            g_res_mgr->load_topology(topology);
            spdlog::info("Loaded external topology: {} nodes, {} links",
                        topology.nodes.size(), topology.links.size());
        } else {
            spdlog::info("Generating: sats={}, planes={}, alt={}", total_sats, num_planes, altitude);
            topology = g_topo_mgr->generate_walker_delta(total_sats, num_planes, altitude, inclination, 42);
            g_res_mgr->reset_all_allocations();
            g_res_mgr->load_topology(topology);
        }
        
        Json::Value response;
        response["topology_id"] = "topo_" + topology.metadata.timestamp;
        response["topology_summary"]["total_nodes"] = static_cast<int>(topology.nodes.size());
        response["topology_summary"]["total_links"] = static_cast<int>(topology.links.size());
        response["dynamic_status"] = nlohmann_to_jsoncpp(g_dynamic_sim->status_json());
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to generate topology: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to generate topology";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::startDynamicSimulation(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        const double interval_sec = json ? (*json).get("sampling_interval_sec", 5.0).asDouble() : 5.0;
        const double sim_speed = json ? (*json).get("simulation_speed", 1.0).asDouble() : 1.0;
        const bool enable_faults = false;
        const double node_fault_prob = 0.0;
        const double link_fault_prob = 0.0;

        const bool started = g_dynamic_sim->start(
            interval_sec,
            sim_speed,
            enable_faults,
            node_fault_prob,
            link_fault_prob
        );

        Json::Value response;
        response["started"] = started;
        response["status"] = nlohmann_to_jsoncpp(g_dynamic_sim->status_json());
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to start dynamic simulation: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to start dynamic simulation";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::stopDynamicSimulation(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        g_dynamic_sim->stop();
        Json::Value response;
        response["stopped"] = true;
        response["status"] = nlohmann_to_jsoncpp(g_dynamic_sim->status_json());
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to stop dynamic simulation: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to stop dynamic simulation";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::stepDynamicSimulation(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        const auto snapshot = g_dynamic_sim->step_once();
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(snapshot.to_json()));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to step dynamic simulation: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to step dynamic simulation";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::injectDynamicFaults(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        nlohmann::json payload = nlohmann::json::object();
        if (json) {
            payload = nlohmann::json::parse(json->toStyledString(), nullptr, false);
            if (payload.is_discarded()) {
                payload = nlohmann::json::object();
            }
        }
        const auto result = g_dynamic_sim->inject_faults(payload);
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(result));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to inject dynamic faults: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to inject dynamic faults";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getDynamicSimulationStatus(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto resp = HttpResponse::newHttpJsonResponse(
            nlohmann_to_jsoncpp(g_dynamic_sim->status_json())
        );
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to get dynamic simulation status: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to get dynamic simulation status";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getSatellites(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto topology = g_topo_mgr->get_current_topology();
        
        Json::Value satellites_json(Json::arrayValue);
        for (const auto& sat : topology.nodes) {
            satellites_json.append(nlohmann_to_jsoncpp(sat.to_json()));
        }
        
        auto resp = HttpResponse::newHttpJsonResponse(satellites_json);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to get satellites: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getSatellite(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& id
) {
    try {
        auto sat = g_res_mgr->get_satellite(id);
        auto json_val = nlohmann_to_jsoncpp(sat.to_json());
        auto resp = HttpResponse::newHttpJsonResponse(json_val);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to get satellite {}: {}", id, e.what());
        Json::Value error;
        error["code"] = 404;
        error["message"] = "Satellite not found";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    }
}

} // namespace sfc
