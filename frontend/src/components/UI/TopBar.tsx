import { useMemo, useState } from 'react'
import { Activity, Settings, Satellite, Info, ChevronDown, SlidersHorizontal, LogOut } from 'lucide-react'
import { useStore } from '@/store/useStore'
import { shallow } from 'zustand/shallow'
import SettingsModal from './SettingsModal'
import { useAuth } from '@/auth/AuthContext'

export default function TopBar() {
  const [open, setOpen] = useState(false)
  const [aboutOpen, setAboutOpen] = useState(false)
  const { user, logout } = useAuth()
  const { satCount, linkCount, linkBreakdown, deployCount, orch, decisionTraces } = useStore((s) => {
    const activeLinks = s.links.filter((l: any) => String(l?.status ?? 'active') !== 'down')
    const intra = activeLinks.filter((l: any) => String(l?.link_type ?? '') === 'intra_orbit').length
    const inter = activeLinks.length - intra
    return {
      satCount: s.satellites.length,
      linkCount: activeLinks.length,
      linkBreakdown: `轨道内 ${intra} / 轨道间 ${inter}`,
      deployCount: s.deployments.length,
      orch: s.simulation.orchestration,
      decisionTraces: s.decisionTraces,
    }
  }, shallow)
  const p95LatencyMs = useMemo(() => {
    const direct = Number(orch?.latency_p95_ms ?? 0)
    if (direct > 0) return direct
    const samples = decisionTraces
      .filter((t: any) => String(t?.mode ?? '') === 'session_continuous')
      .slice(0, 120)
      .map((t) => Number(t.inference_time_ms ?? 0))
      .filter((v) => Number.isFinite(v) && v > 0)
      .sort((a, b) => a - b)
    if (samples.length === 0) return 0
    const idx = Math.min(samples.length - 1, Math.floor((samples.length - 1) * 0.95))
    return samples[idx]
  }, [orch?.latency_p95_ms, decisionTraces])
  const gotoMonitorPage = () => {
    if (window.location.pathname === '/monitor') return
    window.history.pushState({}, '', '/monitor')
    window.dispatchEvent(new PopStateEvent('popstate'))
  }
  const gotoControlPage = () => {
    const controlUrl = String((import.meta as any)?.env?.VITE_CONTROL_CENTER_URL || 'http://localhost:3002')
    window.open(controlUrl, '_blank', 'noopener,noreferrer')
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
          background: 'linear-gradient(90deg, rgba(4,8,14,0.72) 0%, rgba(6,12,21,0.66) 56%, rgba(9,20,33,0.62) 100%)',
          borderBottom: '1px solid rgba(102,139,170,0.22)',
          backdropFilter: 'blur(16px)',
          boxShadow: 'inset 0 -1px 0 rgba(98,151,191,0.1)',
        }}>
        
        <div className="flex items-center gap-3">
          <div className="w-7 h-7 rounded-xl flex items-center justify-center shadow-lg"
            style={{ background: 'linear-gradient(135deg, #1f4f76, #14324e)' }}>
            <Satellite className="w-4 h-4 text-white" />
          </div>
          <div>
            <div className="text-sm font-bold text-white leading-none tracking-tight">核心网智能编排系统</div>
            <div className="text-[9px] text-cyan-300/70 leading-none mt-0.5 tracking-widest uppercase">LEO · Open5GS 核心网编排 · 智能重调度</div>
          </div>
        </div>

        <div className="flex items-center gap-4 text-[11px]">
          <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
            style={{ background: 'rgba(34,211,238,0.12)', border: '1px solid rgba(103,232,249,0.32)' }}>
            <div className="w-1.5 h-1.5 rounded-full bg-cyan-300 animate-pulse" />
            <span className="text-cyan-200 font-semibold">{satCount}</span>
            <span className="text-cyan-200/70">在轨卫星</span>
          </div>
          <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
            title={linkBreakdown}
            style={{ background: 'rgba(96,165,250,0.1)', border: '1px solid rgba(96,165,250,0.25)' }}>
            <div className="w-1.5 h-1.5 rounded-full bg-blue-300 animate-pulse" />
            <span className="text-blue-300 font-semibold">{linkCount}</span>
            <span className="text-blue-300/70">ISL</span>
          </div>
          <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
            style={{ background: 'rgba(168,85,247,0.1)', border: '1px solid rgba(196,181,253,0.25)' }}>
            <div className="w-1.5 h-1.5 rounded-full bg-violet-300 animate-pulse" />
            <span className="text-violet-300 font-semibold">{deployCount}</span>
            <span className="text-violet-300/70">核心网部署</span>
          </div>
          {orch && (
            <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
              style={{ background: 'rgba(14,116,144,0.14)', border: '1px solid rgba(34,211,238,0.28)' }}>
              <div className="w-1.5 h-1.5 rounded-full bg-cyan-300 animate-pulse" />
              <span className="text-cyan-300/70">P95 推理时延 {p95LatencyMs.toFixed(0)}ms</span>
            </div>
          )}
          {orch && (
            <div className="flex items-center gap-2 px-3 py-1.5 rounded-full"
              style={{ background: 'rgba(16,185,129,0.12)', border: '1px solid rgba(74,222,128,0.28)' }}>
              <div className="w-1.5 h-1.5 rounded-full bg-emerald-300 animate-pulse" />
              <span className="text-emerald-200 font-semibold">故障恢复 {Number((orch.recovery_success_rate ?? 0) * 100).toFixed(0)}%</span>
              <span className="text-emerald-300/70">{orch.total_recovery_success ?? 0}/{orch.total_recovery_attempts ?? 0}</span>
            </div>
          )}
        </div>

        <div className="flex items-center gap-1.5 relative">
          <button
            onClick={gotoControlPage}
            title="系统控制中心"
            className="h-8 w-8 rounded-full text-cyan-100 inline-flex items-center justify-center transition hover:bg-cyan-400/12 hover:text-cyan-200"
            style={iconBtnStyle}
          >
            <SlidersHorizontal className="w-4 h-4" />
          </button>
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
                  ['系统概览', '动态卫星拓扑与 Open5GS 核心网智能编排可视化系统'],
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
          <div className="ml-1 px-2.5 h-7 rounded-full text-[11px] inline-flex items-center"
            style={{ background: 'rgba(8,31,52,0.5)', border: '1px solid rgba(125,211,252,0.24)', color: '#bae6fd' }}>
            {user ? `${user.username} · ${user.role === 'admin' ? '管理员' : '普通用户'}` : '访客模式'}
          </div>
          {user && (
            <button
              onClick={logout}
              title="退出登录"
              className="h-8 w-8 rounded-full text-cyan-100 inline-flex items-center justify-center transition hover:bg-cyan-400/12 hover:text-cyan-200"
              style={iconBtnStyle}
            >
              <LogOut className="w-4 h-4" />
            </button>
          )}
        </div>
      </header>
      <SettingsModal isOpen={open} onClose={() => setOpen(false)} />
    </>
  )
}
