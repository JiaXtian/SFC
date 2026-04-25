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

function toXYZ(x: number, y: number, z: number): [number, number, number] {
  return [x * KM_TO_U, z * KM_TO_U, -y * KM_TO_U]
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
      const key = `${link.source}|${link.target}`
      const isSelected = selectedKey.has(key)
      const status = String(link?.status ?? 'active')
      const hasFaultTag = String(link?.fault_tag ?? '').trim().length > 0
      if (status === 'down' && !hasFaultTag) continue
      if (!display.showLinks && !isSelected) continue

      if (isSelected) selected.push(link)
      else if (hasFaultTag) fault.push(link)
      else if (link.link_type === 'intra_orbit') normalIntra.push(link)
      else normalInter.push(link)
    }

    return { normalIntra, normalInter, fault, selected }
  }, [links, display.showLinks, selectedKey])

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
      const status = String(l?.status ?? 'active')
      if (status === 'down') return
      set.add(`${src}|${dst}`)
      set.add(`${dst}|${src}`)
    })
    return set
  }, [links])

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
          points: [
            toXYZ(a.coordinates.x, a.coordinates.y, a.coordinates.z),
            toXYZ(b.coordinates.x, b.coordinates.y, b.coordinates.z),
          ],
          link: l,
          color: '#ffffff',
          glowColor: '#e2f1ff',
          lineWidth: 3.6,
          opacity: 0.95,
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
      }
    }
    // Prioritize node picking when user clicks near a satellite endpoint.
    const endpointThreshold = satellites.length >= 5000 ? 0.028 : 0.036
    if (Number.isFinite(bestEndpointD) && Math.sqrt(bestEndpointD) <= endpointThreshold) return
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

      {/* Deployed SFC links: static white thick glow */}
      {highlightedLines.map(item => (
        <group key={item.key}>
          <Line
            points={item.points}
            color={item.color}
            lineWidth={heavyHighlightMode ? Math.max(2.1, item.lineWidth - 1.1) : item.lineWidth}
            transparent
            opacity={heavyHighlightMode ? Math.max(0.7, item.opacity * 0.88) : item.opacity}
            raycast={() => null}
          />
          {!heavyHighlightMode && (
            <Line
              points={item.points}
              color={item.glowColor}
              lineWidth={item.isGhost ? 6.2 : 8.5}
              transparent
              opacity={item.isGhost ? Math.max(0.08, item.opacity * 0.55) : 0.24}
              raycast={() => null}
            />
          )}
        </group>
      ))}

      {selectedLines.map(item => {
        const linkType = String((item.link as any)?.link_type ?? 'inter_orbit')
        const hasFaultTag = String((item.link as any)?.fault_tag ?? '').trim().length > 0
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
            lineWidth={4.5}
            transparent
            opacity={1}
            raycast={() => null}
          />
          <Line
            points={item.points}
            color={selectedGlow}
            lineWidth={10.5}
            transparent
            opacity={0.3}
            raycast={() => null}
          />
        </group>
        )
      })}
    </group>
  )
}
