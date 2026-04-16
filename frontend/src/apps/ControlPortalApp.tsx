import ControlCenterApp from '@/ControlCenterApp'
import { useAuth } from '@/auth/AuthContext'
import AuthPage from '@/components/Auth/AuthPage'

export default function ControlPortalApp() {
  const { loading, user, logout } = useAuth()
  const mainUrl = String((import.meta as any)?.env?.VITE_MAIN_SCREEN_URL || 'http://localhost:3001')

  if (loading) {
    return (
      <div className="w-screen h-screen bg-slate-950 flex items-center justify-center text-cyan-200 text-sm">
        正在加载登录状态...
      </div>
    )
  }

  if (!user) return <AuthPage portal="control" />

  if (user.role !== 'admin') {
    return (
      <div className="w-screen h-screen flex items-center justify-center px-6"
        style={{ background: 'radial-gradient(1200px 700px at 50% -10%, rgba(30,86,142,0.26), rgba(2,8,19,0.95) 58%, #02040a 100%)' }}>
        <div className="max-w-md w-full rounded-2xl p-6 text-center"
          style={{ background: 'rgba(255,255,255,0.05)', border: '1px solid rgba(125,211,252,0.25)', backdropFilter: 'blur(14px)' }}>
          <div className="text-cyan-100 text-xl font-semibold">当前账号无权限访问系统控制中心</div>
          <div className="mt-2 text-slate-300 text-sm">普通用户仅允许访问大屏展示主页面。</div>
          <div className="mt-5 flex gap-2">
            <button
              className="flex-1 h-10 rounded-lg text-cyan-100 font-medium"
              style={{ background: 'linear-gradient(135deg, rgba(10,67,118,0.8), rgba(8,36,69,0.8))' }}
              onClick={() => {
                window.location.href = mainUrl
              }}
            >
              返回大屏主页面
            </button>
            <button
              className="h-10 px-4 rounded-lg text-slate-100 font-medium"
              style={{ background: 'rgba(51,65,85,0.72)', border: '1px solid rgba(148,163,184,0.28)' }}
              onClick={logout}
            >
              退出登录
            </button>
          </div>
        </div>
      </div>
    )
  }

  return <ControlCenterApp />
}
