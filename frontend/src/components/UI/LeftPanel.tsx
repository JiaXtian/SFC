import { useRef, useState } from 'react'
import {
  ChevronLeft,
  ChevronRight,
  RefreshCw,
  Orbit,
  Activity,
  Cpu,
  Database,
  FileCode2,
} from 'lucide-react'
import { useStore } from '@/store/useStore'
import { apiClient } from '@/api/client'
import {
  CONSTELLATION_TEMPLATES,
  generateConstellation,
  generateLinks,
  parseThirdPartyTopology,
  prepareTopologyForBackend,
  type ConstellationType,
} from '@/utils/constellationGenerator'

const TYPES_ZH: Record<ConstellationType, string> = {
  starlink_v1: 'Starlink 第一代',
  starlink_v2: 'Starlink 第二代',
  oneweb: 'OneWeb 星座',
  polar: '极地轨道星座',
}

const IMPORT_EXAMPLE_JSON = `{
  "metadata": {
    "total_sats": 2,
    "num_planes": 1,
    "altitude_km": 550,
    "inclination_deg": 53,
    "topology_version": 1,
    "sampling_interval_sec": 15,
    "sim_time": "T+0000s",
    "timestamp": "2026-05-13T00:00:00.000Z"
  },
  "topology": {
    "nodes": [
      {
        "id": "SAT_000_000",
        "type": "satellite",
        "orbital_params": {
          "propagation_model": "SGP4",
          "plane": 0,
          "position_in_plane": 0,
          "raan": 0,
          "true_anomaly": 0,
          "altitude_km": 550,
          "inclination_deg": 53,
          "inclination": 53,
          "eccentricity": 0.0001,
          "argument_of_perigee_deg": 0,
          "mean_anomaly_deg": 0,
          "mean_motion_rev_per_day": 15.055,
          "bstar": 0.00005,
          "epoch_jd": 2461163.5,
          "epoch_iso": "2026-05-13T00:00:00.000Z",
          "propagation_minutes": 0,
          "semi_major_axis_km": 6921,
          "period_minutes": 95.648,
          "tle_line1": "1 00001U 26001A   26133.00000000  .00000000  00000-0  50000-4 0  9990",
          "tle_line2": "2 00001  53.0000   0.0000 0001000   0.0000   0.0000 15.05500000000000"
        },
        "coordinates": { "x": 6921, "y": 0, "z": 0, "lat": 0, "lon": 0 },
        "cpu_total": 24,
        "cpu_available": 24,
        "mem_total": 64,
        "mem_available": 64,
        "disk_total": 320,
        "disk_available": 320,
        "core_network_load": 0.18,
        "core_business_load": {
          "signaling_load": 0.19,
          "session_load": 0.2,
          "user_plane_load": 0.21,
          "mobility_load": 0.18,
          "policy_load": 0.17,
          "auth_load": 0.18,
          "load_index": 0.188333
        },
        "node_reliability": 0.995,
        "status": "active",
        "fault_tag": "",
        "vnfs": [],
        "core_nfs": []
      },
      {
        "id": "SAT_000_001",
        "type": "satellite",
        "orbital_params": {
          "propagation_model": "SGP4",
          "plane": 0,
          "position_in_plane": 1,
          "raan": 0,
          "true_anomaly": 180,
          "altitude_km": 550,
          "inclination_deg": 53,
          "inclination": 53,
          "eccentricity": 0.0001,
          "argument_of_perigee_deg": 0,
          "mean_anomaly_deg": 180,
          "mean_motion_rev_per_day": 15.055,
          "bstar": 0.00005,
          "epoch_jd": 2461163.5,
          "epoch_iso": "2026-05-13T00:00:00.000Z",
          "propagation_minutes": 0,
          "semi_major_axis_km": 6921,
          "period_minutes": 95.648,
          "tle_line1": "1 00002U 26001A   26133.00000000  .00000000  00000-0  50000-4 0  9990",
          "tle_line2": "2 00002  53.0000   0.0000 0001000   0.0000 180.0000 15.05500000000000"
        },
        "coordinates": { "x": -6921, "y": 0, "z": 0, "lat": 0, "lon": 180 },
        "cpu_total": 24,
        "cpu_available": 24,
        "mem_total": 64,
        "mem_available": 64,
        "disk_total": 320,
        "disk_available": 320,
        "core_network_load": 0.18,
        "core_business_load": {
          "signaling_load": 0.19,
          "session_load": 0.2,
          "user_plane_load": 0.21,
          "mobility_load": 0.18,
          "policy_load": 0.17,
          "auth_load": 0.18,
          "load_index": 0.188333
        },
        "node_reliability": 0.995,
        "status": "active",
        "fault_tag": "",
        "vnfs": [],
        "core_nfs": []
      }
    ],
    "links": [
      {
        "source": "SAT_000_000",
        "target": "SAT_000_001",
        "link_type": "intra_orbit",
        "status": "active",
        "fault_tag": "",
        "link_status": 1,
        "latency_ms": 2.4,
        "reliability": 0.999,
        "link_reliability": 0.999,
        "bandwidth_gbps": 20,
        "bandwidth_available_gbps": 18
      }
    ]
  }
}`

