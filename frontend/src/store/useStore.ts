import { create } from 'zustand'
import type { SatelliteData, LinkData } from '@/utils/constellationGenerator'

export interface VNFDeploy { vnf:string; node:string; cpu_used:number; mem_used:number; disk_used?: number }
export interface LinkDetail { src:string; dst:string; latency_ms:number; bandwidth_gbps:number }
export interface Deployment {
  deployment_id:string; request_id:string; sfc_name:string
  candidate_index:number; status:'in-progress'|'completed'|'failed'
  inference_latency_ms?: number
  source_node?: string
  destination_node?: string
  path_nodes?: string[]
  satisfies_constraints?: boolean
  violation_details?: string[]
  bottleneck_bandwidth_gbps?: number
  estimated_reliability?: number
  score_total?: number
  score_breakdown?: {
    latency: number
    resource: number
    reliability: number
    bandwidth: number
    dispersion: number
  }
  score_weights?: {
    latency: number
    resource: number
    reliability: number
    bandwidth: number
    dispersion: number
  }
  score_constraints?: {
    max_latency_ms: number
    min_bandwidth_gbps: number
    min_reliability: number
  }
  deployed_nodes:string[]; per_vnf:VNFDeploy[]; link_details:LinkDetail[]
  total_latency_ms:number; deployed_at:string; progress:number
}
export interface CandidateResult {
  requestId:string; sfcName:string; candidates:any[]; inferenceTime?:number
  topologyVersion:number; requestedTopk?:number; warning?:string
  fallbackOnly?: boolean
  deployableCount?: number
  sourceNode?: string
  destinationNode?: string
  scoringConfig?: {
    optimize: string
    constraints: {
      max_latency_ms: number
      min_bandwidth_gbps: number
      min_reliability: number
    }
    scoreWeights: {
      latency: number
      resource: number
      reliability: number
      bandwidth: number
      dispersion: number
    } | null
    vnfCount: number
  }
}
export interface DisplaySettings {
  showTexture:boolean; showBorders:boolean; showLatLon:boolean
  showLinks:boolean; linkOpacity:number
  showSky:boolean
  showAtmosphere:boolean
  showFPS:boolean
  renderQuality:'high'|'balanced'|'performance'
  rotationSpeed:number
}
export interface Toast { id:string; message:string; type:'info'|'success'|'warning'|'error' }

interface Store {
  satellites: SatelliteData[]; links: LinkData[]
  selectedSatellite: SatelliteData | null
  selectedLink: LinkData | null
  candidateResult: CandidateResult | null
  deployments: Deployment[]; highlightedDeploymentIds: string[]
  topologyVersion: number
  backendTopologySynced: boolean
  display: DisplaySettings; toasts: Toast[]

  setSatellites: (s:SatelliteData[]) => void
  setLinks: (l:LinkData[]) => void
  setSelectedSatellite: (s:SatelliteData|null) => void
  setSelectedLink: (l:LinkData|null) => void
  setCandidateResult: (c:CandidateResult|null) => void
  bumpTopologyVersion: () => void
  setBackendTopologySynced: (synced:boolean) => void
  addDeployment: (d:Deployment) => void
  updateDeployment: (id:string, p:Partial<Deployment>) => void
  removeDeployment: (id:string) => void
  clearDeployments: () => void
  toggleHighlightedDeployment: (id:string) => void
  clearHighlightedDeployments: () => void
  setDisplay: (s:Partial<DisplaySettings>) => void
  addToast: (msg:string, type?:Toast['type']) => void
  removeToast: (id:string) => void
}

export const useStore = create<Store>((set) => ({
  satellites:[], links:[], selectedSatellite:null, selectedLink:null,
  candidateResult:null, deployments:[], highlightedDeploymentIds:[], toasts:[],
  topologyVersion: 0, backendTopologySynced: false,
  display:{
    showTexture:true,
    showBorders:true,
    showLatLon:false,
    showLinks:false,
    linkOpacity:0.7,
    showSky:true,
    showAtmosphere:true,
    showFPS:false,
    renderQuality:'high',
    rotationSpeed:0
  },

  setSatellites: s => set({ satellites:s }),
  setLinks: l => set({ links:l, selectedLink:null }),
  setSelectedSatellite: s => set({ selectedSatellite:s }),
  setSelectedLink: l => set({ selectedLink:l }),
  setCandidateResult: c => set({ candidateResult:c }),
  bumpTopologyVersion: () => set(s => ({ topologyVersion: s.topologyVersion + 1 })),
  setBackendTopologySynced: synced => set({ backendTopologySynced: synced }),
  addDeployment: d => set(s => ({ deployments:[d,...s.deployments] })),
  updateDeployment: (id,p) => set(s => ({ deployments:s.deployments.map(d => d.deployment_id===id?{...d,...p}:d) })),
  removeDeployment: id => set(s => ({
    deployments:s.deployments.filter(d=>d.deployment_id!==id),
    highlightedDeploymentIds: s.highlightedDeploymentIds.filter(x => x !== id),
  })),
  clearDeployments: () => set({ deployments:[], highlightedDeploymentIds:[] }),
  toggleHighlightedDeployment: id => set(s => ({
    highlightedDeploymentIds: s.highlightedDeploymentIds.includes(id)
      ? s.highlightedDeploymentIds.filter(x => x !== id)
      : [...s.highlightedDeploymentIds, id]
  })),
  clearHighlightedDeployments: () => set({ highlightedDeploymentIds:[] }),
  setDisplay: s => set(st => ({ display:{...st.display,...s} })),
  addToast: (msg,type='info') => {
    const id = Date.now().toString()
    set(s => ({ toasts:[...s.toasts,{id,message:msg,type}] }))
    setTimeout(() => set(s => ({ toasts:s.toasts.filter(t=>t.id!==id) })), 4000)
  },
  removeToast: id => set(s => ({ toasts:s.toasts.filter(t=>t.id!==id) })),
}))
