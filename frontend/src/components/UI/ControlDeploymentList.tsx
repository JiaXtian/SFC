import { useState } from 'react'
import { Trash2, RefreshCw, Power, Loader2 } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'
import { resolveSfcLabel } from '@/utils/sfcLabel'

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
  const [runtimeToggling, setRuntimeToggling] = useState<Set<string>>(new Set())
  const {
    deployments,
    setDeployments,
    updateDeployment,
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

  const markRuntimeToggling = (id: string, active: boolean) => {
    setRuntimeToggling((prev) => {
      const next = new Set(prev)
      if (active) next.add(id)
      else next.delete(id)
      return next
    })
  }

  const toggleRuntime = async (dep: any, enabled: boolean) => {
    if (!canManage) {
      openSystemPopup('无权限操作', '普通用户仅可查看部署信息，不允许启停核心网容器。', 'warning')
      return
    }
    const target = String(dep?.backend_deployment_id ?? dep?.deployment_id ?? '')
    if (!target) {
      addToast('无法识别核心网部署ID', 'error')
      return
    }
    markRuntimeToggling(target, true)
    try {
      const res = await apiClient.setDeploymentRuntime(target, enabled)
      if (res?.deployment) updateDeployment(target, res.deployment as any)
      addToast(enabled ? '已提交 Open5GS 网元拉起任务' : '已停止并清理该核心网网元容器', 'success')
      await refresh()
    } catch (e: any) {
      addToast(`${enabled ? '启动' : '停止'}核心网运行态失败: ${e?.message ?? e}`, 'error')
    } finally {
      markRuntimeToggling(target, false)
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
          <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold">已部署核心网详情</div>
          <div className="text-[10px] text-slate-400 mt-0.5">展示运行态、SLA参数与路径重算结果</div>
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
              <th className="px-2 py-2 text-left min-w-[90px]">核心网</th>
              <th className="px-2 py-2 text-left">状态</th>
              <th className="px-2 py-2 text-left min-w-[92px]">Open5GS</th>
              <th className="px-2 py-2 text-left min-w-[150px]">网元/依赖</th>
              <th className="px-2 py-2 text-left">节点</th>
              <th className="px-2 py-2 text-left">容器启动</th>
              <th className="px-2 py-2 text-left">网元运行</th>
              <th className="px-2 py-2 text-left min-w-[120px]">服务状态</th>
              <th className="px-2 py-2 text-left">评分</th>
              <th className="px-2 py-2 text-left">可靠性</th>
              <th className="px-2 py-2 text-left">带宽瓶颈</th>
              <th className="px-2 py-2 text-left min-w-[95px]">路径重算次数</th>
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
              const cTotal = Number(dep?.containers_total ?? 0)
              const cRunning = Number(dep?.containers_running ?? 0)
              const cFailed = Number(dep?.containers_failed ?? 0)
              const nTotal = Number(dep?.core_nfs_total ?? 0)
              const nRunning = Number(dep?.core_nfs_running ?? 0)
              const nFailed = Number(dep?.core_nfs_failed ?? 0)
              const serviceReady = Boolean(dep?.service_ready ?? false)
              const readyForUe = Boolean(dep?.ready_for_ueransim ?? false)
              const explicitRuntime = typeof dep?.runtime_enabled === 'boolean' ? Boolean(dep.runtime_enabled) : null
              const runtimeEnabled = explicitRuntime ?? (serviceReady || cRunning > 0 || nRunning > 0)
              const runtimeKey = String(dep?.backend_deployment_id ?? dep?.deployment_id ?? '')
              const runtimeBusy = runtimeToggling.has(runtimeKey)
              const runtimeDisabled = !canManage || runtimeBusy || String(dep?.status ?? '') === 'failed' || String(dep?.status ?? '') === 'rolled_back'
              const score = Number(dep?.score_total ?? 0)
              const reliability = Number(dep?.estimated_reliability ?? 0)
              const bottleneck = Number(dep?.bottleneck_bandwidth_gbps ?? 0)
              const recompute = Number(dep?.path_recompute_count ?? 0)
              const updatedAt = String(dep?.last_update_at ?? dep?.deployed_at ?? '-').replace('T', ' ').slice(0, 19)
              return (
                <tr key={String(dep?.deployment_id ?? Math.random())} className="border-t border-slate-800/80 text-slate-200">
                  <td className="px-2 py-2 font-medium">{label}</td>
                  <td className="px-2 py-2">{statusLabel(String(dep?.status ?? 'completed'))}</td>
                  <td className="px-2 py-2">
                    <button
                      type="button"
                      onClick={() => toggleRuntime(dep, !runtimeEnabled)}
                      disabled={runtimeDisabled}
                      title={runtimeEnabled ? '关闭并删除该核心网 Open5GS 容器' : '拉起该核心网 Open5GS 网元'}
                      className={`relative h-6 w-[54px] rounded-full border transition inline-flex items-center ${
                        runtimeEnabled
                          ? 'bg-emerald-500/20 border-emerald-300/45'
                          : 'bg-slate-800/75 border-slate-600/70'
                      } ${runtimeDisabled ? 'opacity-60 cursor-not-allowed' : 'hover:border-cyan-300/60'}`}
                    >
                      <span
                        className={`absolute top-0.5 h-5 w-5 rounded-full inline-flex items-center justify-center transition ${
                          runtimeEnabled
                            ? 'left-[29px] bg-emerald-300 text-slate-950'
                            : 'left-0.5 bg-slate-500 text-slate-950'
                        }`}
                      >
                        {runtimeBusy ? <Loader2 className="w-3 h-3 animate-spin" /> : <Power className="w-3 h-3" />}
                      </span>
                      <span className={`absolute top-1/2 -translate-y-1/2 text-[9px] font-semibold ${runtimeEnabled ? 'left-2 text-emerald-100' : 'right-2 text-slate-400'}`}>
                        {runtimeEnabled ? 'ON' : 'OFF'}
                      </span>
                    </button>
                  </td>
                  <td className="px-2 py-2 font-mono">
                    <span>{Array.isArray(dep?.per_vnf) ? dep.per_vnf.length : nTotal || 0}/12 NF</span>
                    <span className="text-slate-500 mx-1">·</span>
                    <span>{Array.isArray(dep?.core_nf_dependencies) ? dep.core_nf_dependencies.length : 20} edges</span>
                  </td>
                  <td className="px-2 py-2">{Array.isArray(dep?.deployed_nodes) ? dep.deployed_nodes.length : 0}</td>
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
                  <td className="px-2 py-2 font-mono text-violet-200">{recompute}</td>
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
                <td colSpan={14} className="px-3 py-8 text-center text-slate-500">当前无已部署核心网</td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}
