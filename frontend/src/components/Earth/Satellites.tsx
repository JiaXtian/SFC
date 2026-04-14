import { useEffect, useMemo, useRef, useState } from 'react'
import { useFrame, useThree } from '@react-three/fiber'
import { Html } from '@react-three/drei'
import * as THREE from 'three'
import { useStore } from '@/store/useStore'
import { shallow } from 'zustand/shallow'

const EARTH_R = 5
const KM_TO_U = EARTH_R / 6371

function satToVec3(x: number, y: number, z: number): THREE.Vector3 {
  return new THREE.Vector3(x * KM_TO_U, z * KM_TO_U, -y * KM_TO_U)
}

function blendColors(colors: string[]): string {
  if (colors.length === 0) return '#39ff6a'
  const c = colors.map(hex => new THREE.Color(hex))
  const out = new THREE.Color(0, 0, 0)
  c.forEach(col => out.add(col))
  out.multiplyScalar(1 / c.length)
  return `#${out.getHexString()}`
}

function isNodeFault(sat: any): boolean {
  const status = String(sat?.status ?? 'active').toLowerCase()
  const faultTag = String(sat?.fault_tag ?? '').trim()
  return status === 'down' || status === 'fault' || status === 'failed' || faultTag.length > 0
}

export default function Satellites() {
  const { satellites, selectedSatellite, deployments, highlightedDeploymentIds, setSelectedSatellite, setSelectedLink } = useStore((s) => ({
    satellites: s.satellites,
    selectedSatellite: s.selectedSatellite,
    deployments: s.deployments,
    highlightedDeploymentIds: s.highlightedDeploymentIds,
    setSelectedSatellite: s.setSelectedSatellite,
    setSelectedLink: s.setSelectedLink,
  }), shallow)
  const { camera } = useThree()
  const [hoveredIdx, setHoveredIdx] = useState<number>(-1)
  const instanceRef = useRef<THREE.InstancedMesh>(null)
  const dummy = useMemo(() => new THREE.Object3D(), [])
  const tmpDir = useMemo(() => new THREE.Vector3(), [])
  const currentPosMapRef = useRef<Map<string, THREE.Vector3>>(new Map())
  const targetPosMapRef = useRef<Map<string, THREE.Vector3>>(new Map())
  const meshDirtyRef = useRef(true)

  const vnfHighlightSet = useMemo(() => {
    if (highlightedDeploymentIds.length === 0) return new Set<string>()
    const active = new Set(highlightedDeploymentIds)
    const nodes = new Set<string>()
    deployments.forEach(dep => {
      if (!active.has(dep.deployment_id)) return
      if (dep.satisfies_constraints === false) return
      dep.deployed_nodes.forEach(nodeId => nodes.add(nodeId))
    })
    return nodes
  }, [deployments, highlightedDeploymentIds])

  const endpointSets = useMemo(() => {
    const ingress = new Set<string>()
    const egress = new Set<string>()
    if (highlightedDeploymentIds.length === 0) return { ingress, egress }
    const active = new Set(highlightedDeploymentIds)
    deployments.forEach(dep => {
      if (!active.has(dep.deployment_id)) return
      if ((dep as any).source_node) ingress.add((dep as any).source_node)
      if ((dep as any).destination_node) egress.add((dep as any).destination_node)
    })
    return { ingress, egress }
  }, [deployments, highlightedDeploymentIds])

  const satelliteIndexById = useMemo(() => {
    const m = new Map<string, number>()
    satellites.forEach((sat, idx) => m.set(sat.id, idx))
    return m
  }, [satellites])

  const { targetPositions, colours } = useMemo(() => {
    const targetPositions: THREE.Vector3[] = []
    const colours: THREE.Color[] = []
    const baseGreen = new THREE.Color('#45f27d')
    const selectedGreen = new THREE.Color('#84ff92')
    const faultRed = new THREE.Color('#ff3b30')
    const selectedFaultRed = new THREE.Color('#ff6b63')

    satellites.forEach(sat => {
      const isFault = isNodeFault(sat)
      targetPositions.push(satToVec3(sat.coordinates.x, sat.coordinates.y, sat.coordinates.z))
      if (isFault && selectedSatellite?.id === sat.id) {
        colours.push(selectedFaultRed)
      } else if (isFault) {
        colours.push(faultRed)
      } else if (selectedSatellite?.id === sat.id) {
        colours.push(selectedGreen)
      } else if (vnfHighlightSet.has(sat.id)) {
        colours.push(new THREE.Color('#e4fff1'))
      } else {
        colours.push(baseGreen)
      }
    })
    return { targetPositions, colours }
  }, [satellites, selectedSatellite, vnfHighlightSet])

  const sphereDetail = useMemo(() => {
    if (satellites.length >= 5000) return { r: 0.055, seg: 6 }
    if (satellites.length >= 3000) return { r: 0.06, seg: 8 }
    return { r: 0.07, seg: 10 }
  }, [satellites.length])

  useEffect(() => {
    const current = currentPosMapRef.current
    const target = targetPosMapRef.current
    const liveIds = new Set<string>()
    satellites.forEach((sat, idx) => {
      liveIds.add(sat.id)
      const t = targetPositions[idx] ?? satToVec3(sat.coordinates.x, sat.coordinates.y, sat.coordinates.z)
      target.set(sat.id, t.clone())
      if (!current.has(sat.id)) {
        current.set(sat.id, t.clone())
      }
    })
    Array.from(current.keys()).forEach((id) => { if (!liveIds.has(id)) current.delete(id) })
    Array.from(target.keys()).forEach((id) => { if (!liveIds.has(id)) target.delete(id) })
    meshDirtyRef.current = true
  }, [satellites, targetPositions])

  const getDisplayPosition = (idx: number) => {
    const sat = satellites[idx]
    if (!sat) return null
    return currentPosMapRef.current.get(sat.id) ?? targetPosMapRef.current.get(sat.id) ?? targetPositions[idx] ?? null
  }

  const vecToTuple = (v: THREE.Vector3): [number, number, number] => [v.x, v.y, v.z]

  useFrame(() => {
    const mesh = instanceRef.current
    if (!mesh || satellites.length === 0 || !meshDirtyRef.current) return
    const n = Math.min(satellites.length, mesh.count)
    for (let i = 0; i < n; i++) {
      const sat = satellites[i]
      const cur = currentPosMapRef.current.get(sat.id)
      const tgt = targetPosMapRef.current.get(sat.id) ?? targetPositions[i]
      const pos = cur && tgt
        ? cur.copy(tgt)
        : (cur ?? tgt)
      if (!pos) continue
      dummy.position.copy(pos)
      dummy.updateMatrix()
      mesh.setMatrixAt(i, dummy.matrix)
      mesh.setColorAt(i, colours[i])
    }
    mesh.instanceMatrix.needsUpdate = true
    if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true
    meshDirtyRef.current = false
  })

  const isOccludedByEarth = (point: THREE.Vector3) => {
    const occlusionR = EARTH_R + 0.02
    const cam = camera.position
    tmpDir.subVectors(point, cam)
    const a = tmpDir.dot(tmpDir)
    if (a < 1e-9) return false
    const b = 2 * cam.dot(tmpDir)
    const c = cam.dot(cam) - occlusionR * occlusionR
    const disc = b * b - 4 * a * c
    if (disc <= 0) return false
    const s = Math.sqrt(disc)
    const t1 = (-b - s) / (2 * a)
    const t2 = (-b + s) / (2 * a)
    const eps = 1e-4
    return (t1 > eps && t1 < 1 - eps) || (t2 > eps && t2 < 1 - eps)
  }

  const handlePick = (idx: number) => {
    if (idx < 0 || idx >= satellites.length) return
    const p = getDisplayPosition(idx)
    if (!p || isOccludedByEarth(p)) return
    setSelectedSatellite(satellites[idx])
    setSelectedLink(null)
  }

  const handlePointerMove = (e: any) => {
    const idx = Number(e.instanceId ?? -1)
    if (satellites.length > 1200) {
      setHoveredIdx(-1)
      document.body.style.cursor = 'default'
      return
    }
    const p = idx >= 0 ? getDisplayPosition(idx) : null
    const vis = idx >= 0 && !!p && !isOccludedByEarth(p)
    setHoveredIdx(vis ? idx : -1)
    document.body.style.cursor = vis ? 'pointer' : 'default'
  }

  const markedSatIds = useMemo(() => {
    const ids = new Set<string>()
    vnfHighlightSet.forEach((id) => ids.add(id))
    endpointSets.ingress.forEach((id) => ids.add(id))
    endpointSets.egress.forEach((id) => ids.add(id))
    return Array.from(ids)
  }, [vnfHighlightSet, endpointSets])

  if (satellites.length === 0) return null

  const hov = hoveredIdx >= 0 ? satellites[hoveredIdx] : null
  const hovPos = hov ? getDisplayPosition(hoveredIdx) : null
  const selectedIdx = selectedSatellite ? satellites.findIndex(s => s.id === selectedSatellite.id) : -1
  const selectedPos = selectedIdx >= 0 ? getDisplayPosition(selectedIdx) : null

  return (
    <group>
      <instancedMesh
        ref={instanceRef}
        args={[undefined, undefined, satellites.length]}
        frustumCulled={false}
        onClick={(e: any) => {
          const idx = Number(e.instanceId ?? -1)
          if (idx >= 0) {
            e.stopPropagation()
            handlePick(idx)
          }
        }}
        onPointerMove={handlePointerMove}
        onPointerOut={() => {
          setHoveredIdx(-1)
          document.body.style.cursor = 'default'
        }}
      >
        <sphereGeometry args={[sphereDetail.r, sphereDetail.seg, sphereDetail.seg]} />
        <meshStandardMaterial
          vertexColors
          toneMapped={false}
          roughness={0.2}
          metalness={0.66}
          emissive="#1da63a"
          emissiveIntensity={0.42}
        />
      </instancedMesh>

      {selectedPos && (
        <group position={vecToTuple(selectedPos)}>
          <mesh raycast={() => null}>
            <sphereGeometry args={[0.11, 16, 16]} />
            <meshBasicMaterial color="#80fbe1" transparent opacity={0.92} />
          </mesh>
          <mesh raycast={() => null}>
            <sphereGeometry args={[0.15, 16, 16]} />
            <meshBasicMaterial color="#67e8f9" transparent opacity={0.35} depthWrite={false} />
          </mesh>
        </group>
      )}

      {/* Static bulging overlays for deployed nodes (VNF / ingress / egress) */}
      {highlightedDeploymentIds.length > 0 && markedSatIds.map((satId) => {
        const idx = satelliteIndexById.get(satId)
        if (idx == null || idx < 0) return null
        const p = getDisplayPosition(idx)
        if (!p) return null
        const isVnf = vnfHighlightSet.has(satId)
        const isIngress = endpointSets.ingress.has(satId)
        const isEgress = endpointSets.egress.has(satId)
        if (!isVnf && !isIngress && !isEgress) return null

        const roleColors: string[] = []
        if (isVnf) roleColors.push('#f5f9f7')
        if (isIngress) roleColors.push('#0097fb')
        if (isEgress) roleColors.push('#f67904')
        const core = blendColors(roleColors)

        return (
          <group key={`mark-${satId}`} position={vecToTuple(p)}>
            <mesh raycast={() => null}>
              <sphereGeometry args={[0.083, 14, 14]} />
              <meshBasicMaterial color={core} transparent opacity={0.9} />
            </mesh>
            <mesh raycast={() => null}>
              <sphereGeometry args={[0.118, 14, 14]} />
              <meshBasicMaterial color={core} transparent opacity={0.28} depthWrite={false} />
            </mesh>
          </group>
        )
      })}

      {hov && hovPos && (
        <group position={vecToTuple(hovPos)}>
          <Html distanceFactor={10} zIndexRange={[200, 0]} style={{ pointerEvents: 'none' }}>
            <div style={{
              transform: 'translate(12px,-50%)', padding: '8px 12px', borderRadius: 10,
              background: 'linear-gradient(135deg, rgba(15,23,42,0.97), rgba(30,41,59,0.97))',
              border: '1px solid rgba(0,255,136,0.4)', fontSize: 11, whiteSpace: 'nowrap',
              boxShadow: '0 8px 32px rgba(0,0,0,0.7)', fontFamily: '"IBM Plex Mono", monospace',
            }}>
              <div style={{ color: '#00ff88', fontWeight: 700, marginBottom: 5 }}>{hov.id}</div>
              <div style={{ color: '#cbd5e1', fontSize: 10 }}>
                CPU <span style={{ color: '#f0f9ff', fontWeight: 600 }}>{hov.cpu_available.toFixed(1)}</span>/{hov.cpu_total}
                &nbsp;&nbsp;
                内存 <span style={{ color: '#f0f9ff', fontWeight: 600 }}>{hov.mem_available.toFixed(0)}</span>/{hov.mem_total}GB
                &nbsp;&nbsp;
                磁盘 <span style={{ color: '#f0f9ff', fontWeight: 600 }}>{(hov as any).disk_available?.toFixed?.(0) ?? '0'}</span>/{(hov as any).disk_total ?? 0}GB
              </div>
              <div style={{ color: '#64748b', marginTop: 3, fontSize: 9 }}>
                状态: {isNodeFault(hov) ? '故障' : '正常'}
                {String((hov as any)?.fault_tag ?? '').trim()
                  ? ` · ${String((hov as any).fault_tag)}`
                  : ''}
              </div>
              <div style={{ color: '#64748b', marginTop: 2, fontSize: 9 }}>
                {Math.abs(hov.coordinates.lat).toFixed(1)}°{hov.coordinates.lat >= 0 ? 'N' : 'S'} &nbsp;
                {Math.abs(hov.coordinates.lon).toFixed(1)}°{hov.coordinates.lon >= 0 ? 'E' : 'W'} &nbsp;
                {hov.orbital_params.altitude_km.toFixed(0)}km
              </div>
            </div>
          </Html>
        </group>
      )}
    </group>
  )
}
