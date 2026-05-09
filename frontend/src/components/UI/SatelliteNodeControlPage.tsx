import { useEffect, useRef, useState } from 'react'
import { ArrowDownUp, CheckSquare, Database, Filter, Info, ListChecks, RefreshCw, Satellite as SatelliteIcon, Trash2, X } from 'lucide-react'
import { apiClient } from '@/api/client'
import ConstellationControlPanel from './ConstellationControlPanel'
import { useStore } from '@/store/useStore'

type Role = 'admin' | 'user'
type SortField =
  | 'id'
  | 'status'
  | 'plane'
  | 'cpu_available'
  | 'mem_available'
  | 'disk_available'
  | 'node_reliability'
  | 'vnf_count'
type SortOrder = 'asc' | 'desc'

export default function SatelliteNodeControlPage({ role }: { role: Role }) {
  const PAGE_SIZE = 50
  const canOperate = role === 'admin'
  const [deletingId, setDeletingId] = useState<string | null>(null)
  const [batchDeleting, setBatchDeleting] = useState(false)
  const [selectingAll, setSelectingAll] = useState(false)
  const [loading, setLoading] = useState(false)
  const [selectedIds, setSelectedIds] = useState<Set<string>>(new Set())
  const [keyword, setKeyword] = useState('')
  const [page, setPage] = useState(1)
  const [sortField, setSortField] = useState<SortField>('id')
  const [sortOrder, setSortOrder] = useState<SortOrder>('asc')
  const [rows, setRows] = useState<any[]>([])
  const [total, setTotal] = useState(0)
  const [totalPages, setTotalPages] = useState(1)
  const [samplingSec, setSamplingSec] = useState<number>(15)
  const [samplingSaving, setSamplingSaving] = useState(false)
  const [samplingRefreshing, setSamplingRefreshing] = useState(false)
  const [detailOpen, setDetailOpen] = useState(false)
  const [detailLoading, setDetailLoading] = useState(false)
  const [detailSat, setDetailSat] = useState<any | null>(null)

  const debounceRef = useRef<number | null>(null)
  const lastFetchKeyRef = useRef('')

  const {
    applyTopologySnapshot,
    openSystemPopup,
    addToast,
    setDeployments,
    setAutoDynamics,
    setSimulationStatus,
  } = useStore()

  const fetchPage = async (opts?: { silent?: boolean; forcePage?: number }) => {
    const targetPage = opts?.forcePage ?? page
    const reqKey = JSON.stringify({
      page: targetPage,
      pageSize: PAGE_SIZE,
      keyword,
      sortField,
      sortOrder,
    })
    if (!opts?.silent) setLoading(true)
    try {
      const resp = await apiClient.getSatellitesPage({
        page: targetPage,
        page_size: PAGE_SIZE,
        q: keyword.trim(),
        sort_by: sortField,
        sort_order: sortOrder,
      })

      if (reqKey !== lastFetchKeyRef.current && !opts?.silent) {
        lastFetchKeyRef.current = reqKey
      }

      const items = Array.isArray(resp?.items) ? resp.items : []
      const nextTotalPages = Math.max(1, Number(resp?.total_pages ?? 1))
      const nextPage = Math.max(1, Math.min(Number(resp?.page ?? targetPage), nextTotalPages))

      setRows(items)
      setTotal(Math.max(0, Number(resp?.total ?? 0)))
      setTotalPages(nextTotalPages)
      if (nextPage !== page) setPage(nextPage)
    } catch (e: any) {
      if (!opts?.silent) addToast(`卫星列表加载失败: ${e?.message ?? e}`, 'error')
    } finally {
      if (!opts?.silent) setLoading(false)
    }
  }

  useEffect(() => {
    if (debounceRef.current) window.clearTimeout(debounceRef.current)
    debounceRef.current = window.setTimeout(() => {
      fetchPage({ forcePage: 1 })
      setPage(1)
    }, 220)
    return () => {
      if (debounceRef.current) window.clearTimeout(debounceRef.current)
    }
  }, [keyword, sortField, sortOrder])

  useEffect(() => {
    fetchPage()
  }, [page])

  useEffect(() => {
    const timer = window.setInterval(() => {
      fetchPage({ silent: true })
    }, 5000)
    const onRefresh = () => { fetchPage({ silent: true }) }
    window.addEventListener('satellite-table-refresh', onRefresh)
    return () => {
      window.clearInterval(timer)
      window.removeEventListener('satellite-table-refresh', onRefresh)
    }
  }, [page, keyword, sortField, sortOrder])

  useEffect(() => {
    if (selectedIds.size === 0) return
    // Only clean impossible IDs after data mutation (delete/regen), keep cross-page selections.
    setSelectedIds((prev) => new Set(Array.from(prev)))
  }, [rows])

  const openSatelliteDetail = async (sat: any) => {
    const satId = String(sat?.id ?? '')
    if (!satId) return
    setDetailOpen(true)
    setDetailSat(sat)
    setDetailLoading(true)
    try {
      const detail = await apiClient.getSatellite(satId)
      setDetailSat({ ...sat, ...detail })
    } catch (e: any) {
      addToast(`卫星详情加载失败: ${e?.message ?? e}`, 'error')
    } finally {
      setDetailLoading(false)
    }
  }

  const reloadTopologyAndDeployments = async () => {
    const [topo, depList] = await Promise.all([
      apiClient.getTopology(),
      apiClient.getDeployments(),
    ])
    applyTopologySnapshot(topo)
    if (Array.isArray(depList)) {
      setDeployments(depList as any)
    }
  }

  const refreshSamplingConfig = async (silent = false) => {
    if (!silent) setSamplingRefreshing(true)
    try {
      const [cfg, status] = await Promise.all([
        apiClient.getControlConfig().catch(() => ({})),
        apiClient.getDynamicStatus().catch(() => ({})),
      ])
      const raw = Number(
        (cfg as any)?.resource_sampling_interval_sec
        ?? (cfg as any)?.control_config?.resource_sampling_interval_sec
        ?? (status as any)?.control_config?.resource_sampling_interval_sec
        ?? (status as any)?.sampling_interval_sec
        ?? samplingSec
      )
      const next = Math.max(10, Math.min(30, Number.isFinite(raw) ? raw : 15))
      setSamplingSec(next)
      setAutoDynamics({ resource_update_sec: next })
      setSimulationStatus({ sampling_interval_sec: next })
    } finally {
      if (!silent) setSamplingRefreshing(false)
    }
  }

  const saveSamplingConfig = async () => {
    setSamplingSaving(true)
    try {
      const next = Math.max(10, Math.min(30, Number(samplingSec || 15)))
      await apiClient.updateControlConfig({
        resource_sampling_interval_sec: next,
        apply_now: true,
      })
      setSamplingSec(next)
      setAutoDynamics({ resource_update_sec: next })
      setSimulationStatus({ sampling_interval_sec: next })
      addToast(`资源采样间隔已更新为 ${next}s`, 'success')
    } catch (e: any) {
      addToast(`采样配置保存失败: ${e?.message ?? e}`, 'error')
    } finally {
      setSamplingSaving(false)
    }
  }

  const deleteSatellite = async (satId: string) => {
    if (!canOperate) {
      openSystemPopup('无权限操作', '普通用户仅允许查看卫星节点控制页面，无法删除卫星。', 'warning')
      return
    }
    if (!window.confirm(`确认删除卫星 ${satId} 吗？删除后会清空已部署策略。`)) return
    setDeletingId(satId)
    try {
      await apiClient.deleteSatellite(satId)
      await reloadTopologyAndDeployments()
      setSelectedIds((prev) => {
        const next = new Set(prev)
        next.delete(satId)
        return next
      })
      await fetchPage({ forcePage: page, silent: true })
      addToast(`已删除卫星 ${satId}`, 'success')
    } catch (e: any) {
      addToast(`删除失败: ${e?.message ?? e}`, 'error')
    } finally {
      setDeletingId(null)
    }
  }

  const deleteBatchSatellites = async () => {
    if (!canOperate) {
      openSystemPopup('无权限操作', '普通用户仅允许查看卫星节点控制页面，无法删除卫星。', 'warning')
      return
    }
    const ids = Array.from(selectedIds)
    if (ids.length === 0) {
      addToast('请先选择至少一颗卫星', 'warning')
      return
    }
    if (!window.confirm(`确认批量删除 ${ids.length} 颗卫星吗？删除后会清空已部署策略。`)) return
    setBatchDeleting(true)
    let ok = 0
    const failed: string[] = []
    try {
      for (const id of ids) {
        try {
          await apiClient.deleteSatellite(id)
          ok += 1
        } catch {
          failed.push(id)
        }
      }
      await reloadTopologyAndDeployments()
      setSelectedIds(new Set())
      await fetchPage({ forcePage: page, silent: true })
      if (failed.length === 0) {
        addToast(`已删除 ${ok} 颗卫星`, 'success')
      } else {
        addToast(`已删除 ${ok} 颗，失败 ${failed.length} 颗`, 'warning')
      }
    } catch (e: any) {
      addToast(`批量删除失败: ${e?.message ?? e}`, 'error')
    } finally {
      setBatchDeleting(false)
    }
  }

  const toggleSelectAllSatellites = async () => {
    if (total > 0 && selectedIds.size >= total) {
      setSelectedIds(new Set())
      addToast('已取消全选', 'info')
      return
    }
    setSelectingAll(true)
    try {
      const all = new Set<string>()
      let targetPage = 1
      let totalPagesFromApi = 1
      do {
        const resp = await apiClient.getSatellitesPage({
          page: targetPage,
          page_size: 500,
          q: keyword.trim(),
          sort_by: 'id',
          sort_order: 'asc',
        })
        const items = Array.isArray(resp?.items) ? resp.items : []
        items.forEach((sat: any) => {
          const id = String(sat?.id ?? '').trim()
          if (id) all.add(id)
        })
        totalPagesFromApi = Math.max(1, Number(resp?.total_pages ?? 1))
        targetPage += 1
      } while (targetPage <= totalPagesFromApi)
      setSelectedIds(all)
      addToast(`已全选 ${all.size} 颗卫星`, 'success')
    } catch (e: any) {
      addToast(`全选失败: ${e?.message ?? e}`, 'error')
    } finally {
      setSelectingAll(false)
    }
  }

  const toggleSort = (field: SortField) => {
    if (sortField === field) {
      setSortOrder((prev) => (prev === 'asc' ? 'desc' : 'asc'))
      return
    }
    setSortField(field)
    setSortOrder('asc')
  }

  const allOnPageSelected = rows.length > 0 && rows.every((sat: any) => selectedIds.has(String(sat.id)))
  const allAcrossSelected = total > 0 && selectedIds.size >= total

  useEffect(() => {
    refreshSamplingConfig(true)
  }, [])

  return (
    <div className="relative h-full grid grid-cols-12 gap-3 overflow-hidden">
      <div className="col-span-12 xl:col-span-3 h-full overflow-y-auto pr-1 space-y-2.5">
        <ConstellationControlPanel
          readonly={!canOperate}
          onUnauthorized={() => openSystemPopup('无权限操作', '普通用户不允许生成或导入星座。', 'warning')}
        />
        <div
          className="rounded-2xl p-3"
          style={{
            background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
            border: '1px solid rgba(112,168,208,0.28)',
            backdropFilter: 'blur(14px)',
          }}
        >
          <div className="text-[12px] uppercase tracking-wide text-cyan-100 font-semibold inline-flex items-center gap-1.5">
            <ListChecks className="w-3.5 h-3.5 text-cyan-300" />
            系统资源采样配置
          </div>
          <div className="mt-1 text-[10px] text-slate-400">控制 CPU/内存/磁盘与核心网业务负载采样周期（10~30秒）。</div>
          <div className="mt-2 flex items-end gap-2">
            <label className="flex-1">
              <div className="text-[10px] text-slate-400 mb-1">采样间隔（秒）</div>
              <input
                type="number"
                min={10}
                max={30}
                step={1}
                value={samplingSec}
                onChange={(e) => setSamplingSec(Math.max(10, Math.min(30, Number(e.target.value) || 15)))}
                className="w-full h-8 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-cyan-100"
              />
            </label>
            <button
              onClick={saveSamplingConfig}
              disabled={samplingSaving}
              className="h-8 px-2.5 rounded-lg text-[12px] text-cyan-100 bg-cyan-500/15 border border-cyan-500/35 disabled:opacity-60"
            >
              {samplingSaving ? '保存中...' : '保存'}
            </button>
            <button
              onClick={() => refreshSamplingConfig(false)}
              disabled={samplingRefreshing}
              className="h-8 px-2 rounded-lg text-[12px] text-slate-200 bg-slate-800/60 border border-slate-700/70 disabled:opacity-60"
              title="刷新采样配置"
            >
              <RefreshCw className={`w-3.5 h-3.5 ${samplingRefreshing ? 'animate-spin' : ''}`} />
            </button>
          </div>
        </div>
      </div>

      <div
        className="col-span-12 xl:col-span-9 h-full rounded-2xl p-3.5 flex flex-col overflow-hidden"
        style={{
          background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
          border: '1px solid rgba(112,168,208,0.28)',
          backdropFilter: 'blur(14px)',
        }}
      >
        <div className="flex items-center justify-between mb-2.5">
          <div className="text-[14px] uppercase tracking-wide text-cyan-100 font-semibold inline-flex items-center gap-1.5">
            <Database className="w-4 h-4 text-cyan-300" />
            卫星节点参数与状态表
          </div>
          <div className="text-[11px] text-slate-400 inline-flex items-center gap-2">
            <SatelliteIcon className="w-3.5 h-3.5" />
            总计 {total} 颗
            <span className="text-slate-500">|</span>
            当前页 {rows.length} 颗
            <span className="text-slate-500">|</span>
            已选 {selectedIds.size} 颗
          </div>
        </div>

        <div className="mb-2.5 rounded-xl border border-slate-700/60 bg-slate-900/25 p-2.5">
          <div className="flex flex-wrap items-end gap-2">
            <label className="min-w-[220px] flex-1">
              <div className="text-[10px] text-slate-400 mb-1 inline-flex items-center gap-1"><Filter className="w-3 h-3" />关键词</div>
              <input
                value={keyword}
                onChange={(e) => setKeyword(e.target.value)}
                placeholder="卫星ID / 故障标签 / 轨道面（输入关键词匹配）"
                className="w-full h-8 px-2.5 rounded-lg bg-slate-900/60 border border-slate-700/70 text-[12px] text-cyan-100"
              />
            </label>
            <button
              onClick={() => fetchPage()}
              disabled={loading}
              className="h-8 px-2.5 rounded-lg text-[12px] text-cyan-100 bg-slate-700/25 border border-slate-500/35 inline-flex items-center gap-1.5"
            >
              <RefreshCw className={`w-3.5 h-3.5 ${loading ? 'animate-spin' : ''}`} />
              刷新
            </button>
            <button
              onClick={toggleSelectAllSatellites}
              disabled={selectingAll || total === 0}
              className="h-8 px-2.5 rounded-lg text-[12px] text-sky-100 bg-sky-500/15 border border-sky-500/35 inline-flex items-center gap-1.5 disabled:opacity-60"
            >
              <CheckSquare className="w-3.5 h-3.5" />
              {selectingAll ? '全选中...' : allAcrossSelected ? '取消全选' : '全选所有卫星'}
            </button>
            <button
              onClick={deleteBatchSatellites}
              disabled={batchDeleting}
              className="h-8 px-2.5 rounded-lg text-[12px] text-rose-100 bg-rose-500/15 border border-rose-500/35 inline-flex items-center gap-1.5 disabled:opacity-60"
            >
              <Trash2 className="w-3.5 h-3.5" />
              {batchDeleting ? '批量删除中...' : '批量删除'}
            </button>
            <button
              onClick={() => setSelectedIds(new Set())}
              className="h-8 px-2.5 rounded-lg text-[12px] text-slate-200 bg-slate-800/60 border border-slate-700/70"
            >
              清空选择
            </button>
          </div>
        </div>

        <div className="flex-1 overflow-auto rounded-xl border border-slate-700/60 bg-slate-950/25">
          <table className="w-full text-[11px]">
            <thead className="sticky top-0 z-10 bg-slate-900/95 text-slate-300">
              <tr>
                <th className="px-2 py-2 text-left w-[34px]">
                  <button
                    className="inline-flex items-center justify-center h-5 w-5 rounded text-cyan-200 hover:bg-cyan-500/15"
                    onClick={() => {
                      if (allOnPageSelected) {
                        setSelectedIds((prev) => {
                          const next = new Set(prev)
                          rows.forEach((sat: any) => next.delete(String(sat.id)))
                          return next
                        })
                      } else {
                        setSelectedIds((prev) => {
                          const next = new Set(prev)
                          rows.forEach((sat: any) => next.add(String(sat.id)))
                          return next
                        })
                      }
                    }}
                    title={allOnPageSelected ? '取消全选本页' : '全选本页'}
                  >
                    <CheckSquare className="w-3.5 h-3.5" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('id')}>
                    卫星ID<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('status')}>
                    状态<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('plane')}>
                    轨道<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">坐标</th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('cpu_available')}>
                    CPU<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('mem_available')}>
                    内存<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('disk_available')}>
                    磁盘<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('node_reliability')}>
                    可靠性<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">
                  容器状态
                </th>
                <th className="px-2 py-2 text-left">
                  <button className="inline-flex items-center gap-1 hover:text-cyan-200" onClick={() => toggleSort('vnf_count')}>
                    运行网元<ArrowDownUp className="w-3 h-3" />
                  </button>
                </th>
                <th className="px-2 py-2 text-left">服务探测</th>
                <th className="px-2 py-2 text-left">业务负载</th>
                <th className="px-2 py-2 text-left">已部署核心网</th>
                <th className="px-2 py-2 text-left">核心网网元类型</th>
                <th className="px-2 py-2 text-left">操作</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((sat: any) => {
                const isDown = String(sat?.status ?? 'active') === 'down'
                const satId = String(sat?.id ?? '')
                const checked = selectedIds.has(satId)
                const sfcNames = Array.isArray(sat?.deployed_sfc_names) ? sat.deployed_sfc_names.map((x: any) => String(x)) : []
                const nfTypes = Array.isArray(sat?.deployed_core_nf_types)
                  ? sat.deployed_core_nf_types.map((x: any) => String(x))
                  : (Array.isArray(sat?.running_core_nf_types) ? sat.running_core_nf_types.map((x: any) => String(x)) : [])
                const containerState = String(sat?.container_state ?? 'stopped')
                const runningCoreNfCount = Number(sat?.running_core_nf_count ?? sat?.deployed_vnf_count ?? nfTypes.length ?? 0)
                const serviceProbeOk = Boolean(sat?.service_probe_ok ?? false)
                const coreBusinessLoadRaw = Number(sat?.core_network_load ?? sat?.core_business_load?.load_index ?? 0)
                const coreBusinessLoad = Number.isFinite(coreBusinessLoadRaw) ? coreBusinessLoadRaw : 0
                return (
                  <tr key={sat.id} className="border-t border-slate-800/80 text-slate-200">
                    <td className="px-2 py-2">
                      <input
                        type="checkbox"
                        checked={checked}
                        onChange={(e) => {
                          setSelectedIds((prev) => {
                            const next = new Set(prev)
                            if (e.target.checked) next.add(satId)
                            else next.delete(satId)
                            return next
                          })
                        }}
                      />
                    </td>
                    <td className="px-2 py-2 font-mono">{sat.id}</td>
                    <td className="px-2 py-2">
                      <span className={`px-1.5 py-0.5 rounded ${isDown ? 'text-rose-200 bg-rose-500/20' : 'text-emerald-200 bg-emerald-500/20'}`}>
                        {isDown ? `故障 ${sat?.fault_tag ? `(${sat.fault_tag})` : ''}` : '正常'}
                      </span>
                    </td>
                    <td className="px-2 py-2">
                      P{sat?.orbital_params?.plane ?? 0}/#{sat?.orbital_params?.position_in_plane ?? 0}
                      <div className="text-[10px] text-slate-400">{Number(sat?.orbital_params?.altitude_km ?? 0).toFixed(0)}km</div>
                    </td>
                    <td className="px-2 py-2">
                      <div>{Number(sat?.coordinates?.lat ?? 0).toFixed(2)}°, {Number(sat?.coordinates?.lon ?? 0).toFixed(2)}°</div>
                    </td>
                    <td className="px-2 py-2 font-mono">{Number(sat?.cpu_available ?? 0).toFixed(1)} / {Number(sat?.cpu_total ?? 0).toFixed(1)}</td>
                    <td className="px-2 py-2 font-mono">{Number(sat?.mem_available ?? 0).toFixed(1)} / {Number(sat?.mem_total ?? 0).toFixed(1)}</td>
                    <td className="px-2 py-2 font-mono">{Number(sat?.disk_available ?? 0).toFixed(1)} / {Number(sat?.disk_total ?? 0).toFixed(1)}</td>
                    <td className="px-2 py-2 font-mono">{(Number(sat?.node_reliability ?? 0) * 100).toFixed(2)}%</td>
                    <td className="px-2 py-2">
                      <span
                        className={`px-1.5 py-0.5 rounded ${
                          containerState === 'running'
                            ? 'text-emerald-200 bg-emerald-500/20'
                            : (containerState === 'starting'
                              ? 'text-amber-200 bg-amber-500/20'
                              : (containerState === 'failed'
                                ? 'text-rose-200 bg-rose-500/20'
                                : 'text-slate-300 bg-slate-700/40'))
                        }`}
                      >
                        {containerState}
                      </span>
                    </td>
                    <td className="px-2 py-2">{runningCoreNfCount}</td>
                    <td className="px-2 py-2">
                      <span className={`px-1.5 py-0.5 rounded ${serviceProbeOk ? 'text-emerald-200 bg-emerald-500/20' : 'text-amber-200 bg-amber-500/20'}`}>
                        {serviceProbeOk ? 'OK' : 'Pending'}
                      </span>
                    </td>
                    <td className="px-2 py-2 font-mono">{(coreBusinessLoad * 100).toFixed(1)}%</td>
                    <td className="px-2 py-2">
                      {sfcNames.length > 0 ? (
                        <div className="flex flex-wrap gap-1">
                          {sfcNames.slice(0, 2).map((name: string) => (
                            <span key={name} className="px-1.5 py-0.5 rounded bg-cyan-500/15 text-cyan-200">{name}</span>
                          ))}
                          {sfcNames.length > 2 && (
                            <span className="px-1.5 py-0.5 rounded bg-slate-700/40 text-slate-300">+{sfcNames.length - 2}</span>
                          )}
                        </div>
                      ) : (
                        <span className="text-slate-500">-</span>
                      )}
                    </td>
                    <td className="px-2 py-2">
                      {nfTypes.length > 0 ? (
                        <div className="flex flex-wrap gap-1">
                          {nfTypes.slice(0, 3).map((name: string) => (
                            <span key={name} className="px-1.5 py-0.5 rounded bg-emerald-500/15 text-emerald-200">{name}</span>
                          ))}
                          {nfTypes.length > 3 && (
                            <span className="px-1.5 py-0.5 rounded bg-slate-700/40 text-slate-300">+{nfTypes.length - 3}</span>
                          )}
                        </div>
                      ) : (
                        <span className="text-slate-500">-</span>
                      )}
                    </td>
                    <td className="px-2 py-2">
                      <div className="inline-flex items-center gap-1">
                        <button
                          onClick={() => openSatelliteDetail(sat)}
                          className="h-7 w-7 rounded-md inline-flex items-center justify-center text-cyan-200 hover:bg-cyan-400/15"
                          title="卫星详情"
                        >
                          <Info className="w-3.5 h-3.5" />
                        </button>
                        <button
                          onClick={() => deleteSatellite(String(sat.id))}
                          className="h-7 w-7 rounded-md inline-flex items-center justify-center text-rose-200 hover:bg-rose-400/15"
                          title="删除卫星"
                          disabled={deletingId === sat.id}
                        >
                          <Trash2 className="w-3.5 h-3.5" />
                        </button>
                      </div>
                    </td>
                  </tr>
                )
              })}
              {rows.length === 0 && (
                <tr>
                  <td colSpan={16} className="px-3 py-8 text-center text-slate-500">当前筛选条件下无卫星节点</td>
                </tr>
              )}
            </tbody>
          </table>
        </div>

        <div className="mt-2.5 flex items-center justify-between text-[11px] text-slate-400">
          <div>
            第 {Math.min(page, totalPages)} / {totalPages} 页
          </div>
          <div className="inline-flex items-center gap-2">
            <button
              className="h-7 px-2.5 rounded-md border border-slate-700/70 bg-slate-900/45 disabled:opacity-50"
              disabled={page <= 1}
              onClick={() => setPage((p) => Math.max(1, p - 1))}
            >
              上一页
            </button>
            <button
              className="h-7 px-2.5 rounded-md border border-slate-700/70 bg-slate-900/45 disabled:opacity-50"
              disabled={page >= totalPages}
              onClick={() => setPage((p) => Math.min(totalPages, p + 1))}
            >
              下一页
            </button>
          </div>
        </div>
      </div>

      {detailOpen && (
        <div
          className="absolute right-0 top-0 bottom-0 z-30 w-[380px] max-w-[92vw] overflow-hidden rounded-l-2xl border-l border-cyan-400/25 bg-slate-950/95 shadow-2xl"
          style={{ backdropFilter: 'blur(18px)' }}
        >
          <div className="flex items-center justify-between border-b border-slate-800/80 px-4 py-3">
            <div>
              <div className="text-[13px] font-semibold text-cyan-100">卫星完整详情</div>
              <div className="text-[11px] font-mono text-slate-400">{detailSat?.id ?? '-'}</div>
            </div>
            <button
              onClick={() => setDetailOpen(false)}
              className="h-8 w-8 rounded-lg inline-flex items-center justify-center text-slate-300 hover:bg-white/10"
              title="关闭"
            >
              <X className="w-4 h-4" />
            </button>
          </div>
          <SatelliteDetailDrawer sat={detailSat} loading={detailLoading} />
        </div>
      )}
    </div>
  )
}

