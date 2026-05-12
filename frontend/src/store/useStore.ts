import { create } from 'zustand'
import type { SatelliteData, LinkData } from '@/utils/constellationGenerator'

export interface VNFDeploy {
  vnf: string
  core_nf?: string
  nf_type?: string
  nf_role?: string
  node: string
  cpu_used: number
  mem_used: number
  disk_used?: number
}
export interface LinkDetail {
  src: string
  dst: string
  dependency_source_nf?: string
  dependency_target_nf?: string
  latency_ms: number
  bandwidth_gbps: number
  bandwidth_available_gbps?: number
  bandwidth_required_gbps?: number
  status?: string
  fault_tag?: string
  reliability?: number
}
export interface CoreNFDependency {
  source: string
  target: string
  criticality?: number
  bandwidth_scale?: number
  latency_weight?: number
  reliability_weight?: number
  bandwidth_required_gbps?: number
}
export interface Deployment {
  deployment_id: string
  backend_deployment_id?: string
  core_network_id?: string
  core_network_label?: string
  request_id: string
  sfc_name: string
  candidate_index: number
  status: 'in-progress' | 'completed' | 'failed' | 'rolled_back'
  inference_latency_ms?: number
  source_node?: string
  destination_node?: string
  path_nodes?: string[]
  satisfies_constraints?: boolean
  violation_details?: string[]
  bottleneck_bandwidth_gbps?: number
  estimated_reliability?: number
  score_total?: number
  score_breakdown?: {
    latency: number
    resource: number
    reliability: number
    bandwidth: number
  }
  score_weights?: {
    latency: number
    resource: number
    reliability: number
    bandwidth: number
  }
  score_constraints?: {
    max_latency_ms: number
    registration_latency_ms?: number
    registration_access_latency_ms?: number
    pdu_session_latency_ms?: number
    pdu_access_latency_ms?: number
    min_bandwidth_gbps: number
    min_reliability: number
  }
  deployed_nodes: string[]
  per_vnf: VNFDeploy[]
  link_details: LinkDetail[]
  core_nf_dependencies?: CoreNFDependency[]
  previous_link_details?: LinkDetail[]
  total_latency_ms: number
  registration_latency_ms?: number
  pdu_session_latency_ms?: number
  deployed_at: string
  progress: number
  strategy_mode?: 'single_request' | 'session_continuous'
  session_id?: string
  topology_version_bound?: number
  path_recompute_count?: number
  path_transition_until?: number
  decision_trigger?: string
  orchestration_phase?: string
  orchestration_progress?: number
  orchestration_mode?: string
  orchestration_trigger?: string
  containers_total?: number
  containers_running?: number
  containers_failed?: number
  core_nfs_total?: number
  core_nfs_running?: number
  core_nfs_failed?: number
  service_ready?: boolean
  ready_for_ueransim?: boolean
  last_error?: string
  last_update_at?: string
}

export interface CandidateResult {
  requestId: string
  sfcName: string
  coreNetworkId?: string
  coreNetworkLabel?: string
  candidates: any[]
  inferenceTime?: number
  topologyVersion: number
  requestedTopk?: number
  warning?: string
  fallbackOnly?: boolean
  deployableCount?: number
  sourceNode?: string
  destinationNode?: string
  scoringConfig?: {
    optimize: string
    constraints: {
      max_latency_ms: number
      registration_latency_ms?: number
      registration_access_latency_ms?: number
      pdu_session_latency_ms?: number
      pdu_access_latency_ms?: number
      min_bandwidth_gbps: number
      min_reliability: number
    }
    scoreWeights: {
      latency: number
      resource: number
      reliability: number
      bandwidth: number
    } | null
  }
  requestPayload?: any
  sessionConfig?: {
    auto_redeploy: boolean
    max_planning_attempts: number
    planning_time_budget_ms: number
  }
}

export interface RequestVNF {
  name: string
  core_nf?: string
  nf_type?: string
  nf_role?: string
  resource_profile?: string
  processing_weight?: number
  stateful?: boolean
  cpu: number
  mem: number
  disk: number
  bw_in?: number
  bw_out?: number
}

export interface DynamicMetrics {
  total_nodes: number
  active_nodes: number
  down_nodes: number
  total_links: number
  active_links: number
  down_links: number
  congested_links: number
  avg_latency_ms: number
  avg_bandwidth_utilization: number
  avg_core_network_load?: number
  avg_signaling_load?: number
  avg_session_load?: number
  avg_user_plane_load?: number
  avg_mobility_load?: number
  avg_policy_load?: number
  avg_auth_load?: number
}

export interface OrchestrationMetrics {
  active_sessions: number
  decisions_this_tick: number
  redeploys_this_tick: number
  failures_this_tick: number
  recovery_attempts_this_tick?: number
  recovery_success_this_tick?: number
  recovery_failures_this_tick?: number
  total_decisions: number
  total_redeploys: number
  total_failures: number
  total_recovery_attempts?: number
  total_recovery_success?: number
  total_recovery_failures?: number
  recovery_success_rate?: number
  latency_mean_ms: number
  latency_p50_ms: number
  latency_p95_ms: number
  latency_p99_ms: number
}

export interface DecisionTrace {
  mode?: 'single_request' | 'session_continuous'
  trigger?: string
  session_id?: string
  request_id: string
  core_network_id?: string
  core_network_label?: string
  source_node?: string
  destination_node?: string
  topology_version: number
  sim_time: string
  inference_time_ms: number
  requested_topk?: number
  returned_topk: number
  deployable_count: number
  fallback_only: boolean
  decision_process?: any
  request_vnfs?: RequestVNF[]
  request_core_nfs?: RequestVNF[]
  core_nf_dependencies?: CoreNFDependency[]
  candidates: Array<{
    score: number
    satisfies_constraints: boolean
    total_latency_ms: number
    registration_latency_ms?: number
    pdu_session_latency_ms?: number
    estimated_reliability: number
    bottleneck_bandwidth_gbps: number
    deployed_nodes?: string[]
    per_vnf?: Array<{
      vnf: string
      core_nf?: string
      nf_type?: string
      nf_role?: string
      node: string
      cpu_used: number
      mem_used: number
      disk_used?: number
    }>
    per_core_nf?: Array<{
      vnf: string
      core_nf?: string
      nf_type?: string
      nf_role?: string
      node: string
      cpu_used: number
      mem_used: number
      disk_used?: number
    }>
    link_details?: LinkDetail[]
    violation_details?: string[]
    reason?: string
  }>
}

export interface TopologyHistoryFrame {
  topology_version: number
  sim_time: string
  nodes: SatelliteData[]
  links: LinkData[]
  metrics: DynamicMetrics | null
}

export interface RuntimeEvent {
  id: string
  type: string
  sim_time?: string
  message: string
  raw?: any
}

export interface AutoDynamicsState {
  enabled: boolean
  playing: boolean
  auto_start_on_topology: boolean
  position_update_hz: number
  resource_update_sec: number
  time_scale: number
  elapsed_sec: number
  last_resource_sync_at: string
  snap_visual_token: number
}

export interface DisplaySettings {
  showTexture: boolean
  showBorders: boolean
  showLatLon: boolean
  showLinks: boolean
  linkOpacity: number
  showSky: boolean
  showAtmosphere: boolean
  showFPS: boolean
  renderQuality: 'high' | 'balanced' | 'performance'
  rotationSpeed: number
}
export interface Toast { id: string; message: string; type: 'info' | 'success' | 'warning' | 'error' }
export interface SystemPopup {
  open: boolean
  title: string
  message: string
  type: 'info' | 'success' | 'warning' | 'error'
}

