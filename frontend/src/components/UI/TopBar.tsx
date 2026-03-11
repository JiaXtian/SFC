import { useState } from 'react'
import { Activity, Settings, Satellite, Info, ChevronDown } from 'lucide-react'
import { useStore } from '@/store/useStore'
import SettingsModal from './SettingsModal'

export default function TopBar() {
  const [open, setOpen] = useState(false)
  const [aboutOpen, setAboutOpen] = useState(false)
  const { satellites, links, deployments, simulation } = useStore()
  const orch = simulation.orchestration
  const gotoMonitorPage = () => {
    if (window.location.pathname === '/monitor') return
    window.history.pushState({}, '', '/monitor')
    window.dispatchEvent(new PopStateEvent('popstate'))
  }
  const iconBtnStyle = {
    color: 'rgba(186,230,253,0.92)',
    background: 'transparent',
    border: 'none',
    boxShadow: 'none',
    backdropFilter: 'none',
  } as const

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
          {orch && (
            <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
              style={{ background: 'rgba(14,116,144,0.14)', border: '1px solid rgba(34,211,238,0.28)' }}>
              <div className="w-1.5 h-1.5 rounded-full bg-cyan-300 animate-pulse" />
              <span className="text-cyan-200 font-semibold">SFC {orch.active_sessions}</span>
              <span className="text-cyan-300/70">P95 {orch.latency_p95_ms.toFixed(0)}ms</span>
            </div>
          )}
          {orch && (
            <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
              style={{ background: 'rgba(16,185,129,0.12)', border: '1px solid rgba(74,222,128,0.28)' }}>
              <div className="w-1.5 h-1.5 rounded-full bg-emerald-300 animate-pulse" />
              <span className="text-emerald-200 font-semibold">恢复 {Number((orch.recovery_success_rate ?? 0) * 100).toFixed(0)}%</span>
              <span className="text-emerald-300/70">{orch.total_recovery_success ?? 0}/{orch.total_recovery_attempts ?? 0}</span>
            </div>
          )}
        </div>

        <div className="flex items-center gap-1.5 relative">
          <button
            onClick={gotoMonitorPage}
            title="系统监控中心"
            className="h-8 w-8 rounded-full text-cyan-100 inline-flex items-center justify-center transition hover:bg-cyan-400/12 hover:text-cyan-200"
            style={iconBtnStyle}
          >
            <Activity className="w-4 h-4" />
          </button>
          <div className="relative">
            <button
              onClick={() => setAboutOpen(v => !v)}
              title="关于系统"
              className="h-8 w-8 rounded-full text-cyan-100 inline-flex items-center justify-center transition hover:bg-cyan-400/12 hover:text-cyan-200"
              style={iconBtnStyle}
            >
              <div className="relative inline-flex items-center justify-center">
                <Info className="w-4 h-4" />
                <ChevronDown className={`absolute -bottom-1 -right-1 w-2.5 h-2.5 transition ${aboutOpen ? 'rotate-180' : ''}`} />
              </div>
            </button>
            {aboutOpen && (
              <div
                className="absolute right-0 mt-1.5 w-52 rounded-xl p-1.5 z-[180]"
                style={{
                  background: 'rgba(7,16,28,0.94)',
                  border: '1px solid rgba(107,146,179,0.32)',
                  boxShadow: '0 12px 28px rgba(0,0,0,0.5)',
                }}
              >
                {[
                  ['系统概览', '动态卫星拓扑与SFC智能编排可视化系统'],
                  ['算法主线', 'GNN + A2C + 启发式剪枝 + 保底路径策略'],
                ].map(([k, v]) => (
                  <div key={k} className="px-2.5 py-1.5 rounded-lg hover:bg-white/5">
                    <div className="text-[11px] text-cyan-100 font-medium">{k}</div>
                    <div className="text-[10px] text-slate-400">{v}</div>
                  </div>
                ))}
              </div>
            )}
          </div>
          <button
            onClick={() => setOpen(true)}
            title="系统设置"
            className="h-8 w-8 rounded-full text-cyan-100 inline-flex items-center justify-center transition hover:bg-cyan-400/12 hover:text-cyan-200"
            style={iconBtnStyle}
          >
            <Settings className="w-4 h-4" />
          </button>
        </div>
      </header>
      <SettingsModal isOpen={open} onClose={() => setOpen(false)} />
    </>
  )
}
