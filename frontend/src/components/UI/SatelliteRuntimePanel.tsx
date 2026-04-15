import { useEffect, useMemo, useState } from 'react'
import { Eye, Pause, Play, RefreshCw, Radar, Trash2 } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'

type SatNode = any

function toNumber(v: any, fallback = 0) {
  const n = Number(v)
  return Number.isFinite(n) ? n : fallback
}

function podStatus(sat: SatNode) {
  return String(sat?.podman?.status ?? sat?.podman_status ?? 'unknown').toLowerCase()
}

function podName(sat: SatNode) {
  return String(sat?.podman?.container_name ?? sat?.podman_container_name ?? '')
}

function podId(sat: SatNode) {
  return String(sat?.podman?.container_id ?? sat?.podman_container_id ?? '')
}

function naturalIdCompare(a: string, b: string) {
  return a.localeCompare(b, 'zh-CN', { numeric: true, sensitivity: 'base' })
}

function splitCsv(raw: any): string[] {
  const text = String(raw ?? '').trim()
  if (!text) return []
  return text
    .split(/[，,;；\n\t\s]+/)
    .map((x) => x.trim())
    .filter(Boolean)
}

function collectSfcNames(sat: SatNode): string[] {
  const set = new Set<string>()
  splitCsv(sat?.deployed_sfc_names).forEach((x) => set.add(x))
  const policy = String(sat?.core_nf_policy ?? '').trim()
  if (policy) set.add(policy)
  const vnfs = Array.isArray(sat?.vnfs) ? sat.vnfs : []
  vnfs.forEach((v: any) => {
    const sfc = String(v?.sfc_id ?? '').trim()
    if (sfc) set.add(sfc)
  })
  return Array.from(set)
}

function collectCoreNfTypes(sat: SatNode): string[] {
  const set = new Set<string>()
  splitCsv(sat?.deployed_core_nf_types).forEach((x) => set.add(x))
  const vnfs = Array.isArray(sat?.vnfs) ? sat.vnfs : []
  vnfs.forEach((v: any) => {
    const t = String(v?.core_nf_type ?? v?.nf_type ?? v?.vnf_type ?? '').trim()
    if (t) set.add(t)
  })
  return Array.from(set)
}

function utilization(sat: SatNode, key: 'cpu' | 'mem' | 'disk') {
  const telemetryKey = `${key}_utilization_ratio`
  const telemetry = toNumber(sat?.telemetry?.[telemetryKey], Number.NaN)
  const direct = toNumber(sat?.[telemetryKey], Number.NaN)
  if (Number.isFinite(telemetry)) return Math.max(0, Math.min(1, telemetry))
  if (Number.isFinite(direct)) return Math.max(0, Math.min(1, direct))
  const total = toNumber(sat?.[`${key}_total`], 0)
  const available = toNumber(sat?.[`${key}_available`], total)
  if (total <= 1e-9) return 0
  return Math.max(0, Math.min(1, 1 - available / total))
}

function badgeClass(kind: 'pod' | 'fault' | 'deploy' | 'node', status: string) {
  const s = status.toLowerCase()
  if (kind === 'pod') {
    if (s === 'running' || s === 'simulated') return 'border-emerald-200 bg-emerald-50 text-emerald-700'
    if (s === 'created' || s === 'stopped' || s === 'exited') return 'border-amber-200 bg-amber-50 text-amber-700'
    return 'border-slate-200 bg-slate-50 text-slate-700'
  }
  if (kind === 'fault') {
    if (s === 'true' || s === '1' || s === 'yes') return 'border-red-200 bg-red-50 text-red-700'
    return 'border-slate-200 bg-slate-50 text-slate-700'
  }
  if (kind === 'deploy') {
    if (s === 'deployed') return 'border-blue-200 bg-blue-50 text-blue-700'
    if (s === 'rolled_back') return 'border-amber-200 bg-amber-50 text-amber-700'
    return 'border-slate-200 bg-slate-50 text-slate-700'
  }
  if (s === 'active' || s === 'running') return 'border-emerald-200 bg-emerald-50 text-emerald-700'
  if (s === 'down' || s === 'failed') return 'border-red-200 bg-red-50 text-red-700'
  return 'border-slate-200 bg-slate-50 text-slate-700'
}

