#include "controllers/SFCController.h"
#include "utils/json_converter.h"
#include "websocket/WSHandler.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <mutex>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <sstream>
#include <limits>
#include <cmath>
#include <cctype>
#include <nlohmann/json.hpp>

namespace sfc {

// 全局部署列表
static std::vector<Deployment> g_deployments;
static std::mutex g_deployments_mutex;

struct CoreNFProfile {
    std::string nf_role;
    std::string resource_profile;
    double cpu_multiplier;
    double mem_multiplier;
    double disk_multiplier;
    double bw_multiplier;
    double min_cpu;
    double min_mem;
    double min_disk;
    double min_bw;
    CoreBusinessLoad business_weights;
};

static std::string normalize_nf_type(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '-' || ch == ' ') out.push_back('_');
        else out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

static CoreNFProfile get_core_nf_profile(const std::string& nf_type_raw) {
    const std::string nf_type = normalize_nf_type(nf_type_raw);
    if (nf_type == "amf") {
        return {"control_plane", "session-heavy", 1.35, 1.30, 1.20, 1.15, 1.2, 2.2, 8.0, 0.2, {0.82, 0.74, 0.22, 0.86, 0.52, 0.58}};
    }
    if (nf_type == "smf") {
        return {"control_plane", "policy-heavy", 1.45, 1.35, 1.30, 1.20, 1.5, 2.6, 10.0, 0.25, {0.70, 0.88, 0.42, 0.58, 0.86, 0.54}};
    }
    if (nf_type == "upf") {
        return {"user_plane", "throughput-heavy", 1.80, 1.55, 1.60, 1.80, 2.0, 3.0, 16.0, 0.45, {0.36, 0.62, 0.94, 0.48, 0.44, 0.30}};
    }
    if (nf_type == "nrf") {
        return {"control_plane", "registry", 1.15, 1.25, 1.20, 1.05, 0.9, 1.8, 6.0, 0.12, {0.92, 0.54, 0.14, 0.46, 0.58, 0.42}};
    }
    if (nf_type == "ausf") {
        return {"control_plane", "auth", 1.20, 1.20, 1.15, 1.10, 0.8, 1.6, 5.0, 0.1, {0.68, 0.46, 0.10, 0.52, 0.50, 0.94}};
    }
    if (nf_type == "udm" || nf_type == "udr") {
        return {"control_plane", "data-plane-db", 1.25, 1.35, 1.65, 1.10, 1.0, 2.0, 20.0, 0.1, {0.56, 0.72, 0.18, 0.58, 0.70, 0.82}};
    }
    if (nf_type == "pcf") {
        return {"control_plane", "policy", 1.30, 1.30, 1.20, 1.10, 1.0, 2.0, 8.0, 0.1, {0.62, 0.78, 0.16, 0.48, 0.94, 0.56}};
    }
    if (nf_type == "nssf") {
        return {"control_plane", "slice-selection", 1.15, 1.15, 1.15, 1.05, 0.8, 1.5, 5.0, 0.08, {0.64, 0.58, 0.18, 0.72, 0.76, 0.42}};
    }
    if (nf_type == "scp") {
        return {"control_plane", "service-communication", 1.24, 1.22, 1.16, 1.18, 0.9, 1.6, 6.0, 0.12, {0.88, 0.62, 0.20, 0.40, 0.62, 0.40}};
    }
    if (nf_type == "sepp") {
        return {"control_plane", "inter-plmn-security", 1.28, 1.25, 1.20, 1.16, 1.0, 1.8, 7.0, 0.12, {0.86, 0.56, 0.16, 0.36, 0.72, 0.88}};
    }
    if (nf_type == "bsf") {
        return {"control_plane", "binding-support", 1.18, 1.18, 1.14, 1.08, 0.8, 1.4, 5.0, 0.08, {0.60, 0.70, 0.12, 0.46, 0.84, 0.48}};
    }
    return {"control_plane", "standard", 1.25, 1.20, 1.20, 1.15, 0.8, 1.5, 6.0, 0.1, {0.56, 0.56, 0.32, 0.52, 0.54, 0.52}};
}

static double clamp01(double x) {
    return std::max(0.0, std::min(1.0, x));
}

static CoreBusinessLoad parse_core_business_load_object(
    const Json::Value& obj,
    const CoreBusinessLoad& fallback
) {
    CoreBusinessLoad load = fallback;
    if (!obj.isObject()) {
        load.normalize_inplace();
        return load;
    }
    load.signaling_load = obj.get("signaling_load", obj.get("signaling", load.signaling_load)).asDouble();
    load.session_load = obj.get("session_load", obj.get("session", load.session_load)).asDouble();
    load.user_plane_load = obj.get("user_plane_load", obj.get("user_plane", obj.get("throughput", load.user_plane_load))).asDouble();
    load.mobility_load = obj.get("mobility_load", obj.get("mobility", load.mobility_load)).asDouble();
    load.policy_load = obj.get("policy_load", obj.get("policy", load.policy_load)).asDouble();
    load.auth_load = obj.get("auth_load", obj.get("auth", load.auth_load)).asDouble();
    load.normalize_inplace();
    return load;
}

static CoreBusinessLoad apply_nf_business_weights(
    const CoreBusinessLoad& request_load,
    const CoreBusinessLoad& nf_weights
) {
    CoreBusinessLoad out;
    out.signaling_load = clamp01(request_load.signaling_load * nf_weights.signaling_load);
    out.session_load = clamp01(request_load.session_load * nf_weights.session_load);
    out.user_plane_load = clamp01(request_load.user_plane_load * nf_weights.user_plane_load);
    out.mobility_load = clamp01(request_load.mobility_load * nf_weights.mobility_load);
    out.policy_load = clamp01(request_load.policy_load * nf_weights.policy_load);
    out.auth_load = clamp01(request_load.auth_load * nf_weights.auth_load);
    out.normalize_inplace();
    return out;
}

static VNF parse_nf_spec_from_json(
    const Json::Value& nf_json,
    size_t index,
    const CoreBusinessLoad& request_business_load
) {
    VNF vnf;
    vnf.name = nf_json.get("core_nf_id", nf_json.get("nf_id", nf_json.get("vnf_id", nf_json.get("name", "")))).asString();
    if (vnf.name.empty()) {
        vnf.name = "core-nf-" + std::to_string(index + 1);
    }
    vnf.nf_type = nf_json.get(
        "core_nf_type",
        nf_json.get("nf_type", nf_json.get("vnf_type", nf_json.get("type", vnf.name)))
    ).asString();
    if (vnf.nf_type.empty()) {
        vnf.nf_type = vnf.name;
    }

    const CoreNFProfile profile = get_core_nf_profile(vnf.nf_type);
    vnf.nf_role = nf_json.get("nf_role", profile.nf_role).asString();
    vnf.resource_profile = nf_json.get("resource_profile", profile.resource_profile).asString();
    vnf.processing_weight = nf_json.get("processing_weight", 1.0).asDouble();
    vnf.stateful = nf_json.get("stateful", true).asBool();

    const double cpu_raw = nf_json.get("cpu_required", nf_json.get("cpu", 1.0)).asDouble();
    const double mem_raw = nf_json.get("mem_required", nf_json.get("mem", 1.0)).asDouble();
    const double disk_default = std::max(2.0, mem_raw * 2.2);
    const double disk_raw = nf_json.get("disk_required_gb", nf_json.get("disk", disk_default)).asDouble();
    const double bw_raw = nf_json.get(
        "bandwidth_required_gbps",
        nf_json.get("bw_required", std::max(nf_json.get("bw_in", 0.1).asDouble(), nf_json.get("bw_out", 0.1).asDouble()))
    ).asDouble();

    vnf.cpu = std::max(profile.min_cpu, cpu_raw * profile.cpu_multiplier);
    vnf.mem = std::max(profile.min_mem, mem_raw * profile.mem_multiplier);
    vnf.disk = std::max(profile.min_disk, disk_raw * profile.disk_multiplier);
    const double bw = std::max(profile.min_bw, bw_raw * profile.bw_multiplier);
    vnf.bw_in = nf_json.isMember("bw_in") ? nf_json["bw_in"].asDouble() : bw;
    vnf.bw_out = nf_json.isMember("bw_out") ? nf_json["bw_out"].asDouble() : bw;

    if (nf_json.isMember("business_load_demand")) {
        vnf.business_load_demand = parse_core_business_load_object(
            nf_json["business_load_demand"],
            request_business_load
        );
    } else if (nf_json.isMember("business_load_weights")) {
        const CoreBusinessLoad weights = parse_core_business_load_object(
            nf_json["business_load_weights"],
            profile.business_weights
        );
        vnf.business_load_demand = apply_nf_business_weights(request_business_load, weights);
    } else {
        vnf.business_load_demand = apply_nf_business_weights(request_business_load, profile.business_weights);
    }
    return vnf;
}

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

static bool parse_candidate_from_json(const Json::Value& cand_json, DeploymentCandidate* out) {
    if (!out || !cand_json.isObject()) return false;
    DeploymentCandidate cand{};
    cand.score = cand_json.get("score", 0.0).asDouble();
    cand.total_latency_ms = cand_json.get("total_latency_ms", 0.0).asDouble();
    cand.estimated_reliability = cand_json.get("estimated_reliability", 0.0).asDouble();
    cand.bottleneck_bandwidth_gbps = cand_json.get("bottleneck_bandwidth_gbps", 0.0).asDouble();
    cand.satisfies_constraints = cand_json.get("satisfies_constraints", true).asBool();
    cand.reason = cand_json.get("reason", "").asString();

    if (cand_json.isMember("deployed_nodes")) {
        for (const auto& node : cand_json["deployed_nodes"]) {
            cand.deployed_nodes.push_back(node.asString());
        }
    }
    const Json::Value& per_nf_json = cand_json.isMember("per_core_nf")
        ? cand_json["per_core_nf"]
        : cand_json["per_vnf"];
    if (per_nf_json.isArray()) {
        for (const auto& pv_json : per_nf_json) {
            DeploymentCandidate::PerVNF pv;
            pv.vnf = pv_json.get("vnf", pv_json.get("core_nf", "")).asString();
            pv.core_nf = pv_json.get("core_nf", pv.vnf).asString();
            pv.nf_type = pv_json.get("nf_type", pv_json.get("core_nf_type", pv.vnf)).asString();
            pv.nf_role = pv_json.get("nf_role", "control_plane").asString();
            pv.node = pv_json.get("node", "").asString();
            pv.cpu_used = pv_json.get("cpu_used", 0.0).asDouble();
            pv.mem_used = pv_json.get("mem_used", 0.0).asDouble();
            pv.disk_used = pv_json.get("disk_used", pv.mem_used * 2.0).asDouble();
            cand.per_vnf.push_back(std::move(pv));
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
            ld.bandwidth_required_gbps = ld_json.get("bandwidth_required_gbps", 0.0).asDouble();
            ld.status = ld_json.get("status", "active").asString();
            ld.reliability = ld_json.get("reliability", 0.999).asDouble();
            cand.link_details.push_back(std::move(ld));
        }
    }
    *out = std::move(cand);
    return !out->deployed_nodes.empty() || !out->per_vnf.empty() || !out->link_details.empty();
}

SFCRequest SFCController::parse_sfc_request(const Json::Value& json) {
    SFCRequest request;
    
    request.request_id = json.get("request_id", "").asString();
    request.service_type = json.get("service_type", "custom_service").asString();
    request.network_domain = json.get("network_domain", json.get("domain", "open5gs")).asString();
    request.source_node = json.isMember("source_node")
        ? json["source_node"].asString()
        : json.get("ingress_node", "").asString();
    request.destination_node = json.isMember("destination_node")
        ? json["destination_node"].asString()
        : json.get("egress_node", "").asString();
    request.priority = json.get("priority", "medium").asString();
    request.optimize = json.get("optimize", "latency").asString();
    request.topk = json.get("topk", 3).asInt();
    request.topology_version = json.get("topology_version", -1).asInt();
    request.sim_time = json.get("sim_time", "").asString();
    request.core_business_load = CoreBusinessLoad{};
    if (json.isMember("core_business_load")) {
        request.core_business_load = parse_core_business_load_object(
            json["core_business_load"],
            request.core_business_load
        );
    }
    request.realtime_mode = json.get("realtime_mode", false).asBool();
    request.max_planning_attempts = json.get("max_planning_attempts", 0).asInt();
    request.planning_time_budget_ms = json.get("planning_time_budget_ms", 0.0).asDouble();
    if (json.isMember("inference")) {
        const auto& inference = json["inference"];
        request.realtime_mode = inference.get("realtime_mode", request.realtime_mode).asBool();
        request.max_planning_attempts = inference.get("max_planning_attempts", request.max_planning_attempts).asInt();
        request.planning_time_budget_ms = inference.get("planning_time_budget_ms", request.planning_time_budget_ms).asDouble();
    }

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
    
    const Json::Value* nf_array = nullptr;
    if (json.isMember("core_nfs")) {
        nf_array = &json["core_nfs"];
    } else if (json.isMember("core_nf_sequence")) {
        nf_array = &json["core_nf_sequence"];
    } else if (json.isMember("vnfs")) {
        nf_array = &json["vnfs"];
    } else if (json.isMember("vnf_sequence")) {
        nf_array = &json["vnf_sequence"];
    }
    if (nf_array && nf_array->isArray()) {
        size_t idx = 0;
        for (const auto& nf_json : *nf_array) {
            request.vnfs.push_back(parse_nf_spec_from_json(nf_json, idx, request.core_business_load));
            ++idx;
        }
    }

    request.topk = std::max(1, std::min(32, request.topk));
    request.core_business_load.normalize_inplace();
    request.max_planning_attempts = std::max(0, std::min(2000, request.max_planning_attempts));
    request.planning_time_budget_ms = std::max(0.0, std::min(30000.0, request.planning_time_budget_ms));
    request.constraints.max_latency_ms = std::max(10.0, request.constraints.max_latency_ms);
    request.constraints.min_bandwidth_gbps = std::max(0.01, request.constraints.min_bandwidth_gbps);
    request.constraints.min_reliability = std::max(0.72, std::min(0.995, request.constraints.min_reliability));

    double reliability_cap = 0.95;
    if (request.vnfs.size() >= 4) {
        reliability_cap = 0.88;
    } else if (request.vnfs.size() >= 2) {
        reliability_cap = 0.91;
    }
    if (request.constraints.min_reliability > reliability_cap) {
        spdlog::warn(
            "Request {} reliability {:.4f} too strict for {} core NFs, capped to {:.4f}",
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
            error["details"] = "core_nfs/core_nf_sequence (or vnfs/vnf_sequence) must not be empty";
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
        
        nlohmann::json decision_process;
        auto candidates = g_inference_engine->inference(sfc_request, topology, &decision_process);
        
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
            response["core_business_load"] = nlohmann_to_jsoncpp(sfc_request.core_business_load.to_json());
            
            response["failure_reasons"] = "No SLA-feasible candidate returned by inference engine.";
            response["topology_version"] = topology.metadata.topology_version;
            response["sim_time"] = topology.metadata.sim_time;
            response["decision_process"] = nlohmann_to_jsoncpp(decision_process);

            WSHandler::broadcast_json({
                {"type", "decision_trace"},
                {"mode", "single_request"},
                {"request_id", sfc_request.request_id},
                {"topology_version", topology.metadata.topology_version},
                {"sim_time", topology.metadata.sim_time},
                {"inference_time_ms", static_cast<double>(duration.count())},
                {"candidate_count", 0},
                {"message", "No candidate returned by inference engine"},
                {"decision_process", decision_process}
            });
            
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
        response["topology_version"] = topology.metadata.topology_version;
        response["sim_time"] = topology.metadata.sim_time;
        response["requested_topk"] = sfc_request.topk;
        response["deployable_count"] = static_cast<int>(feasible_candidates.size());
        response["fallback_only"] = feasible_candidates.empty();
        response["decision_process"] = nlohmann_to_jsoncpp(decision_process);
        response["core_business_load"] = nlohmann_to_jsoncpp(sfc_request.core_business_load.to_json());

        std::vector<DeploymentCandidate> response_candidates;
        if (!feasible_candidates.empty()) {
            response_candidates = feasible_candidates;
        } else {
            response_candidates = infeasible_candidates;
            response["warning"] =
                "当前未找到满足全部SLA的可部署方案，以下候选仅用于问题定位与原因分析。";
        }

        response["returned_topk"] = static_cast<int>(response_candidates.size());
        if (!response["fallback_only"].asBool() &&
            static_cast<int>(response_candidates.size()) < sfc_request.topk) {
            response["warning"] = "Only " + std::to_string(response_candidates.size()) +
                                  " SLA-feasible candidates found; fewer than requested Top-" +
                                  std::to_string(sfc_request.topk) + ".";
        }
        
        Json::Value candidates_json(Json::arrayValue);
        nlohmann::json trace_candidates = nlohmann::json::array();
        for (auto candidate : response_candidates) {
            normalize_candidate_metrics_for_response(candidate, sfc_request);
            auto cand_json = candidate.to_json();
            auto violations = build_constraint_violations(candidate, sfc_request);
            cand_json["violation_details"] = violations;
            if (!candidate.satisfies_constraints && !violations.empty()) {
                cand_json["reason"] = violations.front();
            }
            candidates_json.append(nlohmann_to_jsoncpp(cand_json));

            nlohmann::json trace_per_vnf = nlohmann::json::array();
            for (const auto& pv : candidate.per_vnf) {
                trace_per_vnf.push_back({
                    {"vnf", pv.vnf},
                    {"core_nf", pv.core_nf.empty() ? pv.vnf : pv.core_nf},
                    {"nf_type", pv.nf_type},
                    {"nf_role", pv.nf_role},
                    {"node", pv.node},
                    {"cpu_used", pv.cpu_used},
                    {"mem_used", pv.mem_used},
                    {"disk_used", pv.disk_used}
                });
            }
            nlohmann::json trace_link_details = nlohmann::json::array();
            for (const auto& ld : candidate.link_details) {
                trace_link_details.push_back({
                    {"src", ld.src},
                    {"dst", ld.dst},
                    {"latency_ms", ld.latency_ms},
                    {"bandwidth_gbps", ld.bandwidth_gbps},
                    {"bandwidth_available_gbps", ld.bandwidth_available_gbps},
                    {"bandwidth_required_gbps", ld.bandwidth_required_gbps},
                    {"status", ld.status},
                    {"reliability", ld.reliability}
                });
            }

            trace_candidates.push_back({
                {"score", candidate.score},
                {"satisfies_constraints", candidate.satisfies_constraints},
                {"total_latency_ms", candidate.total_latency_ms},
                {"estimated_reliability", candidate.estimated_reliability},
                {"bottleneck_bandwidth_gbps", candidate.bottleneck_bandwidth_gbps},
                {"deployed_nodes", candidate.deployed_nodes},
                {"per_vnf", trace_per_vnf},
                {"per_core_nf", trace_per_vnf},
                {"link_details", trace_link_details},
                {"reason", candidate.reason}
            });
        }
        response["candidates"] = candidates_json;

        nlohmann::json trace_request_vnfs = nlohmann::json::array();
        for (const auto& vnf : sfc_request.vnfs) {
            trace_request_vnfs.push_back({
                {"name", vnf.name},
                {"core_nf", vnf.name},
                {"nf_type", vnf.nf_type.empty() ? vnf.name : vnf.nf_type},
                {"nf_role", vnf.nf_role},
                {"resource_profile", vnf.resource_profile},
                {"processing_weight", vnf.processing_weight},
                {"stateful", vnf.stateful},
                {"cpu", vnf.cpu},
                {"mem", vnf.mem},
                {"disk", vnf.disk},
                {"bw_in", vnf.bw_in},
                {"bw_out", vnf.bw_out},
                {"business_load_demand", vnf.business_load_demand.to_json()}
            });
        }
        response["request_core_nfs"] = nlohmann_to_jsoncpp(trace_request_vnfs);

        WSHandler::broadcast_json({
            {"type", "decision_trace"},
            {"mode", "single_request"},
            {"request_id", sfc_request.request_id},
            {"topology_version", topology.metadata.topology_version},
            {"sim_time", topology.metadata.sim_time},
            {"source_node", sfc_request.source_node},
            {"destination_node", sfc_request.destination_node},
            {"core_business_load", sfc_request.core_business_load.to_json()},
            {"inference_time_ms", static_cast<double>(duration.count())},
            {"requested_topk", sfc_request.topk},
            {"returned_topk", static_cast<int>(response_candidates.size())},
            {"deployable_count", static_cast<int>(feasible_candidates.size())},
            {"fallback_only", feasible_candidates.empty()},
            {"request_vnfs", trace_request_vnfs},
            {"request_core_nfs", trace_request_vnfs},
            {"candidates", trace_candidates},
            {"decision_process", decision_process}
        });
        
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
        
        // 解析候选方案和核心网网元信息（兼容旧VNF字段）
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
            
            const Json::Value& per_nf_json = cand_json.isMember("per_core_nf")
                ? cand_json["per_core_nf"]
                : cand_json["per_vnf"];
            if (per_nf_json.isArray()) {
                for (const auto& pv_json : per_nf_json) {
                    DeploymentCandidate::PerVNF pv;
                    pv.vnf = pv_json.get("vnf", pv_json.get("core_nf", "")).asString();
                    pv.core_nf = pv_json.get("core_nf", pv.vnf).asString();
                    pv.nf_type = pv_json.get("nf_type", pv_json.get("core_nf_type", pv.vnf)).asString();
                    pv.nf_role = pv_json.get("nf_role", "control_plane").asString();
                    pv.node = pv_json.get("node", "").asString();
                    pv.cpu_used = pv_json.get("cpu_used", 0.0).asDouble();
                    pv.mem_used = pv_json.get("mem_used", 0.0).asDouble();
                    pv.disk_used = pv_json.get("disk_used", pv.mem_used * 2.0).asDouble();
                    candidate.per_vnf.push_back(pv);
                    
                    VNF vnf;
                    vnf.name = pv.core_nf.empty() ? pv.vnf : pv.core_nf;
                    vnf.nf_type = pv.nf_type.empty() ? vnf.name : pv.nf_type;
                    vnf.nf_role = pv.nf_role.empty() ? "control_plane" : pv.nf_role;
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
        
        spdlog::info("✓ Deployment {} completed: {} core NFs on {} nodes",
                    deployment_id, candidate.per_vnf.size(), candidate.deployed_nodes.size());

        {
            std::lock_guard<std::mutex> lock(g_deployments_mutex);
            Deployment dep;
            dep.deployment_id = deployment_id;
            dep.request_id = request_id;
            dep.candidate_index = (*json).get("candidate_index", 0).asInt();
            dep.status = "completed";
            dep.deployed_nodes = candidate.deployed_nodes;
            dep.total_latency_ms = candidate.total_latency_ms;
            dep.progress = 100;
            auto now_ts = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now_ts);
            char buf[100];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t));
            dep.deployed_at = buf;
            for (const auto& pv : candidate.per_vnf) {
                VNFDeployment d;
                d.vnf_id = pv.vnf;
                d.vnf_type = pv.nf_type.empty() ? pv.vnf : pv.nf_type;
                d.core_nf_id = pv.core_nf.empty() ? pv.vnf : pv.core_nf;
                d.core_nf_type = pv.nf_type.empty() ? d.core_nf_id : pv.nf_type;
                d.nf_role = pv.nf_role.empty() ? "control_plane" : pv.nf_role;
                d.resource_profile = get_core_nf_profile(d.core_nf_type).resource_profile;
                d.node = pv.node;
                d.cpu_used = pv.cpu_used;
                d.mem_used = pv.mem_used;
                d.disk_used = pv.disk_used;
                d.sfc_id = request_id;
                dep.per_vnf.push_back(std::move(d));
            }
            g_deployments.push_back(std::move(dep));
        }
        
        Json::Value response;
        response["deployment_id"] = deployment_id;
        response["status"] = "completed";
        response["message"] = "Deployment successful";

        WSHandler::broadcast_json({
            {"type", "deployment_update"},
            {"deployment_id", deployment_id},
            {"request_id", request_id},
            {"status", "completed"},
            {"progress", 100},
            {"topology_version", updated_topology.metadata.topology_version},
            {"sim_time", updated_topology.metadata.sim_time}
        });
        
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

            std::lock_guard<std::mutex> lock(g_deployments_mutex);
            g_deployments.erase(
                std::remove_if(
                    g_deployments.begin(),
                    g_deployments.end(),
                    [&](const Deployment& dep) { return dep.deployment_id == deployment_id; }
                ),
                g_deployments.end()
            );
        }
        
        Json::Value response;
        response["status"] = success ? "success" : "failed";
        response["message"] = success ? "Deployment rolled back" : "Deployment not found";

        WSHandler::broadcast_json({
            {"type", "deployment_update"},
            {"deployment_id", deployment_id},
            {"status", success ? "rolled_back" : "rollback_failed"},
            {"progress", success ? 100 : 0}
        });
        
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

void SFCController::startSession(
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
        if (!g_dynamic_inference) {
            Json::Value error;
            error["code"] = 500;
            error["message"] = "Dynamic inference service unavailable";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
            return;
        }

        const bool auto_redeploy = (*json).get("auto_redeploy", true).asBool();
        const std::string initial_deployment_id = (*json).get("initial_deployment_id", "").asString();
        const Json::Value request_json = json->isMember("request") ? (*json)["request"] : (*json);
        double initial_inference_time_ms = -1.0;
        if ((*json).isMember("initial_inference_time_ms")) {
            initial_inference_time_ms = (*json)["initial_inference_time_ms"].asDouble();
        } else if (request_json.isMember("initial_inference_time_ms")) {
            initial_inference_time_ms = request_json["initial_inference_time_ms"].asDouble();
        }
        auto sfc_request = parse_sfc_request(request_json);
        if (sfc_request.vnfs.empty() || sfc_request.source_node.empty() || sfc_request.destination_node.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "Invalid session request";
            error["details"] = "source_node/destination_node and core_nfs are required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        DeploymentCandidate initial_candidate;
        bool has_initial_candidate = false;
        if (request_json.isMember("initial_candidate")) {
            has_initial_candidate = parse_candidate_from_json(request_json["initial_candidate"], &initial_candidate);
        } else if (json->isMember("initial_candidate")) {
            has_initial_candidate = parse_candidate_from_json((*json)["initial_candidate"], &initial_candidate);
        }

        auto result = g_dynamic_inference->start_session(
            sfc_request,
            auto_redeploy,
            has_initial_candidate ? &initial_candidate : nullptr,
            initial_deployment_id,
            initial_inference_time_ms
        );
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(result));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("startSession failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "startSession failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::stopSession(
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
        const std::string session_id = (*json).get("session_id", "").asString();
        if (session_id.empty()) {
            Json::Value error;
            error["code"] = 400;
            error["message"] = "session_id is required";
            auto resp = HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }
        const bool ok = g_dynamic_inference && g_dynamic_inference->stop_session(session_id);
        Json::Value result;
        result["ok"] = ok;
        result["session_id"] = session_id;
        auto resp = HttpResponse::newHttpJsonResponse(result);
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("stopSession failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "stopSession failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::listSessions(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        nlohmann::json sessions = g_dynamic_inference ? g_dynamic_inference->list_sessions() : nlohmann::json::array();
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(sessions));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("listSessions failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "listSessions failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::getSessionStatus(
    const HttpRequestPtr&,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& session_id
) {
    try {
        nlohmann::json status = g_dynamic_inference
            ? g_dynamic_inference->get_session_status(session_id)
            : nlohmann::json{{"found", false}, {"session_id", session_id}};
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(status));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("getSessionStatus failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "getSessionStatus failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void SFCController::recomputeSession(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& session_id
) {
    try {
        auto json = req->getJsonObject();
        const std::string trigger = json ? (*json).get("trigger", "manual").asString() : "manual";
        nlohmann::json result = g_dynamic_inference
            ? g_dynamic_inference->force_recompute(session_id, trigger)
            : nlohmann::json{{"ok", false}, {"session_id", session_id}, {"reason", "service_unavailable"}};
        auto resp = HttpResponse::newHttpJsonResponse(nlohmann_to_jsoncpp(result));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::error("recomputeSession failed: {}", e.what());
        Json::Value error;
        error["code"] = 500;
        error["message"] = "recomputeSession failed";
        error["details"] = e.what();
        auto resp = HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

} // namespace sfc
