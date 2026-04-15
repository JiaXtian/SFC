#pragma once
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace sfc {

// 轨道参数
struct OrbitalParams {
    int plane;
    int position_in_plane;
    double raan;
    double true_anomaly;
    double altitude_km;
    double inclination_deg = 53.0;
    
    json to_json() const {
        return {
            {"plane", plane},
            {"position_in_plane", position_in_plane},
            {"raan", raan},
            {"true_anomaly", true_anomaly},
            {"altitude_km", altitude_km},
            {"inclination_deg", inclination_deg},
            {"inclination", inclination_deg}
        };
    }
};

// 坐标
struct Coordinates {
    double x, y, z;
    double lat, lon;  // 经纬度
    
    json to_json() const {
        return {
            {"x", x}, {"y", y}, {"z", z},
            {"lat", lat}, {"lon", lon}
        };
    }
};

// 核心网网元部署信息（兼容旧VNF字段）
struct VNFDeployment {
    std::string vnf_id;
    std::string vnf_type;
    std::string core_nf_id;
    std::string core_nf_type;
    std::string nf_role;
    std::string resource_profile;
    std::string node;
    double cpu_used;
    double mem_used;
    double disk_used;
    std::string sfc_id;
    
    json to_json() const {
        return {
            {"vnf_id", vnf_id},
            {"vnf_type", vnf_type},
            {"core_nf_id", core_nf_id.empty() ? vnf_id : core_nf_id},
            {"core_nf_type", core_nf_type.empty() ? vnf_type : core_nf_type},
            {"nf_type", core_nf_type.empty() ? vnf_type : core_nf_type},
            {"nf_role", nf_role},
            {"resource_profile", resource_profile},
            {"node", node},
            {"cpu_used", cpu_used},
            {"mem_used", mem_used},
            {"disk_used", disk_used},
            {"sfc_id", sfc_id}
        };
    }
};

// 卫星节点
struct Satellite {
    std::string id;
    std::string type = "satellite";
    std::string template_id = "starlink_v1";
    OrbitalParams orbital_params;
    Coordinates coordinates;
    double cpu_total;
    double cpu_available;
    double mem_total;
    double mem_available;
    double disk_total;
    double disk_available;
    double core_network_load = 0.5;
    double node_reliability = 0.98;
    std::string status = "active";  // active, down
    std::string fault_tag = "";
    bool fault_injected = false;
    std::string podman_container_name = "";
    std::string podman_container_id = "";
    std::string podman_status = "not_created"; // running, stopped, exited, not_created, unknown
    std::string deployment_state = "none";     // none, deployed, rolled_back
    std::string deployment_detail = "";
    bool core_nf_policy_applied = false;
    std::string core_nf_policy = "";
    double cpu_utilization_ratio = 0.0;
    double mem_utilization_ratio = 0.0;
    double disk_utilization_ratio = 0.0;
    std::string last_collected_at = "";
    std::vector<VNFDeployment> vnfs;
    
    json to_json() const {
        json vnfs_json = json::array();
        for (const auto& vnf : vnfs) {
            vnfs_json.push_back(vnf.to_json());
        }
        
        return {
            {"id", id},
            {"type", type},
            {"template_id", template_id},
            {"orbital_params", orbital_params.to_json()},
            {"coordinates", coordinates.to_json()},
            {"cpu_total", cpu_total},
            {"cpu_available", cpu_available},
            {"mem_total", mem_total},
            {"mem_available", mem_available},
            {"disk_total", disk_total},
            {"disk_available", disk_available},
            {"core_network_load", core_network_load},
            {"node_reliability", node_reliability},
            {"status", status},
            {"fault_tag", fault_tag},
            {"fault_injected", fault_injected},
            {"podman", {
                {"container_name", podman_container_name},
                {"container_id", podman_container_id},
                {"status", podman_status}
            }},
            {"deployment_state", deployment_state},
            {"deployment_detail", deployment_detail},
            {"core_nf_policy_applied", core_nf_policy_applied},
            {"core_nf_policy", core_nf_policy},
            {"telemetry", {
                {"cpu_utilization_ratio", cpu_utilization_ratio},
                {"mem_utilization_ratio", mem_utilization_ratio},
                {"disk_utilization_ratio", disk_utilization_ratio},
                {"last_collected_at", last_collected_at}
            }},
            {"vnfs", vnfs_json},
            {"core_nfs", vnfs_json}
        };
    }
};

// 链路
struct Link {
    std::string source;
    std::string target;
    std::string link_type;  // intra_orbit, inter_orbit, isl
    std::string status = "active"; // active, congested, down
    std::string fault_tag = "";
    double latency_ms;
    double reliability = 0.999;
    double bandwidth_gbps;
    double bandwidth_available_gbps;
    
    json to_json() const {
        const int link_status = (status == "down") ? 0 : 1;
        return {
            {"source", source},
            {"target", target},
            {"link_type", link_type},
            {"status", status},
            {"fault_tag", fault_tag},
            {"link_status", link_status},
            {"latency_ms", latency_ms},
            {"reliability", reliability},
            {"link_reliability", reliability},
            {"bandwidth_gbps", bandwidth_gbps},
            {"bandwidth_available_gbps", bandwidth_available_gbps}
        };
    }
};

// 拓扑
struct Topology {
    struct Metadata {
        int total_sats;
        int num_planes;
        double altitude_km;
        double inclination_deg;
        int topology_version = 0;
        double sampling_interval_sec = 5.0;
        std::string sim_time = "";
        std::string timestamp;
        
        json to_json() const {
            return {
                {"total_sats", total_sats},
                {"num_planes", num_planes},
                {"altitude_km", altitude_km},
                {"inclination_deg", inclination_deg},
                {"topology_version", topology_version},
                {"sampling_interval_sec", sampling_interval_sec},
                {"sim_time", sim_time},
                {"timestamp", timestamp}
            };
        }
    } metadata;
    
    std::vector<Satellite> nodes;
    std::vector<Link> links;
    
    json to_json() const {
        json nodes_json = json::array();
        for (const auto& node : nodes) {
            nodes_json.push_back(node.to_json());
        }
        
        json links_json = json::array();
        for (const auto& link : links) {
            links_json.push_back(link.to_json());
        }
        
        return {
            {"metadata", metadata.to_json()},
            {"topology", {
                {"nodes", nodes_json},
                {"links", links_json}
            }}
        };
    }
};

// 拓扑时序快照（动态仿真统一口径）
struct TopologySnapshot {
    std::string sim_time;
    int topology_version = 0;
    double sampling_interval_sec = 5.0;
    Topology topology;
    struct {
        int total_nodes = 0;
        int active_nodes = 0;
        int down_nodes = 0;
        int total_links = 0;
        int active_links = 0;
        int down_links = 0;
        int congested_links = 0;
        double avg_latency_ms = 0.0;
        double avg_bandwidth_utilization = 0.0;
    } metrics;

    json to_json() const {
        return {
            {"sim_time", sim_time},
            {"topology_version", topology_version},
            {"sampling_interval_sec", sampling_interval_sec},
            {"topology", topology.to_json()["topology"]},
            {"metadata", topology.metadata.to_json()},
            {"metrics", {
                {"total_nodes", metrics.total_nodes},
                {"active_nodes", metrics.active_nodes},
                {"down_nodes", metrics.down_nodes},
                {"total_links", metrics.total_links},
                {"active_links", metrics.active_links},
                {"down_links", metrics.down_links},
                {"congested_links", metrics.congested_links},
                {"avg_latency_ms", metrics.avg_latency_ms},
                {"avg_bandwidth_utilization", metrics.avg_bandwidth_utilization}
            }}
        };
    }
};

// 核心网网元定义（兼容旧VNF命名）
struct VNF {
    std::string name;
    std::string nf_type;
    std::string nf_role = "control_plane";
    std::string resource_profile = "standard";
    double processing_weight = 1.0;
    bool stateful = true;
    double cpu;
    double mem;
    double disk;
    double bw_in;
    double bw_out;
};

// SFC请求
struct SFCRequest {
    std::string request_id;
    std::string service_type = "custom_service";
    std::string network_domain = "open5gs";
    std::string source_node;
    std::string destination_node;
    std::string priority = "medium";
    std::vector<VNF> vnfs;
    struct {
        double max_latency_ms;
        double min_bandwidth_gbps;
        double min_reliability;
    } constraints;
    std::string optimize;  // latency, resource, load_balance
    int topk;
    int topology_version = -1;
    std::string sim_time = "";
    double core_network_load = 0.5;
    double priority_weight = 1.0;
    std::string load_level = "medium"; // low, medium, high
    bool realtime_mode = false;
    int max_planning_attempts = 0;
    double planning_time_budget_ms = 0.0;
    struct {
        double latency = -1.0;
        double resource = -1.0;
        double reliability = -1.0;
        double bandwidth = -1.0;
        double dispersion = -1.0;
    } score_weights;
};

// 部署候选
struct DeploymentCandidate {
    double score;
    std::vector<std::string> deployed_nodes;
    struct PerVNF {
        std::string vnf;
        std::string core_nf;
        std::string nf_type;
        std::string nf_role;
        std::string node;
        double cpu_used;
        double mem_used;
        double disk_used;
        
        json to_json() const {
            const std::string resolved_core_nf = core_nf.empty() ? vnf : core_nf;
            const std::string resolved_nf_type = nf_type.empty() ? resolved_core_nf : nf_type;
            return {
                {"vnf", vnf},
                {"core_nf", resolved_core_nf},
                {"nf_type", resolved_nf_type},
                {"nf_role", nf_role},
                {"node", node},
                {"cpu_used", cpu_used},
                {"mem_used", mem_used},
                {"disk_used", disk_used}
            };
        }
    };
    std::vector<PerVNF> per_vnf;
    double total_latency_ms;
    struct LinkDetail {
        std::string src;
        std::string dst;
        double latency_ms;
        double bandwidth_gbps;
        double bandwidth_available_gbps;
        double bandwidth_required_gbps;
        std::string status;
        double reliability;
        
        json to_json() const {
            return {
                {"src", src},
                {"dst", dst},
                {"latency_ms", latency_ms},
                {"bandwidth_gbps", bandwidth_gbps},
                {"bandwidth_available_gbps", bandwidth_available_gbps},
                {"bandwidth_required_gbps", bandwidth_required_gbps},
                {"status", status},
                {"reliability", reliability}
            };
        }
    };
    std::vector<LinkDetail> link_details;
    double estimated_reliability = 0.0;
    double bottleneck_bandwidth_gbps = 0.0;
    bool satisfies_constraints;
    std::string reason;
    
    json to_json() const {
        json per_vnf_json = json::array();
        for (const auto& pv : per_vnf) {
            per_vnf_json.push_back(pv.to_json());
        }
        
        json link_details_json = json::array();
        for (const auto& ld : link_details) {
            link_details_json.push_back(ld.to_json());
        }
        
        return {
            {"score", score},
            {"deployed_nodes", deployed_nodes},
            {"per_vnf", per_vnf_json},
            {"per_core_nf", per_vnf_json},
            {"total_latency_ms", total_latency_ms},
            {"link_details", link_details_json},
            {"estimated_reliability", estimated_reliability},
            {"bottleneck_bandwidth_gbps", bottleneck_bandwidth_gbps},
            {"satisfies_constraints", satisfies_constraints},
            {"reason", reason}
        };
    }
};

// 部署记录
struct Deployment {
    std::string deployment_id;
    std::string request_id;
    int candidate_index;
    std::string status;  // pending, in-progress, completed, failed
    std::vector<std::string> deployed_nodes;
    std::vector<VNFDeployment> per_vnf;
    double total_latency_ms;
    std::string deployed_at;
    int progress;
    
    json to_json() const {
        json per_vnf_json = json::array();
        for (const auto& pv : per_vnf) {
            per_vnf_json.push_back(pv.to_json());
        }
        
        return {
            {"deployment_id", deployment_id},
            {"request_id", request_id},
            {"candidate_index", candidate_index},
            {"status", status},
            {"deployed_nodes", deployed_nodes},
            {"per_vnf", per_vnf_json},
            {"per_core_nf", per_vnf_json},
            {"total_latency_ms", total_latency_ms},
            {"deployed_at", deployed_at},
            {"progress", progress}
        };
    }
};

} // namespace sfc
