#include "services/ResourceManager.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <unordered_set>

namespace sfc {
namespace {

struct EffectiveResource {
    double cpu = 0.0;
    double mem = 0.0;
    double disk = 0.0;
};

EffectiveResource effective_resource_for(
    const DeploymentCandidate::PerVNF& pv,
    const std::vector<VNF>& vnfs,
    size_t index
) {
    EffectiveResource res;
    if (index < vnfs.size()) {
        res.cpu = std::max(0.0, vnfs[index].cpu);
        res.mem = std::max(0.0, vnfs[index].mem);
        res.disk = std::max(0.0, vnfs[index].disk);
    }
    if (res.cpu <= 1e-9) res.cpu = std::max(0.0, pv.cpu_used);
    if (res.mem <= 1e-9) res.mem = std::max(0.0, pv.mem_used);
    if (res.disk <= 1e-9) res.disk = std::max(0.0, pv.disk_used);
    if (res.disk <= 1e-9 && res.mem > 1e-9) res.disk = res.mem * 2.0;
    return res;
}

} // namespace

ResourceManager::ResourceManager() {
    metadata_.total_sats = 0;
    metadata_.num_planes = 0;
    metadata_.altitude_km = 0.0;
    metadata_.inclination_deg = 0.0;
    spdlog::info("ResourceManager initialized");
}

void ResourceManager::load_topology(const Topology& topology) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    satellites_.clear();
    links_.clear();
    
    metadata_ = topology.metadata; 
    
    for (const auto& node : topology.nodes) {
        satellites_[node.id] = node;
    }
    
    for (const auto& link : topology.links) {
        std::string key1 = make_link_key(link.source, link.target);
        std::string key2 = make_link_key(link.target, link.source);
        links_[key1] = link;
        links_[key2] = link;  // 双向存储
    }
    
    spdlog::info("ResourceManager loaded topology: {} nodes, {} links (bidirectional)",
                satellites_.size(), links_.size());
}

Topology ResourceManager::export_current_topology() const {
    Topology topo;
    std::lock_guard<std::mutex> lock(mutex_);

    topo.metadata = metadata_; 
    topo.metadata.total_sats = static_cast<int>(satellites_.size());

    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        topo.metadata.timestamp = ss.str();
    }

    topo.nodes.reserve(satellites_.size());
    for (const auto& kv : satellites_) {
        topo.nodes.push_back(kv.second);
    }


    std::unordered_set<std::string> added_links;
    topo.links.reserve(links_.size() / 2);
    
    for (const auto& kv : links_) {
        const auto& link = kv.second;
        std::string link_id = link.source < link.target 
            ? link.source + "-" + link.target 
            : link.target + "-" + link.source;
        
        if (added_links.find(link_id) == added_links.end()) {
            topo.links.push_back(link);
            added_links.insert(link_id);
        }
    }

    return topo;
}

void ResourceManager::reset_all_allocations() {
    std::lock_guard<std::mutex> lock(mutex_);
    deployments_.clear();
    link_allocated_gbps_.clear();
}

bool ResourceManager::check_node_resources(
    const std::string& node_id,
    double cpu_required,
    double mem_required,
    double disk_required
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = satellites_.find(node_id);
    if (it == satellites_.end()) {
        return false;
    }
    
    return it->second.cpu_available >= cpu_required &&
           it->second.mem_available >= mem_required &&
           it->second.disk_available >= disk_required;
}

bool ResourceManager::check_link_bandwidth(
    const std::string& src,
    const std::string& dst,
    double bw_required
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key1 = make_link_key(src, dst);
    std::string key2 = make_link_key(dst, src);
    
    auto it = links_.find(key1);
    if (it == links_.end()) {
        it = links_.find(key2);
    }
    
    if (it == links_.end()) {
        return false;
    }
    
    return it->second.bandwidth_available_gbps >= bw_required;
}

