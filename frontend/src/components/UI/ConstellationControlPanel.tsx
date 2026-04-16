import { useEffect, useMemo, useRef, useState } from 'react'
import { AlertTriangle, FileCode2, Orbit, RefreshCw, UploadCloud } from 'lucide-react'
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
        "status": "active"
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

type DeployProgress = {
  active: boolean
  phase: string
  message: string
  total: number
  processed: number
  running: number
  created: number
  simulated: number
  stopped: number
  failed: number
  percent: number
  elapsedSec: number
  etaSec: number | null
}

function toNumber(v: any, fallback = 0) {
  const n = Number(v)
  return Number.isFinite(n) ? n : fallback
}

function validateImportedTopology(parsed: ReturnType<typeof parseThirdPartyTopology>): string[] {
  const errors: string[] = []
  const ids = new Set(parsed.satellites.map(s => s.id))
  if (parsed.satellites.length === 0) errors.push('未检测到卫星节点')

  parsed.satellites.forEach((sat, idx) => {
    if (!sat.id?.trim()) errors.push(`节点#${idx + 1} 缺少 id`)
    if (!Number.isFinite(sat.cpu_total) || sat.cpu_total <= 0) errors.push(`节点 ${sat.id || idx} cpu_total 非法`)
    if (!Number.isFinite(sat.mem_total) || sat.mem_total <= 0) errors.push(`节点 ${sat.id || idx} mem_total 非法`)
    if (!Number.isFinite(sat.disk_total) || sat.disk_total <= 0) errors.push(`节点 ${sat.id || idx} disk_total 非法`)
  })

  parsed.links.forEach((link, idx) => {
    if (!ids.has(link.source) || !ids.has(link.target)) {
      errors.push(`链路#${idx + 1} 端点不存在: ${link.source} -> ${link.target}`)
    }
    if (!Number.isFinite(link.latency_ms) || link.latency_ms <= 0) errors.push(`链路#${idx + 1} latency_ms 非法`)
    if (!Number.isFinite(link.bandwidth_gbps) || link.bandwidth_gbps <= 0) errors.push(`链路#${idx + 1} bandwidth_gbps 非法`)
  })
  return errors
}

