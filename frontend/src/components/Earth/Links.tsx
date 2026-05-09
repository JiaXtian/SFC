import { useEffect, useMemo } from 'react'
import * as THREE from 'three'
import { Line } from '@react-three/drei'
import { useStore } from '@/store/useStore'
import { shallow } from 'zustand/shallow'

const EARTH_R = 5
const KM_TO_U = EARTH_R / 6371

type RenderLink = {
  source: string
  target: string
  link_type?: string
  status?: string
  fault_tag?: string
  latency_ms?: number
  bandwidth_gbps?: number
  bandwidth_available_gbps?: number
}

function isNodeFault(sat: any): boolean {
  const status = String(sat?.status ?? 'active').trim().toLowerCase()
  const faultTag = String(sat?.fault_tag ?? '').trim()
  return status === 'down' || status === 'fault' || status === 'failed' || faultTag.length > 0
}

function toXYZ(x: number, y: number, z: number): [number, number, number] {
  return [x * KM_TO_U, z * KM_TO_U, -y * KM_TO_U]
}

function bulgedLinkPoints(
  a: [number, number, number],
  b: [number, number, number],
): [number, number, number][] {
  const va = new THREE.Vector3(a[0], a[1], a[2])
  const vb = new THREE.Vector3(b[0], b[1], b[2])
  const delta = new THREE.Vector3().subVectors(vb, va)
  const length = delta.length()
  if (length < 1e-6) return [a, b]

  const start = va.clone().addScaledVector(delta, 0.055)
  const end = vb.clone().addScaledVector(delta, -0.055)
  const mid = new THREE.Vector3().addVectors(start, end).multiplyScalar(0.5)
  const outward = mid.clone().normalize()
  const bulge = Math.max(0.018, Math.min(0.11, length * 0.028))
  mid.addScaledVector(outward, bulge)
  return [
    [start.x, start.y, start.z],
    [mid.x, mid.y, mid.z],
    [end.x, end.y, end.z],
  ]
}

function createGeometry(segments: Array<{ a: [number, number, number]; b: [number, number, number] }>) {
  const arr = new Float32Array(segments.length * 6)
  let o = 0
  for (const s of segments) {
    arr[o++] = s.a[0]
    arr[o++] = s.a[1]
    arr[o++] = s.a[2]
    arr[o++] = s.b[0]
    arr[o++] = s.b[1]
    arr[o++] = s.b[2]
  }
  const geometry = new THREE.BufferGeometry()
  geometry.setAttribute('position', new THREE.BufferAttribute(arr, 3))
  geometry.computeBoundingSphere()
  return geometry
}

function isOccludedByEarth(cam: THREE.Vector3, point: THREE.Vector3): boolean {
  const occlusionR = EARTH_R + 0.02
  const dir = new THREE.Vector3().subVectors(point, cam)
  const a = dir.dot(dir)
  if (a < 1e-9) return false
  const b = 2 * cam.dot(dir)
  const c = cam.dot(cam) - occlusionR * occlusionR
  const disc = b * b - 4 * a * c
  if (disc <= 0) return false
  const s = Math.sqrt(disc)
  const t1 = (-b - s) / (2 * a)
  const t2 = (-b + s) / (2 * a)
  const eps = 1e-4
  return (t1 > eps && t1 < 1 - eps) || (t2 > eps && t2 < 1 - eps)
}