bool ResourceManager::allocate_resources(
    const std::string& deployment_id,
    const DeploymentCandidate& candidate,
    const std::vector<VNF>& vnfs
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ResourceSnapshot snapshot;
    auto rollback_partial = [&]() {
        for (const auto& [node_id, cpu_amount] : snapshot.cpu_allocations) {
            auto node_it = satellites_.find(node_id);
            if (node_it != satellites_.end()) {
                node_it->second.cpu_available = std::min(node_it->second.cpu_total, node_it->second.cpu_available + cpu_amount);
            }
        }
        for (const auto& [node_id, mem_amount] : snapshot.mem_allocations) {
            auto node_it = satellites_.find(node_id);
            if (node_it != satellites_.end()) {
                node_it->second.mem_available = std::min(node_it->second.mem_total, node_it->second.mem_available + mem_amount);
            }
        }
        for (const auto& [node_id, disk_amount] : snapshot.disk_allocations) {
            auto node_it = satellites_.find(node_id);
            if (node_it != satellites_.end()) {
                node_it->second.disk_available = std::min(node_it->second.disk_total, node_it->second.disk_available + disk_amount);
            }
        }
        for (const auto& [src, dst, bw_amount] : snapshot.bw_allocations) {
            std::string key1 = make_link_key(src, dst);
            std::string key2 = make_link_key(dst, src);
            auto link_it = links_.find(key1);
            if (link_it == links_.end()) link_it = links_.find(key2);
            if (link_it != links_.end()) {
                link_it->second.bandwidth_available_gbps =
                    std::min(link_it->second.bandwidth_gbps, link_it->second.bandwidth_available_gbps + bw_amount);
                link_it->second.status =
                    (link_it->second.bandwidth_available_gbps <= link_it->second.bandwidth_gbps * 0.15)
                        ? "congested" : "active";
                links_[key1] = link_it->second;
                links_[key2] = link_it->second;
            }
            auto alloc_it = link_allocated_gbps_.find(make_canonical_link_key(src, dst));
            if (alloc_it != link_allocated_gbps_.end()) {
                alloc_it->second -= bw_amount;
                if (alloc_it->second <= 1e-9) link_allocated_gbps_.erase(alloc_it);
            }
        }
    };
    
    // 分配节点资源。以 candidate.per_vnf 为主，vnfs 仅作为资源规格补充；
    // 避免核心网候选只携带 per_core_nf/per_vnf 时因为 vnfs 为空而没有扣减。
    for (size_t i = 0; i < candidate.per_vnf.size(); ++i) {
        const auto& pv = candidate.per_vnf[i];
        const auto res = effective_resource_for(pv, vnfs, i);
        if (pv.node.empty()) continue;
        if (res.cpu <= 1e-9 && res.mem <= 1e-9 && res.disk <= 1e-9) {
            spdlog::warn("Skip zero resource allocation for deployment {} NF {} on {}",
                         deployment_id, pv.core_nf.empty() ? pv.vnf : pv.core_nf, pv.node);
            continue;
        }
        
        auto it = satellites_.find(pv.node);
        if (it == satellites_.end()) {
            spdlog::error("Node {} not found", pv.node);
            rollback_partial();
            return false;
        }
        
        if (it->second.cpu_available < res.cpu ||
            it->second.mem_available < res.mem ||
            it->second.disk_available < res.disk) {
            spdlog::error("Insufficient resources on node {}", pv.node);
            rollback_partial();
            return false;
        }
        
        it->second.cpu_available -= res.cpu;
        it->second.mem_available -= res.mem;
        it->second.disk_available -= res.disk;
        
        snapshot.cpu_allocations.push_back({pv.node, res.cpu});
        snapshot.mem_allocations.push_back({pv.node, res.mem});
        snapshot.disk_allocations.push_back({pv.node, res.disk});
        
        spdlog::debug("Allocated on {}: CPU {:.2f}, MEM {:.2f}, DISK {:.2f}", 
                     pv.node, res.cpu, res.mem, res.disk);
    }
    
    // 分配链路带宽
    for (const auto& link_detail : candidate.link_details) {
        std::string key1 = make_link_key(link_detail.src, link_detail.dst);
        std::string key2 = make_link_key(link_detail.dst, link_detail.src);
        
        auto it = links_.find(key1);
        if (it == links_.end()) {
            it = links_.find(key2);
        }
        
        if (it == links_.end()) {
            spdlog::error("Link {}->{} not found", link_detail.src, link_detail.dst);
            rollback_partial();
            return false;
        }
        
        double bw_needed = link_detail.bandwidth_required_gbps > 0.0
                         ? link_detail.bandwidth_required_gbps
                         : 0.1;
        if (it->second.bandwidth_available_gbps + 1e-9 < bw_needed) {
            spdlog::error(
                "Insufficient bandwidth on link {}->{}: need {:.3f}Gbps, available {:.3f}Gbps",
                link_detail.src,
                link_detail.dst,
                bw_needed,
                it->second.bandwidth_available_gbps
            );
            rollback_partial();
            return false;
        }

        it->second.bandwidth_available_gbps -= bw_needed;
        if (it->second.bandwidth_available_gbps <= it->second.bandwidth_gbps * 0.15) {
            it->second.status = "congested";
        } else {
            it->second.status = "active";
        }
        snapshot.bw_allocations.push_back({link_detail.src, link_detail.dst, bw_needed});
        link_allocated_gbps_[make_canonical_link_key(link_detail.src, link_detail.dst)] += bw_needed;

        // 双向键都更新，保持状态一致
        if (key1 != key2) {
            links_[key1] = it->second;
            links_[key2] = it->second;
        }
    }
    
    deployments_[deployment_id] = snapshot;
    spdlog::info("Resources allocated for deployment {}", deployment_id);
    return true;
}

