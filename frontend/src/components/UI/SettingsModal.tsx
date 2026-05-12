import { X } from 'lucide-react'
import { useStore } from '@/store/useStore'

export default function SettingsModal({ isOpen, onClose }: { isOpen: boolean; onClose: () => void }) {
  const {
    display,
    setDisplay,
    autoDynamics,
    setAutoDynamics,
  } = useStore()
  const orbitHz = Math.max(0.5, Math.min(5, Number(autoDynamics.position_update_hz ?? 4)))

  if (!isOpen) return null
  return (
    <div className="fixed inset-0 z-[10000] flex items-center justify-center px-4" style={{ background: 'rgba(0,0,0,0.8)', backdropFilter: 'blur(8px)' }}>
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
          <label className="flex items-center justify-between gap-3">
            <span className="text-sm text-slate-300">轨道计算频率</span>
            <span className="inline-flex items-center gap-1.5 text-[12px] text-cyan-100">
              <input
                type="number"
                min={0.5}
                max={5}
                step={0.5}
                value={orbitHz}
                onChange={(e) => {
                  const next = Math.max(0.5, Math.min(5, Number(e.target.value) || 4))
                  setAutoDynamics({ position_update_hz: next })
                }}
                className="h-7 w-16 rounded-md bg-slate-900/60 px-2 text-right text-[12px] text-cyan-100 outline-none focus:ring-1 focus:ring-cyan-400/45"
              />
              <span className="text-slate-400">Hz</span>
            </span>
          </label>
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
