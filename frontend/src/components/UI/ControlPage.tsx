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
    <div className="control-modern absolute inset-0 z-[92] overflow-hidden">
      <div className="control-bg-glow control-bg-glow-a" />
      <div className="control-bg-glow control-bg-glow-b" />
      <div className="relative h-full w-full">
        <header className="control-topbar h-16 px-8">
          <div className="flex h-full items-center">
            <div className="control-title">系统控制中心</div>
          </div>
        </header>

        <div className="flex h-[calc(100%_-_64px)] min-h-0">
          <aside className="control-sidebar w-[250px] px-4 py-5">
            <div className="space-y-1.5">
              {tabs.map((item) => {
                const Icon = item.icon
                const active = activeTab === item.key
                return (
                  <button
                    key={item.key}
                    onClick={() => setActiveTab(item.key)}
                    className={`control-nav-btn ${active ? 'is-active' : ''}`}
                  >
                    <div className="flex items-center gap-2">
                      <Icon className="h-5 w-5" />
                      <div className="text-[17px] font-medium">{item.label}</div>
                    </div>
                  </button>
                )
              })}
            </div>
          </aside>

          <main className="control-main flex-1 overflow-hidden p-4 xl:p-5">
            <div className="control-content-surface h-full min-h-0 w-full">
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