interface Store {
  satellites: SatelliteData[]
  links: LinkData[]
  selectedSatellite: SatelliteData | null
  selectedLink: LinkData | null
  candidateResult: CandidateResult | null
  deployments: Deployment[]
  highlightedDeploymentIds: string[]
  topologyVersion: number
  backendTopologySynced: boolean
  display: DisplaySettings
  toasts: Toast[]
  systemPopup: SystemPopup
  simulation: {
    connected: boolean
    running: boolean
    sampling_interval_sec: number
    simulation_speed: number
    sim_time: string
    topology_version: number
    metrics: DynamicMetrics | null
    orchestration: OrchestrationMetrics | null
    view_mode: 'realtime' | 'playback'
    history: TopologyHistoryFrame[]
    history_cursor: number
  }
  decisionTraces: DecisionTrace[]
  runtimeEvents: RuntimeEvent[]
  suppressedSessionIds: string[]
  autoDynamics: AutoDynamicsState

  setSatellites: (s: SatelliteData[]) => void
  setLinks: (l: LinkData[]) => void
  setSelectedSatellite: (s: SatelliteData | null) => void
  setSelectedLink: (l: LinkData | null) => void
  setCandidateResult: (c: CandidateResult | null) => void
  bumpTopologyVersion: () => void
  setTopologyVersion: (v: number) => void
  setBackendTopologySynced: (synced: boolean) => void
  setDeployments: (deployments: Deployment[]) => void
  addDeployment: (d: Deployment) => void
  updateDeployment: (id: string, p: Partial<Deployment>) => void
  removeDeployment: (id: string) => void
  clearDeployments: () => void
  refreshDeploymentPaths: () => void
  toggleHighlightedDeployment: (id: string) => void
  clearHighlightedDeployments: () => void
  setDisplay: (s: Partial<DisplaySettings>) => void
  addToast: (msg: string, type?: Toast['type']) => void
  removeToast: (id: string) => void
  openSystemPopup: (title: string, message: string, type?: SystemPopup['type']) => void
  closeSystemPopup: () => void
  setSimulationStatus: (status: Partial<Store['simulation']>) => void
  setAutoDynamics: (patch: Partial<AutoDynamicsState>) => void
  setSimulationViewMode: (mode: 'realtime' | 'playback') => void
  setPlaybackCursor: (cursor: number) => void
  applyTopologySnapshot: (snapshot: any) => void
  pushDecisionTrace: (trace: DecisionTrace) => void
  pushRuntimeEvent: (evt: Omit<RuntimeEvent, 'id'>) => void
  upsertSessionDeploymentFromTrace: (trace: DecisionTrace) => void
  suppressSessionDeployment: (sessionId: string) => void
}

type PathGraph = {
  adj: Map<string, string[]>
  linkMap: Map<string, LinkData>
}

const DEFAULT_CORE_NF_DEPENDENCIES: CoreNFDependency[] = [
  { source: 'nrf', target: 'scp', bandwidth_scale: 0.55 },
  { source: 'scp', target: 'amf', bandwidth_scale: 0.72 },
  { source: 'scp', target: 'smf', bandwidth_scale: 0.76 },
  { source: 'scp', target: 'ausf', bandwidth_scale: 0.42 },
  { source: 'scp', target: 'udm', bandwidth_scale: 0.44 },
  { source: 'scp', target: 'pcf', bandwidth_scale: 0.34 },
  { source: 'scp', target: 'nssf', bandwidth_scale: 0.28 },
  { source: 'scp', target: 'bsf', bandwidth_scale: 0.24 },
  { source: 'scp', target: 'sepp', bandwidth_scale: 0.3 },
  { source: 'amf', target: 'ausf', bandwidth_scale: 0.46 },
  { source: 'amf', target: 'udm', bandwidth_scale: 0.48 },
  { source: 'amf', target: 'smf', bandwidth_scale: 0.86 },
  { source: 'amf', target: 'nssf', bandwidth_scale: 0.34 },
  { source: 'smf', target: 'upf', bandwidth_scale: 1.0 },
  { source: 'smf', target: 'pcf', bandwidth_scale: 0.44 },
  { source: 'smf', target: 'bsf', bandwidth_scale: 0.3 },
  { source: 'smf', target: 'udm', bandwidth_scale: 0.38 },
  { source: 'udm', target: 'udr', bandwidth_scale: 0.56 },
  { source: 'pcf', target: 'udr', bandwidth_scale: 0.34 },
  { source: 'pcf', target: 'bsf', bandwidth_scale: 0.28 },
]

function normalizeNfType(value: any): string {
  return String(value ?? '').trim().toLowerCase().replace(/[-\s]+/g, '_')
}

function linkKey(src: string, dst: string) {
  return `${src}|${dst}`
}

function remapSelectedLink(selectedLink: LinkData | null, links: LinkData[]): LinkData | null {
  if (!selectedLink) return null
  const src = String((selectedLink as any)?.source ?? '')
  const dst = String((selectedLink as any)?.target ?? '')
  if (!src || !dst || !Array.isArray(links)) return null
  for (const l of links as any[]) {
    const a = String(l?.source ?? '')
    const b = String(l?.target ?? '')
    if ((a === src && b === dst) || (a === dst && b === src)) {
      return l as LinkData
    }
  }
  return null
}

function remapSelectedSatellite(selectedSatellite: SatelliteData | null, satellites: SatelliteData[]): SatelliteData | null {
  if (!selectedSatellite) return null
  const selectedId = String((selectedSatellite as any)?.id ?? '')
  if (!selectedId || !Array.isArray(satellites)) return null
  for (const sat of satellites as any[]) {
    if (String(sat?.id ?? '') === selectedId) {
      return sat as SatelliteData
    }
  }
  return null
}

function isSatelliteDown(sat: SatelliteData | undefined): boolean {
  const status = String((sat as any)?.status ?? 'active').trim().toLowerCase()
  const faultTag = String((sat as any)?.fault_tag ?? '').trim()
  return status === 'down' || status === 'fault' || status === 'failed' || faultTag.length > 0
}

function buildAdjacency(links: LinkData[], satellites: SatelliteData[]): PathGraph {
  const downNodes = new Set<string>()
  satellites.forEach((sat: any) => {
    const id = String(sat?.id ?? '')
    if (!id) return
    if (isSatelliteDown(sat as SatelliteData)) downNodes.add(id)
  })
  const adj = new Map<string, string[]>()
  const linkMap = new Map<string, LinkData>()
  links.forEach((l) => {
    if (downNodes.has(l.source) || downNodes.has(l.target)) return
    const status = (l as any).status ?? 'active'
    const avail = Number((l as any).bandwidth_available_gbps ?? l.bandwidth_gbps ?? 0)
    if (status === 'down' || avail <= 0) return
    if (!adj.has(l.source)) adj.set(l.source, [])
    if (!adj.has(l.target)) adj.set(l.target, [])
    adj.get(l.source)!.push(l.target)
    adj.get(l.target)!.push(l.source)
    linkMap.set(linkKey(l.source, l.target), l)
    linkMap.set(linkKey(l.target, l.source), l)
  })
  return { adj, linkMap }
}

function shortestPath(src: string, dst: string, adj: Map<string, string[]>) {
  if (!src || !dst) return []
  if (src === dst) return [src]
  const prev = new Map<string, string>()
  const visited = new Set<string>([src])
  const queue: string[] = [src]
  let qi = 0
  while (qi < queue.length) {
    const cur = queue[qi++]
    const edges = adj.get(cur) ?? []
    for (const next of edges) {
      if (visited.has(next)) continue
      visited.add(next)
      prev.set(next, cur)
      if (next === dst) {
        qi = queue.length
        break
      }
      queue.push(next)
    }
  }

  if (!visited.has(dst)) return []
  const path: string[] = [dst]
  let cur = dst
  for (let i = 0; i < visited.size + 2; i++) {
    const p = prev.get(cur)
    if (!p) break
    path.push(p)
    if (p === src) break
    cur = p
  }
  if (path[path.length - 1] !== src) return []
  return path.reverse()
}

