#include "network_graph.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace sfc {

static float clamp01(float x) {
    if (!std::isfinite(x)) return 0.0f;
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

bool NetworkGraph::load_from_json(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filepath << std::endl;
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        std::cerr << "Error: JSON parsing failed: " << e.what() << std::endl;
        return false;
    }

    try {
        auto& topology = j["topology"];
        auto& nodes_json = topology["nodes"];

        nodes_.clear();
        node_id_to_idx_.clear();

        for (size_t i = 0; i < nodes_json.size(); ++i) {
            auto& node_json = nodes_json[i];
            Node node;
            node.id = node_json["id"].get<std::string>();
            node.type = node_json.value("type", "satellite");

            node.resources.cpu_total = node_json.value("cpu_total", 0.0f);
            node.resources.cpu_available = node_json.value("cpu_available", 0.0f);
            node.resources.mem_total = node_json.value("mem_total", 0.0f);
            node.resources.mem_available = node_json.value("mem_available", 0.0f);
            node.resources.disk_total = node_json.value("disk_total", 256.0f);
            node.resources.disk_available = node_json.value("disk_available", node.resources.disk_total * 0.8f);

            node.core_network_load = node_json.value("core_network_load", 0.5f);
            if (node_json.contains("core_business_load")) {
                const auto& cbl = node_json["core_business_load"];
                node.core_business_load.signaling_load = cbl.value("signaling_load", node.core_network_load);
                node.core_business_load.session_load = cbl.value("session_load", node.core_network_load);
                node.core_business_load.user_plane_load = cbl.value("user_plane_load", node.core_network_load);
                node.core_business_load.mobility_load = cbl.value("mobility_load", node.core_network_load);
                node.core_business_load.policy_load = cbl.value("policy_load", node.core_network_load);
                node.core_business_load.auth_load = cbl.value("auth_load", node.core_network_load);
            } else {
                node.core_business_load.signaling_load = node_json.value("signaling_load", node.core_network_load);
                node.core_business_load.session_load = node_json.value("session_load", node.core_network_load);
                node.core_business_load.user_plane_load = node_json.value("user_plane_load", node.core_network_load);
                node.core_business_load.mobility_load = node_json.value("mobility_load", node.core_network_load);
                node.core_business_load.policy_load = node_json.value("policy_load", node.core_network_load);
                node.core_business_load.auth_load = node_json.value("auth_load", node.core_network_load);
            }
            node.core_business_load.signaling_load = clamp01(node.core_business_load.signaling_load);
            node.core_business_load.session_load = clamp01(node.core_business_load.session_load);
            node.core_business_load.user_plane_load = clamp01(node.core_business_load.user_plane_load);
            node.core_business_load.mobility_load = clamp01(node.core_business_load.mobility_load);
            node.core_business_load.policy_load = clamp01(node.core_business_load.policy_load);
            node.core_business_load.auth_load = clamp01(node.core_business_load.auth_load);
            node.core_network_load = (
                node.core_business_load.signaling_load +
                node.core_business_load.session_load +
                node.core_business_load.user_plane_load +
                node.core_business_load.mobility_load +
                node.core_business_load.policy_load +
                node.core_business_load.auth_load
            ) / 6.0f;
            node.node_reliability = node_json.value("node_reliability", 0.98f);

            node_id_to_idx_[node.id] = i;
            nodes_.push_back(node);
        }

        auto& links_json = topology["links"];
        links_.clear();
        link_map_.clear();

        for (auto& link_json : links_json) {
            Link link;
            link.source = link_json["source"].get<std::string>();
            link.target = link_json["target"].get<std::string>();
            link.latency_ms = link_json.value("latency_ms", 1.0f);
            link.bandwidth_gbps = link_json.value("bandwidth_gbps", 0.0f);
            link.bandwidth_available_gbps = link_json.value("bandwidth_available_gbps", 0.0f);
            link.link_status = link_json.value("link_status", 1);
            link.link_reliability = link_json.value("link_reliability", 0.98f);

            if (node_id_to_idx_.count(link.source) && node_id_to_idx_.count(link.target)) {
                size_t src_idx = node_id_to_idx_[link.source];
                size_t tgt_idx = node_id_to_idx_[link.target];
                nodes_[src_idx].neighbors.push_back(tgt_idx);
                nodes_[tgt_idx].predecessors.push_back(src_idx);
                link_map_[link.source][link.target] = links_.size();
            }

            links_.push_back(link);
        }

        std::cout << "[NetworkGraph] Loaded: " << nodes_.size() << " nodes, " << links_.size() << " links" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to parse topology: " << e.what() << std::endl;
        return false;
    }
}

