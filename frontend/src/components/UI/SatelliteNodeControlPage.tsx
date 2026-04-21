import { useMemo, useState } from 'react'
import { Copy, Trash2, Database, Satellite as SatelliteIcon } from 'lucide-react'
import { apiClient } from '@/api/client'
import ConstellationControlPanel from './ConstellationControlPanel'
import { useStore } from '@/store/useStore'

type Role = 'admin' | 'user'

export default function SatelliteNodeControlPage({ role }: { role: Role }) {
  const canOperate = role === 'admin'
  const [deletingId, setDeletingId] = useState<string | null>(null)
  const {
    satellites,
    deployments,
    applyTopologySnapshot,
    openSystemPopup,
    addToast,
    setDeployments,
  } = useStore()

  const vnfCountByNode = useMemo(() => {
    const map = new Map<string, number>()
    deployments.forEach((dep: any) => {
      const per = Array.isArray(dep?.per_core_nf) ? dep.per_core_nf : (Array.isArray(dep?.per_vnf) ? dep.per_vnf : [])
      per.forEach((p: any) => {
        const node = String(p?.node ?? '')
        if (!node) return
        map.set(node, (map.get(node) ?? 0) + 1)
      })
    })
    return map
  }, [deployments])

  const copySatellite = async (sat: any) => {
    try {
      await navigator.clipboard.writeText(JSON.stringify(sat, null, 2))
      addToast(`已复制 ${sat.id} 参数`, 'success')
    } catch {
      addToast('复制失败，请检查浏览器权限', 'error')
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
      const [topo, depList] = await Promise.all([
        apiClient.getTopology(),
        apiClient.getDeployments(),
      ])
      applyTopologySnapshot(topo)
      if (Array.isArray(depList)) {
        setDeployments(depList as any)
      }
      addToast(`已删除卫星 ${satId}`, 'success')
    } catch (e: any) {
      addToast(`删除失败: ${e?.message ?? e}`, 'error')
    } finally {
      setDeletingId(null)
    }
  }

  return (
    <div className="h-full grid grid-cols-12 gap-3 overflow-hidden">
      <div className="col-span-12 xl:col-span-3 h-full overflow-hidden">
        <ConstellationControlPanel
          readonly={!canOperate}
          onUnauthorized={() => openSystemPopup('无权限操作', '普通用户不允许生成或导入星座。', 'warning')}
        />
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
          <div className="text-[11px] text-slate-400 inline-flex items-center gap-1.5">
            <SatelliteIcon className="w-3.5 h-3.5" />
            共 {satellites.length} 颗卫星
          </div>
        </div>

        <div className="flex-1 overflow-auto rounded-xl border border-slate-700/60 bg-slate-950/25">
          <table className="w-full text-[11px]">
            <thead className="sticky top-0 z-10 bg-slate-900/95 text-slate-300">
              <tr>
                <th className="px-2 py-2 text-left">卫星ID</th>
                <th className="px-2 py-2 text-left">状态</th>
                <th className="px-2 py-2 text-left">轨道</th>
                <th className="px-2 py-2 text-left">坐标</th>
                <th className="px-2 py-2 text-left">CPU</th>
                <th className="px-2 py-2 text-left">内存</th>
                <th className="px-2 py-2 text-left">磁盘</th>
                <th className="px-2 py-2 text-left">可靠性</th>
                <th className="px-2 py-2 text-left">网元数</th>
                <th className="px-2 py-2 text-left">操作</th>
              </tr>
            </thead>
            <tbody>
              {satellites.map((sat: any) => {
                const isDown = String(sat?.status ?? 'active') === 'down'
                return (
                  <tr key={sat.id} className="border-t border-slate-800/80 text-slate-200">
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
                    <td className="px-2 py-2">{vnfCountByNode.get(String(sat.id)) ?? 0}</td>
                    <td className="px-2 py-2">
                      <div className="inline-flex items-center gap-1">
                        <button
                          onClick={() => copySatellite(sat)}
                          className="h-7 w-7 rounded-md inline-flex items-center justify-center text-cyan-200 hover:bg-cyan-400/15"
                          title="复制参数"
                        >
                          <Copy className="w-3.5 h-3.5" />
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
              {satellites.length === 0 && (
                <tr>
                  <td colSpan={10} className="px-3 py-8 text-center text-slate-500">当前数据库暂无卫星节点</td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  )
}
