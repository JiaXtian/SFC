import { useEffect, useMemo, useState } from 'react'
import { Activity, AlertTriangle, Clock3, ListChecks, Plus, RotateCw, X, Trash2 } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'

type ActiveFault = {
  node_id: string
  fault_type: string
  injection_mode: string
  ttl_ticks: number
  remaining_sec: number
  fetched_at_ms: number
}

function faultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    power_failure: '供电故障',
    cpu_overload: '计算过载',
    thermal_shutdown: '过热停机',
    control_plane_sync_loss: '控制面失步',
    software_crash: '软件崩溃',
    clock_drift: '时钟漂移',
  }
  return map[tag] ?? tag
}

function splitIds(raw: string): string[] {
  return raw
    .split(/[\s,;，；\n\t]+/)
    .map((x) => x.trim())
    .filter(Boolean)
}

function readNodeFaults(status: any): ActiveFault[] {
  const now = Date.now()
  const arr = Array.isArray(status?.active_fault_details?.node) ? status.active_fault_details.node : []
  return arr.map((f: any) => ({
    node_id: String(f?.node_id ?? ''),
    fault_type: String(f?.fault_type ?? 'unknown'),
    injection_mode: String(f?.injection_mode ?? 'manual'),
    ttl_ticks: Math.max(0, Number(f?.ttl_ticks ?? 0)),
    remaining_sec: Math.max(0, Number(f?.remaining_sec ?? 0)),
    fetched_at_ms: now,
  }))
}

