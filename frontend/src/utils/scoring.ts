export type ScoreConstraints = {
  max_latency_ms: number
  min_bandwidth_gbps: number
  min_reliability: number
}

export type ScoreWeights = {
  latency: number
  resource: number
  reliability: number
  bandwidth: number
}

export type ScoreBreakdown = {
  latencyScore: number
  resourceScore: number
  reliabilityScore: number
  bandwidthScore: number
  total: number
}

export function normalizeWeights(weights: ScoreWeights): ScoreWeights {
  const sum = weights.latency + weights.resource + weights.reliability + weights.bandwidth
  const norm = sum > 1e-9 ? sum : 1
  return {
    latency: weights.latency / norm,
    resource: weights.resource / norm,
    reliability: weights.reliability / norm,
    bandwidth: weights.bandwidth / norm,
  }
}

function clamp01(v: number): number {
  return Math.max(0, Math.min(1, v))
}

export function scoreLatency(latencyMs: number, maxLatencyMs: number): number {
  if (maxLatencyMs <= 0) return 1
  const ratio = Math.max(0, latencyMs) / maxLatencyMs
  if (ratio <= 1) return clamp01(1 - 0.35 * Math.pow(ratio, 0.8))
  return clamp01(0.65 * Math.exp(-1.7 * (ratio - 1)))
}

export function scoreBandwidth(bottleneckGbps: number, minBandwidthGbps: number): number {
  if (minBandwidthGbps <= 0) return 1
  const ratio = Math.max(0, bottleneckGbps) / minBandwidthGbps
  if (ratio >= 1) return clamp01(0.65 + 0.35 * (1 - Math.exp(-1.2 * (ratio - 1))))
  return clamp01(0.65 * Math.pow(ratio, 0.9))
}

export function scoreReliability(estimatedReliability: number, minReliability: number): number {
  if (minReliability <= 0) return 1
  const ratio = Math.max(0, estimatedReliability) / minReliability
  if (ratio >= 1) return clamp01(0.7 + 0.3 * (1 - Math.exp(-4 * (ratio - 1))))
  return clamp01(0.7 * Math.pow(ratio, 3))
}

export function scoreResource(nodes: any[]): number {
  if (!Array.isArray(nodes) || nodes.length === 0) return 1
  const avg = nodes.reduce((sum, node) => {
    const cpu = node.cpu_total > 0 ? node.cpu_available / node.cpu_total : 0
    const mem = node.mem_total > 0 ? node.mem_available / node.mem_total : 0
    const disk = node.disk_total > 0 ? node.disk_available / node.disk_total : 0
    return sum + (cpu + mem + disk) / 3
  }, 0)
  return clamp01(avg / nodes.length)
}

export function computeScoreBreakdown(args: {
  totalLatencyMs: number
  bottleneckBandwidthGbps: number
  estimatedReliability: number
  deployedNodeIds: string[]
  constraints: ScoreConstraints
  weights: ScoreWeights
  satellites: any[]
}): ScoreBreakdown {
  const {
    totalLatencyMs,
    bottleneckBandwidthGbps,
    estimatedReliability,
    deployedNodeIds,
    constraints,
    weights,
    satellites,
  } = args

  const nodes = (deployedNodeIds || [])
    .map(id => (satellites || []).find((s: any) => s.id === id))
    .filter(Boolean)
  const latencyScore = scoreLatency(totalLatencyMs, constraints.max_latency_ms)
  const bandwidthScore = scoreBandwidth(bottleneckBandwidthGbps, constraints.min_bandwidth_gbps)
  const reliabilityScore = scoreReliability(estimatedReliability, constraints.min_reliability)
  const resourceScore = scoreResource(nodes)

  const total =
    weights.latency * latencyScore +
    weights.resource * resourceScore +
    weights.reliability * reliabilityScore +
    weights.bandwidth * bandwidthScore

  return {
    latencyScore,
    resourceScore,
    reliabilityScore,
    bandwidthScore,
    total: clamp01(total),
  }
}
