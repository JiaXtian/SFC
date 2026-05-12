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
    altitude_km: number; inclination: number; inclination_deg?: number
    propagation_model?: string
    eccentricity?: number
    argument_of_perigee_deg?: number
    mean_anomaly_deg?: number
    mean_motion_rev_per_day?: number
    bstar?: number
    epoch_jd?: number
    epoch_iso?: string
    propagation_minutes?: number
    semi_major_axis_km?: number
    period_minutes?: number
    tle_line1?: string
    tle_line2?: string
  }
  coordinates: { x: number; y: number; z: number; lat: number; lon: number }
  cpu_total: number; cpu_available: number
  mem_total: number; mem_available: number
  disk_total: number; disk_available: number
  core_network_load?: number
  core_business_load?: {
    signaling_load: number
    session_load: number
    user_plane_load: number
    mobility_load: number
    policy_load: number
    auth_load: number
    load_index?: number
  }
  node_reliability?: number
  status?: 'active' | 'down'
  fault_tag?: string
  container_name?: string
  container_state?: 'stopped' | 'starting' | 'running' | 'failed' | string
  running_core_nf_types?: string[]
  running_core_nf_count?: number
  service_probe_ok?: boolean
  deployed_sfc_names?: string[]
  deployed_core_nf_types?: string[]
  deployed_vnf_count?: number
  vnfs: any[]
  core_nfs?: any[]
}

export interface LinkData {
  source: string; target: string
  link_type: 'intra_orbit' | 'inter_orbit'
  status?: 'active' | 'congested' | 'down'
  fault_tag?: string
  reliability?: number
  latency_ms: number; bandwidth_gbps: number; bandwidth_available_gbps: number
}

const PI = Math.PI
const EARTH_R = 6371.0
const SGP4_EARTH_R = 6378.135
const SGP4_MU = 398600.8
const SGP4_J2 = 1.082616e-3
const C = 299792.458

function seeded(n: number) {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453
  return x - Math.floor(x)
}

function clamp01(v: number): number {
  if (!Number.isFinite(v)) return 0
  if (v < 0) return 0
  if (v > 1) return 1
  return v
}

function buildCoreBusinessLoad(seedBase: number, avgResourceLoad: number) {
  const base = clamp01(avgResourceLoad)
  const signaling = clamp01(base * 0.72 + seeded(seedBase + 401) * 0.32)
  const session = clamp01(base * 0.70 + seeded(seedBase + 487) * 0.34)
  const userPlane = clamp01(base * 0.76 + seeded(seedBase + 521) * 0.30)
  const mobility = clamp01(base * 0.66 + seeded(seedBase + 569) * 0.36)
  const policy = clamp01(base * 0.64 + seeded(seedBase + 613) * 0.34)
  const auth = clamp01(base * 0.62 + seeded(seedBase + 659) * 0.32)
  const loadIndex = (signaling + session + userPlane + mobility + policy + auth) / 6
  return {
    signaling_load: signaling,
    session_load: session,
    user_plane_load: userPlane,
    mobility_load: mobility,
    policy_load: policy,
    auth_load: auth,
    load_index: loadIndex,
  }
}

function julianDate(date = new Date()): number {
  return 2440587.5 + date.getTime() / 86400000
}

function wrapDeg(v: number): number {
  const x = v % 360
  return x < 0 ? x + 360 : x
}

function meanMotionFromAltitude(altitudeKm: number): number {
  const a = SGP4_EARTH_R + Math.max(100, altitudeKm)
  const nRadSec = Math.sqrt(SGP4_MU / (a * a * a))
  return nRadSec * 86400 / (2 * PI)
}

function semiMajorAxis(meanMotionRevPerDay: number): number {
  const n = Math.max(1e-9, meanMotionRevPerDay) * 2 * PI / 86400
  return Math.cbrt(SGP4_MU / (n * n))
}

function solveKepler(meanRad: number, ecc: number): number {
  let e = meanRad
  for (let i = 0; i < 10; i++) {
    const f = e - ecc * Math.sin(e) - meanRad
    const fp = 1 - ecc * Math.cos(e)
    if (Math.abs(fp) < 1e-12) break
    const step = f / fp
    e -= step
    if (Math.abs(step) < 1e-12) break
  }
  return e
}

