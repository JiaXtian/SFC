import type { Deployment } from '@/store/useStore'

function toMs(raw: string | undefined): number {
  const t = Date.parse(String(raw ?? ''))
  return Number.isFinite(t) ? t : 0
}

export function formatSfcSeq(seq: number): string {
  const safe = Math.max(1, Math.floor(Number(seq) || 1))
  return `SFC-${String(safe).padStart(3, '0')}`
}

function parseSeq(label: string) {
  const m = /^SFC-(\d{1,})$/i.exec(String(label ?? '').trim())
  return m ? Number(m[1]) : 0
}

function extractSfcLabel(raw: string): string {
  const m = /\bSFC-(\d{1,})\b/i.exec(String(raw ?? ''))
  if (!m) return ''
  return formatSfcSeq(Number(m[1]))
}

export function buildSfcLabelMaps(deployments: Deployment[]) {
  const order = [...(Array.isArray(deployments) ? deployments : [])].sort((a: any, b: any) => {
    const ta = toMs(a?.deployed_at)
    const tb = toMs(b?.deployed_at)
    if (ta !== tb) return ta - tb
    const ra = String(a?.request_id ?? '')
    const rb = String(b?.request_id ?? '')
    if (ra !== rb) return ra.localeCompare(rb)
    return String(a?.deployment_id ?? '').localeCompare(String(b?.deployment_id ?? ''))
  })

  const bySession = new Map<string, string>()
  const byRequest = new Map<string, string>()
  const byDeployment = new Map<string, string>()
  const usedLabels = new Set<string>()

  order.forEach((dep: any) => {
    const sid = String(dep?.session_id ?? '')
    const rid = String(dep?.request_id ?? '')
    const did = String(dep?.deployment_id ?? '')
    const backendDid = String(dep?.backend_deployment_id ?? '')
    const preset = extractSfcLabel(String(dep?.sfc_name ?? ''))
    if (!preset) return
    const label = preset
    usedLabels.add(label)
    if (sid && !bySession.has(sid)) bySession.set(sid, label)
    if (rid && !byRequest.has(rid)) byRequest.set(rid, label)
    if (did && !byDeployment.has(did)) byDeployment.set(did, label)
    if (backendDid && !byDeployment.has(backendDid)) byDeployment.set(backendDid, label)
  })

  let seq = Math.max(1, ...Array.from(usedLabels).map((x) => parseSeq(x)).filter((n) => Number.isFinite(n) && n > 0)) + 1
  const allocLabel = () => {
    let label = formatSfcSeq(seq++)
    while (usedLabels.has(label)) label = formatSfcSeq(seq++)
    usedLabels.add(label)
    return label
  }

  order.forEach((dep: any) => {
    const sid = String(dep?.session_id ?? '')
    const rid = String(dep?.request_id ?? '')
    const did = String(dep?.deployment_id ?? '')
    const backendDid = String(dep?.backend_deployment_id ?? '')

    let label = ''
    if (did && byDeployment.has(did)) label = String(byDeployment.get(did))
    else if (backendDid && byDeployment.has(backendDid)) label = String(byDeployment.get(backendDid))
    else if (sid && bySession.has(sid)) label = String(bySession.get(sid))
    else if (rid && byRequest.has(rid)) label = String(byRequest.get(rid))
    else label = allocLabel()

    if (sid && !bySession.has(sid)) bySession.set(sid, label)
    if (rid && !byRequest.has(rid)) byRequest.set(rid, label)
    if (did && !byDeployment.has(did)) byDeployment.set(did, label)
    if (backendDid && !byDeployment.has(backendDid)) byDeployment.set(backendDid, label)
  })

  return { bySession, byRequest, byDeployment }
}

export function resolveSfcLabel(
  deployments: Deployment[],
  ids?: { sessionId?: string; requestId?: string; deploymentId?: string },
) {
  const maps = buildSfcLabelMaps(deployments)
  const sid = String(ids?.sessionId ?? '')
  const rid = String(ids?.requestId ?? '')
  const did = String(ids?.deploymentId ?? '')
  if (did && maps.byDeployment.has(did)) return String(maps.byDeployment.get(did))
  if (sid && maps.bySession.has(sid)) return String(maps.bySession.get(sid))
  if (rid && maps.byRequest.has(rid)) return String(maps.byRequest.get(rid))
  return 'SFC-未编号'
}
