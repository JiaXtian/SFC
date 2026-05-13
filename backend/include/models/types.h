#pragma once
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace sfc {

struct CoreBusinessLoad {
    double signaling_load = 0.0;     // 信令交互负载
    double session_load = 0.0;       // 会话建立/维护负载
    double user_plane_load = 0.0;    // 用户面吞吐负载
    double mobility_load = 0.0;      // 移动性/切换负载
    double policy_load = 0.0;        // 策略与QoS控制负载
    double auth_load = 0.0;          // 鉴权与安全负载

    static double clamp01(double x) {
        if (x < 0.0) return 0.0;
        if (x > 1.0) return 1.0;
        return x;
    }

    void normalize_inplace() {
        signaling_load = clamp01(signaling_load);
        session_load = clamp01(session_load);
        user_plane_load = clamp01(user_plane_load);
        mobility_load = clamp01(mobility_load);
        policy_load = clamp01(policy_load);
        auth_load = clamp01(auth_load);
    }

    double load_index() const {
        return (
            clamp01(signaling_load) +
            clamp01(session_load) +
            clamp01(user_plane_load) +
            clamp01(mobility_load) +
            clamp01(policy_load) +
            clamp01(auth_load)
        ) / 6.0;
    }

    json to_json() const {
        return {
            {"signaling_load", clamp01(signaling_load)},
            {"session_load", clamp01(session_load)},
            {"user_plane_load", clamp01(user_plane_load)},
            {"mobility_load", clamp01(mobility_load)},
            {"policy_load", clamp01(policy_load)},
            {"auth_load", clamp01(auth_load)},
            {"load_index", load_index()}
        };
    }
};

// 轨道参数
struct OrbitalParams {
    std::string propagation_model = "SGP4";
    int plane = 0;
    int position_in_plane = 0;
    double raan = 0.0;
    double true_anomaly = 0.0;
    double altitude_km = 550.0;
    double inclination_deg = 53.0;
    double eccentricity = 0.0001;
    double argument_of_perigee_deg = 0.0;
    double mean_anomaly_deg = 0.0;
    double mean_motion_rev_per_day = 0.0;
    double bstar = 0.0;
    double epoch_jd = 0.0;
    double propagation_minutes = 0.0;
    double semi_major_axis_km = 0.0;
    double period_minutes = 0.0;
    std::string epoch_iso = "";
    std::string tle_line1 = "";
    std::string tle_line2 = "";
    
    json to_json() const {
        return {
            {"propagation_model", propagation_model},
            {"plane", plane},
            {"position_in_plane", position_in_plane},
            {"raan", raan},
            {"true_anomaly", true_anomaly},
            {"altitude_km", altitude_km},
            {"inclination_deg", inclination_deg},
            {"inclination", inclination_deg},
            {"eccentricity", eccentricity},
            {"argument_of_perigee_deg", argument_of_perigee_deg},
            {"mean_anomaly_deg", mean_anomaly_deg},
            {"mean_motion_rev_per_day", mean_motion_rev_per_day},
            {"bstar", bstar},
            {"epoch_jd", epoch_jd},
            {"epoch_iso", epoch_iso},
            {"propagation_minutes", propagation_minutes},
            {"semi_major_axis_km", semi_major_axis_km},
            {"period_minutes", period_minutes},
            {"tle_line1", tle_line1},
            {"tle_line2", tle_line2}
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
    OrbitalParams orbital_params;
    Coordinates coordinates;
    double cpu_total;
    double cpu_available;
    double mem_total;
    double mem_available;
    double disk_total;
    double disk_available;
    double core_network_load = 0.0;
    CoreBusinessLoad core_business_load;
    double node_reliability = 0.98;
    std::string status = "active";  // active, down
    std::string fault_tag = "";
    std::vector<VNFDeployment> vnfs;
    
    json to_json() const {
        json vnfs_json = json::array();
        for (const auto& vnf : vnfs) {
            vnfs_json.push_back(vnf.to_json());
        }
        
        return {
            {"id", id},
            {"type", type},
            {"orbital_params", orbital_params.to_json()},
            {"coordinates", coordinates.to_json()},
            {"cpu_total", cpu_total},
            {"cpu_available", cpu_available},
            {"mem_total", mem_total},
            {"mem_available", mem_available},
            {"disk_total", disk_total},
            {"disk_available", disk_available},
            {"core_network_load", core_network_load},
            {"core_business_load", core_business_load.to_json()},
            {"node_reliability", node_reliability},
            {"status", status},
            {"fault_tag", fault_tag},
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
        double sampling_interval_sec = 15.0;
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
    double sampling_interval_sec = 15.0;
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
        double avg_core_network_load = 0.0;
        double avg_signaling_load = 0.0;
        double avg_session_load = 0.0;
        double avg_user_plane_load = 0.0;
        double avg_mobility_load = 0.0;
        double avg_policy_load = 0.0;
        double avg_auth_load = 0.0;
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
                {"avg_bandwidth_utilization", metrics.avg_bandwidth_utilization},
                {"avg_core_network_load", metrics.avg_core_network_load},
                {"avg_signaling_load", metrics.avg_signaling_load},
                {"avg_session_load", metrics.avg_session_load},
                {"avg_user_plane_load", metrics.avg_user_plane_load},
                {"avg_mobility_load", metrics.avg_mobility_load},
                {"avg_policy_load", metrics.avg_policy_load},
                {"avg_auth_load", metrics.avg_auth_load}
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
    CoreBusinessLoad business_load_demand;
};

// 核心网网元功能依赖
struct CoreNFDependency {
    std::string source;
    std::string target;
    double criticality = 1.0;
    double bandwidth_scale = 0.5;
    double latency_weight = 1.0;
    double reliability_weight = 1.0;
    double bandwidth_required_gbps = 0.0;

    json to_json() const {
        return {
            {"source", source},
            {"target", target},
            {"criticality", criticality},
            {"bandwidth_scale", bandwidth_scale},
            {"latency_weight", latency_weight},
            {"reliability_weight", reliability_weight},
            {"bandwidth_required_gbps", bandwidth_required_gbps}
        };
    }
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
    std::vector<CoreNFDependency> core_nf_dependencies;
    std::vector<std::vector<std::string>> custom_nf_bindings;
    std::vector<std::string> independent_core_nfs;
    struct {
        double max_latency_ms;
        double registration_latency_ms = 120.0;
        double registration_access_latency_ms = 8.0;
        double pdu_session_latency_ms = 100.0;
        double pdu_access_latency_ms = 10.0;
        double min_bandwidth_gbps;
        double min_reliability;
    } constraints;
    std::string optimize;  // latency, resource, load_balance
    int topk;
    int topology_version = -1;
    std::string sim_time = "";
    CoreBusinessLoad core_business_load;
    bool realtime_mode = false;
    int max_planning_attempts = 0;
    double planning_time_budget_ms = 0.0;
    std::string inference_profile = "fast";  // fast, balanced, quality
    struct {
        double latency = -1.0;
        double resource = -1.0;
        double reliability = -1.0;
        double bandwidth = -1.0;
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
    double registration_latency_ms = 0.0;
    double pdu_session_latency_ms = 0.0;
    struct LinkDetail {
        std::string src;
        std::string dst;
        std::string dependency_source_nf;
        std::string dependency_target_nf;
        double latency_ms;
        double bandwidth_gbps;
        double bandwidth_available_gbps;
        double bandwidth_required_gbps;
        std::string status;
        double reliability;
        
        json to_json() const {
            json out = {
                {"src", src},
                {"dst", dst},
                {"dependency_source_nf", dependency_source_nf},
                {"dependency_target_nf", dependency_target_nf},
                {"latency_ms", latency_ms},
                {"bandwidth_gbps", bandwidth_gbps},
                {"bandwidth_available_gbps", bandwidth_available_gbps},
                {"bandwidth_required_gbps", bandwidth_required_gbps},
                {"status", status},
                {"reliability", reliability}
            };
            if (!dependency_source_nf.empty() || !dependency_target_nf.empty()) {
                out["core_nf_dependency"] = {
                    {"source", dependency_source_nf},
                    {"target", dependency_target_nf}
                };
            }
            return out;
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
            {"registration_latency_ms", registration_latency_ms},
            {"pdu_session_latency_ms", pdu_session_latency_ms},
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
