import { useEffect, useMemo, useState } from 'react'
import {
  Activity,
  AlertTriangle,
  ArrowLeft,
  CheckCircle2,
  Server,
  ShieldCheck,
  Timer,
} from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'

function navigateTo(path: string) {
  if (window.location.pathname === path) return
  window.history.pushState({}, '', path)
  window.dispatchEvent(new PopStateEvent('popstate'))
}

function AxisLineChart({
  values,
  color,
  title,
  xLabel = '时间',
  yLabel = '值',
  height = 156,
}: {
  values: number[]
  color: string
  title: string
  xLabel?: string
  yLabel?: string
  height?: number
}) {
  const width = 420
  const left = 44
  const right = 12
  const top = 16
  const bottom = 34
  const plotW = width - left - right
  const plotH = height - top - bottom
  if (!values.length) {
    return <div className="text-[10px] text-slate-500 text-center py-2">暂无数据</div>
  }
  const min = Math.min(...values)
  const max = Math.max(...values)
  const range = Math.max(1e-9, max - min)
  const points = values
    .map((v, i) => {
      const x = left + (i / Math.max(1, values.length - 1)) * plotW
      const y = top + (1 - (v - min) / range) * plotH
      return `${x.toFixed(1)},${y.toFixed(1)}`
    })
    .join(' ')
  const yTicks = [0, 0.5, 1].map((p) => {
    const y = top + (1 - p) * plotH
    const val = (min + p * range).toFixed(range >= 10 ? 0 : 2)
    return { y, val }
  })
  const xTickLeft = values.length > 1 ? '早' : '当前'
  const xTickRight = '新'

  return (
    <div className="w-full flex flex-col items-center">
      <div className="text-[10px] text-slate-400 mb-1">{title}</div>
      <svg width="100%" viewBox={`0 0 ${width} ${height}`} className="max-w-[460px] overflow-visible">
        <line x1={left} y1={top} x2={left} y2={top + plotH} stroke="rgba(148,163,184,0.55)" strokeWidth="1" />
        <line x1={left} y1={top + plotH} x2={left + plotW} y2={top + plotH} stroke="rgba(148,163,184,0.55)" strokeWidth="1" />
        {yTicks.map((t, idx) => (
          <g key={`y-${idx}`}>
            <line x1={left} y1={t.y} x2={left + plotW} y2={t.y} stroke="rgba(71,85,105,0.35)" strokeWidth="1" strokeDasharray="3 3" />
            <text x={left - 6} y={t.y + 3} textAnchor="end" fontSize="10" fill="#94a3b8">{t.val}</text>
          </g>
        ))}
        <text x={left} y={top + plotH + 18} fontSize="10" fill="#94a3b8">{xTickLeft}</text>
        <text x={left + plotW} y={top + plotH + 18} textAnchor="end" fontSize="10" fill="#94a3b8">{xTickRight}</text>
        <text x={left + plotW / 2} y={height - 4} textAnchor="middle" fontSize="10" fill="#94a3b8">{xLabel}</text>
        <text
          x={12}
          y={top + plotH / 2}
          textAnchor="middle"
          transform={`rotate(-90 12 ${top + plotH / 2})`}
          fontSize="10"
          fill="#94a3b8"
        >
          {yLabel}
        </text>
        <polyline
          points={points}
          fill="none"
          stroke={color}
          strokeWidth="2.6"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
      </svg>
    </div>
  )
}

