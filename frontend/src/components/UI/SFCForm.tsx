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
import type { LinkData, SatelliteData } from '@/utils/constellationGenerator'
import { toChineseFailureList, toChineseFailureText } from '@/utils/failureText'

const vnfTemplates = {
  firewall: { cpu: 0.8, mem: 1.5, bw_in: 0.5, bw_out: 0.5 },
  load_balancer: { cpu: 1.2, mem: 2.0, bw_in: 2.0, bw_out: 2.0 },
  nat: { cpu: 0.6, mem: 1.0, bw_in: 1.0, bw_out: 1.0 },
  ids_ips: { cpu: 1.5, mem: 3.0, bw_in: 2.0, bw_out: 2.0 },
  vpn_gateway: { cpu: 1.0, mem: 2.0, bw_in: 1.5, bw_out: 1.5 },
  web_server: { cpu: 2.0, mem: 4.0, bw_in: 0.5, bw_out: 2.0 },
  app_server: { cpu: 2.5, mem: 6.0, bw_in: 1.0, bw_out: 1.5 },
  db_server: { cpu: 3.0, mem: 8.0, bw_in: 0.5, bw_out: 0.5 },
  cache_server: { cpu: 1.5, mem: 4.0, bw_in: 1.0, bw_out: 1.0 },
  transcoder: { cpu: 3.0, mem: 6.0, bw_in: 5.0, bw_out: 3.0 },
  cdn_cache: { cpu: 1.5, mem: 8.0, bw_in: 3.0, bw_out: 10.0 },
  stream_server: { cpu: 2.0, mem: 4.0, bw_in: 0.5, bw_out: 8.0 },
  video_processor: { cpu: 3.5, mem: 7.0, bw_in: 4.0, bw_out: 4.0 },
  data_collector: { cpu: 1.0, mem: 2.0, bw_in: 1.0, bw_out: 0.5 },
  processor: { cpu: 2.5, mem: 5.0, bw_in: 0.5, bw_out: 0.5 },
  storage: { cpu: 0.8, mem: 3.0, bw_in: 0.5, bw_out: 0.2 },
  analytics: { cpu: 2.8, mem: 6.0, bw_in: 1.0, bw_out: 0.3 },
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

type GraphEdge = {
  to: string
  latencyMs: number
  bandwidthGbps: number
  bandwidthAvailableGbps: number
  reliability: number
}

function makeLinkKey(a: string, b: string) {
  return `${a}|${b}`
}

function buildGraph(links: LinkData[]) {
  const graph = new Map<string, GraphEdge[]>()
  const edgeByKey = new Map<string, GraphEdge>()
  links.forEach((l: any) => {
    const a = String(l?.source ?? '')
    const b = String(l?.target ?? '')
    if (!a || !b || a === b) return
    const status = String(l?.status ?? 'active')
    const bwAvail = Number(l?.bandwidth_available_gbps ?? l?.bandwidth_gbps ?? 0)
    const bwTotal = Number(l?.bandwidth_gbps ?? 0)
    if (status === 'down' || bwAvail <= 0 || bwTotal <= 0) return
    const latencyMs = Math.max(0.01, Number(l?.latency_ms ?? 1))
    const reliability = Math.max(0, Math.min(1, Number(l?.reliability ?? l?.link_reliability ?? 0.999)))
    const ab: GraphEdge = { to: b, latencyMs, bandwidthGbps: bwTotal, bandwidthAvailableGbps: bwAvail, reliability }
    const ba: GraphEdge = { to: a, latencyMs, bandwidthGbps: bwTotal, bandwidthAvailableGbps: bwAvail, reliability }
    if (!graph.has(a)) graph.set(a, [])
    if (!graph.has(b)) graph.set(b, [])
    graph.get(a)!.push(ab)
    graph.get(b)!.push(ba)
    edgeByKey.set(makeLinkKey(a, b), ab)
    edgeByKey.set(makeLinkKey(b, a), ba)
  })
  return { graph, edgeByKey }
}

function shortestPath(graph: Map<string, GraphEdge[]>, source: string, target: string): string[] {
  if (!source || !target) return []
  if (source === target) return [source]
  const dist = new Map<string, number>()
  const prev = new Map<string, string>()
  const visited = new Set<string>()
  const nodes = new Set<string>()
  graph.forEach((_, key) => nodes.add(key))
  nodes.add(source)
  nodes.add(target)
  nodes.forEach((n) => dist.set(n, Number.POSITIVE_INFINITY))
  dist.set(source, 0)

  while (visited.size < nodes.size) {
    let bestNode = ''
    let bestDist = Number.POSITIVE_INFINITY
    nodes.forEach((n) => {
      if (visited.has(n)) return
      const d = dist.get(n) ?? Number.POSITIVE_INFINITY
      if (d < bestDist) {
        bestDist = d
        bestNode = n
      }
    })
    if (!bestNode || !Number.isFinite(bestDist)) break
    if (bestNode === target) break
    visited.add(bestNode)
    const edges = graph.get(bestNode) ?? []
    edges.forEach((e) => {
      if (visited.has(e.to)) return
      const nd = bestDist + e.latencyMs
      if (nd < (dist.get(e.to) ?? Number.POSITIVE_INFINITY)) {
        dist.set(e.to, nd)
        prev.set(e.to, bestNode)
      }
    })
  }

  if (!prev.has(target)) return []
  const path = [target]
  let cur = target
  for (let i = 0; i < nodes.size + 2; i++) {
    const p = prev.get(cur)
    if (!p) break
    path.push(p)
    if (p === source) break
    cur = p
  }
  if (path[path.length - 1] !== source) return []
  return path.reverse()
}

function buildFallbackCandidate(args: {
  satellites: SatelliteData[]
  links: LinkData[]
  sourceNode: string
  destinationNode: string
  vnfs: Array<{ name: string; cpu: number; mem: number; disk: number }>
  constraints: { max_latency_ms: number; min_bandwidth_gbps: number; min_reliability: number }
}) {
  const { satellites, links, sourceNode, destinationNode, vnfs, constraints } = args
  const satMap = new Map<string, any>()
  satellites.forEach((s: any) => satMap.set(String(s.id), s))
  const { graph, edgeByKey } = buildGraph(links)
  const basePath = shortestPath(graph, sourceNode, destinationNode)
  if (basePath.length < 2) return null

  const availableByNode = new Map<string, { cpu: number; mem: number; disk: number }>()
  basePath.forEach((nodeId) => {
    const n: any = satMap.get(nodeId)
    if (!n) return
    availableByNode.set(nodeId, {
      cpu: Number(n.cpu_available ?? 0),
      mem: Number(n.mem_available ?? 0),
      disk: Number(n.disk_available ?? 0),
    })
  })

  const per_vnf: any[] = []
  const deployed_nodes: string[] = []
  let scanStartIdx = 0
  for (let i = 0; i < vnfs.length; i++) {
    const v = vnfs[i]
    let selectedNode = ''
    for (let k = scanStartIdx; k < basePath.length; k++) {
      const nodeId = basePath[k]
      const avail = availableByNode.get(nodeId)
      if (!avail) continue
      if (avail.cpu >= v.cpu && avail.mem >= v.mem && avail.disk >= v.disk) {
        avail.cpu -= v.cpu
        avail.mem -= v.mem
        avail.disk -= v.disk
        selectedNode = nodeId
        scanStartIdx = k
        break
      }
    }
    if (!selectedNode) {
      return null
    }
    deployed_nodes.push(selectedNode)
    per_vnf.push({
      vnf: v.name || `vnf-${i + 1}`,
      node: selectedNode,
      cpu_used: Number(v.cpu),
      mem_used: Number(v.mem),
      disk_used: Number(v.disk),
    })
  }

  const anchors = [sourceNode, ...deployed_nodes, destinationNode]
    .filter(Boolean)
    .filter((n, idx, arr) => idx === 0 || n !== arr[idx - 1])
  const link_details: any[] = []
  let fullPathNodes: string[] = [anchors[0]]

  for (let i = 0; i + 1 < anchors.length; i++) {
    const seg = shortestPath(graph, anchors[i], anchors[i + 1])
    if (seg.length < 2) return null
    for (let j = 0; j + 1 < seg.length; j++) {
      const src = seg[j]
      const dst = seg[j + 1]
      const edge = edgeByKey.get(makeLinkKey(src, dst))
      if (!edge) return null
      link_details.push({
        src,
        dst,
        latency_ms: edge.latencyMs,
        bandwidth_gbps: edge.bandwidthGbps,
        bandwidth_available_gbps: edge.bandwidthAvailableGbps,
        bandwidth_required_gbps: 0,
        status: 'active',
        reliability: edge.reliability,
      })
      if (fullPathNodes[fullPathNodes.length - 1] !== src) fullPathNodes.push(src)
      fullPathNodes.push(dst)
    }
  }

  const totalLatency = link_details.reduce((acc, l) => acc + Number(l.latency_ms || 0), 0)
  const bottleneckBw = link_details.length > 0
    ? Math.min(...link_details.map((l) => Number(l.bandwidth_available_gbps ?? l.bandwidth_gbps ?? 0)))
    : 0
  const linkReliability = link_details.reduce((acc, l) => acc * Math.max(1e-9, Number(l.reliability ?? 0.999)), 1)
  const nodeReliability = deployed_nodes.reduce((acc, nodeId) => {
    const rel = Number((satMap.get(nodeId) as any)?.node_reliability ?? 0.998)
    return acc * Math.max(1e-9, Math.min(1, rel))
  }, 1)
  const estimatedReliability = linkReliability * nodeReliability

  const violation_details: string[] = []
  if (Number.isFinite(constraints.max_latency_ms) && constraints.max_latency_ms > 0 && totalLatency > constraints.max_latency_ms) {
    violation_details.push(`端到端时延超限：${totalLatency.toFixed(2)}ms > ${constraints.max_latency_ms}ms`)
  }
  if (Number.isFinite(constraints.min_bandwidth_gbps) && constraints.min_bandwidth_gbps > 0 && bottleneckBw < constraints.min_bandwidth_gbps) {
    violation_details.push(`瓶颈带宽不足：${bottleneckBw.toFixed(3)}Gbps < ${constraints.min_bandwidth_gbps}Gbps`)
  }
  if (Number.isFinite(constraints.min_reliability) && constraints.min_reliability > 0 && estimatedReliability < constraints.min_reliability) {
    violation_details.push(`可靠性不足：${estimatedReliability.toFixed(4)} < ${constraints.min_reliability.toFixed(4)}`)
  }
  const satisfies_constraints = violation_details.length === 0

  return {
    score: 0,
    reason: satisfies_constraints ? '' : violation_details[0] ?? '仅找到保底路径候选（不满足SLA）',
    satisfies_constraints,
    deployed_nodes,
    per_vnf,
    path_nodes: fullPathNodes,
    link_details,
    total_latency_ms: totalLatency,
    bottleneck_bandwidth_gbps: bottleneckBw,
    estimated_reliability: estimatedReliability,
    violation_details,
    __fallback: true,
  }
}

const sfcTemplates = [
  {
    name: '示例',
    vnfs: ['firewall', 'vpn_gateway', 'app_server', 'db_server'] as VNFTemplateName[],
    constraints: { max_latency_ms: 140, min_bandwidth_gbps: 1.5, min_reliability: 0.92 },
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
  const { setCandidateResult, addToast, topologyVersion, backendTopologySynced, satellites, links, simulation } = useStore()

  const [mode, setMode] = useState<'template' | 'custom'>('template')
  const [selectedTemplate, setSelectedTemplate] = useState(0)
  const [hoverTemplate, setHoverTemplate] = useState<number | null>(null)

  const [customSFC, setCustomSFC] = useState({
    name: '自定义SFC',
    vnfs: [
      { type: 'firewall', name: 'vnf-1-firewall', ...vnfTemplates.firewall },
      { type: 'web_server', name: 'vnf-2-web', ...vnfTemplates.web_server },
    ] as VNFConfig[],
    constraints: { max_latency_ms: 120, min_bandwidth_gbps: 1.0, min_reliability: 0.92 },
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
    priority: 'medium',
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
        vnfs: [...prev.vnfs, { type: 'firewall', name: `vnf-${idx}-firewall`, ...vnfTemplates.firewall }],
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
      next[index] = { ...next[index], type: value, name: prevName, ...vnfTemplates[value] }
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
                {key === 'dispersion' && <InfoHint text="分散度=已部署卫星数/VNF数。权重越高越鼓励跨星分散部署。" />}
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
      alert('当前星座未完成后端同步，无法规划部署。请先重新生成/导入并确认同步成功。')
      return
    }

    if (enableCustomWeights && Math.abs(scoreWeightSum - 1.0) > 1e-6) {
      alert(`自定义评分权重总和必须为 1.0，当前为 ${scoreWeightSum.toFixed(3)}。`)
      return
    }
    if (satelliteIds.length < 2) {
      alert('当前拓扑卫星数量不足，至少需要 2 颗卫星才能配置 source_node/destination_node。')
      return
    }
    if (!trafficEndpoints.source_node || !trafficEndpoints.destination_node) {
      alert('请填写 SFC 流量入口（source_node）和出口（destination_node）。')
      return
    }
    if (!satelliteIds.includes(trafficEndpoints.source_node) || !satelliteIds.includes(trafficEndpoints.destination_node)) {
      alert('source_node 或 destination_node 不存在于当前星座，请从下拉候选选择或输入正确卫星 ID。')
      return
    }
    if (trafficEndpoints.source_node === trafficEndpoints.destination_node) {
      alert('source_node 与 destination_node 不能相同。')
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
                name: `vnf-${idx + 1}-${type}`,
                ...vnfTemplates[type],
              })),
              constraints: selectedTpl.constraints,
              topk: templateAdvanced.topk,
              optimize: templateAdvanced.optimize,
            }
          : customSFC

      const reqId = `req-${Date.now()}`
      const optimizeMode = enableCustomWeights ? 'custom' : sfc.optimize || 'latency'
      const payload = {
        request_id: reqId,
        topology_version: topologyVersion,
        sim_time: simulation.sim_time,
        source_node: trafficEndpoints.source_node,
        destination_node: trafficEndpoints.destination_node,
        priority: trafficEndpoints.priority,
        vnfs: sfc.vnfs.map((v: any, idx: number) => ({
          name: v.name?.trim() || `vnf-${idx + 1}`,
          cpu: v.cpu,
          mem: v.mem,
          bw_in: v.bw_in,
          bw_out: v.bw_out,
          disk: Number.isFinite(v.disk) ? v.disk : v.mem * 2.0,
        })),
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
          vnfCount: payload.vnfs.length,
          constraints: sfc.constraints,
          weights: activeWeights,
          satellites,
        })
        return { ...c, score: breakdown.total }
      })
      const sortedCandidates = scoredCandidates.sort((a: any, b: any) => (b.score ?? 0) - (a.score ?? 0))

      let finalCandidates = sortedCandidates
      if (finalCandidates.length === 0) {
        const fallback = buildFallbackCandidate({
          satellites: satellites as SatelliteData[],
          links: links as LinkData[],
          sourceNode: trafficEndpoints.source_node,
          destinationNode: trafficEndpoints.destination_node,
          vnfs: payload.vnfs,
          constraints: sfc.constraints,
        })
        if (fallback) {
          const fallbackScore = computeScoreBreakdown({
            totalLatencyMs: Number(fallback.total_latency_ms ?? 0),
            bottleneckBandwidthGbps: Number(fallback.bottleneck_bandwidth_gbps ?? 0),
            estimatedReliability: Number(fallback.estimated_reliability ?? 0),
            deployedNodeIds: fallback.deployed_nodes ?? [],
            vnfCount: payload.vnfs.length,
            constraints: sfc.constraints,
            weights: activeWeights,
            satellites,
          })
          finalCandidates = [{ ...fallback, score: fallbackScore.total }]
          addToast('后端未返回候选，已生成本地保底候选策略', 'warning')
        }
      }

      if (finalCandidates.length === 0) {
        const errorMsg = toChineseFailureText(result.error || 'No feasible deployment found')
        addToast(`部署失败: ${errorMsg}`, 'error')
        if (result.failure_reasons || result.details) {
          setTimeout(() => {
            const details = toChineseFailureList(result.failure_reasons || result.details).join('\n')
            alert(`无可行部署方案\n\n详细原因:\n${details}`)
          }, 500)
        }
        setBusy(false)
        return
      }

      const feasible = finalCandidates.filter((c: any) => c.satisfies_constraints)
      if (feasible.length === 0) {
        addToast('未找到完全满足约束的方案，已展示候选供人工决策', 'warning')
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
        fallbackOnly: !!result.fallback_only || feasible.length === 0,
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
          vnfCount: payload.vnfs.length,
        },
        requestPayload: payload,
        sessionConfig: {
          auto_redeploy: sessionRealtimeConfig.auto_redeploy,
          max_planning_attempts: sessionRealtimeConfig.max_planning_attempts,
          planning_time_budget_ms: sessionRealtimeConfig.planning_time_budget_ms,
        },
      })

      if (feasible.length === 0) {
        addToast(`生成 ${finalCandidates.length} 个候选方案（均不满足SLA，可人工选择）`, 'warning')
      } else {
        addToast(`生成 ${finalCandidates.length} 个候选方案`, 'success')
      }
      if (result.warning || finalCandidates.length < (sfc.topk || 1)) {
        alert(toChineseFailureText(result.warning || `仅生成 ${finalCandidates.length} 个可行方案，少于请求的 Top-${sfc.topk || 1}。`))
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
                      <span className="text-[9px] text-slate-500 font-mono">{tpl.vnfs.length}V</span>
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
                        <div className="text-[9px] text-slate-400 font-mono">{tpl.vnfs.length} VNFs</div>
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
                          <div className="px-2 py-1">VNF</div>
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
                                {idx + 1}. {type}
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
              <div className="text-[10px] text-slate-400 uppercase tracking-wider font-semibold">VNF列表 ({customSFC.vnfs.length})</div>
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
                          {name}
                        </option>
                      ))}
                    </select>
                    <button onClick={() => removeVNF(i)} className="p-1 rounded hover:bg-red-900/30 transition">
                      <Trash2 className="w-3.5 h-3.5 text-slate-500 hover:text-red-400" />
                    </button>
                  </div>

                  <div className="mb-2">
                    <div className="text-[10px] text-slate-500 mb-0.5">VNF 名称（自定义）</div>
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
          <div>
            <div className="text-slate-500 mb-0.5 flex items-center gap-1">
              priority
              <InfoHint text="请求优先级标签，和 priority_weight 一起影响编排偏好。" />
            </div>
            <select
              value={trafficEndpoints.priority}
              onChange={e => setTrafficEndpoints(prev => ({ ...prev, priority: e.target.value }))}
              className="w-full px-2 py-1 rounded"
              style={{ background: 'rgba(9,17,31,0.9)', border: '1px solid rgba(98,128,152,0.25)', color: '#fff' }}
            >
              <option value="low">低</option>
              <option value="medium">中</option>
              <option value="high">高</option>
            </select>
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