function validateImportedTopology(parsed: ReturnType<typeof parseThirdPartyTopology>): string[] {
  const errors: string[] = []
  const ids = new Set(parsed.satellites.map(s => s.id))

  if (parsed.satellites.length === 0) {
    errors.push('未检测到卫星节点')
  }

  parsed.satellites.forEach((sat, idx) => {
    if (!sat.id?.trim()) errors.push(`节点#${idx + 1} 缺少 id`)
    if (!Number.isFinite(sat.coordinates.x) || !Number.isFinite(sat.coordinates.y) || !Number.isFinite(sat.coordinates.z)) {
      errors.push(`节点 ${sat.id || idx} 坐标非法`)
    }
    if (!Number.isFinite(sat.cpu_total) || sat.cpu_total <= 0) {
      errors.push(`节点 ${sat.id || idx} cpu_total 非法`)
    }
    if (!Number.isFinite(sat.mem_total) || sat.mem_total <= 0) {
      errors.push(`节点 ${sat.id || idx} mem_total 非法`)
    }
    if (!Number.isFinite(sat.disk_total) || sat.disk_total <= 0) {
      errors.push(`节点 ${sat.id || idx} disk_total 非法`)
    }
  })

  parsed.links.forEach((link, idx) => {
    if (!ids.has(link.source) || !ids.has(link.target)) {
      errors.push(`链路#${idx + 1} 端点不存在: ${link.source} -> ${link.target}`)
    }
    if (!Number.isFinite(link.latency_ms) || link.latency_ms <= 0) {
      errors.push(`链路#${idx + 1} latency_ms 非法`)
    }
    if (!Number.isFinite(link.bandwidth_gbps) || link.bandwidth_gbps <= 0) {
      errors.push(`链路#${idx + 1} bandwidth_gbps 非法`)
    }
  })

  return errors
}

