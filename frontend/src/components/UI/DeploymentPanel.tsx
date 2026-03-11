import { useMemo, useState } from 'react'
import { Trash2, ChevronDown, ChevronUp, CheckCircle, Clock, XCircle, Eye, EyeOff, AlertTriangle } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore, type Deployment } from '@/store/useStore'

function buildPathNodes(dep: Deployment): string[] {
  if (Array.isArray(dep.path_nodes) && dep.path_nodes.length > 0) return dep.path_nodes
  if (!dep.link_details || dep.link_details.length === 0) {
    if (dep.source_node && dep.destination_node) return [dep.source_node, dep.destination_node]
    return dep.deployed_nodes || []
  }
  const nextMap = new Map<string, string>()
  const inDegree = new Map<string, number>()
  const outDegree = new Map<string, number>()
  dep.link_details.forEach((l: any) => {
    if (!l?.src || !l?.dst) return
    if (!nextMap.has(l.src)) nextMap.set(l.src, l.dst)
    outDegree.set(l.src, (outDegree.get(l.src) || 0) + 1)
    inDegree.set(l.dst, (inDegree.get(l.dst) || 0) + 1)
    if (!inDegree.has(l.src)) inDegree.set(l.src, inDegree.get(l.src) || 0)
    if (!outDegree.has(l.dst)) outDegree.set(l.dst, outDegree.get(l.dst) || 0)
  })
  let start = dep.source_node || ''
  if (!start) {
    start = [...outDegree.keys()].find(k => (inDegree.get(k) || 0) === 0 && (outDegree.get(k) || 0) > 0) || dep.link_details[0]?.src || ''
  }
  if (!start) return dep.deployed_nodes || []
  const nodes = [start]
  const seen = new Set<string>([start])
  for (let i = 0; i < dep.link_details.length + 2; i++) {
    const nxt = nextMap.get(nodes[nodes.length - 1])
    if (!nxt) break
    nodes.push(nxt)
    if (seen.has(nxt)) break
    seen.add(nxt)
  }
  if (dep.destination_node && nodes[nodes.length - 1] !== dep.destination_node) {
    nodes.push(dep.destination_node)
  }
  return nodes
}

const StatusIcon = ({ s }: { s: string }) =>
  s === 'completed' ? <CheckCircle className="w-3.5 h-3.5 text-green-400" />
  : s === 'failed'  ? <XCircle    className="w-3.5 h-3.5 text-red-400"   />
  :                   <Clock      className="w-3.5 h-3.5 text-yellow-400 animate-pulse" />

const StatusLabel: Record<string, [string, string]> = {
  completed: ['已部署', 'text-green-400'],
  'in-progress': ['部署中', 'text-yellow-400'],
  failed: ['失败', 'text-red-400'],
}