export default function ConstellationControlPanel() {
  const [loading, setLoading] = useState(false)
  const [type, setType] = useState<ConstellationType>('starlink_v1')
  const [total, setTotal] = useState(72)
  const [planes, setPlanes] = useState(6)
  const [showImportExample, setShowImportExample] = useState(false)
  const importInputRef = useRef<HTMLInputElement>(null)
  const progressTimerRef = useRef<number | null>(null)
  const startedAtRef = useRef<number>(0)
  const seenProgressLogsRef = useRef<Set<string>>(new Set())

  const [progress, setProgress] = useState<DeployProgress>({
    active: false,
    phase: 'idle',
    message: '',
    total: 0,
    processed: 0,
    running: 0,
    created: 0,
    simulated: 0,
    stopped: 0,
    failed: 0,
    percent: 0,
    elapsedSec: 0,
    etaSec: null,
  })
  const [bootLogs, setBootLogs] = useState<string[]>([])

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
    setSelectedSatellite,
    setSelectedLink,
    addToast,
    openSystemPopup,
  } = useStore()

  const tpl = CONSTELLATION_TEMPLATES[type]
  const satsPerPlaneMin = planes > 0 ? Math.floor(total / planes) : 0
  const satsPerPlaneMax = planes > 0 ? Math.ceil(total / planes) : 0

  const stopProgressPolling = () => {
    if (progressTimerRef.current) {
      window.clearInterval(progressTimerRef.current)
      progressTimerRef.current = null
    }
  }

  const appendBootLog = (line: string) => {
    setBootLogs(prev => [line, ...prev].slice(0, 240))
  }

  const startProgressPolling = (expectedTotal: number, message: string) => {
    stopProgressPolling()
    seenProgressLogsRef.current = new Set()
    startedAtRef.current = Date.now()
    setBootLogs([])
    setProgress({
      active: true,
      phase: 'provisioning',
      message,
      total: expectedTotal,
      processed: 0,
      running: 0,
      created: 0,
      simulated: 0,
      stopped: 0,
      failed: 0,
      percent: 0,
      elapsedSec: 0,
      etaSec: null,
    })
    progressTimerRef.current = window.setInterval(async () => {
      try {
        const elapsed = Math.max(0, Math.floor((Date.now() - startedAtRef.current) / 1000))
        const status = await apiClient.getRuntimeStatus()
        const runtime = status?.runtime ?? status ?? {}
        const provisioning = runtime?.provisioning ?? {}
        const requested = Math.max(
          expectedTotal,
          toNumber(provisioning?.requested, toNumber(runtime.total_nodes, expectedTotal)),
        )
        const processed = Math.max(0, toNumber(provisioning?.processed, 0))
        const running = Math.max(0, toNumber(runtime.running, 0))
        const stopped = Math.max(0, toNumber(runtime.stopped, 0))
        const created = Math.max(0, toNumber(runtime.created, 0))
        const simulated = Math.max(0, toNumber(runtime.simulated, 0))
        const notCreated = Math.max(0, toNumber(runtime.not_created, 0))
        const failed = Math.max(0, toNumber(runtime.failed, 0))
        const effectiveByState = Math.max(0, running + stopped + created + simulated + failed)
        const effectiveProcessed = Math.min(
          requested,
          Math.max(processed, effectiveByState),
        )
        const percent = requested > 0 ? Math.max(0, Math.min(1, effectiveProcessed / requested)) : 0
        const etaSec = effectiveProcessed > 0 && effectiveProcessed < requested
          ? Math.max(0, Math.round((elapsed / effectiveProcessed) * (requested - effectiveProcessed)))
          : null

        setProgress(prev => ({
          ...prev,
          active: Boolean(provisioning?.active),
          phase: String(provisioning?.phase ?? 'idle'),
          total: requested,
          processed: Math.max(prev.processed, effectiveProcessed),
          running,
          created,
          simulated,
          stopped: Math.max(stopped, notCreated),
          failed,
          percent,
          elapsedSec: elapsed,
          etaSec,
        }))

        const recentLogs = Array.isArray(provisioning?.recent_logs) ? provisioning.recent_logs : []
        const t = new Date().toLocaleTimeString('zh-CN', { hour12: false })
        recentLogs.forEach((line: any) => {
          const text = String(line ?? '').trim()
          if (!text) return
          if (seenProgressLogsRef.current.has(text)) return
          seenProgressLogsRef.current.add(text)
          appendBootLog(`[${t}] ${text}`)
        })
      } catch {
        // ignore transient polling errors during deployment
      }
    }, 900)
  }

  const finishProgress = (runtimeResult: any, message: string) => {
    stopProgressPolling()
    const elapsed = Math.max(0, Math.floor((Date.now() - startedAtRef.current) / 1000))
    const totalNodes = Math.max(progress.total, toNumber(runtimeResult?.requested, progress.total))
    const running = Math.max(0, toNumber(runtimeResult?.runtime?.running, progress.running))
    const created = Math.max(0, toNumber(runtimeResult?.runtime?.created, progress.created))
    const simulated = Math.max(0, toNumber(runtimeResult?.runtime?.simulated, progress.simulated))
    const completed = Math.max(
      running + created + simulated,
      toNumber(runtimeResult?.created_or_started ?? runtimeResult?.started, progress.processed),
    )
    const failed = Math.max(0, toNumber(runtimeResult?.failed, progress.failed))
    const stopped = Math.max(0, totalNodes - completed - failed)
    setProgress({
      active: false,
      phase: 'idle',
      message,
      total: totalNodes,
      processed: Math.max(0, totalNodes - failed),
      running,
      created,
      simulated,
      stopped: Math.max(0, stopped),
      failed,
      percent: totalNodes > 0 ? Math.max(0, Math.min(1, (completed + failed) / totalNodes)) : 1,
      elapsedSec: elapsed,
      etaSec: 0,
    })
  }

  useEffect(() => () => stopProgressPolling(), [])

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
      // ignore
    }
  }

  const checkAndConfirmExistingRuntime = async (): Promise<{ proceed: boolean; forceReset: boolean }> => {
    try {
      const status = await apiClient.getRuntimeStatus()
      const runtime = status?.runtime ?? status ?? {}
      const totalNodes = toNumber(runtime.total_nodes, 0)
      if (totalNodes <= 0) return { proceed: true, forceReset: false }
      const ok = window.confirm(
        `检测到当前存在 ${totalNodes} 个卫星节点（包含运行/停止状态）。\n` +
        '重新生成/导入会停止并删除已有卫星 pod，同时清空数据库中的历史卫星状态后再重建。\n\n' +
        '是否继续？'
      )
      return ok ? { proceed: true, forceReset: true } : { proceed: false, forceReset: false }
    } catch {
      return { proceed: true, forceReset: false }
    }
  }

  const syncAndStartDynamic = async (
    sats: any[],
    links: any[],
    forceResetExisting: boolean,
    label: string,
  ) => {
    startProgressPolling(sats.length, label)
    const topologyData: any = prepareTopologyForBackend(sats, links, type, planes)
    topologyData.metadata = {
      ...(topologyData.metadata || {}),
      force_reset_existing: forceResetExisting,
      recreate_existing_pods: forceResetExisting,
    }
    const generateRes = await apiClient.generateTopology(topologyData)

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
    finishProgress(generateRes?.runtime, `${label}完成`)
    return generateRes
  }

  const doGenerate = async () => {
    setLoading(true)
    await clearExistingOrchestrationState()
    bumpTopologyVersion()
    setBackendTopologySynced(false)

    try {
      const confirmResult = await checkAndConfirmExistingRuntime()
      if (!confirmResult.proceed) {
        addToast('已取消重建操作，保持当前卫星节点与数据库状态不变', 'info')
        return
      }
      const forceReset = confirmResult.forceReset
      if (forceReset) {
        openSystemPopup('重建确认', '系统将执行：停止旧节点 + 删除旧容器 + 清空持久化 + 生成新星座。', 'warning')
      }

      const sats = generateConstellation(total, planes, type)
      const links = generateLinks(sats, type)
      setSatellites(sats as any)
      setLinks(links as any)
      setAutoDynamics({
        enabled: true,
        playing: true,
        elapsed_sec: 0,
      })

      const generateRes = await syncAndStartDynamic(sats as any[], links as any[], forceReset, '星座部署')
      setBackendTopologySynced(true)
      const failed = toNumber(generateRes?.runtime?.failed, 0)
      if (failed > 0) {
        addToast(`星座生成完成，但有 ${failed} 个节点启动失败`, 'warning')
      } else {
        addToast(`星座已生成：${sats.length} 节点 / ${links.length} 链路`, 'success')
      }
      if (generateRes?.reset?.requested) {
        addToast('已执行旧节点/数据库重置，再完成新星座部署', 'info')
      }
    } catch (err: any) {
      setBackendTopologySynced(false)
      stopProgressPolling()
      setProgress(prev => ({
        ...prev,
        active: false,
        phase: 'idle',
        message: '部署失败',
        etaSec: null,
      }))
      openSystemPopup('后端同步失败', err?.message ?? '生成失败，请检查后端日志。', 'error')
    } finally {
      setLoading(false)
    }
  }

  const onImportThirdParty = async (file?: File) => {
    if (!file) return
    setLoading(true)
    await clearExistingOrchestrationState()
    bumpTopologyVersion()
    setBackendTopologySynced(false)

    try {
      const confirmResult = await checkAndConfirmExistingRuntime()
      if (!confirmResult.proceed) {
        addToast('已取消导入操作，保持当前卫星节点与数据库状态不变', 'info')
        return
      }
      const forceReset = confirmResult.forceReset
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

      startProgressPolling(parsed.satellites.length, '导入拓扑部署')
      const topologyData: any = {
        metadata: {
          total_sats: parsed.satellites.length,
          num_planes: planes,
          altitude_km: parsed.satellites[0]?.orbital_params?.altitude_km ?? tpl.altitude_km,
          inclination_deg: parsed.satellites[0]?.orbital_params?.inclination ?? tpl.inclination_deg,
          template_id: type,
          timestamp: new Date().toISOString(),
          force_reset_existing: forceReset,
          recreate_existing_pods: forceReset,
        },
        nodes: parsed.satellites,
        links: parsed.links,
      }
      const generateRes = await apiClient.generateTopology(topologyData)
      finishProgress(generateRes?.runtime, '导入拓扑部署完成')

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
      addToast(
        `导入成功：${parsed.satellites.length} 颗卫星，${parsed.links.length} 条链路`,
        'success',
      )
      if (generateRes?.reset?.requested) {
        addToast('导入前已执行旧节点/数据库重置', 'info')
      }
    } catch (e: any) {
      setBackendTopologySynced(false)
      stopProgressPolling()
      setProgress(prev => ({
        ...prev,
        active: false,
        phase: 'idle',
        message: '导入失败',
        etaSec: null,
      }))
      openSystemPopup('导入失败', e?.message ?? '请提供合法 JSON 拓扑文件', 'error')
    } finally {
      setLoading(false)
    }
  }

  const progressPercent = useMemo(() => Math.round(progress.percent * 100), [progress.percent])

  return (
    <div className="constellation-panel flex h-full min-h-0 flex-col p-4">
      <div className="mb-3 flex items-center gap-2">
        <h3 className="text-base font-semibold text-[#0b1220]">卫星构型生成与导入</h3>
        <span className="bg-blue-50 px-2 py-0.5 text-xs text-blue-700">节点模板 + 第三方拓扑</span>
      </div>

      <div className="grid min-h-0 flex-1 grid-cols-1 gap-3 overflow-auto pr-1">
        <div className="space-y-3 border border-blue-100 bg-white p-3">
          <div>
            <label className="mb-1 block text-xs font-medium text-blue-700">星座模板</label>
            <select
              value={type}
              onChange={e => {
                const next = e.target.value as ConstellationType
                const def = CONSTELLATION_TEMPLATES[next]
                setType(next)
                setTotal(def.defaultSats)
                setPlanes(def.defaultPlanes)
              }}
              className="h-10 w-full border border-blue-200 bg-white px-3 text-sm text-[#0b1220] outline-none focus:border-blue-700"
            >
              {Object.entries(TYPES_ZH).map(([k, v]) => (
                <option key={k} value={k}>{v}</option>
              ))}
            </select>
          </div>

          <div className="grid grid-cols-2 gap-2 text-xs text-[#0b1220]">
            <div className="border border-blue-100 bg-white px-2 py-1.5">
              <div className="text-blue-700">轨道高度</div>
              <div className="font-semibold">{tpl.altitude_km} km</div>
            </div>
            <div className="border border-blue-100 bg-white px-2 py-1.5">
              <div className="text-blue-700">轨道倾角</div>
              <div className="font-semibold">{tpl.inclination_deg}°</div>
            </div>
            <div className="border border-blue-100 bg-white px-2 py-1.5">
              <div className="text-blue-700">CPU范围</div>
              <div className="font-semibold">{tpl.cpuPerSat[0]} ~ {tpl.cpuPerSat[1]}</div>
            </div>
            <div className="border border-blue-100 bg-white px-2 py-1.5">
              <div className="text-blue-700">内存范围</div>
              <div className="font-semibold">{tpl.memPerSat[0]} ~ {tpl.memPerSat[1]} GB</div>
            </div>
          </div>

          <div className="grid grid-cols-2 gap-2">
            <div>
              <label className="mb-1 block text-xs font-medium text-blue-700">卫星总数</label>
              <input
                type="number"
                value={total}
                min={24}
                max={6000}
                step={1}
                onChange={e => setTotal(Math.max(24, Math.min(6000, parseInt(e.target.value, 10) || 24)))}
                className="h-10 w-full border border-blue-200 bg-white px-3 text-sm text-[#0b1220] outline-none focus:border-blue-700"
              />
            </div>
            <div>
              <label className="mb-1 block text-xs font-medium text-blue-700">轨道面数</label>
              <input
                type="number"
                value={planes}
                min={1}
                max={72}
                step={1}
                onChange={e => setPlanes(Math.max(1, Math.min(72, parseInt(e.target.value, 10) || 1)))}
                className="h-10 w-full border border-blue-200 bg-white px-3 text-sm text-[#0b1220] outline-none focus:border-blue-700"
              />
            </div>
          </div>

          <div className="border border-blue-100 bg-blue-50 px-3 py-2 text-xs text-blue-800">
            规模预估：<span className="font-semibold">{total}</span> 颗卫星，
            每轨道面 <span className="font-semibold">{satsPerPlaneMin} ~ {satsPerPlaneMax}</span> 颗
          </div>

          <div className="flex flex-wrap gap-2">
            <button
              onClick={doGenerate}
              disabled={loading}
              className="inline-flex h-10 items-center gap-1.5 bg-[#08214a] px-3 text-sm font-medium text-white disabled:cursor-not-allowed disabled:opacity-60"
            >
              <RefreshCw className={`h-4 w-4 ${loading ? 'animate-spin' : ''}`} />
              {loading ? '处理中...' : '生成模拟星座'}
            </button>
            <button
              onClick={() => importInputRef.current?.click()}
              disabled={loading}
              className="inline-flex h-10 items-center gap-1.5 border border-blue-200 bg-white px-3 text-sm font-medium text-[#0b1220] disabled:cursor-not-allowed disabled:opacity-60"
            >
              <UploadCloud className="h-4 w-4" />
              导入第三方构型
            </button>
            <button
              type="button"
              onClick={() => setShowImportExample(v => !v)}
              className="inline-flex h-10 items-center gap-1.5 border border-blue-200 bg-white px-3 text-sm font-medium text-[#0b1220]"
            >
              <FileCode2 className="h-4 w-4" />
              JSON示例
            </button>
          </div>

          <input
            ref={importInputRef}
            type="file"
            accept=".json,application/json"
            className="hidden"
            onChange={e => onImportThirdParty(e.target.files?.[0])}
          />
        </div>

        <div className="space-y-3 border border-blue-100 bg-white p-3">
          <div className="flex items-center gap-2 text-sm font-medium text-[#0b1220]">
            <Orbit className="h-4 w-4 text-blue-700" />
            节点部署进度
          </div>
          <div className="border border-blue-100 bg-white p-3">
            <div className="mb-1 text-xs text-blue-700">{progress.message || '待开始'}</div>
            <div className="mono-progress-track h-2.5 w-full overflow-hidden rounded-full">
              <div
                className="mono-progress-fill h-full transition-all"
                style={{ width: `${progressPercent}%` }}
              />
            </div>
            <div className="mt-2 grid grid-cols-2 gap-2 text-xs text-[#0b1220]">
              <div>进度：<span className="font-semibold">{progressPercent}%</span></div>
              <div>总节点：<span className="font-semibold">{progress.total}</span></div>
              <div>已处理：<span className="font-semibold">{progress.processed}</span></div>
              <div>运行中：<span className="font-semibold">{progress.running}</span></div>
              <div>创建态：<span className="font-semibold">{progress.created}</span></div>
              <div>模拟态：<span className="font-semibold">{progress.simulated}</span></div>
              <div>失败：<span className="font-semibold">{progress.failed}</span></div>
              <div>停止：<span className="font-semibold">{progress.stopped}</span></div>
              <div>已耗时：<span className="font-semibold">{progress.elapsedSec}s</span></div>
            </div>
            <div className="mt-1 text-xs">
              预计剩余：{
                progress.etaSec == null
                  ? (progress.active ? '估算中（等待首批节点完成）' : '-')
                  : `${progress.etaSec}s`
              }
            </div>
          </div>

          <div className="border border-blue-100 bg-white p-3">
            <div className="mb-2 flex items-center gap-1 text-xs font-medium text-[#0b1220]">
              <AlertTriangle className="h-3.5 w-3.5 text-blue-700" />
              节点启动日志（逐节点）
            </div>
            <div className="max-h-44 overflow-auto border border-blue-100 bg-white p-2 font-mono text-[11px] text-[#0b1220]">
              {bootLogs.length === 0 && <div className="text-blue-500">暂无日志（部署时将实时显示）</div>}
              {bootLogs.map((line, idx) => (
                <div key={`${line}-${idx}`} className="py-0.5">{line}</div>
              ))}
            </div>
          </div>

          {showImportExample && (
            <div className="border border-blue-100 bg-white p-3">
              <div className="mb-1 text-xs font-medium text-[#0b1220]">第三方拓扑 JSON 示例</div>
              <pre className="max-h-52 overflow-auto whitespace-pre-wrap border border-blue-100 bg-white p-2 text-[11px] leading-5 text-[#0b1220]">
                {IMPORT_EXAMPLE_JSON}
              </pre>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