export default function LeftPanel() {
  const [collapsed, setCollapsed] = useState(false)
  const [loading, setLoading] = useState(false)
  const {
    setSatellites,
    setLinks,
    clearDeployments,
    clearHighlightedDeployments,
    setCandidateResult,
    bumpTopologyVersion,
    setBackendTopologySynced,
    setAutoDynamics,
    setSimulationStatus,
    applyTopologySnapshot,
    openSystemPopup,
    setSelectedSatellite,
    setSelectedLink,
  } = useStore()
  const [type, setType] = useState<ConstellationType>('starlink_v1')
  const [total, setTotal] = useState(72)
  const [planes, setPlanes] = useState(6)
  const [showImportExample, setShowImportExample] = useState(false)
  const importInputRef = useRef<HTMLInputElement>(null)

  const tpl = CONSTELLATION_TEMPLATES[type]
  const satsPerPlaneMin = planes > 0 ? Math.floor(total / planes) : 0
  const satsPerPlaneMax = planes > 0 ? Math.ceil(total / planes) : 0
  const actual = total

  const clearExistingOrchestrationState = async () => {
    clearHighlightedDeployments()
    clearDeployments()
    setCandidateResult(null)
    setSelectedSatellite(null)
    setSelectedLink(null)
    try {
      const list = await apiClient.listSFCSessions()
      if (Array.isArray(list) && list.length > 0) {
        await Promise.allSettled(
          list.map((sess: any) => apiClient.stopSFCSession(String(sess?.session_id ?? '')))
        )
      }
    } catch {
      // Ignore session clear errors; local clear has already completed.
    }
  }

  const doGenerate = async () => {
    setLoading(true)
    await clearExistingOrchestrationState()
    bumpTopologyVersion()
    setBackendTopologySynced(false)

    await new Promise(r => setTimeout(r, 30))
    try {
      const sats = generateConstellation(total, planes, type)
      const links = generateLinks(sats, type)

      setSatellites(sats as any)
      setLinks(links as any)
      setAutoDynamics({
        enabled: true,
        playing: true,
        elapsed_sec: 0,
      })

      try {
        const topologyData: any = prepareTopologyForBackend(sats, links, type, planes)
        topologyData.force_replace = true
        topologyData.metadata = {
          ...(topologyData.metadata ?? {}),
          constellation_template: type,
          force_replace: true,
        }
        await apiClient.generateTopology(topologyData)
        const topo = await apiClient.getTopology()
        applyTopologySnapshot(topo)
        try {
          const ad = useStore.getState().autoDynamics
          await apiClient.startDynamicSimulation({
            sampling_interval_sec: ad.resource_update_sec,
            simulation_speed: ad.time_scale,
          })
          setSimulationStatus({
            running: true,
            sampling_interval_sec: ad.resource_update_sec,
            simulation_speed: ad.time_scale,
          })
        } catch (e) {
          console.warn('动态仿真自动启动失败，将继续使用前端准实时动画模式', e)
        }
        setBackendTopologySynced(true)
      } catch (err: any) {
        setBackendTopologySynced(false)
        console.warn('后端同步失败', err?.message)
        openSystemPopup(
          '后端同步失败',
          '星座已在前端更新，但后端同步失败。为避免部署使用旧星座，请先确保后端可用并重新生成/导入。',
          'error',
        )
      }
      clearDeployments()

    } finally {
      setLoading(false)
    }
  }

  const onTypeChange = (t: ConstellationType) => {
    setType(t)
    const def = CONSTELLATION_TEMPLATES[t]
    setTotal(def.defaultSats)
    setPlanes(def.defaultPlanes)
  }

  const onImportThirdParty = async (file?: File) => {
    if (!file) return
    setLoading(true)
    try {
      await clearExistingOrchestrationState()
      const text = await file.text()
      const raw = JSON.parse(text)
      const parsed = parseThirdPartyTopology(raw)
      const errors = validateImportedTopology(parsed)

      if (errors.length > 0) {
        openSystemPopup('导入校验失败', `共 ${errors.length} 项问题：\n${errors.slice(0, 8).join('\n')}`, 'warning')
        return
      }

      setSatellites(parsed.satellites as any)
      setLinks(parsed.links as any)
      setAutoDynamics({
        enabled: true,
        playing: true,
        elapsed_sec: 0,
      })
      clearHighlightedDeployments()
      setCandidateResult(null)
      bumpTopologyVersion()
      setBackendTopologySynced(false)

      try {
        const rawMetadata = raw?.metadata ?? {}
        const importedNumPlanes = Number(rawMetadata?.num_planes)
        const importedAltitude = Number(rawMetadata?.altitude_km)
        const importedInclination = Number(rawMetadata?.inclination_deg)
        const topologyData: any = {
          metadata: {
            ...(rawMetadata ?? {}),
            total_sats: parsed.satellites.length,
            num_planes: Number.isFinite(importedNumPlanes) && importedNumPlanes > 0 ? importedNumPlanes : planes,
            altitude_km: Number.isFinite(importedAltitude) && importedAltitude > 0
              ? importedAltitude
              : parsed.satellites[0]?.orbital_params?.altitude_km ?? tpl.altitude_km,
            inclination_deg: Number.isFinite(importedInclination) && importedInclination > 0
              ? importedInclination
              : parsed.satellites[0]?.orbital_params?.inclination ?? tpl.inclination_deg,
            constellation_template: 'imported_topology',
            force_replace: true,
            timestamp: rawMetadata?.timestamp ?? new Date().toISOString(),
          },
          force_replace: true,
          topology: {
            nodes: parsed.satellites,
            links: parsed.links,
          },
        }
        await apiClient.generateTopology(topologyData)
        const topo = await apiClient.getTopology()
        applyTopologySnapshot(topo)
        try {
          const ad = useStore.getState().autoDynamics
          await apiClient.startDynamicSimulation({
            sampling_interval_sec: ad.resource_update_sec,
            simulation_speed: ad.time_scale,
          })
          setSimulationStatus({
            running: true,
            sampling_interval_sec: ad.resource_update_sec,
            simulation_speed: ad.time_scale,
          })
        } catch (e) {
          console.warn('动态仿真自动启动失败，将继续使用前端准实时动画模式', e)
        }
        setBackendTopologySynced(true)
        openSystemPopup(
          '导入成功',
          `导入成功并通过校验：${parsed.satellites.length} 颗卫星，${parsed.links.length} 条链路。`,
          'success',
        )
      } catch (e) {
        setBackendTopologySynced(false)
        console.warn('导入拓扑已本地生效，但后端同步失败', e)
        openSystemPopup(
          '后端同步失败',
          '导入拓扑后端同步失败。为避免部署使用旧星座，请先修复后端连接后重新导入。',
          'error',
        )
      }
      clearDeployments()
    } catch (e) {
      console.error(e)
      openSystemPopup('导入失败', '请提供合法的 JSON 拓扑文件。', 'error')
    } finally {
      setLoading(false)
    }
  }

  return (
    <div
      className="absolute left-0 top-11 bottom-11 z-10 flex transition-all duration-300"
      style={{ width: collapsed ? 32 : 'clamp(248px, 17vw, 368px)' }}
    >
      <button
        onClick={() => setCollapsed(!collapsed)}
        className="absolute -right-3 top-5 w-6 h-6 rounded-full flex items-center justify-center z-20 shadow-lg transition-colors"
        style={{ background: 'linear-gradient(135deg, #0f2036, #182f45)' }}
      >
        {collapsed ? <ChevronRight className="w-3 h-3 text-gray-200" /> : <ChevronLeft className="w-3 h-3 text-gray-200" />}
      </button>

      {!collapsed && (
        <div
          className="w-full flex flex-col"
          style={{
            background: 'linear-gradient(180deg, rgba(4,8,14,0.95) 0%, rgba(6,12,21,0.92) 55%, rgba(9,20,33,0.9) 100%)',
            borderRight: '1px solid rgba(87, 126, 160, 0.24)',
            backdropFilter: 'blur(16px)',
            boxShadow: 'inset -1px 0 0 rgba(103,164,209,0.12), inset -30px 0 60px rgba(34,99,152,0.08)',
          }}
        >
          <div className="px-3 py-2" style={{ borderBottom: '1px solid rgba(95, 128, 156, 0.2)' }}>
            <div>
              <div className="text-xs font-bold text-slate-100 uppercase tracking-wider">星座构建</div>
              <div className="text-[10px] text-slate-400 mt-0.5">星座参数化构型规划接入</div>
            </div>
          </div>

          <div className="flex-1 overflow-y-auto px-3 py-2">
            <div className="min-h-full flex flex-col">
              <div className="space-y-2">
            <div>
              <label className="block text-[10px] font-semibold text-slate-400 mb-1.5 uppercase tracking-wide">星座构型</label>
              <select
                value={type}
                onChange={e => onTypeChange(e.target.value as ConstellationType)}
                className="w-full px-2.5 py-1.5 rounded-xl text-sm font-medium text-slate-100 transition-all"
                style={{ background: 'rgba(22,31,48,0.84)', border: '1px solid rgba(113, 140, 167, 0.28)', outline: 'none' }}
              >
                {Object.entries(TYPES_ZH).map(([k, v]) => {
                  const t = CONSTELLATION_TEMPLATES[k as ConstellationType]
                  return (
                    <option key={k} value={k}>
                      {v} · {t.operator}
                    </option>
                  )
                })}
              </select>
            </div>

            <div
              className="rounded-xl p-2.5 space-y-2"
              style={{ background: 'linear-gradient(140deg, rgba(17,30,49,0.82), rgba(12,20,35,0.92))', border: '1px solid rgba(81, 130, 170, 0.35)' }}
            >
              <div className="grid grid-cols-2 gap-1.5">
                {[
                  { icon: <Orbit className="w-3 h-3" />, label: '轨道高度', value: `${tpl.altitude_km} km` },
                  { icon: <Activity className="w-3 h-3" />, label: '轨道倾角', value: `${tpl.inclination_deg}°` },
                  { icon: <Cpu className="w-3 h-3" />, label: 'CPU 容量', value: `${tpl.cpuPerSat[0]}~${tpl.cpuPerSat[1]}` },
                  { icon: <Database className="w-3 h-3" />, label: '内存容量', value: `${tpl.memPerSat[0]}~${tpl.memPerSat[1]} GB` },
                ].map(item => (
                  <div
                    key={item.label}
                    className="rounded-lg px-2 py-1"
                    style={{ background: 'rgba(8,16,28,0.8)', border: '1px solid rgba(97,127,155,0.22)' }}
                  >
                    <div className="text-[9px] text-slate-500 uppercase tracking-wide mb-0.5 flex items-center gap-1">
                      {item.icon}
                      {item.label}
                    </div>
                    <div className="text-[11px] text-slate-100 font-semibold">{item.value}</div>
                  </div>
                ))}
              </div>

              <div className="grid grid-cols-3 gap-1.5 text-center">
                {[
                  ['轨道内链路', `${tpl.linkBudget.intraGbps} Gbps`],
                  ['跨轨道链路', `${tpl.linkBudget.interGbps} Gbps`],
                  ['磁盘容量', `${tpl.diskPerSat[0]}~${tpl.diskPerSat[1]} GB`],
                ].map(([label, value]) => (
                  <div
                    key={label}
                    className="rounded-lg px-1.5 py-1"
                    style={{ background: 'rgba(7,13,24,0.74)', border: '1px solid rgba(100,128,152,0.2)' }}
                  >
                    <div className="text-[8px] text-slate-500 mb-0.5">{label}</div>
                    <div className="text-[10px] text-cyan-100 font-semibold">{value}</div>
                  </div>
                ))}
              </div>
            </div>

            <div className="grid grid-cols-2 gap-2">
              <div>
                <label className="block text-[10px] font-semibold text-slate-400 mb-1 uppercase tracking-wide">卫星总数</label>
                <input
                  type="number"
                  value={total}
                  min={24}
                  max={6000}
                  step={1}
                  onChange={e => setTotal(Math.max(24, Math.min(6000, parseInt(e.target.value) || 24)))}
                  className="w-full h-8 px-2 rounded-lg text-sm font-semibold text-cyan-100 bg-black/30 border border-slate-700/80 outline-none"
                />
              </div>
              <div>
                <label className="block text-[10px] font-semibold text-slate-400 mb-1 uppercase tracking-wide">轨道面数</label>
                <input
                  type="number"
                  value={planes}
                  min={1}
                  max={72}
                  step={1}
                  onChange={e => setPlanes(Math.max(1, parseInt(e.target.value) || 1))}
                  className="w-full h-8 px-2 rounded-lg text-sm font-semibold text-sky-100 bg-black/30 border border-slate-700/80 outline-none"
                />
              </div>
            </div>

            <div
              className="rounded-xl px-2.5 py-1.5 text-center"
              style={{ background: 'rgba(26, 85, 119, 0.2)', border: '1px solid rgba(88, 153, 194, 0.38)' }}
            >
              <div className="text-[9px] text-cyan-300 font-semibold mb-0.5">构型规模预估</div>
              <div className="text-2xl font-bold text-cyan-100 leading-tight">{actual}</div>
              <div className="text-[10px] text-slate-300 mt-0.5">{satsPerPlaneMin}~{satsPerPlaneMax} 颗/面 × {planes} 面</div>
            </div>
              </div>

              <div className="mt-auto pt-2 space-y-2">

            <button
              onClick={doGenerate}
              disabled={loading}
              className="w-full py-2 rounded-xl text-sm font-bold text-white flex items-center justify-center gap-2 transition shadow-lg mt-2"
              style={{ background: loading ? 'rgba(50,50,60,0.8)' : 'linear-gradient(135deg, #12345a 0%, #0f233b 100%)' }}
            >
              <RefreshCw className={`w-4 h-4 ${loading ? 'animate-spin' : ''}`} />
              {loading ? '生成中...' : '生成模拟星座'}
            </button>

            <div className="w-full mt-2 flex items-center gap-2">
              <button
                onClick={() => importInputRef.current?.click()}
                disabled={loading}
                className="flex-1 py-2 rounded-lg text-xs font-semibold text-slate-200 transition"
                style={{ background: 'rgba(20,31,49,0.84)', border: '1px solid rgba(108, 138, 165, 0.28)' }}
              >
                导入第三方构型
              </button>
              <button
                type="button"
                onClick={() => setShowImportExample(v => !v)}
                className="px-2.5 py-2 rounded-lg text-[11px] font-semibold text-cyan-200 transition"
                style={{ background: 'rgba(16,44,66,0.82)', border: '1px solid rgba(89, 149, 188, 0.38)' }}
              >
                <span className="inline-flex items-center gap-1"><FileCode2 className="w-3.5 h-3.5" /> 示例</span>
              </button>
            </div>

            {showImportExample && (
              <div
                className="mt-2 rounded-lg p-2 max-h-44 overflow-auto"
                style={{ background: 'rgba(6,14,24,0.85)', border: '1px solid rgba(102, 134, 158, 0.28)' }}
              >
                <div className="text-[10px] text-slate-400 mb-1">示例 JSON（后端 Topology 结构）</div>
                <pre className="text-[9px] leading-4 text-slate-300 whitespace-pre-wrap">{IMPORT_EXAMPLE_JSON}</pre>
              </div>
            )}

            <input
              ref={importInputRef}
              type="file"
              accept=".json,application/json"
              className="hidden"
              onChange={e => onImportThirdParty(e.target.files?.[0])}
            />
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