bool ResourceManager::restore_allocation_snapshot(
    const std::string& deployment_id,
    const DeploymentCandidate& candidate,
    const std::vector<VNF>& vnfs
) {
    if (deployment_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing = deployments_.find(deployment_id);
    if (existing != deployments_.end()) {
        for (const auto& [src, dst, bw] : existing->second.bw_allocations) {
            auto alloc_it = link_allocated_gbps_.find(make_canonical_link_key(src, dst));
            if (alloc_it == link_allocated_gbps_.end()) continue;
            alloc_it->second -= bw;
            if (alloc_it->second <= 1e-9) link_allocated_gbps_.erase(alloc_it);
        }
        deployments_.erase(existing);
    }

    ResourceSnapshot snapshot;
    for (size_t i = 0; i < candidate.per_vnf.size(); ++i) {
        const auto& pv = candidate.per_vnf[i];
        if (pv.node.empty()) continue;
        const auto res = effective_resource_for(pv, vnfs, i);
        if (res.cpu <= 1e-9 && res.mem <= 1e-9 && res.disk <= 1e-9) continue;
        snapshot.cpu_allocations.push_back({pv.node, res.cpu});
        snapshot.mem_allocations.push_back({pv.node, res.mem});
        snapshot.disk_allocations.push_back({pv.node, res.disk});
    }

    for (const auto& link_detail : candidate.link_details) {
        if (link_detail.src.empty() || link_detail.dst.empty() || link_detail.src == link_detail.dst) continue;
        double bw_needed = link_detail.bandwidth_required_gbps > 0.0
                         ? link_detail.bandwidth_required_gbps
                         : 0.1;
        snapshot.bw_allocations.push_back({link_detail.src, link_detail.dst, bw_needed});
        link_allocated_gbps_[make_canonical_link_key(link_detail.src, link_detail.dst)] += bw_needed;
    }

    deployments_[deployment_id] = std::move(snapshot);
    spdlog::info("Resource allocation snapshot restored for deployment {}", deployment_id);
    return true;
}

bool ResourceManager::release_resources(const std::string& deployment_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = deployments_.find(deployment_id);
    if (it == deployments_.end()) {
        spdlog::warn("Deployment {} not found for rollback", deployment_id);
        return false;
    }
    
    const auto& snapshot = it->second;
    
    for (const auto& [node_id, cpu_amount] : snapshot.cpu_allocations) {
        auto node_it = satellites_.find(node_id);
        if (node_it != satellites_.end()) {
            node_it->second.cpu_available += cpu_amount;
            spdlog::debug("Released CPU {:.2f} on {}", cpu_amount, node_id);
        }
    }
    
    for (const auto& [node_id, mem_amount] : snapshot.mem_allocations) {
        auto node_it = satellites_.find(node_id);
        if (node_it != satellites_.end()) {
            node_it->second.mem_available += mem_amount;
            spdlog::debug("Released MEM {:.2f} on {}", mem_amount, node_id);
        }
    }

    for (const auto& [node_id, disk_amount] : snapshot.disk_allocations) {
        auto node_it = satellites_.find(node_id);
        if (node_it != satellites_.end()) {
            node_it->second.disk_available += disk_amount;
            spdlog::debug("Released DISK {:.2f} on {}", disk_amount, node_id);
        }
    }
    
    for (const auto& [src, dst, bw_amount] : snapshot.bw_allocations) {
        // 🔥 双向查找
        std::string key1 = make_link_key(src, dst);
        std::string key2 = make_link_key(dst, src);
        const std::string canonical = make_canonical_link_key(src, dst);
        
        auto link_it = links_.find(key1);
        if (link_it == links_.end()) {
            link_it = links_.find(key2);
        }
        
        if (link_it != links_.end()) {
            link_it->second.bandwidth_available_gbps += bw_amount;
            if (link_it->second.bandwidth_available_gbps > link_it->second.bandwidth_gbps) {
                link_it->second.bandwidth_available_gbps = link_it->second.bandwidth_gbps;
            }
            if (link_it->second.bandwidth_available_gbps <= link_it->second.bandwidth_gbps * 0.15) {
                link_it->second.status = "congested";
            } else {
                link_it->second.status = "active";
            }

            // 双向键都更新，保持状态一致
            if (key1 != key2) {
                links_[key1] = link_it->second;
                links_[key2] = link_it->second;
            }
        }

        auto alloc_it = link_allocated_gbps_.find(canonical);
        if (alloc_it != link_allocated_gbps_.end()) {
            alloc_it->second -= bw_amount;
            if (alloc_it->second <= 1e-9) {
                link_allocated_gbps_.erase(alloc_it);
            }
        }
    }
    
    deployments_.erase(it);
    spdlog::info("Resources released for deployment {}", deployment_id);
    return true;
}

Satellite ResourceManager::get_satellite(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = satellites_.find(node_id);
    if (it != satellites_.end()) return it->second;
    throw std::runtime_error("Satellite not found: " + node_id);
}

double ResourceManager::get_allocated_link_bandwidth(
    const std::string& src,
    const std::string& dst
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = link_allocated_gbps_.find(make_canonical_link_key(src, dst));
    if (it == link_allocated_gbps_.end()) return 0.0;
    return std::max(0.0, it->second);
}

std::tuple<double, double, double> ResourceManager::get_allocated_node_resources(
    const std::string& node_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double cpu = 0.0;
    double mem = 0.0;
    double disk = 0.0;

    for (const auto& dep_kv : deployments_) {
        const auto& snapshot = dep_kv.second;
        for (const auto& [nid, amount] : snapshot.cpu_allocations) {
            if (nid == node_id) cpu += amount;
        }
        for (const auto& [nid, amount] : snapshot.mem_allocations) {
            if (nid == node_id) mem += amount;
        }
        for (const auto& [nid, amount] : snapshot.disk_allocations) {
            if (nid == node_id) disk += amount;
        }
    }
    return {
        std::max(0.0, cpu),
        std::max(0.0, mem),
        std::max(0.0, disk)
    };
}

std::string ResourceManager::make_link_key(const std::string& src, const std::string& dst) const {
    return src + "->" + dst;
}

std::string ResourceManager::make_canonical_link_key(const std::string& src, const std::string& dst) const {
    if (src <= dst) {
        return src + "<->" + dst;
    }
    return dst + "<->" + src;
}

} // namespace sfc
