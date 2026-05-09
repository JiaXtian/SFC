import { useState } from 'react'
import { SlidersHorizontal, Users, LogOut, Satellite, ShieldAlert, RadioTower } from 'lucide-react'
import { useAuth } from '@/auth/AuthContext'
import SatelliteNodeControlPage from './SatelliteNodeControlPage'
import SFCForm from './SFCForm'
import FaultInjectionControl from './FaultInjectionControl'
import UserManagementPage from './UserManagementPage'
import ControlDeploymentList from './ControlDeploymentList'
import UERANSIMValidationPage from './UERANSIMValidationPage'

type Tab = 'satellite' | 'strategy' | 'validation' | 'fault' | 'users'

function StrategyDeployBody({ canManage }: { canManage: boolean }) {
  return (
    <div className="h-full grid grid-cols-12 gap-2.5 overflow-hidden">
      <div className="col-span-12 xl:col-span-4 h-full overflow-hidden">
        <div
          className="rounded-2xl p-3 h-full overflow-hidden"
          style={{
            background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
            border: '1px solid rgba(112,168,208,0.28)',
            backdropFilter: 'blur(14px)',
          }}
        >
          <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold mb-2">核心网策略部署</div>
          <div className="h-[calc(100%-28px)] overflow-y-auto pr-1">
            <SFCForm />
          </div>
        </div>
      </div>

      <div className="col-span-12 xl:col-span-8 h-full overflow-hidden">
        <ControlDeploymentList canManage={canManage} />
      </div>
    </div>
  )
}

function FaultControlBody() {
  return (
    <div className="h-full grid grid-cols-12 gap-2.5 overflow-hidden">
      <div className="col-span-12 h-full overflow-hidden">
        <FaultInjectionControl />
      </div>
    </div>
  )
}

export default function ControlPage() {
  const { user, logout } = useAuth()
  const role = user?.role === 'admin' ? 'admin' : 'user'
  const canManage = role === 'admin'
  const [tab, setTab] = useState<Tab>('satellite')

  const visibleTabs: Tab[] = canManage ? ['satellite', 'strategy', 'validation', 'fault', 'users'] : ['satellite']
  const activeTab = visibleTabs.includes(tab) ? tab : 'satellite'

  return (
    <div
      className="absolute inset-0 z-[92] overflow-hidden"
      style={{
        background: 'radial-gradient(1200px 600px at 50% -20%, rgba(33,88,128,0.24), rgba(3,8,16,0.94) 54%, #02050b 100%)',
      }}
    >
      <div className="mx-auto px-4 py-3.5 h-full" style={{ width: 'min(99vw, 1980px)' }}>
        <div className="flex items-center gap-2.5 mb-3.5">
          <div className="text-2xl font-semibold text-slate-100 inline-flex items-center gap-2">
            <SlidersHorizontal className="w-5 h-5 text-cyan-300" />
            系统控制中心
          </div>
          {!canManage && (
            <div className="h-9 px-3 rounded-xl text-[12px] text-amber-100 inline-flex items-center gap-1.5"
              style={{ background: 'rgba(120,53,15,0.35)', border: '1px solid rgba(251,191,36,0.35)' }}>
              <ShieldAlert className="w-3.5 h-3.5 text-amber-300" />
              普通用户模式：仅可查看卫星节点控制
            </div>
          )}
          <div className="ml-auto flex items-center gap-2">
            <div className="h-9 px-3 rounded-xl text-[12px] text-cyan-100 inline-flex items-center"
              style={{ background: 'rgba(7,22,39,0.7)', border: '1px solid rgba(125,211,252,0.22)' }}>
              {user?.username ?? 'user'} · {canManage ? '管理员' : '普通用户'}
            </div>
            <button
              onClick={logout}
              className="h-9 px-3 rounded-xl text-cyan-100 text-[12px] inline-flex items-center gap-1.5"
              style={{ background: 'rgba(51,65,85,0.7)', border: '1px solid rgba(148,163,184,0.25)' }}
            >
              <LogOut className="w-3.5 h-3.5" />
              退出登录
            </button>
          </div>
        </div>

        <div className="mb-3 flex items-center gap-2">
          {visibleTabs.includes('satellite') && (
            <button
              className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
                activeTab === 'satellite' ? 'text-cyan-100' : 'text-slate-300'
              }`}
              style={activeTab === 'satellite'
                ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
                : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
              onClick={() => setTab('satellite')}
            >
              <Satellite className="w-3.5 h-3.5" />
              卫星节点控制
            </button>
          )}
          {visibleTabs.includes('strategy') && (
            <button
              className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
                activeTab === 'strategy' ? 'text-cyan-100' : 'text-slate-300'
              }`}
              style={activeTab === 'strategy'
                ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
                : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
              onClick={() => setTab('strategy')}
            >
              <SlidersHorizontal className="w-3.5 h-3.5" />
              核心网部署
            </button>
          )}
          {visibleTabs.includes('validation') && (
            <button
              className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
                activeTab === 'validation' ? 'text-cyan-100' : 'text-slate-300'
              }`}
              style={activeTab === 'validation'
                ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
                : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
              onClick={() => setTab('validation')}
            >
              <RadioTower className="w-3.5 h-3.5" />
              功能验证
            </button>
          )}
          {visibleTabs.includes('fault') && (
            <button
              className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
                activeTab === 'fault' ? 'text-cyan-100' : 'text-slate-300'
              }`}
              style={activeTab === 'fault'
                ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
                : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
              onClick={() => setTab('fault')}
            >
              <ShieldAlert className="w-3.5 h-3.5" />
              故障控制
            </button>
          )}
          {visibleTabs.includes('users') && (
            <button
              className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
                activeTab === 'users' ? 'text-cyan-100' : 'text-slate-300'
              }`}
              style={activeTab === 'users'
                ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
                : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
              onClick={() => setTab('users')}
            >
              <Users className="w-3.5 h-3.5" />
              用户控制
            </button>
          )}
        </div>

        <div className="h-[calc(100%-104px)] overflow-hidden">
          {activeTab === 'satellite' && <SatelliteNodeControlPage role={role} />}
          {activeTab === 'strategy' && <StrategyDeployBody canManage={canManage} />}
          {activeTab === 'validation' && <UERANSIMValidationPage />}
          {activeTab === 'fault' && <FaultControlBody />}
          {activeTab === 'users' && <UserManagementPage />}
        </div>
      </div>
    </div>
  )
}
