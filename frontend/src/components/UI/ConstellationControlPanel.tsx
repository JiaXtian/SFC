import { useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { RefreshCw, Upload, Orbit, Activity, Cpu, Database, FileText, X } from 'lucide-react'
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

const CONTROL_TYPES: ConstellationType[] = [
  'starlink_v1',
  'starlink_v2',
  'oneweb',
  'iridium',
  'telesat',
  'kuiper',
  'polar',
]

function validateImportedTopology(parsed: ReturnType<typeof parseThirdPartyTopology>): string[] {
  const errors: string[] = []
  const ids = new Set(parsed.satellites.map(s => s.id))
  if (parsed.satellites.length === 0) errors.push('未检测到卫星节点')
  parsed.links.forEach((link, idx) => {
    if (!ids.has(link.source) || !ids.has(link.target)) {
      errors.push(`链路#${idx + 1} 端点不存在`)
    }
  })
  return errors
}

export default function ConstellationControlPanel({
  readonly = false,
  onUnauthorized,
}: {
  readonly?: boolean
  onUnauthorized?: () => void
}) {
  const [loading, setLoading] = useState(false)
  const [type, setType] = useState<ConstellationType>('starlink_v1')
  const [total, setTotal] = useState(72)
  const [planes, setPlanes] = useState(6)
  const [showImportExample, setShowImportExample] = useState(false)
  const importInputRef = useRef<HTMLInputElement>(null)

  const {
    setSatellites,
    setLinks,
    satellites,
    deployments,
    clearDeployments,
    clearHighlightedDeployments,
    setCandidateResult,
    bumpTopologyVersion,
    setBackendTopologySynced,
    setAutoDynamics,
    setSimulationStatus,
    applyTopologySnapshot,
    setSelectedSatellite,
    setSelectedLink,
    addToast,
  } = useStore()

  const tpl = CONSTELLATION_TEMPLATES[type]

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
    } catch {}
  }

  const syncAndStartDynamic = async (sats: any[], links: any[], templateId: string) => {
    const topologyData: any = prepareTopologyForBackend(sats, links, type, planes)
    topologyData.force_replace = true
    topologyData.metadata = {
      ...(topologyData.metadata ?? {}),
      constellation_template: templateId,
      force_replace: true,
    }
    await apiClient.generateTopology(topologyData)
    const topo = await apiClient.getTopology()
    applyTopologySnapshot(topo)
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
  }

  const doGenerate = async () => {
    if (readonly) {
      onUnauthorized?.()
      return
    }
    if ((satellites.length > 0 || deployments.length > 0) &&
      !window.confirm('再次生成星座将清除所有已有卫星节点和已部署策略，是否继续？')) {
      return
    }
    setLoading(true)
    try {
      await clearExistingOrchestrationState()
      bumpTopologyVersion()
      setBackendTopologySynced(false)
      const sats = generateConstellation(total, planes, type)
      const links = generateLinks(sats, type)
      setSatellites(sats as any)
      setLinks(links as any)
      setAutoDynamics({ enabled: true, playing: true, elapsed_sec: 0 })
      await syncAndStartDynamic(sats, links, type)
      setBackendTopologySynced(true)
      addToast(`星座已更新：${sats.length} 节点 / ${links.length} 链路`, 'success')
    } catch (e: any) {
      setBackendTopologySynced(false)
      addToast(`生成或同步失败: ${e?.message ?? e}`, 'error')
    } finally {
      setLoading(false)
    }
  }

  const onImportThirdParty = async (file?: File) => {
    if (!file) return
    if (readonly) {
      onUnauthorized?.()
      return
    }
    if ((satellites.length > 0 || deployments.length > 0) &&
      !window.confirm('导入新星座将清除所有已有卫星节点和已部署策略，是否继续？')) {
      return
    }
    setLoading(true)
    try {
      await clearExistingOrchestrationState()
      const text = await file.text()
      const raw = JSON.parse(text)
      const parsed = parseThirdPartyTopology(raw)
      const errors = validateImportedTopology(parsed)
      if (errors.length > 0) {
        addToast(`导入失败: ${errors[0]}`, 'error')
        return
      }

      setSatellites(parsed.satellites as any)
      setLinks(parsed.links as any)
      setAutoDynamics({ enabled: true, playing: true, elapsed_sec: 0 })
      bumpTopologyVersion()
      setBackendTopologySynced(false)
      await syncAndStartDynamic(parsed.satellites as any[], parsed.links as any[], 'imported_topology')
      setBackendTopologySynced(true)
      addToast(`导入成功：${parsed.satellites.length} 节点 / ${parsed.links.length} 链路`, 'success')
    } catch (e: any) {
      setBackendTopologySynced(false)
      addToast(`导入失败: ${e?.message ?? e}`, 'error')
    } finally {
      setLoading(false)
    }
  }

  return (
    <div
      className="rounded-2xl p-3"
      style={{
        background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
        border: '1px solid rgba(112,168,208,0.28)',
        backdropFilter: 'blur(14px)',
      }}
    >
      <div className="text-[14px] uppercase tracking-wide text-cyan-100 font-semibold mb-2">星座模拟生成</div>
      <div className="space-y-2.5">
        <select
          value={type}
          onChange={(e) => {
            const next = e.target.value as ConstellationType
            const def = CONSTELLATION_TEMPLATES[next]
            setType(next)
            setTotal(def.defaultSats)
            setPlanes(def.defaultPlanes)
          }}
          className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-slate-100 text-[13px]"
        >
          {CONTROL_TYPES.map((k) => (
            <option key={k} value={k}>{TYPES_ZH[k]}</option>
          ))}
        </select>

        <div className="grid grid-cols-2 gap-2 text-[12px]">
          <label className="space-y-1">
            <div className="text-slate-400">卫星总数</div>
            <input
              type="number"
              min={24}
              max={6000}
              value={total}
              onChange={(e) => setTotal(Math.max(24, Math.min(6000, Number(e.target.value) || 24)))}
              className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-cyan-100"
            />
          </label>
          <label className="space-y-1">
            <div className="text-slate-400">轨道面数</div>
            <input
              type="number"
              min={1}
              max={72}
              value={planes}
              onChange={(e) => setPlanes(Math.max(1, Math.min(72, Number(e.target.value) || 1)))}
              className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-cyan-100"
            />
          </label>
        </div>

        <div className="grid grid-cols-2 gap-2 text-[11px]">
          <div className="rounded-lg px-2 py-1.5 bg-slate-900/35 border border-slate-700/60 text-slate-300 inline-flex items-center gap-1"><Orbit className="w-3 h-3 text-cyan-300" />高度 {tpl.altitude_km}km</div>
          <div className="rounded-lg px-2 py-1.5 bg-slate-900/35 border border-slate-700/60 text-slate-300 inline-flex items-center gap-1"><Activity className="w-3 h-3 text-cyan-300" />倾角 {tpl.inclination_deg}°</div>
          <div className="rounded-lg px-2 py-1.5 bg-slate-900/35 border border-slate-700/60 text-slate-300 inline-flex items-center gap-1"><Cpu className="w-3 h-3 text-cyan-300" />CPU {tpl.cpuPerSat[0]}~{tpl.cpuPerSat[1]}</div>
          <div className="rounded-lg px-2 py-1.5 bg-slate-900/35 border border-slate-700/60 text-slate-300 inline-flex items-center gap-1"><Database className="w-3 h-3 text-cyan-300" />内存 {tpl.memPerSat[0]}~{tpl.memPerSat[1]}GB</div>
        </div>

        <button
          onClick={doGenerate}
          disabled={loading}
          className="w-full h-10 rounded-lg text-[13px] font-semibold text-white flex items-center justify-center gap-2 disabled:opacity-60"
          style={{
            background: 'linear-gradient(135deg, rgba(23,93,139,0.88), rgba(16,63,102,0.88))',
            border: '1px solid rgba(130,201,243,0.34)',
          }}
        >
          <RefreshCw className={`w-3.5 h-3.5 ${loading ? 'animate-spin' : ''}`} />
          {loading ? '处理中...' : '生成模拟星座'}
        </button>

        <div className="grid grid-cols-[1fr_auto] gap-2">
          <button
            onClick={() => importInputRef.current?.click()}
            disabled={loading}
            className="h-9 rounded-lg text-[13px] text-slate-200 flex items-center justify-center gap-2 disabled:opacity-60 bg-slate-900/50 border border-slate-700/70"
          >
            <Upload className="w-3.5 h-3.5" />
            导入第三方拓扑
          </button>
          <button
            type="button"
            onClick={() => setShowImportExample(true)}
            className="h-9 px-3 rounded-lg text-[12px] text-cyan-100 bg-cyan-500/10 border border-cyan-500/35 inline-flex items-center justify-center gap-1.5"
          >
            <FileText className="w-3.5 h-3.5" />
            示例
          </button>
        </div>
        <input
          ref={importInputRef}
          type="file"
          accept=".json,application/json"
          className="hidden"
          onChange={(e) => onImportThirdParty(e.target.files?.[0])}
        />
      </div>
      {showImportExample && createPortal((
        <div className="fixed inset-0 z-[10000] flex items-center justify-center bg-black/65 px-4">
          <div className="w-[680px] max-w-full max-h-[82vh] overflow-hidden rounded-2xl border border-cyan-400/25 bg-slate-950 shadow-2xl">
            <div className="flex items-center justify-between px-4 py-3 border-b border-slate-800/80">
              <div>
                <div className="text-[13px] font-semibold text-cyan-100">第三方拓扑 JSON 示例字段</div>
                <div className="text-[10px] text-slate-400 mt-0.5">支持 nodes/links 直接结构或 topology.nodes/topology.links 嵌套结构</div>
              </div>
              <button onClick={() => setShowImportExample(false)} className="h-8 w-8 rounded-lg inline-flex items-center justify-center text-slate-300 hover:bg-white/10">
                <X className="w-4 h-4" />
              </button>
            </div>
            <pre className="max-h-[65vh] overflow-auto p-4 text-[11px] leading-relaxed text-slate-200 bg-slate-950/95">
{`{
  "metadata": {
    "total_sats": 2,
    "num_planes": 1,
    "altitude_km": 550,
    "inclination_deg": 53,
    "propagation_model": "SGP4"
  },
  "nodes": [
    {
      "id": "SAT_000_000",
      "type": "satellite",
      "orbital_params": {
        "propagation_model": "SGP4",
        "tle_line1": "1 00001U 26001A   26131.00000000  .00000000  00000-0  50000-4 0  9990",
        "tle_line2": "2 00001  53.0000   0.0000 0001000   0.0000   0.0000 15.05500000000000",
        "plane": 0,
        "position_in_plane": 0,
        "raan": 0,
        "true_anomaly": 0,
        "altitude_km": 550,
        "inclination_deg": 53,
        "eccentricity": 0.0001,
        "mean_anomaly_deg": 0,
        "mean_motion_rev_per_day": 15.05,
        "bstar": 0.00005,
        "epoch_iso": "2026-05-11T00:00:00Z"
      },
      "coordinates": { "x": 6928.1, "y": 0, "z": 0, "lat": 0, "lon": 0 },
      "cpu_total": 24,
      "cpu_available": 24,
      "mem_total": 64,
      "mem_available": 64,
      "disk_total": 320,
      "disk_available": 320,
      "node_reliability": 0.985,
      "status": "active"
    },
    {
      "id": "SAT_000_001",
      "type": "satellite",
      "orbital_params": {
        "propagation_model": "SGP4",
        "tle_line1": "1 00002U 26001A   26131.00000000  .00000000  00000-0  50000-4 0  9990",
        "tle_line2": "2 00002  53.0000   0.0000 0001000   0.0000 180.0000 15.05500000000000",
        "plane": 0,
        "position_in_plane": 1,
        "raan": 0,
        "true_anomaly": 180,
        "altitude_km": 550,
        "inclination_deg": 53,
        "eccentricity": 0.0001,
        "mean_anomaly_deg": 180,
        "mean_motion_rev_per_day": 15.05,
        "bstar": 0.00005,
        "epoch_iso": "2026-05-11T00:00:00Z"
      },
      "coordinates": { "x": -6928.1, "y": 0, "z": 0, "lat": 0, "lon": 180 },
      "cpu_total": 24,
      "cpu_available": 24,
      "mem_total": 64,
      "mem_available": 64,
      "disk_total": 320,
      "disk_available": 320,
      "node_reliability": 0.985,
      "status": "active"
    }
  ],
  "links": [
    {
      "source": "SAT_000_000",
      "target": "SAT_000_001",
      "link_type": "intra_orbit",
      "status": "active",
      "latency_ms": 2.4,
      "reliability": 0.999,
      "bandwidth_gbps": 20,
      "bandwidth_available_gbps": 18
    }
  ]
}`}
            </pre>
          </div>
        </div>
      ), document.body)}
    </div>
  )
}
