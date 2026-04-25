import { useEffect, useRef } from 'react'
import { useStore } from '@/store/useStore'
import { resolveSfcLabel } from '@/utils/sfcLabel'
import { getAuthToken } from '@/auth/session'
import { apiClient } from '@/api/client'

function defaultWsUrl() {
  const envUrl = (import.meta as any)?.env?.VITE_WS_URL?.trim?.()
  const token = getAuthToken()
  if (envUrl) {
    if (!token) return envUrl
    const sep = envUrl.includes('?') ? '&' : '?'
    return `${envUrl}${sep}token=${encodeURIComponent(token)}`
  }

  const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws'
  const base = `${protocol}://${window.location.host}/ws/updates`
  if (!token) return base
  return `${base}?token=${encodeURIComponent(token)}`
}

function faultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    power_failure: '供电故障',
    cpu_overload: '计算过载',
    thermal_shutdown: '过热停机',
    control_plane_sync_loss: '控制面失步',
    software_crash: '软件崩溃',
    clock_drift: '时钟漂移',
    optical_signal_loss: '光链路信号丢失',
    beam_misalignment: '波束失准',
    interference_jamming: '链路干扰',
    routing_blackhole: '路由黑洞',
    transceiver_failure: '收发器故障',
    line_degradation: '链路退化',
    endpoint_node_fault: '端点节点故障',
    line_of_sight_loss: '视距中断',
  }
  return map[tag] ?? tag
}

function sfcLabelByIds(sessionId: string, requestId?: string): string {
  const st = useStore.getState()
  return resolveSfcLabel(st.deployments as any, {
    sessionId,
    requestId: String(requestId ?? ''),
  })
}

function sfcLabelByDeploymentId(deploymentId: string): string {
  const st = useStore.getState()
  return resolveSfcLabel(st.deployments as any, {
    deploymentId: String(deploymentId ?? ''),
  })
}

