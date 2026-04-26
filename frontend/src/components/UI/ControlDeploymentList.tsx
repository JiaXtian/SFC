import { Trash2, RefreshCw } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'
import { resolveSfcLabel } from '@/utils/sfcLabel'

function phaseLabel(raw: string): string {
  const v = String(raw ?? '').trim().toLowerCase()
  const map: Record<string, string> = {
    queued: '排队中',
    pending: '等待中',
    starting: '启动中',
    running: '运行中',
    degraded: '降级运行',
    ready: '已就绪',
    rolled_back: '已回滚',
    failed: '失败',
  }
  return map[v] ?? (raw || '-')
}

function statusLabel(raw: string): string {
  const v = String(raw ?? '').trim().toLowerCase()
  const map: Record<string, string> = {
    completed: '已部署',
    running: '运行中',
    degraded: '降级',
    failed: '失败',
    rolled_back: '已回滚',
    'in-progress': '部署中',
    in_progress: '部署中',
  }
  return map[v] ?? (raw || '-')
}

export default function ControlDeploymentList({ canManage }: { canManage: boolean }) {
  const {
    deployments,
    setDeployments,
    removeDeployment,
    suppressSessionDeployment,
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
      const sessionId = String(dep?.session_id ?? '')
      if (sessionId) {
        try {
          await apiClient.stopSFCSession(sessionId)
        } catch {
          // ignore stop failure and continue rollback path
        }
        suppressSessionDeployment(sessionId)
      }
      const rollbackTarget = String(dep?.backend_deployment_id ?? dep?.deployment_id ?? '')
      await apiClient.rollbackDeployment(rollbackTarget)
      removeDeployment(String(dep?.deployment_id ?? rollbackTarget))
      if (rollbackTarget && rollbackTarget !== String(dep?.deployment_id ?? '')) {
        removeDeployment(rollbackTarget)
      }
      addToast(`已删除部署 ${label}`, 'success')
      await refresh()
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
      <div className="flex items-center justify-between mb-2 gap-2">
        <div>
          <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold">已部署SFC详情</div>
          <div className="text-[10px] text-slate-400 mt-0.5">展示运行态、SLA相关参数、编排进度及会话信息</div>
        </div>
        <button
          onClick={refresh}
          className="h-7 px-2.5 rounded-lg text-[11px] text-cyan-100 inline-flex items-center gap-1 border border-cyan-500/30 bg-cyan-500/10"
        >
          <RefreshCw className="w-3.5 h-3.5" />
          刷新
        </button>
      </div>

      <div className="flex-1 overflow-auto rounded-xl border border-slate-700/60 bg-slate-950/25">
        <table className="w-full text-[10px]">
          <thead className="sticky top-0 z-10 bg-slate-900/95 text-slate-300">
            <tr>
              <th className="px-2 py-2 text-left min-w-[90px]">SFC</th>
              <th className="px-2 py-2 text-left">状态</th>
              <th className="px-2 py-2 text-left min-w-[150px]">入口 → 出口</th>
              <th className="px-2 py-2 text-left">节点</th>
              <th className="px-2 py-2 text-left min-w-[95px]">编排阶段</th>
              <th className="px-2 py-2 text-left">容器启动</th>
              <th className="px-2 py-2 text-left">网元运行</th>
              <th className="px-2 py-2 text-left min-w-[120px]">服务状态</th>
              <th className="px-2 py-2 text-left">评分</th>
              <th className="px-2 py-2 text-left">可靠性</th>
              <th className="px-2 py-2 text-left">带宽瓶颈</th>
              <th className="px-2 py-2 text-left min-w-[105px]">会话/重算</th>
              <th className="px-2 py-2 text-left min-w-[80px]">触发</th>
              <th className="px-2 py-2 text-left">链路时延</th>
              <th className="px-2 py-2 text-left">最后更新</th>
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
              const phase = String(dep?.orchestration_phase ?? '-')
              const progress = Number(dep?.orchestration_progress ?? 0)
              const cTotal = Number(dep?.containers_total ?? 0)
              const cRunning = Number(dep?.containers_running ?? 0)
              const cFailed = Number(dep?.containers_failed ?? 0)
              const nTotal = Number(dep?.core_nfs_total ?? 0)
              const nRunning = Number(dep?.core_nfs_running ?? 0)
              const nFailed = Number(dep?.core_nfs_failed ?? 0)
              const serviceReady = Boolean(dep?.service_ready ?? false)
              const readyForUe = Boolean(dep?.ready_for_ueransim ?? false)
              const score = Number(dep?.score_total ?? 0)
              const reliability = Number(dep?.estimated_reliability ?? 0)
              const bottleneck = Number(dep?.bottleneck_bandwidth_gbps ?? 0)
              const recompute = Number(dep?.path_recompute_count ?? 0)
              const trigger = String(dep?.decision_trigger ?? dep?.orchestration_trigger ?? '-')
              const updatedAt = String(dep?.last_update_at ?? dep?.deployed_at ?? '-').replace('T', ' ').slice(0, 19)
              return (
                <tr key={String(dep?.deployment_id ?? Math.random())} className="border-t border-slate-800/80 text-slate-200">
                  <td className="px-2 py-2 font-medium">{label}</td>
                  <td className="px-2 py-2">{statusLabel(String(dep?.status ?? 'completed'))}</td>
                  <td className="px-2 py-2 font-mono">
                    <span>{String(dep?.source_node ?? '-')}</span>
                    <span className="text-slate-500 mx-1">→</span>
                    <span>{String(dep?.destination_node ?? '-')}</span>
                  </td>
                  <td className="px-2 py-2">{Array.isArray(dep?.deployed_nodes) ? dep.deployed_nodes.length : 0}</td>
                  <td className="px-2 py-2">
                    <div>{phaseLabel(phase)}</div>
                    <div className="text-[9px] text-slate-400">{progress}%</div>
                    {String(dep?.last_error ?? '').trim() && (
                      <div className="text-[9px] text-rose-300">{String(dep.last_error)}</div>
                    )}
                  </td>
                  <td className="px-2 py-2">
                    <div>{cRunning}/{cTotal}</div>
                    {cFailed > 0 && <div className="text-[9px] text-rose-300">失败 {cFailed}</div>}
                  </td>
                  <td className="px-2 py-2">
                    <div>{nRunning}/{nTotal}</div>
                    {nFailed > 0 && <div className="text-[9px] text-rose-300">失败 {nFailed}</div>}
                  </td>
                  <td className="px-2 py-2">
                    <span className={`px-1.5 py-0.5 rounded ${serviceReady ? 'text-emerald-200 bg-emerald-500/20' : 'text-amber-200 bg-amber-500/20'}`}>
                      {serviceReady ? '服务就绪' : '服务未就绪'}
                    </span>
                    <div className="mt-1">
                      <span className={`px-1.5 py-0.5 rounded ${readyForUe ? 'text-emerald-200 bg-emerald-500/20' : 'text-slate-300 bg-slate-700/40'}`}>
                        {readyForUe ? 'UE可验证' : 'UE未就绪'}
                      </span>
                    </div>
                  </td>
                  <td className="px-2 py-2 font-mono">{score.toFixed(3)}</td>
                  <td className="px-2 py-2 font-mono">{(reliability * 100).toFixed(2)}%</td>
                  <td className="px-2 py-2 font-mono">{bottleneck.toFixed(2)}Gbps</td>
                  <td className="px-2 py-2">
                    <div>{String(dep?.session_id ? '连续会话' : '单次部署')}</div>
                    <div className="text-[9px] text-slate-400">路径重算 {recompute} 次</div>
                  </td>
                  <td className="px-2 py-2 text-slate-300">{trigger || '-'}</td>
                  <td className="px-2 py-2 font-mono">{Number(dep?.total_latency_ms ?? 0).toFixed(2)}ms</td>
                  <td className="px-2 py-2">{updatedAt}</td>
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
                <td colSpan={16} className="px-3 py-8 text-center text-slate-500">当前无已部署 SFC</td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}
