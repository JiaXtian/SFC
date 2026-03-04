// ============================================================
// LEO Constellation Generator - Backend Compatible
// ============================================================
export type ConstellationType =
  | 'starlink_v1' | 'starlink_v2' | 'oneweb'
  | 'iridium'     | 'telesat'    | 'kuiper'
  | 'polar'       | 'qianfan'

export interface ConstellationTemplate {
  id: ConstellationType
  name: string
  operator: string
  description: string
  detailedInfo: string
  altitude_km: number
  inclination_deg: number
  fPhasing: number
  isWalkerStar: boolean
  defaultSats: number
  defaultPlanes: number
  linkBudget: { intraGbps: number; interGbps: number }
  cpuPerSat: [number, number]
  memPerSat: [number, number]
  diskPerSat: [number, number]
}

export const CONSTELLATION_TEMPLATES: Record<ConstellationType, ConstellationTemplate> = {
  starlink_v1: {
    id: 'starlink_v1', name: 'Starlink Phase-1', operator: 'SpaceX',
    description: '550km 高度, 53° 倾角 Walker-Delta 星座',
    detailedInfo: '第一代 Starlink 外壳针对全球宽带覆盖进行优化。使用 Walker-Delta (T/P/F) 配置，53° 倾角提供 ±60° 纬度覆盖。中等高度减少延迟同时保持良好的覆盖密度。典型配置：每个轨道面 22 颗卫星，跨越多个轨道面以提供连续服务。',
    altitude_km: 550, inclination_deg: 53, fPhasing: 1, isWalkerStar: false,
    defaultSats: 72, defaultPlanes: 6,
    linkBudget: { intraGbps: 25, interGbps: 20 },
    cpuPerSat: [16, 24], memPerSat: [32, 64],
    diskPerSat: [180, 320],
  },
  starlink_v2: {
    id: 'starlink_v2', name: 'Starlink Gen-2', operator: 'SpaceX',
    description: '530km 高度, 43° 倾角，先进激光星间链路',
    detailedInfo: '第二代迷你卫星配备增强型星间激光链路。较低倾角 (43°) 针对高流量纬度优化。增强的功率预算和计算能力支持更高吞吐量和星载处理。多壳层架构包含 43°、53° 和 70° 壳层，实现全球覆盖优化。',
    altitude_km: 530, inclination_deg: 43, fPhasing: 0, isWalkerStar: false,
    defaultSats: 80, defaultPlanes: 8,
    linkBudget: { intraGbps: 40, interGbps: 30 },
    cpuPerSat: [24, 32], memPerSat: [64, 128],
    diskPerSat: [280, 520],
  },
  oneweb: {
    id: 'oneweb', name: 'OneWeb Constellation', operator: 'Eutelsat OneWeb',
    description: '1200km 高度, 87.9° 近极地轨道',
    detailedInfo: '高轨道近极地星座，提供包括极地在内的真正全球覆盖。87.9° 倾角确保极地到极地的连接性。较高轨道 (1200km) 减少所需卫星数量但增加延迟。主要面向需要可靠极地覆盖的企业、政府和海事/航空市场。',
    altitude_km: 1200, inclination_deg: 87.9, fPhasing: 1, isWalkerStar: false,
    defaultSats: 72, defaultPlanes: 12,
    linkBudget: { intraGbps: 10, interGbps: 8 },
    cpuPerSat: [8, 16], memPerSat: [16, 32],
    diskPerSat: [120, 220],
  },
  iridium: {
    id: 'iridium', name: 'Iridium NEXT', operator: 'Iridium',
    description: '780km 高度, 86.4° Walker-Star 配置',
    detailedInfo: '经典 Walker-Star 极地星座（6 个轨道面，每面 11 颗卫星，86.4° 倾角）。针对全球语音和低带宽物联网服务优化。交叉接缝相位模式确保所有纬度的连续覆盖。中等高度平衡覆盖、链路预算和轨道寿命。具有 20 多年运营历史的成熟架构。',
    altitude_km: 780, inclination_deg: 86.4, fPhasing: 0, isWalkerStar: true,
    defaultSats: 66, defaultPlanes: 6,
    linkBudget: { intraGbps: 1.5, interGbps: 1.0 },
    cpuPerSat: [4, 8], memPerSat: [8, 16],
    diskPerSat: [80, 150],
  },
  telesat: {
    id: 'telesat', name: 'Telesat Lightspeed', operator: 'Telesat',
    description: '双层: 98.98° SSO 极地 + 40° 倾斜轨道',
    detailedInfo: '创新的双层架构结合太阳同步极地轨道 (98.98°, 1325km) 和中倾角壳层 (40°-50°, 1015km)。极地层提供连续的高纬度覆盖和遥感能力。倾斜层优化人口密集区域的容量。先进相控阵天线支持灵活的波束转向。',
    altitude_km: 1015, inclination_deg: 98.98, fPhasing: 1, isWalkerStar: false,
    defaultSats: 78, defaultPlanes: 13,
    linkBudget: { intraGbps: 16, interGbps: 12 },
    cpuPerSat: [12, 20], memPerSat: [24, 48],
    diskPerSat: [140, 260],
  },
  kuiper: {
    id: 'kuiper', name: 'Amazon Kuiper', operator: 'Amazon',
    description: '630km 高度, 双倾角 51.9°/33° 壳层',
    detailedInfo: '亚马逊的 LEO 星座使用双倾角架构针对 ±56° 纬度覆盖。51.9° 壳层提供人口密集区域的主要覆盖。33° 赤道壳层在高流量区域增加容量。较低高度 (630km) 最小化实时应用延迟。与 AWS 地面基础设施集成以实现边缘计算。',
    altitude_km: 630, inclination_deg: 51.9, fPhasing: 1, isWalkerStar: false,
    defaultSats: 80, defaultPlanes: 8,
    linkBudget: { intraGbps: 20, interGbps: 16 },
    cpuPerSat: [12, 24], memPerSat: [24, 48],
    diskPerSat: [160, 300],
  },
  polar: {
    id: 'polar', name: 'Polar Orbit Constellation', operator: 'Custom',
    description: '600km 高度, 90° 极地 Walker-Star',
    detailedInfo: '纯极地轨道 (90° 倾角) Walker-Star 配置，非常适合地球观测、气象和 AIS 跟踪。提供完整的极地到极地覆盖，具有规律的重访时间。较低高度 (600km) 支持高分辨率成像并减少通信延迟。由于可预测的地面轨迹，地面站网络简化。',
    altitude_km: 600, inclination_deg: 90, fPhasing: 0, isWalkerStar: true,
    defaultSats: 60, defaultPlanes: 6,
    linkBudget: { intraGbps: 8, interGbps: 6 },
    cpuPerSat: [8, 16], memPerSat: [16, 32],
    diskPerSat: [110, 210],
  },
  qianfan: {
    id: 'qianfan', name: 'Qianfan / 千帆', operator: '上海垣信卫星',
    description: '面向宽带与低时延业务的多层 LEO 架构（演示参数）',
    detailedInfo: '用于演示千帆星座规划流程的预置工程参数，支持前端快速生成并与后端拓扑同步，可结合第三方 JSON 构型进行替换与验证。',
    altitude_km: 650, inclination_deg: 53, fPhasing: 1, isWalkerStar: false,
    defaultSats: 96, defaultPlanes: 8,
    linkBudget: { intraGbps: 24, interGbps: 18 },
    cpuPerSat: [14, 26], memPerSat: [28, 64], diskPerSat: [220, 420],
  },
}

