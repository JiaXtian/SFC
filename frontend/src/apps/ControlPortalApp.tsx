import ControlCenterApp from '@/ControlCenterApp'
import { useAuth } from '@/auth/AuthContext'
import AuthPage from '@/components/Auth/AuthPage'

export default function ControlPortalApp() {
  const { loading, user } = useAuth()

  if (loading) {
    return (
      <div className="w-screen h-screen bg-slate-950 flex items-center justify-center text-cyan-200 text-sm">
        正在加载登录状态...
      </div>
    )
  }

  if (!user) return <AuthPage portal="control" />

  return <ControlCenterApp />
}
