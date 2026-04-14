import { useEffect, useMemo, useState } from 'react'
import {
  Activity,
  AlertTriangle,
  CheckCircle2,
  Pause,
  Play,
  Radio,
  RefreshCcw,
  Rewind,
  StepForward,
} from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore, type DecisionTrace } from '@/store/useStore'

function traceKey(t: DecisionTrace) {
  return [
    t.mode ?? 'single_request',
    t.session_id ?? '',
    t.request_id ?? '',
    t.topology_version ?? 0,
    t.sim_time ?? '',
  ].join('|')
}

function pickSelectedAttempt(t?: DecisionTrace) {
  const steps = Array.isArray(t?.decision_process?.steps) ? t!.decision_process.steps : []
  return steps.find((s: any) => !!s?.satisfies_constraints) ?? steps[0] ?? null
}

function shortToken(raw: string): string {
  const text = String(raw ?? '').trim()
  if (!text) return '0000'
  const parts = text.split(/[_-]/).filter(Boolean)
  const tail = parts.length > 0 ? parts[parts.length - 1] : text
  return tail.slice(-6)
}

export default function DynamicPanel() {
  const {
    simulation,
    setSimulationStatus,
    setSimulationViewMode,
    setPlaybackCursor,
    decisionTraces,
    runtimeEvents,
    deployments,
    addToast,
  } = useStore()

  const [busy, setBusy] = useState(false)
  const [intervalSec, setIntervalSec] = useState(5)
  const [speed, setSpeed] = useState(1)
  const [selectedTraceKey, setSelectedTraceKey] = useState('')
  const [sessions, setSessions] = useState<any[]>([])
  const [sessionsBusy, setSessionsBusy] = useState(false)

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

  const loadSessions = async () => {
    setSessionsBusy(true)
    try {
      const list = await apiClient.listSFCSessions()
      setSessions(Array.isArray(list) ? list : [])
    } catch {
      setSessions([])
    } finally {
      setSessionsBusy(false)
    }
  }

  useEffect(() => {
    loadSessions()
    const timer = window.setInterval(loadSessions, 3000)
    return () => window.clearInterval(timer)
  }, [])

  useEffect(() => {
    if (decisionTraces.length === 0) return
    const headKey = traceKey(decisionTraces[0])
    setSelectedTraceKey((prev) => prev || headKey)
  }, [decisionTraces])

  const start = async () => {
    setBusy(true)
    try {
      const res = await apiClient.startDynamicSimulation({
        sampling_interval_sec: intervalSec,
        simulation_speed: speed,
      })
      setSimulationStatus({
        running: !!res?.status?.running,
        sampling_interval_sec: Number(res?.status?.sampling_interval_sec ?? intervalSec),
        simulation_speed: Number(res?.status?.simulation_speed ?? speed),
      })
      addToast('动态仿真已启动', 'success')
    } catch (e: any) {
      addToast(`启动失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const stop = async () => {
    setBusy(true)
    try {
      const res = await apiClient.stopDynamicSimulation()
      setSimulationStatus({ running: !!res?.status?.running })
      addToast('动态仿真已停止', 'info')
    } catch (e: any) {
      addToast(`停止失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const step = async () => {
    setBusy(true)
    try {
      const snapshot = await apiClient.stepDynamicSimulation()
      useStore.getState().applyTopologySnapshot(snapshot)
      addToast('已执行单步推进', 'success')
    } catch (e: any) {
      addToast(`单步推进失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const stopSession = async (sessionId: string) => {
    const dep = deployments.find((d: any) => String(d?.session_id ?? '') === String(sessionId))
    const sfcLabel = dep?.sfc_name ? String(dep.sfc_name) : `SFC-${shortToken(String(dep?.request_id ?? sessionId))}`
    try {
      await apiClient.stopSFCSession(sessionId)
      addToast(`${sfcLabel} 已停止`, 'success')
      loadSessions()
    } catch (e: any) {
      addToast(`停止会话失败: ${e?.message ?? e}`, 'error')
    }
  }

  const recomputeSession = async (sessionId: string) => {
    const dep = deployments.find((d: any) => String(d?.session_id ?? '') === String(sessionId))
    const sfcLabel = dep?.sfc_name ? String(dep.sfc_name) : `SFC-${shortToken(String(dep?.request_id ?? sessionId))}`
    try {
      await apiClient.recomputeSFCSession(sessionId, 'manual_panel')
      addToast(`${sfcLabel} 已触发重算`, 'success')
    } catch (e: any) {
      addToast(`会话重算失败: ${e?.message ?? e}`, 'error')
    }
  }

  const metrics = simulation.metrics
  const orch = simulation.orchestration
  const history = simulation.history
  const historyCursor = simulation.history_cursor
  const latestHistoryIdx = history.length - 1

  const selectedTrace = useMemo(() => {
    if (decisionTraces.length === 0) return null
    const found = decisionTraces.find((t) => traceKey(t) === selectedTraceKey)
    return found ?? decisionTraces[0]
  }, [decisionTraces, selectedTraceKey])
  const selectedAttempt = useMemo(() => pickSelectedAttempt(selectedTrace ?? undefined), [selectedTrace])
  const perVnfSteps = Array.isArray(selectedAttempt?.decision_process?.per_core_nf)
    ? selectedAttempt.decision_process.per_core_nf
    : (Array.isArray(selectedAttempt?.decision_process?.per_vnf)
      ? selectedAttempt.decision_process.per_vnf
      : [])

  return (
    <div className="flex-1 overflow-y-auto p-3 space-y-3 text-xs text-slate-200">
      <div
        className="rounded-xl p-3 space-y-2"
        style={{ background: 'rgba(13,24,38,0.8)', border: '1px solid rgba(99,130,158,0.25)' }}
      >
        <div className="flex items-center justify-between">
          <div className="font-semibold text-slate-100 uppercase tracking-wide">动态仿真控制</div>
          <div className="flex items-center gap-1 text-[10px]">
            <Radio className={`w-3 h-3 ${simulation.connected ? 'text-emerald-400' : 'text-slate-500'}`} />
            <span className={simulation.connected ? 'text-emerald-300' : 'text-slate-500'}>
              {simulation.connected ? 'WS在线' : 'WS离线'}
            </span>
          </div>
        </div>

        <div className="grid grid-cols-2 gap-2">
          <label className="space-y-1">
            <div className="text-[10px] text-slate-400">采样周期(s)</div>
            <input type="number" min={1} max={30} step={1} value={intervalSec}
              onChange={e => setIntervalSec(Math.max(1, Math.min(30, Number(e.target.value) || 5)))}
              className="w-full px-2 py-1 rounded bg-slate-900/70 border border-slate-700" />
          </label>
          <label className="space-y-1">
            <div className="text-[10px] text-slate-400">仿真倍速</div>
            <input type="number" min={0.1} max={20} step={0.1} value={speed}
              onChange={e => setSpeed(Math.max(0.1, Math.min(20, Number(e.target.value) || 1)))}
              className="w-full px-2 py-1 rounded bg-slate-900/70 border border-slate-700" />
          </label>
        </div>

        <div className="flex items-center gap-2">
          <button
            disabled={busy}
            onClick={start}
            className="px-3 py-1.5 rounded bg-emerald-700/80 hover:bg-emerald-600 flex items-center gap-1 disabled:opacity-50"
          >
            <Play className="w-3 h-3" />启动
          </button>
          <button
            disabled={busy}
            onClick={stop}
            className="px-3 py-1.5 rounded bg-amber-700/80 hover:bg-amber-600 flex items-center gap-1 disabled:opacity-50"
          >
            <Pause className="w-3 h-3" />停止
          </button>
          <button
            disabled={busy}
            onClick={step}
            className="px-3 py-1.5 rounded bg-sky-700/80 hover:bg-sky-600 flex items-center gap-1 disabled:opacity-50"
          >
            <StepForward className="w-3 h-3" />单步
          </button>
        </div>

        <div className="mt-2 rounded-lg p-2 border border-slate-700/70 bg-slate-950/35 space-y-2">
          <div className="text-[11px] font-semibold text-slate-200">动态拓扑显示模式</div>
          <div className="flex items-center gap-2">
            <button
              onClick={() => setSimulationViewMode('realtime')}
              className={`px-2.5 py-1 rounded text-[11px] ${simulation.view_mode === 'realtime' ? 'bg-cyan-700/70 text-cyan-100' : 'bg-slate-800 text-slate-300'}`}
            >
              实时模式
            </button>
            <button
              onClick={() => setSimulationViewMode('playback')}
              className={`px-2.5 py-1 rounded text-[11px] ${simulation.view_mode === 'playback' ? 'bg-indigo-700/70 text-indigo-100' : 'bg-slate-800 text-slate-300'}`}
            >
              回放模式
            </button>
            <button
              onClick={() => setPlaybackCursor(Math.max(0, historyCursor - 1))}
              disabled={history.length === 0}
              className="px-2 py-1 rounded bg-slate-800 text-slate-300 disabled:opacity-50"
            >
              <Rewind className="w-3 h-3" />
            </button>
            <button
              onClick={() => setPlaybackCursor(latestHistoryIdx)}
              disabled={history.length === 0}
              className="px-2 py-1 rounded bg-slate-800 text-slate-300 disabled:opacity-50"
            >
              <RefreshCcw className="w-3 h-3" />
            </button>
          </div>
          <div className="text-[10px] text-slate-400">
            历史帧 {history.length} · 当前游标 {historyCursor >= 0 ? historyCursor + 1 : 0}/{history.length || 0}
          </div>
          <input
            type="range"
            min={0}
            max={Math.max(0, latestHistoryIdx)}
            value={historyCursor < 0 ? 0 : historyCursor}
            disabled={history.length === 0}
            onChange={(e) => setPlaybackCursor(Number(e.target.value))}
            className="w-full"
          />
        </div>
      </div>

      <div
        className="rounded-xl p-3 space-y-1"
        style={{ background: 'rgba(13,24,38,0.8)', border: '1px solid rgba(99,130,158,0.25)' }}
      >
        <div className="font-semibold uppercase tracking-wide text-slate-100 flex items-center gap-1">
          <Activity className="w-3.5 h-3.5 text-cyan-300" />全局运行指标
        </div>
        <div>运行状态: <span className={simulation.running ? 'text-emerald-300' : 'text-slate-400'}>{simulation.running ? '运行中' : '已停止'}</span></div>
        <div>显示模式: <span className={simulation.view_mode === 'realtime' ? 'text-cyan-300' : 'text-indigo-300'}>{simulation.view_mode === 'realtime' ? '实时' : '回放'}</span></div>
        <div>仿真时间: <span className="text-slate-300">{simulation.sim_time || '-'}</span></div>
        <div>拓扑版本: <span className="text-cyan-300 font-mono">{simulation.topology_version}</span></div>
        {metrics && (
          <>
            <div>节点: {metrics.active_nodes}/{metrics.total_nodes} 活跃</div>
            <div>链路: {metrics.active_links}/{metrics.total_links} 活跃，拥塞 {metrics.congested_links}</div>
            <div>平均链路时延: {metrics.avg_latency_ms.toFixed(2)} ms</div>
            <div>平均带宽利用率: {(metrics.avg_bandwidth_utilization * 100).toFixed(1)}%</div>
          </>
        )}
        {orch && (
          <>
            <div className="mt-2 pt-2 border-t border-slate-700/60 text-slate-300">编排引擎指标</div>
            <div>活跃会话: {orch.active_sessions}</div>
            <div>本tick决策: {orch.decisions_this_tick}，重算重部署: {orch.redeploys_this_tick}</div>
            <div>累计决策: {orch.total_decisions}，失败: {orch.total_failures}</div>
            <div>算法时延: mean {orch.latency_mean_ms.toFixed(1)}ms · P95 {orch.latency_p95_ms.toFixed(1)}ms · P99 {orch.latency_p99_ms.toFixed(1)}ms</div>
          </>
        )}
      </div>

      <div
        className="rounded-xl p-3 space-y-2"
        style={{ background: 'rgba(13,24,38,0.8)', border: '1px solid rgba(99,130,158,0.25)' }}
      >
        <div className="font-semibold uppercase tracking-wide text-slate-100">在线编排会话</div>
        <div className="max-h-36 overflow-y-auto space-y-1">
          {sessions.map((s: any) => (
            <div key={s.session_id} className="rounded px-2 py-1 bg-slate-900/60 border border-slate-800 text-[11px]">
              <div className="text-slate-200">
                {(() => {
                  const dep = deployments.find((d: any) => String(d?.session_id ?? '') === String(s.session_id))
                  if (dep?.sfc_name) return String(dep.sfc_name)
                  return `SFC-${shortToken(String(dep?.request_id ?? s.session_id ?? ''))}`
                })()}
              </div>
              <div className="text-slate-400">v{s.last_topology_version} · {s.last_inference_time_ms?.toFixed?.(1) ?? s.last_inference_time_ms}ms · 决策{ s.decisions_total }</div>
              <div className="flex gap-2 mt-1">
                <button onClick={() => recomputeSession(s.session_id)} className="px-2 py-0.5 rounded bg-sky-700/70 text-sky-100">重算</button>
                <button onClick={() => stopSession(s.session_id)} className="px-2 py-0.5 rounded bg-rose-700/70 text-rose-100">停止</button>
              </div>
            </div>
          ))}
          {sessions.length === 0 && <div className="text-slate-500 text-[11px]">暂无会话</div>}
        </div>
        <button
          onClick={loadSessions}
          disabled={sessionsBusy}
          className="px-2.5 py-1 rounded bg-slate-800 text-slate-200 text-[11px] disabled:opacity-50"
        >
          {sessionsBusy ? '刷新中...' : '刷新会话'}
        </button>
      </div>

      <div
        className="rounded-xl p-3"
        style={{ background: 'rgba(13,24,38,0.8)', border: '1px solid rgba(99,130,158,0.25)' }}
      >
        <div className="font-semibold uppercase tracking-wide text-slate-100 mb-2 flex items-center gap-1">
          <AlertTriangle className="w-3.5 h-3.5 text-orange-300" />事件流
        </div>
        <div className="space-y-1 max-h-40 overflow-y-auto text-[11px]">
          {runtimeEvents.slice(0, 20).map((evt) => (
            <div key={evt.id} className="rounded px-2 py-1 bg-slate-900/60 border border-slate-800">
              <div className="text-slate-300">{evt.message}</div>
              <div className="text-slate-500 text-[10px]">{evt.sim_time || '-'}</div>
            </div>
          ))}
          {runtimeEvents.length === 0 && <div className="text-slate-500">暂无事件</div>}
        </div>
      </div>

      <div
        className="rounded-xl p-3 space-y-2"
        style={{ background: 'rgba(13,24,38,0.8)', border: '1px solid rgba(99,130,158,0.25)' }}
      >
        <div className="font-semibold uppercase tracking-wide text-slate-100">策略过程可视化</div>
        <div className="grid grid-cols-2 gap-2">
          <div className="space-y-1 max-h-52 overflow-y-auto">
            {decisionTraces.slice(0, 20).map((t) => {
              const k = traceKey(t)
              const active = k === traceKey(selectedTrace ?? t) && (selectedTraceKey ? k === selectedTraceKey : true)
              return (
                <button
                  key={k}
                  onClick={() => setSelectedTraceKey(k)}
                  className={`w-full text-left rounded px-2 py-1 border ${active ? 'border-cyan-400 bg-cyan-900/20' : 'border-slate-800 bg-slate-900/60'}`}
                >
                  <div className="text-cyan-200 text-[11px]">{t.request_id}</div>
                  <div className="text-slate-400 text-[10px]">
                    {t.mode ?? 'single'} · topo_v{t.topology_version} · {t.inference_time_ms?.toFixed?.(1) ?? t.inference_time_ms}ms
                  </div>
                </button>
              )
            })}
            {decisionTraces.length === 0 && <div className="text-slate-500 text-[11px]">暂无策略轨迹</div>}
          </div>

          <div className="space-y-2 max-h-52 overflow-y-auto">
            {!selectedTrace && <div className="text-slate-500 text-[11px]">请选择一条策略轨迹</div>}
            {selectedTrace && (
              <>
                <div className="rounded p-2 bg-slate-900/60 border border-slate-800 text-[10px]">
                  <div>输入: {selectedTrace.source_node} → {selectedTrace.destination_node}</div>
                  <div>约束结果: 返回候选 {selectedTrace.returned_topk}，可部署 {selectedTrace.deployable_count}</div>
                  <div>推理时延: {selectedTrace.inference_time_ms?.toFixed?.(2) ?? selectedTrace.inference_time_ms}ms</div>
                  <div>触发: {selectedTrace.trigger ?? '-'}</div>
                </div>

                <div className="rounded p-2 bg-slate-900/60 border border-slate-800 text-[10px] space-y-1">
                  <div className="text-slate-300 font-semibold">候选评分</div>
                  {selectedTrace.candidates?.slice(0, 5).map((c, idx) => (
                    <div key={idx} className="flex items-center justify-between">
                      <span className="text-slate-300">#{idx + 1} {c.satisfies_constraints ? '可部署' : '回退'}</span>
                      <span className="text-cyan-300">score {Number(c.score ?? 0).toFixed(3)}</span>
                    </div>
                  ))}
                </div>

                <div className="rounded p-2 bg-slate-900/60 border border-slate-800 text-[10px] space-y-1">
                  <div className="text-slate-300 font-semibold">决策过程</div>
                  <div>尝试轮次: {Array.isArray(selectedTrace?.decision_process?.steps) ? selectedTrace.decision_process.steps.length : 0}</div>
                  {selectedAttempt && (
                    <>
                      <div className="text-emerald-300 inline-flex items-center gap-1">
                        <CheckCircle2 className="w-3 h-3" />
                        选中尝试 #{selectedAttempt.attempt_id ?? '-'} · relax {selectedAttempt.relax_factor ?? '-'}
                      </div>
                      <div>最终得分: {Number(selectedAttempt.score ?? 0).toFixed(3)} · 时延 {Number(selectedAttempt.latency_ms ?? 0).toFixed(2)}ms</div>
                    </>
                  )}
                  {perVnfSteps.slice(0, 8).map((p: any, i: number) => (
                    <div key={i} className="rounded px-2 py-1 border border-slate-800 bg-slate-950/40">
                      核心网网元{i + 1} {p.core_nf ?? p.vnf_name ?? '-'}: {p.prev_node ?? '-'} → {p.selected_node ?? '-'}
                    </div>
                  ))}
                </div>
              </>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}