const Node* NetworkGraph::get_node(const std::string& id) const {
    auto it = node_id_to_idx_.find(id);
    return (it != node_id_to_idx_.end()) ? &nodes_[it->second] : nullptr;
}

size_t NetworkGraph::get_node_index(const std::string& id) const {
    auto it = node_id_to_idx_.find(id);
    if (it == node_id_to_idx_.end()) {
        throw std::runtime_error("Node not found: " + id);
    }
    return it->second;
}

const Link* NetworkGraph::get_link(const std::string& source, const std::string& target) const {
    auto it1 = link_map_.find(source);
    if (it1 == link_map_.end()) {
        return nullptr;
    }
    auto it2 = it1->second.find(target);
    return (it2 != it1->second.end()) ? &links_[it2->second] : nullptr;
}

std::vector<float> NetworkGraph::get_node_features() const {
    std::vector<float> features;
    features.reserve(nodes_.size() * 14);

    for (const auto& node : nodes_) {
        float cpu_ratio = node.resources.cpu_total > 1e-6f ? node.resources.cpu_available / node.resources.cpu_total : 0.0f;
        float mem_ratio = node.resources.mem_total > 1e-6f ? node.resources.mem_available / node.resources.mem_total : 0.0f;
        float disk_ratio = node.resources.disk_total > 1e-6f ? node.resources.disk_available / node.resources.disk_total : 0.0f;

        float active_ratio = 0.0f;
        float bw_ratio = 0.0f;
        float latency_norm = 1.0f;
        int out_count = 0;

        for (size_t nb_idx : node.neighbors) {
            const auto& nb = nodes_[nb_idx];
            const Link* l = get_link(node.id, nb.id);
            if (!l) {
                continue;
            }
            out_count += 1;
            active_ratio += static_cast<float>(l->link_status);
            if (l->bandwidth_gbps > 1e-6f) {
                bw_ratio += l->bandwidth_available_gbps / l->bandwidth_gbps;
            }
            latency_norm += l->latency_ms / 50.0f;
        }

        if (out_count > 0) {
            active_ratio /= static_cast<float>(out_count);
            bw_ratio /= static_cast<float>(out_count);
            latency_norm /= static_cast<float>(out_count);
        }

        features.push_back(cpu_ratio);
        features.push_back(mem_ratio);
        features.push_back(disk_ratio);
        features.push_back(node.core_network_load);
        features.push_back(node.node_reliability);
        features.push_back(active_ratio);
        features.push_back(bw_ratio);
        features.push_back(std::min(5.0f, std::max(0.0f, latency_norm)));
        features.push_back(node.core_business_load.signaling_load);
        features.push_back(node.core_business_load.session_load);
        features.push_back(node.core_business_load.user_plane_load);
        features.push_back(node.core_business_load.mobility_load);
        features.push_back(node.core_business_load.policy_load);
        features.push_back(node.core_business_load.auth_load);
    }

    return features;
}

std::vector<int64_t> NetworkGraph::get_edge_index() const {
    std::vector<int64_t> edge_index;
    edge_index.reserve(links_.size() * 2);
    for (const auto& link : links_) {
        auto it_src = node_id_to_idx_.find(link.source);
        auto it_tgt = node_id_to_idx_.find(link.target);
        if (it_src != node_id_to_idx_.end() && it_tgt != node_id_to_idx_.end()) {
            edge_index.push_back(static_cast<int64_t>(it_src->second));
            edge_index.push_back(static_cast<int64_t>(it_tgt->second));
        }
    }
    return edge_index;
}

