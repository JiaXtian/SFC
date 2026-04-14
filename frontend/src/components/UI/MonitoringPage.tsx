import { useEffect, useMemo, useState } from 'react'
import {
  AlertTriangle,
  ArrowLeft,
  CheckCircle2,
  Server,
  ShieldCheck,
  Timer,
} from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'

function faultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    power_failure: '供电故障',
    cpu_overload: '计算过载',
    thermal_shutdown: '过热停机',
    control_plane_sync_loss: '控制面失步',
    software_crash: '软件崩溃',
    clock_drift: '时钟漂移',
    optical_signal_loss: '光信号丢失',
    beam_misalignment: '波束失准',
    interference_jamming: '链路干扰',
    routing_blackhole: '路由黑洞',
    transceiver_failure: '收发器故障',
    line_degradation: '链路退化',
    endpoint_node_fault: '端点节点故障',
    line_of_sight_loss: '视距中断',
    topology_inconsistent: '拓扑不一致',
    resource_or_link_fault: '资源/链路故障',
    other: '其他',
    unknown: '未知',
  }
  return map[tag] ?? tag
}

const DEFAULT_NODE_FAULT_TYPES = [
  'power_failure',
  'cpu_overload',
  'thermal_shutdown',
  'control_plane_sync_loss',
  'software_crash',
  'clock_drift',
]

function navigateTo(path: string) {
  if (window.location.pathname === path) return
  window.history.pushState({}, '', path)
  window.dispatchEvent(new PopStateEvent('popstate'))
}

function rescheduleReasonLabel(trigger: string): string {
  switch (trigger) {
    case 'source_node_down':
      return '源节点故障'
    case 'destination_node_down':
      return '宿节点故障'
    case 'deployment_node_down':
      return '部署节点故障'
    case 'anchor_path_disconnected':
      return '锚点路径断连'
    case 'topology_tick_bootstrap':
      return '初始策略构建'
    case 'session_start':
      return '会话启动'
    case 'manual_initial_candidate':
      return '手动初始方案'
    case 'unlabeled':
      return '未标注触发器'
    default:
      return trigger && trigger !== 'unknown' ? trigger : '未标注触发器'
  }
}

