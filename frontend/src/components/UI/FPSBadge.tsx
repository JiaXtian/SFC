import { useEffect, useRef, useState } from 'react'
import { useStore } from '@/store/useStore'

export default function FPSBadge() {
  const showFPS = useStore((s) => s.display.showFPS)
  const [fps, setFps] = useState(0)
  const frameCount = useRef(0)
  const lastTime = useRef(performance.now())
  const rafId = useRef<number | null>(null)

  useEffect(() => {
    if (!showFPS) return

    const tick = (now: number) => {
      frameCount.current += 1
      const elapsed = now - lastTime.current
      if (elapsed >= 500) {
        setFps(Math.round((frameCount.current * 1000) / elapsed))
        frameCount.current = 0
        lastTime.current = now
      }
      rafId.current = requestAnimationFrame(tick)
    }

    rafId.current = requestAnimationFrame(tick)
    return () => {
      if (rafId.current !== null) cancelAnimationFrame(rafId.current)
      rafId.current = null
      frameCount.current = 0
      lastTime.current = performance.now()
    }
  }, [showFPS])

  if (!showFPS) return null

  return (
    <div
      className="px-2.5 h-[34px] rounded-full flex items-center gap-2 pointer-events-none"
      style={{
        background: 'rgba(7, 12, 20, 0.52)',
        border: '1px solid rgba(118, 145, 171, 0.2)',
        backdropFilter: 'blur(8px)',
      }}
    >
      <div className="text-[10px] text-slate-300 font-semibold leading-none">FPS</div>
      <div className="text-xs font-semibold text-cyan-100 leading-none">{fps}</div>
    </div>
  )
}
