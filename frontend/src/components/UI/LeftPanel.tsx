import { useRef, useState } from 'react'
import {
  ChevronLeft,
  ChevronRight,
  RefreshCw,
  AlertCircle,
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
  iridium: 'Iridium NEXT',
  telesat: 'Telesat Lightspeed',
  kuiper: 'Amazon Kuiper',
  polar: '极地轨道星座',
  qianfan: '千帆星座',
}

const IMPORT_EXAMPLE_JSON = `{
  "metadata": {
    "total_sats": 4,
    "num_planes": 2,
    "altitude_km": 550,
    "inclination_deg": 53,
    "timestamp": "2026-01-01T00:00:00.000Z"
  },
  "topology": {
    "nodes": [
      {
        "id": "SAT_000_000",
        "orbital_params": {
          "plane": 0,
          "position_in_plane": 0,
          "raan": 0,
          "true_anomaly": 0,
          "altitude_km": 550,
          "inclination": 53
        },
        "coordinates": { "x": 6921, "y": 0, "z": 0, "lat": 0, "lon": 0 },
        "cpu_total": 24,
        "cpu_available": 24,
        "mem_total": 64,
        "mem_available": 64,
        "disk_total": 320,
        "disk_available": 320,
        "vnfs": []
      }
    ],
    "links": [
      {
        "source": "SAT_000_000",
        "target": "SAT_000_001",
        "link_type": "intra_orbit",
        "status": "active",
        "reliability": 0.999,
        "latency_ms": 3.2,
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
    deployments,
    clearHighlightedDeployments,
    setCandidateResult,
    bumpTopologyVersion,
    setBackendTopologySynced,
    setAutoDynamics,
    setSimulationStatus,
  } = useStore()
  const [type, setType] = useState<ConstellationType>('starlink_v1')
  const [total, setTotal] = useState(72)
  const [planes, setPlanes] = useState(6)
  const [showWarning, setShowWarning] = useState(false)
  const [showImportExample, setShowImportExample] = useState(false)
  const importInputRef = useRef<HTMLInputElement>(null)

  const tpl = CONSTELLATION_TEMPLATES[type]
  const satsPerPlaneMin = planes > 0 ? Math.floor(total / planes) : 0
  const satsPerPlaneMax = planes > 0 ? Math.ceil(total / planes) : 0
  const actual = total

  const handleGenerate = async () => {
    if (deployments.length > 0) {
      setShowWarning(true)
      return
    }
    doGenerate()
  }

  const doGenerate = async () => {
    setShowWarning(false)
    setLoading(true)
    clearHighlightedDeployments()
    setCandidateResult(null)
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
        const topologyData = prepareTopologyForBackend(sats, links, type, planes)
        await apiClient.generateTopology(topologyData)
        try {
          const ad = useStore.getState().autoDynamics
          await apiClient.startDynamicSimulation({
            sampling_interval_sec: ad.resource_update_sec,
            simulation_speed: ad.time_scale,
            enable_faults: true,
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
        alert('星座已在前端更新，但后端同步失败。为避免部署使用旧星座，请先确保后端可用并重新生成/导入。')
      }

      const store = useStore.getState()
      store.deployments.forEach(d => store.removeDeployment(d.deployment_id))
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
      const text = await file.text()
      const raw = JSON.parse(text)
      const parsed = parseThirdPartyTopology(raw)
      const errors = validateImportedTopology(parsed)

      if (errors.length > 0) {
        alert(`导入校验失败（共 ${errors.length} 项）:\n${errors.slice(0, 8).join('\n')}`)
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
        const topologyData = {
          metadata: {
            total_sats: parsed.satellites.length,
            num_planes: planes,
            altitude_km: parsed.satellites[0]?.orbital_params?.altitude_km ?? tpl.altitude_km,
            inclination_deg: parsed.satellites[0]?.orbital_params?.inclination ?? tpl.inclination_deg,
            timestamp: new Date().toISOString(),
          },
          nodes: parsed.satellites,
          links: parsed.links,
        }
        await apiClient.generateTopology(topologyData)
        try {
          const ad = useStore.getState().autoDynamics
          await apiClient.startDynamicSimulation({
            sampling_interval_sec: ad.resource_update_sec,
            simulation_speed: ad.time_scale,
            enable_faults: true,
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
        alert(`导入成功并通过校验：${parsed.satellites.length} 颗卫星，${parsed.links.length} 条链路。`)
      } catch (e) {
        setBackendTopologySynced(false)
        console.warn('导入拓扑已本地生效，但后端同步失败', e)
        alert('导入拓扑后端同步失败。为避免部署使用旧星座，请先修复后端连接后重新导入。')
      }
    } catch (e) {
      console.error(e)
      alert('导入失败：请提供合法的 JSON 拓扑文件')
    } finally {
      setLoading(false)
    }
  }

  return (
    <div className={`absolute left-0 top-11 bottom-11 z-10 flex transition-all duration-300 ${collapsed ? 'w-8' : 'w-[268px]'}`}>
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

          <div className="flex-1 overflow-y-auto px-3 py-2 space-y-2">
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

            <button
              onClick={handleGenerate}
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
                <div className="text-[10px] text-slate-400 mb-1">示例 JSON（可直接参考字段）</div>
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
      )}

      {showWarning && (
        <div className="fixed inset-0 z-50 flex items-center justify-center" style={{ background: 'rgba(0,0,0,0.8)' }}>
          <div
            className="w-96 rounded-2xl overflow-hidden shadow-2xl"
            style={{ background: 'linear-gradient(180deg, #1a1a2e 0%, #0a0a15 100%)', border: '1px solid rgba(248,113,113,0.3)' }}
          >
            <div
              className="px-5 py-4 flex items-center gap-3"
              style={{ background: 'rgba(239,68,68,0.15)', borderBottom: '1px solid rgba(248,113,113,0.2)' }}
            >
              <AlertCircle className="w-5 h-5 text-red-400" />
              <div className="text-sm font-bold text-white">警告：存在活跃部署</div>
            </div>
            <div className="px-5 py-4 space-y-3">
              <p className="text-sm text-gray-300 leading-relaxed">重新生成星座将清除全部 {deployments.length} 个活跃的 SFC 部署。</p>
              <p className="text-xs text-gray-500">请确认是否继续。</p>
            </div>
            <div className="px-5 py-3 flex gap-2" style={{ borderTop: '1px solid rgba(100,100,120,0.15)' }}>
              <button
                onClick={() => setShowWarning(false)}
                className="flex-1 py-2 rounded-lg text-sm font-medium text-gray-300 transition"
                style={{ background: 'rgba(50,50,60,0.5)' }}
              >
                取消
              </button>
              <button
                onClick={doGenerate}
                className="flex-1 py-2 rounded-lg text-sm font-bold text-white transition"
                style={{ background: 'linear-gradient(135deg, #dc2626, #b91c1c)' }}
              >
                确认清除并重建
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
