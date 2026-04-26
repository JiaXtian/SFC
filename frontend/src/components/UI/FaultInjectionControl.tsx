import { useEffect, useMemo, useState } from 'react'
import { Activity, AlertTriangle, ListChecks, Plus, RefreshCw, Trash2, X } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'

type NodeFault = {
  node_id: string
  fault_type: string
  injection_mode: string
  ttl_ticks: number
  remaining_sec: number
  fetched_at_ms: number
}

type LinkFault = {
  link_key: string
  source: string
  target: string
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

function linkFaultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    optical_signal_loss: '光链路信号丢失',
    beam_misalignment: '波束失准',
    interference_jamming: '链路干扰',
    routing_blackhole: '路由黑洞',
    transceiver_failure: '收发器故障',
    line_degradation: '链路退化',
    endpoint_node_fault: '端点节点故障',
  }
  return map[tag] ?? tag
}

function splitIds(raw: string): string[] {
  return raw
    .split(/[\s,;，；\n\t]+/)
    .map((x) => x.trim())
    .filter(Boolean)
}

function readNodeFaults(status: any): NodeFault[] {
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

function readLinkFaults(status: any): LinkFault[] {
  const now = Date.now()
  const arr = Array.isArray(status?.active_fault_details?.link) ? status.active_fault_details.link : []
  return arr
    .map((f: any) => {
      const source = String(f?.source ?? '')
      const target = String(f?.target ?? '')
      const linkKey = String(f?.link_key ?? (source && target ? `${source}|${target}` : ''))
      return {
        link_key: linkKey,
        source,
        target,
        fault_type: String(f?.fault_type ?? 'unknown'),
        injection_mode: String(f?.injection_mode ?? 'manual'),
        ttl_ticks: Math.max(0, Number(f?.ttl_ticks ?? 0)),
        remaining_sec: Math.max(0, Number(f?.remaining_sec ?? 0)),
        fetched_at_ms: now,
      } as LinkFault
    })
    .filter((f) => !!f.link_key)
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
  const [linkFaultCatalog, setLinkFaultCatalog] = useState<string[]>([])
  const [activeNodeFaults, setActiveNodeFaults] = useState<NodeFault[]>([])
  const [activeLinkFaults, setActiveLinkFaults] = useState<LinkFault[]>([])
  const [nowMs, setNowMs] = useState(Date.now())

  const [injectScope, setInjectScope] = useState<'single' | 'batch'>('single')
  const [injectMode, setInjectMode] = useState<'random' | 'manual'>('random')
  const [injectNodeId, setInjectNodeId] = useState('')
  const [injectBatchCount, setInjectBatchCount] = useState(5)
  const [injectFaultType, setInjectFaultType] = useState('auto')
  const [injectDurationSec, setInjectDurationSec] = useState(20)
  const [injectOnlyActive, setInjectOnlyActive] = useState(true)

  const [linkFaultSource, setLinkFaultSource] = useState('')
  const [linkFaultTarget, setLinkFaultTarget] = useState('')
  const [linkFaultType, setLinkFaultType] = useState('auto')
  const [linkFaultDurationSec, setLinkFaultDurationSec] = useState(20)

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

      const nodeFaults = Array.isArray(status?.fault_catalog?.node)
        ? status.fault_catalog.node.map((x: any) => String(x))
        : []
      const linkFaults = Array.isArray(status?.fault_catalog?.link)
        ? status.fault_catalog.link.map((x: any) => String(x))
        : []
      setNodeFaultCatalog(nodeFaults)
      setLinkFaultCatalog(linkFaults)
      setActiveNodeFaults(readNodeFaults(status))
      setActiveLinkFaults(readLinkFaults(status))
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

  const injectNodeFault = async () => {
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
      if (injected > 0) addToast(`已注入 ${injected} 个节点故障`, 'success')
      else if (skipped > 0) addToast(`目标已存在故障，跳过 ${skipped} 个节点`, 'warning')
      else addToast('未注入故障（目标可能无效）', 'warning')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`节点故障注入失败: ${e?.message ?? e}`, 'error')
    } finally {
      setInjectingFaults(false)
    }
  }

  const injectLinkFault = async () => {
    setInjectingFaults(true)
    try {
      const source = linkFaultSource.trim()
      const target = linkFaultTarget.trim()
      if (!source || !target) throw new Error('请先输入链路两端卫星ID')
      if (!satIdSet.has(source) || !satIdSet.has(target)) throw new Error('链路端点卫星ID不存在于当前星座')
      if (source === target) throw new Error('链路端点不能相同')

      const res = await apiClient.injectDynamicFaults({
        entity_type: 'link',
        action: 'inject',
        source,
        target,
        fault_type: linkFaultType || 'auto',
        ttl_ticks: secToTicks(Math.max(1, Math.min(3600, Number(linkFaultDurationSec || 1)))),
        overwrite_existing: true,
      })
      const injected = Number(res?.injected ?? 0)
      const skipped = Number(res?.skipped_existing ?? 0)
      if (injected > 0) addToast(`已注入链路故障: ${source} ↔ ${target}`, 'success')
      else if (skipped > 0) addToast(`链路故障已存在: ${source} ↔ ${target}`, 'warning')
      else addToast('未注入链路故障（目标链路可能不存在）', 'warning')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`链路故障注入失败: ${e?.message ?? e}`, 'error')
    } finally {
      setInjectingFaults(false)
    }
  }

  const removeFault = async (nodeId: string) => {
    setManagingFaults(true)
    try {
      const res = await apiClient.injectDynamicFaults({ entity_type: 'node', action: 'remove', node_id: nodeId })
      const removed = Number(res?.removed ?? 0)
      addToast(removed > 0 ? `已移除 ${nodeId} 故障` : `节点 ${nodeId} 当前无故障`, removed > 0 ? 'success' : 'warning')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`移除节点故障失败: ${e?.message ?? e}`, 'error')
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
      addToast(extended > 0 ? `已为 ${nodeId} 延长 ${deltaSeconds}s` : `节点 ${nodeId} 当前无故障`, extended > 0 ? 'success' : 'warning')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`延长节点故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const removeLinkFault = async (source: string, target: string) => {
    setManagingFaults(true)
    try {
      const res = await apiClient.injectDynamicFaults({ entity_type: 'link', action: 'remove', source, target })
      const removed = Number(res?.removed ?? 0)
      addToast(removed > 0 ? `已移除链路故障 ${source} ↔ ${target}` : `链路 ${source} ↔ ${target} 当前无故障`, removed > 0 ? 'success' : 'warning')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`移除链路故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const extendLinkFault = async (source: string, target: string) => {
    setManagingFaults(true)
    try {
      const deltaSeconds = Math.max(1, Math.min(3600, Number(extendSeconds || 1)))
      const res = await apiClient.injectDynamicFaults({
        entity_type: 'link',
        action: 'extend',
        source,
        target,
        delta_seconds: deltaSeconds,
      })
      const extended = Number(res?.extended ?? 0)
      addToast(extended > 0 ? `已为链路 ${source} ↔ ${target} 延长 ${deltaSeconds}s` : `链路 ${source} ↔ ${target} 当前无故障`, extended > 0 ? 'success' : 'warning')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`延长链路故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const removeAllNodeFaults = async () => {
    const ids = activeNodeFaults.map((f) => f.node_id).filter(Boolean)
    if (ids.length === 0) {
      addToast('当前没有节点故障', 'info')
      return
    }
    setManagingFaults(true)
    try {
      const res = await apiClient.injectDynamicFaults({ entity_type: 'node', action: 'remove', node_ids: ids })
      addToast(`已移除 ${Number(res?.removed ?? 0)} 个节点故障`, 'success')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`批量移除节点故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const removeAllLinkFaults = async () => {
    if (activeLinkFaults.length === 0) {
      addToast('当前没有链路故障', 'info')
      return
    }
    setManagingFaults(true)
    try {
      let removed = 0
      for (const f of activeLinkFaults) {
        const res = await apiClient.injectDynamicFaults({ entity_type: 'link', action: 'remove', source: f.source, target: f.target })
        removed += Number(res?.removed ?? 0)
      }
      addToast(`已移除 ${removed} 条链路故障`, 'success')
      await refreshStatus(true)
      await syncTopologyAfterFaultMutation()
    } catch (e: any) {
      addToast(`批量移除链路故障失败: ${e?.message ?? e}`, 'error')
    } finally {
      setManagingFaults(false)
    }
  }

  const liveRemaining = (fault: { remaining_sec: number; fetched_at_ms: number }) => {
    const elapsed = Math.max(0, (nowMs - fault.fetched_at_ms) / 1000)
    return Math.max(0, fault.remaining_sec - elapsed)
  }

  const sortedNodeFaults = useMemo(
    () => [...activeNodeFaults].sort((a, b) => liveRemaining(a) - liveRemaining(b)),
    [activeNodeFaults, nowMs],
  )
  const sortedLinkFaults = useMemo(
    () => [...activeLinkFaults].sort((a, b) => liveRemaining(a) - liveRemaining(b)),
    [activeLinkFaults, nowMs],
  )

  return (
    <div
      className="rounded-2xl p-3 h-full flex flex-col"
      style={{
        background: 'linear-gradient(160deg, rgba(11,17,30,0.78), rgba(8,13,24,0.66))',
        border: '1px solid rgba(112,168,208,0.28)',
        backdropFilter: 'blur(14px)',
      }}
    >
      <div className="flex items-center justify-between mb-2">
        <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold inline-flex items-center gap-1.5">
          <Activity className="w-4 h-4 text-cyan-300" />故障控制面板
        </div>
        <div className="inline-flex items-center gap-2">
          <span className="text-[11px] text-slate-400 inline-flex items-center gap-1">
            <ListChecks className="w-3.5 h-3.5 text-cyan-300" />故障时间延长时长（秒）
          </span>
          <input
            type="number"
            min={1}
            max={3600}
            step={1}
            value={extendSeconds}
            onChange={(e) => setExtendSeconds(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))}
            className="w-24 h-7 px-2 rounded-md bg-slate-900/60 border border-slate-700/70 text-amber-200 text-[12px]"
          />
          <button
            type="button"
            onClick={() => refreshStatus(false)}
            disabled={refreshing}
            className="h-7 px-2 rounded-md text-[11px] text-cyan-100 bg-slate-800/70 border border-slate-700/70 inline-flex items-center gap-1 disabled:opacity-60"
          >
            <RefreshCw className={`w-3 h-3 ${refreshing ? 'animate-spin' : ''}`} />刷新
          </button>
        </div>
      </div>

      <div className="flex-1 overflow-y-auto pr-1 space-y-2.5">
        <div className="grid grid-cols-12 gap-2.5">
          <section className="col-span-12 xl:col-span-6 p-2.5 rounded-xl border border-slate-700/60 bg-slate-900/25 space-y-2">
            <div className="text-[12px] text-cyan-100 font-semibold inline-flex items-center gap-1.5">
              <AlertTriangle className="w-3.5 h-3.5 text-cyan-300" />卫星节点故障注入
            </div>
            <div className="text-[11px] text-slate-400">用于模拟单星或多星失效，触发重算/重调度行为。</div>
            <div className="grid grid-cols-12 gap-2">
              <label className="col-span-12 md:col-span-4">
                <div className="text-[10px] text-slate-400 mb-1">故障类型</div>
                <select value={injectFaultType} onChange={(e) => setInjectFaultType(e.target.value)} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-slate-200">
                  <option value="auto">自动匹配</option>
                  {nodeFaultCatalog.map((ft) => <option key={ft} value={ft}>{faultTypeLabel(ft)}</option>)}
                </select>
              </label>
              <label className="col-span-6 md:col-span-4">
                <div className="text-[10px] text-slate-400 mb-1">注入范围</div>
                <select value={injectScope} onChange={(e) => setInjectScope((e.target.value as 'single' | 'batch') || 'single')} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-slate-200">
                  <option value="single">单颗卫星</option>
                  <option value="batch">批量卫星</option>
                </select>
              </label>
              <label className="col-span-6 md:col-span-4">
                <div className="text-[10px] text-slate-400 mb-1">持续秒数</div>
                <input type="number" min={1} max={3600} step={1} value={injectDurationSec} onChange={(e) => setInjectDurationSec(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-amber-200" />
              </label>

              {injectScope === 'single' ? (
                <label className="col-span-12">
                  <div className="text-[10px] text-slate-400 mb-1">目标节点ID</div>
                  <input list="control-satellite-list" value={injectNodeId} onChange={(e) => setInjectNodeId(e.target.value)} placeholder="如 SAT_000_001" className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-cyan-200 font-mono" />
                </label>
              ) : (
                <>
                  <label className="col-span-6 md:col-span-3">
                    <div className="text-[10px] text-slate-400 mb-1">批量模式</div>
                    <select value={injectMode} onChange={(e) => setInjectMode((e.target.value as 'random' | 'manual') || 'random')} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-slate-200">
                      <option value="random">随机</option>
                      <option value="manual">手动列表</option>
                    </select>
                  </label>
                  {injectMode === 'random' ? (
                    <>
                      <label className="col-span-3 md:col-span-3">
                        <div className="text-[10px] text-slate-400 mb-1">随机数量</div>
                        <input type="number" min={1} max={1000} value={injectBatchCount} onChange={(e) => setInjectBatchCount(Math.max(1, Math.min(1000, Number(e.target.value) || 1)))} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-amber-200" />
                      </label>
                      <label className="col-span-3 md:col-span-3">
                        <div className="text-[10px] text-slate-400 mb-1">节点范围</div>
                        <span className="h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-slate-200 inline-flex items-center gap-2 w-full">
                          <input type="checkbox" checked={injectOnlyActive} onChange={(e) => setInjectOnlyActive(e.target.checked)} className="accent-cyan-400" />
                          仅活跃节点
                        </span>
                      </label>
                    </>
                  ) : (
                    <>
                      <label className="col-span-9 md:col-span-7">
                        <div className="text-[10px] text-slate-400 mb-1">手动节点列表</div>
                        <input type="text" list="control-satellite-list" value={manualEntryInput} onChange={(e) => setManualEntryInput(e.target.value)} placeholder="支持逗号/空格/换行粘贴多个ID" className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-cyan-200 font-mono" />
                      </label>
                      <button type="button" onClick={() => { addManualIds(manualEntryInput); setManualEntryInput('') }} className="col-span-3 md:col-span-2 h-8 mt-[18px] rounded-lg text-[12px] text-cyan-100 bg-slate-800/70 border border-cyan-700/40 inline-flex items-center justify-center gap-1">
                        <Plus className="w-3.5 h-3.5" />添加
                      </button>
                    </>
                  )}
                </>
              )}
            </div>
            {injectScope === 'batch' && injectMode === 'manual' && (
              <div className="max-h-16 overflow-y-auto rounded-lg border border-slate-700/65 bg-slate-900/35 p-1.5 flex flex-wrap gap-1.5">
                {manualNodeIds.length === 0 && <div className="text-[11px] text-slate-500">尚未添加卫星ID</div>}
                {manualNodeIds.map((id) => (
                  <span key={id} className="inline-flex items-center gap-1 px-2 py-1 rounded-md text-[11px] text-cyan-100 bg-slate-800/70 border border-slate-600/60">
                    {id}
                    <button type="button" onClick={() => setManualNodeIds((prev) => prev.filter((x) => x !== id))} className="text-slate-400 hover:text-rose-300"><X className="w-3 h-3" /></button>
                  </span>
                ))}
              </div>
            )}
            <button disabled={injectingFaults} onClick={injectNodeFault} className="w-full h-8 rounded-lg text-[12px] text-cyan-100 bg-cyan-500/15 border border-cyan-500/35 disabled:opacity-60">
              {injectingFaults ? '注入中...' : '执行节点故障注入'}
            </button>
          </section>

          <section className="col-span-12 xl:col-span-6 p-2.5 rounded-xl border border-slate-700/60 bg-slate-900/25 space-y-2">
            <div className="text-[12px] text-cyan-100 font-semibold inline-flex items-center gap-1.5">
              <AlertTriangle className="w-3.5 h-3.5 text-cyan-300" />卫星链路故障注入
            </div>
            <div className="text-[11px] text-slate-400">用于模拟链路断连或退化，验证路径重算与服务恢复能力。</div>
            <div className="grid grid-cols-12 gap-2">
              <label className="col-span-12">
                <div className="text-[10px] text-slate-400 mb-1">故障类型</div>
                <select value={linkFaultType} onChange={(e) => setLinkFaultType(e.target.value)} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-slate-200">
                  <option value="auto">自动匹配</option>
                  {linkFaultCatalog.map((ft) => <option key={ft} value={ft}>{linkFaultTypeLabel(ft)}</option>)}
                </select>
              </label>
              <label className="col-span-6">
                <div className="text-[10px] text-slate-400 mb-1">源节点ID</div>
                <input list="control-satellite-list" value={linkFaultSource} onChange={(e) => setLinkFaultSource(e.target.value)} placeholder="如 SAT_000_001" className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-amber-100 font-mono" />
              </label>
              <label className="col-span-6">
                <div className="text-[10px] text-slate-400 mb-1">宿节点ID</div>
                <input list="control-satellite-list" value={linkFaultTarget} onChange={(e) => setLinkFaultTarget(e.target.value)} placeholder="如 SAT_000_002" className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-amber-100 font-mono" />
              </label>
              <label className="col-span-12">
                <div className="text-[10px] text-slate-400 mb-1">持续秒数</div>
                <input type="number" min={1} max={3600} step={1} value={linkFaultDurationSec} onChange={(e) => setLinkFaultDurationSec(Math.max(1, Math.min(3600, Number(e.target.value) || 1)))} className="w-full h-8 px-2 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-amber-200" />
              </label>
            </div>
            <button disabled={injectingFaults} onClick={injectLinkFault} className="w-full h-8 rounded-lg text-[12px] text-cyan-100 bg-cyan-500/15 border border-cyan-500/35 disabled:opacity-60">
              {injectingFaults ? '注入中...' : '执行链路故障注入'}
            </button>
          </section>
        </div>

        <div className="grid grid-cols-12 gap-2.5">
          <section className="col-span-12 xl:col-span-6 p-2.5 rounded-xl border border-slate-700/60 bg-slate-900/25">
            <div className="flex items-center justify-between mb-2">
              <div className="text-[12px] text-cyan-100 font-semibold">当前卫星节点故障</div>
              <button type="button" onClick={removeAllNodeFaults} disabled={managingFaults || sortedNodeFaults.length === 0} className="h-7 px-2 rounded-md text-[11px] text-slate-200 bg-slate-800/70 border border-slate-700/70 disabled:opacity-60">清空节点故障</button>
            </div>
            <div className="max-h-[340px] overflow-auto rounded-lg border border-slate-700/65">
              <table className="w-full text-[11px]">
                <thead className="sticky top-0 z-10 bg-slate-900/95 text-slate-400">
                  <tr>
                    <th className="px-2 py-1.5 text-left font-medium">节点ID</th>
                    <th className="px-2 py-1.5 text-left font-medium">故障类型</th>
                    <th className="px-2 py-1.5 text-left font-medium">模式</th>
                    <th className="px-2 py-1.5 text-left font-medium">TTL</th>
                    <th className="px-2 py-1.5 text-left font-medium">剩余</th>
                    <th className="px-2 py-1.5 text-left font-medium">操作</th>
                  </tr>
                </thead>
                <tbody>
                  {sortedNodeFaults.length === 0 && (
                    <tr><td colSpan={6} className="px-2 py-5 text-center text-slate-500">当前无节点故障</td></tr>
                  )}
                  {sortedNodeFaults.map((f) => {
                    const remain = liveRemaining(f)
                    const remainLabel = remain > 0 ? `${remain.toFixed(0)}s` : '0s'
                    const remainColor = remain <= 5 ? 'text-rose-300' : remain <= 15 ? 'text-amber-300' : 'text-emerald-300'
                    return (
                      <tr key={`${f.node_id}-${f.fault_type}`} className="border-t border-slate-800/80 text-slate-200">
                        <td className="px-2 py-2 font-mono">{f.node_id}</td>
                        <td className="px-2 py-2">{faultTypeLabel(f.fault_type)}</td>
                        <td className="px-2 py-2 text-slate-400">{f.injection_mode === 'manual' ? '手动' : f.injection_mode}</td>
                        <td className="px-2 py-2 font-mono">{f.ttl_ticks}</td>
                        <td className={`px-2 py-2 font-semibold ${remainColor}`}>{remainLabel}</td>
                        <td className="px-2 py-2">
                          <div className="inline-flex items-center gap-1.5">
                            <button type="button" onClick={() => extendFault(f.node_id)} disabled={managingFaults} className="h-7 px-2 rounded-md text-[11px] text-slate-100 bg-slate-800/70 border border-slate-700/70 disabled:opacity-60 inline-flex items-center gap-1"><Plus className="w-3 h-3" />延长</button>
                            <button type="button" onClick={() => removeFault(f.node_id)} disabled={managingFaults} className="h-7 w-7 rounded-md text-slate-100 bg-slate-800/70 border border-slate-700/70 disabled:opacity-60 inline-flex items-center justify-center"><Trash2 className="w-3.5 h-3.5" /></button>
                          </div>
                        </td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
          </section>

          <section className="col-span-12 xl:col-span-6 p-2.5 rounded-xl border border-slate-700/60 bg-slate-900/25">
            <div className="flex items-center justify-between mb-2">
              <div className="text-[12px] text-cyan-100 font-semibold">当前卫星链路故障</div>
              <button type="button" onClick={removeAllLinkFaults} disabled={managingFaults || sortedLinkFaults.length === 0} className="h-7 px-2 rounded-md text-[11px] text-slate-200 bg-slate-800/70 border border-slate-700/70 disabled:opacity-60">清空链路故障</button>
            </div>
            <div className="max-h-[340px] overflow-auto rounded-lg border border-slate-700/65">
              <table className="w-full text-[11px]">
                <thead className="sticky top-0 z-10 bg-slate-900/95 text-slate-400">
                  <tr>
                    <th className="px-2 py-1.5 text-left font-medium">源节点</th>
                    <th className="px-2 py-1.5 text-left font-medium">宿节点</th>
                    <th className="px-2 py-1.5 text-left font-medium">故障类型</th>
                    <th className="px-2 py-1.5 text-left font-medium">模式</th>
                    <th className="px-2 py-1.5 text-left font-medium">TTL</th>
                    <th className="px-2 py-1.5 text-left font-medium">剩余</th>
                    <th className="px-2 py-1.5 text-left font-medium">操作</th>
                  </tr>
                </thead>
                <tbody>
                  {sortedLinkFaults.length === 0 && (
                    <tr><td colSpan={7} className="px-2 py-5 text-center text-slate-500">当前无链路故障</td></tr>
                  )}
                  {sortedLinkFaults.map((f) => {
                    const remain = liveRemaining(f)
                    const remainLabel = remain > 0 ? `${remain.toFixed(0)}s` : '0s'
                    const remainColor = remain <= 5 ? 'text-rose-300' : remain <= 15 ? 'text-amber-300' : 'text-emerald-300'
                    return (
                      <tr key={`${f.link_key}-${f.fault_type}`} className="border-t border-slate-800/80 text-slate-200">
                        <td className="px-2 py-2 font-mono">{f.source}</td>
                        <td className="px-2 py-2 font-mono">{f.target}</td>
                        <td className="px-2 py-2">{linkFaultTypeLabel(f.fault_type)}</td>
                        <td className="px-2 py-2 text-slate-400">{f.injection_mode === 'manual' ? '手动' : f.injection_mode}</td>
                        <td className="px-2 py-2 font-mono">{f.ttl_ticks}</td>
                        <td className={`px-2 py-2 font-semibold ${remainColor}`}>{remainLabel}</td>
                        <td className="px-2 py-2">
                          <div className="inline-flex items-center gap-1.5">
                            <button type="button" onClick={() => extendLinkFault(f.source, f.target)} disabled={managingFaults} className="h-7 px-2 rounded-md text-[11px] text-slate-100 bg-slate-800/70 border border-slate-700/70 disabled:opacity-60 inline-flex items-center gap-1"><Plus className="w-3 h-3" />延长</button>
                            <button type="button" onClick={() => removeLinkFault(f.source, f.target)} disabled={managingFaults} className="h-7 w-7 rounded-md text-slate-100 bg-slate-800/70 border border-slate-700/70 disabled:opacity-60 inline-flex items-center justify-center"><Trash2 className="w-3.5 h-3.5" /></button>
                          </div>
                        </td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
          </section>
        </div>
      </div>

      <datalist id="control-satellite-list">
        {satellites.slice(0, 8000).map((sat: any) => (
          <option key={String(sat?.id ?? '')} value={String(sat?.id ?? '')} />
        ))}
      </datalist>
    </div>
  )
}
