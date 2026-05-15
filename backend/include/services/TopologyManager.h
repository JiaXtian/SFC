#pragma once
#include "models/types.h"
#include <memory>
#include <mutex>

namespace sfc {

class TopologyManager {
public:
    TopologyManager();
    
    // 生成Walker-Delta星座
    Topology generate_walker_delta(
        int total_sats,
        int num_planes,
        double altitude_km,
        double inclination_deg,
        int seed = 42
    );
    
    // 获取当前拓扑
    Topology get_current_topology() const;
    
    // 更新拓扑（线程安全）
    void update_topology(const Topology& topology);
    
    // 保存当前拓扑（用于资源更新后）
    void save_current_topology(const Topology& topology);
    
    // 使用 SGP4 平均轨道根数计算卫星位置
    Coordinates calculate_position(const OrbitalParams& params, double minutes_since_epoch = 0.0);
    
private:
    mutable std::mutex topology_mutex_;
    Topology current_topology_;
    
    void establish_links(Topology& topology);
    double calculate_distance(const Coordinates& a, const Coordinates& b);
    double calculate_latency(double distance_km);
};

} // namespace sfc
