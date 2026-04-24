import { useMemo, useRef, useState } from 'react'
import { X, CheckCircle, XCircle, Clock, Server, Zap, ChevronRight, Gauge, Info } from 'lucide-react'
import { useStore } from '@/store/useStore'
import { apiClient } from '@/api/client'
import { computeScoreBreakdown, normalizeWeights } from '@/utils/scoring'
import { toChineseFailureList, toChineseFailureText } from '@/utils/failureText'

function sanitizeLinkDetails(linkDetails: any[]): any[] {
  if (!Array.isArray(linkDetails)) return []
  const seen = new Set<string>()
  const out: any[] = []
  linkDetails.forEach((l: any) => {
    const src = String(l?.src ?? '')
    const dst = String(l?.dst ?? '')
    if (!src || !dst || src === dst) return
    const key = `${src}|${dst}`
    if (seen.has(key)) return
    seen.add(key)
    out.push({ ...l, src, dst })
  })
  return out
}

function buildPathNodesFromDeployment(cand: any, fallbackSrc?: string, fallbackDst?: string): string[] {
  const fromCandidatePath = Array.isArray(cand?.path_nodes)
    ? cand.path_nodes.map((n: any) => String(n)).filter(Boolean)
    : []
  if (fromCandidatePath.length >= 2) return fromCandidatePath

  const perCore = Array.isArray(cand?.per_core_nf)
    ? cand.per_core_nf
    : (Array.isArray(cand?.per_vnf) ? cand.per_vnf : [])
  const fromPerVnf = perCore.map((p: any) => String(p?.node ?? '')).filter(Boolean)
  const core = fromPerVnf.length > 0
    ? fromPerVnf
    : (Array.isArray(cand?.deployed_nodes) ? cand.deployed_nodes.map((n: any) => String(n)).filter(Boolean) : [])
  const seq = [fallbackSrc ?? '', ...core, fallbackDst ?? ''].filter(Boolean)
  const dedup = seq.filter((n, idx) => idx === 0 || n !== seq[idx - 1])
  if (dedup.length >= 2) return dedup
  if (fallbackSrc && fallbackDst && fallbackSrc !== fallbackDst) return [fallbackSrc, fallbackDst]
  return fallbackSrc ? [fallbackSrc] : []
}

function buildViolationDetails(
  cand: any,
  constraints: { max_latency_ms: number; min_bandwidth_gbps: number; min_reliability: number }
): string[] {
  if (Array.isArray(cand?.violation_details) && cand.violation_details.length > 0) {
    return toChineseFailureList(cand.violation_details)
  }
  const reasons: string[] = []
  const latency = Number(cand?.total_latency_ms ?? 0)
  const bw = Number(cand?.bottleneck_bandwidth_gbps ?? 0)
  const rel = Number(cand?.estimated_reliability ?? 0)
  if (constraints.max_latency_ms > 0 && latency > constraints.max_latency_ms) {
    reasons.push(`端到端时延超限: ${latency.toFixed(2)}ms > ${constraints.max_latency_ms}ms`)
  }
  if (constraints.min_bandwidth_gbps > 0 && bw < constraints.min_bandwidth_gbps) {
    reasons.push(`瓶颈带宽不足: ${bw.toFixed(3)}Gbps < ${constraints.min_bandwidth_gbps}Gbps`)
  }
  if (constraints.min_reliability > 0 && rel < constraints.min_reliability) {
    reasons.push(`可靠性不足: ${rel.toFixed(4)} < ${constraints.min_reliability.toFixed(4)}`)
  }
  if (reasons.length === 0 && cand?.reason) reasons.push(toChineseFailureText(String(cand.reason)))
  if (reasons.length === 0) reasons.push('后端未返回详细原因，可能由节点资源、链路约束或策略兼容性导致')
  return reasons
}