export default function FaultInjectionControl() {
  const {
    simulation,
    satellites,
    setSimulationStatus,
    addToast,
  } = useStore((s) => ({
    simulation: s.simulation,
    satellites: s.satellites,
    setSimulationStatus: s.setSimulationStatus,
    addToast: s.addToast,
  }))

  const [injectingFaults, setInjectingFaults] = useState(false)
  const [managingFaults, setManagingFaults] = useState(false)
  const [refreshing, setRefreshing] = useState(false)

  const [nodeFaultCatalog, setNodeFaultCatalog] = useState<string[]>([])
  const [activeFaults, setActiveFaults] = useState<ActiveFault[]>([])
  const [nowMs, setNowMs] = useState(Date.now())

  const [injectScope, setInjectScope] = useState<'single' | 'batch'>('single')
  const [injectMode, setInjectMode] = useState<'random' | 'manual'>('random')
  const [injectNodeId, setInjectNodeId] = useState('')
  const [injectBatchCount, setInjectBatchCount] = useState(5)
  const [injectFaultType, setInjectFaultType] = useState('auto')
  const [injectDurationSec, setInjectDurationSec] = useState(20)
  const [injectOnlyActive, setInjectOnlyActive] = useState(true)

  const [manualEntryInput, setManualEntryInput] = useState('')
  const [manualNodeIds, setManualNodeIds] = useState<string[]>([])

  const [extendSeconds, setExtendSeconds] = useState(15)

  const satIdSet = useMemo(() => {
    const out = new Set<string>()
    satellites.forEach((sat: any) => {
      const id = String(sat?.id ?? '').trim()
      if (id) out.add(id)
    })
    return out
  }, [satellites])

  const refreshStatus = async (silent = false) => {
    if (!silent) setRefreshing(true)
    try {
      const status = await apiClient.getDynamicStatus()
      setSimulationStatus({
        running: !!status?.running,
        sampling_interval_sec: Number(status?.sampling_interval_sec ?? simulation.sampling_interval_sec ?? 5),
        simulation_speed: Number(status?.simulation_speed ?? simulation.simulation_speed ?? 1),
      })

      const nodeFaults = Array.isArray(status?.fault_catalog?.node)
        ? status.fault_catalog.node.map((x: any) => String(x))
        : []
      setNodeFaultCatalog(nodeFaults)
      setActiveFaults(readNodeFaults(status))
    } catch {
      // ignore transient failures
    } finally {
      if (!silent) setRefreshing(false)
    }
  }

  useEffect(() => {
    refreshStatus(false)
    const ticker = window.setInterval(() => setNowMs(Date.now()), 1000)
    const poller = window.setInterval(() => refreshStatus(true), 4000)
    return () => {
      window.clearInterval(ticker)
      window.clearInterval(poller)
    }
  }, [])

  const addManualIds = (raw: string) => {
    const parsed = splitIds(raw)
    if (parsed.length === 0) return
    const valid: string[] = []
    const invalid: string[] = []
    parsed.forEach((id) => {
      if (satIdSet.has(id)) valid.push(id)
      else invalid.push(id)
    })
    if (invalid.length > 0) {
      addToast(`以下卫星ID不存在，已忽略: ${invalid.slice(0, 6).join(', ')}`, 'warning')
    }
    if (valid.length === 0) return
    setManualNodeIds((prev) => Array.from(new Set([...prev, ...valid])))
  }

  const secToTicks = (seconds: number) => {
    const samplingSec = Math.max(1, Number(simulation.sampling_interval_sec || 5))
    return Math.max(1, Math.ceil(seconds / samplingSec))
  }

  const injectSpecificFault = async () => {
    setInjectingFaults(true)
    try {
      const durationSec = Math.max(1, Math.min(3600, Number(injectDurationSec || 1)))
      const payload: any = {
        entity_type: 'node',
        action: 'inject',
        ttl_ticks: secToTicks(durationSec),
        fault_type: injectFaultType || 'auto',
        overwrite_existing: true,
      }

      if (injectScope === 'single') {
        const targetId = injectNodeId.trim()
        if (!targetId) throw new Error('请先输入目标卫星ID')
        if (!satIdSet.has(targetId)) throw new Error('目标卫星ID不存在于当前星座')
        payload.node_id = targetId
      } else if (injectMode === 'manual') {
        if (manualNodeIds.length === 0) throw new Error('请先添加批量卫星ID')
        payload.node_ids = manualNodeIds
      } else {
        payload.batch_count = Math.max(1, Math.min(1000, Number(injectBatchCount || 1)))
        payload.only_active = injectOnlyActive
      }

      const res = await apiClient.injectDynamicFaults(payload)
      const injected = Number(res?.injected ?? 0)
      const skipped = Number(res?.skipped_existing ?? 0)
      if (injected > 0) {
        addToast(`已注入 ${injected} 个节点故障`, 'success')
      } else if (skipped > 0) {
        addToast(`目标已存在故障，跳过 ${skipped} 个节点`, 'warning')
      } else {
        addToast('未注入故障（目标可能无效）', 'warning')
      }
      await refreshStatus(true)
    } catch (e: any) {
      addToast(`故障注入失败: ${e?.message ?? e}`, 'error')
    } finally {
      setInjectingFaults(false)
    }
  }

  const removeFault = async (nodeId: string) => {
    setManagingFaults(true)
    try {
      const res = await apiClient.injectDynamicFaults({
        entity_type: 'node',
        action: 'remove',
        node_id: nodeId,
      })
      const removed = Number(res?.removed ?? 0)
      addToast(removed > 0 ? `已移除 ${nodeId} 故障` : `节点 ${nodeId} 当前无故障`, removed > 0 ? 'success' : 'warning')
      await refreshStatus(true)
    } catch (e: any) {
      addToast(`移除故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const extendFault = async (nodeId: string) => {
    setManagingFaults(true)
    try {
      const deltaSeconds = Math.max(1, Math.min(3600, Number(extendSeconds || 1)))
      const res = await apiClient.injectDynamicFaults({
        entity_type: 'node',
        action: 'extend',
        node_id: nodeId,
        delta_seconds: deltaSeconds,
      })
      const extended = Number(res?.extended ?? 0)
      addToast(
        extended > 0 ? `已为 ${nodeId} 延长 ${deltaSeconds}s` : `节点 ${nodeId} 当前无故障`,
        extended > 0 ? 'success' : 'warning',
      )
      await refreshStatus(true)
    } catch (e: any) {
      addToast(`延长故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const removeAllFaults = async () => {
    const ids = activeFaults.map((f) => f.node_id).filter(Boolean)
    if (ids.length === 0) {
      addToast('当前没有可移除的手动故障', 'info')
      return
    }
    setManagingFaults(true)
    try {
      const res = await apiClient.injectDynamicFaults({
        entity_type: 'node',
        action: 'remove',
        node_ids: ids,
      })
      addToast(`已移除 ${Number(res?.removed ?? 0)} 个节点故障`, 'success')
      await refreshStatus(true)
    } catch (e: any) {
      addToast(`批量移除失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const liveRemaining = (fault: ActiveFault) => {
    const elapsed = Math.max(0, (nowMs - fault.fetched_at_ms) / 1000)
    return Math.max(0, fault.remaining_sec - elapsed)
  }

  const sortedFaults = useMemo(() => {
    return [...activeFaults].sort((a, b) => liveRemaining(a) - liveRemaining(b))
  }, [activeFaults, nowMs])

  return (
    <div className="flex h-full min-h-0 flex-col rounded-2xl bg-white p-3">
      <div className="mb-3 flex items-center gap-2 text-2xl font-semibold text-black">
        <Activity className="h-6 w-6" />故障控制中心
      </div>

      <div className="grid min-h-0 flex-1 grid-cols-1 gap-4 xl:grid-cols-12">
        <section className="xl:col-span-5 min-h-0 overflow-auto space-y-3 pr-1">
          <div className="text-lg font-semibold">手动注入</div>

          <div className="grid grid-cols-2 gap-2">
            <select
              value={injectScope}
              onChange={(e) => setInjectScope((e.target.value as 'single' | 'batch') || 'single')}
              className="h-10 rounded-xl border border-slate-200 bg-white px-3"
            >
              <option value="single">单颗卫星</option>
              <option value="batch">批量卫星</option>
            </select>
            <select
              value={injectFaultType}
              onChange={(e) => setInjectFaultType(e.target.value)}
              className="h-10 rounded-xl border border-slate-200 bg-white px-3"
            >
              <option value="auto">故障类型: 自动匹配</option>
              {nodeFaultCatalog.map((ft) => <option key={ft} value={ft}>{faultTypeLabel(ft)}</option>)}
            </select>
          </div>

          <label className="block space-y-1">
            <div className="inline-flex items-center gap-1 text-sm">
              <Clock3 className="h-4 w-4" />故障注入时间（s）
            </div>
            <input
              type="number"
              min={1}
              max={3600}
              step={1}
              value={injectDurationSec}
              onChange={(e) => setInjectDurationSec(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))}
              className="h-10 w-full rounded-xl border border-slate-200 bg-white px-3"
            />
          </label>

          {injectScope === 'single' ? (
            <input
              list="control-satellite-list"
              value={injectNodeId}
              onChange={(e) => setInjectNodeId(e.target.value)}
              placeholder="输入卫星ID（支持下拉选择）"
              className="h-10 w-full rounded-xl border border-slate-200 bg-white px-3 font-mono"
            />
          ) : (
            <div className="space-y-2">
              <select
                value={injectMode}
                onChange={(e) => setInjectMode((e.target.value as 'random' | 'manual') || 'random')}
                className="h-10 w-full rounded-xl border border-slate-200 bg-white px-3"
              >
                <option value="random">随机批量</option>
                <option value="manual">手动列表</option>
              </select>
              {injectMode === 'random' ? (
                <div className="grid grid-cols-2 gap-2">
                  <input
                    type="number"
                    min={1}
                    max={1000}
                    value={injectBatchCount}
                    onChange={(e) => setInjectBatchCount(Math.max(1, Math.min(1000, Number(e.target.value) || 1)))}
                    className="h-10 rounded-xl border border-slate-200 bg-white px-3"
                  />
                  <label className="inline-flex items-center gap-2 text-sm">
                    <input type="checkbox" checked={injectOnlyActive} onChange={(e) => setInjectOnlyActive(e.target.checked)} />
                    仅活跃节点
                  </label>
                </div>
              ) : (
                <div className="space-y-1.5">
                  <div className="flex gap-2">
                    <input
                      type="text"
                      list="control-satellite-list"
                      value={manualEntryInput}
                      onChange={(e) => setManualEntryInput(e.target.value)}
                      placeholder="输入卫星ID后添加，支持粘贴多个"
                      className="h-10 flex-1 rounded-xl border border-slate-200 bg-white px-3 font-mono"
                    />
                    <button
                      type="button"
                      onClick={() => {
                        addManualIds(manualEntryInput)
                        setManualEntryInput('')
                      }}
                      className="inline-flex h-10 items-center gap-1 rounded-xl border border-slate-200 bg-white px-3"
                    >
                      <Plus className="h-3.5 w-3.5" />添加
                    </button>
                  </div>
                  <div className="flex max-h-40 flex-wrap gap-1.5 overflow-y-auto rounded-xl border border-slate-200 bg-white p-2">
                    {manualNodeIds.length === 0 && <div className="text-xs">尚未添加卫星ID</div>}
                    {manualNodeIds.map((id) => (
                      <span key={id} className="inline-flex items-center gap-1 rounded-lg border border-slate-200 px-2 py-1 text-xs">
                        {id}
                        <button
                          type="button"
                          onClick={() => setManualNodeIds((prev) => prev.filter((x) => x !== id))}
                          className="text-slate-600"
                          aria-label={`remove-${id}`}
                        >
                          <X className="h-3 w-3" />
                        </button>
                      </span>
                    ))}
                  </div>
                </div>
              )}
            </div>
          )}

          <button
            disabled={injectingFaults}
            onClick={injectSpecificFault}
            className="h-10 w-full rounded-xl border border-slate-300 bg-white text-sm font-semibold disabled:opacity-60"
          >
            {injectingFaults ? '注入中...' : '注入故障'}
          </button>
        </section>

        <section className="xl:col-span-7 flex min-h-0 flex-col">
          <div className="mb-2 flex items-center justify-between">
            <div className="inline-flex items-center gap-1.5 text-lg font-semibold">
              <ListChecks className="h-4 w-4" />手动故障列表
            </div>
            <div className="flex items-center gap-1.5">
              <button
                type="button"
                onClick={() => refreshStatus(false)}
                disabled={refreshing}
                className="inline-flex h-9 items-center gap-1 rounded-xl border border-slate-200 bg-white px-2 text-sm disabled:opacity-60"
              >
                <RotateCw className={`h-3 w-3 ${refreshing ? 'animate-spin' : ''}`} />刷新
              </button>
              <button
                type="button"
                onClick={removeAllFaults}
                disabled={managingFaults || activeFaults.length === 0}
                className="h-9 rounded-xl border border-slate-200 bg-white px-3 text-sm disabled:opacity-60"
              >
                全部移除
              </button>
            </div>
          </div>

          <div className="mb-2 flex items-center gap-2 text-sm">
            <div>延长时长</div>
            <input
              type="number"
              min={1}
              max={3600}
              step={1}
              value={extendSeconds}
              onChange={(e) => setExtendSeconds(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))}
              className="h-9 w-24 rounded-xl border border-slate-200 bg-white px-2"
            />
            <div>s</div>
          </div>

          <div className="min-h-0 flex-1 overflow-y-auto space-y-1.5 rounded-xl border border-slate-200 bg-white p-2">
            {sortedFaults.length === 0 && (
              <div className="rounded-xl border border-slate-200 px-3 py-2 text-sm">
                当前无手动注入故障
              </div>
            )}
            {sortedFaults.map((f) => {
              const remain = liveRemaining(f)
              const remainLabel = remain > 0 ? `${remain.toFixed(0)}s` : '0s'
              const remainClass =
                remain <= 5 ? 'text-red-600' : remain <= 15 ? 'text-amber-600' : 'text-emerald-600'
              return (
                <div key={`${f.node_id}-${f.fault_type}`} className="rounded-xl border border-slate-200 bg-white px-3 py-2">
                  <div className="flex items-center justify-between gap-2">
                    <div className="font-mono text-sm">{f.node_id}</div>
                    <div className={`text-sm font-semibold ${remainClass}`}>{remainLabel}</div>
                  </div>
                  <div className="mt-0.5 text-xs">
                    {faultTypeLabel(f.fault_type)}
                    <span className="text-slate-600"> · {f.injection_mode === 'manual' ? '手动注入' : f.injection_mode}</span>
                  </div>
                  <div className="mt-1.5 flex items-center justify-end gap-2">
                    <button
                      type="button"
                      onClick={() => extendFault(f.node_id)}
                      disabled={managingFaults}
                      className="inline-flex h-8 items-center gap-1 rounded-xl border border-slate-200 bg-white px-2.5 text-xs disabled:opacity-60"
                      title={`延长 ${extendSeconds}s`}
                    >
                      <Plus className="h-3 w-3" />
                      <span className="font-mono">{extendSeconds}s</span>
                    </button>
                    <button
                      type="button"
                      onClick={() => removeFault(f.node_id)}
                      disabled={managingFaults}
                      className="inline-flex h-8 w-8 items-center justify-center rounded-xl border border-slate-200 bg-white disabled:opacity-60"
                      title="移除故障"
                    >
                      <Trash2 className="h-3.5 w-3.5" />
                    </button>
                  </div>
                </div>
              )
            })}
          </div>
        </section>
      </div>

      <datalist id="control-satellite-list">
        {satellites.slice(0, 8000).map((sat: any) => (
          <option key={String(sat?.id ?? '')} value={String(sat?.id ?? '')} />
        ))}
      </datalist>
    </div>
  )
}
