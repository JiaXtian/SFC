import { useEffect, useRef } from 'react'
import { useStore } from '@/store/useStore'

export function useWebSocket() {
  const ref = useRef<WebSocket | null>(null)
  const { updateDeployment } = useStore()

  useEffect(() => {
    try {
      const ws = new WebSocket('ws://localhost:8080/ws/updates')
      ref.current = ws
      ws.onmessage = (evt) => {
        try {
          const data = JSON.parse(evt.data)
          if (data.type === 'deployment_update') {
            updateDeployment(data.deployment_id, { status: data.status, progress: data.progress })
          }
        } catch { /* ignore */ }
      }
      ws.onerror = () => { /* backend may not be running */ }
    } catch { /* ignore */ }
    return () => ref.current?.close()
  }, [])
}
