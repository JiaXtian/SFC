import type { Deployment } from '@/store/useStore'

const REGISTRY_KEY = 'sfc_core_identity_registry_v2'

type Registry = {
  nextSeq: number
  bySession: Record<string, string>
  byRequest: Record<string, string>
  byDeployment: Record<string, string>
  byCoreId: Record<string, string>
}

function toMs(raw: string | undefined): number {
  const t = Date.parse(String(raw ?? ''))
  return Number.isFinite(t) ? t : 0
}

export function formatSfcSeq(seq: number): string {
  const safe = Math.max(1, Math.floor(Number(seq) || 1))
  return `CORE-${String(safe).padStart(2, '0')}`
}

function parseSeq(label: string) {
  const m = /^(?:CORE|SFC)-(\d{1,})$/i.exec(String(label ?? '').trim())
  return m ? Number(m[1]) : 0
}

function extractSfcLabel(raw: string): string {
  const m = /^(?:CORE|SFC)-(\d{1,})$/i.exec(String(raw ?? '').trim())
  if (!m) return ''
  return formatSfcSeq(Number(m[1]))
}

function blankRegistry(): Registry {
  return {
    nextSeq: 1,
    bySession: {},
    byRequest: {},
    byDeployment: {},
    byCoreId: {},
  }
}

function loadRegistry(): Registry {
  if (typeof window === 'undefined') return blankRegistry()
  try {
    const raw = window.localStorage.getItem(REGISTRY_KEY)
    if (!raw) return blankRegistry()
    const parsed = JSON.parse(raw)
    return {
      nextSeq: Math.max(1, Number(parsed?.nextSeq ?? 1) || 1),
      bySession: parsed?.bySession && typeof parsed.bySession === 'object' ? parsed.bySession : {},
      byRequest: parsed?.byRequest && typeof parsed.byRequest === 'object' ? parsed.byRequest : {},
      byDeployment: parsed?.byDeployment && typeof parsed.byDeployment === 'object' ? parsed.byDeployment : {},
      byCoreId: parsed?.byCoreId && typeof parsed.byCoreId === 'object' ? parsed.byCoreId : {},
    }
  } catch {
    return blankRegistry()
  }
}

function saveRegistry(registry: Registry) {
  if (typeof window === 'undefined') return
  try {
    window.localStorage.setItem(REGISTRY_KEY, JSON.stringify(registry))
  } catch {
    // Best-effort cache only.
  }
}

function allRegistryLabels(registry: Registry) {
  return [
    ...Object.values(registry.bySession),
    ...Object.values(registry.byRequest),
    ...Object.values(registry.byDeployment),
    ...Object.values(registry.byCoreId),
  ].filter(Boolean)
}

function allocateLabel(registry: Registry, usedLabels: Set<string>) {
  let seq = Math.max(
    Number(registry.nextSeq ?? 1) || 1,
    Math.max(0, ...Array.from(usedLabels).map((x) => parseSeq(x)).filter((n) => Number.isFinite(n) && n > 0)) + 1,
  )
  let label = formatSfcSeq(seq++)
  while (usedLabels.has(label)) label = formatSfcSeq(seq++)
  registry.nextSeq = seq
  usedLabels.add(label)
  return label
}

function allocateFirstUnused(usedLabels: Set<string>) {
  let seq = 1
  let label = formatSfcSeq(seq)
  while (usedLabels.has(label)) {
    seq += 1
    label = formatSfcSeq(seq)
  }
  return label
}

function idFromParts(parts: string[]) {
  const clean = parts.map((x) => String(x ?? '').trim()).filter(Boolean)
  if (clean.length > 0) return `core-${clean.join('-')}`
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return `core-${crypto.randomUUID()}`
  }
  return `core-${Date.now()}-${Math.random().toString(16).slice(2, 10)}`
}

export function rememberSfcIdentity(ids: {
  sessionId?: string
  requestId?: string
  deploymentId?: string
  backendDeploymentId?: string
  coreNetworkId?: string
  label?: string
  allowAllocate?: boolean
}) {
  const registry = loadRegistry()
  const used = new Set(allRegistryLabels(registry))
  const preset = extractSfcLabel(String(ids.label ?? ''))
  const existing =
    (ids.coreNetworkId && registry.byCoreId[ids.coreNetworkId]) ||
    (ids.deploymentId && registry.byDeployment[ids.deploymentId]) ||
    (ids.backendDeploymentId && registry.byDeployment[ids.backendDeploymentId]) ||
    (ids.sessionId && registry.bySession[ids.sessionId]) ||
    (ids.requestId && registry.byRequest[ids.requestId]) ||
    ''
  const label = preset || existing || (ids.allowAllocate ? allocateLabel(registry, used) : '')
  const coreNetworkId = String(ids.coreNetworkId ?? idFromParts([
    String(ids.deploymentId ?? ids.backendDeploymentId ?? ''),
    String(ids.sessionId ?? ''),
    String(ids.requestId ?? ''),
  ]))

  if (label) {
    if (ids.sessionId) registry.bySession[String(ids.sessionId)] = label
    if (ids.requestId) registry.byRequest[String(ids.requestId)] = label
    if (ids.deploymentId) registry.byDeployment[String(ids.deploymentId)] = label
    if (ids.backendDeploymentId) registry.byDeployment[String(ids.backendDeploymentId)] = label
    if (coreNetworkId) registry.byCoreId[coreNetworkId] = label
    saveRegistry(registry)
  }
  return { core_network_id: coreNetworkId, core_network_label: label }
}