function dependencyListForDeployment(dep: Deployment): CoreNFDependency[] {
  return Array.isArray(dep.core_nf_dependencies) && dep.core_nf_dependencies.length > 0
    ? dep.core_nf_dependencies
    : DEFAULT_CORE_NF_DEPENDENCIES
}

function nfNodeMapForDeployment(dep: Deployment): Map<string, string> {
  const out = new Map<string, string>()
  ;(Array.isArray(dep.per_vnf) ? dep.per_vnf : []).forEach((p) => {
    const node = String(p?.node ?? '')
    if (!node) return
    const keys = [
      normalizeNfType(p?.nf_type),
      normalizeNfType(p?.core_nf),
      normalizeNfType(p?.vnf),
    ].filter(Boolean)
    keys.forEach((k) => out.set(k, node))
  })
  return out
}

function dependencySegmentsForDeployment(dep: Deployment) {
  const nodeByNf = nfNodeMapForDeployment(dep)
  const segments: Array<{ source: string; target: string; srcNode: string; dstNode: string; requiredBw: number }> = []
  dependencyListForDeployment(dep).forEach((d) => {
    const source = normalizeNfType(d?.source)
    const target = normalizeNfType(d?.target)
    const srcNode = nodeByNf.get(source) ?? ''
    const dstNode = nodeByNf.get(target) ?? ''
    if (!source || !target || !srcNode || !dstNode || srcNode === dstNode) return
    const requiredBw = Math.max(0, Number(d?.bandwidth_required_gbps ?? 0))
    segments.push({ source, target, srcNode, dstNode, requiredBw })
  })
  return segments
}

function buildAnchorsForDeployment(dep: Deployment): string[] {
  const seq = [
    dep.source_node ?? '',
    ...(Array.isArray(dep.deployed_nodes) ? dep.deployed_nodes : []),
    dep.destination_node ?? '',
  ].filter(Boolean)
  const compact = seq.filter((n, idx) => idx === 0 || n !== seq[idx - 1])
  if (compact.length >= 2) return compact

  const fromPathNodes = Array.isArray(dep.path_nodes) ? dep.path_nodes.filter(Boolean) : []
  if (fromPathNodes.length >= 2) return fromPathNodes
  return []
}

function followsSingleChain(dep: Deployment, anchors: string[]) {
  const details = Array.isArray(dep.link_details) ? dep.link_details : []
  if (details.length === 0 || anchors.length < 2) return false

  const nextMap = new Map<string, string>()
  const outDegree = new Map<string, number>()
  const inDegree = new Map<string, number>()
  for (const l of details) {
    const src = String(l?.src ?? '')
    const dst = String(l?.dst ?? '')
    if (!src || !dst || src === dst) return false
    if (!nextMap.has(src)) nextMap.set(src, dst)
    outDegree.set(src, (outDegree.get(src) || 0) + 1)
    inDegree.set(dst, (inDegree.get(dst) || 0) + 1)
    if ((outDegree.get(src) || 0) > 1) return false
    if ((inDegree.get(dst) || 0) > 1) return false
  }

  const start = String(anchors[0] ?? '')
  const end = String(anchors[anchors.length - 1] ?? '')
  if (!start || !end) return false

  const visitedEdges = new Set<string>()
  const orderedNodes: string[] = [start]
  let cur = start
  for (let i = 0; i < details.length + 2; i++) {
    const nxt = nextMap.get(cur)
    if (!nxt) break
    const ek = `${cur}|${nxt}`
    if (visitedEdges.has(ek)) return false
    visitedEdges.add(ek)
    orderedNodes.push(nxt)
    cur = nxt
  }
  if (orderedNodes[orderedNodes.length - 1] !== end) return false
  if (visitedEdges.size !== details.length) return false

  let anchorIdx = 0
  for (const n of orderedNodes) {
    if (n === anchors[anchorIdx]) anchorIdx += 1
    if (anchorIdx >= anchors.length) break
  }
  return anchorIdx >= anchors.length
}

function linksAreStillUsable(dep: Deployment, linkMap: Map<string, LinkData>) {
  if (!Array.isArray(dep.link_details) || dep.link_details.length === 0) return false
  for (const l of dep.link_details) {
    const cur = linkMap.get(linkKey(String(l.src), String(l.dst)))
    if (!cur) return false
    const status = String((cur as any)?.status ?? 'active')
    const avail = Number((cur as any)?.bandwidth_available_gbps ?? (cur as any)?.bandwidth_gbps ?? 0)
    const required = Number((l as any)?.bandwidth_required_gbps ?? 0)
    if (status === 'down' || avail <= 0) return false
    if (required > 1e-9 && avail + 1e-9 < required) return false
  }
  return true
}

function refreshLinkMetrics(linkDetails: LinkDetail[], linkMap: Map<string, LinkData>) {
  let changed = false
  const latencyEps = 0.8
  const bwEps = 0.35
  const reliabilityEps = 0.0015
  const refreshed = linkDetails.map((l) => {
    const cur = linkMap.get(linkKey(String(l.src), String(l.dst)))
    if (!cur) return l
    const next: LinkDetail = {
      src: String(l.src),
      dst: String(l.dst),
      dependency_source_nf: String(l.dependency_source_nf ?? ''),
      dependency_target_nf: String(l.dependency_target_nf ?? ''),
      latency_ms: Number((cur as any)?.latency_ms ?? l.latency_ms ?? 0),
      bandwidth_gbps: Number((cur as any)?.bandwidth_gbps ?? l.bandwidth_gbps ?? 0),
      bandwidth_available_gbps: Number((cur as any)?.bandwidth_available_gbps ?? l.bandwidth_available_gbps ?? l.bandwidth_gbps ?? 0),
      bandwidth_required_gbps: Number(l.bandwidth_required_gbps ?? 0),
      status: String((cur as any)?.status ?? l.status ?? 'active'),
      fault_tag: String((cur as any)?.fault_tag ?? l.fault_tag ?? ''),
      reliability: Number((cur as any)?.reliability ?? (cur as any)?.link_reliability ?? l.reliability ?? 0),
    }
    if (
      Math.abs((next.latency_ms ?? 0) - Number(l.latency_ms ?? 0)) > latencyEps ||
      Math.abs((next.bandwidth_available_gbps ?? 0) - Number(l.bandwidth_available_gbps ?? 0)) > bwEps ||
      Math.abs((next.reliability ?? 0) - Number(l.reliability ?? 0)) > reliabilityEps ||
      String(next.status ?? '') !== String(l.status ?? '')
    ) {
      changed = true
    }
    return next
  })
  return { refreshed, changed }
}

