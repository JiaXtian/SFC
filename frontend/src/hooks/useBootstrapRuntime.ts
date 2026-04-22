import { useEffect, useRef } from 'react'
import { apiClient } from '@/api/client'
import { useStore, type Deployment } from '@/store/useStore'

function toNumber(v: any, fallback = 0) {
  const n = Number(v)
  return Number.isFinite(n) ? n : fallback
}

function toPositiveNumberOrUndefined(v: any) {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return undefined
  return n
}

function normalizeDeployment(raw: any): Deployment {
  const deploymentId = String(raw?.deployment_id ?? raw?.backend_deployment_id ?? `deploy-${Date.now()}`)
  const backendId = String(raw?.backend_deployment_id ?? raw?.deployment_id ?? deploymentId)
  return {
    deployment_id: deploymentId,
    backend_deployment_id: backendId,
    request_id: String(raw?.request_id ?? ''),
    sfc_name: String(raw?.sfc_name ?? raw?.request_id ?? deploymentId),
    candidate_index: toNumber(raw?.candidate_index ?? 0, 0),
    status: String(raw?.status ?? 'completed') as Deployment['status'],
    inference_latency_ms: toPositiveNumberOrUndefined(raw?.inference_latency_ms),
    source_node: String(raw?.source_node ?? ''),
    destination_node: String(raw?.destination_node ?? ''),
    path_nodes: Array.isArray(raw?.path_nodes) ? raw.path_nodes.map((x: any) => String(x)) : [],
    satisfies_constraints: raw?.satisfies_constraints == null ? undefined : Boolean(raw?.satisfies_constraints),
    violation_details: Array.isArray(raw?.violation_details) ? raw.violation_details.map((x: any) => String(x)) : [],
    bottleneck_bandwidth_gbps: toNumber(raw?.bottleneck_bandwidth_gbps ?? 0, 0),
    estimated_reliability: toNumber(raw?.estimated_reliability ?? 0, 0),
    score_total: toNumber(raw?.score_total ?? 0, 0),
    score_breakdown: raw?.score_breakdown && typeof raw.score_breakdown === 'object'
      ? {
          latency: toNumber(raw?.score_breakdown?.latency ?? 0, 0),
          resource: toNumber(raw?.score_breakdown?.resource ?? 0, 0),
          reliability: toNumber(raw?.score_breakdown?.reliability ?? 0, 0),
          bandwidth: toNumber(raw?.score_breakdown?.bandwidth ?? 0, 0),
          dispersion: toNumber(raw?.score_breakdown?.dispersion ?? 0, 0),
        }
      : undefined,
    score_weights: raw?.score_weights && typeof raw.score_weights === 'object'
      ? {
          latency: toNumber(raw?.score_weights?.latency ?? 0, 0),
          resource: toNumber(raw?.score_weights?.resource ?? 0, 0),
          reliability: toNumber(raw?.score_weights?.reliability ?? 0, 0),
          bandwidth: toNumber(raw?.score_weights?.bandwidth ?? 0, 0),
          dispersion: toNumber(raw?.score_weights?.dispersion ?? 0, 0),
        }
      : undefined,
    score_constraints: raw?.score_constraints && typeof raw.score_constraints === 'object'
      ? {
          max_latency_ms: toNumber(raw?.score_constraints?.max_latency_ms ?? 0, 0),
          min_bandwidth_gbps: toNumber(raw?.score_constraints?.min_bandwidth_gbps ?? 0, 0),
          min_reliability: toNumber(raw?.score_constraints?.min_reliability ?? 0, 0),
        }
      : undefined,
    deployed_nodes: Array.isArray(raw?.deployed_nodes) ? raw.deployed_nodes.map((x: any) => String(x)) : [],
    per_vnf: Array.isArray(raw?.per_core_nf)
      ? raw.per_core_nf
      : (Array.isArray(raw?.per_vnf) ? raw.per_vnf : []),
    link_details: Array.isArray(raw?.link_details) ? raw.link_details : [],
    total_latency_ms: toNumber(raw?.total_latency_ms ?? 0, 0),
    deployed_at: String(raw?.deployed_at ?? new Date().toISOString()),
    progress: toNumber(raw?.progress ?? 100, 100),
    strategy_mode: String(raw?.strategy_mode ?? 'single_request') as Deployment['strategy_mode'],
    session_id: raw?.session_id ? String(raw.session_id) : undefined,
    topology_version_bound: raw?.topology_version_bound == null ? undefined : toNumber(raw.topology_version_bound, 0),
    path_recompute_count: raw?.path_recompute_count == null ? undefined : toNumber(raw.path_recompute_count, 0),
    decision_trigger: raw?.decision_trigger ? String(raw.decision_trigger) : undefined,
  }
}

export function useBootstrapRuntime() {
  const bootstrappedRef = useRef(false)

  useEffect(() => {
    if (bootstrappedRef.current) return
    bootstrappedRef.current = true

    const st = useStore.getState()
    const {
      applyTopologySnapshot,
      setDeployments,
      setSimulationStatus,
      setAutoDynamics,
      setBackendTopologySynced,
    } = st

    ;(async () => {
      const [topologyRes, deploymentsRes, statusRes, configRes] = await Promise.allSettled([
        apiClient.getTopology(),
        apiClient.getDeployments(),
        apiClient.getDynamicStatus(),
        apiClient.getControlConfig(),
      ])

      if (topologyRes.status === 'fulfilled') {
        const topo = topologyRes.value
        applyTopologySnapshot(topo)
        const nodes = topo?.topology?.nodes ?? topo?.nodes ?? []
        setBackendTopologySynced(Array.isArray(nodes) && nodes.length > 0)
      }

      if (deploymentsRes.status === 'fulfilled' && Array.isArray(deploymentsRes.value)) {
        const deployments = deploymentsRes.value.map(normalizeDeployment)
        setDeployments(deployments)
      }

      if (statusRes.status === 'fulfilled') {
        const status = statusRes.value ?? {}
        setSimulationStatus({
          running: Boolean(status?.running),
          sampling_interval_sec: toNumber(status?.sampling_interval_sec ?? 5, 5),
          simulation_speed: toNumber(status?.simulation_speed ?? 1, 1),
          topology_version: toNumber(status?.topology_version ?? 0, 0),
          sim_time: String(status?.sim_time ?? ''),
          metrics: status?.metrics ?? null,
        })
      }

      if (configRes.status === 'fulfilled') {
        const cfg = configRes.value ?? {}
        setAutoDynamics({
          resource_update_sec: Math.max(1, Math.min(60, toNumber(cfg?.resource_sampling_interval_sec ?? 5, 5))),
          time_scale: Math.max(0.1, Math.min(20, toNumber(cfg?.simulation_speed ?? 1, 1))),
        })
      }
    })()
  }, [])
}
