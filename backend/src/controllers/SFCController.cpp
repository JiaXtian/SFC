#include "controllers/SFCController.h"
#include "utils/json_converter.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <mutex>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <sstream>
#include <limits>
#include <cmath>

namespace sfc {

// 全局部署列表
static std::vector<Deployment> g_deployments;
static std::mutex g_deployments_mutex;

static std::vector<std::string> build_constraint_violations(
    const DeploymentCandidate& cand,
    const SFCRequest& req
) {
    std::vector<std::string> violations;
    if (cand.total_latency_ms > req.constraints.max_latency_ms) {
        std::ostringstream oss;
        oss << "时延超限: " << cand.total_latency_ms << "ms > " << req.constraints.max_latency_ms << "ms";
        violations.push_back(oss.str());
    }
    if (cand.bottleneck_bandwidth_gbps + 1e-9 < req.constraints.min_bandwidth_gbps) {
        std::ostringstream oss;
        oss << "带宽不足: " << cand.bottleneck_bandwidth_gbps << "Gbps < " << req.constraints.min_bandwidth_gbps << "Gbps";
        violations.push_back(oss.str());
    }
    if (cand.estimated_reliability + 1e-9 < req.constraints.min_reliability) {
        std::ostringstream oss;
        oss << "可靠性不足: " << cand.estimated_reliability << " < " << req.constraints.min_reliability;
        violations.push_back(oss.str());
    }
    if (violations.empty() && !cand.reason.empty()) {
        violations.push_back(cand.reason);
    }
    return violations;
}

static void normalize_candidate_metrics_for_response(
    DeploymentCandidate& cand,
    const SFCRequest& req
) {
    if (cand.total_latency_ms <= 1e-9 && !cand.link_details.empty()) {
        double total_latency = 0.0;
        for (const auto& link : cand.link_details) {
            total_latency += std::max(0.0, link.latency_ms);
        }
        cand.total_latency_ms = total_latency;
    }

    if (cand.bottleneck_bandwidth_gbps <= 1e-9) {
        if (!cand.link_details.empty()) {
            double bottleneck = std::numeric_limits<double>::infinity();
            for (const auto& link : cand.link_details) {
                bottleneck = std::min(bottleneck, std::max(0.0, link.bandwidth_available_gbps));
            }
            cand.bottleneck_bandwidth_gbps =
                std::isfinite(bottleneck) ? bottleneck : req.constraints.min_bandwidth_gbps;
        } else {
            cand.bottleneck_bandwidth_gbps = req.constraints.min_bandwidth_gbps;
        }
    }

    if (cand.estimated_reliability <= 1e-9) {
        double reliability = 1.0;
        for (const auto& link : cand.link_details) {
            reliability *= std::max(1e-9, link.reliability);
        }
        cand.estimated_reliability = std::max(reliability, 1e-6);
    }
}

SFCRequest SFCController::parse_sfc_request(const Json::Value& json) {
    SFCRequest request;
    
    request.request_id = json.get("request_id", "").asString();
    request.service_type = json.get("service_type", "custom_service").asString();
    request.source_node = json.isMember("source_node")
        ? json["source_node"].asString()
        : json.get("ingress_node", "").asString();
    request.destination_node = json.isMember("destination_node")
        ? json["destination_node"].asString()
        : json.get("egress_node", "").asString();
    request.priority = json.get("priority", "medium").asString();
    request.optimize = json.get("optimize", "latency").asString();
    request.topk = json.get("topk", 3).asInt();
    request.core_network_load = json.get("core_network_load", 0.5).asDouble();
    request.priority_weight = json.get("priority_weight", 1.0).asDouble();
    request.load_level = json.get("load_level", "medium").asString();

    if (json.isMember("score_weights")) {
        const auto& sw = json["score_weights"];
        request.score_weights.latency = sw.get("latency", -1.0).asDouble();
        request.score_weights.resource = sw.get("resource", -1.0).asDouble();
        request.score_weights.reliability = sw.get("reliability", -1.0).asDouble();
        request.score_weights.bandwidth = sw.get("bandwidth", -1.0).asDouble();
        request.score_weights.dispersion = sw.get("dispersion", -1.0).asDouble();
    }
    
    if (json.isMember("constraints")) {
        const auto& constraints = json["constraints"];
        request.constraints.max_latency_ms = constraints.get("max_latency_ms", 150.0).asDouble();
        request.constraints.min_bandwidth_gbps = constraints.get("min_bandwidth_gbps", 0.5).asDouble();
        request.constraints.min_reliability = constraints.get("min_reliability", 0.95).asDouble();
    } else if (json.isMember("sla")) {
        const auto& sla = json["sla"];
        request.constraints.max_latency_ms = sla.get("latency_requirement_ms", 150.0).asDouble();
        request.constraints.min_bandwidth_gbps = sla.get("bandwidth_demand_gbps", 0.5).asDouble();
        request.constraints.min_reliability = sla.get("reliability_requirement", 0.95).asDouble();
    } else {
        request.constraints.max_latency_ms = json.get("max_latency_ms", 150.0).asDouble();
        request.constraints.min_bandwidth_gbps = json.get("bandwidth_demand_gbps", 0.5).asDouble();
        request.constraints.min_reliability = json.get("reliability_requirement", 0.95).asDouble();
    }
    
    if (json.isMember("vnfs")) {
        for (const auto& vnf_json : json["vnfs"]) {
            VNF vnf;
            vnf.name = vnf_json.get("name", "").asString();
            vnf.cpu = vnf_json.get("cpu", 1.0).asDouble();
            vnf.mem = vnf_json.get("mem", 1.0).asDouble();
            vnf.disk = vnf_json.get("disk", vnf.mem * 2.0).asDouble();
            vnf.bw_in = vnf_json.get("bw_in", 0.1).asDouble();
            vnf.bw_out = vnf_json.get("bw_out", 0.1).asDouble();
            request.vnfs.push_back(vnf);
        }
    } else if (json.isMember("vnf_sequence")) {
        for (const auto& vnf_json : json["vnf_sequence"]) {
            VNF vnf;
            vnf.name = vnf_json.get("vnf_id", vnf_json.get("name", "")).asString();
            vnf.cpu = vnf_json.get("cpu_required", vnf_json.get("cpu", 1.0)).asDouble();
            vnf.mem = vnf_json.get("mem_required", vnf_json.get("mem", 1.0)).asDouble();
            vnf.disk = vnf_json.get("disk_required_gb", vnf_json.get("disk", vnf.mem * 2.0)).asDouble();
            const double bw_req = vnf_json.get("bandwidth_required_gbps", 0.1).asDouble();
            vnf.bw_in = vnf_json.get("bw_in", bw_req).asDouble();
            vnf.bw_out = vnf_json.get("bw_out", bw_req).asDouble();
            request.vnfs.push_back(vnf);
        }
    }

    request.topk = std::max(1, std::min(32, request.topk));
    request.core_network_load = std::max(0.0, std::min(1.0, request.core_network_load));
    request.priority_weight = std::max(0.1, request.priority_weight);
    request.constraints.max_latency_ms = std::max(10.0, request.constraints.max_latency_ms);
    request.constraints.min_bandwidth_gbps = std::max(0.01, request.constraints.min_bandwidth_gbps);
    request.constraints.min_reliability = std::max(0.72, std::min(0.995, request.constraints.min_reliability));

    const double reliability_cap = request.vnfs.size() >= 4 ? 0.95 : 0.97;
    if (request.constraints.min_reliability > reliability_cap) {
        spdlog::warn(
            "Request {} reliability {:.4f} too strict for {} VNFs, capped to {:.4f}",
            request.request_id,
            request.constraints.min_reliability,
            request.vnfs.size(),
            reliability_cap
        );
        request.constraints.min_reliability = reliability_cap;
    }
    
    return request;
}

void SFCController::plan(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
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
        
        auto sfc_request = parse_sfc_request(*json);
        Topology topology = g_res_mgr->export_current_topology();

        if (sfc_request.vnfs.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid SFC request";
            error["details"] = "vnfs/vnf_sequence must not be empty";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        if (sfc_request.source_node.empty() || sfc_request.destination_node.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid SFC request";
            error["details"] = "source_node and destination_node are required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }
        if (sfc_request.source_node == sfc_request.destination_node) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid SFC request";
            error["details"] = "source_node and destination_node must be different";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        std::unordered_set<std::string> topo_nodes;
        topo_nodes.reserve(topology.nodes.size() * 2);
        for (const auto& node : topology.nodes) topo_nodes.insert(node.id);
        if (topo_nodes.find(sfc_request.source_node) == topo_nodes.end() ||
            topo_nodes.find(sfc_request.destination_node) == topo_nodes.end()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid SFC request";
            error["details"] = "source_node or destination_node not found in current topology";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        spdlog::info("Starting inference: nodes={}, links={}", 
                    topology.nodes.size(), topology.links.size());
        
        auto candidates = g_inference_engine->inference(sfc_request, topology);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        std::vector<DeploymentCandidate> feasible_candidates;
        std::vector<DeploymentCandidate> infeasible_candidates;
        feasible_candidates.reserve(candidates.size());
        infeasible_candidates.reserve(candidates.size());
        for (const auto& cand : candidates) {
            if (cand.satisfies_constraints) {
                feasible_candidates.push_back(cand);
            } else {
                infeasible_candidates.push_back(cand);
            }
        }
        std::sort(feasible_candidates.begin(), feasible_candidates.end(),
                  [](const DeploymentCandidate& a, const DeploymentCandidate& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.total_latency_ms < b.total_latency_ms;
                  });
        std::sort(infeasible_candidates.begin(), infeasible_candidates.end(),
                  [](const DeploymentCandidate& a, const DeploymentCandidate& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.total_latency_ms < b.total_latency_ms;
                  });

        if (feasible_candidates.empty() && infeasible_candidates.empty()) {
            spdlog::warn("No feasible deployment found for request {}", sfc_request.request_id);
            
            Json::Value response;
            response["request_id"] = sfc_request.request_id;
            response["service_type"] = sfc_request.service_type;
            response["source_node"] = sfc_request.source_node;
            response["destination_node"] = sfc_request.destination_node;
            response["inference_time_ms"] = static_cast<double>(duration.count());
            response["candidates"] = Json::Value(Json::arrayValue);
            response["error"] = "No feasible deployment found";
            response["details"] = "All generated candidates violate constraints";
            
            response["failure_reasons"] = "No SLA-feasible candidate returned by inference engine.";
            
            auto resp = HttpResponse::newHttpJsonResponse(response);
            resp->setStatusCode(k200OK);  // 返回200但标记为无可行方案
            callback(resp);
            return;
        }
        
        Json::Value response;
        response["request_id"] = sfc_request.request_id;
        response["service_type"] = sfc_request.service_type;
        response["source_node"] = sfc_request.source_node;
        response["destination_node"] = sfc_request.destination_node;
        response["inference_time_ms"] = static_cast<double>(duration.count());
        response["requested_topk"] = sfc_request.topk;
        response["deployable_count"] = static_cast<int>(feasible_candidates.size());
        response["fallback_only"] = feasible_candidates.empty();

        std::vector<DeploymentCandidate> response_candidates;
        if (!feasible_candidates.empty()) {
            response_candidates = feasible_candidates;
        } else {
            response_candidates = infeasible_candidates;
            response["warning"] =
                "当前未找到满足全部SLA的方案，已返回候选策略供人工决策（可强制部署）。";
        }

        response["returned_topk"] = static_cast<int>(response_candidates.size());
        if (!response["fallback_only"].asBool() &&
            static_cast<int>(response_candidates.size()) < sfc_request.topk) {
            response["warning"] = "Only " + std::to_string(response_candidates.size()) +
                                  " SLA-feasible candidates found; fewer than requested Top-" +
                                  std::to_string(sfc_request.topk) + ".";
        }
        
        Json::Value candidates_json(Json::arrayValue);
        for (auto candidate : response_candidates) {
            normalize_candidate_metrics_for_response(candidate, sfc_request);
            auto cand_json = candidate.to_json();
            auto violations = build_constraint_violations(candidate, sfc_request);
            cand_json["violation_details"] = violations;
            if (!candidate.satisfies_constraints && !violations.empty()) {
                cand_json["reason"] = violations.front();
            }
            candidates_json.append(nlohmann_to_jsoncpp(cand_json));
        }
        response["candidates"] = candidates_json;
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Inference failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Inference failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::deploy(
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
        
        std::string request_id = (*json).get("request_id", "").asString();
        (void)(*json).get("candidate_index", 0).asInt();
        
        // 解析候选方案和VNF信息
        DeploymentCandidate candidate;
        std::vector<VNF> vnfs;
        
        if (json->isMember("candidate")) {
            const auto& cand_json = (*json)["candidate"];
            
            candidate.score = cand_json.get("score", 0.0).asDouble();
            candidate.total_latency_ms = cand_json.get("total_latency_ms", 0.0).asDouble();
            
            if (cand_json.isMember("deployed_nodes")) {
                for (const auto& node : cand_json["deployed_nodes"]) {
                    candidate.deployed_nodes.push_back(node.asString());
                }
            }
            
            if (cand_json.isMember("per_vnf")) {
                for (const auto& pv_json : cand_json["per_vnf"]) {
                    DeploymentCandidate::PerVNF pv;
                    pv.vnf = pv_json.get("vnf", "").asString();
                    pv.node = pv_json.get("node", "").asString();
                    pv.cpu_used = pv_json.get("cpu_used", 0.0).asDouble();
                    pv.mem_used = pv_json.get("mem_used", 0.0).asDouble();
                    pv.disk_used = pv_json.get("disk_used", pv.mem_used * 2.0).asDouble();
                    candidate.per_vnf.push_back(pv);
                    
                    VNF vnf;
                    vnf.name = pv.vnf;
                    vnf.cpu = pv.cpu_used;
                    vnf.mem = pv.mem_used;
                    vnf.disk = pv.disk_used;
                    vnf.bw_in = 0.0;
                    vnf.bw_out = 0.0;
                    vnfs.push_back(vnf);
                }
            }
            
            if (cand_json.isMember("link_details")) {
                for (const auto& ld_json : cand_json["link_details"]) {
                    DeploymentCandidate::LinkDetail ld;
                    ld.src = ld_json.get("src", "").asString();
                    ld.dst = ld_json.get("dst", "").asString();
                    ld.latency_ms = ld_json.get("latency_ms", 0.0).asDouble();
                    ld.bandwidth_gbps = ld_json.get("bandwidth_gbps", 0.0).asDouble();
                    ld.bandwidth_available_gbps = ld_json.get("bandwidth_available_gbps", ld.bandwidth_gbps).asDouble();
                    ld.bandwidth_required_gbps = ld_json.get("bandwidth_required_gbps", 0.1).asDouble();
                    ld.status = ld_json.get("status", "active").asString();
                    ld.reliability = ld_json.get("reliability", 0.999).asDouble();
                    candidate.link_details.push_back(ld);
                }
            }
        }
        
        // 生成部署ID
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        std::string deployment_id = "deploy_" + std::to_string(timestamp);
        
        // 分配资源
        bool allocated = g_res_mgr->allocate_resources(deployment_id, candidate, vnfs);
        
        if (!allocated) {
            Json::Value error;
            error["code"] = 500;
            error["message"] = "Failed to allocate resources";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
            return;
        }
        
        // 保存更新后的拓扑
        auto updated_topology = g_res_mgr->export_current_topology();
        g_topo_mgr->save_current_topology(updated_topology);
        
        spdlog::info("✓ Deployment {} completed: {} VNFs on {} nodes",
                    deployment_id, candidate.per_vnf.size(), candidate.deployed_nodes.size());
        
        Json::Value response;
        response["deployment_id"] = deployment_id;
        response["status"] = "completed";
        response["message"] = "Deployment successful";
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Deploy failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "Deploy failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::rollback(
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
        
        std::string deployment_id = (*json).get("deployment_id", "").asString();
        
        bool success = g_res_mgr->release_resources(deployment_id);
        
        if (success) {
            auto updated_topology = g_res_mgr->export_current_topology();
            g_topo_mgr->save_current_topology(updated_topology);
            
            spdlog::info("✓ Deployment {} rolled back", deployment_id);
        }
        
        Json::Value response;
        response["status"] = success ? "success" : "failed";
        response["message"] = success ? "Deployment rolled back" : "Deployment not found";
        
        auto resp = HttpResponse::newHttpJsonResponse(response);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Rollback failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::getDeployments(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        std::lock_guard<std::mutex> lock(g_deployments_mutex);
        
        Json::Value deployments_json(Json::arrayValue);
        for (const auto& deployment : g_deployments) {
            deployments_json.append(nlohmann_to_jsoncpp(deployment.to_json()));
        }
        
        auto resp = HttpResponse::newHttpJsonResponse(deployments_json);
        callback(resp);
        
    } catch (const std::exception& e) {
        spdlog::error("Get deployments failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

} // namespace sfc
