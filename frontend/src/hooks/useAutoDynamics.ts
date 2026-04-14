import { useEffect, useRef } from 'react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'

const EARTH_RADIUS_KM = 6371
const LIGHT_SPEED_KM_S = 299792.458
const MU_EARTH = 398600.4418

type Vec3 = { x: number; y: number; z: number }
type LinkStatus = 'active' | 'congested' | 'down'

function norm(v: Vec3) {
  return Math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
}

function toLatLon(v: Vec3) {
  const r = Math.max(1e-9, norm(v))
  const lat = Math.asin(Math.max(-1, Math.min(1, v.z / r))) * 180 / Math.PI
  const lon = Math.atan2(v.y, v.x) * 180 / Math.PI
  return { lat, lon }
}

function satPositionAtTime(
  altitudeKm: number,
  inclinationDeg: number,
  raanDeg: number,
  trueAnomalyDeg0: number,
  elapsedSec: number
) {
  const r = EARTH_RADIUS_KM + altitudeKm
  const inc = (inclinationDeg * Math.PI) / 180
  const raan = (raanDeg * Math.PI) / 180
  const n = Math.sqrt(MU_EARTH / (r * r * r))
  const ta = ((trueAnomalyDeg0 * Math.PI) / 180 + n * elapsedSec) % (2 * Math.PI)

  const cosO = Math.cos(raan)
  const sinO = Math.sin(raan)
  const cosI = Math.cos(inc)
  const sinI = Math.sin(inc)
  const cosV = Math.cos(ta)
  const sinV = Math.sin(ta)
  const xOrb = r * cosV
  const yOrb = r * sinV

  const x = cosO * xOrb - sinO * cosI * yOrb
  const y = sinO * xOrb + cosO * cosI * yOrb
  const z = sinI * yOrb
  return { pos: { x, y, z }, trueAnomalyDeg: ((ta * 180) / Math.PI + 360) % 360 }
}

function minDistanceToOriginSegment(a: Vec3, b: Vec3) {
  const ab = { x: b.x - a.x, y: b.y - a.y, z: b.z - a.z }
  const ab2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z
  if (ab2 < 1e-9) return norm(a)
  const tRaw = -(a.x * ab.x + a.y * ab.y + a.z * ab.z) / ab2
  const t = Math.max(0, Math.min(1, tRaw))
  const p = { x: a.x + ab.x * t, y: a.y + ab.y * t, z: a.z + ab.z * t }
  return norm(p)
}

function hasLineOfSight(a: Vec3, b: Vec3) {
  return minDistanceToOriginSegment(a, b) > EARTH_RADIUS_KM + 55
}

function maxRangeKm(r1: number, r2: number) {
  const h1 = Math.sqrt(Math.max(0, r1 * r1 - EARTH_RADIUS_KM * EARTH_RADIUS_KM))
  const h2 = Math.sqrt(Math.max(0, r2 * r2 - EARTH_RADIUS_KM * EARTH_RADIUS_KM))
  return (h1 + h2) * 0.98
}

function practicalIslRangeKm(
  r1: number,
  r2: number,
  linkType: 'intra_orbit' | 'inter_orbit'
) {
  const h1 = Math.max(0, r1 - EARTH_RADIUS_KM)
  const h2 = Math.max(0, r2 - EARTH_RADIUS_KM)
  const hMin = Math.min(h1, h2)
  const physicsLimit = maxRangeKm(r1, r2)
  const engineeringLimit = linkType === 'inter_orbit'
    ? Math.min(3200, 1500 + 1.0 * hMin)
    : Math.min(3800, 1800 + 1.2 * hMin)
  return Math.min(physicsLimit, engineeringLimit)
}

function linkKey(a: string, b: string) {
  return `${a}|${b}`
}

function pairKey(a: string, b: string) {
  return a < b ? `${a}|${b}` : `${b}|${a}`
}

