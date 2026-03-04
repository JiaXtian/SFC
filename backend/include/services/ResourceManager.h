#pragma once
#include "models/types.h"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <tuple>

namespace sfc {

class ResourceManager {
public:
    ResourceManager();
    
    // 检查节点资源
    bool check_node_resources(
        const std::string& node_id,
        double cpu_required,
        double mem_required,
        double disk_required = 0.0
    );
    
    // 检查链路带宽
    bool check_link_bandwidth(
        const std::string& src,
        const std::string& dst,
        double bw_required
    );
    
    // 分配资源
    bool allocate_resources(
        const std::string& deployment_id,
        const DeploymentCandidate& candidate,
        const std::vector<VNF>& vnfs
    );
    
    // 释放资源（回滚）
    bool release_resources(const std::string& deployment_id);
    
    // 加载拓扑：修改后支持保存元数据
    void load_topology(const Topology& topology);
    
    // 导出拓扑：修改后支持恢复元数据，供推理引擎使用
    Topology export_current_topology() const;
    
    // 获取节点信息
    Satellite get_satellite(const std::string& node_id) const;
    
private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Satellite> satellites_;
    std::unordered_map<std::string, Link> links_;
    
    // 新增：保存拓扑元数据（如 num_planes, altitude 等）
    Topology::Metadata metadata_;
    
    struct ResourceSnapshot {
        std::vector<std::pair<std::string, double>> cpu_allocations;
        std::vector<std::pair<std::string, double>> mem_allocations;
        std::vector<std::pair<std::string, double>> disk_allocations;
        std::vector<std::tuple<std::string, std::string, double>> bw_allocations;
    };
    std::unordered_map<std::string, ResourceSnapshot> deployments_;
    
    std::string make_link_key(const std::string& src, const std::string& dst) const;
};

} // namespace sfc
