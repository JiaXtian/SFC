#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfc {

struct NodeResources {
    float cpu_total = 0.0f;
    float cpu_available = 0.0f;
    float mem_total = 0.0f;
    float mem_available = 0.0f;
    float disk_total = 0.0f;
    float disk_available = 0.0f;

    bool has_sufficient_resources(float cpu_req, float mem_req, float disk_req) const {
        return cpu_available >= cpu_req && mem_available >= mem_req && disk_available >= disk_req;
    }
};

struct Node {
    std::string id;
    std::string type;
    NodeResources resources;
    float core_network_load = 0.5f;
    float node_reliability = 0.98f;
    std::vector<size_t> neighbors;
    std::vector<size_t> predecessors;
};

struct Link {
    std::string source;
    std::string target;
    float latency_ms = 0.0f;
    float bandwidth_gbps = 0.0f;
    float bandwidth_available_gbps = 0.0f;
    int link_status = 1;
    float link_reliability = 0.98f;
};

struct VNFRequirement {
    std::string vnf_id;
    std::string vnf_type;
    float cpu_required = 0.0f;
    float mem_required = 0.0f;
    float bandwidth_required_gbps = 0.0f;
    float disk_required_gb = 0.0f;
};

struct SFCRequest {
    std::string request_id;
    std::string service_type;
    std::vector<VNFRequirement> vnf_sequence;
    std::string source_node;
    std::string destination_node;
    float max_latency_ms = 0.0f;
    float bandwidth_demand_gbps = 0.0f;
    float reliability_requirement = 0.97f;
    float core_network_load = 0.5f;
    float priority_weight = 1.0f;
    std::string priority;
};

class NetworkGraph {
public:
    NetworkGraph() = default;
    bool load_from_json(const std::string& filepath);
    size_t get_total_links() const;
    const std::vector<Node>& get_nodes() const { return nodes_; }
    const std::vector<Link>& get_links() const { return links_; }
    const Node* get_node(const std::string& id) const;
    const Node& get_node_by_index(size_t idx) const { return nodes_[idx]; }
    const Link* get_link(const std::string& source, const std::string& target) const;
    size_t get_node_index(const std::string& id) const;

    std::vector<float> get_node_features() const;
    std::vector<int64_t> get_edge_index() const;
    NetworkGraph clone() const;

    void update_node_resources(const std::string& node_id, float cpu_delta, float mem_delta, float disk_delta = 0.0f);
    void update_link_bandwidth(const std::string& src, const std::string& tgt, float bw_delta);

private:
    std::vector<Node> nodes_;
    std::vector<Link> links_;
    std::unordered_map<std::string, size_t> node_id_to_idx_;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> link_map_;
};

std::vector<SFCRequest> load_sfc_requests(const std::string& filepath);

} // namespace sfc
