import { useEffect, useMemo } from 'react'
import * as THREE from 'three'
import { Line } from '@react-three/drei'
import { useStore } from '@/store/useStore'
import { shallow } from 'zustand/shallow'

const EARTH_R = 5
const KM_TO_U = EARTH_R / 6371
const MAX_RENDER_HIGH = 18000
const MAX_RENDER_BALANCED = 12000
const MAX_RENDER_PERF = 6000

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
      const faultTag = String(link?.fault_tag ?? '')
      const isFault = status === 'down' && faultTag !== 'line_of_sight_loss' && faultTag !== 'topology_inconsistent'

      if (!display.showLinks && !isSelected && !isFault) continue

      if (isSelected) selected.push(link)
      else if (isFault) fault.push(link)
      else if (link.link_type === 'intra_orbit') normalIntra.push(link)
      else normalInter.push(link)
    }

    const normalCount = normalIntra.length + normalInter.length
    let maxRenderNormal = display.renderQuality === 'performance'
      ? MAX_RENDER_PERF
      : (display.renderQuality === 'balanced' ? MAX_RENDER_BALANCED : MAX_RENDER_HIGH)
    if (satellites.length > 3200) {
      maxRenderNormal = Math.floor(maxRenderNormal * 0.72)
    } else if (satellites.length > 1800) {
      maxRenderNormal = Math.floor(maxRenderNormal * 0.58)
    } else if (satellites.length > 900) {
      maxRenderNormal = Math.floor(maxRenderNormal * 0.48)
    }
    if (normalCount > maxRenderNormal) {
      const stride = Math.ceil(normalCount / Math.max(1, maxRenderNormal))
      const sampledIntra: RenderLink[] = []
      const sampledInter: RenderLink[] = []
      normalIntra.forEach((l, i) => { if (i % stride === 0) sampledIntra.push(l) })
      normalInter.forEach((l, i) => { if (i % stride === 0) sampledInter.push(l) })
      return { normalIntra: sampledIntra, normalInter: sampledInter, fault, selected }
    }

    return { normalIntra, normalInter, fault, selected }
  }, [links, display.showLinks, selectedKey, display.renderQuality, satellites.length])

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
      const avail = Number(l?.bandwidth_available_gbps ?? l?.bandwidth_gbps ?? 0)
      if (status === 'down' || avail <= 0) return
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
        raycaster.params.Line.threshold = 0.015
        THREE.LineSegments.prototype.raycast.call(this, raycaster, intersects)
        raycaster.params.Line.threshold = prev
      },
    []
  )

  const handleLinkPick = (item: { links: RenderLink[] } | null, e: any) => {
    if (!item) return
    const ray: THREE.Ray | undefined = e?.ray
    if (!ray) return
    const a = new THREE.Vector3()
    const b = new THREE.Vector3()
    const pt = new THREE.Vector3()
    let best: RenderLink | null = null
    let bestD = Number.POSITIVE_INFINITY
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
      }
    }
    // Keep threshold tight to prioritize satellite picking when near nodes.
    if (!Number.isFinite(bestD) || Math.sqrt(bestD) > 0.018) return
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
          raycast={raycastNormalLink}
          onClick={(e) => handleLinkPick(meshes.intra, e)}
        >
          <lineBasicMaterial color="#4dfa7d" transparent opacity={display.linkOpacity * 0.9} depthWrite={false} />
        </lineSegments>
      )}
      {meshes.inter && (
        <lineSegments
          geometry={meshes.inter.geometry}
          raycast={raycastNormalLink}
          onClick={(e) => handleLinkPick(meshes.inter, e)}
        >
          <lineBasicMaterial color="#2ed96f" transparent opacity={display.linkOpacity * 0.78} depthWrite={false} />
        </lineSegments>
      )}
      {meshes.fault && (
        <lineSegments
          geometry={meshes.fault.geometry}
          raycast={raycastNormalLink}
          onClick={(e) => handleLinkPick(meshes.fault, e)}
        >
          <lineBasicMaterial color="#ff3b30" transparent opacity={Math.max(0.65, display.linkOpacity * 0.95)} depthWrite={false} />
        </lineSegments>
      )}

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
        const status = String((item.link as any)?.status ?? 'active')
        const isFault = status === 'down'
        return (
        <group key={item.key}>
          <Line
            points={item.points}
            color={isFault ? '#ff6b63' : '#7df9ff'}
            lineWidth={4.5}
            transparent
            opacity={1}
            raycast={() => null}
          />
          <Line
            points={item.points}
            color={isFault ? '#ff3b30' : '#22d3ee'}
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