function tleEpoch(epochJd: number): string {
  const d = new Date((epochJd - 2440587.5) * 86400000)
  const start = Date.UTC(d.getUTCFullYear(), 0, 0)
  const doy = Math.floor((d.getTime() - start) / 86400000)
  const yy = String(d.getUTCFullYear() % 100).padStart(2, '0')
  const frac = ((d.getUTCHours() * 3600 + d.getUTCMinutes() * 60 + d.getUTCSeconds() + d.getUTCMilliseconds() / 1000) / 86400)
    .toFixed(8)
    .slice(1)
  return `${yy}${String(doy).padStart(3, '0')}${frac}`
}

function makeTleLines(satNum: number, op: any): { line1: string; line2: string } {
  const sat = String(((satNum % 100000) + 100000) % 100000).padStart(5, '0')
  const ecc7 = String(Math.round(Math.max(0, Math.min(0.9999999, op.eccentricity)) * 1e7)).padStart(7, '0')
  const line1 = `1 ${sat}U 26001A   ${tleEpoch(op.epoch_jd)}  .00000000  00000-0  5000-4 0  9990`
  const line2 = `2 ${sat} ${op.inclination_deg.toFixed(4).padStart(8)} ${wrapDeg(op.raan).toFixed(4).padStart(8)} ${ecc7} ${wrapDeg(op.argument_of_perigee_deg).toFixed(4).padStart(8)} ${wrapDeg(op.mean_anomaly_deg).toFixed(4).padStart(8)} ${op.mean_motion_rev_per_day.toFixed(8).padStart(11)}00000`
  return { line1, line2 }
}

function julianDateFromYearDoy(year: number, dayOfYear: number): number {
  const ms = Date.UTC(year, 0, 1) + (dayOfYear - 1) * 86400000
  return 2440587.5 + ms / 86400000
}

function parseCompactTleExponential(raw: string): number {
  let s = String(raw ?? '').trim()
  if (!s) return 0
  let sign = 1
  if (s[0] === '-' || s[0] === '+') {
    sign = s[0] === '-' ? -1 : 1
    s = s.slice(1)
  }
  s = s.replace(/\s+/g, '')
  const match = s.match(/^(\d+)([+-]\d+)$/)
  if (!match) return 0
  const mantissa = Number(match[1]) / Math.pow(10, match[1].length)
  return sign * mantissa * Math.pow(10, Number(match[2]))
}

export function parseTleOrbitalParams(tleLine1: string, tleLine2: string): Partial<SatelliteData['orbital_params']> {
  const line1 = String(tleLine1 ?? '').trim()
  const line2 = String(tleLine2 ?? '').trim()
  if (line1.length < 63 || line2.length < 63 || !line1.startsWith('1') || !line2.startsWith('2')) {
    throw new Error('TLE 两行根数格式不完整')
  }
  const epochRaw = line1.slice(18, 32).trim()
  const yy = Number(epochRaw.slice(0, 2))
  const year = yy < 57 ? 2000 + yy : 1900 + yy
  const dayOfYear = Number(epochRaw.slice(2))
  const epochJd = julianDateFromYearDoy(year, dayOfYear)
  const inclinationDeg = Number(line2.slice(8, 16).trim())
  const raan = Number(line2.slice(17, 25).trim())
  const eccDigits = line2.slice(26, 33).trim()
  const eccentricity = Number(`0.${eccDigits || '0'}`)
  const argumentOfPerigeeDeg = Number(line2.slice(34, 42).trim())
  const meanAnomalyDeg = Number(line2.slice(43, 51).trim())
  const meanMotionRevPerDay = Number(line2.slice(52, 63).trim())
  if (![inclinationDeg, raan, eccentricity, argumentOfPerigeeDeg, meanAnomalyDeg, meanMotionRevPerDay, epochJd].every(Number.isFinite)) {
    throw new Error('TLE 轨道根数字段无法解析')
  }
  const semiMajor = semiMajorAxis(meanMotionRevPerDay)
  return {
    propagation_model: 'SGP4',
    raan: wrapDeg(raan),
    true_anomaly: wrapDeg(meanAnomalyDeg),
    inclination: inclinationDeg,
    inclination_deg: inclinationDeg,
    altitude_km: semiMajor - SGP4_EARTH_R,
    eccentricity,
    argument_of_perigee_deg: wrapDeg(argumentOfPerigeeDeg),
    mean_anomaly_deg: wrapDeg(meanAnomalyDeg),
    mean_motion_rev_per_day: meanMotionRevPerDay,
    bstar: parseCompactTleExponential(line1.slice(53, 61)),
    epoch_jd: epochJd,
    epoch_iso: new Date((epochJd - 2440587.5) * 86400000).toISOString(),
    semi_major_axis_km: semiMajor,
    period_minutes: 1440 / meanMotionRevPerDay,
    tle_line1: line1,
    tle_line2: line2,
  }
}