export default function Links() {
  const {
    satellites,
    links,
    display,
    deployments,
    highlightedDeploymentIds,
    selectedLink,
    setSelectedLink,
    setSelectedSatellite,
  } = useStore((s) => ({
    satellites: s.satellites,
    links: s.links,
    display: s.display,
    deployments: s.deployments,
    highlightedDeploymentIds: s.highlightedDeploymentIds,
    selectedLink: s.selectedLink,
    setSelectedLink: s.setSelectedLink,
    setSelectedSatellite: s.setSelectedSatellite,
  }), shallow)

  const satMap = useMemo(() => {
    const m = new Map<string, any>()
    satellites.forEach((s: any) => m.set(s.id, s))
    return m
  }, [satellites])

  const selectedKey = useMemo(() => {
    if (!selectedLink) return new Set<string>()
    return new Set<string>([
      `${selectedLink.source}|${selectedLink.target}`,
      `${selectedLink.target}|${selectedLink.source}`,
    ])
  }, [selectedLink])

  const groups = useMemo(() => {
    const normalIntra: RenderLink[] = []
    const normalInter: RenderLink[] = []
    const fault: RenderLink[] = []
    const selected: RenderLink[] = []

    for (const link of links as any[]) {
      const srcSat = satMap.get(String(link?.source ?? ''))
      const dstSat = satMap.get(String(link?.target ?? ''))
      const endpointDown = isNodeFault(srcSat) || isNodeFault(dstSat)
      if (endpointDown) continue

      const key = `${link.source}|${link.target}`
      const isSelected = selectedKey.has(key)
      const status = String(link?.status ?? 'active')
      const faultTag = String(link?.fault_tag ?? '').trim()
      const hasFaultTag = faultTag.length > 0
      const showAsFaultLink = hasFaultTag && faultTag !== 'endpoint_node_fault'
      if (status === 'down' && !hasFaultTag) continue
      if (!display.showLinks && !isSelected) continue

      if (isSelected) selected.push(link)
      else if (showAsFaultLink) fault.push(link)
      else if (link.link_type === 'intra_orbit') normalIntra.push(link)
      else normalInter.push(link)
    }

    return { normalIntra, normalInter, fault, selected }
  }, [links, satMap, display.showLinks, selectedKey])

  const meshes = useMemo(() => {
    const make = (items: RenderLink[]) => {
      const segs: Array<{ a: [number, number, number]; b: [number, number, number] }> = []
      const mapped: RenderLink[] = []
      for (const l of items) {
        const a = satMap.get(l.source)
        const b = satMap.get(l.target)
        if (!a || !b) continue
        segs.push({ a: toXYZ(a.coordinates.x, a.coordinates.y, a.coordinates.z), b: toXYZ(b.coordinates.x, b.coordinates.y, b.coordinates.z) })
        mapped.push(l)
      }
      if (segs.length === 0) return null
      return { geometry: createGeometry(segs), links: mapped }
    }

    return {
      intra: make(groups.normalIntra),
      inter: make(groups.normalInter),
      fault: make(groups.fault),
      selected: make(groups.selected),
    }
  }, [groups, satMap])

  const activeLinkSet = useMemo(() => {
    const set = new Set<string>()
    links.forEach((l: any) => {
      const src = String(l?.source ?? '')
      const dst = String(l?.target ?? '')
      if (!src || !dst) return
      const srcSat = satMap.get(src)
      const dstSat = satMap.get(dst)
      if (isNodeFault(srcSat) || isNodeFault(dstSat)) return
      const status = String(l?.status ?? 'active')
      if (status === 'down') return
      set.add(`${src}|${dst}`)
      set.add(`${dst}|${src}`)
    })
    return set
  }, [links, satMap])

  const highlightedLines = useMemo(() => {
    const out: Array<{
      key: string
      points: [number, number, number][]
      link: RenderLink
      color: string
      glowColor: string
      lineWidth: number
      opacity: number
      isGhost: boolean
    }> = []
    const active = new Set(highlightedDeploymentIds)
    deployments.forEach((dep: any) => {
      if (!active.has(dep.deployment_id)) return
      if (dep.satisfies_constraints === false || dep.status === 'failed') return

      const currentLinks = Array.isArray(dep.link_details) ? dep.link_details : []
      currentLinks.forEach((l: any, i: number) => {
        const lk = `${String(l?.src ?? '')}|${String(l?.dst ?? '')}`
        if (!activeLinkSet.has(lk)) return
        const a = satMap.get(l.src)
        const b = satMap.get(l.dst)
        if (!a || !b) return
        out.push({
          key: `hl-cur-${dep.deployment_id}-${l.src}-${l.dst}-${i}`,
          points: bulgedLinkPoints(
            toXYZ(a.coordinates.x, a.coordinates.y, a.coordinates.z),
            toXYZ(b.coordinates.x, b.coordinates.y, b.coordinates.z),
          ),
          link: l,
          color: '#22d3ee',
          glowColor: '#fbbf24',
          lineWidth: 1.85,
          opacity: 0.9,
          isGhost: false,
        })
      })
    })
    return out
  }, [deployments, highlightedDeploymentIds, satMap, activeLinkSet])
  const heavyHighlightMode = satellites.length >= 2600

  const selectedLines = useMemo(() => {
    const out: Array<{ key: string; points: [number, number, number][]; link: RenderLink }> = []
    groups.selected.forEach((l, i) => {
      const a = satMap.get(l.source)
      const b = satMap.get(l.target)
      if (!a || !b) return
      out.push({
        key: `sel-${l.source}-${l.target}-${i}`,
        points: [toXYZ(a.coordinates.x, a.coordinates.y, a.coordinates.z), toXYZ(b.coordinates.x, b.coordinates.y, b.coordinates.z)],
        link: l,
      })
    })
    return out
  }, [groups.selected, satMap])

  const faultLines = useMemo(() => {
    const out: Array<{ key: string; points: [number, number, number][]; link: RenderLink }> = []
    groups.fault.forEach((l, i) => {
      const a = satMap.get(l.source)
      const b = satMap.get(l.target)
      if (!a || !b) return
      out.push({
        key: `fault-${l.source}-${l.target}-${i}`,
        points: [
          toXYZ(a.coordinates.x, a.coordinates.y, a.coordinates.z),
          toXYZ(b.coordinates.x, b.coordinates.y, b.coordinates.z),
        ],
        link: l,
      })
    })
    return out
  }, [groups.fault, satMap])

  useEffect(() => {
    return () => {
      meshes.intra?.geometry.dispose()
      meshes.inter?.geometry.dispose()
      meshes.fault?.geometry.dispose()
      meshes.selected?.geometry.dispose()
    }
  }, [meshes])

  const raycastNormalLink = useMemo(
    () =>
      function raycastLineSegments(this: THREE.LineSegments, raycaster: THREE.Raycaster, intersects: THREE.Intersection[]) {
        const prev = raycaster.params.Line.threshold
        // Shrink line picking radius to avoid stealing most satellite clicks.
        raycaster.params.Line.threshold = satellites.length >= 5000 ? 0.014 : 0.018
        THREE.LineSegments.prototype.raycast.call(this, raycaster, intersects)
        raycaster.params.Line.threshold = prev
      },
    [satellites.length]
  )

  const handleLinkPick = (item: { links: RenderLink[] } | null, e: any) => {
    if (!item) return
    const ray: THREE.Ray | undefined = e?.ray
    if (!ray) return
    const a = new THREE.Vector3()
    const b = new THREE.Vector3()
    const pt = new THREE.Vector3()
    const endpoint = new THREE.Vector3()
    let best: RenderLink | null = null
    let bestEndpointSat: any = null
    let bestD = Number.POSITIVE_INFINITY
    let bestEndpointD = Number.POSITIVE_INFINITY
    const cam = ray.origin
    for (const link of item.links) {
      const s = satMap.get(link.source)
      const t = satMap.get(link.target)
      if (!s || !t) continue
      const pa = toXYZ(s.coordinates.x, s.coordinates.y, s.coordinates.z)
      const pb = toXYZ(t.coordinates.x, t.coordinates.y, t.coordinates.z)
      a.set(pa[0], pa[1], pa[2])
      b.set(pb[0], pb[1], pb[2])
      const d = ray.distanceSqToSegment(a, b, undefined, pt)
      if (isOccludedByEarth(cam, pt)) continue
      if (d < bestD) {
        bestD = d
        best = link
        endpoint.set(pa[0], pa[1], pa[2])
        const dA = ray.distanceSqToPoint(endpoint)
        endpoint.set(pb[0], pb[1], pb[2])
        const dB = ray.distanceSqToPoint(endpoint)
        bestEndpointD = Math.min(dA, dB)
        bestEndpointSat = dA <= dB ? s : t
      }
    }
    // Prioritize node picking when user clicks near a satellite endpoint.
    const endpointThreshold = satellites.length >= 5000 ? 0.04 : 0.052
    if (bestEndpointSat && Number.isFinite(bestEndpointD) && Math.sqrt(bestEndpointD) <= endpointThreshold) {
      e.stopPropagation()
      setSelectedSatellite(bestEndpointSat)
      setSelectedLink(null)
      return
    }
    // Keep threshold tight to prioritize satellite picking when near nodes.
    const clickThreshold = satellites.length >= 5000 ? 0.028 : 0.024
    if (!Number.isFinite(bestD) || Math.sqrt(bestD) > clickThreshold) return
    if (!best) return
    e.stopPropagation()
    setSelectedLink(best as any)
    setSelectedSatellite(null)
  }

  if (!meshes.intra && !meshes.inter && !meshes.fault && !meshes.selected && highlightedLines.length === 0) return null

  return (
    <group>
      {meshes.intra && (
        <lineSegments
          geometry={meshes.intra.geometry}
          frustumCulled={false}
          raycast={raycastNormalLink}
          onClick={(e) => handleLinkPick(meshes.intra, e)}
        >
          <lineBasicMaterial color="#45f08b" transparent opacity={display.linkOpacity * 0.9} depthWrite={false} />
        </lineSegments>
      )}
      {meshes.inter && (
        <lineSegments
          geometry={meshes.inter.geometry}
          frustumCulled={false}
          raycast={raycastNormalLink}
          onClick={(e) => handleLinkPick(meshes.inter, e)}
        >
          <lineBasicMaterial color="#a78bfa" transparent opacity={display.linkOpacity * 0.82} depthWrite={false} />
        </lineSegments>
      )}
      {meshes.fault && (
        <lineSegments
          geometry={meshes.fault.geometry}
          frustumCulled={false}
          visible={false}
        />
      )}

      {faultLines.map((item) => (
        <group
          key={item.key}
          onClick={(e: any) => {
            e.stopPropagation()
            setSelectedLink(item.link as any)
            setSelectedSatellite(null)
          }}
        >
          <Line
            points={item.points}
            color="#fde047"
            lineWidth={4.8}
            transparent
            opacity={Math.max(0.86, display.linkOpacity)}
          />
          <Line
            points={item.points}
            color="#f59e0b"
            lineWidth={11.5}
            transparent
            opacity={0.42}
          />
        </group>
      ))}

      {/* Deployed core-network dependency links */}
      {highlightedLines.map(item => (
        <group key={item.key}>
          <Line
            points={item.points}
            color={item.color}
            lineWidth={heavyHighlightMode ? 0.95 : 1.25}
            transparent
            opacity={heavyHighlightMode ? Math.max(0.62, item.opacity * 0.78) : item.opacity}
            depthWrite={false}
            raycast={() => null}
          />
          <Line
            points={item.points}
            color={item.glowColor}
            lineWidth={heavyHighlightMode ? 1.7 : 2.6}
            transparent
            opacity={heavyHighlightMode ? 0.1 : 0.13}
            depthWrite={false}
            raycast={() => null}
          />
        </group>
      ))}

      {selectedLines.map(item => {
        const linkType = String((item.link as any)?.link_type ?? 'inter_orbit')
        const faultTag = String((item.link as any)?.fault_tag ?? '').trim()
        const hasFaultTag = faultTag.length > 0 && faultTag !== 'endpoint_node_fault'
        const selectedCore = hasFaultTag
          ? '#fde047'
          : (linkType === 'intra_orbit' ? '#6ee7b7' : '#c4b5fd')
        const selectedGlow = hasFaultTag
          ? '#f59e0b'
          : (linkType === 'intra_orbit' ? '#10b981' : '#8b5cf6')
        return (
        <group key={item.key}>
          <Line
            points={item.points}
            color={selectedCore}
            lineWidth={2.8}
            transparent
            opacity={1}
            raycast={() => null}
          />
          <Line
            points={item.points}
            color={selectedGlow}
            lineWidth={5.8}
            transparent
            opacity={0.22}
            raycast={() => null}
          />
        </group>
        )
      })}
    </group>
  )
}