export interface SatelliteData {
  id: string
  orbital_params: {
    plane: number; position_in_plane: number
    raan: number; true_anomaly: number
    altitude_km: number; inclination: number
  }
  coordinates: { x: number; y: number; z: number; lat: number; lon: number }
  cpu_total: number; cpu_available: number
  mem_total: number; mem_available: number
  disk_total: number; disk_available: number
  vnfs: any[]
}

export interface LinkData {
  source: string; target: string
  link_type: 'intra_orbit' | 'inter_orbit'
  status?: 'active' | 'congested' | 'down'
  reliability?: number
  latency_ms: number; bandwidth_gbps: number; bandwidth_available_gbps: number
}

const PI = Math.PI
const EARTH_R = 6371.0
const C = 299792.458

function seeded(n: number) {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453
  return x - Math.floor(x)
}

function walkerCoord(
  plane: number, numPlanes: number,
  pos: number, satsPerPlane: number,
  altitude: number, inclination: number,
  fPhasing: number, isWalkerStar: boolean
): { x: number; y: number; z: number; lat: number; lon: number; raan: number; ta: number } {
  const raan = isWalkerStar
    ? (180 / numPlanes) * plane
    : (360 / numPlanes) * plane
  const phaseShift = isWalkerStar ? 0 : (360 / (numPlanes * satsPerPlane)) * plane * fPhasing
  const ta = ((360 / satsPerPlane) * pos + phaseShift) % 360

  const r = EARTH_R + altitude
  const i = inclination * PI / 180
  const O = raan * PI / 180
  const v = ta * PI / 180

  const xOrb = r * Math.cos(v)
  const yOrb = r * Math.sin(v)

  const x = Math.cos(O) * xOrb - Math.sin(O) * Math.cos(i) * yOrb
  const y = Math.sin(O) * xOrb + Math.cos(O) * Math.cos(i) * yOrb
  const z = Math.sin(i) * yOrb

  const lon = Math.atan2(y, x) * 180 / PI
  const lat = Math.asin(Math.max(-1, Math.min(1, z / r))) * 180 / PI

  return { x, y, z, lat, lon, raan, ta }
}