function makeWalkerSgp4Orbit(
  plane: number, numPlanes: number,
  pos: number, satsPerPlane: number,
  altitude: number, inclination: number,
  fPhasing: number, isWalkerStar: boolean,
  epochJd: number
) {
  const raan = isWalkerStar
    ? (180 / numPlanes) * plane
    : (360 / numPlanes) * plane
  const phaseShift = isWalkerStar ? 0 : (360 / (numPlanes * satsPerPlane)) * plane * fPhasing
  const ta = wrapDeg((360 / satsPerPlane) * pos + phaseShift)
  const meanMotion = meanMotionFromAltitude(altitude)
  const sma = semiMajorAxis(meanMotion)
  const op = {
    propagation_model: 'SGP4',
    plane,
    position_in_plane: pos,
    raan,
    true_anomaly: ta,
    altitude_km: altitude,
    inclination,
    inclination_deg: inclination,
    eccentricity: 0.0001,
    argument_of_perigee_deg: 0,
    mean_anomaly_deg: ta,
    mean_motion_rev_per_day: meanMotion,
    bstar: 0.00005,
    epoch_jd: epochJd,
    epoch_iso: new Date((epochJd - 2440587.5) * 86400000).toISOString(),
    propagation_minutes: 0,
    semi_major_axis_km: sma,
    period_minutes: 1440 / meanMotion,
  }
  const tle = makeTleLines(plane * 1000 + pos + 1, op)
  return { ...op, tle_line1: tle.line1, tle_line2: tle.line2 }
}

export function propagateSgp4Orbit(op: any, minutesSinceEpoch = 0): { x: number; y: number; z: number; lat: number; lon: number; true_anomaly: number; mean_anomaly_deg: number; raan: number; argument_of_perigee_deg: number; altitude_km: number } {
  const tleLine1 = String(op?.tle_line1 ?? '').trim()
  const tleLine2 = String(op?.tle_line2 ?? '').trim()
  const source = (tleLine1 && tleLine2 && !op?.__tle_parsed)
    ? { ...op, ...parseTleOrbitalParams(tleLine1, tleLine2), __tle_parsed: true }
    : op
  const ecc = Math.max(0, Math.min(0.25, Number(source.eccentricity ?? 0.0001)))
  const mm = Number(source.mean_motion_rev_per_day ?? meanMotionFromAltitude(Number(source.altitude_km ?? 550)))
  const a = semiMajorAxis(mm)
  const inc = Number(source.inclination_deg ?? source.inclination ?? 53) * PI / 180
  const p = a * (1 - ecc * ecc)
  const nRadMin = mm * 2 * PI / 1440
  const coeff = 1.5 * SGP4_J2 * SGP4_EARTH_R * SGP4_EARTH_R / (p * p) * nRadMin
  const raanRate = -coeff * Math.cos(inc)
  const argpRate = 0.5 * coeff * (5 * Math.cos(inc) * Math.cos(inc) - 1)
  const meanRate = nRadMin + 0.5 * coeff * Math.sqrt(Math.max(1e-9, 1 - ecc * ecc)) * (3 * Math.cos(inc) * Math.cos(inc) - 1)
  const raan = Number(source.raan ?? 0) * PI / 180 + raanRate * minutesSinceEpoch
  const argp = Number(source.argument_of_perigee_deg ?? 0) * PI / 180 + argpRate * minutesSinceEpoch
  const mean = Number(source.mean_anomaly_deg ?? source.true_anomaly ?? 0) * PI / 180 + meanRate * minutesSinceEpoch
  const eAnom = solveKepler(mean % (2 * PI), ecc)
  const radius = a * (1 - ecc * Math.cos(eAnom))
  const nu = Math.atan2(Math.sqrt(Math.max(0, 1 - ecc * ecc)) * Math.sin(eAnom), Math.cos(eAnom) - ecc)
  const u = argp + nu
  const x = radius * (Math.cos(raan) * Math.cos(u) - Math.sin(raan) * Math.sin(u) * Math.cos(inc))
  const y = radius * (Math.sin(raan) * Math.cos(u) + Math.cos(raan) * Math.sin(u) * Math.cos(inc))
  const z = radius * (Math.sin(u) * Math.sin(inc))
  return {
    x, y, z,
    lat: Math.asin(Math.max(-1, Math.min(1, z / radius))) * 180 / PI,
    lon: Math.atan2(y, x) * 180 / PI,
    true_anomaly: wrapDeg(nu * 180 / PI),
    mean_anomaly_deg: wrapDeg((mean % (2 * PI)) * 180 / PI),
    raan: wrapDeg(raan * 180 / PI),
    argument_of_perigee_deg: wrapDeg(argp * 180 / PI),
    altitude_km: radius - SGP4_EARTH_R,
  }
}

