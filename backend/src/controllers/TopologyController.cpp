#include "controllers/TopologyController.h"
#include "services/AuthGlobals.h"
#include "services/RuntimeStateService.h"
#include "utils/json_converter.h"
#include "websocket/WSHandler.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>

namespace {

bool json_bool(const Json::Value& obj, const std::string& key, bool fallback = false) {
    if (!obj.isMember(key)) return fallback;
    const auto& v = obj[key];
    if (v.isBool()) return v.asBool();
    if (v.isInt() || v.isUInt()) return v.asInt() != 0;
    if (v.isString()) {
        std::string s = v.asString();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s == "1" || s == "true" || s == "yes" || s == "on";
    }
    return fallback;
}

double clamp_double(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

}  // namespace

namespace sfc {

void TopologyController::getTopology(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        Topology topology;
        bool loaded_from_db = false;
        if (g_runtime_state_service) {
            std::string ignored_template;
            loaded_from_db = g_runtime_state_service->load_topology(&topology, &ignored_template);
        }
        if (!loaded_from_db) {
            topology = g_topo_mgr->get_current_topology();
        }
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
        bool force_replace = json_bool(*json, "force_replace", false);
        std::string constellation_template = json->get("constellation_template", "").asString();

        if (json->isMember("metadata")) {
            const auto& meta = (*json)["metadata"];
            if (meta.isMember("total_satellites")) total_sats = meta["total_satellites"].asInt();
            if (meta.isMember("total_sats")) total_sats = meta["total_sats"].asInt();
            if (meta.isMember("num_planes")) num_planes = meta["num_planes"].asInt();
            if (meta.isMember("altitude_km")) altitude = meta["altitude_km"].asDouble();
            if (meta.isMember("inclination_deg")) inclination = meta["inclination_deg"].asDouble();
            force_replace = json_bool(meta, "force_replace", force_replace);
            if (constellation_template.empty()) {
                constellation_template = meta.get("constellation_template", "").asString();
            }
            if (constellation_template.empty()) {
                constellation_template = meta.get("constellation_type", "").asString();
            }
            if (constellation_template.empty()) {
                constellation_template = meta.get("template_id", "").asString();
            }
        } else {
            total_sats = json->get("total_sats", 576).asInt();
            num_planes = json->get("num_planes", 24).asInt();
        }

        const bool has_nodes = json->isMember("nodes") && (*json)["nodes"].isArray() && (*json)["nodes"].size() > 0;
        const bool has_links = json->isMember("links") && (*json)["links"].isArray() && (*json)["links"].size() > 0;
        if (constellation_template.empty()) {
            constellation_template = has_nodes ? "imported_topology" : "starlink_v1";
        }

        Topology existing = g_topo_mgr->get_current_topology();
        if (!force_replace && !existing.nodes.empty()) {
            Json::Value response;
            response["code"] = 409;
            response["message"] = "topology_exists";
            response["details"] = "Re-generating topology will clear all existing satellites and deployments";
            response["requires_confirmation"] = true;
            response["existing_topology"]["total_nodes"] = static_cast<int>(existing.nodes.size());
            response["existing_topology"]["total_links"] = static_cast<int>(existing.links.size());
            auto resp = HttpResponse::newHttpJsonResponse(response);
            resp->setStatusCode(k409Conflict);
            callback(resp);
            return;
        }

        const auto old_dynamic_status = g_dynamic_sim->status_json();
        const bool was_running = old_dynamic_status.value("running", false);
        double sampling_interval_sec = clamp_double(old_dynamic_status.value("sampling_interval_sec", 5.0), 1.0, 60.0);
        double simulation_speed = clamp_double(old_dynamic_status.value("simulation_speed", 1.0), 0.1, 20.0);
        g_dynamic_sim->stop();

        Topology topology;
        if (has_nodes) {
            topology.metadata.total_sats = total_sats;
            topology.metadata.num_planes = num_planes;
            topology.metadata.altitude_km = altitude;
            topology.metadata.inclination_deg = inclination;
            topology.metadata.topology_version = json->isMember("metadata")
                ? (*json)["metadata"].get("topology_version", 0).asInt()
                : 0;
            topology.metadata.sampling_interval_sec = json->isMember("metadata")
                ? (*json)["metadata"].get("sampling_interval_sec", sampling_interval_sec).asDouble()
                : sampling_interval_sec;
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
                if (sat.id.empty()) continue;

                if (n.isMember("orbital_params")) {
                    const auto& op = n["orbital_params"];
                    sat.orbital_params.plane = op.get("plane", 0).asInt();
                    sat.orbital_params.position_in_plane = op.get("position_in_plane", 0).asInt();
                    sat.orbital_params.raan = op.get("raan", 0.0).asDouble();
                    sat.orbital_params.true_anomaly = op.get("true_anomaly", 0.0).asDouble();
                    sat.orbital_params.altitude_km = op.get("altitude_km", altitude).asDouble();
                    sat.orbital_params.inclination_deg = op.get(
                        "inclination_deg",
                        op.get("inclination", inclination).asDouble()
                    ).asDouble();
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
                const double resource_health = std::max(0.0, std::min(1.0, (cpu_ratio + mem_ratio + disk_ratio) / 3.0));
                sat.core_business_load = CoreBusinessLoad{};
                sat.core_network_load = 0.0;
                sat.node_reliability = n.get("node_reliability", 0.985 + 0.014 * resource_health).asDouble();
                sat.status = n.get("status", "active").asString();
                sat.fault_tag = n.get("fault_tag", "").asString();
                sat.core_network_load = std::max(0.0, std::min(1.0, sat.core_network_load));
                sat.node_reliability = std::max(0.7, std::min(0.999, sat.node_reliability));
                topology.nodes.push_back(std::move(sat));
            }

            if (has_links) {
                for (const auto& l : (*json)["links"]) {
                    Link link;
                    link.source = l.get("source", "").asString();
                    link.target = l.get("target", "").asString();
                    if (link.source.empty() || link.target.empty()) continue;
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
                    topology.links.push_back(std::move(link));
                }
            }

            topology.metadata.total_sats = static_cast<int>(topology.nodes.size());
            if (json->isMember("metadata") && (*json)["metadata"].isMember("num_planes")) {
                topology.metadata.num_planes = (*json)["metadata"].get("num_planes", num_planes).asInt();
            } else {
                topology.metadata.num_planes = max_plane_idx + 1;
            }

            spdlog::info(
                "Loaded external topology: {} nodes, {} links",
                topology.nodes.size(),
                topology.links.size()
            );
        } else {
            spdlog::info("Generating topology: sats={}, planes={}, alt={}", total_sats, num_planes, altitude);
            topology = g_topo_mgr->generate_walker_delta(total_sats, num_planes, altitude, inclination, 42);
        }

        g_res_mgr->reset_all_allocations();
        g_res_mgr->load_topology(topology);
        g_topo_mgr->save_current_topology(topology);

        if (g_runtime_state_service) {
            const nlohmann::json control_config = g_runtime_state_service->load_control_config();
            if (control_config.is_object()) {
                if (control_config.contains("resource_sampling_interval_sec")) {
                    sampling_interval_sec = clamp_double(
                        control_config.value("resource_sampling_interval_sec", sampling_interval_sec),
                        1.0,
                        60.0
                    );
                }
                if (control_config.contains("simulation_speed")) {
                    simulation_speed = clamp_double(
                        control_config.value("simulation_speed", simulation_speed),
                        0.1,
                        20.0
                    );
                }
            }
            g_runtime_state_service->save_topology(topology, constellation_template);
            g_runtime_state_service->clear_deployments();
            g_runtime_state_service->clear_runtime_events();
            const nlohmann::json evt = {
                {"type", "topology_replaced"},
                {"sim_time", topology.metadata.timestamp},
                {"constellation_template", constellation_template},
                {"topology_version", topology.metadata.topology_version},
                {"total_nodes", topology.nodes.size()},
                {"total_links", topology.links.size()},
                {"message", "Topology replaced and deployments cleared"}
            };
            g_runtime_state_service->append_runtime_event(evt);
            WSHandler::broadcast_json(evt);
        }

        const bool started = g_dynamic_sim->start(
            sampling_interval_sec,
            simulation_speed,
            false,
            0.0,
            0.0
        );
        if (!started && was_running) {
            spdlog::warn("Dynamic simulation restart failed after topology replacement");
        }

        Json::Value response;
        response["topology_id"] = "topo_" + topology.metadata.timestamp;
        response["topology_summary"]["total_nodes"] = static_cast<int>(topology.nodes.size());
        response["topology_summary"]["total_links"] = static_cast<int>(topology.links.size());
        response["topology_summary"]["constellation_template"] = constellation_template;
        response["replaced_previous_topology"] = !existing.nodes.empty();
        response["requires_confirmation"] = false;
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

        if (g_runtime_state_service) {
            nlohmann::json cfg = g_runtime_state_service->load_control_config();
            if (!cfg.is_object()) cfg = nlohmann::json::object();
            cfg["resource_sampling_interval_sec"] = clamp_double(interval_sec, 1.0, 60.0);
            cfg["simulation_speed"] = clamp_double(sim_speed, 0.1, 20.0);
            cfg["running"] = started;
            g_runtime_state_service->save_control_config(cfg);
        }

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
        if (g_runtime_state_service) {
            nlohmann::json cfg = g_runtime_state_service->load_control_config();
            if (!cfg.is_object()) cfg = nlohmann::json::object();
            cfg["running"] = false;
            g_runtime_state_service->save_control_config(cfg);
        }
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
        nlohmann::json status = g_dynamic_sim->status_json();
        if (g_runtime_state_service) {
            status["control_config"] = g_runtime_state_service->load_control_config();
        }
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(status));
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

void TopologyController::deleteSatellite(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& id
) {
    try {
        Topology topology = g_res_mgr->export_current_topology();
        if (topology.nodes.empty()) {
            topology = g_topo_mgr->get_current_topology();
        }

        const auto before_nodes = topology.nodes.size();
        topology.nodes.erase(
            std::remove_if(
                topology.nodes.begin(),
                topology.nodes.end(),
                [&](const Satellite& sat) { return sat.id == id; }),
            topology.nodes.end());
        if (topology.nodes.size() == before_nodes) {
            Json::Value error;
            error["code"] = 404;
            error["message"] = "Satellite not found";
            error["node_id"] = id;
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }

        topology.links.erase(
            std::remove_if(
                topology.links.begin(),
                topology.links.end(),
                [&](const Link& link) { return link.source == id || link.target == id; }),
            topology.links.end());
        topology.metadata.total_sats = static_cast<int>(topology.nodes.size());
        topology.metadata.topology_version += 1;
        topology.metadata.sim_time = "";

        const auto dynamic_status = g_dynamic_sim->status_json();
        const bool was_running = dynamic_status.value("running", false);
        const double interval_sec = clamp_double(dynamic_status.value("sampling_interval_sec", 5.0), 1.0, 60.0);
        const double speed = clamp_double(dynamic_status.value("simulation_speed", 1.0), 0.1, 20.0);
        g_dynamic_sim->stop();

        g_res_mgr->reset_all_allocations();
        g_res_mgr->load_topology(topology);
        g_topo_mgr->save_current_topology(topology);

        if (g_runtime_state_service) {
            std::string template_name = "custom";
            Topology persisted;
            if (g_runtime_state_service->load_topology(&persisted, &template_name) && template_name.empty()) {
                template_name = "custom";
            }
            g_runtime_state_service->save_topology(topology, template_name);
            g_runtime_state_service->clear_deployments();
            const nlohmann::json evt = {
                {"type", "satellite_deleted"},
                {"node_id", id},
                {"topology_version", topology.metadata.topology_version},
                {"message", "Satellite deleted and deployments cleared"}
            };
            g_runtime_state_service->append_runtime_event(evt);
            WSHandler::broadcast_json(evt);
        }

        if (was_running) {
            g_dynamic_sim->start(interval_sec, speed, false, 0.0, 0.0);
        }

        Json::Value response;
        response["deleted"] = true;
        response["node_id"] = id;
        response["remaining_nodes"] = static_cast<Json::UInt64>(topology.nodes.size());
        response["remaining_links"] = static_cast<Json::UInt64>(topology.links.size());
        response["dynamic_status"] = nlohmann_to_jsoncpp(g_dynamic_sim->status_json());
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to delete satellite {}: {}", id, e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to delete satellite";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getRuntimeEvents(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        int limit = 120;
        const std::string limit_param = req->getParameter("limit");
        if (!limit_param.empty()) {
            try {
                limit = std::stoi(limit_param);
            } catch (...) {
                limit = 120;
            }
        }
        nlohmann::json events = nlohmann::json::array();
        if (g_runtime_state_service) {
            events = g_runtime_state_service->list_runtime_events(limit);
        }
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(events));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to get runtime events: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to get runtime events";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getControlConfig(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        nlohmann::json cfg = nlohmann::json::object();
        if (g_runtime_state_service) {
            cfg = g_runtime_state_service->load_control_config();
        }
        if (!cfg.is_object()) cfg = nlohmann::json::object();
        if (!cfg.contains("resource_sampling_interval_sec")) {
            cfg["resource_sampling_interval_sec"] = 5.0;
        }
        if (!cfg.contains("simulation_speed")) {
            cfg["simulation_speed"] = 1.0;
        }
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(cfg));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to get control config: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to get control config";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::updateControlConfig(
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

        const double sampling = clamp_double(
            json->get("resource_sampling_interval_sec", 5.0).asDouble(),
            1.0,
            60.0
        );
        const double speed = clamp_double(
            json->get("simulation_speed", 1.0).asDouble(),
            0.1,
            20.0
        );
        const bool apply_now = json_bool(*json, "apply_now", true);

        nlohmann::json cfg = nlohmann::json::object();
        if (g_runtime_state_service) {
            cfg = g_runtime_state_service->load_control_config();
            if (!cfg.is_object()) cfg = nlohmann::json::object();
            cfg["resource_sampling_interval_sec"] = sampling;
            cfg["simulation_speed"] = speed;
            g_runtime_state_service->save_control_config(cfg);
        }

        if (apply_now) {
            g_dynamic_sim->start(sampling, speed, false, 0.0, 0.0);
        }

        Json::Value response;
        response["ok"] = true;
        response["resource_sampling_interval_sec"] = sampling;
        response["simulation_speed"] = speed;
        response["apply_now"] = apply_now;
        response["dynamic_status"] = nlohmann_to_jsoncpp(g_dynamic_sim->status_json());
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to update control config: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to update control config";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

} // namespace sfc