export function useWebSocket(options: { applyTopologySnapshot?: boolean } = {}) {
  const ref = useRef<WebSocket | null>(null)
  const reconnectRef = useRef<number | null>(null)
  const endpointFaultPopupCooldownRef = useRef<Record<string, number>>({})
  const applyTopologySnapshotEnabled = options.applyTopologySnapshot !== false

  useEffect(() => {
    const state = useStore.getState()
    const {
      setSimulationStatus,
      updateDeployment,
      removeDeployment,
      setDeployments,
      applyTopologySnapshot,
      pushDecisionTrace,
      upsertSessionDeploymentFromTrace,
      pushRuntimeEvent,
      addToast,
    } = state

    let closedByUnmount = false

    const connect = () => {
      try {
        const ws = new WebSocket(defaultWsUrl())
        ref.current = ws

        ws.onopen = () => {
          setSimulationStatus({ connected: true })
          pushRuntimeEvent({ type: 'ws', message: '实时事件通道已连接' })
        }

        ws.onmessage = (evt) => {
          try {
            const data = JSON.parse(evt.data)
            const type = String(data?.type ?? '')
            if (type === 'deployment_update') {
              const deploymentId = String(data.deployment_id ?? '')
              const sfcLabel = sfcLabelByDeploymentId(deploymentId)
              const status = String(data.status ?? 'completed')
              if (status === 'rolled_back') {
                removeDeployment(deploymentId)
                const removedIds = Array.isArray(data?.removed_deployment_ids) ? data.removed_deployment_ids : []
                removedIds.forEach((id: any) => {
                  const rid = String(id ?? '')
                  if (rid) removeDeployment(rid)
                })
                ;(async () => {
                  try {
                    const [depList, topo] = await Promise.all([
                      apiClient.getDeployments(),
                      apiClient.getTopology(),
                    ])
                    if (Array.isArray(depList)) {
                      setDeployments(depList as any)
                    }
                    if (applyTopologySnapshotEnabled) {
                      applyTopologySnapshot(topo)
                    } else {
                      const topoRaw = topo?.topology ?? topo
                      const meta = topoRaw?.metadata ?? {}
                      setSimulationStatus({
                        sim_time: String(meta?.sim_time ?? ''),
                        topology_version: Number(meta?.topology_version ?? 0),
                      })
                    }
                    window.dispatchEvent(new Event('satellite-table-refresh'))
                  } catch {
                    // ignore sync failures
                  }
                })()
              } else {
                updateDeployment(deploymentId, {
                  status: status as any,
                  progress: Number(data.progress ?? 100),
                })
              }
              pushRuntimeEvent({
                type: 'deployment_update',
                sim_time: data.sim_time,
                message: `部署进度 ${sfcLabel}: ${data.status ?? 'completed'} (${Number(data.progress ?? 100)}%)`,
                raw: data,
              })
              return
            }
            if (type === 'deployment_runtime_update') {
              const deploymentId = String(data.deployment_id ?? '')
              const sfcLabel = sfcLabelByDeploymentId(deploymentId)
              if (deploymentId) {
                updateDeployment(deploymentId, {
                  orchestration_phase: String(data.orchestration_phase ?? ''),
                  orchestration_progress: Number(data.orchestration_progress ?? 0),
                  containers_total: Number(data.containers_total ?? 0),
                  containers_running: Number(data.containers_running ?? 0),
                  containers_failed: Number(data.containers_failed ?? 0),
                  core_nfs_total: Number(data.core_nfs_total ?? 0),
                  core_nfs_running: Number(data.core_nfs_running ?? 0),
                  core_nfs_failed: Number(data.core_nfs_failed ?? 0),
                  service_ready: Boolean(data.service_ready),
                  ready_for_ueransim: Boolean(data.ready_for_ueransim),
                  last_error: String(data.last_error ?? ''),
                  last_update_at: String(data.last_update_at ?? ''),
                } as any)
              }
              pushRuntimeEvent({
                type: 'deployment_runtime_update',
                sim_time: data.last_update_at,
                message: `部署运行态更新 ${sfcLabel}: ${String(data.orchestration_phase ?? 'unknown')} (${Number(data.orchestration_progress ?? 0)}%)`,
                raw: data,
              })
              return
            }
            if (type === 'topology_tick') {
              if (applyTopologySnapshotEnabled) {
                applyTopologySnapshot(data.snapshot)
                const mode = useStore.getState().simulation.view_mode
                if (mode === 'playback') {
                  pushRuntimeEvent({
                    type: 'playback_buffered',
                    sim_time: data?.snapshot?.sim_time,
                    message: '收到实时拓扑更新周期数据（已缓冲，当前为回放模式）',
                    raw: { topology_version: data?.snapshot?.topology_version },
                  })
                }
              } else {
                const snapshot = data?.snapshot ?? {}
                useStore.getState().setSimulationStatus({
                  sim_time: String(snapshot?.sim_time ?? ''),
                  topology_version: Number(snapshot?.topology_version ?? 0),
                  metrics: snapshot?.metrics ?? null,
                })
              }
              return
            }
            if (type === 'metrics_tick') {
              useStore.getState().setSimulationStatus({
                sim_time: String(data.sim_time ?? ''),
                topology_version: Number(data.topology_version ?? 0),
                metrics: data.metrics ?? null,
              })
              return
            }
            if (type === 'decision_trace') {
              const trace = data as any
              pushDecisionTrace(trace)
              upsertSessionDeploymentFromTrace(trace)
              const trigger = String(trace?.trigger ?? '')
              if (trigger === 'source_node_down' || trigger === 'destination_node_down') {
                const sessionId = String(trace?.session_id ?? trace?.request_id ?? 'unknown')
                const sfcLabel = sfcLabelByIds(String(trace?.session_id ?? ''), String(trace?.request_id ?? ''))
                const key = `${sessionId}:${trigger}`
                const now = Date.now()
                const last = endpointFaultPopupCooldownRef.current[key] ?? 0
                if (now - last > 12000) {
                  endpointFaultPopupCooldownRef.current[key] = now
                  const label = trigger === 'source_node_down' ? '源节点' : '宿节点'
                  pushRuntimeEvent({
                    type: 'reschedule_trigger',
                    sim_time: data.sim_time,
                    message: `${sfcLabel} 触发重调度：${label}故障，系统正在重算可用路径`,
                    raw: {
                      session_id: trace?.session_id,
                      request_id: trace?.request_id,
                      trigger,
                      label,
                    },
                  })
                }
              }
              pushRuntimeEvent({
                type: 'decision_trace',
                sim_time: data.sim_time,
                message: `策略决策完成: ${data.request_id} [${data.mode ?? 'single'}]`,
                raw: data,
              })
              return
            }
            if (type === 'session_update') {
              const sid = String(data.session_id ?? '')
              const rid = String(data.request_id ?? '')
              const sfcLabel = sfcLabelByIds(sid, rid)
              pushRuntimeEvent({
                type,
                sim_time: data.sim_time,
                message: `SFC编排更新 ${sfcLabel}: ${data.status}`,
                raw: data,
              })
              return
            }
            if (type === 'orchestration_metrics_tick') {
              useStore.getState().setSimulationStatus({
                orchestration: data as any,
                sim_time: String(data.sim_time ?? ''),
                topology_version: Number(data.topology_version ?? 0),
              })
              return
            }
            if (type === 'fault_event' || type === 'recovery_event' || type === 'fault_update_event') {
              const label = type === 'fault_event'
                ? '故障事件'
                : (type === 'recovery_event' ? '恢复事件' : '故障更新')
              const entityType = String(data.entity_type ?? 'entity')
              const entityId = String(data.entity_id ?? '')
              const entity = entityType === 'session'
                ? sfcLabelByIds(entityId, String(data.request_id ?? ''))
                : `${entityType}:${entityId}`
              const faultType = String(data.fault_type ?? data.reason ?? '')
              const faultText = faultType ? ` (${faultTypeLabel(faultType)})` : ''
              pushRuntimeEvent({
                type,
                sim_time: data.sim_time,
                message: `${label} ${entity}${faultText}`,
                raw: data,
              })
              addToast(
                `${label} ${entity}${faultText}`,
                type === 'fault_event' ? 'warning' : (type === 'recovery_event' ? 'success' : 'info'),
              )
              return
            }
            if (type) {
              pushRuntimeEvent({
                type,
                sim_time: data.sim_time,
                message: String(data.message ?? `系统事件: ${type}`),
                raw: data,
              })
            }
          } catch {
            // ignore
          }
        }

        ws.onerror = () => {
          setSimulationStatus({ connected: false })
        }

        ws.onclose = () => {
          setSimulationStatus({ connected: false })
          if (closedByUnmount) return
          if (reconnectRef.current) window.clearTimeout(reconnectRef.current)
          reconnectRef.current = window.setTimeout(() => connect(), 2000)
        }
      } catch {
        setSimulationStatus({ connected: false })
      }
    }

    connect()
    return () => {
      closedByUnmount = true
      if (reconnectRef.current) window.clearTimeout(reconnectRef.current)
      ref.current?.close()
    }
  }, [])
}
