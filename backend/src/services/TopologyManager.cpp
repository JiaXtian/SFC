#include "services/TopologyManager.h"
#include <cmath>
#include <random>
#include <chrono>
#include <spdlog/spdlog.h>

namespace sfc {

const double EARTH_RADIUS = 6371.0;  // km
const double PI = 3.14159265358979323846;
const double LIGHT_SPEED = 299792.458;  // km/s

TopologyManager::TopologyManager() {
    spdlog::info("TopologyManager initialized (no default topology)");

    std::lock_guard<std::mutex> lock(topology_mutex_);
    current_topology_.metadata.total_sats = 0;
    current_topology_.metadata.num_planes = 0;
    current_topology_.metadata.altitude_km = 0.0;
    current_topology_.metadata.inclination_deg = 0.0;
    current_topology_.metadata.topology_version = 0;
    current_topology_.metadata.sampling_interval_sec = 5.0;
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
    topo.metadata.sampling_interval_sec = 5.0;
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char buf[100];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t));
    topo.metadata.timestamp = buf;
    
    int sats_per_plane = total_sats / num_planes;
    
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> cpu_dist(12.0, 24.0);
    std::uniform_real_distribution<double> mem_dist(24.0, 48.0);
    std::uniform_real_distribution<double> disk_dist(120.0, 360.0);
    std::uniform_real_distribution<double> signaling_load_dist(0.18, 0.72);
    std::uniform_real_distribution<double> session_load_dist(0.20, 0.76);
    std::uniform_real_distribution<double> user_plane_load_dist(0.22, 0.82);
    std::uniform_real_distribution<double> mobility_load_dist(0.15, 0.70);
    std::uniform_real_distribution<double> policy_load_dist(0.16, 0.68);
    std::uniform_real_distribution<double> auth_load_dist(0.14, 0.66);
    
    // 生成卫星
    for (int plane = 0; plane < num_planes; ++plane) {
        for (int pos = 0; pos < sats_per_plane; ++pos) {
            Satellite sat;
            
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "SAT_%03d_%03d", plane, pos);
            sat.id = id_buf;

            sat.orbital_params.plane = plane;
            sat.orbital_params.position_in_plane = pos;
            sat.orbital_params.raan = (360.0 / num_planes) * plane;
            sat.orbital_params.true_anomaly = (360.0 / sats_per_plane) * pos;
            sat.orbital_params.altitude_km = altitude_km;
            sat.orbital_params.inclination_deg = inclination_deg;
            
            sat.coordinates = calculate_position(plane, pos, num_planes, sats_per_plane, altitude_km);
            
            sat.cpu_total = cpu_dist(rng);
            sat.cpu_available = sat.cpu_total;
            sat.mem_total = mem_dist(rng);
            sat.mem_available = sat.mem_total;
            sat.disk_total = disk_dist(rng);
            sat.disk_available = sat.disk_total;
            sat.core_business_load.signaling_load = signaling_load_dist(rng);
            sat.core_business_load.session_load = session_load_dist(rng);
            sat.core_business_load.user_plane_load = user_plane_load_dist(rng);
            sat.core_business_load.mobility_load = mobility_load_dist(rng);
            sat.core_business_load.policy_load = policy_load_dist(rng);
            sat.core_business_load.auth_load = auth_load_dist(rng);
            sat.core_business_load.normalize_inplace();
            sat.core_network_load = sat.core_business_load.load_index();
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

Coordinates TopologyManager::calculate_position(
    int plane_id, int sat_in_plane, int total_planes,
    int sats_per_plane, double altitude_km, double time_offset
) {
    Coordinates coord;
    double orbit_radius = EARTH_RADIUS + altitude_km;
    double raan = (2.0 * PI / total_planes) * plane_id;
    double true_anomaly = (2.0 * PI / sats_per_plane) * sat_in_plane + time_offset;
    
    coord.x = orbit_radius * cos(true_anomaly) * cos(raan);
    coord.y = orbit_radius * cos(true_anomaly) * sin(raan);
    coord.z = orbit_radius * sin(true_anomaly);
    coord.lat = asin(coord.z / orbit_radius) * 180.0 / PI;
    coord.lon = atan2(coord.y, coord.x) * 180.0 / PI;
    
    return coord;
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
            
            // 跨轨道链路
            if (plane < num_planes - 1) {
                int next_plane = plane + 1;
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