function shortToken(raw: string): string {
  const text = String(raw ?? '').trim()
  if (!text) return '0000'
  const parts = text.split(/[_-]/).filter(Boolean)
  const tail = parts.length > 0 ? parts[parts.length - 1] : text
  return tail.slice(-6)
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
    runtimeEvents,
    decisionTraces,
    deployments,
  } = useStore()
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

  const metrics = simulation.metrics
  const orch = simulation.orchestration
  const history = simulation.history.slice(-120)
  const orchestrationTraces = useMemo(
    () => decisionTraces.filter((t: any) => String(t?.mode ?? '') === 'session_continuous'),
    [decisionTraces]
  )

  const latencySeries = useMemo(
    () => history.map((h) => Number(h.metrics?.avg_latency_ms ?? 0)),
    [history]
  )
  const bwSeries = useMemo(
    () => history.map((h) => Number(h.metrics?.avg_bandwidth_utilization ?? 0) * 100),
    [history]
  )
  const faultInfraSeries = useMemo(
    () => history.map((h) => Number(h.metrics?.down_nodes ?? 0)),
    [history]
  )
  const inferenceSeries = useMemo(
    () => orchestrationTraces
      .slice(0, 40)
      .reverse()
      .map((t) => Number(t.inference_time_ms ?? 0))
      .filter((v) => Number.isFinite(v) && v > 0),
    [orchestrationTraces]
  )

  const faultEvents = useMemo(
    () => runtimeEvents.filter((e) => e.type === 'fault_event').slice(0, 20),
    [runtimeEvents]
  )
  const recoveryEvents = useMemo(
    () => runtimeEvents.filter((e) => e.type === 'recovery_event').slice(0, 20),
    [runtimeEvents]
  )
  const faultHeatmap = useMemo(() => {
    const fallbackRows = DEFAULT_NODE_FAULT_TYPES
    const rows: string[] = []
    const cols = 12
    const faults = runtimeEvents.filter((e) => e.type === 'fault_event')
    const typeCounts = new Map<string, number>()
    faults.forEach((e) => {
      const ft = String(e.raw?.fault_type ?? e.raw?.reason ?? 'unknown') || 'unknown'
      typeCounts.set(ft, (typeCounts.get(ft) ?? 0) + 1)
    })
    if (typeCounts.size > 0) {
      Array.from(typeCounts.entries())
        .sort((a, b) => b[1] - a[1])
        .slice(0, 6)
        .forEach(([ft]) => rows.push(ft))
    }
    fallbackRows.forEach((ft) => {
      if (rows.length >= 6) return
      if (!rows.includes(ft)) rows.push(ft)
    })
    if (rows.length === 0) rows.push('unknown')
    const includeOther = typeCounts.size > rows.length
    if (includeOther) rows.push('other')
    const matrix = rows.map(() => new Array(cols).fill(0))
    if (faults.length === 0) {
      return { rows: rows.map((r) => faultTypeLabel(r)), matrix }
    }
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
      const ft = String(e.raw?.fault_type ?? e.raw?.reason ?? 'unknown') || 'unknown'
      const r = rows.indexOf(ft)
      if (r >= 0) {
        matrix[r][c] += 1
      } else if (includeOther) {
        matrix[rows.length - 1][c] += 1
      }
    })
    return { rows: rows.map((r) => faultTypeLabel(r)), matrix }
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

  const decisionQuality = useMemo(() => {
    const traces = orchestrationTraces.slice(0, 240)
    const total = traces.length
    let deployableHits = 0
    let latencySum = 0
    let latencyCount = 0
    const byTrigger = new Map<string, { count: number; deployable: number; latencySum: number; latencyCount: number }>()
    traces.forEach((t: any) => {
      const rawTrigger = String(t?.trigger ?? '')
      const trigger = rawTrigger && rawTrigger !== 'unknown' ? rawTrigger : 'unlabeled'
      const deployable = Number(t?.deployable_count ?? 0) > 0
      const latency = Number(t?.inference_time_ms ?? 0)
      if (deployable) deployableHits += 1
      if (Number.isFinite(latency) && latency > 0) {
        latencySum += latency
        latencyCount += 1
      }
      const row = byTrigger.get(trigger) ?? { count: 0, deployable: 0, latencySum: 0, latencyCount: 0 }
      row.count += 1
      if (deployable) row.deployable += 1
      if (Number.isFinite(latency) && latency > 0) {
        row.latencySum += latency
        row.latencyCount += 1
      }
      byTrigger.set(trigger, row)
    })
    const rows = Array.from(byTrigger.entries())
      .map(([trigger, row]) => ({
        trigger,
        label: rescheduleReasonLabel(trigger),
        count: row.count,
        deployableRate: row.count > 0 ? (row.deployable / row.count) * 100 : 0,
        avgLatencyMs: row.latencyCount > 0 ? row.latencySum / row.latencyCount : 0,
      }))
      .sort((a, b) => b.count - a.count)
      .slice(0, 8)
    return {
      total,
      deployableRate: total > 0 ? (deployableHits / total) * 100 : 0,
      avgLatencyMs: latencyCount > 0 ? latencySum / latencyCount : 0,
      rows,
    }
  }, [orchestrationTraces])

  const decisionSuccessSeries = useMemo(() => {
    const seq = orchestrationTraces
      .slice(0, 80)
      .reverse()
      .map((t) => (Number(t.deployable_count ?? 0) > 0 ? 100 : 0))
    const window = 8
    const out: number[] = []
    for (let i = 0; i < seq.length; i++) {
      let sum = 0
      let count = 0
      for (let k = Math.max(0, i - window + 1); k <= i; k++) {
        sum += seq[k]
        count += 1
      }
      out.push(count > 0 ? sum / count : 0)
    }
    return out
  }, [orchestrationTraces])

  const inferredP95Latency = useMemo(() => {
    const direct = Number(orch?.latency_p95_ms ?? 0)
    if (direct > 0) return direct
    const values = orchestrationTraces
      .slice(0, 200)
      .map((t) => Number(t.inference_time_ms ?? 0))
      .filter((v) => Number.isFinite(v) && v > 0)
      .sort((a, b) => a - b)
    if (values.length === 0) return 0
    const idx = Math.min(values.length - 1, Math.floor((values.length - 1) * 0.95))
    return values[idx]
  }, [orch?.latency_p95_ms, orchestrationTraces])

  const recoveryRate = Number(orch?.recovery_success_rate ?? 0) * 100
  const resolveSfcLabel = (sessionId?: string, requestId?: string) => {
    const sid = String(sessionId ?? '')
    const rid = String(requestId ?? '')
    const bySession = sid
      ? deployments.find((d: any) => String(d?.session_id ?? '') === sid)
      : null
    if (bySession?.sfc_name) return String(bySession.sfc_name)
    if (bySession?.request_id) return `SFC-${shortToken(String(bySession.request_id))}`
    if (rid) return `SFC-${shortToken(rid)}`
    if (sid) return `SFC-${shortToken(sid)}`
    return 'SFC-未知'
  }
  const stabilityRows = useMemo(() => {
    const tracesBySession = new Map<string, any[]>()
    orchestrationTraces.forEach((trace: any) => {
      const sid = String(trace?.session_id ?? '')
      if (!sid) return
      if (!tracesBySession.has(sid)) tracesBySession.set(sid, [])
      tracesBySession.get(sid)!.push(trace)
    })

    return deployments
      .filter((dep: any) => !!dep?.session_id)
      .map((dep: any) => {
        const sessionId = String(dep.session_id)
        const traces = tracesBySession.get(sessionId) ?? []
        const rescheduleTraces = traces.filter((t: any) => {
          const trigger = String(t?.trigger ?? '')
          if (!trigger) return false
          return !['session_start', 'manual_initial_candidate', 'topology_tick_bootstrap'].includes(trigger)
        })
        const reasonCount = new Map<string, number>()
        rescheduleTraces.forEach((t: any) => {
          const trigger = String(t?.trigger ?? 'unknown')
          reasonCount.set(trigger, (reasonCount.get(trigger) ?? 0) + 1)
        })
        const sortedReasons = Array.from(reasonCount.entries()).sort((a, b) => b[1] - a[1])
        const latestTrigger = String(rescheduleTraces[0]?.trigger ?? dep.decision_trigger ?? '')
        const pathRecompute = Math.max(0, Number(dep.path_recompute_count ?? 0))
        const forcedReschedules = rescheduleTraces.length
        const failedPenalty = dep.status === 'failed' ? 18 : 0
        const stabilityScore = Math.max(0, 100 - Math.min(95, forcedReschedules * 9 + pathRecompute * 2 + failedPenalty))
        let stabilityLevel = '高稳定'
        if (stabilityScore < 70) stabilityLevel = '低稳定'
        else if (stabilityScore < 85) stabilityLevel = '中稳定'

        const reasonSummary = sortedReasons.length > 0
          ? sortedReasons
              .slice(0, 2)
              .map(([r, c]) => `${rescheduleReasonLabel(r)}(${c})`)
              .join('，')
          : '无必要重调度'

        return {
          sessionId,
          requestId: String(dep.request_id ?? '-'),
          sfcName: String(dep.sfc_name ?? `SFC ${dep.request_id ?? ''}`),
          stabilityScore,
          stabilityLevel,
          forcedReschedules,
          pathRecompute,
          latestReason: rescheduleReasonLabel(latestTrigger),
          reasonSummary,
          inferenceMs: Number(dep.inference_latency_ms ?? 0),
        }
      })
      .sort((a, b) => a.stabilityScore - b.stabilityScore)
  }, [deployments, orchestrationTraces])

  const maxRescheduleCount = useMemo(() => {
    if (stabilityRows.length === 0) return 1
    return Math.max(1, ...stabilityRows.map((r) => r.forcedReschedules))
  }, [stabilityRows])
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
            className="h-9 px-3 rounded-xl text-cyan-100 text-sm flex items-center gap-1.5 transition hover:brightness-110"
            style={{
              background: 'linear-gradient(135deg, rgba(22,52,78,0.52), rgba(11,26,44,0.48))',
              border: '1px solid rgba(114,172,215,0.34)',
              boxShadow: '0 8px 20px rgba(0,0,0,0.36), inset 0 1px 0 rgba(160,220,255,0.16)',
              backdropFilter: 'blur(10px)',
            }}
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
              <AxisLineChart values={faultInfraSeries} color="#f59e0b" title="故障节点数量趋势" yLabel="数量" xLabel="采样时序" />
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
                <div className="text-cyan-200 text-lg font-semibold">{inferredP95Latency.toFixed(1)}ms</div>
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
                  <div className="text-slate-200 mt-0.5">
                    {String(e.raw?.entity_type ?? '') === 'session'
                      ? `${e.type === 'fault_event' ? '故障事件' : '恢复事件'} ${resolveSfcLabel(String(e.raw?.entity_id ?? ''), String(e.raw?.request_id ?? ''))}`
                      : e.message}
                  </div>
                  <div className="text-[10px] text-slate-400 mt-0.5">
                    类型: {faultTypeLabel(String(e.raw?.fault_type ?? e.raw?.reason ?? 'unknown'))}
                  </div>
                </div>
              ))}
              {runtimeEvents.length === 0 && <div className="text-[11px] text-slate-500">暂无事件</div>}
            </div>
          </div>

          <div className="col-span-12 lg:col-span-6 rounded-2xl p-3"
            style={{ background: 'rgba(8,16,28,0.66)', border: '1px solid rgba(90,125,153,0.28)', backdropFilter: 'blur(10px)' }}>
            <div className="text-[12px] uppercase tracking-wide text-slate-300 font-semibold mb-2 flex items-center gap-1.5">
              <Timer className="w-4 h-4 text-emerald-300" />编排决策质量分布
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              说明：展示最近决策的可部署率、平均推理时延和触发来源分布，更直接反映编排器质量。
            </div>
            <div className="grid grid-cols-3 gap-2 text-[11px] mb-2">
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">样本数</div>
                <div className="text-emerald-300 text-lg font-semibold">{decisionQuality.total}</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">可部署率</div>
                <div className="text-emerald-300 text-lg font-semibold">{decisionQuality.deployableRate.toFixed(1)}%</div>
              </div>
              <div className="rounded-lg p-2 bg-slate-900/45 border border-slate-700/60">
                <div className="text-slate-400">平均推理时延</div>
                <div className="text-emerald-300 text-lg font-semibold">{decisionQuality.avgLatencyMs.toFixed(1)}ms</div>
              </div>
            </div>
            <div className="mt-2">
              <AxisLineChart values={decisionSuccessSeries} color="#34d399" title="最近决策可部署率趋势(滚动窗口)" yLabel="可部署率(%)" xLabel="决策序列" />
            </div>
            <div className="mt-2 max-h-40 overflow-y-auto rounded-lg border border-slate-700/60 bg-slate-900/35">
              <table className="w-full text-[11px]">
                <thead className="sticky top-0 bg-slate-900/85">
                  <tr className="text-slate-400">
                    <th className="text-left px-2 py-1 font-medium">触发来源</th>
                    <th className="text-right px-2 py-1 font-medium">次数</th>
                    <th className="text-right px-2 py-1 font-medium">可部署率</th>
                    <th className="text-right px-2 py-1 font-medium">均值时延</th>
                  </tr>
                </thead>
                <tbody>
                  {decisionQuality.rows.map((row) => (
                    <tr key={row.trigger} className="border-t border-slate-800/80">
                      <td className="px-2 py-1.5 text-cyan-200">{row.label}</td>
                      <td className="px-2 py-1.5 text-right text-slate-200">{row.count}</td>
                      <td className="px-2 py-1.5 text-right text-emerald-200">{row.deployableRate.toFixed(1)}%</td>
                      <td className="px-2 py-1.5 text-right text-violet-200">{row.avgLatencyMs.toFixed(1)}ms</td>
                    </tr>
                  ))}
                  {decisionQuality.rows.length === 0 && (
                    <tr>
                      <td className="px-2 py-2 text-slate-500" colSpan={4}>暂无决策数据</td>
                    </tr>
                  )}
                </tbody>
              </table>
            </div>
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
              <ShieldCheck className="w-4 h-4 text-cyan-300" />SFC稳定性与重调度原因
            </div>
            <div className="text-[10px] text-slate-500 mb-2">
              说明：仅统计“必要重调度”触发（节点故障/路径断连），用于评估各SFC在动态拓扑中的稳定运行能力。
            </div>
            <div className="overflow-x-auto">
              <table className="w-full min-w-[980px] text-[11px] border-separate border-spacing-y-1">
                <thead>
                  <tr className="text-slate-400">
                    <th className="text-left font-medium px-2 py-1">SFC</th>
                    <th className="text-left font-medium px-2 py-1">稳定性</th>
                    <th className="text-left font-medium px-2 py-1">必要重调度</th>
                    <th className="text-left font-medium px-2 py-1">路径重算</th>
                    <th className="text-left font-medium px-2 py-1">最近原因</th>
                    <th className="text-left font-medium px-2 py-1">原因统计</th>
                    <th className="text-left font-medium px-2 py-1">最近推理时延</th>
                  </tr>
                </thead>
                <tbody>
                  {stabilityRows.map((row) => (
                    <tr key={row.sessionId} className="bg-slate-900/45 border border-slate-700/60">
                      <td className="px-2 py-1.5 text-cyan-200">
                        <div className="font-medium">{row.sfcName}</div>
                        <div className="text-[10px] text-slate-500">{row.requestId}</div>
                      </td>
                      <td className="px-2 py-1.5">
                        <div className="text-slate-200">{row.stabilityLevel} ({row.stabilityScore.toFixed(0)})</div>
                        <div className="mt-1 h-1.5 rounded-full bg-slate-800/90 overflow-hidden">
                          <div
                            className="h-full rounded-full"
                            style={{
                              width: `${Math.max(4, Math.min(100, row.stabilityScore))}%`,
                              background: row.stabilityScore >= 85
                                ? 'linear-gradient(90deg, #34d399, #22d3ee)'
                                : row.stabilityScore >= 70
                                  ? 'linear-gradient(90deg, #f59e0b, #f97316)'
                                  : 'linear-gradient(90deg, #fb7185, #ef4444)',
                            }}
                          />
                        </div>
                      </td>
                      <td className="px-2 py-1.5 text-amber-200">
                        {row.forcedReschedules}
                        <div className="mt-1 h-1 rounded-full bg-slate-800/90 overflow-hidden">
                          <div
                            className="h-full rounded-full bg-amber-300/80"
                            style={{ width: `${Math.min(100, (row.forcedReschedules / maxRescheduleCount) * 100)}%` }}
                          />
                        </div>
                      </td>
                      <td className="px-2 py-1.5 text-violet-200">{row.pathRecompute}</td>
                      <td className="px-2 py-1.5 text-rose-200">{row.latestReason}</td>
                      <td className="px-2 py-1.5 text-slate-300">{row.reasonSummary}</td>
                      <td className="px-2 py-1.5 text-emerald-200">{row.inferenceMs.toFixed(1)} ms</td>
                    </tr>
                  ))}
                  {stabilityRows.length === 0 && (
                    <tr>
                      <td className="px-2 py-2 text-slate-500" colSpan={7}>暂无已部署SFC稳定性数据</td>
                    </tr>
                  )}
                </tbody>
              </table>
            </div>
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
              {orchestrationTraces.slice(0, 20).map((t, idx) => (
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
              {orchestrationTraces.length === 0 && <div className="text-[11px] text-slate-500">暂无决策轨迹</div>}
            </div>
          </div>
        </div>
      </div>
      </div>
    </div>
  )
}
