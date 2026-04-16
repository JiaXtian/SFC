import { useEffect, useMemo, useState } from 'react'
import { Plus, Pencil, Trash2, RefreshCw, Shield, User } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useAuth } from '@/auth/AuthContext'
import type { AuthUser, UserRole } from '@/types/auth'

type CreateForm = {
  username: string
  password: string
  role: UserRole
}

type EditForm = {
  id: number
  username: string
  password: string
  confirmPassword: string
  role: UserRole
}

function toErrorText(error: any) {
  const message = String(error?.response?.data?.message ?? '')
  const details = String(error?.response?.data?.details ?? '')
  return [message, details].filter(Boolean).join('：') || String(error?.message ?? '请求失败')
}

export default function UserManagementPage() {
  const { user: currentUser } = useAuth()
  const [users, setUsers] = useState<AuthUser[]>([])
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')
  const [creating, setCreating] = useState(false)
  const [saving, setSaving] = useState(false)
  const [createForm, setCreateForm] = useState<CreateForm>({
    username: '',
    password: '',
    role: 'user',
  })
  const [editForm, setEditForm] = useState<EditForm | null>(null)

  const sortedUsers = useMemo(() => {
    return [...users].sort((a, b) => Number(a.id) - Number(b.id))
  }, [users])

  const fetchUsers = async () => {
    setLoading(true)
    setError('')
    try {
      const data = await apiClient.listUsers()
      setUsers(Array.isArray(data?.users) ? data.users : [])
    } catch (err: any) {
      setError(toErrorText(err))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    fetchUsers()
  }, [])

  const submitCreate = async () => {
    if (creating) return
    if (!createForm.username.trim()) {
      setError('请输入用户名')
      return
    }
    if (createForm.password.length < 6) {
      setError('初始密码长度至少 6 位')
      return
    }
    setCreating(true)
    setError('')
    try {
      await apiClient.createUser({
        username: createForm.username.trim(),
        password: createForm.password,
        role: createForm.role,
      })
      setCreateForm({ username: '', password: '', role: 'user' })
      await fetchUsers()
    } catch (err: any) {
      setError(toErrorText(err))
    } finally {
      setCreating(false)
    }
  }

  const submitEdit = async () => {
    if (!editForm || saving) return
    if (!editForm.username.trim()) {
      setError('用户名不能为空')
      return
    }
    if (editForm.password && editForm.password !== editForm.confirmPassword) {
      setError('两次输入的新密码不一致')
      return
    }
    setSaving(true)
    setError('')
    try {
      await apiClient.updateUser(editForm.id, {
        username: editForm.username.trim(),
        role: editForm.role,
        ...(editForm.password ? {
          password: editForm.password,
          confirm_password: editForm.confirmPassword,
        } : {}),
      })
      setEditForm(null)
      await fetchUsers()
    } catch (err: any) {
      setError(toErrorText(err))
    } finally {
      setSaving(false)
    }
  }

  const removeUser = async (target: AuthUser) => {
    if (!window.confirm(`确认删除用户 ${target.username} 吗？`)) return
    setError('')
    try {
      await apiClient.deleteUser(Number(target.id))
      await fetchUsers()
    } catch (err: any) {
      setError(toErrorText(err))
    }
  }

  return (
    <div
      className="rounded-2xl p-4 h-full flex flex-col"
      style={{
        background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
        border: '1px solid rgba(112,168,208,0.28)',
        backdropFilter: 'blur(14px)',
      }}
    >
      <div className="flex items-center gap-2 mb-3">
        <Shield className="w-4.5 h-4.5 text-cyan-300" />
        <div className="text-[14px] uppercase tracking-wide text-cyan-100 font-semibold">用户管理</div>
        <button
          className="ml-auto h-8 px-3 rounded-lg text-[12px] text-cyan-100 flex items-center gap-1.5"
          style={{ background: 'rgba(14,39,67,0.7)', border: '1px solid rgba(125,211,252,0.28)' }}
          onClick={fetchUsers}
          disabled={loading}
        >
          <RefreshCw className={`w-3.5 h-3.5 ${loading ? 'animate-spin' : ''}`} />
          刷新
        </button>
      </div>

      {error && (
        <div className="mb-3 rounded-lg px-3 py-2 text-[12px] text-rose-200"
          style={{ background: 'rgba(244,63,94,0.12)', border: '1px solid rgba(251,113,133,0.28)' }}>
          {error}
        </div>
      )}

      <div className="rounded-xl p-3 mb-3"
        style={{ background: 'rgba(9,20,36,0.7)', border: '1px solid rgba(125,211,252,0.18)' }}>
        <div className="text-[12px] text-slate-300 mb-2">新增用户</div>
        <div className="grid grid-cols-12 gap-2">
          <input
            className="col-span-12 md:col-span-4 h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
            placeholder="用户名"
            value={createForm.username}
            onChange={(e) => setCreateForm((s) => ({ ...s, username: e.target.value }))}
          />
          <input
            className="col-span-12 md:col-span-4 h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
            placeholder="初始密码"
            type="password"
            value={createForm.password}
            onChange={(e) => setCreateForm((s) => ({ ...s, password: e.target.value }))}
          />
          <select
            className="col-span-6 md:col-span-2 h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
            value={createForm.role}
            onChange={(e) => setCreateForm((s) => ({ ...s, role: e.target.value as UserRole }))}
          >
            <option value="user">普通用户</option>
            <option value="admin">管理员</option>
          </select>
          <button
            className="col-span-6 md:col-span-2 h-9 rounded-lg text-[12px] font-semibold text-cyan-100 inline-flex items-center justify-center gap-1.5"
            style={{ background: 'linear-gradient(135deg, rgba(22,138,173,0.82), rgba(14,116,144,0.84))' }}
            onClick={submitCreate}
            disabled={creating}
          >
            <Plus className="w-3.5 h-3.5" />
            {creating ? '创建中' : '创建'}
          </button>
        </div>
      </div>

      <div className="flex-1 min-h-0 rounded-xl overflow-hidden border border-slate-500/20 bg-slate-900/35">
        <div className="grid grid-cols-12 px-3 py-2.5 text-[11px] text-slate-300 border-b border-slate-500/20 font-semibold">
          <div className="col-span-1">ID</div>
          <div className="col-span-3">用户名</div>
          <div className="col-span-2">角色</div>
          <div className="col-span-3">创建时间</div>
          <div className="col-span-3">操作</div>
        </div>
        <div className="h-[calc(100%-40px)] overflow-y-auto">
          {sortedUsers.map((u) => (
            <div key={u.id} className="grid grid-cols-12 items-center px-3 py-2 text-[12px] border-b border-slate-700/30">
              <div className="col-span-1 text-slate-300">{u.id}</div>
              <div className="col-span-3 text-cyan-100 flex items-center gap-1.5">
                <User className="w-3.5 h-3.5 text-cyan-300/80" />
                {u.username}
              </div>
              <div className="col-span-2">
                <span
                  className="px-2 py-0.5 rounded-full text-[10px]"
                  style={u.role === 'admin'
                    ? { background: 'rgba(56,189,248,0.15)', color: '#7dd3fc', border: '1px solid rgba(56,189,248,0.35)' }
                    : { background: 'rgba(148,163,184,0.16)', color: '#cbd5e1', border: '1px solid rgba(148,163,184,0.32)' }}
                >
                  {u.role === 'admin' ? '管理员' : '普通用户'}
                </span>
              </div>
              <div className="col-span-3 text-slate-300 text-[11px]">{u.created_at ?? '-'}</div>
              <div className="col-span-3 flex items-center gap-2">
                <button
                  className="h-7 px-2.5 rounded-md text-[11px] text-cyan-100 inline-flex items-center gap-1"
                  style={{ background: 'rgba(14,116,144,0.4)', border: '1px solid rgba(125,211,252,0.25)' }}
                  onClick={() => setEditForm({
                    id: Number(u.id),
                    username: u.username,
                    password: '',
                    confirmPassword: '',
                    role: (u.role === 'admin' ? 'admin' : 'user'),
                  })}
                >
                  <Pencil className="w-3.5 h-3.5" />
                  编辑
                </button>
                <button
                  className="h-7 px-2.5 rounded-md text-[11px] text-rose-200 inline-flex items-center gap-1 disabled:opacity-40"
                  style={{ background: 'rgba(190,24,93,0.25)', border: '1px solid rgba(251,113,133,0.3)' }}
                  onClick={() => removeUser(u)}
                  disabled={Number(currentUser?.id) === Number(u.id)}
                >
                  <Trash2 className="w-3.5 h-3.5" />
                  删除
                </button>
              </div>
            </div>
          ))}
          {!loading && sortedUsers.length === 0 && (
            <div className="text-center text-[12px] text-slate-400 py-8">暂无用户数据</div>
          )}
        </div>
      </div>

      {editForm && (
        <div className="fixed inset-0 z-[220] flex items-center justify-center" style={{ background: 'rgba(0,0,0,0.66)' }}>
          <div className="w-[500px] max-w-[92vw] rounded-2xl p-4"
            style={{ background: 'rgba(6,13,24,0.94)', border: '1px solid rgba(125,211,252,0.24)', backdropFilter: 'blur(12px)' }}>
            <div className="text-cyan-100 text-[15px] font-semibold mb-3">编辑用户 #{editForm.id}</div>
            <div className="space-y-2.5">
              <input
                className="w-full h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
                value={editForm.username}
                onChange={(e) => setEditForm((s) => s ? ({ ...s, username: e.target.value }) : s)}
                placeholder="用户名"
              />
              <div className="grid grid-cols-2 gap-2">
                <input
                  className="w-full h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
                  value={editForm.password}
                  type="password"
                  onChange={(e) => setEditForm((s) => s ? ({ ...s, password: e.target.value }) : s)}
                  placeholder="新密码（留空不修改）"
                />
                <input
                  className="w-full h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
                  value={editForm.confirmPassword}
                  type="password"
                  onChange={(e) => setEditForm((s) => s ? ({ ...s, confirmPassword: e.target.value }) : s)}
                  placeholder="确认新密码"
                />
              </div>
              <select
                className="w-full h-9 rounded-lg px-3 text-[12px] text-slate-100 bg-slate-900/60 border border-slate-500/25 focus:border-cyan-300/70 outline-none transition"
                value={editForm.role}
                onChange={(e) => setEditForm((s) => s ? ({ ...s, role: e.target.value as UserRole }) : s)}
              >
                <option value="user">普通用户</option>
                <option value="admin">管理员</option>
              </select>
            </div>
            <div className="mt-4 flex justify-end gap-2">
              <button
                className="h-9 px-3 rounded-lg text-[12px] text-slate-200"
                style={{ background: 'rgba(71,85,105,0.7)', border: '1px solid rgba(148,163,184,0.22)' }}
                onClick={() => setEditForm(null)}
              >
                取消
              </button>
              <button
                className="h-9 px-3 rounded-lg text-[12px] text-cyan-100"
                style={{ background: 'linear-gradient(135deg, rgba(14,116,144,0.85), rgba(8,47,73,0.9))', border: '1px solid rgba(125,211,252,0.28)' }}
                onClick={submitEdit}
                disabled={saving}
              >
                {saving ? '保存中...' : '保存变更'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
