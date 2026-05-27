import axios from 'axios'
import { getAuthToken } from '@/auth/session'
import type { AuthUser, UserRole } from '@/types/auth'

const http = axios.create({ baseURL: '/api/v1', timeout: 30000, headers: { 'Content-Type': 'application/json' } })

http.interceptors.request.use((config) => {
  const token = getAuthToken()
  if (token) {
    const headers = (config.headers ?? {}) as any
    headers.Authorization = `Bearer ${token}`
    config.headers = headers
  }
  return config
})

class APIClient {
  async login(p: { username: string; password: string }) {
    return (await http.post('/auth/login', p)).data as { token: string; user: AuthUser }
  }

  async register(p: { username: string; password: string; confirm_password: string }) {
    return (await http.post('/auth/register', p)).data as { token: string; user: AuthUser }
  }

  async getCurrentUser() {
    return (await http.get('/auth/me')).data as { user: AuthUser }
  }

  async listUsers() {
    return (await http.get('/users')).data as { users: AuthUser[] }
  }

  async createUser(p: { username: string; password: string; role: UserRole }) {
    return (await http.post('/users', p)).data as { user: AuthUser }
  }

  async updateUser(userId: number, p: { username?: string; password?: string; confirm_password?: string; role?: UserRole }) {
    return (await http.put(`/users/${userId}`, p)).data as { user: AuthUser }
  }

  async deleteUser(userId: number) {
    return (await http.delete(`/users/${userId}`)).data as { status: string; user_id: number }
  }

  async getTopology() { return (await http.get('/topology')).data }
  
  async generateTopology(p: any) { 
    try {
      return (await http.post('/topology/generate', p, { timeout: 180000 })).data 
    } catch (err: any) {
      console.warn('后端API调用失败')
      throw err
    }
  }
  
  async getSatellites() { return (await http.get('/satellites?legacy=1')).data }
  async getSatellite(id: string) { return (await http.get(`/satellite/${encodeURIComponent(id)}`)).data }
  async getSatellitesPage(p: {
    page?: number
    page_size?: number
    q?: string
    status?: 'all' | 'active' | 'down'
    plane?: number
    sort_by?: 'id' | 'status' | 'plane' | 'cpu_available' | 'mem_available' | 'disk_available' | 'node_reliability' | 'vnf_count'
    sort_order?: 'asc' | 'desc'
  } = {}) {
    return (await http.get('/satellites', { params: p })).data as {
      items: any[]
      total: number
      page: number
      page_size: number
      total_pages: number
    }
  }
  async deleteSatellite(nodeId: string) { return (await http.delete(`/satellite/${encodeURIComponent(nodeId)}`)).data }

  async getDynamicStatus() { return (await http.get('/topology/dynamic/status')).data }
  async getControlConfig() { return (await http.get('/runtime/config')).data }
  async updateControlConfig(p: { resource_sampling_interval_sec?: number; simulation_speed?: number; apply_now?: boolean }) {
    return (await http.put('/runtime/config', p)).data
  }

  async startDynamicSimulation(p: {
    sampling_interval_sec?: number
    simulation_speed?: number
  } = {}) {
    return (await http.post('/topology/dynamic/start', p)).data
  }

  async stopDynamicSimulation() {
    return (await http.post('/topology/dynamic/stop', {})).data
  }

  async stepDynamicSimulation() {
    return (await http.post('/topology/dynamic/step', {})).data
  }

  async injectDynamicFaults(p: {
    entity_type?: 'node' | 'link'
    action?: 'inject' | 'remove' | 'extend'
    node_id?: string
    node_ids?: string[]
    source?: string
    target?: string
    src?: string
    dst?: string
    links?: Array<{ source?: string; target?: string; src?: string; dst?: string }>
    link_id?: string
    link_ids?: string[]
    fault_type?: string
    duration_sec?: number
    duration_seconds?: number
    delta_seconds?: number
    batch_count?: number
    only_active?: boolean
    overwrite_existing?: boolean
  } = {}) {
    return (await http.post('/topology/dynamic/faults/inject', p)).data
  }
  
  async planSFC(p: any) { return (await http.post('/sfc/plan', p, { timeout: 180000 })).data }

  async startSFCSession(p: any) {
    return (await http.post('/sfc/session/start', p, { timeout: 180000 })).data
  }

  async stopSFCSession(sessionId: string) {
    return (await http.post('/sfc/session/stop', { session_id: sessionId })).data
  }

  async recomputeSFCSession(sessionId: string, trigger = 'manual') {
    return (await http.post(`/sfc/session/${sessionId}/recompute`, { trigger })).data
  }

  async listSFCSessions() {
    return (await http.get('/sfc/sessions')).data
  }

  async getSFCSession(sessionId: string) {
    return (await http.get(`/sfc/session/${sessionId}`)).data
  }
  
  async deploySFC(p: { 
    request_id: string; 
    candidate_index: number;
    candidate: any; // 完整的候选方案
    core_nf_dependencies?: any[];
    custom_nf_bindings?: string[][];
    independent_core_nfs?: string[];
    custom_nf_independent?: string[];
    custom_core_graph?: boolean;
    allow_partial_core_nfs?: boolean;
    active_core_nf_types?: string[];
    core_nfs?: any[];
    core_network_id?: string;
    core_network_label?: string;
    sfc_name?: string;
    source_node?: string;
    destination_node?: string;
    path_nodes?: string[];
    inference_latency_ms?: number;
    score_breakdown?: any;
    score_weights?: any;
    score_constraints?: any;
    strategy_mode?: 'single_request' | 'session_continuous';
  }) { 
    return (await http.post('/sfc/deploy', p)).data 
  }
  
  async rollbackDeployment(id: string) { 
    return (await http.post('/sfc/rollback', { deployment_id: id })).data 
  }

  async setDeploymentRuntime(id: string, enabled: boolean) {
    return (await http.post('/sfc/deployment/runtime', { deployment_id: id, enabled }, { timeout: 180000 })).data
  }
  
  async getDeployments() { return (await http.get('/deployments')).data }
  async getUERANSIMDeployments() {
    return (await http.get('/ueransim/deployments')).data as { items: any[] }
  }
  async startUERANSIMVerification(p: { deployment_id: string; mode?: 'smoke' | 'reschedule'; flow?: 'smoke' | 'reschedule' }) {
    return (await http.post('/ueransim/verification/start', p, { timeout: 30000 })).data as { job: any }
  }
  async getUERANSIMVerification(jobId: string) {
    return (await http.get(`/ueransim/verification/${encodeURIComponent(jobId)}`, { timeout: 30000 })).data as { job: any }
  }
  async sendUERANSIMMessage(jobId: string, p: { from: 'ue1' | 'ue2'; to: 'ue1' | 'ue2'; message: string }) {
    return (await http.post(`/ueransim/verification/${encodeURIComponent(jobId)}/messages`, p, { timeout: 30000 })).data as { job: any; transport: string }
  }
  async stopUERANSIMVerification(jobId: string) {
    return (await http.post(`/ueransim/verification/${encodeURIComponent(jobId)}/stop`, {}, { timeout: 30000 })).data as { job: any }
  }
  async healthCheck() { return (await http.get('/health')).data }
}

export const apiClient = new APIClient()
