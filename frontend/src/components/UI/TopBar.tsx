import { useState } from 'react'
import { Settings, Satellite } from 'lucide-react'
import { useStore } from '@/store/useStore'
import SettingsModal from './SettingsModal'

export default function TopBar() {
  const [open, setOpen] = useState(false)
  const { satellites, links, deployments } = useStore()

  return (
    <>
      <header className="absolute top-0 left-0 right-0 h-11 z-30 flex items-center justify-between px-5"
        style={{
          background: 'linear-gradient(90deg, rgba(3,7,12,0.97) 0%, rgba(7,15,26,0.95) 56%, rgba(12,27,43,0.92) 100%)',
          borderBottom: '1px solid rgba(102,139,170,0.22)',
          backdropFilter: 'blur(16px)',
          boxShadow: 'inset 0 -1px 0 rgba(98,151,191,0.1), 0 8px 24px rgba(0,0,0,0.35)',
        }}>
        
        <div className="flex items-center gap-3">
          <div className="w-7 h-7 rounded-xl flex items-center justify-center shadow-lg"
            style={{ background: 'linear-gradient(135deg, #1f4f76, #14324e)' }}>
            <Satellite className="w-4 h-4 text-white" />
          </div>
          <div>
            <div className="text-sm font-bold text-white leading-none tracking-tight">核心网智能编排系统</div>
            <div className="text-[9px] text-cyan-300/70 leading-none mt-0.5 tracking-widest uppercase">LEO · 服务功能链编排 · 核心网编排决策自动化</div>
          </div>
        </div>

        <div className="flex items-center gap-4 text-[11px]">
          <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
            style={{ background: 'rgba(34,211,238,0.12)', border: '1px solid rgba(103,232,249,0.32)' }}>
            <div className="w-1.5 h-1.5 rounded-full bg-cyan-300 animate-pulse" />
            <span className="text-cyan-200 font-semibold">{satellites.length}</span>
            <span className="text-cyan-200/70">在轨卫星</span>
          </div>
          <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
            style={{ background: 'rgba(96,165,250,0.1)', border: '1px solid rgba(96,165,250,0.25)' }}>
            <div className="w-1.5 h-1.5 rounded-full bg-blue-300 animate-pulse" />
            <span className="text-blue-300 font-semibold">{links.length}</span>
            <span className="text-blue-300/70">ISL</span>
          </div>
          <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
            style={{ background: 'rgba(168,85,247,0.1)', border: '1px solid rgba(196,181,253,0.25)' }}>
            <div className="w-1.5 h-1.5 rounded-full bg-violet-300 animate-pulse" />
            <span className="text-violet-300 font-semibold">{deployments.length}</span>
            <span className="text-violet-300/70">SFC 部署</span>
          </div>
        </div>

        <button onClick={() => setOpen(true)} className="p-2 rounded-xl transition hover:bg-white/5">
          <Settings className="w-4 h-4 text-gray-400 hover:text-white transition" />
        </button>
      </header>
      <SettingsModal isOpen={open} onClose={() => setOpen(false)} />
    </>
  )
}