export default function DeploymentPanel() {
  const {
    deployments,
    removeDeployment,
    highlightedDeploymentIds,
    toggleHighlightedDeployment,
    setSatellites,
    setLinks,
    suppressSessionDeployment,
  } = useStore()
  const [expanded, setExpanded] = useState<string | null>(null)
  const [expandedLinks, setExpandedLinks] = useState<Record<string, boolean>>({})
  const [deleteConfirm, setDeleteConfirm] = useState<string | null>(null)
  const [rolling, setRolling]   = useState<string | null>(null)
  const sortedDeployments = useMemo(() => {
    return [...deployments].sort((a, b) => {
      const sa = Number(a.score_total ?? -1)
      const sb = Number(b.score_total ?? -1)
      if (sa >= 0 && sb >= 0 && sa !== sb) return sb - sa
      if (sa >= 0 && sb < 0) return -1
      if (sb >= 0 && sa < 0) return 1
      return (a.total_latency_ms ?? Number.POSITIVE_INFINITY) - (b.total_latency_ms ?? Number.POSITIVE_INFINITY)
    })
  }, [deployments])

  // 关键：点击眼睛图标切换高亮
  const toggleHighlight = (dep: Deployment) => {
    toggleHighlightedDeployment(dep.deployment_id)
  }

  const rollback = async (dep: Deployment) => {
    setRolling(dep.deployment_id)
    setDeleteConfirm(null)
    try {
      const rollbackTarget = dep.backend_deployment_id || dep.deployment_id
      await apiClient.rollbackDeployment(rollbackTarget)
      if ((dep as any).session_id) {
        try {
          await apiClient.stopSFCSession((dep as any).session_id)
        } catch {}
        suppressSessionDeployment((dep as any).session_id)
      }
      removeDeployment(dep.deployment_id)
      
      // 🔥 关键修改：刷新卫星资源状态
      try {
        const topo = await apiClient.getTopology()
        const topology = topo?.topology ?? topo
        if (Array.isArray(topology?.nodes)) setSatellites(topology.nodes)
        if (Array.isArray(topology?.links)) setLinks(topology.links)
        console.log('[资源释放] 已刷新卫星资源状态')
      } catch (e) {
        console.warn('[资源释放] 刷新失败:', e)
      }
    } catch (e: any) { 
      alert(`回滚失败: ${e.message ?? e}`) 
    }
    setRolling(null)
  }

  if (deployments.length === 0) return (
    <div className="flex-1 flex flex-col items-center justify-center text-gray-600 gap-3 py-8">
      <div className="w-14 h-14 rounded-2xl flex items-center justify-center"
        style={{ background: 'rgba(50,50,60,0.2)', border: '1px solid rgba(100,100,120,0.15)' }}>
        <CheckCircle className="w-7 h-7 opacity-25" />
      </div>
      <div className="text-xs font-medium text-gray-400">暂无部署记录</div>
      <div className="text-[10px] text-gray-700 text-center px-4">
        提交并确认部署后在此显示
      </div>
    </div>
  )

  return (
    <>
      <div className="flex-1 overflow-y-auto px-3 py-2.5 space-y-2">
        {sortedDeployments.map(dep => {
          const isExpanded  = expanded === dep.deployment_id
          const isHL        = highlightedDeploymentIds.includes(dep.deployment_id)
          const [lbl, lc]   = StatusLabel[dep.status] ?? ['未知', 'text-gray-500']

          return (
            <div key={dep.deployment_id} className="rounded-xl overflow-hidden transition-all"
              style={{
                background: isHL ? 'rgba(255,255,255,0.08)' : 'rgba(20,20,35,0.6)',
                border: isHL ? '1px solid rgba(255,255,255,0.35)' : '1px solid rgba(100,100,120,0.15)',
              }}>
              <div className="px-3 py-2.5">
                <div className="flex items-center gap-2 mb-1.5">
                  <StatusIcon s={dep.status} />
                  <span className="flex-1 text-[12px] font-semibold text-white truncate">{dep.sfc_name || dep.deployment_id}</span>
                  
                  {/* 关键：眼睛图标切换高亮 */}
                  <button onClick={() => toggleHighlight(dep)} title={isHL ? "取消高亮" : "在地球上高亮显示"}
                    className="p-1 rounded hover:bg-white/5 transition">
                    {isHL
                      ? <EyeOff className="w-3.5 h-3.5 text-white" />
                      : <Eye    className="w-3.5 h-3.5 text-gray-600 hover:text-gray-300" />}
                  </button>
                  
                  <button onClick={() => setExpanded(isExpanded ? null : dep.deployment_id)}
                    className="p-1 rounded hover:bg-white/5 transition">
                    {isExpanded ? <ChevronUp className="w-3.5 h-3.5 text-gray-500" /> : <ChevronDown className="w-3.5 h-3.5 text-gray-500" />}
                  </button>
                  <button onClick={() => setDeleteConfirm(dep.deployment_id)} disabled={rolling === dep.deployment_id}
                    className="p-1 rounded hover:bg-red-900/30 transition">
                    <Trash2 className="w-3.5 h-3.5 text-gray-700 hover:text-red-400" />
                  </button>
                </div>

                <div className="flex items-center justify-between text-[10px]">
                  <span className={`${lc} font-medium`}>{lbl}</span>
                  <span className="text-gray-600 font-mono">{dep.total_latency_ms?.toFixed(1)}ms</span>
                  <span className="text-gray-700">{new Date(dep.deployed_at).toLocaleTimeString('zh',{hour:'2-digit',minute:'2-digit'})}</span>
                </div>
                {(typeof dep.inference_latency_ms === 'number' || typeof (dep as any).topology_version_bound === 'number') && (
                  <div className="mt-1 flex items-center justify-between text-[10px] font-mono">
                    <span className="text-cyan-300">
                      {typeof dep.inference_latency_ms === 'number' ? `推理时延 ${dep.inference_latency_ms.toFixed(1)}ms` : '-'}
                    </span>
                    <span className="text-indigo-300">
                      {typeof (dep as any).topology_version_bound === 'number'
                        ? `topo_v${(dep as any).topology_version_bound} · 路径重算 ${(dep as any).path_recompute_count ?? 0} 次`
                        : '-'}
                    </span>
                  </div>
                )}

                <div className="flex flex-wrap gap-1 mt-1.5">
                  {dep.satisfies_constraints === false && (
                    <span className="px-1.5 py-0.5 rounded text-[9px] font-semibold" style={{ background: 'rgba(220,38,38,0.15)', border: '1px solid rgba(248,113,113,0.35)', color: '#fda4af' }}>
                      强制部署(不满足约束)
                    </span>
                  )}
                  {dep.source_node && (
                    <span className="px-1.5 py-0.5 rounded font-mono text-[9px]" style={{ background: 'rgba(59,130,246,0.16)', border: '1px solid rgba(96,165,250,0.35)', color: '#93c5fd' }}>
                      入口 {dep.source_node}
                    </span>
                  )}
                  {dep.destination_node && (
                    <span className="px-1.5 py-0.5 rounded font-mono text-[9px]" style={{ background: 'rgba(249,115,22,0.15)', border: '1px solid rgba(251,146,60,0.35)', color: '#fdba74' }}>
                      出口 {dep.destination_node}
                    </span>
                  )}
                  {dep.deployed_nodes.slice(0,4).map(n => (
                    <span key={n} className="px-1.5 py-0.5 rounded font-mono text-[9px]"
                      style={{ background: 'rgba(0,255,136,0.12)', border: '1px solid rgba(0,255,136,0.25)', color: '#00ff88' }}>
                      {n.length > 18 ? n.slice(0,18)+'…' : n}
                    </span>
                  ))}
                  {dep.deployed_nodes.length > 4 && (
                    <span className="px-1.5 py-0.5 rounded font-mono text-[9px] text-gray-600">+{dep.deployed_nodes.length - 4}</span>
                  )}
                </div>
              </div>

              {isExpanded && (
                <div className="px-3 pb-3 space-y-2" style={{ borderTop: '1px solid rgba(100,100,120,0.08)' }}>
                  {dep.satisfies_constraints === false && Array.isArray(dep.violation_details) && dep.violation_details.length > 0 && (
                    <div className="mt-2 rounded-lg p-2.5" style={{ background: 'rgba(127,29,29,0.2)', border: '1px solid rgba(248,113,113,0.28)' }}>
                      <div className="text-[11px] text-rose-300 font-semibold mb-1">强制部署原因</div>
                      <div className="space-y-0.5">
                        {dep.violation_details.map((reason, idx) => (
                          <div key={idx} className="text-[11px] text-rose-100">- {reason}</div>
                        ))}
                      </div>
                    </div>
                  )}
                  <div className="mt-2">
                    <div className="text-[11px] text-gray-500 uppercase tracking-wider mb-1.5 font-semibold">VNF 详情</div>
                    <div className="space-y-1">
                      {dep.per_vnf?.map((v, i) => (
                        <div key={i} className="flex items-center justify-between px-2 py-1.5 rounded-lg text-[11px]"
                          style={{ background: 'rgba(0,255,136,0.05)', border: '1px solid rgba(0,255,136,0.12)' }}>
                          <div className="flex items-center gap-2">
                            <span className="w-4.5 h-4.5 rounded flex items-center justify-center text-[9px] font-bold"
                              style={{ background: 'rgba(0,255,136,0.2)', color: '#00ff88' }}>{i+1}</span>
                            <span className="font-medium text-gray-300">{v.vnf}</span>
                            <span className="text-gray-700">→</span>
                            <span className="font-mono text-green-400">{v.node}</span>
                          </div>
                          <div className="flex gap-2 text-gray-600">
                            <span>CPU {v.cpu_used?.toFixed(2)}</span>
                            <span>MEM {v.mem_used?.toFixed(1)}G</span>
                            <span>DISK {(v as any).disk_used?.toFixed?.(1) ?? '0.0'}G</span>
                          </div>
                        </div>
                      ))}
                    </div>
                  </div>

                  {dep.link_details && dep.link_details.length > 0 && (
                    <div>
                      <div className="text-[11px] text-gray-500 uppercase tracking-wider mb-1.5 font-semibold">链路 ({dep.link_details.length} 跳)</div>
                      <div className="mb-1 text-[10px] text-cyan-300 font-mono break-all">
                        完整路径: {(() => {
                          const nodes = buildPathNodes(dep)
                          const full = nodes.join(' -> ')
                          if (expandedLinks[dep.deployment_id] || full.length <= 180) return full
                          return `${full.slice(0, 180)} ...`
                        })()}
                      </div>
                      <div className="mb-1.5 text-[10px] text-green-400 font-mono">
                        路径总时延: {dep.link_details.reduce((acc, l) => acc + Number(l.latency_ms || 0), 0).toFixed(2)}ms
                      </div>
                      <div className="space-y-1">
                        {(expandedLinks[dep.deployment_id] ? dep.link_details : dep.link_details.slice(0, 8)).map((l, i) => (
                          <div key={i} className="flex items-center justify-between px-2 py-1 rounded text-[10px]"
                            style={{ background: 'rgba(20,20,35,0.4)', border: '1px solid rgba(100,100,120,0.08)' }}>
                            <span className="font-mono text-green-400">
                              <span className="text-green-400">{l.src}</span>
                              <span className="text-gray-700 mx-1">→</span>
                              <span className="text-green-400">{l.dst}</span>
                            </span>
                            <span className="text-green-300">{l.latency_ms?.toFixed(2)}ms</span>
                          </div>
                        ))}
                      </div>
                      {dep.link_details.length > 8 && (
                        <button
                          onClick={() =>
                            setExpandedLinks(prev => ({ ...prev, [dep.deployment_id]: !prev[dep.deployment_id] }))
                          }
                          className="mt-1.5 text-[11px] font-medium text-green-400 hover:text-green-300 transition"
                        >
                          {expandedLinks[dep.deployment_id] ? '收起链路明细' : `展开全部链路 (+${dep.link_details.length - 8})`}
                        </button>
                      )}
                    </div>
                  )}

                  {dep.score_breakdown && dep.score_constraints && dep.score_weights && (
                    <div className="rounded-lg p-2" style={{ background: 'rgba(20,20,35,0.4)', border: '1px solid rgba(100,100,120,0.12)' }}>
                      <div className="text-[11px] text-cyan-300 font-semibold mb-1">方案评分</div>
                      <div className="grid grid-cols-3 gap-x-2 gap-y-1 text-[10px] text-slate-300">
                        <span>总分 {(Number(dep.score_total || 0) * 100).toFixed(1)}</span>
                        <span>时延 {(dep.score_breakdown.latency * 100).toFixed(1)}</span>
                        <span>带宽 {(dep.score_breakdown.bandwidth * 100).toFixed(1)}</span>
                        <span>可靠性 {(dep.score_breakdown.reliability * 100).toFixed(1)}</span>
                        <span>资源 {(dep.score_breakdown.resource * 100).toFixed(1)}</span>
                        <span>分散度 {(dep.score_breakdown.dispersion * 100).toFixed(1)}</span>
                      </div>
                      <div className="mt-1 text-[10px] text-slate-500 leading-relaxed">
                        细则: 时延/带宽/可靠性采用连续曲线评分（非阈值即满分）; 低于阈值会连续惩罚，高于阈值仍保留差异。<br />
                        权重: L {dep.score_weights.latency.toFixed(2)} / R {dep.score_weights.resource.toFixed(2)} / Rel {dep.score_weights.reliability.toFixed(2)} / BW {dep.score_weights.bandwidth.toFixed(2)} / D {dep.score_weights.dispersion.toFixed(2)}。
                      </div>
                    </div>
                  )}
                </div>
              )}
            </div>
          )
        })}
      </div>

      {deleteConfirm && (
        <div className="fixed inset-0 z-50 flex items-center justify-center" style={{ background: 'rgba(0,0,0,0.75)' }}>
          <div className="w-80 rounded-2xl overflow-hidden shadow-2xl"
            style={{ background: 'linear-gradient(180deg, #1a1a2e 0%, #0a0a15 100%)', border: '1px solid rgba(248,113,113,0.3)' }}>
            <div className="px-4 py-3 flex items-center gap-2"
              style={{ background: 'rgba(239,68,68,0.15)', borderBottom: '1px solid rgba(248,113,113,0.2)' }}>
              <AlertTriangle className="w-4 h-4 text-red-400" />
              <div className="text-sm font-bold text-white">确认回滚部署</div>
            </div>
            <div className="px-4 py-3">
              <p className="text-sm text-gray-300 leading-relaxed mb-2">
                确定要回滚此 SFC 部署吗？
              </p>
              <p className="text-xs text-gray-500">
                此操作将释放所有已分配的资源，且无法撤销。
              </p>
            </div>
            <div className="px-4 py-3 flex gap-2" style={{ borderTop: '1px solid rgba(100,100,120,0.15)' }}>
              <button onClick={() => setDeleteConfirm(null)}
                className="flex-1 py-2 rounded-lg text-sm font-medium text-gray-300 transition"
                style={{ background: 'rgba(50,50,60,0.5)' }}>
                取消
              </button>
              <button onClick={() => {
                const dep = deployments.find(d => d.deployment_id === deleteConfirm)
                if (dep) rollback(dep)
              }}
                className="flex-1 py-2 rounded-lg text-sm font-bold text-white transition"
                style={{ background: 'linear-gradient(135deg, #dc2626, #b91c1c)' }}>
                确认回滚
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  )
}
