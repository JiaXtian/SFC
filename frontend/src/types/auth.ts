export type UserRole = 'admin' | 'user'

export interface AuthUser {
  id: number
  username: string
  role: UserRole
  created_at?: string
  updated_at?: string
}