function rebuildDeploymentPath(dep: Deployment, graph: PathGraph): Deployment {
  const { adj, linkMap } = graph
  const anchors = buildAnchorsForDeployment(dep)
  const dependencySegments = dependencySegmentsForDeployment(dep)
  if (anchors.length < 2 && dependencySegments.length === 0) return dep

  const requiredByEdge = new Map<string, number>()
  const requiredSamples: number[] = []
  ;(Array.isArray(dep.link_details) ? dep.link_details : []).forEach((l) => {
    const req = Number(l?.bandwidth_required_gbps ?? 0)
    if (req <= 1e-9) return
    const a = String(l?.src ?? '')
    const b = String(l?.dst ?? '')
    if (!a || !b) return
    requiredByEdge.set(linkKey(a, b), req)
    requiredSamples.push(req)
  })
  const requiredFallback = requiredSamples.length > 0
    ? Math.max(...requiredSamples)
    : Math.max(0.1, Number(dep.score_constraints?.min_bandwidth_gbps ?? 0))

  if (dependencySegments.length > 0 && linksAreStillUsable(dep, linkMap)) {
    const currentLinks = Array.isArray(dep.link_details) ? dep.link_details : []
    const { refreshed, changed } = refreshLinkMetrics(currentLinks, linkMap)
    if (!changed) return dep
    const totalLatency = refreshed.reduce((acc, l) => acc + Number(l.latency_ms || 0), 0)
    return {
      ...dep,
      link_details: refreshed,
      total_latency_ms: totalLatency > 0 ? totalLatency : dep.total_latency_ms,
    }
  }

  if (dependencySegments.length > 0) {
    const newNodes: string[] = []
    const newLinks: LinkDetail[] = []
    let segmentFailed = false
    for (const seg of dependencySegments) {
      const segPath = shortestPath(seg.srcNode, seg.dstNode, adj)
      if (segPath.length < 2) {
        segmentFailed = true
        continue
      }
      segPath.forEach((node) => {
        if (newNodes[newNodes.length - 1] !== node) newNodes.push(node)
      })
      for (let k = 0; k + 1 < segPath.length; k++) {
        const src = segPath[k]
        const dst = segPath[k + 1]
        const lk = linkMap.get(linkKey(src, dst))
        const req = seg.requiredBw > 0 ? seg.requiredBw : requiredFallback
        newLinks.push({
          src,
          dst,
          dependency_source_nf: seg.source,
          dependency_target_nf: seg.target,
          latency_ms: Number((lk as any)?.latency_ms ?? 0),
          bandwidth_gbps: Number((lk as any)?.bandwidth_gbps ?? 0),
          bandwidth_available_gbps: Number((lk as any)?.bandwidth_available_gbps ?? 0),
          bandwidth_required_gbps: req,
          status: (lk as any)?.status ?? 'down',
          fault_tag: String((lk as any)?.fault_tag ?? ''),
          reliability: Number((lk as any)?.reliability ?? (lk as any)?.link_reliability ?? 0),
        })
      }
    }
    if (segmentFailed || newLinks.length === 0) {
      return {
        ...dep,
        link_details: [],
        total_latency_ms: 0,
      }
    }
    const totalLatency = newLinks.reduce((acc, l) => acc + Number(l.latency_ms || 0), 0)
    return {
      ...dep,
      path_nodes: newNodes,
      link_details: newLinks,
      total_latency_ms: totalLatency > 0 ? totalLatency : dep.total_latency_ms,
    }
  }

  const chainValid = followsSingleChain(dep, anchors)
  if (chainValid && linksAreStillUsable(dep, linkMap)) {
    const currentLinks = Array.isArray(dep.link_details) ? dep.link_details : []
    const { refreshed, changed } = refreshLinkMetrics(currentLinks, linkMap)
    if (!changed) return dep
    const totalLatency = refreshed.reduce((acc, l) => acc + Number(l.latency_ms || 0), 0)
    return {
      ...dep,
      link_details: refreshed,
      total_latency_ms: totalLatency > 0 ? totalLatency : dep.total_latency_ms,
    }
  }

  const newNodes: string[] = [anchors[0]]
  const newLinks: LinkDetail[] = []
  let segmentFailed = false
  for (let i = 0; i + 1 < anchors.length; i++) {
    const segPath = shortestPath(anchors[i], anchors[i + 1], adj)
    if (segPath.length < 2) {
      segmentFailed = true
      continue
    }
    for (let k = 0; k + 1 < segPath.length; k++) {
      const src = segPath[k]
      const dst = segPath[k + 1]
      if (newNodes[newNodes.length - 1] !== src) newNodes.push(src)
      newNodes.push(dst)
      const lk = linkMap.get(linkKey(src, dst))
      const req = Number(requiredByEdge.get(linkKey(src, dst)) ?? requiredFallback)
      newLinks.push({
        src,
        dst,
        latency_ms: Number((lk as any)?.latency_ms ?? 0),
        bandwidth_gbps: Number((lk as any)?.bandwidth_gbps ?? 0),
        bandwidth_available_gbps: Number((lk as any)?.bandwidth_available_gbps ?? 0),
        bandwidth_required_gbps: req,
        status: (lk as any)?.status ?? 'down',
        fault_tag: String((lk as any)?.fault_tag ?? ''),
        reliability: Number((lk as any)?.reliability ?? (lk as any)?.link_reliability ?? 0),
      })
    }
  }
  if (segmentFailed || newLinks.length === 0) {
    return {
      ...dep,
      link_details: [],
      total_latency_ms: 0,
    }
  }
  const totalLatency = newLinks.reduce((acc, l) => acc + Number(l.latency_ms || 0), 0)
  return {
    ...dep,
    path_nodes: newNodes,
    link_details: newLinks,
    total_latency_ms: totalLatency > 0 ? totalLatency : dep.total_latency_ms,
  }
}

function pathSignature(linkDetails: LinkDetail[] | undefined): string {
  if (!Array.isArray(linkDetails) || linkDetails.length === 0) return ''
  return linkDetails.map((l) => `${l.src}->${l.dst}`).join('|')
}

function decisionTraceSignature(trace: DecisionTrace): string {
  const mode = String(trace?.mode ?? '')
  const trigger = String(trace?.trigger ?? '')
  const sessionId = String(trace?.session_id ?? '')
  const requestId = String(trace?.request_id ?? '')
  const topo = Number(trace?.topology_version ?? 0)
  const simTime = String(trace?.sim_time ?? '')
  const ms = Number(trace?.inference_time_ms ?? 0).toFixed(3)
  const deployable = Number(trace?.deployable_count ?? 0)
  const returned = Number(trace?.returned_topk ?? 0)
  return [mode, trigger, sessionId, requestId, topo, simTime, ms, deployable, returned].join('|')
}

function pickTracePerVnf(trace: DecisionTrace): VNFDeploy[] {
  const requestVnfs = Array.isArray(trace?.request_core_nfs)
    ? trace.request_core_nfs
    : (Array.isArray(trace?.request_vnfs) ? trace.request_vnfs : [])
  const bestCandidate = Array.isArray(trace?.candidates)
    ? (trace.candidates.find((c: any) => !!c?.satisfies_constraints) ?? trace.candidates[0])
    : null
  const candidatePer = bestCandidate && Array.isArray((bestCandidate as any).per_core_nf)
    ? (bestCandidate as any).per_core_nf
    : ((bestCandidate && Array.isArray((bestCandidate as any).per_vnf)) ? (bestCandidate as any).per_vnf : [])
  if (bestCandidate && candidatePer.length > 0) {
    const fromCandidate = candidatePer.map((p: any, i: number) => {
      const spec = requestVnfs[i] ?? {}
      const cpu = Number(p?.cpu_used ?? (spec as any).cpu ?? 0)
      const mem = Number(p?.mem_used ?? (spec as any).mem ?? 0)
      const disk = Number(p?.disk_used ?? (spec as any).disk ?? Math.max(0, mem * 2.0))
      const coreNf = String(p?.core_nf ?? p?.vnf ?? p?.name ?? (spec as any).core_nf ?? (spec as any).name ?? `core-nf-${i + 1}`)
      return {
        vnf: coreNf,
        core_nf: coreNf,
        nf_type: String(p?.nf_type ?? (spec as any).nf_type ?? coreNf),
        nf_role: String(p?.nf_role ?? (spec as any).nf_role ?? ''),
        node: String(p?.node ?? ''),
        cpu_used: cpu,
        mem_used: mem,
        disk_used: disk,
      }
    })
    const hasAnyResource = fromCandidate.some((p) => p.cpu_used > 0 || p.mem_used > 0 || (p.disk_used ?? 0) > 0)
    if (hasAnyResource) return fromCandidate
    const deployedNodes = Array.isArray((bestCandidate as any).deployed_nodes)
      ? (bestCandidate as any).deployed_nodes.map((x: any) => String(x))
      : fromCandidate.map((p) => p.node)
    const filled = deployedNodes.map((node: string, i: number) => {
      const prev = fromCandidate[i]
      const spec = requestVnfs[i] ?? {}
      const coreNf = String(prev?.core_nf ?? prev?.vnf ?? (spec as any).core_nf ?? (spec as any).name ?? `core-nf-${i + 1}`)
      return {
        vnf: coreNf,
        core_nf: coreNf,
        nf_type: String(prev?.nf_type ?? (spec as any).nf_type ?? coreNf),
        nf_role: String(prev?.nf_role ?? (spec as any).nf_role ?? ''),
        node: node || String(prev?.node ?? ''),
        cpu_used: Number(prev?.cpu_used ?? (spec as any).cpu ?? 0),
        mem_used: Number(prev?.mem_used ?? (spec as any).mem ?? 0),
        disk_used: Number(prev?.disk_used ?? (spec as any).disk ?? 0),
      }
    })
    return filled
  }
  const attempts = Array.isArray(trace?.decision_process?.steps) ? trace.decision_process.steps : []
  const selectedAttempt = attempts.find((s: any) => !!s?.satisfies_constraints) ?? attempts[0]
  const per = Array.isArray(selectedAttempt?.decision_process?.per_core_nf)
    ? selectedAttempt.decision_process.per_core_nf
    : (Array.isArray(selectedAttempt?.decision_process?.per_vnf)
      ? selectedAttempt.decision_process.per_vnf
      : [])
  const selected = per.filter((p: any) => p?.status === 'selected' && p?.selected_node)
  return selected.map((p: any, i: number) => {
    const spec = requestVnfs[i] ?? {}
    const coreNf = String(p?.core_nf ?? p?.vnf_name ?? (spec as any).core_nf ?? (spec as any).name ?? `core-nf-${i + 1}`)
    return {
      vnf: coreNf,
      core_nf: coreNf,
      nf_type: String(p?.nf_type ?? (spec as any).nf_type ?? coreNf),
      nf_role: String(p?.nf_role ?? (spec as any).nf_role ?? ''),
      node: String(p.selected_node),
      cpu_used: Number(p?.cpu_used ?? (spec as any).cpu ?? 0),
      mem_used: Number(p?.mem_used ?? (spec as any).mem ?? 0),
      disk_used: Number(p?.disk_used ?? (spec as any).disk ?? 0),
    }
  })
}