// 生成与后端一致的卫星命名: SAT_XXX_XXX
function generateSatelliteID(plane: number, pos: number, numPlanes: number, satsPerPlane: number): string {
  const planeStr = String(plane).padStart(3, '0')
  const posStr = String(pos).padStart(3, '0')
  return `SAT_${planeStr}_${posStr}`
}

export function generateConstellation(
  totalSats: number,
  numPlanes: number,
  type: ConstellationType
): SatelliteData[] {
  if (totalSats <= 0 || numPlanes <= 0) return []
  const tpl = CONSTELLATION_TEMPLATES[type]
  const satsPerPlaneBase = Math.floor(totalSats / numPlanes)
  const remainder = totalSats % numPlanes
  const satellites: SatelliteData[] = []

  const halfPlanes = type === 'telesat' ? Math.floor(numPlanes / 2) : numPlanes

  for (let plane = 0; plane < numPlanes; plane++) {
    const satsInThisPlane = satsPerPlaneBase + (plane < remainder ? 1 : 0)
    if (satsInThisPlane <= 0) continue
    const isSSO = type === 'telesat' && plane >= halfPlanes
    const inclination = isSSO ? 98.98 : tpl.inclination_deg
    const altitude = isSSO ? 1325 : tpl.altitude_km

    for (let pos = 0; pos < satsInThisPlane; pos++) {
      const coord = walkerCoord(
        plane, numPlanes, pos, satsInThisPlane,
        altitude, inclination, tpl.fPhasing, tpl.isWalkerStar
      )

      const seed = plane * 1000 + pos
      const cpuT = Math.floor(tpl.cpuPerSat[0] + seeded(seed) * (tpl.cpuPerSat[1] - tpl.cpuPerSat[0]))
      const memT = Math.floor(tpl.memPerSat[0] + seeded(seed + 50) * (tpl.memPerSat[1] - tpl.memPerSat[0]))
      const diskT = Math.floor(tpl.diskPerSat[0] + seeded(seed + 75) * (tpl.diskPerSat[1] - tpl.diskPerSat[0]))
      const cpuLoad = seeded(seed + 100) * 0.65
      const memLoad = seeded(seed + 200) * 0.65
      const diskLoad = seeded(seed + 300) * 0.65

      satellites.push({
        id: generateSatelliteID(plane, pos, numPlanes, satsInThisPlane),
        orbital_params: {
          plane, position_in_plane: pos,
          raan: coord.raan, true_anomaly: coord.ta,
          altitude_km: altitude, inclination,
        },
        coordinates: { x: coord.x, y: coord.y, z: coord.z, lat: coord.lat, lon: coord.lon },
        cpu_total: cpuT, cpu_available: cpuT * (1 - cpuLoad),
        mem_total: memT, mem_available: memT * (1 - memLoad),
        disk_total: diskT, disk_available: diskT * (1 - diskLoad),
        vnfs: [],
      })
    }
  }
  return satellites
}

function hasLineOfSight(a: SatelliteData, b: SatelliteData): boolean {
  const ax = a.coordinates.x, ay = a.coordinates.y, az = a.coordinates.z
  const bx = b.coordinates.x, by = b.coordinates.y, bz = b.coordinates.z
  
  const dx = bx - ax, dy = by - ay, dz = bz - az
  const d2 = dx*dx + dy*dy + dz*dz
  if (d2 === 0) return false
  
  const t = -(ax*dx + ay*dy + az*dz) / d2
  if (t < 0 || t > 1) return true
  
  const cx = ax + t*dx, cy = ay + t*dy, cz = az + t*dz
  const dist2 = cx*cx + cy*cy + cz*cz
  
  return dist2 > (EARTH_R + 50) * (EARTH_R + 50)
}