function clamp(minVal: number, maxVal: number, v: number) {
  return Math.max(minVal, Math.min(maxVal, v))
}

function distanceKm(a: Vec3, b: Vec3) {
  const dx = b.x - a.x
  const dy = b.y - a.y
  const dz = b.z - a.z
  return Math.sqrt(dx * dx + dy * dy + dz * dz)
}

function buildDynamicLinkPlan(sats: any[], prevLinks: any[]) {
  const dedup = new Set<string>()
  const out: Array<{ source: string; target: string; link_type: 'intra_orbit' | 'inter_orbit' }> = []
  const add = (source: string, target: string, linkType: 'intra_orbit' | 'inter_orbit') => {
    if (!source || !target || source === target) return
    const k = pairKey(source, target)
    if (dedup.has(k)) return
    dedup.add(k)
    out.push({ source, target, link_type: linkType })
  }

  // Keep backend-provided link pairing stable; otherwise inter-orbit links may be remapped
  // frame-to-frame and lose resource-accounting consistency.
  if (Array.isArray(prevLinks) && prevLinks.length > 0) {
    prevLinks.forEach((l: any) => {
      const source = String(l?.source ?? '')
      const target = String(l?.target ?? '')
      const linkType: 'intra_orbit' | 'inter_orbit' =
        String(l?.link_type ?? '') === 'intra_orbit' ? 'intra_orbit' : 'inter_orbit'
      add(source, target, linkType)
    })
    if (out.length > 0) return out
  }

  // Fallback for the very first local frame when no links exist yet.
  const byPlane = new Map<number, any[]>()
  sats.forEach((sat) => {
    const plane = Number(sat?.orbital_params?.plane ?? 0)
    if (!byPlane.has(plane)) byPlane.set(plane, [])
    byPlane.get(plane)!.push(sat)
  })
  byPlane.forEach((arr) => {
    arr.sort((a, b) => {
      const pa = Number(a?.orbital_params?.position_in_plane ?? 0)
      const pb = Number(b?.orbital_params?.position_in_plane ?? 0)
      return pa - pb
    })
  })
  const planeIds = Array.from(byPlane.keys()).sort((a, b) => a - b)

  byPlane.forEach((arr) => {
    const n = arr.length
    if (n < 2) return
    for (let i = 0; i < n; i++) {
      add(String(arr[i]?.id ?? ''), String(arr[(i + 1) % n]?.id ?? ''), 'intra_orbit')
    }
  })

  for (let p = 0; p < planeIds.length; p++) {
    const aPlane = byPlane.get(planeIds[p]) ?? []
    const bPlane = byPlane.get(planeIds[(p + 1) % planeIds.length]) ?? []
    const n = Math.min(aPlane.length, bPlane.length)
    for (let i = 0; i < n; i++) {
      add(String(aPlane[i]?.id ?? ''), String(bPlane[i]?.id ?? ''), 'inter_orbit')
    }
  }
  return out
}

function remapSelectedLink(selectedLink: any, links: any[]) {
  if (!selectedLink) return null
  const src = String(selectedLink?.source ?? '')
  const dst = String(selectedLink?.target ?? '')
  if (!src || !dst) return null
  const found = links.find((l: any) => {
    const a = String(l?.source ?? '')
    const b = String(l?.target ?? '')
    return (a === src && b === dst) || (a === dst && b === src)
  })
  return found ?? null
}

function remapSelectedSatellite(selectedSatellite: any, satellites: any[]) {
  if (!selectedSatellite) return null
  const selectedId = String(selectedSatellite?.id ?? '')
  if (!selectedId) return null
  const found = satellites.find((s: any) => String(s?.id ?? '') === selectedId)
  return found ?? null
}

