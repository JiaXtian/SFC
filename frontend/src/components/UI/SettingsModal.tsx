import { useEffect, useState } from 'react'
import { RefreshCw, X } from 'lucide-react'
import { useStore } from '@/store/useStore'
import { apiClient } from '@/api/client'

export default function SettingsModal({ isOpen, onClose }: { isOpen: boolean; onClose: () => void }) {
  const {
    display,
    setDisplay,
    autoDynamics,
    setAutoDynamics,
    setSimulationStatus,
    addToast,
  } = useStore()
  const [tickSec, setTickSec] = useState(autoDynamics.resource_update_sec)
  const [orbitHz, setOrbitHz] = useState(autoDynamics.position_update_hz)
  const [savingTick, setSavingTick] = useState(false)

  useEffect(() => {
    if (!isOpen) return
    setTickSec(autoDynamics.resource_update_sec)
    setOrbitHz(autoDynamics.position_update_hz)
    ;(async () => {
      try {
        const cfg = await apiClient.getControlConfig()
        const next = Math.max(10, Math.min(120, Number(cfg?.resource_sampling_interval_sec ?? autoDynamics.resource_update_sec ?? 15)))
        setTickSec(next)
        setAutoDynamics({ resource_update_sec: next })
      } catch {
        // ignore transient config fetch failures
      }
    })()
  }, [isOpen])

  const saveDynamicRefresh = async () => {
    setSavingTick(true)
    try {
      const nextTick = Math.max(10, Math.min(120, Number(tickSec || 15)))
      const nextHz = Math.max(0.5, Math.min(12, Number(orbitHz || 4)))
      await apiClient.updateControlConfig({
        resource_sampling_interval_sec: nextTick,
        apply_now: true,
      })
      setAutoDynamics({ resource_update_sec: nextTick, position_update_hz: nextHz })
      setSimulationStatus({ sampling_interval_sec: nextTick })
      setTickSec(nextTick)
      setOrbitHz(nextHz)
      addToast(`动态拓扑 Tick 已更新为 ${nextTick}s`, 'success')
    } catch (e: any) {
      addToast(`动态刷新配置保存失败: ${e?.message ?? e}`, 'error')
    } finally {
      setSavingTick(false)
    }
  }

  if (!isOpen) return null
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center px-4" style={{ background: 'rgba(0,0,0,0.8)', backdropFilter: 'blur(8px)' }}>
      <div className="w-[390px] max-w-full rounded-xl shadow-2xl" style={{ background: 'linear-gradient(180deg, #1e293b, #0f172a)', border: '1px solid rgba(100,116,139,0.25)' }}>
        <div className="flex items-center justify-between px-4 py-3" style={{ borderBottom: '1px solid rgba(100,116,139,0.12)' }}>
          <span className="text-sm font-semibold text-white">系统设置</span>
          <button onClick={onClose} className="p-1 rounded hover:bg-white/5">
            <X className="w-4 h-4 text-slate-400" />
          </button>
        </div>
        <div className="p-4 space-y-3 max-h-[70vh] overflow-auto">
          <div>
            <label className="text-[11px] text-slate-500 block mb-1.5">渲染质量</label>
            <select
              value={display.renderQuality}
              onChange={(e) => setDisplay({ renderQuality: e.target.value as 'high' | 'balanced' | 'performance' })}
              className="w-full px-3 py-1.5 rounded text-sm text-white"
              style={{ background: 'rgba(30,41,59,0.7)', border: '1px solid rgba(100,116,139,0.15)' }}>
              <option value="high">高质量（推荐）</option>
              <option value="balanced">平衡</option>
              <option value="performance">低功耗</option>
            </select>
          </div>
          <label className="flex items-center justify-between cursor-pointer">
            <span className="text-sm text-slate-300">显示 FPS</span>
            <input
              type="checkbox"
              checked={display.showFPS}
              onChange={(e) => setDisplay({ showFPS: e.target.checked })}
              className="w-4 h-4 accent-blue-600"
            />
          </label>
          <div className="rounded-lg border border-slate-700/70 bg-slate-950/35 p-3 space-y-3">
            <div className="flex items-center justify-between gap-2">
              <div className="text-[12px] font-semibold text-cyan-100">动态拓扑刷新</div>
              <button
                onClick={saveDynamicRefresh}
                disabled={savingTick}
                className="h-7 px-2.5 rounded-md text-[11px] text-cyan-100 bg-cyan-500/10 border border-cyan-500/30 inline-flex items-center gap-1 disabled:opacity-60"
              >
                <RefreshCw className={`w-3 h-3 ${savingTick ? 'animate-spin' : ''}`} />
                保存
              </button>
            </div>
            <label className="block">
              <div className="mb-1 text-[11px] text-slate-400">Tick 间隔（10~120 秒）</div>
              <input
                type="number"
                min={10}
                max={120}
                step={1}
                value={tickSec}
                onChange={(e) => setTickSec(Math.max(10, Math.min(120, Number(e.target.value) || 15)))}
                className="w-full h-8 px-2.5 rounded-md bg-slate-900/70 border border-slate-700/80 text-cyan-100 text-[12px]"
              />
            </label>
            <label className="block">
              <div className="mb-1 text-[11px] text-slate-400">轨道渲染频率（0.5~12 Hz）</div>
              <input
                type="number"
                min={0.5}
                max={12}
                step={0.5}
                value={orbitHz}
                onChange={(e) => setOrbitHz(Math.max(0.5, Math.min(12, Number(e.target.value) || 4)))}
                className="w-full h-8 px-2.5 rounded-md bg-slate-900/70 border border-slate-700/80 text-cyan-100 text-[12px]"
              />
            </label>
          </div>
        </div>
        <div className="px-4 pb-4">
          <button onClick={onClose} 
            className="w-full py-2 rounded-lg text-sm font-medium text-white transition" 
            style={{ background: 'linear-gradient(135deg, #1e40af, #1e3a8a)' }}>
            关闭
          </button>
        </div>
      </div>
    </div>
  )
}