function AxisHistogram({
  values,
  bins = 10,
  width = 420,
  height = 168,
  color = '#34d399',
  title = '分布',
  xLabel = '区间',
  yLabel = '频次',
}: {
  values: number[]
  bins?: number
  width?: number
  height?: number
  color?: string
  title?: string
  xLabel?: string
  yLabel?: string
}) {
  const left = 44
  const right = 12
  const top = 16
  const bottom = 34
  const plotW = width - left - right
  const plotH = height - top - bottom
  if (!values.length) {
    return <div className="text-[10px] text-slate-500 text-center py-2">暂无数据</div>
  }
  const min = Math.min(...values)
  const max = Math.max(...values)
  const range = Math.max(1e-9, max - min)
  const counts = new Array(Math.max(3, bins)).fill(0)
  values.forEach((v) => {
    const ratio = (v - min) / range
    const idx = Math.min(counts.length - 1, Math.max(0, Math.floor(ratio * counts.length)))
    counts[idx] += 1
  })
  const maxCount = Math.max(1, ...counts)
  const gap = 2
  const barW = (plotW - gap * (counts.length - 1)) / counts.length
  return (
    <div className="w-full flex flex-col items-center">
      <div className="text-[10px] text-slate-400 mb-1">{title}</div>
      <svg width="100%" viewBox={`0 0 ${width} ${height}`} className="max-w-[460px] overflow-visible">
        <line x1={left} y1={top} x2={left} y2={top + plotH} stroke="rgba(148,163,184,0.55)" strokeWidth="1" />
        <line x1={left} y1={top + plotH} x2={left + plotW} y2={top + plotH} stroke="rgba(148,163,184,0.55)" strokeWidth="1" />
        {[0, 0.5, 1].map((p, idx) => {
          const y = top + (1 - p) * plotH
          const val = Math.round(p * maxCount)
          return (
            <g key={`h-y-${idx}`}>
              <line x1={left} y1={y} x2={left + plotW} y2={y} stroke="rgba(71,85,105,0.35)" strokeWidth="1" strokeDasharray="3 3" />
              <text x={left - 6} y={y + 3} textAnchor="end" fontSize="10" fill="#94a3b8">{val}</text>
            </g>
          )
        })}
        {counts.map((count, idx) => {
          const h = (count / maxCount) * plotH
          const x = left + idx * (barW + gap)
          const y = top + plotH - h
          return (
            <rect
              key={`bar-${idx}`}
              x={x}
              y={y}
              width={Math.max(1, barW)}
              height={Math.max(1, h)}
              rx={1.5}
              fill={color}
              opacity={0.82}
            />
          )
        })}
        <text x={left} y={top + plotH + 18} fontSize="10" fill="#94a3b8">{min.toFixed(1)}</text>
        <text x={left + plotW} y={top + plotH + 18} textAnchor="end" fontSize="10" fill="#94a3b8">{max.toFixed(1)}</text>
        <text x={left + plotW / 2} y={height - 4} textAnchor="middle" fontSize="10" fill="#94a3b8">{xLabel}</text>
        <text
          x={12}
          y={top + plotH / 2}
          textAnchor="middle"
          transform={`rotate(-90 12 ${top + plotH / 2})`}
          fontSize="10"
          fill="#94a3b8"
        >
          {yLabel}
        </text>
      </svg>
    </div>
  )
}

function Heatmap({
  matrix,
  rowLabels,
  width = 420,
  height = 150,
  title = '热力图',
}: {
  matrix: number[][]
  rowLabels: string[]
  width?: number
  height?: number
  title?: string
}) {
  if (!matrix.length || !matrix[0]?.length) {
    return <div className="text-[10px] text-slate-500 text-center py-2">暂无数据</div>
  }
  const rows = matrix.length
  const cols = matrix[0].length
  const leftPad = 52
  const topPad = 20
  const bottomPad = 24
  const cellW = (width - leftPad - 6) / cols
  const cellH = (height - topPad - bottomPad) / rows
  const maxVal = Math.max(1, ...matrix.flat())
  return (
    <div className="w-full flex flex-col items-center">
      <div className="text-[10px] text-slate-400 mb-1">{title}</div>
      <svg width="100%" viewBox={`0 0 ${width} ${height}`} className="max-w-[460px]">
        {rowLabels.map((label, r) => (
          <text key={`lb-${label}`} x={4} y={topPad + r * cellH + cellH * 0.68} fontSize="9" fill="#94a3b8">
            {label}
          </text>
        ))}
        {matrix.map((row, r) =>
          row.map((v, c) => {
            const t = v / maxVal
            const alpha = 0.12 + t * 0.78
            const x = leftPad + c * cellW
            const y = topPad + r * cellH
            return (
              <rect
                key={`hm-${r}-${c}`}
                x={x}
                y={y}
                width={Math.max(1, cellW - 1)}
                height={Math.max(1, cellH - 1)}
                rx={1.5}
                fill={`rgba(56,189,248,${alpha.toFixed(3)})`}
              />
            )
          })
        )}
        <text x={leftPad + (cols * cellW) / 2} y={height - 5} textAnchor="middle" fontSize="10" fill="#94a3b8">
          时间窗口（由旧到新）
        </text>
      </svg>
    </div>
  )
}

