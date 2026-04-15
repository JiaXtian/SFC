import { useEffect, useMemo, useState, type ReactNode } from 'react'
import {
  Send,
  Loader2,
  Plus,
  Trash2,
  Settings2,
  ChevronDown,
  ChevronUp,
  Info,
  FolderKanban,
  SlidersHorizontal,
  Radar,
} from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'
import { computeScoreBreakdown, normalizeWeights } from '@/utils/scoring'
import { toChineseFailureList, toChineseFailureText } from '@/utils/failureText'

const vnfTemplates = {
  amf: { cpu: 1.6, mem: 3.0, bw_in: 0.25, bw_out: 0.25, disk: 10 },
  smf: { cpu: 1.9, mem: 3.4, bw_in: 0.3, bw_out: 0.3, disk: 12 },
  upf: { cpu: 2.8, mem: 4.6, bw_in: 1.0, bw_out: 1.0, disk: 18 },
  ausf: { cpu: 1.2, mem: 2.4, bw_in: 0.2, bw_out: 0.2, disk: 8 },
  udm: { cpu: 1.5, mem: 3.2, bw_in: 0.2, bw_out: 0.2, disk: 20 },
  udr: { cpu: 1.4, mem: 3.0, bw_in: 0.2, bw_out: 0.2, disk: 22 },
  pcf: { cpu: 1.6, mem: 3.0, bw_in: 0.2, bw_out: 0.2, disk: 12 },
  nrf: { cpu: 1.1, mem: 2.2, bw_in: 0.15, bw_out: 0.15, disk: 8 },
  nssf: { cpu: 1.1, mem: 2.0, bw_in: 0.15, bw_out: 0.15, disk: 8 },
  scp: { cpu: 1.3, mem: 2.6, bw_in: 0.2, bw_out: 0.2, disk: 10 },
}

type VNFTemplateName = keyof typeof vnfTemplates
const DEFAULT_SOURCE_NODE = 'SAT_000_000'
const DEFAULT_DESTINATION_NODE = 'SAT_000_001'

interface VNFConfig {
  type: VNFTemplateName
  name: string
  cpu: number
  mem: number
  bw_in: number
  bw_out: number
  disk?: number
}

const sfcTemplates = [
  {
    name: 'open5GS 基础链',
    vnfs: ['amf', 'smf', 'upf'] as VNFTemplateName[],
    constraints: { max_latency_ms: 160, min_bandwidth_gbps: 1.0, min_reliability: 0.9 },
  },
  {
    name: 'open5GS 扩展链',
    vnfs: ['nrf', 'scp', 'ausf', 'udm', 'udr', 'amf', 'smf', 'upf', 'pcf', 'nssf'] as VNFTemplateName[],
    constraints: { max_latency_ms: 240, min_bandwidth_gbps: 1.2, min_reliability: 0.86 },
  },
]

function InfoHint({ text }: { text: string }) {
  const [open, setOpen] = useState(false)

  return (
    <span className="inline-flex flex-col align-middle">
      <button
        type="button"
        className="inline-flex items-center justify-center"
        onClick={e => {
          e.preventDefault()
          e.stopPropagation()
          setOpen(v => !v)
        }}
      >
        <Info className="w-3 h-3 text-slate-500 hover:text-slate-200 transition-colors cursor-pointer" />
      </button>
      {open && <span className="mt-1 max-w-64 text-[9px] leading-relaxed text-slate-400">{text}</span>}
    </span>
  )
}

function FoldHeader({
  icon,
  title,
  open,
  onToggle,
}: {
  icon: ReactNode
  title: string
  open: boolean
  onToggle: () => void
}) {
  return (
    <button
      type="button"
      onClick={onToggle}
      className="w-full flex items-center justify-between text-[10px] text-slate-300 uppercase tracking-wider font-semibold hover:text-white transition"
    >
      <span className="inline-flex items-center gap-1.5">
        {icon}
        {title}
      </span>
      {open ? <ChevronUp className="w-3 h-3" /> : <ChevronDown className="w-3 h-3" />}
    </button>
  )
}

