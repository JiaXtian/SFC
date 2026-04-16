import type { AuthUser } from '@/types/auth'

const TOKEN_COOKIE = 'sfc_token'
const USER_ID_COOKIE = 'sfc_uid'
const USERNAME_COOKIE = 'sfc_uname'
const USER_ROLE_COOKIE = 'sfc_role'

function setCookie(name: string, value: string, days = 7) {
  const expires = new Date(Date.now() + days * 24 * 60 * 60 * 1000).toUTCString()
  document.cookie = `${name}=${encodeURIComponent(value)}; expires=${expires}; path=/; SameSite=Lax`
}

function getCookie(name: string): string {
  const target = `${name}=`
  const cookies = document.cookie ? document.cookie.split('; ') : []
  for (const item of cookies) {
    if (item.startsWith(target)) {
      return decodeURIComponent(item.slice(target.length))
    }
  }
  return ''
}

function clearCookie(name: string) {
  document.cookie = `${name}=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/; SameSite=Lax`
}

export function getAuthToken() {
  return getCookie(TOKEN_COOKIE)
}

export function setAuthSession(token: string, user: AuthUser) {
  setCookie(TOKEN_COOKIE, token, 7)
  setCookie(USER_ID_COOKIE, String(user.id ?? ''), 7)
  setCookie(USERNAME_COOKIE, user.username ?? '', 7)
  setCookie(USER_ROLE_COOKIE, user.role ?? 'user', 7)
}

export function clearAuthSession() {
  clearCookie(TOKEN_COOKIE)
  clearCookie(USER_ID_COOKIE)
  clearCookie(USERNAME_COOKIE)
  clearCookie(USER_ROLE_COOKIE)
}

export function getAuthUserFromSession(): AuthUser | null {
  const token = getAuthToken()
  if (!token) return null
  const idRaw = getCookie(USER_ID_COOKIE)
  const username = getCookie(USERNAME_COOKIE)
  const roleRaw = getCookie(USER_ROLE_COOKIE)
  const id = Number(idRaw)
  if (!username || !Number.isFinite(id) || id <= 0) return null
  const role = roleRaw === 'admin' ? 'admin' : 'user'
  return { id, username, role }
}
