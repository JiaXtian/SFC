#include "controllers/TopologyController.h"
#include "utils/json_converter.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <unordered_set>

namespace sfc {
namespace {

std::string normalize_id_token(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') out.push_back(ch);
        else out.push_back('_');
    }
    if (out.empty()) out = "default";
    return out;
}

} // namespace

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
        std::string template_id = "starlink_v1";
        bool recreate_existing_pods = false;
        bool force_reset_existing = false;

        if (json->isMember("metadata")) {
            const auto& meta = (*json)["metadata"];
            if (meta.isMember("total_satellites")) total_sats = meta["total_satellites"].asInt();
            if (meta.isMember("total_sats")) total_sats = meta["total_sats"].asInt();
            if (meta.isMember("num_planes")) num_planes = meta["num_planes"].asInt(); // 确保前端传了这个
            if (meta.isMember("altitude_km")) altitude = meta["altitude_km"].asDouble();
            if (meta.isMember("inclination_deg")) inclination = meta["inclination_deg"].asDouble();
            if (meta.isMember("template_id")) template_id = meta["template_id"].asString();
            else if (meta.isMember("constellation_type")) template_id = meta["constellation_type"].asString();
            if (meta.isMember("recreate_existing_pods")) recreate_existing_pods = meta["recreate_existing_pods"].asBool();
            if (meta.isMember("force_reset_existing")) force_reset_existing = meta["force_reset_existing"].asBool();
        } 
        else {
            total_sats = json->get("total_sats", 576).asInt();
            num_planes = json->get("num_planes", 24).asInt();
            template_id = json->get("template_id", "starlink_v1").asString();
            recreate_existing_pods = json->get("recreate_existing_pods", false).asBool();
            force_reset_existing = json->get("force_reset_existing", false).asBool();
        }

        nlohmann::json reset_result = {
            {"requested", force_reset_existing},
            {"runtime", {{"ok", true}, {"skipped", true}}},
            {"persistence", {{"ok", true}, {"skipped", true}}}
        };
        if (force_reset_existing) {
            try {
                auto previous = g_topo_mgr->get_current_topology();
                if (g_sat_runtime) {
                    reset_result["runtime"] = g_sat_runtime->stop_all_satellite_nodes(previous, true);
                } else {
                    reset_result["runtime"] = {
                        {"ok", false},
                        {"message", "runtime service unavailable"}
                    };
                }
                if (g_persistence) {
                    reset_result["persistence"] = g_persistence->reset_all_data();
                } else {
                    reset_result["persistence"] = {
                        {"ok", false},
                        {"message", "persistence service unavailable"}
                    };
                }
            } catch (const std::exception& ex) {
                reset_result["runtime"] = {
                    {"ok", false},
                    {"message", "force reset failed"},
                    {"details", ex.what()}
                };
            }
        }

        const bool has_nodes = json->isMember("nodes") && (*json)["nodes"].isArray() && (*json)["nodes"].size() > 0;
        const bool has_links = json->isMember("links") && (*json)["links"].isArray() && (*json)["links"].size() > 0;

        Topology topology;
        nlohmann::json runtime_result = {
            {"enabled", false},
            {"message", "runtime service not available"}
        };

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
                sat.template_id = n.get("template_id", template_id).asString();

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
                sat.fault_injected = n.get("fault_injected", !sat.fault_tag.empty()).asBool();
                sat.deployment_state = n.get("deployment_state", "none").asString();
                sat.deployment_detail = n.get("deployment_detail", "").asString();
                sat.core_nf_policy_applied = n.get("core_nf_policy_applied", false).asBool();
                sat.core_nf_policy = n.get("core_nf_policy", "").asString();
                sat.cpu_utilization_ratio = n.get("cpu_utilization_ratio", 1.0 - cpu_ratio).asDouble();
                sat.mem_utilization_ratio = n.get("mem_utilization_ratio", 1.0 - mem_ratio).asDouble();
                sat.disk_utilization_ratio = n.get("disk_utilization_ratio", 1.0 - disk_ratio).asDouble();
                sat.last_collected_at = n.get("last_collected_at", "").asString();
                if (n.isMember("telemetry") && n["telemetry"].isObject()) {
                    const auto& t = n["telemetry"];
                    sat.cpu_utilization_ratio = t.get("cpu_utilization_ratio", sat.cpu_utilization_ratio).asDouble();
                    sat.mem_utilization_ratio = t.get("mem_utilization_ratio", sat.mem_utilization_ratio).asDouble();
                    sat.disk_utilization_ratio = t.get("disk_utilization_ratio", sat.disk_utilization_ratio).asDouble();
                    sat.last_collected_at = t.get("last_collected_at", sat.last_collected_at).asString();
                }
                if (n.isMember("podman") && n["podman"].isObject()) {
                    sat.podman_container_name = n["podman"].get("container_name", "").asString();
                    sat.podman_container_id = n["podman"].get("container_id", "").asString();
                    sat.podman_status = n["podman"].get("status", "unknown").asString();
                } else {
                    sat.podman_container_name = n.get("podman_container_name", "").asString();
                    sat.podman_container_id = n.get("podman_container_id", "").asString();
                    sat.podman_status = n.get("podman_status", "unknown").asString();
                }
                sat.core_network_load = std::max(0.0, std::min(1.0, sat.core_network_load));
                sat.node_reliability = std::max(0.7, std::min(0.999, sat.node_reliability));
                sat.cpu_utilization_ratio = std::max(0.0, std::min(1.0, sat.cpu_utilization_ratio));
                sat.mem_utilization_ratio = std::max(0.0, std::min(1.0, sat.mem_utilization_ratio));
                sat.disk_utilization_ratio = std::max(0.0, std::min(1.0, sat.disk_utilization_ratio));

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

            spdlog::info("Loaded external topology: {} nodes, {} links",
                        topology.nodes.size(), topology.links.size());
        } else {
            spdlog::info("Generating: sats={}, planes={}, alt={}", total_sats, num_planes, altitude);
            topology = g_topo_mgr->generate_walker_delta(total_sats, num_planes, altitude, inclination, 42);
        }

        if (g_sat_runtime) {
            runtime_result = g_sat_runtime->provision_constellation(
                topology,
                template_id.empty() ? "starlink_v1" : template_id,
                recreate_existing_pods
            );
        }

        g_topo_mgr->save_current_topology(topology);
        g_res_mgr->reset_all_allocations();
        g_res_mgr->load_topology(topology);

        nlohmann::json persist_result = {
            {"ok", false},
            {"enabled", false},
            {"message", "persistence service not available"}
        };
        if (g_persistence) {
            const std::string cid = "constellation_" + normalize_id_token(topology.metadata.timestamp);
            g_persistence->set_constellation_id(cid);
            persist_result = g_persistence->persist_topology_snapshot(topology, "generate", true);
        }
        
        Json::Value response;
        response["topology_id"] = "topo_" + topology.metadata.timestamp;
        response["topology_summary"]["total_nodes"] = static_cast<int>(topology.nodes.size());
        response["topology_summary"]["total_links"] = static_cast<int>(topology.links.size());
        response["dynamic_status"] = nlohmann_to_jsoncpp(g_dynamic_sim->status_json());
        response["runtime"] = nlohmann_to_jsoncpp(runtime_result);
        response["persistence"] = nlohmann_to_jsoncpp(persist_result);
        response["reset"] = nlohmann_to_jsoncpp(reset_result);
        
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

        nlohmann::json recompute = {
            {"requested", false},
            {"active_sessions", 0},
            {"triggered", 0},
            {"results", nlohmann::json::array()},
            {"listener_tick_forced", false}
        };
        const int changed_count =
            result.value("injected", 0) +
            result.value("removed", 0) +
            result.value("extended", 0);
        const int skipped_existing = result.value("skipped_existing", 0);

        if ((changed_count > 0 || skipped_existing > 0) && g_dynamic_inference) {
            recompute["requested"] = true;
            const auto sessions = g_dynamic_inference->list_sessions();
            if (sessions.is_array()) {
                recompute["active_sessions"] = static_cast<int>(sessions.size());
                for (const auto& sess : sessions) {
                    const std::string sid = sess.value("session_id", std::string(""));
                    const bool active = sess.value("active", true);
                    if (sid.empty() || !active) continue;
                    auto one = g_dynamic_inference->force_recompute(sid, "manual_fault_injection");
                    recompute["results"].push_back(one);
                    const auto inner = one.value("result", nlohmann::json::object());
                    const std::string status = inner.value("status", std::string(""));
                    if (one.value("ok", false) && status != "inactive" && status != "session_inactive") {
                        recompute["triggered"] = recompute.value("triggered", 0) + 1;
                    }
                }
            }

            if (recompute.value("triggered", 0) == 0 && changed_count > 0 && g_dynamic_sim) {
                // Fallback: force one orchestration tick to avoid missed listener-triggered rescheduling.
                g_dynamic_inference->on_topology_tick(g_dynamic_sim->get_latest_snapshot());
                recompute["listener_tick_forced"] = true;
            }
        }

        if (g_persistence) {
            Topology t = g_topo_mgr->get_current_topology();
            (void)g_persistence->persist_topology_snapshot(t, "fault_inject", true);
        }
        nlohmann::json response = result;
        response["recompute"] = recompute;
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
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

