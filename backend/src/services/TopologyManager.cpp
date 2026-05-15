#include "services/TopologyManager.h"
#include "utils/Sgp4Propagator.h"
#include <cmath>
#include <random>
#include <chrono>
#include <spdlog/spdlog.h>

namespace sfc {

const double LIGHT_SPEED = 299792.458;  // km/s

TopologyManager::TopologyManager() {
    spdlog::info("TopologyManager initialized (no default topology)");

    std::lock_guard<std::mutex> lock(topology_mutex_);
    current_topology_.metadata.total_sats = 0;
    current_topology_.metadata.num_planes = 0;
    current_topology_.metadata.altitude_km = 0.0;
    current_topology_.metadata.inclination_deg = 0.0;
    current_topology_.metadata.topology_version = 0;
    current_topology_.metadata.sampling_interval_sec = 15.0;
    current_topology_.metadata.sim_time = "";
    current_topology_.metadata.timestamp = "1970-01-01T00:00:00Z";
}

Topology TopologyManager::generate_walker_delta(
    int total_sats,
    int num_planes,
    double altitude_km,
    double inclination_deg,
    int seed
) {
    std::lock_guard<std::mutex> lock(topology_mutex_);
    
    spdlog::info("Generating Walker-Delta constellation: {} sats, {} planes, {} km altitude",
                total_sats, num_planes, altitude_km);
    
    Topology topo;
    topo.metadata.total_sats = total_sats;
    topo.metadata.num_planes = num_planes;
    topo.metadata.altitude_km = altitude_km;
    topo.metadata.inclination_deg = inclination_deg;
    topo.metadata.topology_version = 0;
    topo.metadata.sampling_interval_sec = 15.0;
    
    const double epoch_jd = sgp4::julian_date_now();
    topo.metadata.timestamp = sgp4::iso_utc_now();
    
    int sats_per_plane = total_sats / num_planes;
    
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> cpu_dist(12.0, 24.0);
    std::uniform_real_distribution<double> mem_dist(24.0, 48.0);
    std::uniform_real_distribution<double> disk_dist(120.0, 360.0);
    
    // 生成卫星
    for (int plane = 0; plane < num_planes; ++plane) {
        for (int pos = 0; pos < sats_per_plane; ++pos) {
            Satellite sat;
            
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "SAT_%03d_%03d", plane, pos);
            sat.id = id_buf;

            sat.orbital_params = sgp4::make_walker_sgp4_params(
                plane,
                pos,
                num_planes,
                sats_per_plane,
                altitude_km,
                inclination_deg,
                1,
                epoch_jd
            );
            sat.coordinates = calculate_position(sat.orbital_params);
            
            sat.cpu_total = cpu_dist(rng);
            sat.cpu_available = sat.cpu_total;
            sat.mem_total = mem_dist(rng);
            sat.mem_available = sat.mem_total;
            sat.disk_total = disk_dist(rng);
            sat.disk_available = sat.disk_total;
            sat.core_business_load = CoreBusinessLoad{};
            sat.core_business_load.signaling_load = 0.0;
            sat.core_business_load.session_load = 0.0;
            sat.core_business_load.user_plane_load = 0.0;
            sat.core_business_load.mobility_load = 0.0;
            sat.core_business_load.policy_load = 0.0;
            sat.core_business_load.auth_load = 0.0;
            sat.core_network_load = 0.0;
            sat.node_reliability = 0.98;
            sat.status = "active";
            sat.fault_tag.clear();
            
            topo.nodes.push_back(sat);
        }
    }
    
    establish_links(topo);
    current_topology_ = topo;
    
    spdlog::info("✓ Generated and set as current: {} nodes, {} links",
                topo.nodes.size(), topo.links.size());
    
    return topo;
}

Coordinates TopologyManager::calculate_position(const OrbitalParams& params, double minutes_since_epoch) {
    return sgp4::propagate(params, minutes_since_epoch).coordinates;
}

void TopologyManager::establish_links(Topology& topology) {
    int num_planes = topology.metadata.num_planes;
    int total_sats = topology.metadata.total_sats;
    int sats_per_plane = total_sats / num_planes;
    
    for (int plane = 0; plane < num_planes; ++plane) {
        for (int pos = 0; pos < sats_per_plane; ++pos) {
            int current_idx = plane * sats_per_plane + pos;
            
            // 同轨道内链路
            int next_in_plane = (pos + 1) % sats_per_plane;
            int next_idx = plane * sats_per_plane + next_in_plane;
            
            Link intra_link;
            intra_link.source = topology.nodes[current_idx].id;
            intra_link.target = topology.nodes[next_idx].id;
            intra_link.link_type = "intra_orbit";
            intra_link.status = "active";
            
            double distance = calculate_distance(
                topology.nodes[current_idx].coordinates,
                topology.nodes[next_idx].coordinates
            );
            intra_link.latency_ms = calculate_latency(distance);
            intra_link.reliability = 0.9992;
            intra_link.bandwidth_gbps = 16.0;
            intra_link.bandwidth_available_gbps = 15.2;
            
            topology.links.push_back(intra_link);
            
            // 跨轨道链路：相邻轨道面形成闭环，避免最后一个轨道面缺少实时跨轨 ISL。
            if (num_planes > 1 && !(num_planes == 2 && plane == 1)) {
                int next_plane = (plane + 1) % num_planes;
                int neighbor_idx = next_plane * sats_per_plane + pos;
                
                Link inter_link;
                inter_link.source = topology.nodes[current_idx].id;
                inter_link.target = topology.nodes[neighbor_idx].id;
                inter_link.link_type = "inter_orbit";
                inter_link.status = "active";
                
                double inter_distance = calculate_distance(
                    topology.nodes[current_idx].coordinates,
                    topology.nodes[neighbor_idx].coordinates
                );
                inter_link.latency_ms = calculate_latency(inter_distance);
                inter_link.reliability = 0.9985;
                inter_link.bandwidth_gbps = 12.0;
                inter_link.bandwidth_available_gbps = 10.8;
                
                topology.links.push_back(inter_link);
            }
        }
    }
}

double TopologyManager::calculate_distance(const Coordinates& a, const Coordinates& b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    double dz = a.z - b.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
}

double TopologyManager::calculate_latency(double distance_km) {
    return (distance_km / LIGHT_SPEED) * 1000.0;
}

Topology TopologyManager::get_current_topology() const {
    std::lock_guard<std::mutex> lock(topology_mutex_);
    return current_topology_;
}

void TopologyManager::update_topology(const Topology& topology) {
    std::lock_guard<std::mutex> lock(topology_mutex_);
    current_topology_ = topology;
    spdlog::info("Topology updated: {} nodes, {} links", 
                topology.nodes.size(), topology.links.size());
}

void TopologyManager::save_current_topology(const Topology& topology) {
    std::lock_guard<std::mutex> lock(topology_mutex_);
    current_topology_ = topology;
    spdlog::info("Topology saved: {} nodes, {} links", 
                topology.nodes.size(), topology.links.size());
}

} // namespace sfc