function derivePathAnchors(trace: DecisionTrace, chosen: any, perVnf: VNFDeploy[]): string[] {
  const fromTrace = Array.isArray(chosen?.deployed_nodes) ? chosen.deployed_nodes : []
  const fromPerVnf = perVnf.map((p) => p.node).filter(Boolean)
  const core = fromTrace.length > 0 ? fromTrace : fromPerVnf
  const seq = [...core].filter(Boolean)
  return seq.filter((n, idx) => idx === 0 || n !== seq[idx - 1])
}

function pickTraceLinkDetails(chosen: any): LinkDetail[] {
  if (!Array.isArray(chosen?.link_details)) return []
  const out: LinkDetail[] = []
  chosen.link_details.forEach((l: any) => {
    const src = String(l?.src ?? '')
    const dst = String(l?.dst ?? '')
    if (!src || !dst || src === dst) return
    const dep = l?.core_nf_dependency ?? {}
    out.push({
      src,
      dst,
      dependency_source_nf: String(l?.dependency_source_nf ?? dep?.source ?? ''),
      dependency_target_nf: String(l?.dependency_target_nf ?? dep?.target ?? ''),
      latency_ms: Number(l?.latency_ms ?? 0),
      bandwidth_gbps: Number(l?.bandwidth_gbps ?? 0),
      bandwidth_available_gbps: Number(l?.bandwidth_available_gbps ?? 0),
      bandwidth_required_gbps: Number(l?.bandwidth_required_gbps ?? 0),
      status: String(l?.status ?? 'active'),
      fault_tag: String(l?.fault_tag ?? ''),
      reliability: Number(l?.reliability ?? 0),
    })
  })
  return out
}

function mergeNodesForContinuousMotion(current: SatelliteData[], incoming: any[]): SatelliteData[] {
  if (!Array.isArray(current) || current.length === 0) return Array.isArray(incoming) ? (incoming as SatelliteData[]) : []
  if (!Array.isArray(incoming) || incoming.length === 0) return current
  const byId = new Map<string, any>()
  incoming.forEach((n: any) => byId.set(String(n?.id ?? ''), n))
  return current.map((sat: any) => {
    const src = byId.get(String(sat.id))
    if (!src) return sat
    return {
      ...sat,
      cpu_total: Number(src.cpu_total ?? sat.cpu_total),
      cpu_available: Number(src.cpu_available ?? sat.cpu_available),
      mem_total: Number(src.mem_total ?? sat.mem_total),
      mem_available: Number(src.mem_available ?? sat.mem_available),
      disk_total: Number(src.disk_total ?? sat.disk_total),
      disk_available: Number(src.disk_available ?? sat.disk_available),
      core_network_load: Number(src.core_network_load ?? sat.core_network_load ?? 0),
      core_business_load: {
        signaling_load: Number(src.core_business_load?.signaling_load ?? sat.core_business_load?.signaling_load ?? src.core_network_load ?? sat.core_network_load ?? 0),
        session_load: Number(src.core_business_load?.session_load ?? sat.core_business_load?.session_load ?? src.core_network_load ?? sat.core_network_load ?? 0),
        user_plane_load: Number(src.core_business_load?.user_plane_load ?? sat.core_business_load?.user_plane_load ?? src.core_network_load ?? sat.core_network_load ?? 0),
        mobility_load: Number(src.core_business_load?.mobility_load ?? sat.core_business_load?.mobility_load ?? src.core_network_load ?? sat.core_network_load ?? 0),
        policy_load: Number(src.core_business_load?.policy_load ?? sat.core_business_load?.policy_load ?? src.core_network_load ?? sat.core_network_load ?? 0),
        auth_load: Number(src.core_business_load?.auth_load ?? sat.core_business_load?.auth_load ?? src.core_network_load ?? sat.core_network_load ?? 0),
      },
      node_reliability: Number(src.node_reliability ?? sat.node_reliability ?? 0.98),
      status: String(src.status ?? 'active'),
      fault_tag: String(src.fault_tag ?? ''),
      container_name: String(src.container_name ?? sat.container_name ?? ''),
      container_state: String(src.container_state ?? sat.container_state ?? 'stopped'),
      running_core_nf_types: Array.isArray(src.running_core_nf_types)
        ? src.running_core_nf_types.map((x: any) => String(x))
        : (Array.isArray(sat.running_core_nf_types) ? sat.running_core_nf_types.map((x: any) => String(x)) : []),
      running_core_nf_count: Number(src.running_core_nf_count ?? sat.running_core_nf_count ?? 0),
      service_probe_ok: Boolean(src.service_probe_ok ?? sat.service_probe_ok ?? false),
      deployed_sfc_names: Array.isArray(src.deployed_sfc_names)
        ? src.deployed_sfc_names.map((x: any) => String(x))
        : (Array.isArray(sat.deployed_sfc_names) ? sat.deployed_sfc_names.map((x: any) => String(x)) : []),
      deployed_core_nf_types: Array.isArray(src.deployed_core_nf_types)
        ? src.deployed_core_nf_types.map((x: any) => String(x))
        : (Array.isArray(sat.deployed_core_nf_types) ? sat.deployed_core_nf_types.map((x: any) => String(x)) : []),
      deployed_vnf_count: Number(src.deployed_vnf_count ?? sat.deployed_vnf_count ?? 0),
      vnfs: Array.isArray(src.core_nfs) ? src.core_nfs : (Array.isArray(src.vnfs) ? src.vnfs : sat.vnfs),
    }
  })
}

