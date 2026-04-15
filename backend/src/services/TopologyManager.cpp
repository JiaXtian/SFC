#include "services/TopologyManager.h"
#include <cmath>
#include <random>
#include <chrono>
#include <map>
#include <algorithm>
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
    
    std::vector<int> sats_per_plane(static_cast<size_t>(std::max(1, num_planes)), 0);
    if (num_planes > 0) {
        for (int i = 0; i < num_planes; ++i) {
            sats_per_plane[static_cast<size_t>(i)] = total_sats / num_planes;
        }
        for (int i = 0; i < (total_sats % num_planes); ++i) {
            sats_per_plane[static_cast<size_t>(i)] += 1;
        }
    }
    
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> cpu_dist(12.0, 24.0);
    std::uniform_real_distribution<double> mem_dist(24.0, 48.0);
    std::uniform_real_distribution<double> disk_dist(120.0, 360.0);
    
    // 生成卫星
    for (int plane = 0; plane < num_planes; ++plane) {
        const int sats_in_plane = sats_per_plane[static_cast<size_t>(plane)];
        if (sats_in_plane <= 0) continue;
        for (int pos = 0; pos < sats_in_plane; ++pos) {
            Satellite sat;
            
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "SAT_%03d_%03d", plane, pos);
            sat.id = id_buf;

            sat.orbital_params.plane = plane;
            sat.orbital_params.position_in_plane = pos;
            sat.orbital_params.raan = (360.0 / num_planes) * plane;
            sat.orbital_params.true_anomaly = (360.0 / sats_in_plane) * pos;
            sat.orbital_params.altitude_km = altitude_km;
            sat.orbital_params.inclination_deg = inclination_deg;
            
            sat.coordinates = calculate_position(plane, pos, num_planes, sats_in_plane, altitude_km);
            
            sat.cpu_total = cpu_dist(rng);
            sat.cpu_available = sat.cpu_total;
            sat.mem_total = mem_dist(rng);
            sat.mem_available = sat.mem_total;
            sat.disk_total = disk_dist(rng);
            sat.disk_available = sat.disk_total;
            sat.core_network_load = 0.5;
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
    topology.links.clear();
    std::map<int, std::vector<int>> plane_to_indices;
    for (size_t i = 0; i < topology.nodes.size(); ++i) {
        plane_to_indices[topology.nodes[i].orbital_params.plane].push_back(static_cast<int>(i));
    }
    for (auto& kv : plane_to_indices) {
        auto& indices = kv.second;
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            return topology.nodes[a].orbital_params.position_in_plane <
                   topology.nodes[b].orbital_params.position_in_plane;
        });
    }

    std::vector<int> plane_ids;
    plane_ids.reserve(plane_to_indices.size());
    for (const auto& kv : plane_to_indices) plane_ids.push_back(kv.first);

    for (size_t pi = 0; pi < plane_ids.size(); ++pi) {
        const auto& current_plane_indices = plane_to_indices[plane_ids[pi]];
        if (current_plane_indices.size() <= 1) continue;

        for (size_t pos = 0; pos < current_plane_indices.size(); ++pos) {
            const int current_idx = current_plane_indices[pos];
            const int next_idx = current_plane_indices[(pos + 1) % current_plane_indices.size()];

            Link intra_link;
            intra_link.source = topology.nodes[current_idx].id;
            intra_link.target = topology.nodes[next_idx].id;
            intra_link.link_type = "intra_orbit";
            intra_link.status = "active";

            const double distance = calculate_distance(
                topology.nodes[current_idx].coordinates,
                topology.nodes[next_idx].coordinates
            );
            intra_link.latency_ms = calculate_latency(distance);
            intra_link.reliability = 0.9992;
            intra_link.bandwidth_gbps = 16.0;
            intra_link.bandwidth_available_gbps = 15.2;
            topology.links.push_back(intra_link);
        }
    }

    for (size_t pi = 0; pi + 1 < plane_ids.size(); ++pi) {
        const auto& plane_a = plane_to_indices[plane_ids[pi]];
        const auto& plane_b = plane_to_indices[plane_ids[pi + 1]];
        if (plane_a.empty() || plane_b.empty()) continue;
        for (size_t i = 0; i < plane_a.size(); ++i) {
            const int current_idx = plane_a[i];
            const size_t mapped = static_cast<size_t>(
                std::floor((static_cast<double>(i) / std::max<size_t>(1, plane_a.size())) * plane_b.size())
            ) % plane_b.size();
            const int neighbor_idx = plane_b[mapped];

            Link inter_link;
            inter_link.source = topology.nodes[current_idx].id;
            inter_link.target = topology.nodes[neighbor_idx].id;
            inter_link.link_type = "inter_orbit";
            inter_link.status = "active";

            const double inter_distance = calculate_distance(
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
