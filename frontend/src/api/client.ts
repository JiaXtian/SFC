import axios from 'axios'

const http = axios.create({ baseURL: '/api/v1', timeout: 30000, headers: { 'Content-Type': 'application/json' } })

class APIClient {
  async getTopology() { return (await http.get('/topology')).data }
  
  async generateTopology(p: any) { 
    try {
      return (await http.post('/topology/generate', p, { timeout: 180000 })).data 
    } catch (err: any) {
      console.warn('后端API调用失败')
      throw err
    }
  }
  
  async getSatellites() { return (await http.get('/satellites')).data }
  
  async planSFC(p: any) { return (await http.post('/sfc/plan', p, { timeout: 180000 })).data }
  
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