function mergeLinksForContinuousMotion(current: LinkData[], incoming: any[]): LinkData[] {
  if (!Array.isArray(current) || current.length === 0) return Array.isArray(incoming) ? (incoming as LinkData[]) : []
  if (!Array.isArray(incoming) || incoming.length === 0) return current
  const byKey = new Map<string, any>()
  incoming.forEach((l: any) => {
    const a = String(l?.source ?? '')
    const b = String(l?.target ?? '')
    byKey.set(linkKey(a, b), l)
    byKey.set(linkKey(b, a), l)
  })
  return current.map((lk: any) => {
    const src = byKey.get(linkKey(String(lk.source), String(lk.target)))
    if (!src) return lk
    const resourceStatus = String(src.status ?? lk.__resource_status ?? lk.status ?? 'active')
    const visualStatus = String(lk.status ?? 'active')
    return {
      ...lk,
      bandwidth_gbps: Number(src.bandwidth_gbps ?? lk.bandwidth_gbps ?? 0),
      bandwidth_available_gbps: Number(src.bandwidth_available_gbps ?? lk.bandwidth_available_gbps ?? lk.bandwidth_gbps ?? 0),
      reliability: Number(src.reliability ?? src.link_reliability ?? lk.reliability ?? 0.999),
      fault_tag: String(src.fault_tag ?? ''),
      __resource_status: resourceStatus,
      __resource_status_seed: resourceStatus,
      // Keep visual status stable between periodic backend syncs to avoid link flashing.
      status: visualStatus,
    }
  })
}

function toFinite(v: any, fallback = 0): number {
  const n = Number(v)
  return Number.isFinite(n) ? n : fallback
}

function hasSameNodeSet(current: SatelliteData[], incoming: any[]): boolean {
  if (!Array.isArray(current) || !Array.isArray(incoming)) return false
  if (current.length === 0 || incoming.length === 0) return false
  if (current.length !== incoming.length) return false
  const ids = new Set(current.map((s: any) => String(s?.id ?? '')).filter(Boolean))
  if (ids.size !== current.length) return false
  for (const src of incoming as any[]) {
    const id = String(src?.id ?? '')
    if (!id || !ids.has(id)) return false
  }
  return true
}

function hasSameLinkSet(current: LinkData[], incoming: any[]): boolean {
  if (!Array.isArray(current) || !Array.isArray(incoming)) return false
  if (current.length === 0 || incoming.length === 0) return false
  if (current.length !== incoming.length) return false
  const edgeSet = new Set<string>()
  current.forEach((l: any) => {
    const a = String(l?.source ?? '')
    const b = String(l?.target ?? '')
    if (!a || !b) return
    edgeSet.add(a < b ? `${a}|${b}` : `${b}|${a}`)
  })
  if (edgeSet.size !== current.length) return false
  for (const src of incoming as any[]) {
    const a = String(src?.source ?? '')
    const b = String(src?.target ?? '')
    if (!a || !b) return false
    const key = a < b ? `${a}|${b}` : `${b}|${a}`
    if (!edgeSet.has(key)) return false
  }
  return true
}

function orbitalLayoutChanged(current: SatelliteData[], incoming: any[]): boolean {
  if (!Array.isArray(current) || !Array.isArray(incoming) || current.length === 0 || incoming.length === 0) return true
  const byId = new Map<string, any>()
  current.forEach((sat: any) => byId.set(String(sat?.id ?? ''), sat))
  let checked = 0
  for (const src of incoming as any[]) {
    const id = String(src?.id ?? '')
    const cur = byId.get(id)
    if (!cur) return true
    const cOp: any = cur?.orbital_params ?? {}
    const nOp: any = src?.orbital_params ?? {}
    const samePlane = toFinite(cOp.plane, -1) === toFinite(nOp.plane, -1)
    const samePos = toFinite(cOp.position_in_plane, -1) === toFinite(nOp.position_in_plane, -1)
    const cIncl = toFinite(cOp.inclination ?? cOp.inclination_deg, 0)
    const nIncl = toFinite(nOp.inclination ?? nOp.inclination_deg, 0)
    const sameIncl = Math.abs(cIncl - nIncl) <= 1e-6
    const sameMeanMotion = Math.abs(toFinite(cOp.mean_motion_rev_per_day, 0) - toFinite(nOp.mean_motion_rev_per_day, 0)) <= 1e-7
    const sameEcc = Math.abs(toFinite(cOp.eccentricity, 0) - toFinite(nOp.eccentricity, 0)) <= 1e-8
    const sameEpoch = Math.abs(toFinite(cOp.epoch_jd, 0) - toFinite(nOp.epoch_jd, 0)) <= 1e-8
    const sameModel = String(cOp.propagation_model ?? 'SGP4') === String(nOp.propagation_model ?? 'SGP4')
    if (!(samePlane && samePos && sameIncl && sameMeanMotion && sameEcc && sameEpoch && sameModel)) return true
    checked += 1
    if (checked >= 32) break
  }
  return false
}

