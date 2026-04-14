import { ArrowLeft, SlidersHorizontal } from 'lucide-react'
import ConstellationControlPanel from './ConstellationControlPanel'
import SFCForm from './SFCForm'
import FaultInjectionControl from './FaultInjectionControl'

function navigateTo(path: string) {
  if (window.location.pathname === path) return
  window.history.pushState({}, '', path)
  window.dispatchEvent(new PopStateEvent('popstate'))
}

export default function ControlPage() {
  return (
    <div
      className="absolute inset-0 z-[92] overflow-auto"
      style={{
        background: 'radial-gradient(1200px 600px at 50% -20%, rgba(33,88,128,0.24), rgba(3,8,16,0.94) 54%, #02050b 100%)',
      }}
    >
      <div className="mx-auto px-4 py-3.5" style={{ width: 'min(99vw, 1980px)' }}>
        <div className="flex items-center gap-2.5 mb-3.5">
          <button
            onClick={() => navigateTo('/')}
            className="h-10 px-3.5 rounded-xl text-cyan-100 text-[14px] flex items-center gap-1.5 transition hover:brightness-110"
            style={{
              background: 'linear-gradient(135deg, rgba(22,52,78,0.14), rgba(11,26,44,0.1))',
              backdropFilter: 'blur(10px)',
            }}
          >
            <ArrowLeft className="w-4 h-4" />
            返回大屏
          </button>
          <div className="text-2xl font-semibold text-slate-100 inline-flex items-center gap-2">
            <SlidersHorizontal className="w-5 h-5 text-cyan-300" />
            系统控制页面
          </div>
          <div className="ml-auto text-[13px] text-slate-400">控制台</div>
        </div>

        <div className="grid grid-cols-12 gap-3.5">
          <div className="col-span-12 xl:col-span-3 2xl:col-span-3 min-h-[800px]">
            <ConstellationControlPanel />
          </div>

          <div className="col-span-12 xl:col-span-5 2xl:col-span-6 min-h-[800px]">
            <div
              className="rounded-2xl p-3.5 h-full"
              style={{
                background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
                border: '1px solid rgba(112,168,208,0.28)',
                backdropFilter: 'blur(14px)',
              }}
            >
              <div className="text-[14px] uppercase tracking-wide text-cyan-100 font-semibold mb-2.5">部署策略生成</div>
              <div className="h-[calc(100%-28px)] overflow-y-auto" style={{ zoom: 1.08 }}>
                <SFCForm />
              </div>
            </div>
          </div>

          <div className="col-span-12 xl:col-span-4 2xl:col-span-3 min-h-[800px]">
            <FaultInjectionControl />
          </div>
        </div>
      </div>
    </div>
  )
}