export function reserveSfcIdentity(ids: {
  sessionId?: string
  requestId?: string
  deploymentId?: string
  backendDeploymentId?: string
  coreNetworkId?: string
  label?: string
}) {
  return rememberSfcIdentity({ ...ids, allowAllocate: true })
}

export function reserveSfcIdentityForPlan(
  deployments: Deployment[],
  ids: {
    requestId: string
    coreNetworkId?: string
  },
) {
  const registry = loadRegistry()
  const requestId = String(ids.requestId ?? '')
  const existing = requestId ? registry.byRequest[requestId] : ''
  if (existing) {
    return rememberSfcIdentity({
      requestId,
      coreNetworkId: ids.coreNetworkId,
      label: existing,
    })
  }

  const usedLabels = new Set<string>()
  ;(Array.isArray(deployments) ? deployments : []).forEach((dep: any) => {
    const coreId = String(dep?.core_network_id ?? '')
    const did = String(dep?.deployment_id ?? '')
    const backendDid = String(dep?.backend_deployment_id ?? '')
    const sid = String(dep?.session_id ?? '')
    const rid = String(dep?.request_id ?? '')
    const label =
      extractSfcLabel(String(dep?.core_network_label ?? dep?.sfc_name ?? '')) ||
      (coreId && registry.byCoreId[coreId]) ||
      (did && registry.byDeployment[did]) ||
      (backendDid && registry.byDeployment[backendDid]) ||
      (sid && registry.bySession[sid]) ||
      (rid && registry.byRequest[rid]) ||
      ''
    const normalized = extractSfcLabel(label)
    if (normalized) usedLabels.add(normalized)
  })

  const label = allocateFirstUnused(usedLabels)
  const maxUsedSeq = Math.max(0, ...Array.from(usedLabels).map((x) => parseSeq(x)))
  registry.nextSeq = Math.max(registry.nextSeq, maxUsedSeq + 2)
  saveRegistry(registry)

  return rememberSfcIdentity({
    requestId,
    coreNetworkId: ids.coreNetworkId,
    label,
  })
}

export function clearSfcIdentityRegistry() {
  if (typeof window === 'undefined') return
  try {
    window.localStorage.removeItem(REGISTRY_KEY)
  } catch {
    // Best-effort cache only.
  }
}

export function buildSfcLabelMaps(deployments: Deployment[]) {
  const registry = loadRegistry()
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
  const byCoreId = new Map<string, string>()
  const usedLabels = new Set<string>(allRegistryLabels(registry))

  Object.entries(registry.bySession).forEach(([k, v]) => { if (k && v) bySession.set(k, v) })
  Object.entries(registry.byRequest).forEach(([k, v]) => { if (k && v) byRequest.set(k, v) })
  Object.entries(registry.byDeployment).forEach(([k, v]) => { if (k && v) byDeployment.set(k, v) })
  Object.entries(registry.byCoreId).forEach(([k, v]) => { if (k && v) byCoreId.set(k, v) })

  order.forEach((dep: any) => {
    const sid = String(dep?.session_id ?? '')
    const rid = String(dep?.request_id ?? '')
    const did = String(dep?.deployment_id ?? '')
    const backendDid = String(dep?.backend_deployment_id ?? '')
    const coreId = String(dep?.core_network_id ?? '')
    const preset = extractSfcLabel(String(dep?.core_network_label ?? dep?.sfc_name ?? ''))
    const label =
      preset ||
      (coreId && byCoreId.get(coreId)) ||
      (did && byDeployment.get(did)) ||
      (backendDid && byDeployment.get(backendDid)) ||
      (sid && bySession.get(sid)) ||
      (rid && byRequest.get(rid)) ||
      ''
    if (!label) return
    usedLabels.add(label)
    if (sid) bySession.set(sid, label)
    if (rid) byRequest.set(rid, label)
    if (did) byDeployment.set(did, label)
    if (backendDid) byDeployment.set(backendDid, label)
    if (coreId) byCoreId.set(coreId, label)
  })

  order.forEach((dep: any) => {
    const sid = String(dep?.session_id ?? '')
    const rid = String(dep?.request_id ?? '')
    const did = String(dep?.deployment_id ?? '')
    const backendDid = String(dep?.backend_deployment_id ?? '')
    const coreId = String(dep?.core_network_id ?? '')

    let label = ''
    if (coreId && byCoreId.has(coreId)) label = String(byCoreId.get(coreId))
    else if (did && byDeployment.has(did)) label = String(byDeployment.get(did))
    else if (backendDid && byDeployment.has(backendDid)) label = String(byDeployment.get(backendDid))
    else if (sid && bySession.has(sid)) label = String(bySession.get(sid))
    else if (rid && byRequest.has(rid)) label = String(byRequest.get(rid))
    else label = allocateLabel(registry, usedLabels)

    if (sid) bySession.set(sid, label)
    if (rid) byRequest.set(rid, label)
    if (did) byDeployment.set(did, label)
    if (backendDid) byDeployment.set(backendDid, label)
    if (coreId) byCoreId.set(coreId, label)
  })

  registry.bySession = Object.fromEntries(bySession)
  registry.byRequest = Object.fromEntries(byRequest)
  registry.byDeployment = Object.fromEntries(byDeployment)
  registry.byCoreId = Object.fromEntries(byCoreId)
  saveRegistry(registry)

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
  if (!sid && !rid && !did) return 'CORE-未编号'
  return 'CORE-未编号'
}