export default function MonitoringPage() {
  const {
    simulation,
    setSimulationStatus,
    addToast,
    runtimeEvents,
    decisionTraces,
  } = useStore()
  const [updatingFaults, setUpdatingFaults] = useState(false)
  const [enableFaults, setEnableFaults] = useState(true)
  const [nodeFaultProb, setNodeFaultProb] = useState(0.0002)
  const [linkFaultProb, setLinkFaultProb] = useState(0.0005)
  const [viewport, setViewport] = useState(() => ({
    w: window.innerWidth,
    h: window.innerHeight,
  }))

  useEffect(() => {
    let mounted = true
    apiClient.getDynamicStatus()
      .then((res) => {
        if (!mounted) return
        setSimulationStatus({
          running: !!res.running,
          sampling_interval_sec: Number(res.sampling_interval_sec ?? 5),
          simulation_speed: Number(res.simulation_speed ?? 1),
          topology_version: Number(res.topology_version ?? 0),
          sim_time: String(res.sim_time ?? ''),
          metrics: res.metrics ?? null,
        })
      })
      .catch(() => {})
    return () => { mounted = false }
  }, [setSimulationStatus])

  useEffect(() => {
    const onResize = () => {
      setViewport({
        w: window.innerWidth,
        h: window.innerHeight,
      })
    }
    window.addEventListener('resize', onResize)
    return () => window.removeEventListener('resize', onResize)
  }, [])

  const applyFaultConfig = async () => {
    setUpdatingFaults(true)
    try {
      const samplingSec = Math.max(1, Number(simulation.sampling_interval_sec || 5))
      const speed = Math.max(0.1, Number(simulation.simulation_speed || 1))
      const res = await apiClient.startDynamicSimulation({
        sampling_interval_sec: samplingSec,
        simulation_speed: speed,
        enable_faults: enableFaults,
        node_fault_prob_per_tick: nodeFaultProb,
        link_fault_prob_per_tick: linkFaultProb,
      })
      setSimulationStatus({
        running: !!res?.status?.running,
        sampling_interval_sec: Number(res?.status?.sampling_interval_sec ?? samplingSec),
        simulation_speed: Number(res?.status?.simulation_speed ?? speed),
      })
      addToast('故障注入参数已更新', 'success')
    } catch (e: any) {
      addToast(`故障参数更新失败: ${e?.message ?? e}`, 'error')
    } finally {
      setUpdatingFaults(false)
    }
  }

  const metrics = simulation.metrics
  const orch = simulation.orchestration
  const history = simulation.history.slice(-120)

  const latencySeries = useMemo(
    () => history.map((h) => Number(h.metrics?.avg_latency_ms ?? 0)),
    [history]
  )
  const bwSeries = useMemo(
    () => history.map((h) => Number(h.metrics?.avg_bandwidth_utilization ?? 0) * 100),
    [history]
  )
  const downLinkSeries = useMemo(
    () => history.map((h) => Number(h.metrics?.down_links ?? 0)),
    [history]
  )
  const inferenceSeries = useMemo(
    () => decisionTraces.slice(0, 40).reverse().map((t) => Number(t.inference_time_ms ?? 0)),
    [decisionTraces]
  )

  const faultEvents = useMemo(
    () => runtimeEvents.filter((e) => e.type === 'fault_event').slice(0, 20),
    [runtimeEvents]
  )
  const recoveryEvents = useMemo(
    () => runtimeEvents.filter((e) => e.type === 'recovery_event').slice(0, 20),
    [runtimeEvents]
  )
  const recoveryLatencySec = useMemo(() => {
    const ordered = [...runtimeEvents]
      .filter((e) => e.type === 'fault_event' || e.type === 'recovery_event')
      .reverse()
    const faultAt = new Map<string, number>()
    const latencies: number[] = []
    ordered.forEach((e) => {
      const entityType = String(e.raw?.entity_type ?? 'entity')
      const entityId = String(e.raw?.entity_id ?? '')
      if (!entityId) return
      const key = `${entityType}|${entityId}`
      const ts = Date.parse(String(e.sim_time ?? ''))
      if (!Number.isFinite(ts)) return
      if (e.type === 'fault_event') {
        faultAt.set(key, ts)
        return
      }
      const prev = faultAt.get(key)
      if (!prev || ts < prev) return
      const sec = (ts - prev) / 1000
      if (Number.isFinite(sec) && sec >= 0) latencies.push(sec)
      faultAt.delete(key)
    })
    return latencies.slice(-160)
  }, [runtimeEvents])

  const faultHeatmap = useMemo(() => {
    const rows = ['node', 'link', 'session', 'other']
    const cols = 12
    const matrix = rows.map(() => new Array(cols).fill(0))
    const faults = runtimeEvents.filter((e) => e.type === 'fault_event')
    if (faults.length === 0) return { rows, matrix }
    const times = faults
      .map((e) => Date.parse(String(e.sim_time ?? '')))
      .filter((t) => Number.isFinite(t))
    const latest = times.length > 0 ? Math.max(...times) : Date.now()
    const bucketMs = 5 * 60 * 1000
    const start = latest - cols * bucketMs
    faults.forEach((e) => {
      const ts = Date.parse(String(e.sim_time ?? ''))
      if (!Number.isFinite(ts) || ts < start) return
      const c = Math.min(cols - 1, Math.max(0, Math.floor((ts - start) / bucketMs)))
      const entityType = String(e.raw?.entity_type ?? 'other')
      const r = rows.indexOf(entityType)
      matrix[r >= 0 ? r : 3][c] += 1
    })
    return { rows, matrix }
  }, [runtimeEvents])

  const sessionImpactSeries = useMemo(() => {
    const ordered = [...runtimeEvents].reverse()
    const vals: number[] = []
    ordered.forEach((e) => {
      if (e.type !== 'recovery_event') return
      const impact = Number(e.raw?.affected_services ?? (e.raw?.entity_type === 'session' ? 1 : 0))
      vals.push(Math.max(0, impact))
    })
    return vals.slice(-80)
  }, [runtimeEvents])

  const recoveryRate = Number(orch?.recovery_success_rate ?? 0) * 100
  const monitorScale = useMemo(() => {
    const widthScale = viewport.w / 1680
    const heightScale = viewport.h / 950
    const scale = Math.min(widthScale, heightScale)
    return Math.max(0.92, Math.min(1.34, scale))
  }, [viewport])

  return (
    <div
      className="fixed inset-0 z-[120] overflow-y-auto overflow-x-hidden"
      style={{
        background:
          'radial-gradient(1200px 520px at 50% 110%, rgba(24,72,115,0.24) 0%, rgba(3,8,16,0.86) 42%, #010206 76%, #000000 100%)',
        fontFamily: '"IBM Plex Sans", "Noto Sans SC", sans-serif',
      }}
    >
      <div
        className="mx-auto"
        style={{
          transform: `scale(${monitorScale})`,
          transformOrigin: 'top center',
          width: `${100 / monitorScale}%`,
          minHeight: `${100 / monitorScale}%`,
        }}
      >
      <div className="mx-auto px-4 py-3 pb-8" style={{ width: 'min(96vw, 1860px)' }}>
        <div className="flex items-center gap-2.5 mb-3">
          <button
            onClick={() => navigateTo('/')}
            className="h-9 px-3 rounded-lg text-slate-200 text-sm flex items-center gap-1.5"
            style={{ background: 'rgba(14,22,36,0.62)', border: '1px solid rgba(86,112,136,0.32)' }}
          >
            <ArrowLeft className="w-4 h-4" />
            返回主页面
          </button>
          <div className="text-xl font-semibold text-slate-100">系统监控中心</div>
          <div className="ml-auto text-xs text-slate-400">
            topo_v{simulation.topology_version} · {simulation.sim_time || '-'}
          </div>
        </div>

        <div className="grid grid-cols-12 gap-2.5">
          <div
            className="col-span-12 rounded-xl px-3 py-2.5"
            style={{
              background: 'rgba(8,16,28,0.72)',
              border: '1px solid rgba(90,125,153,0.28)',
              backdropFilter: 'blur(10px)',
              fontFamily: '"Space Grotesk", "Noto Sans SC", sans-serif',
            }}
          >
            <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-[1.2fr_1fr_1fr_auto] items-end gap-2">
              <div className="rounded-lg px-2.5 py-2 bg-slate-900/35 border border-slate-700/55">
                <div className="text-[11px] uppercase tracking-wide text-slate-300 font-semibold flex items-center gap-1.5">
                  <Activity className="w-4 h-4 text-cyan-300" />
                  故障注入参数
                </div>
                <div className="text-[10px] text-slate-500 mt-0.5">
                  概率按每个拓扑更新周期生效，建议保持低概率扰动。
                </div>
              </div>
              <label className="text-[10px] text-slate-400 rounded-lg px-2 py-1.5 bg-slate-900/40 border border-slate-700/60">
                节点故障概率/周期
                <input
                  type="number"
                  min={0}
                  max={0.05}
                  step={0.0001}
                  value={nodeFaultProb}
                  onChange={(e) => setNodeFaultProb(Math.max(0, Math.min(0.05, Number(e.target.value) || 0)))}
                  className="w-full mt-1 h-7 px-2 rounded bg-slate-900/65 border border-slate-700/70 text-amber-200 font-mono"
                />
              </label>
              <label className="text-[10px] text-slate-400 rounded-lg px-2 py-1.5 bg-slate-900/40 border border-slate-700/60">
                链路故障概率/周期
                <input
                  type="number"
                  min={0}
                  max={0.1}
                  step={0.0001}
                  value={linkFaultProb}
                  onChange={(e) => setLinkFaultProb(Math.max(0, Math.min(0.1, Number(e.target.value) || 0)))}
                  className="w-full mt-1 h-7 px-2 rounded bg-slate-900/65 border border-slate-700/70 text-orange-200 font-mono"
                />
              </label>
              <div className="flex flex-col gap-1 items-end rounded-lg px-2.5 py-2 bg-slate-900/35 border border-slate-700/55">
                <label className="inline-flex items-center gap-1.5 text-[10px] text-slate-300">
                  <input type="checkbox" checked={enableFaults} onChange={(e) => setEnableFaults(e.target.checked)} />
                  启用故障注入
                </label>
                <button
                  disabled={updatingFaults}
                  onClick={applyFaultConfig}
                  className="px-3 py-1.5 rounded-md text-xs text-cyan-100 bg-cyan-700/70"
                >
                  {updatingFaults ? '更新中...' : '应用参数'}
                </button>
                <div className="text-[9px] text-slate-500 text-right">
                  周期 {simulation.sampling_interval_sec}s · 倍速 {simulation.simulation_speed.toFixed(1)}x
                </div>
              </div>
            </div>
          </div>

          <div className="col-span-12 lg:col-span-6 rounded-2xl p-3"
            style={{
              background: 'rgba(8,16,28,0.66)',
              border: '1px solid rgba(90,125,153,0.28)',
              backdropFilter: 'blur(10px)',
              fontFamily: '"Space Grotesk", "Noto Sans SC", sans-serif',
            }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <Server className="w-4 h-4 text-cyan-300" />拓扑资源健康
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              说明：用于观察全局节点/链路可用性及资源负载走势，判断是否接近容量瓶颈。
            </div>
            <div className="grid grid-cols-2 md:grid-cols-5 gap-2 text-[11px] mb-2">
              <div className="rounded-lg px-2 py-1.5 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">节点活跃</div>
                <div className="text-cyan-200 font-semibold">{metrics?.active_nodes ?? 0}/{metrics?.total_nodes ?? 0}</div>
              </div>
              <div className="rounded-lg px-2 py-1.5 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">链路活跃</div>
                <div className="text-cyan-200 font-semibold">{metrics?.active_links ?? 0}/{metrics?.total_links ?? 0}</div>
              </div>
              <div className="rounded-lg px-2 py-1.5 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">拥塞链路</div>
                <div className="text-amber-200 font-semibold">{metrics?.congested_links ?? 0}</div>
              </div>
              <div className="rounded-lg px-2 py-1.5 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">平均时延</div>
                <div className="text-emerald-200 font-semibold">{(metrics?.avg_latency_ms ?? 0).toFixed(2)} ms</div>
              </div>
              <div className="rounded-lg px-2 py-1.5 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">平均带宽利用率</div>
                <div className="text-violet-200 font-semibold">{((metrics?.avg_bandwidth_utilization ?? 0) * 100).toFixed(1)}%</div>
              </div>
            </div>
            <div className="mt-2">
              <AxisLineChart values={latencySeries} color="#38bdf8" title="链路平均时延趋势" yLabel="毫秒(ms)" xLabel="采样时序" />
            </div>
            <div className="mt-1">
              <AxisLineChart values={bwSeries} color="#34d399" title="平均带宽利用率趋势" yLabel="利用率(%)" xLabel="采样时序" />
            </div>
          </div>

          <div className="col-span-12 lg:col-span-6 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <ShieldCheck className="w-4 h-4 text-emerald-300" />容错恢复指标
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              说明：展示故障触发后的恢复质量，包括本周期效果与累计恢复结果。
            </div>
            <div className="grid grid-cols-2 gap-2 text-[11px]">
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">恢复成功率</div>
                <div className="text-emerald-300 text-lg font-semibold">{recoveryRate.toFixed(1)}%</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">本周期恢复</div>
                <div className="text-cyan-200 text-lg font-semibold">{orch?.recovery_success_this_tick ?? 0}/{orch?.recovery_attempts_this_tick ?? 0}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">累计恢复成功</div>
                <div className="text-emerald-300 text-lg font-semibold">{orch?.total_recovery_success ?? 0}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">累计恢复失败</div>
                <div className="text-rose-300 text-lg font-semibold">{orch?.total_recovery_failures ?? 0}</div>
              </div>
            </div>
            <div className="mt-2">
              <AxisLineChart values={downLinkSeries} color="#f59e0b" title="故障链路数量趋势" yLabel="链路数" xLabel="采样时序" />
            </div>
          </div>

          <div className="col-span-12 lg:col-span-7 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold flex items-center gap-1.5">
              <Timer className="w-4 h-4 text-violet-300" />编排性能指标
            </div>
            <div className="text-[10px] text-slate-500 mt-1">
              说明：用于评估编排器在动态拓扑下的吞吐、重部署频率和推理时延稳定性。
            </div>
            <div className="grid grid-cols-2 gap-2 mt-2 text-[11px]">
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">活跃SFC</div>
                <div className="text-cyan-200 text-lg font-semibold">{orch?.active_sessions ?? 0}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">本周期决策 / 重部署</div>
                <div className="text-cyan-200 text-lg font-semibold">{orch?.decisions_this_tick ?? 0} / {orch?.redeploys_this_tick ?? 0}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">P95 算法时延</div>
                <div className="text-cyan-200 text-lg font-semibold">{(orch?.latency_p95_ms ?? 0).toFixed(1)}ms</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">累计决策 / 失败</div>
                <div className="text-cyan-200 text-lg font-semibold">{orch?.total_decisions ?? 0} / {orch?.total_failures ?? 0}</div>
              </div>
            </div>
            <div className="mt-2">
              <AxisLineChart values={inferenceSeries} color="#a78bfa" title="最近策略推理时延趋势" yLabel="时延(ms)" xLabel="决策序列" />
            </div>
          </div>

          <div className="col-span-12 lg:col-span-5 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <AlertTriangle className="w-4 h-4 text-amber-300" />故障-恢复事件流
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              说明：按时间展示故障与恢复事件，可用于定位影响范围与恢复链路。
            </div>
            <div className="grid grid-cols-2 gap-2 mb-2 text-[11px]">
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">最近故障事件</div>
                <div className="text-amber-200 text-lg font-semibold">{faultEvents.length}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">最近恢复事件</div>
                <div className="text-emerald-200 text-lg font-semibold">{recoveryEvents.length}</div>
              </div>
            </div>
            <div className="max-h-60 overflow-y-auto space-y-1 pr-1">
              {[...runtimeEvents.filter((e) => e.type === 'fault_event' || e.type === 'recovery_event').slice(0, 30)].map((e) => (
                <div
                  key={e.id}
                  className="rounded-lg p-2 border text-[11px]"
                  style={{
                    background: e.type === 'fault_event' ? 'rgba(113,42,27,0.3)' : 'rgba(16,84,67,0.28)',
                    borderColor: e.type === 'fault_event' ? 'rgba(251,146,60,0.35)' : 'rgba(74,222,128,0.3)',
                  }}
                >
                  <div className="flex items-center justify-between">
                    <span className={e.type === 'fault_event' ? 'text-amber-200' : 'text-emerald-200'}>
                      {e.type === 'fault_event' ? '故障' : '恢复'}
                    </span>
                    <span className="text-slate-400">{e.sim_time || '-'}</span>
                  </div>
                  <div className="text-slate-200 mt-0.5">{e.message}</div>
                </div>
              ))}
              {runtimeEvents.length === 0 && <div className="text-[11px] text-slate-500">暂无事件</div>}
            </div>
          </div>

          <div className="col-span-12 lg:col-span-6 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <Timer className="w-4 h-4 text-emerald-300" />恢复时延分布
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              说明：统计“故障事件到恢复事件”的耗时分布，越集中且越靠左表示恢复越快。
            </div>
            <div className="grid grid-cols-3 gap-2 text-[11px] mb-2">
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">样本数</div>
                <div className="text-emerald-300 text-lg font-semibold">{recoveryLatencySec.length}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">平均恢复时延</div>
                <div className="text-emerald-300 text-lg font-semibold">
                  {recoveryLatencySec.length > 0
                    ? `${(recoveryLatencySec.reduce((a, b) => a + b, 0) / recoveryLatencySec.length).toFixed(1)}s`
                    : '-'}
                </div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">最大恢复时延</div>
                <div className="text-emerald-300 text-lg font-semibold">
                  {recoveryLatencySec.length > 0 ? `${Math.max(...recoveryLatencySec).toFixed(1)}s` : '-'}
                </div>
              </div>
            </div>
            <AxisHistogram
              values={recoveryLatencySec}
              bins={12}
              color="#34d399"
              title="恢复时延直方分布"
              xLabel="恢复时延区间(s)"
              yLabel="样本数"
            />
          </div>

          <div className="col-span-12 lg:col-span-6 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <AlertTriangle className="w-4 h-4 text-sky-300" />故障热力与会话影响
            </div>
            <div className="text-[10px] text-slate-500 mb-1">
              说明：热力图反映时间窗口内不同故障类型密度；曲线展示恢复事件造成的业务影响量。
            </div>
            <div className="text-[10px] text-slate-400">最近 60 分钟故障热力（5分钟粒度）</div>
            <Heatmap matrix={faultHeatmap.matrix} rowLabels={faultHeatmap.rows} title="故障类型时序热力图" />
            <div className="mt-1 text-[10px] text-slate-400">恢复事件会话影响趋势（受影响业务数）</div>
            <AxisLineChart values={sessionImpactSeries} color="#22d3ee" title="恢复事件会话影响趋势" yLabel="受影响业务数" xLabel="事件序列" />
          </div>

          <div className="col-span-12 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <CheckCircle2 className="w-4 h-4 text-cyan-300" />策略决策轨迹（最近20条）
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              轨迹表说明：`deployable=a/b` 表示返回候选中满足硬约束的方案数/总候选数。
            </div>
            <div className="max-h-56 overflow-y-auto space-y-1">
              {decisionTraces.slice(0, 20).map((t, idx) => (
                <div key={`${t.request_id}-${t.topology_version}-${idx}`}
                  className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60 text-[11px]">
                  <div className="flex items-center justify-between">
                    <span className="text-cyan-200 font-mono">{t.request_id}</span>
                    <span className="text-slate-400">{t.sim_time}</span>
                  </div>
                  <div className="text-slate-300 mt-0.5">
                    topo_v{t.topology_version} · 推理{Number(t.inference_time_ms ?? 0).toFixed(1)}ms ·
                    deployable={t.deployable_count}/{t.returned_topk}
                  </div>
                </div>
              ))}
              {decisionTraces.length === 0 && <div className="text-[11px] text-slate-500">暂无决策轨迹</div>}
            </div>
          </div>
        </div>
      </div>
      </div>
    </div>
  )
}
