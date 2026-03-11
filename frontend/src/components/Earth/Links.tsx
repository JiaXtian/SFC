import { useEffect, useMemo, useState } from 'react'
import * as THREE from 'three'
import { Line } from '@react-three/drei'
import { useStore } from '@/store/useStore'

const EARTH_R = 5
const KM_TO_U = EARTH_R / 6371
const MAX_RENDER_HIGH = 18000
const MAX_RENDER_BALANCED = 12000
const MAX_RENDER_PERF = 6000

type RenderLink = {
  source: string
  target: string
  link_type?: string
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
  } = useStore()

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
    const selected: RenderLink[] = []

    for (const link of links as any[]) {
      const key = `${link.source}|${link.target}`
      const isSelected = selectedKey.has(key)
      const status = String(link?.status ?? 'active')
      const availBw = Number(link?.bandwidth_available_gbps ?? link?.bandwidth_gbps ?? 0)

      if (!display.showLinks && !isSelected) continue
      if (!isSelected && (status === 'down' || availBw <= 0)) continue

      if (isSelected) selected.push(link)
      else if (link.link_type === 'intra_orbit') normalIntra.push(link)
      else normalInter.push(link)
    }

    const normalCount = normalIntra.length + normalInter.length
    let maxRenderNormal = display.renderQuality === 'performance'
      ? MAX_RENDER_PERF
      : (display.renderQuality === 'balanced' ? MAX_RENDER_BALANCED : MAX_RENDER_HIGH)
    if (satellites.length > 3200) {
      maxRenderNormal = Math.floor(maxRenderNormal * 0.72)
    }
    if (normalCount > maxRenderNormal) {
      const stride = Math.ceil(normalCount / Math.max(1, maxRenderNormal))
      const sampledIntra: RenderLink[] = []
      const sampledInter: RenderLink[] = []
      normalIntra.forEach((l, i) => { if (i % stride === 0) sampledIntra.push(l) })
      normalInter.forEach((l, i) => { if (i % stride === 0) sampledInter.push(l) })
      return { normalIntra: sampledIntra, normalInter: sampledInter, selected }
    }

    return { normalIntra, normalInter, selected }
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

  const [nowMs, setNowMs] = useState(() => Date.now())
  useEffect(() => {
    const timer = window.setInterval(() => setNowMs(Date.now()), 120)
    return () => window.clearInterval(timer)
  }, [])

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
      const transitionUntil = Number(dep.path_transition_until ?? 0)
      const inTransition = transitionUntil > nowMs
      const remain = inTransition ? Math.max(0, Math.min(1, (transitionUntil - nowMs) / 900)) : 0

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
          color: inTransition ? '#e8f4ff' : '#ffffff',
          glowColor: inTransition ? '#7dd3fc' : '#e2f1ff',
          lineWidth: inTransition ? 4.2 : 3.6,
          opacity: inTransition ? 0.98 : 0.95,
          isGhost: false,
        })
      })

      if (inTransition && Array.isArray(dep.previous_link_details)) {
        dep.previous_link_details.forEach((l: any, i: number) => {
          const lk = `${String(l?.src ?? '')}|${String(l?.dst ?? '')}`
          if (!activeLinkSet.has(lk)) return
          const a = satMap.get(l.src)
          const b = satMap.get(l.dst)
          if (!a || !b) return
          out.push({
            key: `hl-old-${dep.deployment_id}-${l.src}-${l.dst}-${i}`,
            points: [
              toXYZ(a.coordinates.x, a.coordinates.y, a.coordinates.z),
              toXYZ(b.coordinates.x, b.coordinates.y, b.coordinates.z),
            ],
            link: l,
            color: '#60a5fa',
            glowColor: '#38bdf8',
            lineWidth: 2.6,
            opacity: 0.45 * remain,
            isGhost: true,
          })
        })
      }
    })
    return out
  }, [deployments, highlightedDeploymentIds, satMap, nowMs, activeLinkSet])

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
      meshes.selected?.geometry.dispose()
    }
  }, [meshes])

  const enableLinkPicking = useMemo(() => {
    const rendered = (groups.normalIntra.length + groups.normalInter.length + groups.selected.length)
    if (display.renderQuality === 'performance') return rendered <= 8000
    return rendered <= 12000
  }, [groups, display.renderQuality])

  const handleClick = (item: { links: RenderLink[] } | null, e: any) => {
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
    if (!Number.isFinite(bestD) || Math.sqrt(bestD) > 0.02) return
    const l = best
    if (!l) return
    e.stopPropagation()
    setSelectedLink(l as any)
    setSelectedSatellite(null)
  }

  if (!meshes.intra && !meshes.inter && !meshes.selected && highlightedLines.length === 0) return null

  return (
    <group>
      {meshes.intra && (
        <lineSegments geometry={meshes.intra.geometry} onClick={enableLinkPicking ? ((e) => handleClick(meshes.intra, e)) : undefined}>
          <lineBasicMaterial color="#65ff7f" transparent opacity={display.linkOpacity * 0.93} depthWrite={false} />
        </lineSegments>
      )}
      {meshes.inter && (
        <lineSegments geometry={meshes.inter.geometry} onClick={enableLinkPicking ? ((e) => handleClick(meshes.inter, e)) : undefined}>
          <lineBasicMaterial color="#8b5bf9" transparent opacity={display.linkOpacity * 0.8} depthWrite={false} />
        </lineSegments>
      )}

      {/* Deployed SFC links: static white thick glow */}
      {highlightedLines.map(item => (
        <group key={item.key}>
          <Line
            points={item.points}
            color={item.color}
            lineWidth={item.lineWidth}
            transparent
            opacity={item.opacity}
            raycast={() => null}
          />
          <Line
            points={item.points}
            color={item.glowColor}
            lineWidth={item.isGhost ? 6.2 : 8.5}
            transparent
            opacity={item.isGhost ? Math.max(0.08, item.opacity * 0.55) : 0.24}
            raycast={() => null}
          />
        </group>
      ))}

      {selectedLines.map(item => (
        <group key={item.key}>
          <Line points={item.points} color="#7df9ff" lineWidth={4.5} transparent opacity={1} raycast={() => null} />
          <Line points={item.points} color="#22d3ee" lineWidth={10.5} transparent opacity={0.3} raycast={() => null} />
        </group>
      ))}
    </group>
  )
}