NetworkGraph NetworkGraph::clone() const {
    NetworkGraph copy;
    copy.nodes_ = nodes_;
    copy.links_ = links_;
    copy.node_id_to_idx_ = node_id_to_idx_;
    copy.link_map_ = link_map_;
    return copy;
}

void NetworkGraph::update_node_resources(const std::string& node_id, float cpu_delta, float mem_delta, float disk_delta) {
    auto it = node_id_to_idx_.find(node_id);
    if (it != node_id_to_idx_.end()) {
        nodes_[it->second].resources.cpu_available += cpu_delta;
        nodes_[it->second].resources.mem_available += mem_delta;
        nodes_[it->second].resources.disk_available += disk_delta;
    }
}

void NetworkGraph::update_link_bandwidth(const std::string& src, const std::string& tgt, float bw_delta) {
    auto it1 = link_map_.find(src);
    if (it1 != link_map_.end()) {
        auto it2 = it1->second.find(tgt);
        if (it2 != it1->second.end()) {
            links_[it2->second].bandwidth_available_gbps += bw_delta;
        }
    }
}

std::vector<SFCRequest> load_sfc_requests(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open SFC requests file: " + filepath);
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("JSON parsing failed: ") + e.what());
    }

    std::vector<SFCRequest> requests;
    try {
        auto& requests_json = j["requests"];
        for (auto& req_json : requests_json) {
            SFCRequest req;
            req.request_id = req_json["request_id"].get<std::string>();
            req.service_type = req_json["service_type"].get<std::string>();
            req.network_domain = req_json.value("network_domain", "open5gs");
            req.source_node = req_json["source_node"].get<std::string>();
            req.destination_node = req_json["destination_node"].get<std::string>();
            req.max_latency_ms = req_json.value("max_latency_ms", 100.0f);
            req.priority = req_json.value("priority", "medium");
            req.bandwidth_demand_gbps = req_json.value("bandwidth_demand_gbps", 0.1f);
            req.reliability_requirement = req_json.value("reliability_requirement", 0.97f);
            if (req_json.contains("core_business_load")) {
                const auto& cbl = req_json["core_business_load"];
                req.core_business_load.signaling_load = cbl.value("signaling_load", 0.5f);
                req.core_business_load.session_load = cbl.value("session_load", 0.5f);
                req.core_business_load.user_plane_load = cbl.value("user_plane_load", 0.5f);
                req.core_business_load.mobility_load = cbl.value("mobility_load", 0.5f);
                req.core_business_load.policy_load = cbl.value("policy_load", 0.5f);
                req.core_business_load.auth_load = cbl.value("auth_load", 0.5f);
            } else {
                req.core_business_load.signaling_load = 0.5f;
                req.core_business_load.session_load = 0.5f;
                req.core_business_load.user_plane_load = 0.5f;
                req.core_business_load.mobility_load = 0.5f;
                req.core_business_load.policy_load = 0.5f;
                req.core_business_load.auth_load = 0.5f;
            }

            if (req_json.contains("sla")) {
                auto sla = req_json["sla"];
                req.max_latency_ms = sla.value("latency_requirement_ms", req.max_latency_ms);
                req.bandwidth_demand_gbps = sla.value("bandwidth_demand_gbps", req.bandwidth_demand_gbps);
                req.reliability_requirement = sla.value("reliability_requirement", req.reliability_requirement);
            }

            auto seq_json = req_json.contains("core_nf_sequence")
                ? req_json["core_nf_sequence"]
                : (req_json.contains("vnf_sequence") ? req_json["vnf_sequence"] : nlohmann::json::array());
            if (seq_json.empty() && req_json.contains("core_nfs")) {
                seq_json = nlohmann::json::array();
                for (const auto& nf_json : req_json["core_nfs"]) {
                    seq_json.push_back({
                        {"vnf_id", nf_json.value("core_nf_id", nf_json.value("name", std::string("")))},
                        {"core_nf_id", nf_json.value("core_nf_id", nf_json.value("name", std::string("")))},
                        {"vnf_type", nf_json.value("nf_type", nf_json.value("core_nf_type", std::string("")))},
                        {"core_nf_type", nf_json.value("core_nf_type", nf_json.value("nf_type", std::string("")))},
                        {"nf_type", nf_json.value("nf_type", nf_json.value("core_nf_type", std::string("")))},
                        {"nf_role", nf_json.value("nf_role", std::string("control_plane"))},
                        {"processing_weight", nf_json.value("processing_weight", 1.0f)},
                        {"stateful", nf_json.value("stateful", true)},
                        {"cpu_required", nf_json.value("cpu", 0.0f)},
                        {"mem_required", nf_json.value("mem", 0.0f)},
                        {"disk_required_gb", nf_json.value("disk", 0.0f)},
                        {"bandwidth_required_gbps", std::max(nf_json.value("bw_in", 0.0f), nf_json.value("bw_out", 0.0f))},
                    });
                }
            }
            for (auto& vnf_json : seq_json) {
                VNFRequirement vnf;
                vnf.vnf_id = vnf_json.value("vnf_id", vnf_json.value("core_nf_id", std::string("")));
                vnf.core_nf_id = vnf_json.value("core_nf_id", vnf.vnf_id);
                vnf.vnf_type = vnf_json.value("vnf_type", vnf_json.value("core_nf_type", std::string("")));
                vnf.core_nf_type = vnf_json.value("core_nf_type", vnf.vnf_type);
                vnf.nf_type = vnf_json.value("nf_type", vnf.core_nf_type.empty() ? vnf.vnf_type : vnf.core_nf_type);
                vnf.nf_role = vnf_json.value("nf_role", std::string("control_plane"));
                vnf.processing_weight = vnf_json.value("processing_weight", 1.0f);
                vnf.stateful = vnf_json.value("stateful", true);
                vnf.cpu_required = vnf_json.value("cpu_required", 0.0f);
                vnf.mem_required = vnf_json.value("mem_required", 0.0f);
                vnf.bandwidth_required_gbps = vnf_json.value("bandwidth_required_gbps", 0.0f);
                vnf.disk_required_gb = vnf_json.value("disk_required_gb", 0.0f);
                if (vnf_json.contains("business_load_demand")) {
                    const auto& bld = vnf_json["business_load_demand"];
                    vnf.business_load_demand.signaling_load = bld.value("signaling_load", req.core_business_load.signaling_load);
                    vnf.business_load_demand.session_load = bld.value("session_load", req.core_business_load.session_load);
                    vnf.business_load_demand.user_plane_load = bld.value("user_plane_load", req.core_business_load.user_plane_load);
                    vnf.business_load_demand.mobility_load = bld.value("mobility_load", req.core_business_load.mobility_load);
                    vnf.business_load_demand.policy_load = bld.value("policy_load", req.core_business_load.policy_load);
                    vnf.business_load_demand.auth_load = bld.value("auth_load", req.core_business_load.auth_load);
                } else {
                    vnf.business_load_demand = req.core_business_load;
                }
                if (vnf.vnf_id.empty()) {
                    vnf.vnf_id = "core_nf_" + std::to_string(req.vnf_sequence.size());
                }
                if (vnf.vnf_type.empty()) {
                    vnf.vnf_type = vnf.nf_type.empty() ? vnf.vnf_id : vnf.nf_type;
                }
                if (vnf.core_nf_id.empty()) {
                    vnf.core_nf_id = vnf.vnf_id;
                }
                if (vnf.core_nf_type.empty()) {
                    vnf.core_nf_type = vnf.vnf_type;
                }
                req.vnf_sequence.push_back(vnf);
            }
            requests.push_back(req);
        }
        std::cout << "[Main] Loaded " << requests.size() << " SFC requests" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to parse SFC requests: ") + e.what());
    }

    return requests;
}

} // namespace sfc
