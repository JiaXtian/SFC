import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import { apiClient } from '@/api/client'
import { clearAuthSession, getAuthToken, getAuthUserFromSession, setAuthSession } from './session'
import type { AuthUser } from '@/types/auth'

type AuthContextValue = {
  loading: boolean
  user: AuthUser | null
  login: (username: string, password: string) => Promise<AuthUser>
  register: (username: string, password: string, confirmPassword: string) => Promise<AuthUser>
  logout: () => void
  refreshUser: () => Promise<AuthUser | null>
}

const AuthContext = createContext<AuthContextValue | null>(null)

export function AuthProvider({ children }: { children: ReactNode }) {
  const [loading, setLoading] = useState(true)
  const [user, setUser] = useState<AuthUser | null>(() => getAuthUserFromSession())

  const refreshUser = useCallback(async () => {
    const token = getAuthToken()
    if (!token) {
      setUser(null)
      return null
    }
    try {
      const data = await apiClient.getCurrentUser()
      const u = data?.user as AuthUser
      if (!u || !u.username) throw new Error('invalid_user_payload')
      setAuthSession(token, u)
      setUser(u)
      return u
    } catch {
      clearAuthSession()
      setUser(null)
      return null
    }
  }, [])

  useEffect(() => {
    let disposed = false
    ;(async () => {
      try {
        await refreshUser()
      } finally {
        if (!disposed) setLoading(false)
      }
    })()
    return () => {
      disposed = true
    }
  }, [refreshUser])

  const login = useCallback(async (username: string, password: string) => {
    const data = await apiClient.login({ username, password })
    const token = String(data?.token ?? '')
    const u = data?.user as AuthUser
    if (!token || !u?.username) {
      throw new Error('登录返回异常')
    }
    setAuthSession(token, u)
    setUser(u)
    return u
  }, [])

  const register = useCallback(async (username: string, password: string, confirmPassword: string) => {
    const data = await apiClient.register({
      username,
      password,
      confirm_password: confirmPassword,
    })
    const token = String(data?.token ?? '')
    const u = data?.user as AuthUser
    if (!token || !u?.username) {
      throw new Error('注册返回异常')
    }
    setAuthSession(token, u)
    setUser(u)
    return u
  }, [])

  const logout = useCallback(() => {
    clearAuthSession()
    setUser(null)
  }, [])

  const value = useMemo<AuthContextValue>(() => ({
    loading,
    user,
    login,
    register,
    logout,
    refreshUser,
  }), [loading, user, login, register, logout, refreshUser])

  return (
    <AuthContext.Provider value={value}>
      {children}
    </AuthContext.Provider>
  )
}

export function useAuth() {
  const ctx = useContext(AuthContext)
  if (!ctx) throw new Error('useAuth must be used within AuthProvider')
  return ctx
}
