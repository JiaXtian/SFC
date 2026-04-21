import { Trash2, RefreshCw } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'
import { resolveSfcLabel } from '@/utils/sfcLabel'

export default function ControlDeploymentList({ canManage }: { canManage: boolean }) {
  const {
    deployments,
    setDeployments,
    removeDeployment,
    applyTopologySnapshot,
    addToast,
    openSystemPopup,
  } = useStore()

  const refresh = async () => {
    try {
      const [depList, topo] = await Promise.all([
        apiClient.getDeployments(),
        apiClient.getTopology(),
      ])
      if (Array.isArray(depList)) setDeployments(depList as any)
      applyTopologySnapshot(topo)
    } catch (e: any) {
      addToast(`刷新失败: ${e?.message ?? e}`, 'error')
    }
  }

  const rollback = async (dep: any) => {
    if (!canManage) {
      openSystemPopup('无权限操作', '普通用户仅可查看部署信息，不允许删除部署。', 'warning')
      return
    }
    const label = resolveSfcLabel(deployments as any, {
      deploymentId: String(dep?.deployment_id ?? ''),
      sessionId: String(dep?.session_id ?? ''),
      requestId: String(dep?.request_id ?? ''),
    })
    if (!window.confirm(`确认删除部署 ${label} 吗？`)) return
    try {
      const rollbackTarget = String(dep?.backend_deployment_id ?? dep?.deployment_id ?? '')
      await apiClient.rollbackDeployment(rollbackTarget)
      removeDeployment(String(dep?.deployment_id ?? rollbackTarget))
      addToast(`已删除部署 ${label}`, 'success')
      const topo = await apiClient.getTopology()
      applyTopologySnapshot(topo)
    } catch (e: any) {
      addToast(`删除失败: ${e?.message ?? e}`, 'error')
    }
  }

  return (
    <div
      className="rounded-2xl p-3.5 h-full flex flex-col overflow-hidden"
      style={{
        background: 'linear-gradient(160deg, rgba(11,17,30,0.78), rgba(8,13,24,0.66))',
        border: '1px solid rgba(112,168,208,0.28)',
        backdropFilter: 'blur(14px)',
      }}
    >
      <div className="flex items-center justify-between mb-2">
        <div className="text-[14px] uppercase tracking-wide text-cyan-100 font-semibold">当前已部署 SFC（后台管理）</div>
        <button
          onClick={refresh}
          className="h-7 px-2.5 rounded-lg text-[11px] text-cyan-100 inline-flex items-center gap-1 border border-cyan-500/30 bg-cyan-500/10"
        >
          <RefreshCw className="w-3.5 h-3.5" />
          刷新
        </button>
      </div>

      <div className="flex-1 overflow-auto rounded-xl border border-slate-700/60 bg-slate-950/25">
        <table className="w-full text-[11px]">
          <thead className="sticky top-0 z-10 bg-slate-900/95 text-slate-300">
            <tr>
              <th className="px-2 py-2 text-left">SFC</th>
              <th className="px-2 py-2 text-left">状态</th>
              <th className="px-2 py-2 text-left">节点数</th>
              <th className="px-2 py-2 text-left">链路时延</th>
              <th className="px-2 py-2 text-left">部署时间</th>
              <th className="px-2 py-2 text-left">操作</th>
            </tr>
          </thead>
          <tbody>
            {deployments.map((dep: any) => {
              const label = resolveSfcLabel(deployments as any, {
                deploymentId: String(dep?.deployment_id ?? ''),
                sessionId: String(dep?.session_id ?? ''),
                requestId: String(dep?.request_id ?? ''),
              })
              return (
                <tr key={String(dep?.deployment_id ?? Math.random())} className="border-t border-slate-800/80 text-slate-200">
                  <td className="px-2 py-2 font-medium">{label}</td>
                  <td className="px-2 py-2">{String(dep?.status ?? 'completed')}</td>
                  <td className="px-2 py-2">{Array.isArray(dep?.deployed_nodes) ? dep.deployed_nodes.length : 0}</td>
                  <td className="px-2 py-2 font-mono">{Number(dep?.total_latency_ms ?? 0).toFixed(2)}ms</td>
                  <td className="px-2 py-2">{String(dep?.deployed_at ?? '-').replace('T', ' ').slice(0, 19)}</td>
                  <td className="px-2 py-2">
                    <button
                      onClick={() => rollback(dep)}
                      className="h-7 w-7 rounded-md inline-flex items-center justify-center text-rose-200 hover:bg-rose-400/15"
                      title="删除部署"
                    >
                      <Trash2 className="w-3.5 h-3.5" />
                    </button>
                  </td>
                </tr>
              )
            })}
            {deployments.length === 0 && (
              <tr>
                <td colSpan={6} className="px-3 py-8 text-center text-slate-500">当前无已部署 SFC</td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