export default function SFCForm() {
  const { setCandidateResult, addToast, openSystemPopup, topologyVersion, backendTopologySynced, satellites, simulation } = useStore()

  const [mode, setMode] = useState<'template' | 'custom'>('template')
  const [selectedTemplate, setSelectedTemplate] = useState(0)
  const [hoverTemplate, setHoverTemplate] = useState<number | null>(null)

  const [customSFC, setCustomSFC] = useState({
    name: '自定义SFC',
    vnfs: [
      { type: 'amf', name: 'AMF-1', ...vnfTemplates.amf },
      { type: 'smf', name: 'SMF-2', ...vnfTemplates.smf },
      { type: 'upf', name: 'UPF-3', ...vnfTemplates.upf },
    ] as VNFConfig[],
    constraints: { max_latency_ms: 180, min_bandwidth_gbps: 1.0, min_reliability: 0.88 },
    topk: 1,
    optimize: 'latency',
  })

  const [runtimeContext, setRuntimeContext] = useState({
    core_network_load: 0.5,
    priority_weight: 1.0,
    load_level: 'medium',
  })
  const sessionRealtimeConfig = {
    max_planning_attempts: 20,
    planning_time_budget_ms: 450,
    auto_redeploy: true,
  }
  const [trafficEndpoints, setTrafficEndpoints] = useState({
    source_node: DEFAULT_SOURCE_NODE,
    destination_node: DEFAULT_DESTINATION_NODE,
  })

  const [showRuntimeContext, setShowRuntimeContext] = useState(false)
  const [templateAdvanced, setTemplateAdvanced] = useState({ topk: 1, optimize: 'latency' })
  const [enableCustomWeights, setEnableCustomWeights] = useState(false)
  const [scoreWeights, setScoreWeights] = useState({
    latency: 0.45,
    resource: 0.1,
    reliability: 0.25,
    bandwidth: 0.2,
    dispersion: 0.0,
  })
  const [busy, setBusy] = useState(false)

  const scoreWeightSum = useMemo(
    () => scoreWeights.latency + scoreWeights.resource + scoreWeights.reliability + scoreWeights.bandwidth + scoreWeights.dispersion,
    [scoreWeights]
  )
  const satelliteIds = useMemo(() => satellites.map(s => s.id), [satellites])

  const addVNF = () => {
    setCustomSFC(prev => {
      const idx = prev.vnfs.length + 1
      return {
        ...prev,
        vnfs: [...prev.vnfs, { type: 'amf', name: `AMF-${idx}`, ...vnfTemplates.amf }],
      }
    })
  }

  const removeVNF = (index: number) => {
    setCustomSFC(prev => ({ ...prev, vnfs: prev.vnfs.filter((_, i) => i !== index) }))
  }

  const updateVNFType = (index: number, value: VNFTemplateName) => {
    setCustomSFC(prev => {
      const next = [...prev.vnfs]
      const prevName = next[index]?.name || value
      const suggested = `${String(value).toUpperCase()}-${index + 1}`
      next[index] = { ...next[index], type: value, name: prevName || suggested, ...vnfTemplates[value] }
      return { ...prev, vnfs: next }
    })
  }

  const updateVNFField = (index: number, field: keyof VNFConfig, value: any) => {
    setCustomSFC(prev => {
      const next = [...prev.vnfs]
      next[index] = { ...next[index], [field]: value }
      return { ...prev, vnfs: next }
    })
  }

  const renderScoreSection = () => (
    <div className="space-y-2">
      <label className="flex items-center gap-2 text-[10px] text-slate-300 cursor-pointer">
        <input type="checkbox" checked={enableCustomWeights} onChange={e => setEnableCustomWeights(e.target.checked)} />
        自定义评分权重
        <InfoHint text="启用后按时延/资源/可靠性/带宽/分散度加权评分，权重总和需为 1。" />
      </label>

      {enableCustomWeights && (
        <div className="grid grid-cols-2 gap-2">
          {[
            ['latency', '时延权重'],
            ['resource', '资源权重'],
            ['reliability', '可靠性权重'],
            ['bandwidth', '带宽权重'],
            ['dispersion', '分散度权重'],
          ].map(([key, label]) => (
            <div key={key} className={key === 'dispersion' ? 'col-span-2' : ''}>
              <div className="text-[10px] text-slate-500 mb-1 flex items-center gap-1">
                {label}
                {key === 'dispersion' && <InfoHint text="分散度=已部署卫星数/核心网网元数。权重越高越鼓励跨星分散部署。" />}
              </div>
              <input
                type="number"
                step="0.01"
                min="0"
                max="1"
                value={scoreWeights[key as keyof typeof scoreWeights]}
                onChange={e =>
                  setScoreWeights(s => ({ ...s, [key]: Math.max(0, Math.min(1, parseFloat(e.target.value) || 0)) }))
                }
                className="w-full px-2.5 py-1.5 rounded-lg text-xs"
                style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#f8fafc' }}
              />
            </div>
          ))}
          <div className="col-span-2 text-[10px]">
            <div className={Math.abs(scoreWeightSum - 1.0) < 1e-6 ? 'text-emerald-400' : 'text-red-400'}>
              权重总和: {scoreWeightSum.toFixed(3)}（必须等于 1.000）
            </div>
          </div>
        </div>
      )}

    </div>
  )

  const submit = async () => {
    if (!backendTopologySynced) {
      openSystemPopup('无法生成部署策略', '当前星座未完成后端同步，无法规划部署。请先重新生成/导入并确认同步成功。', 'warning')
      return
    }

    if (enableCustomWeights && Math.abs(scoreWeightSum - 1.0) > 1e-6) {
      openSystemPopup('参数校验失败', `自定义评分权重总和必须为 1.0，当前为 ${scoreWeightSum.toFixed(3)}。`, 'warning')
      return
    }
    if (satelliteIds.length < 2) {
      openSystemPopup('参数校验失败', '当前拓扑卫星数量不足，至少需要 2 颗卫星才能配置 source_node/destination_node。', 'warning')
      return
    }
    if (!trafficEndpoints.source_node || !trafficEndpoints.destination_node) {
      openSystemPopup('参数校验失败', '请填写 SFC 流量入口（source_node）和出口（destination_node）。', 'warning')
      return
    }
    if (!satelliteIds.includes(trafficEndpoints.source_node) || !satelliteIds.includes(trafficEndpoints.destination_node)) {
      openSystemPopup('参数校验失败', 'source_node 或 destination_node 不存在于当前星座，请从下拉候选选择或输入正确卫星 ID。', 'warning')
      return
    }
    if (trafficEndpoints.source_node === trafficEndpoints.destination_node) {
      openSystemPopup('参数校验失败', 'source_node 与 destination_node 不能相同。', 'warning')
      return
    }

    setBusy(true)
    try {
      const selectedTpl = sfcTemplates[selectedTemplate]
      const sfc =
        mode === 'template'
          ? {
              name: selectedTpl.name,
              vnfs: selectedTpl.vnfs.map((type, idx) => ({
                name: `${String(type).toUpperCase()}-${idx + 1}`,
                type,
                ...vnfTemplates[type],
              })),
              constraints: selectedTpl.constraints,
              topk: templateAdvanced.topk,
              optimize: templateAdvanced.optimize,
            }
          : customSFC

      const reqId = `req-${Date.now()}`
      const optimizeMode = enableCustomWeights ? 'custom' : sfc.optimize || 'latency'
      const coreNfs = sfc.vnfs.map((v: any, idx: number) => {
        const nfType = String(v.type || v.nf_type || v.name || 'amf')
        const nfName = v.name?.trim() || `core-nf-${idx + 1}-${nfType}`
        return {
          name: nfName,
          core_nf_id: nfName,
          core_nf_type: nfType,
          nf_type: nfType,
          nf_role: nfType === 'upf' ? 'user_plane' : 'control_plane',
          cpu: v.cpu,
          mem: v.mem,
          bw_in: v.bw_in,
          bw_out: v.bw_out,
          disk: Number.isFinite(v.disk) ? v.disk : v.mem * 2.0,
        }
      })
      const payload = {
        request_id: reqId,
        network_domain: 'open5gs',
        topology_version: topologyVersion,
        sim_time: simulation.sim_time,
        source_node: trafficEndpoints.source_node,
        destination_node: trafficEndpoints.destination_node,
        core_nfs: coreNfs,
        core_nf_sequence: coreNfs,
        vnfs: coreNfs,
        constraints: sfc.constraints,
        optimize: optimizeMode,
        topk: sfc.topk || 1,
        core_network_load: runtimeContext.core_network_load,
        priority_weight: runtimeContext.priority_weight,
        load_level: runtimeContext.load_level,
        max_planning_attempts: sessionRealtimeConfig.max_planning_attempts,
        planning_time_budget_ms: sessionRealtimeConfig.planning_time_budget_ms,
        ...(enableCustomWeights
          ? {
              score_weights: {
                latency: scoreWeights.latency,
                resource: scoreWeights.resource,
                reliability: scoreWeights.reliability,
                bandwidth: scoreWeights.bandwidth,
                dispersion: scoreWeights.dispersion,
              },
            }
          : {}),
      }

      const result = await apiClient.planSFC(payload)
      const activeWeights = normalizeWeights(
        enableCustomWeights
          ? {
              latency: scoreWeights.latency,
              resource: scoreWeights.resource,
              reliability: scoreWeights.reliability,
              bandwidth: scoreWeights.bandwidth,
              dispersion: scoreWeights.dispersion,
            }
          : optimizeMode === 'resource'
            ? { latency: 0.2, resource: 0.4, reliability: 0.25, bandwidth: 0.15, dispersion: 0 }
            : optimizeMode === 'balanced'
              ? { latency: 0.25, resource: 0.25, reliability: 0.25, bandwidth: 0.25, dispersion: 0 }
              : { latency: 0.45, resource: 0.1, reliability: 0.25, bandwidth: 0.2, dispersion: 0 }
      )
      const scoredCandidates = [...(result.candidates || [])].map((c: any) => {
        const breakdown = computeScoreBreakdown({
          totalLatencyMs: Number(c.total_latency_ms ?? 0),
          bottleneckBandwidthGbps: Number(c.bottleneck_bandwidth_gbps ?? 0),
          estimatedReliability: Number(c.estimated_reliability ?? 0),
          deployedNodeIds: c.deployed_nodes ?? [],
          vnfCount: payload.core_nfs.length,
          constraints: sfc.constraints,
          weights: activeWeights,
          satellites,
        })
        return { ...c, score: breakdown.total }
      })
      const sortedCandidates = scoredCandidates.sort((a: any, b: any) => (b.score ?? 0) - (a.score ?? 0))

      const finalCandidates = sortedCandidates

      if (finalCandidates.length === 0) {
        const errorMsg = toChineseFailureText(result.error || 'No feasible deployment found')
        addToast(`部署失败: ${errorMsg}`, 'error')
        if (result.failure_reasons || result.details) {
          const details = toChineseFailureList(result.failure_reasons || result.details).join('\n')
          openSystemPopup('无可行部署方案', `详细原因:\n${details}`, 'error')
        }
        setBusy(false)
        return
      }

      const feasible = finalCandidates.filter((c: any) => c.satisfies_constraints)
      if (feasible.length === 0) {
        const reasonPool: string[] = []
        finalCandidates.forEach((cand: any, idx: number) => {
          const detailList = toChineseFailureList(
            Array.isArray(cand?.violation_details) && cand.violation_details.length > 0
              ? cand.violation_details
              : cand?.reason
          )
          if (detailList.length === 0) {
            reasonPool.push(`候选方案 #${idx + 1}: 未返回详细失败原因（可能由资源、链路或SLA约束导致）`)
            return
          }
          detailList.forEach((d) => reasonPool.push(`候选方案 #${idx + 1}: ${d}`))
        })
        const uniqReasons = Array.from(new Set(reasonPool)).slice(0, 12)
        addToast('未找到满足约束的可部署方案', 'error')
        openSystemPopup('未找到满足约束的可部署方案', `详细原因:\n${uniqReasons.join('\n')}`, 'error')
        setBusy(false)
        return
      }

      setCandidateResult({
        requestId: reqId,
        sfcName: sfc.name,
        candidates: finalCandidates,
        inferenceTime: result.inference_time_ms,
        sourceNode: result.source_node || trafficEndpoints.source_node,
        destinationNode: result.destination_node || trafficEndpoints.destination_node,
        topologyVersion: Number(result.topology_version ?? topologyVersion),
        requestedTopk: sfc.topk || 1,
        warning: result.warning || '',
        fallbackOnly: false,
        deployableCount: typeof result.deployable_count === 'number' ? result.deployable_count : feasible.length,
        scoringConfig: {
          optimize: optimizeMode,
          constraints: sfc.constraints,
          scoreWeights: enableCustomWeights
            ? {
                latency: scoreWeights.latency,
                resource: scoreWeights.resource,
                reliability: scoreWeights.reliability,
                bandwidth: scoreWeights.bandwidth,
                dispersion: scoreWeights.dispersion,
              }
            : null,
          vnfCount: payload.core_nfs.length,
        },
        requestPayload: payload,
        sessionConfig: {
          auto_redeploy: sessionRealtimeConfig.auto_redeploy,
          max_planning_attempts: sessionRealtimeConfig.max_planning_attempts,
          planning_time_budget_ms: sessionRealtimeConfig.planning_time_budget_ms,
        },
      })

      addToast(`生成 ${finalCandidates.length} 个候选方案`, 'success')
      if (result.warning || finalCandidates.length < (sfc.topk || 1)) {
        openSystemPopup(
          '候选方案提示',
          toChineseFailureText(result.warning || `仅生成 ${finalCandidates.length} 个可行方案，少于请求的 Top-${sfc.topk || 1}。`),
          'warning',
        )
      }
    } catch (err: any) {
      console.error('[SFCForm] Submit error:', err)
      const errorMsg = toChineseFailureText(err.response?.data?.details || err.message || '未知错误')
      addToast(`请求失败: ${errorMsg}`, 'error')
    }
    setBusy(false)
  }

  const selectedTpl = sfcTemplates[selectedTemplate]

  return (
    <div className="space-y-3 h-full pr-1 pb-2 text-slate-100">
      <div className="flex gap-2">
        <button
          onClick={() => setMode('template')}
          className="flex-1 py-2 rounded-lg text-xs font-bold transition"
          style={
            mode === 'template'
              ? { background: 'linear-gradient(135deg, #14385f, #10263e)', color: '#fff' }
              : { background: 'rgba(16,27,43,0.6)', color: '#94a3b8', border: '1px solid rgba(102,132,160,0.18)' }
          }
        >
          模板
        </button>
        <button
          onClick={() => setMode('custom')}
          className="flex-1 py-2 rounded-lg text-xs font-bold transition"
          style={
            mode === 'custom'
              ? { background: 'linear-gradient(135deg, #14385f, #10263e)', color: '#fff' }
              : { background: 'rgba(16,27,43,0.6)', color: '#94a3b8', border: '1px solid rgba(102,132,160,0.18)' }
          }
        >
          自定义
        </button>
      </div>

      {mode === 'template' && (
        <div className="space-y-3">
          <div className="rounded-xl p-3 min-h-[176px] relative" style={{ background: 'rgba(6,14,24,0.84)', border: '1px solid rgba(90,122,147,0.26)' }}>
            <div className="text-[10px] text-slate-400 uppercase tracking-wider font-semibold mb-2">模板区域（可扩展）</div>
            <div className="grid grid-cols-1 gap-2">
              {sfcTemplates.map((tpl, i) => (
                <div key={tpl.name} className="relative">
                  <button
                    onClick={() => setSelectedTemplate(i)}
                    onMouseEnter={() => setHoverTemplate(i)}
                    onMouseLeave={() => setHoverTemplate(null)}
                    className="w-full text-left px-2.5 py-2 rounded-lg transition"
                    style={{
                      background:
                        selectedTemplate === i
                          ? 'linear-gradient(135deg, rgba(20,64,94,0.85), rgba(14,30,48,0.9))'
                          : 'rgba(12,22,36,0.82)',
                      border:
                        selectedTemplate === i
                          ? '1px solid rgba(97,160,204,0.58)'
                          : '1px solid rgba(87,116,139,0.24)',
                    }}
                  >
                    <div className="flex items-center justify-between gap-2">
                      <span className="inline-flex items-center gap-1.5 text-[11px] font-semibold text-slate-100">
                        <FolderKanban className="w-3.5 h-3.5 text-cyan-300" />
                        {tpl.name}
                      </span>
                      <span className="text-[9px] text-slate-500 font-mono">{tpl.vnfs.length}NF</span>
                    </div>
                  </button>

                  {hoverTemplate === i && (
                    <div
                      className="absolute left-0 right-0 top-[calc(100%+6px)] z-20 rounded-lg p-3"
                      style={{
                        background: 'rgba(5,12,22,0.96)',
                        border: '1px solid rgba(101, 157, 197, 0.42)',
                        boxShadow: '0 12px 32px rgba(0,0,0,0.55), 0 0 20px rgba(63,142,194,0.22)',
                      }}
                    >
                      <div className="flex items-center justify-between mb-2">
                        <div className="text-[11px] font-semibold text-cyan-100">{tpl.name} 模板详情</div>
                        <div className="text-[9px] text-slate-400 font-mono">{tpl.vnfs.length} Core NFs</div>
                      </div>

                      <div className="grid grid-cols-3 gap-1.5 mb-2">
                        <div className="rounded-md px-2 py-1" style={{ background: 'rgba(10,20,34,0.9)', border: '1px solid rgba(83,114,138,0.2)' }}>
                          <div className="text-[8px] text-slate-500 uppercase">时延</div>
                          <div className="text-[10px] text-slate-200 font-semibold">≤ {tpl.constraints.max_latency_ms}ms</div>
                        </div>
                        <div className="rounded-md px-2 py-1" style={{ background: 'rgba(10,20,34,0.9)', border: '1px solid rgba(83,114,138,0.2)' }}>
                          <div className="text-[8px] text-slate-500 uppercase">带宽</div>
                          <div className="text-[10px] text-slate-200 font-semibold">≥ {tpl.constraints.min_bandwidth_gbps}Gbps</div>
                        </div>
                        <div className="rounded-md px-2 py-1" style={{ background: 'rgba(10,20,34,0.9)', border: '1px solid rgba(83,114,138,0.2)' }}>
                          <div className="text-[8px] text-slate-500 uppercase">可靠性</div>
                          <div className="text-[10px] text-slate-200 font-semibold">≥ {(tpl.constraints.min_reliability * 100).toFixed(1)}%</div>
                        </div>
                      </div>

                      <div className="rounded-md overflow-hidden" style={{ border: '1px solid rgba(83,114,138,0.26)' }}>
                        <div
                          className="grid grid-cols-[1.4fr_1fr_1fr_1fr] text-[8px] uppercase tracking-wider"
                          style={{ background: 'rgba(14,27,44,0.95)', color: '#94a3b8' }}
                        >
                          <div className="px-2 py-1">核心网网元</div>
                          <div className="px-2 py-1 text-right">CPU</div>
                          <div className="px-2 py-1 text-right">MEM</div>
                          <div className="px-2 py-1 text-right">BW in/out</div>
                        </div>

                        {tpl.vnfs.map((type, idx) => {
                          const vnf = vnfTemplates[type]
                          return (
                            <div
                              key={`${type}-${idx}`}
                              className="grid grid-cols-[1.4fr_1fr_1fr_1fr] text-[9px]"
                              style={{
                                background: idx % 2 === 0 ? 'rgba(9,18,31,0.92)' : 'rgba(7,15,26,0.92)',
                                borderTop: idx === 0 ? 'none' : '1px solid rgba(78,102,122,0.16)',
                              }}
                            >
                              <div className="px-2 py-1 text-cyan-100 font-mono">
                                {idx + 1}. {String(type).toUpperCase()}
                              </div>
                              <div className="px-2 py-1 text-right text-slate-300 font-mono">{vnf.cpu}</div>
                              <div className="px-2 py-1 text-right text-slate-300 font-mono">{vnf.mem}</div>
                              <div className="px-2 py-1 text-right text-slate-300 font-mono">
                                {vnf.bw_in}/{vnf.bw_out}
                              </div>
                            </div>
                          )
                        })}
                      </div>
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>

          <div className="rounded-xl px-3 py-2.5" style={{ background: 'rgba(10,19,33,0.5)', border: '1px solid rgba(92,123,150,0.22)' }}>
            <div className="w-full flex items-center text-[10px] text-slate-300 uppercase tracking-wider font-semibold">
              <span className="inline-flex items-center gap-1.5">
                <SlidersHorizontal className="w-3.5 h-3.5 text-cyan-300" />
                候选策略设置
              </span>
            </div>

            <div className="mt-2 space-y-2">
              <div>
                <div className="text-[10px] text-slate-500 mb-1">Top-K 候选方案数</div>
                <input
                  type="number"
                  min="1"
                  max="10"
                  value={templateAdvanced.topk}
                  onChange={e => setTemplateAdvanced(prev => ({ ...prev, topk: parseInt(e.target.value) || 1 }))}
                  className="w-full px-3 py-1.5 rounded-lg text-xs"
                  style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#fff' }}
                />
              </div>
              <div>
                <div className="text-[10px] text-slate-500 mb-1">优化目标</div>
                <select
                  value={templateAdvanced.optimize}
                  onChange={e => setTemplateAdvanced(prev => ({ ...prev, optimize: e.target.value }))}
                  disabled={enableCustomWeights}
                  className="w-full px-3 py-1.5 rounded-lg text-xs"
                  style={{
                    background: enableCustomWeights ? 'rgba(14,24,39,0.45)' : 'rgba(14,24,39,0.9)',
                    border: '1px solid rgba(99,130,158,0.25)',
                    color: enableCustomWeights ? '#94a3b8' : '#fff',
                  }}
                >
                  <option value="latency">时延优先</option>
                  <option value="resource">资源优先</option>
                  <option value="balanced">均衡</option>
                </select>
              </div>
              <div className="pt-1.5" style={{ borderTop: '1px solid rgba(88,116,139,0.3)' }}>
                {renderScoreSection()}
              </div>
            </div>
          </div>
        </div>
      )}

      {mode === 'custom' && (
        <div className="space-y-3">
          <div>
            <div className="text-[10px] text-slate-400 uppercase tracking-wider mb-1.5 font-semibold">SFC名称</div>
            <input
              type="text"
              value={customSFC.name}
              onChange={e => setCustomSFC(prev => ({ ...prev, name: e.target.value }))}
              className="w-full px-3 py-2 rounded-lg text-sm"
              style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#fff' }}
              placeholder="输入SFC名称"
            />
          </div>

          <div>
            <div className="flex items-center justify-between mb-1.5">
              <div className="text-[10px] text-slate-400 uppercase tracking-wider font-semibold">核心网网元列表 ({customSFC.vnfs.length})</div>
              <button onClick={addVNF} className="px-2 py-1 rounded text-[10px] font-bold flex items-center gap-1 transition hover:bg-white/5" style={{ color: '#67e8f9' }}>
                <Plus className="w-3 h-3" /> 添加
              </button>
            </div>

            <div className="space-y-2 max-h-[300px] overflow-y-auto pr-1">
              {customSFC.vnfs.map((vnf, i) => (
                <div key={i} className="p-2.5 rounded-lg" style={{ background: 'rgba(12,22,38,0.86)', border: '1px solid rgba(96,125,149,0.23)' }}>
                  <div className="grid grid-cols-[48px_1fr_28px] gap-2 mb-2 items-center">
                    <span className="text-[10px] text-slate-500 font-mono">#{i + 1}</span>
                    <select
                      value={vnf.type}
                      onChange={e => updateVNFType(i, e.target.value as VNFTemplateName)}
                      className="w-full px-2 py-1 rounded text-xs"
                      style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
                    >
                      {Object.keys(vnfTemplates).map(name => (
                        <option key={name} value={name}>
                          {name.toUpperCase()}
                        </option>
                      ))}
                    </select>
                    <button onClick={() => removeVNF(i)} className="p-1 rounded hover:bg-red-900/30 transition">
                      <Trash2 className="w-3.5 h-3.5 text-slate-500 hover:text-red-400" />
                    </button>
                  </div>

                  <div className="mb-2">
                    <div className="text-[10px] text-slate-500 mb-0.5">网元名称（自定义）</div>
                    <input
                      type="text"
                      value={vnf.name}
                      onChange={e => updateVNFField(i, 'name', e.target.value)}
                      className="w-full px-2 py-1 rounded text-xs"
                      style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
                    />
                  </div>

                  <div className="grid grid-cols-2 gap-2 text-[10px]">
                    {[
                      ['cpu', 'CPU', 0.1],
                      ['mem', 'MEM (GB)', 0.1],
                      ['disk', 'DISK (GB)', 0.1],
                      ['bw_in', 'BW IN (Gbps)', 0.1],
                      ['bw_out', 'BW OUT (Gbps)', 0.1],
                    ].map(([field, label, step]) => (
                      <div key={field} className={field === 'disk' ? '' : ''}>
                        <div className="text-slate-500 mb-0.5">{label}</div>
                        <input
                          type="number"
                          step={step as number}
                          value={field === 'disk' ? (vnf.disk ?? vnf.mem * 2) : (vnf as any)[field]}
                          onChange={e => updateVNFField(i, field as keyof VNFConfig, parseFloat(e.target.value))}
                          className="w-full px-2 py-1 rounded"
                          style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
                        />
                      </div>
                    ))}
                  </div>
                </div>
              ))}
            </div>
          </div>

          <div className="grid grid-cols-3 gap-2">
            <div>
              <div className="text-[10px] text-slate-400 uppercase tracking-wider mb-1 font-semibold">最大时延 (ms)</div>
              <input
                type="number"
                value={customSFC.constraints.max_latency_ms}
                onChange={e =>
                  setCustomSFC(prev => ({ ...prev, constraints: { ...prev.constraints, max_latency_ms: parseInt(e.target.value) || 10 } }))
                }
                className="w-full px-3 py-2 rounded-lg text-sm"
                style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#fff' }}
              />
            </div>
            <div>
              <div className="text-[10px] text-slate-400 uppercase tracking-wider mb-1 font-semibold">最小带宽 (Gbps)</div>
              <input
                type="number"
                step="0.1"
                value={customSFC.constraints.min_bandwidth_gbps}
                onChange={e =>
                  setCustomSFC(prev => ({ ...prev, constraints: { ...prev.constraints, min_bandwidth_gbps: parseFloat(e.target.value) || 0.01 } }))
                }
                className="w-full px-3 py-2 rounded-lg text-sm"
                style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#fff' }}
              />
            </div>
            <div>
              <div className="text-[10px] text-slate-400 uppercase tracking-wider mb-1 font-semibold">最低可靠性</div>
              <input
                type="number"
                step="0.001"
                min="0"
                max="1"
                value={customSFC.constraints.min_reliability}
                onChange={e =>
                  setCustomSFC(prev => ({ ...prev, constraints: { ...prev.constraints, min_reliability: parseFloat(e.target.value) || 0.72 } }))
                }
                className="w-full px-3 py-2 rounded-lg text-sm"
                style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#fff' }}
              />
            </div>
          </div>

          <div className="rounded-xl px-3 py-2.5" style={{ background: 'rgba(10,19,33,0.5)', border: '1px solid rgba(92,123,150,0.22)' }}>
            <div className="w-full flex items-center text-[10px] text-slate-300 uppercase tracking-wider font-semibold">
              <span className="inline-flex items-center gap-1.5">
                <Settings2 className="w-3.5 h-3.5 text-cyan-300" />
                候选策略设置
              </span>
            </div>

            <div className="mt-2 space-y-2">
              <div>
                <div className="text-[10px] text-slate-500 mb-1">Top-K 候选方案数</div>
                <input
                  type="number"
                  min="1"
                  max="10"
                  value={customSFC.topk}
                  onChange={e => setCustomSFC(prev => ({ ...prev, topk: parseInt(e.target.value) || 1 }))}
                  className="w-full px-3 py-1.5 rounded-lg text-xs"
                  style={{ background: 'rgba(14,24,39,0.9)', border: '1px solid rgba(99,130,158,0.25)', color: '#fff' }}
                />
              </div>
              <div>
                <div className="text-[10px] text-slate-500 mb-1">优化目标</div>
                <select
                  value={customSFC.optimize}
                  onChange={e => setCustomSFC(prev => ({ ...prev, optimize: e.target.value }))}
                  disabled={enableCustomWeights}
                  className="w-full px-3 py-1.5 rounded-lg text-xs"
                  style={{
                    background: enableCustomWeights ? 'rgba(14,24,39,0.45)' : 'rgba(14,24,39,0.9)',
                    border: '1px solid rgba(99,130,158,0.25)',
                    color: enableCustomWeights ? '#94a3b8' : '#fff',
                  }}
                >
                  <option value="latency">时延优先</option>
                  <option value="resource">资源优先</option>
                  <option value="balanced">均衡</option>
                </select>
              </div>
              <div className="pt-1.5" style={{ borderTop: '1px solid rgba(88,116,139,0.3)' }}>
                {renderScoreSection()}
              </div>
            </div>
          </div>
        </div>
      )}

      <div className="rounded-xl px-3 py-2.5" style={{ background: 'rgba(10,19,33,0.5)', border: '1px solid rgba(92,123,150,0.22)' }}>
        <div className="text-[10px] text-slate-300 uppercase tracking-wider font-semibold mb-2 flex items-center gap-1.5">
          <Radar className="w-3.5 h-3.5 text-cyan-300" />
          流量入口与出口
        </div>
        <div className="grid grid-cols-2 gap-2 text-[10px]">
          <div>
            <div className="text-slate-500 mb-0.5 flex items-center gap-1">
              source_node
              <InfoHint text="SFC 流量入口卫星，必须存在于当前星座。" />
            </div>
            <input
              type="text"
              list="sfc-satellite-options"
              value={trafficEndpoints.source_node}
              onChange={e => setTrafficEndpoints(prev => ({ ...prev, source_node: e.target.value.trim() }))}
              className="w-full px-2 py-1 rounded"
              placeholder="SAT_000_000"
              style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
            />
          </div>
          <div>
            <div className="text-slate-500 mb-0.5 flex items-center gap-1">
              destination_node
              <InfoHint text="SFC 流量出口卫星，必须存在于当前星座。" />
            </div>
            <input
              type="text"
              list="sfc-satellite-options"
              value={trafficEndpoints.destination_node}
              onChange={e => setTrafficEndpoints(prev => ({ ...prev, destination_node: e.target.value.trim() }))}
              className="w-full px-2 py-1 rounded"
              placeholder="SAT_000_001"
              style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
            />
          </div>
          <div />
        </div>
        <datalist id="sfc-satellite-options">
          {satelliteIds.map(id => (
            <option key={id} value={id} />
          ))}
        </datalist>
      </div>

      <div className="rounded-xl px-3 py-2.5" style={{ background: 'rgba(10,19,33,0.5)', border: '1px solid rgba(92,123,150,0.22)' }}>
        <FoldHeader
          icon={<Radar className="w-3.5 h-3.5 text-cyan-300" />}
          title="模型上下文参数"
          open={showRuntimeContext}
          onToggle={() => setShowRuntimeContext(v => !v)}
        />

        {showRuntimeContext && (
          <div className="grid grid-cols-3 gap-2 text-[10px] mt-2">
            <div>
              <div className="text-slate-500 mb-0.5 flex items-center gap-1">
                网络负载 (0-1)
                <InfoHint text="数值越高表示越拥塞，调度会更保守。" />
              </div>
              <input
                type="number"
                min="0"
                max="1"
                step="0.05"
                value={runtimeContext.core_network_load}
                onChange={e => setRuntimeContext(prev => ({ ...prev, core_network_load: parseFloat(e.target.value) || 0 }))}
                className="w-full px-2 py-1 rounded"
                style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
              />
            </div>
            <div>
              <div className="text-slate-500 mb-0.5 flex items-center gap-1">
                优先级权重
                <InfoHint text="值越高，该请求排序权重越高。" />
              </div>
              <input
                type="number"
                min="0"
                step="0.1"
                value={runtimeContext.priority_weight}
                onChange={e => setRuntimeContext(prev => ({ ...prev, priority_weight: parseFloat(e.target.value) || 0 }))}
                className="w-full px-2 py-1 rounded"
                style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
              />
            </div>
            <div>
              <div className="text-slate-500 mb-0.5 flex items-center gap-1">
                负载等级
                <InfoHint text="低/中/高为离散网络负载标签。" />
              </div>
              <select
                value={runtimeContext.load_level}
                onChange={e => setRuntimeContext(prev => ({ ...prev, load_level: e.target.value }))}
                className="w-full px-2 py-1 rounded"
                style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
              >
                <option value="low">低</option>
                <option value="medium">中</option>
                <option value="high">高</option>
              </select>
            </div>
          </div>
        )}
      </div>

      <button
        onClick={submit}
        disabled={busy}
        className="w-full py-3 rounded-xl text-sm font-bold text-white flex items-center justify-center gap-2 transition shadow-lg disabled:opacity-50"
        style={{ background: busy ? 'rgba(50,50,60,0.8)' : 'linear-gradient(135deg, #15385d, #0f243b)' }}
      >
        {busy ? (
          <>
            <Loader2 className="w-4 h-4 animate-spin" />
            规划中...
          </>
        ) : (
          <>
            <Send className="w-4 h-4" />
            生成部署策略
          </>
        )}
      </button>

    </div>
  )
}
