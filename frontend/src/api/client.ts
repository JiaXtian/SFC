import axios from 'axios'

const http = axios.create({ baseURL: '/api/v1', timeout: 30000, headers: { 'Content-Type': 'application/json' } })

class APIClient {
  async getTopology() { return (await http.get('/topology')).data }
  
  async generateTopology(p: any) { 
    try {
      return (await http.post('/topology/generate', p, { timeout: 1800000 })).data 
    } catch (err: any) {
      console.warn('后端API调用失败')
      throw err
    }
  }
  
  async getSatellites() { return (await http.get('/satellites')).data }
  async getRuntimeStatus() { return (await http.get('/satellites/runtime/status')).data }
  async getPersistenceStatus() { return (await http.get('/persistence/status')).data }
  async collectSatelliteTelemetry(force = false) {
    return (await http.post('/satellites/runtime/collect', { force })).data
  }
  async startSatellitePods(nodeIds: string[], recreateContainers = false) {
    return (await http.post(
      '/satellites/pods/start',
      { node_ids: nodeIds, recreate_containers: recreateContainers },
      { timeout: 600000 },
    )).data
  }
  async stopSatellitePods(nodeIds: string[], removeContainers = false) {
    return (await http.post(
      '/satellites/pods/stop',
      { node_ids: nodeIds, remove_containers: removeContainers },
      { timeout: 600000 },
    )).data
  }
  async stopAllSatellitePods(removeContainers = false) {
    return (await http.post(
      '/satellites/pods/stop_all',
      { remove_containers: removeContainers },
      { timeout: 600000 },
    )).data
  }
  async deleteSatelliteNodes(nodeIds: string[], removeContainers = true) {
    return (await http.post(
      '/satellites/nodes/delete',
      { node_ids: nodeIds, remove_containers: removeContainers },
      { timeout: 600000 },
    )).data
  }
  async resetPersistence(confirm = true) {
    return (await http.post('/persistence/reset', { confirm })).data
  }

  async getDynamicStatus() { return (await http.get('/topology/dynamic/status')).data }

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
    entity_type?: 'node'
    action?: 'inject' | 'remove' | 'extend'
    node_id?: string
    node_ids?: string[]
    fault_type?: string
    ttl_ticks?: number
    delta_ttl_ticks?: number
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
  }) { 
    return (await http.post('/sfc/deploy', p)).data 
  }
  
  async rollbackDeployment(id: string) { 
    return (await http.post('/sfc/rollback', { deployment_id: id })).data 
  }
  
  async getDeployments() { return (await http.get('/deployments')).data }
  async healthCheck() { return (await http.get('/health')).data }
}

export const apiClient = new APIClient()