function fmtNum(v: any, digits = 3, suffix = '') {
  const n = Number(v)
  return Number.isFinite(n) ? `${n.toFixed(digits)}${suffix}` : '-'
}

function DetailSection({ title, rows }: { title: string; rows: Array<[string, any]> }) {
  return (
    <div className="rounded-xl border border-slate-800/80 bg-slate-900/35 p-3">
      <div className="mb-2 text-[11px] font-semibold text-cyan-100">{title}</div>
      <div className="space-y-1.5">
        {rows.map(([k, v]) => (
          <div key={k} className="flex items-start justify-between gap-3 text-[11px]">
            <span className="text-slate-500">{k}</span>
            <span className="max-w-[220px] break-words text-right font-mono text-slate-200">{v ?? '-'}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

function SatelliteDetailDrawer({ sat, loading }: { sat: any | null; loading: boolean }) {
  const op = sat?.orbital_params ?? {}
  const c = sat?.coordinates ?? {}
  const core = sat?.core_business_load ?? {}
  const nfTypes = Array.isArray(sat?.running_core_nf_types) ? sat.running_core_nf_types.join(', ') : ''
  return (
    <div className="h-[calc(100%-57px)] overflow-y-auto p-3 space-y-3">
      {loading && <div className="text-[11px] text-cyan-200">正在刷新详情...</div>}
      <DetailSection title="节点状态" rows={[
        ['ID', sat?.id],
        ['类型', sat?.type ?? 'satellite'],
        ['状态', sat?.status ?? 'active'],
        ['故障标签', sat?.fault_tag || '无'],
        ['容器', sat?.container_state ?? 'stopped'],
        ['服务探测', sat?.service_probe_ok ? 'OK' : 'Pending'],
        ['运行网元', nfTypes || '无'],
        ['可靠性', fmtNum(Number(sat?.node_reliability ?? 0) * 100, 2, '%')],
      ]} />
      <DetailSection title="SGP4 / TLE 轨道根数" rows={[
        ['传播模型', op.propagation_model ?? 'SGP4'],
        ['Epoch ISO', op.epoch_iso],
        ['Epoch JD', fmtNum(op.epoch_jd, 8)],
        ['Mean motion', fmtNum(op.mean_motion_rev_per_day, 8, ' rev/day')],
        ['Mean anomaly', fmtNum(op.mean_anomaly_deg, 4, ' deg')],
        ['Eccentricity', fmtNum(op.eccentricity, 7)],
        ['Inclination', fmtNum(op.inclination_deg ?? op.inclination, 4, ' deg')],
        ['RAAN', fmtNum(op.raan, 4, ' deg')],
        ['Arg perigee', fmtNum(op.argument_of_perigee_deg, 4, ' deg')],
        ['BSTAR', fmtNum(op.bstar, 7)],
        ['Semi-major axis', fmtNum(op.semi_major_axis_km, 3, ' km')],
        ['Period', fmtNum(op.period_minutes, 3, ' min')],
        ['Propagation', fmtNum(op.propagation_minutes, 2, ' min')],
        ['TLE line 1', op.tle_line1 || '-'],
        ['TLE line 2', op.tle_line2 || '-'],
      ]} />
      <DetailSection title="当前位置" rows={[
        ['Latitude', fmtNum(c.lat, 4, ' deg')],
        ['Longitude', fmtNum(c.lon, 4, ' deg')],
        ['X', fmtNum(c.x, 3, ' km')],
        ['Y', fmtNum(c.y, 3, ' km')],
        ['Z', fmtNum(c.z, 3, ' km')],
        ['Altitude', fmtNum(op.altitude_km, 3, ' km')],
        ['True anomaly', fmtNum(op.true_anomaly, 4, ' deg')],
      ]} />
      <DetailSection title="资源与业务负载" rows={[
        ['CPU', `${fmtNum(sat?.cpu_available, 2)} / ${fmtNum(sat?.cpu_total, 2)}`],
        ['MEM', `${fmtNum(sat?.mem_available, 2)} / ${fmtNum(sat?.mem_total, 2)} GB`],
        ['DISK', `${fmtNum(sat?.disk_available, 2)} / ${fmtNum(sat?.disk_total, 2)} GB`],
        ['Core load', fmtNum(Number(sat?.core_network_load ?? core.load_index ?? 0) * 100, 2, '%')],
        ['Signaling', fmtNum(Number(core.signaling_load ?? 0) * 100, 2, '%')],
        ['Session', fmtNum(Number(core.session_load ?? 0) * 100, 2, '%')],
        ['User plane', fmtNum(Number(core.user_plane_load ?? 0) * 100, 2, '%')],
        ['Mobility', fmtNum(Number(core.mobility_load ?? 0) * 100, 2, '%')],
        ['Policy', fmtNum(Number(core.policy_load ?? 0) * 100, 2, '%')],
        ['Auth', fmtNum(Number(core.auth_load ?? 0) * 100, 2, '%')],
      ]} />
      <div className="rounded-xl border border-slate-800/80 bg-slate-900/35 p-3">
        <div className="mb-2 text-[11px] font-semibold text-cyan-100">完整原始参数</div>
        <pre className="max-h-72 overflow-auto whitespace-pre-wrap break-words rounded-lg bg-slate-950/70 p-2 text-[10px] leading-relaxed text-slate-300">
          {JSON.stringify(sat ?? {}, null, 2)}
        </pre>
      </div>
    </div>
  )
}