export default function SatelliteRuntimePanel() {
  const { addToast } = useStore((s) => ({ addToast: s.addToast }))

  const [satellites, setSatellites] = useState<SatNode[]>([])
  const [runtime, setRuntime] = useState<any>(null)
  const [persistence, setPersistence] = useState<any>(null)
  const [loading, setLoading] = useState(false)
  const [busy, setBusy] = useState(false)
  const [keyword, setKeyword] = useState('')
  const [selected, setSelected] = useState<Set<string>>(new Set())
  const [detailSat, setDetailSat] = useState<SatNode | null>(null)
  const [page, setPage] = useState(1)
  const [pollSec, setPollSec] = useState(5)
  const [autoRefresh, setAutoRefresh] = useState(true)

  const pageSize = 45

  const refresh = async (silent = false) => {
    if (!silent) setLoading(true)
    try {
      const [sats, status] = await Promise.all([
        apiClient.getSatellites(),
        apiClient.getRuntimeStatus(),
      ])
      setSatellites(Array.isArray(sats) ? sats : [])
      setRuntime(status?.runtime ?? status ?? null)
      setPersistence(status?.persistence ?? null)
    } catch (e: any) {
      if (!silent) addToast(`刷新卫星列表失败: ${e?.message ?? e}`, 'error')
    } finally {
      if (!silent) setLoading(false)
    }
  }

  useEffect(() => {
    refresh(false)
  }, [])

  useEffect(() => {
    if (!autoRefresh) return
    const timer = window.setInterval(() => refresh(true), Math.max(2, pollSec) * 1000)
    return () => window.clearInterval(timer)
  }, [autoRefresh, pollSec])

  const filtered = useMemo(() => {
    const sorted = [...satellites].sort((a: any, b: any) =>
      naturalIdCompare(String(a?.id ?? ''), String(b?.id ?? '')),
    )
    const q = keyword.trim().toLowerCase()
    if (!q) return sorted
    return sorted.filter((sat) => {
      const id = String(sat?.id ?? '').toLowerCase()
      const tmpl = String(sat?.template_id ?? '').toLowerCase()
      const pod = podName(sat).toLowerCase()
      const sfc = collectSfcNames(sat).join(',').toLowerCase()
      return id.includes(q) || tmpl.includes(q) || pod.includes(q) || sfc.includes(q)
    })
  }, [satellites, keyword])

  const totalPages = Math.max(1, Math.ceil(filtered.length / pageSize))
  const safePage = Math.max(1, Math.min(totalPages, page))
  const pageRows = useMemo(() => {
    const start = (safePage - 1) * pageSize
    return filtered.slice(start, start + pageSize)
  }, [filtered, safePage])

  useEffect(() => {
    if (page !== safePage) setPage(safePage)
  }, [safePage, page])

  const selectedIds = useMemo(() => Array.from(selected), [selected])

  const toggleRow = (id: string, checked: boolean) => {
    setSelected((prev) => {
      const next = new Set(prev)
      if (checked) next.add(id)
      else next.delete(id)
      return next
    })
  }

  const toggleAllCurrentPage = (checked: boolean) => {
    setSelected((prev) => {
      const next = new Set(prev)
      pageRows.forEach((sat) => {
        const id = String(sat?.id ?? '')
        if (!id) return
        if (checked) next.add(id)
        else next.delete(id)
      })
      return next
    })
  }

  const collectTelemetry = async () => {
    setBusy(true)
    try {
      await apiClient.collectSatelliteTelemetry(true)
      await refresh(true)
      addToast('已触发卫星节点状态采集', 'success')
    } catch (e: any) {
      addToast(`触发采集失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const startNodes = async (nodeIds: string[]) => {
    if (nodeIds.length === 0) {
      addToast('请先选择节点', 'warning')
      return
    }
    setBusy(true)
    try {
      await apiClient.startSatellitePods(nodeIds, false)
      addToast(`已提交启动 ${nodeIds.length} 个节点`, 'success')
      setSelected(new Set())
      await refresh(true)
    } catch (e: any) {
      addToast(`启动节点失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const stopNodes = async (nodeIds: string[]) => {
    if (nodeIds.length === 0) {
      addToast('请先选择节点', 'warning')
      return
    }
    setBusy(true)
    try {
      await apiClient.stopSatellitePods(nodeIds, false)
      addToast(`已提交停止 ${nodeIds.length} 个节点`, 'success')
      setSelected(new Set())
      await refresh(true)
    } catch (e: any) {
      addToast(`停止节点失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const deleteNodes = async (nodeIds: string[]) => {
    if (nodeIds.length === 0) {
      addToast('请先选择节点', 'warning')
      return
    }
    if (!window.confirm(`确认删除 ${nodeIds.length} 个卫星节点吗？将同时删除对应 pod 与链路。`)) return
    setBusy(true)
    try {
      await apiClient.deleteSatelliteNodes(nodeIds, true)
      addToast(`已删除 ${nodeIds.length} 个卫星节点`, 'success')
      setSelected(new Set())
      await refresh(true)
    } catch (e: any) {
      addToast(`删除节点失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBusy(false)
    }
  }

  const running = toNumber(runtime?.running, 0)
  const stopped = toNumber(runtime?.stopped, 0)
  const failed = toNumber(runtime?.failed, 0)

  return (
    <div className="flex h-full min-h-0 flex-col rounded-2xl bg-white p-3">
      <div className="mb-3 flex flex-wrap items-center gap-2">
        <h3 className="text-2xl font-semibold text-black">卫星节点控制中心</h3>
        <span className={`rounded-lg border px-2 py-0.5 text-sm ${badgeClass('pod', 'running')}`}>运行 {running}</span>
        <span className={`rounded-lg border px-2 py-0.5 text-sm ${badgeClass('pod', 'created')}`}>停止 {stopped}</span>
        <span className={`rounded-lg border px-2 py-0.5 text-sm ${badgeClass('fault', 'true')}`}>异常 {failed}</span>
        <span className="rounded-lg border border-slate-200 px-2 py-0.5 text-sm">持久化 {persistence?.mode ?? 'mysql_only'}</span>
      </div>

      <div className="mb-3 flex flex-wrap items-center gap-2">
        <input
          value={keyword}
          onChange={(e) => setKeyword(e.target.value)}
          placeholder="搜索卫星ID / 模板 / 容器名 / SFC"
          className="h-10 w-[420px] max-w-full rounded-xl border border-slate-200 bg-white px-3 text-sm text-black outline-none focus:border-slate-400"
        />
        <button
          onClick={() => refresh(false)}
          disabled={loading || busy}
          className="inline-flex h-10 items-center gap-1 rounded-xl border border-slate-200 bg-white px-3 text-sm text-black disabled:opacity-60"
        >
          <RefreshCw className={`h-4 w-4 ${loading ? 'animate-spin' : ''}`} />刷新
        </button>
        <button
          onClick={collectTelemetry}
          disabled={loading || busy}
          className="inline-flex h-10 items-center gap-1 rounded-xl border border-slate-200 bg-white px-3 text-sm text-black disabled:opacity-60"
        >
          <Radar className="h-4 w-4" />采集
        </button>
        <button
          onClick={() => startNodes(selectedIds)}
          disabled={loading || busy}
          className="inline-flex h-10 items-center gap-1 rounded-xl border border-slate-200 bg-white px-3 text-sm text-black disabled:opacity-60"
        >
          <Play className="h-4 w-4" />启动({selected.size})
        </button>
        <button
          onClick={() => stopNodes(selectedIds)}
          disabled={loading || busy}
          className="inline-flex h-10 items-center gap-1 rounded-xl border border-slate-200 bg-white px-3 text-sm text-black disabled:opacity-60"
        >
          <Pause className="h-4 w-4" />暂停({selected.size})
        </button>
        <button
          onClick={() => deleteNodes(selectedIds)}
          disabled={loading || busy}
          className="inline-flex h-10 items-center gap-1 rounded-xl border border-slate-200 bg-white px-3 text-sm text-black disabled:opacity-60"
        >
          <Trash2 className="h-4 w-4" />删除({selected.size})
        </button>
      </div>

      <div className="mb-3 flex items-center gap-2 text-sm text-black">
        <label className="inline-flex items-center gap-2">
          资源读取间隔(s)
          <input
            type="number"
            min={2}
            max={60}
            step={1}
            value={pollSec}
            onChange={(e) => setPollSec(Math.max(2, Math.min(60, Number(e.target.value) || 5)))}
            className="h-9 w-20 rounded-xl border border-slate-200 bg-white px-2"
          />
        </label>
        <label className="inline-flex items-center gap-2">
          <input type="checkbox" checked={autoRefresh} onChange={(e) => setAutoRefresh(e.target.checked)} />
          自动刷新
        </label>
      </div>

      <div className="min-h-0 flex-1 overflow-auto rounded-xl border border-slate-200">
        <table className="min-w-[2600px] w-full text-[12px] text-black">
          <thead className="sticky top-0 bg-white">
            <tr className="border-b border-slate-200 text-black">
              <th className="px-2 py-2 text-left">
                <input
                  type="checkbox"
                  checked={pageRows.length > 0 && pageRows.every((sat) => selected.has(String(sat?.id ?? '')))}
                  onChange={(e) => toggleAllCurrentPage(e.target.checked)}
                />
              </th>
              <th className="px-2 py-2 text-left">卫星ID</th>
              <th className="px-2 py-2 text-left">高度(km)</th>
              <th className="px-2 py-2 text-left">倾角(°)</th>
              <th className="px-2 py-2 text-left">Plane</th>
              <th className="px-2 py-2 text-left">Pos</th>
              <th className="px-2 py-2 text-left">经度</th>
              <th className="px-2 py-2 text-left">纬度</th>
              <th className="px-2 py-2 text-left">Podman状态</th>
              <th className="px-2 py-2 text-left">容器名</th>
              <th className="px-2 py-2 text-left">容器ID</th>
              <th className="px-2 py-2 text-left">节点状态</th>
              <th className="px-2 py-2 text-left">是否故障</th>
              <th className="px-2 py-2 text-left">故障类型</th>
              <th className="px-2 py-2 text-left">部署状态</th>
              <th className="px-2 py-2 text-left">部署SFC名称</th>
              <th className="px-2 py-2 text-left">核心网元类型</th>
              <th className="px-2 py-2 text-left">资源状态</th>
              <th className="px-2 py-2 text-left">操作</th>
            </tr>
          </thead>
          <tbody>
            {pageRows.map((sat) => {
              const id = String(sat?.id ?? '')
              const pStatus = podStatus(sat)
              const nodeStatus = String(sat?.status ?? 'active')
              const faultFlag = String(Boolean(sat?.fault_injected))
              const faultTag = String(sat?.fault_tag || 'none')
              const deployState = String(sat?.deployment_state || 'none')
              const sfcNames = collectSfcNames(sat)
              const coreTypes = collectCoreNfTypes(sat)
              const cpuU = utilization(sat, 'cpu')
              const memU = utilization(sat, 'mem')
              const diskU = utilization(sat, 'disk')

              return (
                <tr key={id} className="border-b border-slate-100 align-top hover:bg-slate-50/60">
                  <td className="px-2 py-2">
                    <input
                      type="checkbox"
                      checked={selected.has(id)}
                      onChange={(e) => toggleRow(id, e.target.checked)}
                    />
                  </td>
                  <td className="px-2 py-2 font-mono">{id}</td>
                  <td className="px-2 py-2">{toNumber(sat?.orbital_params?.altitude_km, 0).toFixed(2)}</td>
                  <td className="px-2 py-2">{toNumber(sat?.orbital_params?.inclination_deg ?? sat?.orbital_params?.inclination, 0).toFixed(2)}</td>
                  <td className="px-2 py-2">{toNumber(sat?.orbital_params?.plane, 0)}</td>
                  <td className="px-2 py-2">{toNumber(sat?.orbital_params?.position_in_plane, 0)}</td>
                  <td className="px-2 py-2">{toNumber(sat?.coordinates?.lon, 0).toFixed(4)}</td>
                  <td className="px-2 py-2">{toNumber(sat?.coordinates?.lat, 0).toFixed(4)}</td>
                  <td className="px-2 py-2">
                    <span className={`inline-flex rounded-lg border px-2 py-0.5 text-[11px] ${badgeClass('pod', pStatus)}`}>{pStatus}</span>
                  </td>
                  <td className="px-2 py-2 font-mono text-[11px]">{podName(sat) || '-'}</td>
                  <td className="px-2 py-2 font-mono text-[11px]">{podId(sat) ? podId(sat).slice(0, 24) : '-'}</td>
                  <td className="px-2 py-2">
                    <span className={`inline-flex rounded-lg border px-2 py-0.5 text-[11px] ${badgeClass('node', nodeStatus)}`}>{nodeStatus}</span>
                  </td>
                  <td className="px-2 py-2">
                    <span className={`inline-flex rounded-lg border px-2 py-0.5 text-[11px] ${badgeClass('fault', faultFlag)}`}>{faultFlag}</span>
                  </td>
                  <td className="px-2 py-2">{faultTag}</td>
                  <td className="px-2 py-2">
                    <span className={`inline-flex rounded-lg border px-2 py-0.5 text-[11px] ${badgeClass('deploy', deployState)}`}>{deployState}</span>
                  </td>
                  <td className="px-2 py-2">
                    <div className="flex max-w-[260px] flex-wrap gap-1">
                      {sfcNames.length === 0 && <span>-</span>}
                      {sfcNames.map((name) => (
                        <span key={`${id}-sfc-${name}`} className="rounded-lg border border-slate-200 px-2 py-0.5 text-[11px]">
                          {name}
                        </span>
                      ))}
                    </div>
                  </td>
                  <td className="px-2 py-2">
                    <div className="flex max-w-[260px] flex-wrap gap-1">
                      {coreTypes.length === 0 && <span>-</span>}
                      {coreTypes.map((t, idx) => (
                        <span
                          key={`${id}-nf-${t}-${idx}`}
                          className={`rounded-lg border px-2 py-0.5 text-[11px] ${idx % 4 === 0 ? 'border-blue-200 bg-blue-50 text-blue-700' : 'border-slate-200'}`}
                        >
                          {t}
                        </span>
                      ))}
                    </div>
                  </td>
                  <td className="px-2 py-2 text-[11px]">
                    <div>CPU {toNumber(sat?.cpu_available).toFixed(2)}/{toNumber(sat?.cpu_total).toFixed(2)} ({(cpuU * 100).toFixed(1)}%)</div>
                    <div>MEM {toNumber(sat?.mem_available).toFixed(2)}/{toNumber(sat?.mem_total).toFixed(2)} ({(memU * 100).toFixed(1)}%)</div>
                    <div>DISK {toNumber(sat?.disk_available).toFixed(2)}/{toNumber(sat?.disk_total).toFixed(2)} ({(diskU * 100).toFixed(1)}%)</div>
                  </td>
                  <td className="px-2 py-2">
                    <div className="flex items-center gap-1.5">
                      <button
                        onClick={() => startNodes([id])}
                        disabled={busy}
                        className="inline-flex h-8 w-8 items-center justify-center rounded-lg border border-slate-200 bg-white disabled:opacity-50"
                        title="启动"
                      >
                        <Play className="h-4 w-4" />
                      </button>
                      <button
                        onClick={() => stopNodes([id])}
                        disabled={busy}
                        className="inline-flex h-8 w-8 items-center justify-center rounded-lg border border-slate-200 bg-white disabled:opacity-50"
                        title="暂停"
                      >
                        <Pause className="h-4 w-4" />
                      </button>
                      <button
                        onClick={() => deleteNodes([id])}
                        disabled={busy}
                        className="inline-flex h-8 w-8 items-center justify-center rounded-lg border border-slate-200 bg-white disabled:opacity-50"
                        title="删除"
                      >
                        <Trash2 className="h-4 w-4" />
                      </button>
                      <button
                        onClick={() => setDetailSat(sat)}
                        className="inline-flex h-8 w-8 items-center justify-center rounded-lg border border-slate-200 bg-white"
                        title="详情"
                      >
                        <Eye className="h-4 w-4" />
                      </button>
                    </div>
                  </td>
                </tr>
              )
            })}
            {pageRows.length === 0 && (
              <tr>
                <td colSpan={19} className="py-10 text-center text-slate-500">暂无卫星节点数据</td>
              </tr>
            )}
          </tbody>
        </table>
      </div>

      <div className="mt-3 flex items-center gap-2 text-sm text-black">
        <span>总计 {filtered.length} 节点</span>
        <span>当前页 {safePage}/{totalPages}</span>
        <button
          disabled={safePage <= 1}
          onClick={() => setPage((p) => Math.max(1, p - 1))}
          className="h-9 rounded-xl border border-slate-200 bg-white px-3 disabled:opacity-50"
        >
          上一页
        </button>
        <button
          disabled={safePage >= totalPages}
          onClick={() => setPage((p) => Math.min(totalPages, p + 1))}
          className="h-9 rounded-xl border border-slate-200 bg-white px-3 disabled:opacity-50"
        >
          下一页
        </button>
      </div>

      {detailSat && (
        <div className="fixed inset-0 z-[130] bg-black/20">
          <div className="absolute right-0 top-0 h-full w-[min(800px,95vw)] border-l border-slate-200 bg-white p-4">
            <div className="mb-2 flex items-center justify-between">
              <div className="text-lg font-semibold text-black">节点详情：{String(detailSat?.id ?? '-')}</div>
              <button
                onClick={() => setDetailSat(null)}
                className="rounded-xl border border-slate-200 bg-white px-3 py-1.5 text-sm text-black"
              >
                关闭
              </button>
            </div>
            <pre className="h-[calc(100%-56px)] overflow-auto rounded-xl border border-slate-200 bg-white p-3 text-xs leading-6 text-black">
              {JSON.stringify(detailSat, null, 2)}
            </pre>
          </div>
        </div>
      )}
    </div>
  )
}