export default function CandidateModal() {
  const {
    candidateResult,
    setCandidateResult,
    addDeployment,
    applyTopologySnapshot,
    backendTopologySynced,
    satellites,
    pushRuntimeEvent,
    openSystemPopup,
  } = useStore()
  const [sel, setSel] = useState(0)
  const [busy, setBusy] = useState(false)
  const [showScoreInfo, setShowScoreInfo] = useState(false)
  const deployingRef = useRef(false)

  if (!candidateResult) return null
  const {
    requestId,
    sfcName,
    candidates,
    inferenceTime,
    scoringConfig,
    fallbackOnly,
    deployableCount,
    sourceNode,
    destinationNode,
    requestPayload,
    sessionConfig,
  } = candidateResult
  const cand = candidates[sel] ?? candidates[0]
  if (!cand) return null

  const constraints = scoringConfig?.constraints ?? {
    max_latency_ms: 150,
    min_bandwidth_gbps: 0.5,
    min_reliability: 0.95,
  }
  const violationDetails = buildViolationDetails(cand, constraints)

  const activeWeights = useMemo(() => {
    if (scoringConfig?.scoreWeights) return normalizeWeights(scoringConfig.scoreWeights)

    if (scoringConfig?.optimize === 'resource') {
      return { latency: 0.2, resource: 0.4, reliability: 0.25, bandwidth: 0.15, dispersion: 0 }
    }
    if (scoringConfig?.optimize === 'balanced') {
      return { latency: 0.25, resource: 0.25, reliability: 0.25, bandwidth: 0.25, dispersion: 0 }
    }
    return { latency: 0.45, resource: 0.1, reliability: 0.25, bandwidth: 0.2, dispersion: 0 }
  }, [scoringConfig])

  const scoreBreakdown = useMemo(() => {
    const vnfCount = scoringConfig?.vnfCount || cand.per_core_nf?.length || cand.per_vnf?.length || 1
    return computeScoreBreakdown({
      totalLatencyMs: Number(cand.total_latency_ms ?? 0),
      bottleneckBandwidthGbps: Number(cand.bottleneck_bandwidth_gbps ?? 0),
      estimatedReliability: Number(cand.estimated_reliability ?? 0),
      deployedNodeIds: cand.deployed_nodes ?? [],
      vnfCount,
      constraints,
      weights: activeWeights,
      satellites,
    })
  }, [cand, satellites, constraints, activeWeights, scoringConfig])

  const score = scoreBreakdown.total

  const deploy = async () => {
    if (busy || deployingRef.current) return
    if (!backendTopologySynced) {
      openSystemPopup('部署已阻止', '后端拓扑未同步，已禁止部署。请先重新生成/导入星座并完成同步。', 'warning')
      return
    }

    if (!cand.satisfies_constraints) {
      openSystemPopup(
        '方案不满足约束',
        `该方案不满足SLA约束，无法部署。\n\n详细原因:\n${violationDetails.map((d: string) => `- ${d}`).join('\n')}`,
        'warning',
      )
      return
    }

    deployingRef.current = true
    setBusy(true)
    try {
      pushRuntimeEvent({
        type: 'deployment_action',
        message: `开始部署 ${sfcName || requestId}`,
        raw: {
          title: '策略部署开始',
          detail: `${sfcName || requestId} · 候选#${sel + 1}`,
          level: 'info',
        },
      })
      const sanitizedLinks = sanitizeLinkDetails(cand.link_details ?? [])
      const pathNodes = buildPathNodesFromDeployment(cand, sourceNode, destinationNode)
      const deployResp = await apiClient.deploySFC({
        request_id: requestId,
        candidate_index: sel,
        candidate: cand,
        sfc_name: sfcName || requestId,
        source_node: sourceNode,
        destination_node: destinationNode,
        path_nodes: pathNodes,
        inference_latency_ms: Number(inferenceTime ?? 0),
        score_breakdown: {
          latency: scoreBreakdown.latencyScore,
          resource: scoreBreakdown.resourceScore,
          reliability: scoreBreakdown.reliabilityScore,
          bandwidth: scoreBreakdown.bandwidthScore,
          dispersion: scoreBreakdown.dispersionScore,
        },
        score_weights: {
          latency: activeWeights.latency,
          resource: activeWeights.resource,
          reliability: activeWeights.reliability,
          bandwidth: activeWeights.bandwidth,
          dispersion: activeWeights.dispersion,
        },
        score_constraints: constraints,
        strategy_mode: 'single_request',
      })
      const backendDeploymentId = String(deployResp?.deployment_id ?? `dep-${Date.now()}`)

      let sessionId = ''
      try {
        const sessionReq = {
          ...(requestPayload ?? {
            request_id: requestId,
            topology_version: candidateResult.topologyVersion,
            source_node: sourceNode,
            destination_node: destinationNode,
            core_nfs: (cand.per_core_nf ?? cand.per_vnf ?? []).map((v: any, idx: number) => ({
              name: String(v.core_nf ?? v.vnf ?? `core-nf-${idx + 1}`),
              core_nf_id: String(v.core_nf ?? v.vnf ?? `core-nf-${idx + 1}`),
              core_nf_type: String(v.nf_type ?? v.core_nf ?? v.vnf ?? `nf-${idx + 1}`),
              nf_type: String(v.nf_type ?? v.core_nf ?? v.vnf ?? `nf-${idx + 1}`),
              nf_role: String(v.nf_role ?? 'control_plane'),
              cpu: Number(v.cpu_used ?? 0),
              mem: Number(v.mem_used ?? 0),
              disk: Number(v.disk_used ?? 0),
              bw_in: 0.1,
              bw_out: 0.1,
            })) ?? [],
            constraints,
            optimize: scoringConfig?.optimize ?? 'latency',
            topk: candidateResult.requestedTopk ?? 1,
          }),
          request_id: requestId,
          realtime_mode: true,
          max_planning_attempts: Number(sessionConfig?.max_planning_attempts ?? 20),
          planning_time_budget_ms: Number(sessionConfig?.planning_time_budget_ms ?? 450),
          initial_candidate: cand,
        }
        if (!(sessionReq as any).vnfs && Array.isArray((sessionReq as any).core_nfs)) {
          ;(sessionReq as any).vnfs = (sessionReq as any).core_nfs
        }
        if (!(sessionReq as any).core_nf_sequence && Array.isArray((sessionReq as any).core_nfs)) {
          ;(sessionReq as any).core_nf_sequence = (sessionReq as any).core_nfs
        }
        const sessionStartPayload: any = {
          auto_redeploy: Boolean(sessionConfig?.auto_redeploy ?? true),
          initial_deployment_id: backendDeploymentId,
          request: sessionReq,
        }
        if (typeof inferenceTime === 'number' && Number.isFinite(inferenceTime) && inferenceTime > 0) {
          sessionStartPayload.initial_inference_time_ms = Number(inferenceTime)
        }
        const sessionResp = await apiClient.startSFCSession(sessionStartPayload)
        sessionId = String(sessionResp?.session_id ?? '')
      } catch (e: any) {
        openSystemPopup(
          '会话启动失败',
          `初始部署成功，但连续编排会话启动失败：${toChineseFailureText(e?.message ?? e)}`,
          'warning',
        )
      }

      const deploymentId = sessionId ? `sess-deploy-${sessionId}` : backendDeploymentId
      addDeployment({
        deployment_id: deploymentId,
        backend_deployment_id: backendDeploymentId,
        request_id: requestId,
        sfc_name: sfcName || requestId,
        candidate_index: sel,
        status: 'completed',
        source_node: sourceNode,
        destination_node: destinationNode,
        path_nodes: pathNodes,
        satisfies_constraints: !!cand.satisfies_constraints,
        violation_details: violationDetails,
        bottleneck_bandwidth_gbps: Number(cand.bottleneck_bandwidth_gbps ?? 0),
        estimated_reliability: Number(cand.estimated_reliability ?? 0),
        score_total: scoreBreakdown.total,
        score_breakdown: {
          latency: scoreBreakdown.latencyScore,
          resource: scoreBreakdown.resourceScore,
          reliability: scoreBreakdown.reliabilityScore,
          bandwidth: scoreBreakdown.bandwidthScore,
          dispersion: scoreBreakdown.dispersionScore,
        },
        score_weights: {
          latency: activeWeights.latency,
          resource: activeWeights.resource,
          reliability: activeWeights.reliability,
          bandwidth: activeWeights.bandwidth,
          dispersion: activeWeights.dispersion,
        },
        score_constraints: constraints,
        deployed_nodes: cand.deployed_nodes ?? [],
        per_vnf: cand.per_core_nf ?? cand.per_vnf ?? [],
        link_details: sanitizedLinks,
        total_latency_ms: cand.total_latency_ms ?? 0,
        inference_latency_ms: inferenceTime,
        deployed_at: new Date().toISOString(),
        progress: 100,
        strategy_mode: sessionId ? 'session_continuous' : 'single_request',
        session_id: sessionId || undefined,
        orchestration_phase: 'queued',
        orchestration_progress: 5,
        orchestration_mode: 'single_request',
        orchestration_trigger: 'manual_deploy',
        containers_total: Array.isArray(cand.deployed_nodes) ? new Set(cand.deployed_nodes.map((x: any) => String(x))).size : 0,
        containers_running: 0,
        containers_failed: 0,
        core_nfs_total: Array.isArray(cand.per_core_nf ?? cand.per_vnf) ? (cand.per_core_nf ?? cand.per_vnf).length : 0,
        core_nfs_running: 0,
        core_nfs_failed: 0,
        service_ready: false,
        ready_for_ueransim: false,
        last_error: '',
        last_update_at: new Date().toISOString(),
      })
      pushRuntimeEvent({
        type: 'deployment_action',
        message: `部署完成 ${sfcName || requestId}`,
        raw: {
          title: '策略部署完成',
          detail: `${sfcName || requestId} · 节点${(cand.deployed_nodes ?? []).length} · 链路${sanitizedLinks.length}`,
          level: 'ok',
        },
      })

      try {
        const topo = await apiClient.getTopology()
        applyTopologySnapshot(topo)
      } catch (e) {
        console.warn('[资源更新] 刷新失败:', e)
      }

      setCandidateResult(null)
    } catch (e: any) {
      pushRuntimeEvent({
        type: 'deployment_action',
        message: `部署失败 ${sfcName || requestId}`,
        raw: {
          title: '策略部署失败',
          detail: `${sfcName || requestId} · ${toChineseFailureText(e?.message ?? e)}`,
          level: 'warn',
        },
      })
      openSystemPopup('部署失败', toChineseFailureText(e.message ?? e), 'error')
    }
    setBusy(false)
    deployingRef.current = false
  }

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center" style={{ background: 'rgba(0,0,0,0.88)', backdropFilter: 'blur(10px)' }}>
      <div
        className="w-[760px] max-h-[88vh] flex flex-col rounded-2xl overflow-hidden shadow-2xl"
        style={{ background: 'linear-gradient(180deg, #111f32 0%, #0a1525 100%)', border: '1px solid rgba(87,146,191,0.32)' }}
      >
        <div
          className="px-5 py-4 flex items-start justify-between"
          style={{ borderBottom: '1px solid rgba(92,124,150,0.22)', background: 'linear-gradient(135deg, rgba(32,104,151,0.18), rgba(21,62,91,0.2))' }}
        >
          <div>
            <div className="text-base font-bold text-white">{sfcName} - 规划结果</div>
            <div className="text-[11px] text-slate-400 mt-1">
              请求 <span className="text-slate-300 font-mono">{requestId}</span>
              {inferenceTime != null && <span className="ml-3 text-cyan-300">推理时延 {inferenceTime.toFixed(1)}ms</span>}
              {sourceNode && destinationNode && (
                <span className="ml-3 text-amber-300">
                  路径端点 {sourceNode} → {destinationNode}
                </span>
              )}
            </div>
          </div>
          <button onClick={() => setCandidateResult(null)} className="p-1.5 rounded-lg hover:bg-white/5 transition">
            <X className="w-4.5 h-4.5 text-slate-400" />
          </button>
        </div>

        {fallbackOnly && (
          <div className="mx-5 mt-3 rounded-lg px-3 py-2 text-[11px]" style={{ background: 'rgba(120,53,15,0.35)', border: '1px solid rgba(251,191,36,0.45)', color: '#fde68a' }}>
            当前候选方案均不满足约束（deployable={deployableCount ?? 0}）。该窗口仅用于查看失败原因，不支持强制部署。
          </div>
        )}

        <div className="px-5 pt-3 pb-0 flex items-center gap-2 flex-wrap">
          {candidates.map((_: any, i: number) => (
            <button
              key={i}
              onClick={() => setSel(i)}
              className="px-4 py-2 rounded-lg text-xs font-bold transition"
              style={
                sel === i
                  ? { background: 'linear-gradient(135deg, #1f4a70, #12314d)', color: '#fff', boxShadow: '0 2px 8px rgba(15,52,96,0.4)' }
                  : { background: 'rgba(26,42,62,0.5)', color: '#94a3b8', border: '1px solid rgba(94,126,153,0.2)' }
              }
            >
              方案 {i + 1}
            </button>
          ))}
          <div className="ml-auto flex items-center gap-2 text-xs">
            {cand.satisfies_constraints ? (
              <>
                <CheckCircle className="w-4 h-4 text-emerald-400" />
                <span className="text-emerald-400 font-medium">满足约束</span>
              </>
            ) : (
              <>
                <XCircle className="w-4 h-4 text-red-400" />
                <span className="text-red-400 font-medium">{toChineseFailureText(cand.reason ?? '不满足')}</span>
              </>
            )}
          </div>
        </div>

        <div className="flex-1 overflow-y-auto px-5 py-4 space-y-4">
          {!cand.satisfies_constraints && violationDetails.length > 0 && (
            <div className="rounded-xl p-3" style={{ background: 'rgba(127,29,29,0.25)', border: '1px solid rgba(248,113,113,0.35)' }}>
              <div className="text-[11px] font-semibold text-rose-300 mb-1.5">不满足约束原因</div>
              <ul className="space-y-1">
                {violationDetails.map((d: string, i: number) => (
                  <li key={i} className="text-[11px] text-rose-100">- {d}</li>
                ))}
              </ul>
            </div>
          )}

          <div className="rounded-xl overflow-hidden" style={{ background: 'linear-gradient(135deg, rgba(38,109,157,0.2), rgba(23,55,84,0.18))', border: '1px solid rgba(86,139,177,0.35)' }}>
            <div className="px-4 py-3 flex items-center gap-2" style={{ borderBottom: '1px solid rgba(95,126,151,0.2)' }}>
              <Gauge className="w-4 h-4 text-cyan-300" />
              <span className="text-sm font-bold text-white">方案质量评分</span>
              <button onClick={() => setShowScoreInfo(!showScoreInfo)} className="ml-auto p-1 rounded hover:bg-white/10 transition" title="查看评分说明">
                <Info className="w-3.5 h-3.5 text-slate-300" />
              </button>
              <span className="text-2xl font-bold text-cyan-200">{(score * 100).toFixed(0)}</span>
            </div>

            {showScoreInfo && (
              <div className="px-4 py-3 text-[10px] text-slate-300 leading-relaxed" style={{ borderBottom: '1px solid rgba(100,100,120,0.08)', background: 'rgba(2,8,16,0.35)' }}>
                <div className="font-semibold text-slate-100 mb-2">当前评分策略</div>
                <div className="space-y-1">
                  <div>策略模式: <span className="text-cyan-300 font-mono">{scoringConfig?.scoreWeights ? 'custom' : scoringConfig?.optimize || 'latency'}</span></div>
                  <div>约束基准: 时延≤{constraints.max_latency_ms}ms, 带宽≥{constraints.min_bandwidth_gbps}Gbps, 可靠性≥{constraints.min_reliability.toFixed(3)}</div>
                  <div>
                    权重: latency {activeWeights.latency.toFixed(2)} / resource {activeWeights.resource.toFixed(2)} / reliability {activeWeights.reliability.toFixed(2)} /
                    bandwidth {activeWeights.bandwidth.toFixed(2)} / dispersion {activeWeights.dispersion.toFixed(2)}
                  </div>
                  <div className="pt-1 text-slate-400">
                    总分 = w_lat*时延得分 + w_res*资源得分 + w_rel*可靠性得分 + w_bw*带宽得分 + w_disp*分散度得分
                  </div>
                  <div className="text-slate-500">
                    时延/带宽/可靠性采用连续曲线评分，不再“满足阈值即 100 分”，以区分不同候选策略质量。
                  </div>
                </div>
              </div>
            )}

            <div className="px-4 py-3 space-y-2.5">
              {[
                { label: '时延得分', value: scoreBreakdown.latencyScore, color: '#22d3ee', detail: `r=${((Number(cand.total_latency_ms ?? 0)) / Math.max(1e-9, constraints.max_latency_ms)).toFixed(3)}（按连续惩罚曲线计算）` },
                { label: '资源得分', value: scoreBreakdown.resourceScore, color: '#34d399', detail: `基于部署节点剩余 CPU/MEM/DISK 均值` },
                { label: '可靠性得分', value: scoreBreakdown.reliabilityScore, color: '#a78bfa', detail: `r=${((Number(cand.estimated_reliability ?? 0)) / Math.max(1e-9, constraints.min_reliability)).toFixed(3)}（低于阈值立方惩罚）` },
                { label: '带宽得分', value: scoreBreakdown.bandwidthScore, color: '#fbbf24', detail: `r=${((Number(cand.bottleneck_bandwidth_gbps ?? 0)) / Math.max(1e-9, constraints.min_bandwidth_gbps)).toFixed(3)}（按连续增益/惩罚曲线）` },
                { label: '分散度得分', value: scoreBreakdown.dispersionScore, color: '#fb7185', detail: `${cand.deployed_nodes?.length ?? 0} 节点 / ${scoringConfig?.vnfCount || cand.per_core_nf?.length || cand.per_vnf?.length || 1} 核心网网元` },
              ].map(s => (
                <div key={s.label}>
                  <div className="flex justify-between text-[11px] mb-1">
                    <span className="text-slate-300 font-medium">{s.label}</span>
                    <span className="font-mono" style={{ color: s.color }}>
                      {(s.value * 100).toFixed(1)}
                    </span>
                  </div>
                  <div className="h-1.5 rounded-full overflow-hidden" style={{ background: 'rgba(8,16,29,0.9)' }}>
                    <div className="h-full rounded-full transition-all" style={{ width: `${s.value * 100}%`, background: s.color }} />
                  </div>
                  <div className="text-[9px] text-slate-500 mt-0.5">{s.detail}</div>
                </div>
              ))}
            </div>
          </div>

          <div className="grid grid-cols-4 gap-3">
            {[
              { label: '总评分', value: `${(score * 100).toFixed(1)}`, color: '#67e8f9', bg: 'rgba(31,84,118,0.28)' },
              { label: '路径时延', value: `${cand.total_latency_ms?.toFixed(1) ?? '—'}ms`, color: '#34d399', bg: 'rgba(22,92,82,0.26)' },
              { label: '卫星节点', value: `${cand.deployed_nodes?.length ?? 0}`, color: '#c4b5fd', bg: 'rgba(65,56,108,0.25)' },
              { label: 'ISL链路', value: `${cand.link_details?.length ?? 0}`, color: '#fbbf24', bg: 'rgba(110,83,25,0.24)' },
            ].map(m => (
              <div key={m.label} className="rounded-xl p-3 text-center" style={{ background: m.bg, border: '1px solid rgba(99,125,146,0.2)' }}>
                <div className="text-[9px] text-slate-500 uppercase tracking-wider mb-1">{m.label}</div>
                <div className="text-lg font-bold" style={{ color: m.color }}>
                  {m.value}
                </div>
              </div>
            ))}
          </div>

          <div>
            <div className="text-[11px] text-slate-300 uppercase tracking-wider mb-2.5 flex items-center gap-2 font-semibold">
              <Zap className="w-3.5 h-3.5 text-cyan-300" />核心网网元部署方案
            </div>
            <div className="space-y-2">
              {(cand.per_core_nf ?? cand.per_vnf ?? []).map((v: any, i: number) => (
                <div key={i} className="flex items-center gap-3 px-3 py-2.5 rounded-xl transition hover:bg-white/5" style={{ background: 'linear-gradient(135deg, rgba(30,87,122,0.2), rgba(16,42,63,0.2))', border: '1px solid rgba(91,141,177,0.28)' }}>
                  <div className="w-7 h-7 rounded-lg flex items-center justify-center text-[10px] font-bold flex-shrink-0" style={{ background: 'linear-gradient(135deg, #1c486e, #123252)', color: '#bae6fd', border: '1px solid rgba(102,169,210,0.3)' }}>
                    {i + 1}
                  </div>
                  <div className="flex-1 min-w-0">
                    <div className="text-sm font-semibold text-white">{v.core_nf ?? v.vnf}</div>
                    <div className="text-[10px] text-slate-400 mt-0.5">
                      {v.nf_type ?? '-'} · {v.nf_role ?? '-'}
                    </div>
                    <div className="text-[10px] text-slate-400 mt-0.5 flex items-center gap-1.5">
                      <Server className="w-3 h-3" />
                      <span className="text-cyan-200 font-mono">{v.node}</span>
                    </div>
                  </div>
                  <div className="flex gap-4 text-[10px] text-right">
                    <div>
                      <div className="text-slate-500">CPU</div>
                      <div className="text-slate-200 font-mono font-semibold">{v.cpu_used?.toFixed(2) ?? '—'}</div>
                    </div>
                    <div>
                      <div className="text-slate-500">MEM</div>
                      <div className="text-slate-200 font-mono font-semibold">{v.mem_used?.toFixed(1) ?? '—'}G</div>
                    </div>
                  </div>
                </div>
              ))}
            </div>
          </div>

          {(cand.link_details ?? []).length > 0 && (
            <div>
              <div className="text-[11px] text-slate-300 uppercase tracking-wider mb-2.5 flex items-center gap-2 font-semibold">
                <ChevronRight className="w-3.5 h-3.5 text-cyan-300" />星间链路 ({cand.link_details.length} 跳)
              </div>
              <div className="space-y-1.5">
                {cand.link_details.map((l: any, i: number) => (
                  <div key={i} className="flex items-center justify-between px-3 py-2 rounded-lg text-[11px]" style={{ background: 'rgba(13,24,40,0.85)', border: '1px solid rgba(87,116,139,0.22)' }}>
                    <div className="flex items-center gap-2 font-mono">
                      <span className="text-cyan-200">{l.src}</span>
                      <ChevronRight className="w-3 h-3 text-slate-600" />
                      <span className="text-cyan-200">{l.dst}</span>
                    </div>
                    <div className="flex gap-4 text-right">
                      <div className="text-slate-400">
                        <Clock className="w-3 h-3 inline mr-1 text-amber-400" />
                        <span className="font-mono">{l.latency_ms?.toFixed(2) ?? '—'}ms</span>
                      </div>
                      <div className="text-slate-400">
                        <span className="font-mono">{l.bandwidth_gbps?.toFixed(1) ?? '—'}Gbps</span>
                      </div>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          )}
        </div>

        <div className="px-5 py-3.5 flex items-center justify-between" style={{ borderTop: '1px solid rgba(95,126,151,0.22)' }}>
          <button onClick={() => setCandidateResult(null)} className="px-5 py-2 rounded-lg text-sm font-medium text-slate-400 hover:text-white hover:bg-white/5 transition">
            取消
          </button>
          <button
            onClick={deploy}
            disabled={busy || !cand.satisfies_constraints}
            className="px-6 py-2.5 rounded-xl text-sm font-bold text-white flex items-center gap-2.5 transition shadow-lg"
            style={{ background: busy || !cand.satisfies_constraints ? 'rgba(50,50,60,0.8)' : 'linear-gradient(135deg, #1e4e75, #143552)' }}
          >
            <CheckCircle className={`w-4 h-4 ${busy ? 'animate-pulse' : ''}`} />
            {busy ? '部署中...' : '确认部署'}
          </button>
        </div>
      </div>
    </div>
  )
}