function maxISLRange(alt_km: number): number {
  const R = EARTH_R
  const h = alt_km
  const horizon = Math.sqrt(2*R*h + h*h)
  return horizon * 0.95
}

function meetsElevation(a: SatelliteData, b: SatelliteData): boolean {
  const ax = a.coordinates.x, ay = a.coordinates.y, az = a.coordinates.z
  const bx = b.coordinates.x, by = b.coordinates.y, bz = b.coordinates.z
  const dx = bx - ax, dy = by - ay, dz = bz - az
  const dist = Math.sqrt(dx*dx + dy*dy + dz*dz)
  const ra = Math.sqrt(ax*ax + ay*ay + az*az)
  
  const dot = (ax*dx + ay*dy + az*dz) / (ra * dist)
  const angle = Math.acos(Math.max(-1, Math.min(1, -dot))) * 180 / PI
  
  return angle >= 10
}

export function generateLinks(satellites: SatelliteData[], type: ConstellationType): LinkData[] {
  const tpl = CONSTELLATION_TEMPLATES[type]
  const links: LinkData[] = []

  const planeMap = new Map<number, SatelliteData[]>()
  satellites.forEach(s => {
    const p = s.orbital_params.plane
    if (!planeMap.has(p)) planeMap.set(p, [])
    planeMap.get(p)!.push(s)
  })

  const planes = Array.from(planeMap.keys()).sort((a, b) => a - b)

  const dist3d = (a: SatelliteData, b: SatelliteData) => {
    const dx = a.coordinates.x - b.coordinates.x
    const dy = a.coordinates.y - b.coordinates.y
    const dz = a.coordinates.z - b.coordinates.z
    return Math.sqrt(dx*dx + dy*dy + dz*dz)
  }

  const maxRange = maxISLRange(tpl.altitude_km)

  planeMap.forEach(sats => {
    const sorted = [...sats].sort((a,b) => a.orbital_params.position_in_plane - b.orbital_params.position_in_plane)
    for (let i = 0; i < sorted.length; i++) {
      const a = sorted[i], b = sorted[(i+1) % sorted.length]
      const d = dist3d(a, b)
      
      if (d <= maxRange && hasLineOfSight(a, b) && meetsElevation(a, b)) {
        links.push({
          source: a.id, target: b.id, link_type: 'intra_orbit',
          latency_ms: (d / C) * 1000,
          reliability: 0.999,
          status: 'active',
          bandwidth_gbps: tpl.linkBudget.intraGbps,
          bandwidth_available_gbps: tpl.linkBudget.intraGbps * 0.9,
        })
      }
    }
  })

  for (let pi = 0; pi < planes.length; pi++) {
    const planeA = planeMap.get(planes[pi])!
    const planeB = planeMap.get(planes[(pi+1) % planes.length])!
    const n = Math.min(planeA.length, planeB.length)
    
    for (let j = 0; j < n; j++) {
      const a = planeA[j], b = planeB[j]
      const d = dist3d(a, b)
      
      if (d <= maxRange && hasLineOfSight(a, b) && meetsElevation(a, b)) {
        links.push({
          source: a.id, target: b.id, link_type: 'inter_orbit',
          latency_ms: (d / C) * 1000,
          reliability: 0.997,
          status: 'active',
          bandwidth_gbps: tpl.linkBudget.interGbps,
          bandwidth_available_gbps: tpl.linkBudget.interGbps * 0.85,
        })
      }
    }
  }

  return links
}

