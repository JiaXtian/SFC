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
    autoDynamics,
    satellites,
    setSimulationStatus,
    setAutoDynamics,
    addToast,
  } = useStore((s) => ({
    simulation: s.simulation,
    autoDynamics: s.autoDynamics,
    satellites: s.satellites,
    setSimulationStatus: s.setSimulationStatus,
    setAutoDynamics: s.setAutoDynamics,
    addToast: s.addToast,
  }))

  const [injectingFaults, setInjectingFaults] = useState(false)
  const [managingFaults, setManagingFaults] = useState(false)
  const [refreshing, setRefreshing] = useState(false)
  const [savingControl, setSavingControl] = useState(false)
  const [resourceSamplingSec, setResourceSamplingSec] = useState<number>(Math.max(10, Math.min(30, Number(autoDynamics.resource_update_sec || 15))))
  const [simulationSpeed, setSimulationSpeed] = useState<number>(Math.max(0.1, Number(autoDynamics.time_scale || 1)))

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
        sampling_interval_sec: Number(status?.sampling_interval_sec ?? simulation.sampling_interval_sec ?? 15),
        simulation_speed: Number(status?.simulation_speed ?? simulation.simulation_speed ?? 1),
      })
      setResourceSamplingSec(Math.max(10, Math.min(30, Number(status?.control_config?.resource_sampling_interval_sec ?? status?.sampling_interval_sec ?? resourceSamplingSec))))
      setSimulationSpeed(Math.max(0.1, Math.min(20, Number(status?.control_config?.simulation_speed ?? status?.simulation_speed ?? simulationSpeed))))

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

  const syncTopologyAfterFaultMutation = async () => {
    try {
      const topo = await apiClient.getTopology()
      useStore.getState().applyTopologySnapshot(topo)
      window.dispatchEvent(new Event('satellite-table-refresh'))
    } catch {
      // ignore transient sync failures
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

  const applyControlConfig = async () => {
    setSavingControl(true)
    try {
      const sampling = Math.max(10, Math.min(30, Number(resourceSamplingSec || 15)))
      const speed = Math.max(0.1, Math.min(20, Number(simulationSpeed || 1)))
      await apiClient.updateControlConfig({
        resource_sampling_interval_sec: sampling,
        simulation_speed: speed,
        apply_now: true,
      })
      setAutoDynamics({
        resource_update_sec: sampling,
        time_scale: speed,
      })
      setSimulationStatus({
        sampling_interval_sec: sampling,
        simulation_speed: speed,
      })
      addToast('资源感知采样配置已更新', 'success')
      await refreshStatus(true)
    } catch (e: any) {
      addToast(`采样配置更新失败: ${e?.message ?? e}`, 'error')
    } finally {
      setSavingControl(false)
    }
  }

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
    const samplingSec = Math.max(10, Number(simulation.sampling_interval_sec || 15))
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
      await syncTopologyAfterFaultMutation()
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
      await syncTopologyAfterFaultMutation()
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
      await syncTopologyAfterFaultMutation()
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
      await syncTopologyAfterFaultMutation()
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
    <div
      className="rounded-2xl p-3.5 h-full flex flex-col"
      style={{
        background: 'linear-gradient(160deg, rgba(11,17,30,0.78), rgba(8,13,24,0.66))',
        border: '1px solid rgba(112,168,208,0.28)',
        backdropFilter: 'blur(14px)',
      }}
    >
      <div className="text-[14px] uppercase tracking-wide text-cyan-100 font-semibold mb-1.5 flex items-center gap-1.5">
        <Activity className="w-4 h-4 text-cyan-300" />故障注入控制
      </div>

      <div className="flex-1 overflow-y-auto pr-1.5 space-y-3 text-[12px]">
        <section className="rounded-xl p-2.5 border border-slate-700/60 bg-slate-900/25 space-y-2.5">
          <div className="text-slate-200 inline-flex items-center gap-1.5">
            <ListChecks className="w-3.5 h-3.5 text-cyan-300" />资源感知采样配置
          </div>
          <div className="text-[11px] text-slate-400">
            该配置决定系统读取数据库中卫星资源状态的频率，同时用于动态链路/节点状态写库节奏。建议 3~10 秒。
          </div>
          <div className="grid grid-cols-2 gap-2">
            <label className="space-y-1">
              <div className="text-slate-400">采样间隔（秒）</div>
              <input
                type="number"
                min={10}
                max={30}
                step={1}
                value={resourceSamplingSec}
                onChange={(e) => setResourceSamplingSec(Math.max(10, Math.min(30, Number(e.target.value) || 15)))}
                className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-cyan-200"
              />
            </label>
            <label className="space-y-1">
              <div className="text-slate-400">仿真速率（x）</div>
              <input
                type="number"
                min={0.1}
                max={20}
                step={0.1}
                value={simulationSpeed}
                onChange={(e) => setSimulationSpeed(Math.max(0.1, Math.min(20, Number(e.target.value) || 0.1)))}
                className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-cyan-200"
              />
            </label>
          </div>
          <button
            onClick={applyControlConfig}
            disabled={savingControl}
            className="w-full h-9 rounded-lg text-cyan-100 bg-cyan-500/15 border border-cyan-500/35 disabled:opacity-60"
          >
            {savingControl ? '保存中...' : '保存并应用采样配置'}
          </button>
        </section>

        <section className="rounded-xl p-2.5 border border-slate-700/60 bg-slate-900/25 space-y-2.5">
          <div className="text-slate-200 inline-flex items-center gap-1.5">
            <AlertTriangle className="w-3.5 h-3.5 text-amber-300" />手动注入
          </div>

          <div className="grid grid-cols-2 gap-2">
            <select
              value={injectScope}
              onChange={(e) => setInjectScope((e.target.value as 'single' | 'batch') || 'single')}
              className="h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-slate-200"
            >
              <option value="single">单颗卫星</option>
              <option value="batch">批量卫星</option>
            </select>
            <select
              value={injectFaultType}
              onChange={(e) => setInjectFaultType(e.target.value)}
              className="h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-slate-200"
            >
              <option value="auto">故障类型: 自动匹配</option>
              {nodeFaultCatalog.map((ft) => <option key={ft} value={ft}>{faultTypeLabel(ft)}</option>)}
            </select>
          </div>

          <label className="block space-y-1">
            <div className="text-slate-400 inline-flex items-center gap-1">
              <Clock3 className="w-3.5 h-3.5 text-cyan-300" />故障注入时间（s）
            </div>
            <input
              type="number"
              min={1}
              max={3600}
              step={1}
              value={injectDurationSec}
              onChange={(e) => setInjectDurationSec(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))}
              placeholder="例如: 20（表示故障持续20秒）"
              className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-orange-200"
            />
            <div className="text-[11px] text-slate-500">系统将自动按仿真采样周期换算并执行该持续时间。</div>
          </label>

          {injectScope === 'single' ? (
            <div className="space-y-1.5">
              <input
                list="control-satellite-list"
                value={injectNodeId}
                onChange={(e) => setInjectNodeId(e.target.value)}
                placeholder="输入卫星ID（支持下拉选择）"
                className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-cyan-200 font-mono"
              />
              <div className="text-[11px] text-slate-500">支持输入、删除和重新编辑。</div>
            </div>
          ) : (
            <div className="space-y-2">
              <select
                value={injectMode}
                onChange={(e) => setInjectMode((e.target.value as 'random' | 'manual') || 'random')}
                className="w-full h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-slate-200"
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
                    className="h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-amber-200"
                  />
                  <label className="inline-flex items-center gap-2 text-slate-300">
                    <input type="checkbox" checked={injectOnlyActive} onChange={(e) => setInjectOnlyActive(e.target.checked)} className="accent-cyan-400" />
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
                      className="flex-1 h-9 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-cyan-200 font-mono"
                    />
                    <button
                      type="button"
                      onClick={() => {
                        addManualIds(manualEntryInput)
                        setManualEntryInput('')
                      }}
                      className="h-9 px-3 rounded-lg text-cyan-100 bg-slate-800/70 border border-cyan-700/40 inline-flex items-center gap-1"
                    >
                      <Plus className="w-3.5 h-3.5" />添加
                    </button>
                  </div>
                  <div className="text-[11px] text-slate-500">可一次粘贴多个ID（逗号/空格/换行分隔）。</div>
                  <div className="max-h-24 overflow-y-auto rounded-lg border border-slate-700/65 bg-slate-900/35 p-1.5 flex flex-wrap gap-1.5">
                    {manualNodeIds.length === 0 && <div className="text-[11px] text-slate-500">尚未添加卫星ID</div>}
                    {manualNodeIds.map((id) => (
                      <span key={id} className="inline-flex items-center gap-1 px-2 py-1 rounded-md text-[11px] text-cyan-100 bg-slate-800/70 border border-slate-600/60">
                        {id}
                        <button
                          type="button"
                          onClick={() => setManualNodeIds((prev) => prev.filter((x) => x !== id))}
                          className="text-slate-400 hover:text-rose-300"
                          aria-label={`remove-${id}`}
                        >
                          <X className="w-3 h-3" />
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
            className="w-full h-9 rounded-lg text-rose-100 text-[13px] font-semibold disabled:opacity-60"
            style={{ background: 'linear-gradient(135deg, rgba(185,28,28,0.84), rgba(127,29,29,0.82))', border: '1px solid rgba(251,146,60,0.45)' }}
          >
            {injectingFaults ? '注入中...' : '注入故障'}
          </button>
        </section>

        <section className="rounded-xl p-2.5 border border-slate-700/60 bg-slate-900/25 space-y-2">
          <div className="flex items-center justify-between">
            <div className="text-slate-200 inline-flex items-center gap-1.5">
              <ListChecks className="w-3.5 h-3.5 text-cyan-300" />当前手动故障管理
            </div>
            <div className="flex items-center gap-1.5">
              <button
                type="button"
                onClick={() => refreshStatus(false)}
                disabled={refreshing}
                className="h-7 px-2 rounded-md text-[11px] text-cyan-100 bg-slate-800/70 border border-slate-700/70 inline-flex items-center gap-1 disabled:opacity-60"
              >
                <RotateCw className={`w-3 h-3 ${refreshing ? 'animate-spin' : ''}`} />刷新
              </button>
              <button
                type="button"
                onClick={removeAllFaults}
                disabled={managingFaults || activeFaults.length === 0}
                className="h-7 px-2 rounded-md text-[11px] text-rose-100 bg-rose-900/40 border border-rose-800/70 disabled:opacity-60"
              >
                全部移除
              </button>
            </div>
          </div>

          <div className="flex items-center gap-2">
            <div className="text-[11px] text-slate-400">延长时长</div>
            <input
              type="number"
              min={1}
              max={3600}
              step={1}
              value={extendSeconds}
              onChange={(e) => setExtendSeconds(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))}
              className="w-20 h-7 px-2 rounded-md bg-slate-900/60 border border-slate-700/70 text-amber-200"
            />
            <div className="text-[11px] text-slate-500">s（对单条故障点“延长”生效）</div>
          </div>

          <div className="max-h-64 overflow-y-auto space-y-1.5 pr-1">
            {sortedFaults.length === 0 && (
              <div className="text-[11px] text-slate-500 rounded-lg border border-slate-700/60 bg-slate-900/25 px-2 py-2">
                当前无手动注入故障
              </div>
            )}
            {sortedFaults.map((f) => {
              const remain = liveRemaining(f)
              const remainLabel = remain > 0 ? `${remain.toFixed(0)}s` : '0s'
              const remainColor = remain <= 5 ? 'text-rose-300' : remain <= 15 ? 'text-amber-300' : 'text-emerald-300'
              return (
                <div
                  key={`${f.node_id}-${f.fault_type}`}
                  className="rounded-lg border border-slate-700/60 bg-slate-900/30 px-2.5 py-2"
                >
                  <div className="flex items-center justify-between gap-2">
                    <div className="text-[12px] text-cyan-100 font-mono">{f.node_id}</div>
                    <div className={`text-[11px] font-semibold ${remainColor}`}>{remainLabel}</div>
                  </div>
                  <div className="text-[11px] text-slate-300 mt-0.5">
                    {faultTypeLabel(f.fault_type)}
                    <span className="text-slate-500"> · {f.injection_mode === 'manual' ? '手动注入' : f.injection_mode}</span>
                  </div>
                  <div className="flex items-center justify-between gap-2 mt-1.5">
                    <div className="text-[10px] text-slate-500">操作</div>
                    <div className="flex items-center gap-2">
                    <button
                      type="button"
                      onClick={() => extendFault(f.node_id)}
                      disabled={managingFaults}
                      className="h-7 px-2.5 rounded-md text-[11px] text-amber-100 bg-amber-900/30 border border-amber-800/60 disabled:opacity-60 inline-flex items-center gap-1"
                      title={`延长 ${extendSeconds}s`}
                    >
                      <Plus className="w-3 h-3" />
                      <span className="font-mono">{extendSeconds}s</span>
                    </button>
                    <button
                      type="button"
                      onClick={() => removeFault(f.node_id)}
                      disabled={managingFaults}
                      className="h-7 w-7 rounded-md text-rose-100 bg-rose-900/35 border border-rose-800/65 disabled:opacity-60 inline-flex items-center justify-center"
                      title="移除故障"
                    >
                      <Trash2 className="w-3.5 h-3.5" />
                    </button>
                    </div>
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
