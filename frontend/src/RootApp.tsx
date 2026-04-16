import { AuthProvider } from '@/auth/AuthContext'
import MainPortalApp from '@/apps/MainPortalApp'
import ControlPortalApp from '@/apps/ControlPortalApp'

export default function RootApp() {
  return (
    <AuthProvider>
      {__SFC_APP_TARGET__ === 'control' ? <ControlPortalApp /> : <MainPortalApp />}
    </AuthProvider>
  )
}
