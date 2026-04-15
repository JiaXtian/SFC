import { Satellite, ShieldAlert, SlidersHorizontal } from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import FaultControlPage from './control/FaultControlPage'
import SatelliteControlPage from './control/SatelliteControlPage'
import StrategyControlPage from './control/StrategyControlPage'

type ControlTab = 'fault' | 'satellite' | 'strategy'

function readTabFromUrl(): ControlTab {
  const sp = new URLSearchParams(window.location.search)
  const tab = String(sp.get('tab') || '').toLowerCase()
  if (tab === 'fault' || tab === 'strategy' || tab === 'satellite') return tab
  return 'satellite'
}

function writeTabToUrl(tab: ControlTab) {
  const url = new URL(window.location.href)
  url.searchParams.set('tab', tab)
  window.history.replaceState({}, '', `${url.pathname}${url.search}`)
}

export default function ControlPage() {
  const [activeTab, setActiveTab] = useState<ControlTab>(() => readTabFromUrl())

  useEffect(() => {
    const onPop = () => setActiveTab(readTabFromUrl())
    window.addEventListener('popstate', onPop)
    return () => window.removeEventListener('popstate', onPop)
  }, [])

  useEffect(() => {
    writeTabToUrl(activeTab)
  }, [activeTab])

  const tabs = useMemo(() => ([
    { key: 'fault' as const, label: '故障控制', icon: ShieldAlert },
    { key: 'satellite' as const, label: '卫星节点控制', icon: Satellite },
    { key: 'strategy' as const, label: '部署策略', icon: SlidersHorizontal },
  ]), [])

  return (
    <div className="control-monochrome absolute inset-0 z-[92] overflow-hidden bg-white">
      <div className="h-full w-full">
        <div className="h-16 border-b border-slate-200 bg-white px-8 text-black">
          <div className="flex h-full items-center">
            <div className="text-2xl font-semibold tracking-wide">系统控制中心</div>
          </div>
        </div>

        <div className="flex h-[calc(100%-64px)]">
          <aside className="w-[250px] border-r border-slate-200 bg-white px-4 py-6">
            <div className="space-y-2">
              {tabs.map((item) => {
                const Icon = item.icon
                const active = activeTab === item.key
                return (
                  <button
                    key={item.key}
                    onClick={() => setActiveTab(item.key)}
                    className={`w-full rounded-xl border border-transparent px-3 py-3 text-left transition ${
                      active
                        ? 'border-slate-300 text-black'
                        : 'bg-transparent text-black hover:border-slate-200'
                    }`}
                  >
                    <div className="flex items-center gap-2">
                      <Icon className="h-5 w-5 text-black" />
                      <div className={`text-lg ${active ? 'font-semibold' : 'font-medium'}`}>{item.label}</div>
                    </div>
                  </button>
                )
              })}
            </div>
          </aside>

          <main className="flex-1 overflow-hidden bg-white px-6 py-5">
            <div className="h-full w-full">
              {activeTab === 'fault' && <FaultControlPage />}
              {activeTab === 'satellite' && <SatelliteControlPage />}
              {activeTab === 'strategy' && <StrategyControlPage />}
            </div>
          </main>
        </div>
      </div>
    </div>
  )
}
