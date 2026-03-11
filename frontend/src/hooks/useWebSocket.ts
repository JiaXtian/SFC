import { useEffect, useRef } from 'react'
import { useStore } from '@/store/useStore'

function defaultWsUrl() {
  const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws'
  const host = window.location.hostname || 'localhost'
  const port = window.location.port === '8080' ? window.location.port : '8080'
  return `${protocol}://${host}:${port}/ws/updates`
}

export function useWebSocket() {
  const ref = useRef<WebSocket | null>(null)
  const reconnectRef = useRef<number | null>(null)

  useEffect(() => {
    const state = useStore.getState()
    const {
      setSimulationStatus,
      updateDeployment,
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
              updateDeployment(String(data.deployment_id ?? ''), {
                status: data.status ?? 'completed',
                progress: Number(data.progress ?? 100),
              })
              return
            }
            if (type === 'topology_tick') {
              applyTopologySnapshot(data.snapshot)
              const mode = useStore.getState().simulation.view_mode
              if (mode === 'playback') {
                pushRuntimeEvent({
                  type: 'playback_buffered',
                  sim_time: data?.snapshot?.sim_time,
                  message: '收到实时拓扑tick（已缓冲，当前为回放模式）',
                  raw: { topology_version: data?.snapshot?.topology_version },
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
              pushRuntimeEvent({
                type: 'decision_trace',
                sim_time: data.sim_time,
                message: `策略决策完成: ${data.request_id} (topo_v${data.topology_version}) [${data.mode ?? 'single'}]`,
                raw: data,
              })
              return
            }
            if (type === 'session_update') {
              pushRuntimeEvent({
                type,
                sim_time: data.sim_time,
                message: `会话更新 ${data.session_id}: ${data.status} (topo_v${data.topology_version})`,
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
            if (type === 'fault_event' || type === 'recovery_event') {
              const label = type === 'fault_event' ? '故障事件' : '恢复事件'
              const entity = `${data.entity_type ?? 'entity'}:${data.entity_id ?? ''}`
              pushRuntimeEvent({
                type,
                sim_time: data.sim_time,
                message: `${label} ${entity}`,
                raw: data,
              })
              addToast(`${label} ${entity}`, type === 'fault_event' ? 'warning' : 'success')
              return
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
