import type { LinkData, SatelliteData } from './constellationGenerator'

function asFinite(v: any, fallback: number) {
  const n = Number(v)
  return Number.isFinite(n) ? n : fallback
}

export function buildRuntimeTopologyPayload(
  satellites: SatelliteData[],
  links: LinkData[],
  simTime?: string
) {
  const planes = new Set<number>()
  let altSum = 0
  let incSum = 0
  satellites.forEach((s: any) => {
    const op = s?.orbital_params ?? {}
    planes.add(asFinite(op.plane, 0))
    altSum += asFinite(op.altitude_km, 550)
    incSum += asFinite(op.inclination ?? op.inclination_deg, 53)
  })
  const count = Math.max(1, satellites.length)
  const altitude = altSum / count
  const inclination = incSum / count

  return {
    metadata: {
      total_sats: satellites.length,
      num_planes: Math.max(1, planes.size),
      altitude_km: altitude,
      inclination_deg: inclination,
      timestamp: simTime || new Date().toISOString(),
    },
    nodes: satellites.map((s: any) => ({
      id: String(s?.id ?? ''),
      type: 'satellite',
      orbital_params: {
        plane: asFinite(s?.orbital_params?.plane, 0),
        position_in_plane: asFinite(s?.orbital_params?.position_in_plane, 0),
        raan: asFinite(s?.orbital_params?.raan, 0),
        true_anomaly: asFinite(s?.orbital_params?.true_anomaly, 0),
        altitude_km: asFinite(s?.orbital_params?.altitude_km, 550),
        inclination: asFinite(s?.orbital_params?.inclination ?? s?.orbital_params?.inclination_deg, 53),
      },
      coordinates: {
        x: asFinite(s?.coordinates?.x, 0),
        y: asFinite(s?.coordinates?.y, 0),
        z: asFinite(s?.coordinates?.z, 0),
        lat: asFinite(s?.coordinates?.lat, 0),
        lon: asFinite(s?.coordinates?.lon, 0),
      },
      cpu_total: asFinite(s?.cpu_total, 0),
      cpu_available: asFinite(s?.cpu_available, 0),
      mem_total: asFinite(s?.mem_total, 0),
      mem_available: asFinite(s?.mem_available, 0),
      disk_total: asFinite(s?.disk_total, 0),
      disk_available: asFinite(s?.disk_available, 0),
      core_network_load: 0,
      core_business_load: {
        signaling_load: 0,
        session_load: 0,
        user_plane_load: 0,
        mobility_load: 0,
        policy_load: 0,
        auth_load: 0,
      },
      node_reliability: asFinite(s?.node_reliability, 0.985),
      vnfs: Array.isArray(s?.core_nfs) ? s.core_nfs : (Array.isArray(s?.vnfs) ? s.vnfs : []),
      core_nfs: Array.isArray(s?.core_nfs) ? s.core_nfs : (Array.isArray(s?.vnfs) ? s.vnfs : []),
      status: String(s?.status ?? 'active'),
      fault_tag: String(s?.fault_tag ?? ''),
    })),
    links: links.map((l: any) => ({
      source: String(l?.source ?? ''),
      target: String(l?.target ?? ''),
      link_type: String(l?.link_type ?? 'inter_orbit'),
      status: String(l?.status ?? 'active'),
      fault_tag: String(l?.fault_tag ?? ''),
      reliability: asFinite(l?.reliability ?? l?.link_reliability, 0.999),
      latency_ms: asFinite(l?.latency_ms, 0),
      bandwidth_gbps: asFinite(l?.bandwidth_gbps, 0),
      bandwidth_available_gbps: asFinite(l?.bandwidth_available_gbps ?? l?.bandwidth_gbps, 0),
    })),
  }
}