// 准备上传到后端的拓扑数据格式 - 修正字段名以匹配后端 struct Topology
export function prepareTopologyForBackend(
  satellites: SatelliteData[],
  links: LinkData[],
  type: ConstellationType,
  numPlanes: number // 显式传入轨道面数
) {
  const tpl = CONSTELLATION_TEMPLATES[type]
  
  return {
    // 这里的嵌套结构必须对应后端的 json to_json() 逻辑
    metadata: {
      total_sats: satellites.length,      // 后端是 total_sats，不是 total_satellites
      num_planes: numPlanes,              // 显式传递前端选择的面数
      altitude_km: tpl.altitude_km,
      inclination_deg: tpl.inclination_deg,
      timestamp: new Date().toISOString() // 后端字段名是 timestamp
    },
    // 注意：后端 Topology 结构体的 to_json 返回的是 { metadata: ..., topology: { nodes: ..., links: ... } }
    // 但后端接收 generate 接口时，通常直接解析 nodes 和 links
    nodes: satellites.map(s => ({
      id: s.id,
      type: "satellite",
      orbital_params: s.orbital_params,
      coordinates: s.coordinates,
      cpu_total: s.cpu_total,
      cpu_available: s.cpu_available,
      mem_total: s.mem_total,
      mem_available: s.mem_available,
      disk_total: s.disk_total,
      disk_available: s.disk_available,
      vnfs: []
    })),
    links: links.map(l => ({
      source: l.source,
      target: l.target,
      link_type: l.link_type,
      status: l.status ?? 'active',
      reliability: l.reliability ?? 0.999,
      latency_ms: l.latency_ms,
      bandwidth_gbps: l.bandwidth_gbps,
      bandwidth_available_gbps: l.bandwidth_available_gbps
    }))
  }
}

export function parseThirdPartyTopology(raw: any): { satellites: SatelliteData[]; links: LinkData[] } {
  const root = raw?.topology ?? raw ?? {}
  const nodesRaw = Array.isArray(root.nodes) ? root.nodes : []
  const linksRaw = Array.isArray(root.links) ? root.links : []

  const satellites: SatelliteData[] = nodesRaw.map((n: any, idx: number) => {
    const id = String(n.id ?? n.name ?? `SAT_EXT_${idx}`)
    const orbital = n.orbital_params ?? n.orbit ?? {}
    const coords = n.coordinates ?? n.position ?? {}

    const cpuTotal = Number(n.cpu_total ?? n.cpu?.total ?? 16)
    const cpuAvail = Number(n.cpu_available ?? n.cpu?.available ?? cpuTotal)
    const memTotal = Number(n.mem_total ?? n.memory_total ?? n.mem?.total ?? 32)
    const memAvail = Number(n.mem_available ?? n.memory_available ?? n.mem?.available ?? memTotal)
    const diskTotal = Number(n.disk_total ?? n.storage_total ?? n.disk?.total ?? 200)
    const diskAvail = Number(n.disk_available ?? n.storage_available ?? n.disk?.available ?? diskTotal)

    return {
      id,
      orbital_params: {
        plane: Number(orbital.plane ?? orbital.plane_id ?? 0),
        position_in_plane: Number(orbital.position_in_plane ?? orbital.slot ?? idx),
        raan: Number(orbital.raan ?? 0),
        true_anomaly: Number(orbital.true_anomaly ?? orbital.ta ?? 0),
        altitude_km: Number(orbital.altitude_km ?? orbital.altitude ?? 550),
        inclination: Number(orbital.inclination ?? orbital.inclination_deg ?? 53),
      },
      coordinates: {
        x: Number(coords.x ?? 0),
        y: Number(coords.y ?? 0),
        z: Number(coords.z ?? 0),
        lat: Number(coords.lat ?? coords.latitude ?? 0),
        lon: Number(coords.lon ?? coords.longitude ?? 0),
      },
      cpu_total: cpuTotal,
      cpu_available: cpuAvail,
      mem_total: memTotal,
      mem_available: memAvail,
      disk_total: diskTotal,
      disk_available: diskAvail,
      vnfs: Array.isArray(n.vnfs) ? n.vnfs : [],
    }
  })

  const links: LinkData[] = linksRaw.map((l: any) => ({
    source: String(l.source ?? l.src ?? ''),
    target: String(l.target ?? l.dst ?? ''),
    link_type: (l.link_type ?? l.type ?? 'inter_orbit') as 'intra_orbit' | 'inter_orbit',
    status: (l.status ?? 'active') as 'active' | 'congested' | 'down',
    reliability: Number(l.reliability ?? 0.999),
    latency_ms: Number(l.latency_ms ?? l.latency ?? 5),
    bandwidth_gbps: Number(l.bandwidth_gbps ?? l.bandwidth ?? 10),
    bandwidth_available_gbps: Number(l.bandwidth_available_gbps ?? l.available_bandwidth ?? l.bandwidth ?? 10),
  })).filter(l => l.source && l.target)

  return { satellites, links }
}
