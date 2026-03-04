import { useState } from 'react'
import { ChevronRight, ChevronLeft, Layers, List } from 'lucide-react'
import SFCForm from './SFCForm'
import DeploymentPanel from './DeploymentPanel'
import { useStore } from '@/store/useStore'

export default function RightPanel() {
  const [collapsed, setCollapsed] = useState(false)
  const [tab, setTab] = useState<'sfc'|'deploy'>('sfc')
  const { deployments } = useStore()

  return (
    <div className={`absolute right-0 top-11 bottom-11 z-10 flex transition-all duration-300 ${collapsed ? 'w-8' : 'w-[340px]'}`}>
      <button onClick={() => setCollapsed(!collapsed)}
        className="absolute -left-3 top-5 w-6 h-6 rounded-full flex items-center justify-center z-20 shadow-lg"
        style={{ background: 'linear-gradient(135deg, #0f2036, #182f45)' }}>
        {collapsed ? <ChevronLeft className="w-3 h-3 text-gray-300" /> : <ChevronRight className="w-3 h-3 text-gray-300" />}
      </button>

      {!collapsed && (
        <div className="w-full flex flex-col" style={{ background: 'linear-gradient(180deg, rgba(4,8,14,0.95) 0%, rgba(6,12,21,0.92) 55%, rgba(9,20,33,0.9) 100%)', borderLeft: '1px solid rgba(87, 126, 160, 0.24)', backdropFilter: 'blur(12px)', boxShadow: 'inset 1px 0 0 rgba(103,164,209,0.12), inset 30px 0 60px rgba(34,99,152,0.08)' }}>
          <div className="flex items-stretch" style={{ borderBottom: '1px solid rgba(95, 128, 156, 0.2)' }}>
            {(['sfc','deploy'] as const).map(t => (
              <button key={t} onClick={() => setTab(t)}
                className="flex-1 flex items-center justify-center gap-2 py-2.5 text-xs font-semibold transition relative"
                style={{ color: tab === t ? '#7dd3fc' : '#64748b' }}>
                {t === 'sfc' ? <Layers className="w-3.5 h-3.5" /> : <List className="w-3.5 h-3.5" />}
                {t === 'sfc' ? 'SFC 请求' : (
                  <span className="flex items-center gap-1">
                    部署列表
                    {deployments.length > 0 && (
                      <span className="w-4 h-4 rounded-full text-[8px] font-bold flex items-center justify-center"
                        style={{ background: '#0f3460', color: '#fff' }}>{deployments.length}</span>
                    )}
                  </span>
                )}
                {tab === t && (
                  <div className="absolute bottom-0 left-0 right-0 h-0.5" style={{ background: '#38bdf8' }} />
                )}
              </button>
            ))}
          </div>

          {tab === 'sfc' ? (
            <div className="flex-1 overflow-y-auto flex flex-col min-h-0">
              <SFCForm />
            </div>
          ) : (
            <div className="flex-1 overflow-y-auto flex flex-col min-h-0">
              <DeploymentPanel />
            </div>
          )}
        </div>
      )}
    </div>
  )
}