export function useAutoDynamics() {
  const rafRef = useRef<number | null>(null)
  const lastTsRef = useRef<number>(performance.now())
  const posAccRef = useRef(0)
  const resAccRef = useRef(0)
  const pathRefreshAccRef = useRef(0)
  const resourceSyncRunningRef = useRef(false)
  const clockEpochMsRef = useRef<number>(Date.now())

  useEffect(() => {
    const syncResources = async () => {
      if (resourceSyncRunningRef.current) return
      resourceSyncRunningRef.current = true
      try {
        const topo = await apiClient.getTopology()
        const topology = topo?.topology ?? topo
        const nodes = Array.isArray(topology?.nodes) ? topology.nodes : []
        const links = Array.isArray(topology?.links) ? topology.links : []

        useStore.setState((s) => {
          const nodeMap = new Map<string, any>()
          nodes.forEach((n: any) => nodeMap.set(String(n.id), n))
          const mergedNodes = s.satellites.map((sat: any) => {
            const src = nodeMap.get(String(sat.id))
            if (!src) return sat
            return {
              ...sat,
              cpu_total: Number(src.cpu_total ?? sat.cpu_total),
              cpu_available: Number(src.cpu_available ?? sat.cpu_available),
              mem_total: Number(src.mem_total ?? sat.mem_total),
              mem_available: Number(src.mem_available ?? sat.mem_available),
              disk_total: Number(src.disk_total ?? sat.disk_total),
              disk_available: Number(src.disk_available ?? sat.disk_available),
              core_network_load: Number(src.core_network_load ?? sat.core_network_load ?? 0),
              node_reliability: Number(src.node_reliability ?? sat.node_reliability ?? 0.98),
              status: String(src.status ?? 'active'),
              fault_tag: String(src.fault_tag ?? ''),
              vnfs: Array.isArray(src.core_nfs) ? src.core_nfs : (Array.isArray(src.vnfs) ? src.vnfs : sat.vnfs),
            }
          })

          const linkMap = new Map<string, any>()
          links.forEach((l: any) => {
            const a = String(l.source ?? '')
            const b = String(l.target ?? '')
            linkMap.set(linkKey(a, b), l)
            linkMap.set(linkKey(b, a), l)
          })
          const mergedLinks = s.links.map((l: any) => {
            const src = linkMap.get(linkKey(String(l.source), String(l.target)))
            if (!src) return l
            const resourceStatus = String(src.status ?? l.__resource_status ?? l.status ?? 'active')
            const visualStatus = String(l.status ?? 'active')
            return {
              ...l,
              bandwidth_gbps: Number(src.bandwidth_gbps ?? l.bandwidth_gbps ?? 0),
              bandwidth_available_gbps: Number(src.bandwidth_available_gbps ?? l.bandwidth_available_gbps ?? 0),
              reliability: Number(src.reliability ?? src.link_reliability ?? l.reliability ?? 0.999),
              fault_tag: String(src.fault_tag ?? ''),
              __resource_status: resourceStatus,
              __resource_status_seed: resourceStatus,
              // Keep visual state stable between resource sync ticks to avoid flicker.
              status: visualStatus,
            }
          })
          const selectedLink = remapSelectedLink(s.selectedLink, mergedLinks)
          const selectedSatellite = remapSelectedSatellite(s.selectedSatellite, mergedNodes)
          return {
            satellites: mergedNodes,
            links: mergedLinks,
            selectedSatellite,
            selectedLink,
            autoDynamics: {
              ...s.autoDynamics,
              last_resource_sync_at: new Date().toISOString(),
            },
          }
        })
        if (useStore.getState().deployments.length > 0) {
          useStore.getState().refreshDeploymentPaths()
        }
      } catch {
        // ignore transient sync failures
      } finally {
        resourceSyncRunningRef.current = false
      }
    }

    const frame = (ts: number) => {
      const s = useStore.getState()
      const ad = s.autoDynamics
      const dtReal = Math.max(0, (ts - lastTsRef.current) / 1000)
      lastTsRef.current = ts

      if (ad.enabled && !ad.playing && ad.auto_start_on_topology && ad.elapsed_sec <= 0 && s.satellites.length > 0) {
        useStore.getState().setAutoDynamics({ playing: true })
      }

      if (ad.enabled && ad.playing && s.satellites.length > 0) {
        const satCount = s.satellites.length
        const posInterval = satCount >= 5000 ? 0.18 : (satCount >= 3000 ? 0.13 : 0.1)
        posAccRef.current += dtReal
        resAccRef.current += dtReal

        if (posAccRef.current >= posInterval) {
          const stepReal = posAccRef.current
          posAccRef.current = 0
          const elapsedSec = ad.elapsed_sec + stepReal * Math.max(0.1, ad.time_scale)

          if (ad.elapsed_sec === 0) {
            clockEpochMsRef.current = Date.now()
          }

          const posMap = new Map<string, Vec3>()
          const newSats = s.satellites.map((sat: any) => {
            const op = sat.orbital_params ?? {}
            const altitudeKm = Number(op.altitude_km ?? 550)
            const inclinationDeg = Number(op.inclination_deg ?? op.inclination ?? 53)
            const raanDeg = Number(op.raan ?? 0)
            const ta0 = Number((sat as any).__base_true_anomaly ?? op.true_anomaly ?? 0)
            const { pos, trueAnomalyDeg } = satPositionAtTime(
              altitudeKm,
              inclinationDeg,
              raanDeg,
              ta0,
              elapsedSec
            )
            posMap.set(String(sat.id), pos)
            const ll = toLatLon(pos)
            return {
              ...sat,
              __base_true_anomaly: ta0,
              coordinates: { ...sat.coordinates, ...pos, lat: ll.lat, lon: ll.lon },
              orbital_params: { ...sat.orbital_params, true_anomaly: trueAnomalyDeg },
            }
          })

          const prevByPair = new Map<string, any>()
          s.links.forEach((l: any) => {
            const src = String(l?.source ?? '')
            const dst = String(l?.target ?? '')
            if (!src || !dst) return
            const pk = pairKey(src, dst)
            prevByPair.set(pk, l)
          })
          const satStatusById = new Map<string, string>()
          newSats.forEach((sat: any) => {
            satStatusById.set(String(sat?.id ?? ''), String(sat?.status ?? 'active'))
          })
          const dynamicPlan = buildDynamicLinkPlan(newSats, s.links)
          const avgBw = { intra: 18, inter: 12 }
          {
            let intraSum = 0
            let interSum = 0
            let intraCnt = 0
            let interCnt = 0
            s.links.forEach((l: any) => {
              const bw = Number(l?.bandwidth_gbps ?? 0)
              if (bw <= 0) return
              if (String(l?.link_type ?? '') === 'intra_orbit') {
                intraSum += bw
                intraCnt += 1
              } else if (String(l?.link_type ?? '') === 'inter_orbit') {
                interSum += bw
                interCnt += 1
              }
            })
            if (intraCnt > 0) avgBw.intra = intraSum / intraCnt
            if (interCnt > 0) avgBw.inter = interSum / interCnt
          }

          const newLinks = dynamicPlan.map((lk: any) => {
            const src = String(lk.source)
            const dst = String(lk.target)
            const linkType: 'intra_orbit' | 'inter_orbit' = String(lk?.link_type) === 'intra_orbit' ? 'intra_orbit' : 'inter_orbit'
            const a = posMap.get(src)
            const b = posMap.get(dst)
            const prev = prevByPair.get(pairKey(src, dst))
            if (!a || !b) {
              const fallbackBw = Number(prev?.bandwidth_gbps ?? (linkType === 'intra_orbit' ? avgBw.intra : avgBw.inter))
            return {
              source: src,
              target: dst,
              link_type: linkType,
              bandwidth_gbps: fallbackBw,
              bandwidth_available_gbps: 0,
              reliability: Number(prev?.reliability ?? 0.8),
              fault_tag: String(prev?.fault_tag ?? 'topology_inconsistent'),
              __resource_status_seed: String(prev?.__resource_status_seed ?? 'active'),
              __resource_status: String(prev?.__resource_status ?? prev?.status ?? 'active'),
              __dynamic_up: false,
                status: 'down' as LinkStatus,
                latency_ms: Number(prev?.latency_ms ?? 0),
              }
            }

            const d = distanceKm(a, b)
            const los = hasLineOfSight(a, b)
            const range = practicalIslRangeKm(norm(a), norm(b), linkType)
            const up = los && d <= range

            const currentStatus = String(prev?.status ?? 'active')
            const seededResourceStatus = String(
              prev?.__resource_status_seed ??
              (currentStatus === 'congested' || currentStatus === 'down' ? currentStatus : 'active')
            )
            const resourceStatus = String(prev?.__resource_status ?? seededResourceStatus)
            const nextStatus: LinkStatus = resourceStatus === 'down'
              ? 'down'
              : (up ? (resourceStatus === 'congested' ? 'congested' : 'active') : 'down')
            const endpointDown =
              satStatusById.get(src) === 'down' ||
              satStatusById.get(dst) === 'down'

            const bwTotal = Number(prev?.bandwidth_gbps ?? (linkType === 'intra_orbit' ? avgBw.intra : avgBw.inter))
            const bwAvailPrev = Number(prev?.bandwidth_available_gbps ?? bwTotal)
            const bwAvail = nextStatus === 'down'
              ? 0
              : (bwAvailPrev > 0 ? Math.min(bwAvailPrev, bwTotal) : bwTotal * 0.85)
            const distFactor = clamp(0, 1, 1 - d / Math.max(1e-6, range))
            const reliability = clamp(0.95, 0.9997, 0.985 + 0.014 * distFactor)
            return {
              source: src,
              target: dst,
              link_type: linkType,
              bandwidth_gbps: bwTotal,
              bandwidth_available_gbps: bwAvail,
              reliability,
              fault_tag: nextStatus === 'down'
                ? String(prev?.fault_tag ?? (endpointDown ? 'endpoint_node_fault' : 'line_of_sight_loss'))
                : '',
              __resource_status_seed: seededResourceStatus,
              __resource_status: resourceStatus,
              __dynamic_up: up,
              status: nextStatus,
              latency_ms: (d / LIGHT_SPEED_KM_S) * 1000,
            }
          })

          const simIso = new Date(clockEpochMsRef.current + elapsedSec * 1000).toISOString()
          useStore.setState((prev) => ({
            satellites: newSats,
            links: newLinks,
            selectedSatellite: remapSelectedSatellite(prev.selectedSatellite, newSats),
            selectedLink: remapSelectedLink(prev.selectedLink, newLinks),
            simulation: {
              ...prev.simulation,
              sim_time: simIso,
            },
            autoDynamics: {
              ...prev.autoDynamics,
              elapsed_sec: elapsedSec,
            },
          }))
          if (s.deployments.length > 0) {
            pathRefreshAccRef.current += stepReal
            const refreshInterval = satCount >= 5000 ? 1.5 : (satCount >= 4200 ? 0.9 : (satCount >= 2200 ? 0.5 : 0.16))
            if (pathRefreshAccRef.current >= refreshInterval) {
              pathRefreshAccRef.current = 0
              useStore.getState().refreshDeploymentPaths()
            }
          }
        }

        if (resAccRef.current >= Math.max(1, ad.resource_update_sec)) {
          resAccRef.current = 0
          syncResources()
        }
      }

      rafRef.current = window.requestAnimationFrame(frame)
    }

    rafRef.current = window.requestAnimationFrame(frame)
    return () => {
      if (rafRef.current != null) window.cancelAnimationFrame(rafRef.current)
    }
  }, [])
}
