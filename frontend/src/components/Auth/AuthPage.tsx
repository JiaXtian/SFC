import { useMemo, useState, type FormEvent } from 'react'
import { ShieldCheck, UserPlus, LogIn } from 'lucide-react'
import { useAuth } from '@/auth/AuthContext'

type Mode = 'login' | 'register'

function toErrorText(error: any) {
  const backendMessage = String(error?.response?.data?.message ?? '')
  const backendDetails = String(error?.response?.data?.details ?? '')
  if (backendMessage || backendDetails) {
    return [backendMessage, backendDetails].filter(Boolean).join('：')
  }
  return String(error?.message ?? '请求失败，请稍后重试')
}

export default function AuthPage({ portal = 'main' as 'main' | 'control' }) {
  const { login, register } = useAuth()
  const [mode, setMode] = useState<Mode>('login')
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [confirmPassword, setConfirmPassword] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [error, setError] = useState('')

  const mainUrl = useMemo(
    () => String((import.meta as any)?.env?.VITE_MAIN_SCREEN_URL || 'http://localhost:3001'),
    [],
  )

  const redirectToMain = () => {
    if (window.location.origin === mainUrl) {
      window.location.href = '/'
      return
    }
    window.location.href = mainUrl
  }

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (submitting) return
    setError('')
    if (!username.trim()) {
      setError('请输入用户名')
      return
    }
    if (password.length < 6) {
      setError('密码长度至少 6 位')
      return
    }
    if (mode === 'register' && password !== confirmPassword) {
      setError('两次输入的密码不一致')
      return
    }

    setSubmitting(true)
    try {
      if (mode === 'login') {
        await login(username.trim(), password)
      } else {
        await register(username.trim(), password, confirmPassword)
      }
      redirectToMain()
    } catch (err: any) {
      setError(toErrorText(err))
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <div className="auth-shell w-screen h-screen overflow-hidden flex items-center justify-center px-4">
      <div className="auth-bg-orb auth-bg-orb-a" />
      <div className="auth-bg-orb auth-bg-orb-b" />
      <div className="auth-bg-orb auth-bg-orb-c" />
      <div className="auth-grid-pattern" />
      <div className="auth-card w-full max-w-[450px] p-7 sm:p-8 relative z-10">
        <div className="flex items-center gap-2.5 mb-5">
          <div className="h-10 w-10 rounded-xl flex items-center justify-center auth-logo-wrap">
            <ShieldCheck className="w-5 h-5 text-cyan-200" />
          </div>
          <div>
            <div className="text-slate-100 text-[18px] font-semibold">核心网智能编排系统</div>
            <div className="text-cyan-200/70 text-[12px]">
              {portal === 'control' ? '系统控制中心登录' : '大屏展示系统登录'}
            </div>
          </div>
        </div>

        <div className="auth-switch mb-5">
          <button
            className={`auth-switch-item ${mode === 'login' ? 'active' : ''}`}
            onClick={() => setMode('login')}
            type="button"
          >
            <LogIn className="w-3.5 h-3.5" />
            登录
          </button>
          <button
            className={`auth-switch-item ${mode === 'register' ? 'active' : ''}`}
            onClick={() => setMode('register')}
            type="button"
          >
            <UserPlus className="w-3.5 h-3.5" />
            注册
          </button>
        </div>

        <form onSubmit={submit} className="space-y-3.5">
          <label className="block text-[12px] text-slate-300">
            用户名
            <input
              className="auth-input mt-1.5"
              placeholder="请输入用户名"
              value={username}
              onChange={(e) => setUsername(e.target.value)}
              autoComplete="username"
              maxLength={32}
            />
          </label>

          <label className="block text-[12px] text-slate-300">
            密码
            <input
              className="auth-input mt-1.5"
              type="password"
              placeholder="请输入密码（至少6位）"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              autoComplete={mode === 'login' ? 'current-password' : 'new-password'}
            />
          </label>

          {mode === 'register' && (
            <label className="block text-[12px] text-slate-300">
              重复输入密码
              <input
                className="auth-input mt-1.5"
                type="password"
                placeholder="请再次输入密码"
                value={confirmPassword}
                onChange={(e) => setConfirmPassword(e.target.value)}
                autoComplete="new-password"
              />
            </label>
          )}

          {error && (
            <div className="text-[12px] text-rose-300 bg-rose-500/10 border border-rose-300/30 rounded-lg px-3 py-2">
              {error}
            </div>
          )}

          <button
            disabled={submitting}
            className="auth-primary-btn w-full h-11 rounded-xl text-sm font-semibold text-white disabled:opacity-60 disabled:cursor-not-allowed"
            type="submit"
          >
            {submitting ? '处理中...' : mode === 'login' ? '登录并进入系统' : '注册并进入系统'}
          </button>
        </form>

        <div className="mt-4 text-[11px] text-slate-400/90">
          默认账号：
          <span className="text-cyan-200 ml-1">admin / 123456</span>
          <span className="mx-1 text-slate-500">|</span>
          <span className="text-cyan-200">user / 123456</span>
        </div>
      </div>
    </div>
  )
}