function walkerCoord(
  plane: number, numPlanes: number,
  pos: number, satsPerPlane: number,
  altitude: number, inclination: number,
  fPhasing: number, isWalkerStar: boolean,
  epochJd: number
): { orbit: SatelliteData['orbital_params']; coords: { x: number; y: number; z: number; lat: number; lon: number } } {
  const orbit = makeWalkerSgp4Orbit(plane, numPlanes, pos, satsPerPlane, altitude, inclination, fPhasing, isWalkerStar, epochJd)
  const propagated = propagateSgp4Orbit(orbit, 0)
  orbit.true_anomaly = propagated.true_anomaly
  orbit.raan = propagated.raan
  orbit.argument_of_perigee_deg = propagated.argument_of_perigee_deg
  orbit.altitude_km = propagated.altitude_km
  return { orbit, coords: { x: propagated.x, y: propagated.y, z: propagated.z, lat: propagated.lat, lon: propagated.lon } }
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
  const epochJd = julianDate()

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
        altitude, inclination, tpl.fPhasing, tpl.isWalkerStar, epochJd
      )

      const seed = plane * 1000 + pos
      const cpuT = Math.floor(tpl.cpuPerSat[0] + seeded(seed) * (tpl.cpuPerSat[1] - tpl.cpuPerSat[0]))
      const memT = Math.floor(tpl.memPerSat[0] + seeded(seed + 50) * (tpl.memPerSat[1] - tpl.memPerSat[0]))
      const diskT = Math.floor(tpl.diskPerSat[0] + seeded(seed + 75) * (tpl.diskPerSat[1] - tpl.diskPerSat[0]))
      const cpuLoad = seeded(seed + 100) * 0.65
      const memLoad = seeded(seed + 200) * 0.65
      const diskLoad = seeded(seed + 300) * 0.65
      const avgResourceLoad = (cpuLoad + memLoad + diskLoad) / 3
      const businessLoad = buildCoreBusinessLoad(seed, avgResourceLoad)

      satellites.push({
        id: generateSatelliteID(plane, pos, numPlanes, satsInThisPlane),
        orbital_params: coord.orbit,
        coordinates: coord.coords,
        cpu_total: cpuT, cpu_available: cpuT * (1 - cpuLoad),
        mem_total: memT, mem_available: memT * (1 - memLoad),
        disk_total: diskT, disk_available: diskT * (1 - diskLoad),
        core_network_load: businessLoad.load_index,
        core_business_load: businessLoad,
        node_reliability: 0.985 + (1 - businessLoad.load_index) * 0.012,
        status: 'active',
        fault_tag: '',
        vnfs: [],
        core_nfs: [],
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
          fault_tag: '',
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
          fault_tag: '',
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
      core_network_load: Number(s.core_network_load ?? 0.5),
      core_business_load: s.core_business_load ?? {
        signaling_load: Number(s.core_network_load ?? 0.5),
        session_load: Number(s.core_network_load ?? 0.5),
        user_plane_load: Number(s.core_network_load ?? 0.5),
        mobility_load: Number(s.core_network_load ?? 0.5),
        policy_load: Number(s.core_network_load ?? 0.5),
        auth_load: Number(s.core_network_load ?? 0.5),
      },
      node_reliability: Number(s.node_reliability ?? 0.985),
      status: s.status ?? 'active',
      fault_tag: s.fault_tag ?? '',
      vnfs: [],
      core_nfs: []
    })),
    links: links.map(l => ({
      source: l.source,
      target: l.target,
      link_type: l.link_type,
      status: l.status ?? 'active',
      fault_tag: l.fault_tag ?? '',
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
    const cpuTotal = Number(n.cpu_total ?? n.cpu?.total ?? 16)
    const cpuAvail = Number(n.cpu_available ?? n.cpu?.available ?? cpuTotal)
    const memTotal = Number(n.mem_total ?? n.memory_total ?? n.mem?.total ?? 32)
    const memAvail = Number(n.mem_available ?? n.memory_available ?? n.mem?.available ?? memTotal)
    const diskTotal = Number(n.disk_total ?? n.storage_total ?? n.disk?.total ?? 200)
    const diskAvail = Number(n.disk_available ?? n.storage_available ?? n.disk?.available ?? diskTotal)
    const coreBusiness = n.core_business_load ?? {}
    const fallbackLoad = Number(n.core_network_load ?? 0.5)
    const tleLine1 = String(orbital.tle_line1 ?? '').trim()
    const tleLine2 = String(orbital.tle_line2 ?? '').trim()
    if (!tleLine1 || !tleLine2) {
      throw new Error(`节点 ${id} 缺少 SGP4 TLE 两行根数`)
    }
    const tleOrbit = parseTleOrbitalParams(tleLine1, tleLine2)
    const parsedOrbit: SatelliteData['orbital_params'] = {
      ...(tleOrbit as SatelliteData['orbital_params']),
      propagation_model: 'SGP4',
      plane: Number(orbital.plane ?? orbital.plane_id ?? 0),
      position_in_plane: Number(orbital.position_in_plane ?? orbital.slot ?? idx),
      propagation_minutes: Number(orbital.propagation_minutes ?? 0),
      tle_line1: tleLine1,
      tle_line2: tleLine2,
    }
    const propagated = propagateSgp4Orbit(parsedOrbit, Number(parsedOrbit.propagation_minutes ?? 0))

    return {
      id,
      orbital_params: parsedOrbit,
      coordinates: {
        x: propagated.x,
        y: propagated.y,
        z: propagated.z,
        lat: propagated.lat,
        lon: propagated.lon,
      },
      cpu_total: cpuTotal,
      cpu_available: cpuAvail,
      mem_total: memTotal,
      mem_available: memAvail,
      disk_total: diskTotal,
      disk_available: diskAvail,
      core_network_load: Number(n.core_network_load ?? n.load ?? 0.5),
      core_business_load: {
        signaling_load: Number(coreBusiness.signaling_load ?? coreBusiness.signaling ?? fallbackLoad),
        session_load: Number(coreBusiness.session_load ?? coreBusiness.session ?? fallbackLoad),
        user_plane_load: Number(coreBusiness.user_plane_load ?? coreBusiness.user_plane ?? fallbackLoad),
        mobility_load: Number(coreBusiness.mobility_load ?? coreBusiness.mobility ?? fallbackLoad),
        policy_load: Number(coreBusiness.policy_load ?? coreBusiness.policy ?? fallbackLoad),
        auth_load: Number(coreBusiness.auth_load ?? coreBusiness.auth ?? fallbackLoad),
      },
      node_reliability: Number(n.node_reliability ?? 0.985),
      status: String(n.status ?? 'active') as 'active' | 'down',
      fault_tag: String(n.fault_tag ?? ''),
      vnfs: Array.isArray(n.core_nfs) ? n.core_nfs : (Array.isArray(n.vnfs) ? n.vnfs : []),
      core_nfs: Array.isArray(n.core_nfs) ? n.core_nfs : (Array.isArray(n.vnfs) ? n.vnfs : []),
    }
  })

  const links: LinkData[] = linksRaw.map((l: any) => ({
    source: String(l.source ?? l.src ?? ''),
    target: String(l.target ?? l.dst ?? ''),
    link_type: (l.link_type ?? l.type ?? 'inter_orbit') as 'intra_orbit' | 'inter_orbit',
    status: (l.status ?? 'active') as 'active' | 'congested' | 'down',
    fault_tag: String(l.fault_tag ?? ''),
    reliability: Number(l.reliability ?? 0.999),
    latency_ms: Number(l.latency_ms ?? l.latency ?? 5),
    bandwidth_gbps: Number(l.bandwidth_gbps ?? l.bandwidth ?? 10),
    bandwidth_available_gbps: Number(l.bandwidth_available_gbps ?? l.available_bandwidth ?? l.bandwidth ?? 10),
  })).filter(l => l.source && l.target)

  return { satellites, links }
}
