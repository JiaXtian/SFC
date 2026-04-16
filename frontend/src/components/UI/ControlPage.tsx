import { useState } from 'react'
import { ArrowLeft, SlidersHorizontal, Users, LogOut } from 'lucide-react'
import ConstellationControlPanel from './ConstellationControlPanel'
import SFCForm from './SFCForm'
import FaultInjectionControl from './FaultInjectionControl'
import UserManagementPage from './UserManagementPage'
import { useAuth } from '@/auth/AuthContext'

type Tab = 'control' | 'users'

function SystemControlBody() {
  return (
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
  )
}

export default function ControlPage() {
  const [tab, setTab] = useState<Tab>('control')
  const { user, logout } = useAuth()
  const mainUrl = String((import.meta as any)?.env?.VITE_MAIN_SCREEN_URL || 'http://localhost:3001')

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
            onClick={() => {
              window.location.href = mainUrl
            }}
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
            系统控制中心
          </div>
          <div className="ml-auto flex items-center gap-2">
            <div className="h-9 px-3 rounded-xl text-[12px] text-cyan-100 inline-flex items-center"
              style={{ background: 'rgba(7,22,39,0.7)', border: '1px solid rgba(125,211,252,0.22)' }}>
              {user?.username ?? 'admin'} · {user?.role === 'admin' ? '管理员' : '普通用户'}
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
          <button
            className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
              tab === 'control' ? 'text-cyan-100' : 'text-slate-300'
            }`}
            style={tab === 'control'
              ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
              : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
            onClick={() => setTab('control')}
          >
            <SlidersHorizontal className="w-3.5 h-3.5" />
            系统控制中心
          </button>
          <button
            className={`h-9 px-4 rounded-lg text-[12px] font-semibold inline-flex items-center gap-1.5 ${
              tab === 'users' ? 'text-cyan-100' : 'text-slate-300'
            }`}
            style={tab === 'users'
              ? { background: 'linear-gradient(135deg, rgba(8,79,118,0.82), rgba(8,45,74,0.92))', border: '1px solid rgba(125,211,252,0.3)' }
              : { background: 'rgba(30,41,59,0.65)', border: '1px solid rgba(100,116,139,0.3)' }}
            onClick={() => setTab('users')}
          >
            <Users className="w-3.5 h-3.5" />
            用户管理
          </button>
        </div>

        {tab === 'control' ? <SystemControlBody /> : <UserManagementPage />}
      </div>
    </div>
  )
}