export const useStore = create<Store>((set, get) => ({
  satellites: [],
  links: [],
  selectedSatellite: null,
  selectedLink: null,
  candidateResult: null,
  deployments: [],
  highlightedDeploymentIds: [],
  toasts: [],
  systemPopup: {
    open: false,
    title: '',
    message: '',
    type: 'info',
  },
  topologyVersion: 0,
  backendTopologySynced: false,
  display: {
    showTexture: true,
    showBorders: false,
    showLatLon: false,
    showLinks: true,
    linkOpacity: 0.7,
    showSky: true,
    showAtmosphere: true,
    showFPS: false,
    renderQuality: 'high',
    rotationSpeed: 0,
  },
  simulation: {
    connected: false,
    running: false,
    sampling_interval_sec: 15,
    simulation_speed: 1,
    sim_time: '',
    topology_version: 0,
    metrics: null,
    orchestration: null,
    view_mode: 'realtime',
    history: [],
    history_cursor: -1,
  },
  decisionTraces: [],
  runtimeEvents: [],
  suppressedSessionIds: [],
  autoDynamics: {
    enabled: true,
    playing: true,
    auto_start_on_topology: true,
    position_update_hz: 4,
    resource_update_sec: 15,
    time_scale: 1,
    elapsed_sec: 0,
    last_resource_sync_at: '',
    snap_visual_token: 0,
  },

  setSatellites: (s) => set((st) => ({
    satellites: s,
    selectedSatellite: remapSelectedSatellite(st.selectedSatellite, s),
  })),
  setLinks: (l) => {
    set({ links: l, selectedLink: null })
    get().refreshDeploymentPaths()
  },
  setSelectedSatellite: (s) => set({ selectedSatellite: s }),
  setSelectedLink: (l) => set({ selectedLink: l }),
  setCandidateResult: (c) => set({ candidateResult: c }),
  bumpTopologyVersion: () => set((s) => ({ topologyVersion: s.topologyVersion + 1 })),
  setTopologyVersion: (v) => set({ topologyVersion: v }),
  setBackendTopologySynced: (synced) => set({ backendTopologySynced: synced }),
  setDeployments: (deployments) => {
    set((s) => {
      const nextDeployments = Array.isArray(deployments) ? deployments : []
      const knownKeys = new Set(
        s.deployments.flatMap((d) => [String(d.deployment_id ?? ''), String(d.backend_deployment_id ?? '')]).filter(Boolean)
      )

      const nextHighlighted = s.highlightedDeploymentIds.filter((id) =>
        nextDeployments.some((d) => d.deployment_id === id || d.backend_deployment_id === id)
      )
      const highlightedSet = new Set(nextHighlighted)

      nextDeployments.forEach((dep) => {
        if (dep.status === 'failed' || dep.status === 'rolled_back') return
        const key = String(dep.deployment_id ?? dep.backend_deployment_id ?? '')
        if (!key) return
        // Auto-highlight newly loaded deployments (including cold start after restart),
        // while preserving user's existing hide/show toggles for known deployments.
        if (!knownKeys.has(key) && !highlightedSet.has(key)) {
          highlightedSet.add(key)
          nextHighlighted.push(key)
        }
      })

      return {
        deployments: nextDeployments,
        highlightedDeploymentIds: nextHighlighted,
        runtimeEvents: nextDeployments.length === 0
          ? s.runtimeEvents.filter((evt) => {
              const type = String(evt.type)
              if (type === 'recovery_event') return String(evt.raw?.entity_type ?? '') !== 'session'
              return ![
                'deployment_action',
                'deployment_update',
                'deployment_runtime_update',
                'decision_trace',
                'session_update',
                'reschedule_trigger',
                'path_recompute_trigger',
                'planning_result',
              ].includes(type)
            })
          : s.runtimeEvents,
      }
    })
    get().refreshDeploymentPaths()
  },
  addDeployment: (d) => set((s) => {
    const idx = s.deployments.findIndex((x) => x.deployment_id === d.deployment_id)
    if (idx >= 0) {
      const merged = [...s.deployments]
      merged[idx] = { ...merged[idx], ...d }
      return { deployments: merged }
    }
    return { deployments: [d, ...s.deployments] }
  }),
  updateDeployment: (id, p) => set((s) => ({
    deployments: s.deployments.map((d) => (
      d.deployment_id === id || d.backend_deployment_id === id
        ? { ...d, ...p }
        : d
    )),
  })),
  removeDeployment: (id) => set((s) => {
    const removedIdSet = new Set<string>([id])
    s.deployments.forEach((d) => {
      if (d.deployment_id === id || d.backend_deployment_id === id) {
        removedIdSet.add(d.deployment_id)
        if (d.backend_deployment_id) removedIdSet.add(d.backend_deployment_id)
      }
    })
    return {
      deployments: s.deployments.filter((d) => d.deployment_id !== id && d.backend_deployment_id !== id),
      highlightedDeploymentIds: s.highlightedDeploymentIds.filter((x) => !removedIdSet.has(x)),
    }
  }),
  clearDeployments: () => set({ deployments: [], highlightedDeploymentIds: [] }),
  refreshDeploymentPaths: () => set((s) => {
    if (s.deployments.length === 0) return s
    const graph = buildAdjacency(s.links, s.satellites)
    const topoV = Number(s.simulation.topology_version || s.topologyVersion || 0)
    let stateChanged = false
    const nextDeployments = s.deployments.map((dep) => {
      const rebuilt = rebuildDeploymentPath(dep, graph)
      const oldSig = pathSignature(dep.link_details)
      const newSig = pathSignature(rebuilt.link_details)
      const pathChanged = oldSig !== newSig
      const hasBoundVersion = Number.isFinite(Number(dep.topology_version_bound))
      const trackRecompute = hasBoundVersion && (dep.strategy_mode === 'session_continuous' || dep.path_recompute_count != null)
      const nextRecomputeCountRaw = (dep.path_recompute_count ?? 0) + (pathChanged && trackRecompute ? 1 : 0)
      const nextRecomputeCount = dep.path_recompute_count == null && !trackRecompute ? undefined : nextRecomputeCountRaw
      const topologyChanged = Number(dep.topology_version_bound ?? -1) !== topoV
      const deploymentChanged =
        rebuilt !== dep ||
        dep.path_recompute_count !== nextRecomputeCount ||
        topologyChanged ||
        dep.previous_link_details !== undefined ||
        dep.path_transition_until !== undefined

      if (!deploymentChanged) return dep
      stateChanged = true
      return {
        ...rebuilt,
        topology_version_bound: topoV,
        path_recompute_count: nextRecomputeCount,
        previous_link_details: undefined,
        path_transition_until: undefined,
      }
    })
    if (!stateChanged) return s
    return { ...s, deployments: nextDeployments }
  }),
  toggleHighlightedDeployment: (id) => set((s) => ({
    highlightedDeploymentIds: s.highlightedDeploymentIds.includes(id)
      ? s.highlightedDeploymentIds.filter((x) => x !== id)
      : [...s.highlightedDeploymentIds, id],
  })),
  clearHighlightedDeployments: () => set({ highlightedDeploymentIds: [] }),
  setDisplay: (s) => set((st) => ({ display: { ...st.display, ...s } })),
  addToast: (msg, type = 'info') => {
    const id = Date.now().toString()
    set((s) => ({ toasts: [...s.toasts, { id, message: msg, type }] }))
    setTimeout(() => set((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) })), 4000)
  },
  removeToast: (id) => set((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) })),
  openSystemPopup: (title, message, type = 'info') => set({
    systemPopup: { open: true, title, message, type },
  }),
  closeSystemPopup: () => set((s) => ({
    systemPopup: { ...s.systemPopup, open: false },
  })),
  setSimulationStatus: (status) => set((s) => ({ simulation: { ...s.simulation, ...status } })),
  setAutoDynamics: (patch) => set((s) => {
    const next = { ...s.autoDynamics, ...patch }
    const sampling = Number(next.resource_update_sec ?? s.autoDynamics.resource_update_sec ?? 15)
    next.resource_update_sec = Math.max(10, Math.min(30, Number.isFinite(sampling) ? sampling : 15))
    const speed = Number(next.time_scale ?? s.autoDynamics.time_scale ?? 1)
    next.time_scale = Math.max(0.1, Math.min(8, Number.isFinite(speed) ? speed : 1))
    return { autoDynamics: next }
  }),
  setSimulationViewMode: (mode) => {
    set((s) => {
      const next = { ...s.simulation, view_mode: mode }
      if (mode === 'realtime') {
        const idx = next.history.length - 1
        if (idx >= 0) {
          const frame = next.history[idx]
          const nextLinks = frame.links as LinkData[]
          const nextSats = frame.nodes as SatelliteData[]
          return {
            simulation: { ...next, history_cursor: idx },
            satellites: nextSats,
            links: nextLinks,
            selectedSatellite: remapSelectedSatellite(s.selectedSatellite, nextSats),
            selectedLink: remapSelectedLink(s.selectedLink, nextLinks),
            topologyVersion: frame.topology_version || s.topologyVersion,
          }
        }
      }
      return { simulation: next }
    })
    get().refreshDeploymentPaths()
  },
  setPlaybackCursor: (cursor) => {
    set((s) => {
      const history = s.simulation.history
      if (history.length === 0) return {}
      const idx = Math.max(0, Math.min(history.length - 1, cursor))
      const frame = history[idx]
      const nextLinks = frame.links as LinkData[]
      const nextSats = frame.nodes as SatelliteData[]
      return {
        simulation: {
          ...s.simulation,
          view_mode: 'playback',
          history_cursor: idx,
          sim_time: frame.sim_time || s.simulation.sim_time,
          topology_version: frame.topology_version || s.simulation.topology_version,
          metrics: frame.metrics ?? s.simulation.metrics,
        },
        satellites: nextSats,
        links: nextLinks,
        selectedSatellite: remapSelectedSatellite(s.selectedSatellite, nextSats),
        selectedLink: remapSelectedLink(s.selectedLink, nextLinks),
        topologyVersion: frame.topology_version || s.topologyVersion,
      }
    })
    get().refreshDeploymentPaths()
  },
  applyTopologySnapshot: (snapshot) => {
    const nodes = snapshot?.topology?.nodes ?? snapshot?.nodes ?? []
    const links = snapshot?.topology?.links ?? snapshot?.links ?? []
    const metrics = snapshot?.metrics ?? null
    const topoVersion = Number(snapshot?.topology_version ?? snapshot?.metadata?.topology_version ?? 0)
    const simTime = String(snapshot?.sim_time ?? snapshot?.metadata?.sim_time ?? '')

    set((s) => {
      const nodeCount = Array.isArray(nodes) ? nodes.length : 0
      const sameNodes = hasSameNodeSet(s.satellites, Array.isArray(nodes) ? nodes : [])
      const sameLinks = hasSameLinkSet(s.links, Array.isArray(links) ? links : [])
      const layoutChanged = sameNodes ? orbitalLayoutChanged(s.satellites, Array.isArray(nodes) ? nodes : []) : true
      const preserveVisualMotion =
        s.simulation.view_mode === 'realtime' &&
        sameNodes &&
        sameLinks &&
        !layoutChanged &&
        s.satellites.length > 0 &&
        s.links.length > 0
      const mergedNodes = preserveVisualMotion
        ? mergeNodesForContinuousMotion(s.satellites, Array.isArray(nodes) ? nodes : [])
        : nodes
      const mergedLinks = preserveVisualMotion
        ? mergeLinksForContinuousMotion(s.links, Array.isArray(links) ? links : [])
        : links
      const frame: TopologyHistoryFrame | null =
        Array.isArray(nodes) && Array.isArray(links)
          ? {
              topology_version: topoVersion > 0 ? topoVersion : s.topologyVersion + 1,
              sim_time: simTime || s.simulation.sim_time,
              nodes: mergedNodes as SatelliteData[],
              links: mergedLinks as LinkData[],
              metrics,
            }
          : null
      const maxHistoryFrames = nodeCount >= 5000 ? 6 : (nodeCount >= 3000 ? 12 : 120)
      const history = frame
        ? [...s.simulation.history, frame].slice(-maxHistoryFrames)
        : s.simulation.history
      const tailIdx = history.length - 1
      const realtime = s.simulation.view_mode === 'realtime'
      const activeFrame = realtime && tailIdx >= 0 ? history[tailIdx] : null
      const activeLinks = (activeFrame ? activeFrame.links : s.links) as LinkData[]
      return {
        satellites: activeFrame ? activeFrame.nodes : s.satellites,
        links: activeLinks,
        selectedSatellite: remapSelectedSatellite(
          s.selectedSatellite,
          (activeFrame ? activeFrame.nodes : s.satellites) as SatelliteData[]
        ),
        selectedLink: remapSelectedLink(s.selectedLink, activeLinks),
        topologyVersion: activeFrame
          ? activeFrame.topology_version
          : (topoVersion > 0 ? topoVersion : s.topologyVersion + 1),
        backendTopologySynced: true,
        simulation: {
          ...s.simulation,
          sim_time: simTime || s.simulation.sim_time,
          topology_version: topoVersion > 0 ? topoVersion : s.simulation.topology_version,
          metrics: metrics ?? s.simulation.metrics,
          history,
          history_cursor: realtime ? tailIdx : s.simulation.history_cursor,
        },
      }
    })
    get().refreshDeploymentPaths()
  },
  pushDecisionTrace: (trace) => set((s) => ({
    decisionTraces: (() => {
      const sig = decisionTraceSignature(trace)
      const duplicated = s.decisionTraces.some((t) => decisionTraceSignature(t) === sig)
      if (duplicated) return s.decisionTraces
      return [trace, ...s.decisionTraces].slice(0, 80)
    })(),
  })),
  pushRuntimeEvent: (evt) => set((s) => ({
    runtimeEvents: [{ ...evt, id: `${Date.now()}_${Math.random().toString(16).slice(2, 6)}` }, ...s.runtimeEvents].slice(0, 120),
  })),
  upsertSessionDeploymentFromTrace: (trace) => set((s) => {
    if (trace.mode !== 'session_continuous' || !trace.session_id) return {}
    if (s.suppressedSessionIds.includes(trace.session_id)) return {}
    const depId = `sess-deploy-${trace.session_id}`
    const nowIso = new Date().toISOString()
    const topoV = Number(trace.topology_version || s.topologyVersion || 0)
    const candidates = Array.isArray(trace.candidates) ? trace.candidates : []
    const chosen = candidates.find((c: any) => !!c?.satisfies_constraints) ?? null
    const perVnf = pickTracePerVnf(trace)

    const existing = s.deployments.find((d) => d.deployment_id === depId)
    const incomingInference = Number(trace.inference_time_ms ?? Number.NaN)
    const existingInference = Number(existing?.inference_latency_ms ?? Number.NaN)
    const resolvedInference =
      Number.isFinite(incomingInference) && incomingInference > 0
        ? incomingInference
        : (Number.isFinite(existingInference) ? existingInference : 0)
    if (!chosen) {
      if (!existing) return {}
      return {
        deployments: s.deployments.map((dep) =>
          dep.deployment_id === depId
            ? {
                ...dep,
                status: 'in-progress',
                progress: 70,
                satisfies_constraints: false,
                decision_trigger: trace.trigger,
                topology_version_bound: topoV,
                deployed_at: nowIso,
              }
            : dep
        ),
      }
    }

    const anchors = derivePathAnchors(trace, chosen, perVnf)
    const base: Deployment = {
      deployment_id: depId,
      backend_deployment_id: existing?.backend_deployment_id,
      core_network_id: existing?.core_network_id ?? String(trace.core_network_id ?? depId),
      core_network_label: existing?.core_network_label ?? String(trace.core_network_label ?? ''),
      request_id: trace.request_id,
      sfc_name: existing?.core_network_label ?? String(trace.core_network_label ?? existing?.sfc_name ?? `核心网策略 ${trace.request_id}`),
      candidate_index: 0,
      status: chosen.satisfies_constraints ? 'completed' : 'in-progress',
      inference_latency_ms: resolvedInference,
      path_nodes: anchors,
      satisfies_constraints: !!chosen.satisfies_constraints,
      violation_details: Array.isArray(chosen.violation_details) ? chosen.violation_details : [],
      bottleneck_bandwidth_gbps: Number(chosen.bottleneck_bandwidth_gbps ?? 0),
      estimated_reliability: Number(chosen.estimated_reliability ?? 0),
      score_total: Number(chosen.score ?? 0),
      score_constraints: existing?.score_constraints,
      score_weights: existing?.score_weights,
      score_breakdown: existing?.score_breakdown,
      deployed_nodes: Array.isArray(chosen.deployed_nodes) ? chosen.deployed_nodes : perVnf.map((p) => p.node),
      per_vnf: perVnf.length > 0 ? perVnf : (existing?.per_vnf ?? []),
      link_details: pickTraceLinkDetails(chosen),
      core_nf_dependencies: Array.isArray(trace.core_nf_dependencies)
        ? trace.core_nf_dependencies
        : existing?.core_nf_dependencies,
      previous_link_details: undefined,
      total_latency_ms: Number(chosen.total_latency_ms ?? existing?.total_latency_ms ?? 0),
      registration_latency_ms: Number(chosen.registration_latency_ms ?? existing?.registration_latency_ms ?? 0),
      pdu_session_latency_ms: Number(chosen.pdu_session_latency_ms ?? existing?.pdu_session_latency_ms ?? 0),
      deployed_at: nowIso,
      progress: 100,
      strategy_mode: 'session_continuous',
      session_id: trace.session_id,
      topology_version_bound: topoV,
      path_recompute_count: existing?.path_recompute_count ?? 0,
      decision_trigger: trace.trigger,
      path_transition_until: undefined,
    }

    const rebuilt = rebuildDeploymentPath(base, buildAdjacency(s.links, s.satellites))
    const hasExisting = !!existing
    const deployments = hasExisting
      ? s.deployments.map((dep) => (dep.deployment_id === depId ? rebuilt : dep))
      : [
          rebuilt,
          ...s.deployments.filter((dep) =>
            !(dep.request_id === trace.request_id && !dep.session_id)
          ),
        ]

    const highlighted = s.highlightedDeploymentIds.includes(depId)
      ? s.highlightedDeploymentIds
      : [depId, ...s.highlightedDeploymentIds]

    return { deployments, highlightedDeploymentIds: highlighted }
  }),
  suppressSessionDeployment: (sessionId) => set((s) => ({
    suppressedSessionIds: s.suppressedSessionIds.includes(sessionId)
      ? s.suppressedSessionIds
      : [sessionId, ...s.suppressedSessionIds].slice(0, 200),
    deployments: s.deployments.filter((d) => d.session_id !== sessionId),
    highlightedDeploymentIds: s.highlightedDeploymentIds.filter((id) => {
      const dep = s.deployments.find((d) => d.deployment_id === id)
      return dep?.session_id !== sessionId
    }),
  })),
}))