void TopologyController::collectSatelliteTelemetry(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        const bool force = json ? (*json).get("force", false).asBool() : false;
        auto topology = g_topo_mgr->get_current_topology();
        nlohmann::json result = {
            {"ok", false},
            {"message", "runtime service unavailable"}
        };
        if (g_sat_runtime) {
            result = g_sat_runtime->collect_node_telemetry(topology, force);
            g_topo_mgr->save_current_topology(topology);
            g_res_mgr->load_topology(topology);
        }
        nlohmann::json persist = {
            {"ok", false},
            {"enabled", false}
        };
        if (g_persistence) {
            persist = g_persistence->persist_topology_snapshot(topology, "telemetry_collect", force);
        }
        nlohmann::json response = {
            {"runtime", result},
            {"persistence", persist}
        };
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to collect satellite telemetry: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to collect satellite telemetry";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::startSatellitePods(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        std::vector<std::string> node_ids;
        bool recreate_containers = false;
        if (json) {
            if ((*json).isMember("node_ids") && (*json)["node_ids"].isArray()) {
                for (const auto& item : (*json)["node_ids"]) {
                    const std::string id = item.asString();
                    if (!id.empty()) node_ids.push_back(id);
                }
            }
            if (node_ids.empty() && (*json).isMember("node_id")) {
                const std::string one = (*json)["node_id"].asString();
                if (!one.empty()) node_ids.push_back(one);
            }
            recreate_containers = (*json).get("recreate_containers", false).asBool();
        }
        if (node_ids.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "node_ids is required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        auto topology = g_topo_mgr->get_current_topology();
        nlohmann::json result = {
            {"ok", false},
            {"message", "runtime service unavailable"}
        };
        if (g_sat_runtime) {
            result = g_sat_runtime->start_satellite_nodes(topology, node_ids, recreate_containers);
            g_topo_mgr->save_current_topology(topology);
            g_res_mgr->load_topology(topology);
        }
        nlohmann::json persist = {
            {"ok", false},
            {"enabled", false}
        };
        if (g_persistence) {
            persist = g_persistence->persist_topology_snapshot(topology, "start_nodes", true);
            (void)g_persistence->persist_event("start_nodes", std::to_string(node_ids.size()), "manual start satellite pod nodes");
        }
        nlohmann::json response = {
            {"runtime", result},
            {"persistence", persist}
        };
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to start satellite pods: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to start satellite pods";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::stopSatellitePods(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        std::vector<std::string> node_ids;
        bool remove_containers = false;
        if (json) {
            if ((*json).isMember("node_ids") && (*json)["node_ids"].isArray()) {
                for (const auto& item : (*json)["node_ids"]) {
                    const std::string id = item.asString();
                    if (!id.empty()) node_ids.push_back(id);
                }
            }
            if (node_ids.empty() && (*json).isMember("node_id")) {
                const std::string one = (*json)["node_id"].asString();
                if (!one.empty()) node_ids.push_back(one);
            }
            remove_containers = (*json).get("remove_containers", false).asBool();
        }
        if (node_ids.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "node_ids is required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        auto topology = g_topo_mgr->get_current_topology();
        nlohmann::json result = {
            {"ok", false},
            {"message", "runtime service unavailable"}
        };
        if (g_sat_runtime) {
            result = g_sat_runtime->stop_satellite_nodes(topology, node_ids, remove_containers);
            g_topo_mgr->save_current_topology(topology);
            g_res_mgr->load_topology(topology);
        }
        nlohmann::json persist = {
            {"ok", false},
            {"enabled", false}
        };
        if (g_persistence) {
            persist = g_persistence->persist_topology_snapshot(topology, "stop_nodes", true);
            (void)g_persistence->persist_event("stop_nodes", std::to_string(node_ids.size()), "manual stop satellite pod nodes");
        }
        nlohmann::json response = {
            {"runtime", result},
            {"persistence", persist}
        };
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to stop satellite pods: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to stop satellite pods";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::stopAllSatellitePods(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        const bool remove_containers = json ? (*json).get("remove_containers", false).asBool() : false;

        auto topology = g_topo_mgr->get_current_topology();
        nlohmann::json result = {
            {"ok", false},
            {"message", "runtime service unavailable"}
        };
        if (g_sat_runtime) {
            result = g_sat_runtime->stop_all_satellite_nodes(topology, remove_containers);
            g_topo_mgr->save_current_topology(topology);
            g_res_mgr->load_topology(topology);
        }
        nlohmann::json persist = {
            {"ok", false},
            {"enabled", false}
        };
        if (g_persistence) {
            persist = g_persistence->persist_topology_snapshot(topology, "stop_all_nodes", true);
            (void)g_persistence->persist_event("stop_all_nodes", "all", "manual stop all satellite pod nodes");
        }

        nlohmann::json response = {
            {"runtime", result},
            {"persistence", persist}
        };
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to stop all satellite pods: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to stop all satellite pods";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::deleteSatelliteNodes(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        std::vector<std::string> node_ids;
        bool remove_containers = true;
        if (json) {
            if ((*json).isMember("node_ids") && (*json)["node_ids"].isArray()) {
                for (const auto& item : (*json)["node_ids"]) {
                    const std::string id = item.asString();
                    if (!id.empty()) node_ids.push_back(id);
                }
            }
            if (node_ids.empty() && (*json).isMember("node_id")) {
                const std::string one = (*json)["node_id"].asString();
                if (!one.empty()) node_ids.push_back(one);
            }
            remove_containers = (*json).get("remove_containers", true).asBool();
        }
        if (node_ids.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "node_ids is required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        auto topology = g_topo_mgr->get_current_topology();
        nlohmann::json runtime_result = {
            {"ok", false},
            {"message", "runtime service unavailable"}
        };
        if (g_sat_runtime) {
            runtime_result = g_sat_runtime->stop_satellite_nodes(topology, node_ids, remove_containers);
            g_sat_runtime->forget_nodes(node_ids);
        }

        const std::unordered_set<std::string> to_delete(node_ids.begin(), node_ids.end());
        const auto before_nodes = topology.nodes.size();
        const auto before_links = topology.links.size();
        topology.nodes.erase(
            std::remove_if(
                topology.nodes.begin(),
                topology.nodes.end(),
                [&](const Satellite& s) { return to_delete.find(s.id) != to_delete.end(); }
            ),
            topology.nodes.end()
        );
        topology.links.erase(
            std::remove_if(
                topology.links.begin(),
                topology.links.end(),
                [&](const Link& l) {
                    return to_delete.find(l.source) != to_delete.end() || to_delete.find(l.target) != to_delete.end();
                }
            ),
            topology.links.end()
        );

        topology.metadata.total_sats = static_cast<int>(topology.nodes.size());
        int max_plane = -1;
        for (const auto& sat : topology.nodes) {
            max_plane = std::max(max_plane, sat.orbital_params.plane);
        }
        topology.metadata.num_planes = std::max(0, max_plane + 1);

        g_topo_mgr->save_current_topology(topology);
        g_res_mgr->reset_all_allocations();
        g_res_mgr->load_topology(topology);

        nlohmann::json persist = {
            {"ok", false},
            {"enabled", false}
        };
        if (g_persistence) {
            persist = g_persistence->persist_topology_snapshot(topology, "delete_nodes", true);
            (void)g_persistence->persist_event(
                "delete_nodes",
                std::to_string(node_ids.size()),
                "manual delete satellite nodes and linked pods"
            );
        }

        nlohmann::json response = {
            {"ok", true},
            {"deleted_nodes", static_cast<int>(before_nodes - topology.nodes.size())},
            {"deleted_links", static_cast<int>(before_links - topology.links.size())},
            {"runtime", runtime_result},
            {"persistence", persist}
        };
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to delete satellite nodes: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to delete satellite nodes";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getRuntimeStatus(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        nlohmann::json response = {
            {"runtime", g_sat_runtime ? g_sat_runtime->runtime_status() : nlohmann::json{{"enabled", false}}},
            {"persistence", g_persistence ? g_persistence->status() : nlohmann::json{{"enabled", false}}}
        };
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to get runtime status: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to get runtime status";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::resetPersistenceData(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        auto json = req->getJsonObject();
        const bool confirm = json ? (*json).get("confirm", false).asBool() : false;
        if (!confirm) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "confirm=true is required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        nlohmann::json result = {
            {"ok", false},
            {"message", "persistence service unavailable"}
        };
        if (g_persistence) {
            result = g_persistence->reset_all_data();
        }
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(result));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to reset persistence data: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to reset persistence data";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void TopologyController::getPersistenceStatus(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        nlohmann::json response = g_persistence ? g_persistence->status() : nlohmann::json{{"enabled", false}};
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(response));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("Failed to get persistence status: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Failed to get persistence status";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

} // namespace sfc
