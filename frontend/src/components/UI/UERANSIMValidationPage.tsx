import { useEffect, useMemo, useState } from 'react'
import { Activity, AlertTriangle, CheckCircle2, Circle, GitCompare, RadioTower, RefreshCw, Send, Square, Terminal, XCircle } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'
import { formatSfcSeq, resolveSfcLabel } from '@/utils/sfcLabel'

type Direction = 'ue1' | 'ue2'
type ValidationFlow = 'smoke' | 'reschedule'
type PersistedValidationState = {
  jobId?: string
  selectedId?: string
  flow?: ValidationFlow
  from?: Direction
  updatedAt?: number
}

function pillClass(ok: boolean) {
  return ok ? 'text-emerald-200 bg-emerald-500/20 border-emerald-400/30' : 'text-amber-200 bg-amber-500/20 border-amber-400/30'
}

function stepIcon(status: string) {
  if (status === 'success') return <CheckCircle2 className="w-3.5 h-3.5 text-emerald-300" />
  if (status === 'failed') return <XCircle className="w-3.5 h-3.5 text-rose-300" />
  if (status === 'running') return <Activity className="w-3.5 h-3.5 text-cyan-300 animate-pulse" />
  return <Circle className="w-3.5 h-3.5 text-slate-500" />
}

function fmtTime(raw: any) {
  return String(raw ?? '').replace('T', ' ').slice(0, 19) || '-'
}

function deploymentLabel(dep: any, deployments: any[]) {
  const rawName = String(dep?.sfc_name ?? dep?.core_name ?? dep?.name ?? '').trim()
  const namedSeq = /\b(?:CORE|SFC)-(\d{1,})\b/i.exec(rawName)
  if (namedSeq) return formatSfcSeq(Number(namedSeq[1]))
  if (rawName) return rawName
  const id = String(dep?.deployment_id ?? dep?.backend_deployment_id ?? '')
  const idSeq = /\b(?:CORE|SFC)-(\d{1,})\b/i.exec(id)
  if (idSeq) return formatSfcSeq(Number(idSeq[1]))
  return resolveSfcLabel(deployments as any, {
    deploymentId: id,
    sessionId: String(dep?.session_id ?? ''),
    requestId: String(dep?.request_id ?? ''),
  })
}

const VALIDATION_STATE_KEY = 'sfc.ueransim.validation.state'

function normalizeFlow(raw: any): ValidationFlow | undefined {
  if (raw === 'smoke' || raw === 'reschedule') return raw
  return undefined
}

function normalizeDirection(raw: any): Direction | undefined {
  if (raw === 'ue1' || raw === 'ue2') return raw
  return undefined
}

function isActiveJobStatus(raw: any) {
  const status = String(raw ?? '')
  return status === 'running' || status === 'queued'
}

function readPersistedValidationState(): PersistedValidationState {
  if (typeof window === 'undefined') return {}
  try {
    const raw = window.localStorage.getItem(VALIDATION_STATE_KEY)
    if (!raw) return {}
    const parsed = JSON.parse(raw)
    return {
      jobId: typeof parsed?.jobId === 'string' ? parsed.jobId : undefined,
      selectedId: typeof parsed?.selectedId === 'string' ? parsed.selectedId : undefined,
      flow: normalizeFlow(parsed?.flow),
      from: normalizeDirection(parsed?.from),
      updatedAt: typeof parsed?.updatedAt === 'number' ? parsed.updatedAt : undefined,
    }
  } catch {
    return {}
  }
}

function writePersistedValidationState(patch: PersistedValidationState) {
  if (typeof window === 'undefined') return
  try {
    const prev = readPersistedValidationState()
    window.localStorage.setItem(
      VALIDATION_STATE_KEY,
      JSON.stringify({
        ...prev,
        ...patch,
        updatedAt: Date.now(),
      }),
    )
  } catch {
    // Persistence is best-effort; the backend job remains the source of truth.
  }
}

export default function UERANSIMValidationPage({ active = true }: { active?: boolean }) {
  const { addToast } = useStore()
  const persistedInitial = useMemo(() => readPersistedValidationState(), [])
  const [deployments, setDeployments] = useState<any[]>([])
  const [selectedId, setSelectedId] = useState(() => persistedInitial.selectedId ?? '')
  const [loading, setLoading] = useState(false)
  const [job, setJob] = useState<any>(null)
  const [message, setMessage] = useState('')
  const [from, setFrom] = useState<Direction>(() => persistedInitial.from ?? 'ue1')
  const [flow, setFlow] = useState<ValidationFlow>(() => persistedInitial.flow ?? 'smoke')
  const [sending, setSending] = useState(false)

  const applyJob = (nextJob: any) => {
    setJob(nextJob)
    const jobFlow = normalizeFlow(nextJob?.mode)
    const deploymentId = String(nextJob?.deployment_id ?? nextJob?.backend_deployment_id ?? '')
    if (jobFlow) setFlow(jobFlow)
    if (deploymentId) setSelectedId(deploymentId)
    if (nextJob?.job_id) {
      writePersistedValidationState({
        jobId: String(nextJob.job_id),
        selectedId: deploymentId || selectedId,
        flow: jobFlow ?? flow,
        from,
      })
    }
  }

  const selectFlow = (nextFlow: ValidationFlow) => {
    setFlow(nextFlow)
    if (job?.job_id && !isActiveJobStatus(job.status)) {
      setJob(null)
      writePersistedValidationState({ jobId: '', flow: nextFlow, selectedId, from })
    }
  }

  const selectDeployment = (nextId: string) => {
    setSelectedId(nextId)
    if (job?.job_id && !isActiveJobStatus(job.status)) {
      setJob(null)
      writePersistedValidationState({ jobId: '', selectedId: nextId, flow, from })
    }
  }

  const selected = useMemo(
    () => deployments.find((d) => String(d?.deployment_id ?? d?.backend_deployment_id ?? '') === selectedId),
    [deployments, selectedId],
  )

  const loadDeployments = async () => {
    setLoading(true)
    try {
      const res = await apiClient.getUERANSIMDeployments()
      const items = Array.isArray(res.items) ? res.items : []
      setDeployments(items)
      const hasSelected = Boolean(selectedId) && items.some((d: any) => String(d?.deployment_id ?? d?.backend_deployment_id ?? '') === selectedId)
      if (!hasSelected && !isActiveJobStatus(job?.status)) {
        const first = items.find((d: any) => d?.eligible_for_ueransim) ?? items[0]
        if (first) setSelectedId(String(first?.deployment_id ?? first?.backend_deployment_id ?? ''))
      }
    } catch (e: any) {
      addToast(`功能验证部署列表加载失败: ${e?.message ?? e}`, 'error')
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    if (active) loadDeployments()
  }, [active])

  useEffect(() => {
    const saved = readPersistedValidationState()
    if (!saved.jobId) return
    let cancelled = false
    ;(async () => {
      try {
        const res = await apiClient.getUERANSIMVerification(saved.jobId as string)
        if (!cancelled) applyJob(res.job)
      } catch {
        // Keep the saved selection/flow. A missing in-memory backend job will be replaced by the next start.
      }
    })()
    return () => {
      cancelled = true
    }
  }, [])

  useEffect(() => {
    writePersistedValidationState({ selectedId, flow, from })
  }, [selectedId, flow, from])

  useEffect(() => {
    if (!job?.job_id || !isActiveJobStatus(job.status)) return
    const timer = window.setInterval(async () => {
      try {
        const res = await apiClient.getUERANSIMVerification(job.job_id)
        applyJob(res.job)
      } catch {
        // keep the last visible state
      }
    }, 1600)
    return () => window.clearInterval(timer)
  }, [job?.job_id, job?.status])

  const start = async () => {
    if (!selectedId) {
      addToast('请先选择一个核心网部署', 'warning')
      return
    }
    setLoading(true)
    try {
      const res = await apiClient.startUERANSIMVerification({ deployment_id: selectedId, mode: flow })
      applyJob(res.job)
      addToast(flow === 'reschedule' ? '已启动重调度恢复验证' : '已启动 UERANSIM 功能验证', 'success')
    } catch (e: any) {
      addToast(`启动失败: ${e?.response?.data?.message ?? e?.message ?? e}`, 'error')
    } finally {
      setLoading(false)
    }
  }

  const stop = async () => {
    if (!job?.job_id) return
    setLoading(true)
    try {
      const res = await apiClient.stopUERANSIMVerification(job.job_id)
      applyJob(res.job)
      addToast('已停止验证容器', 'success')
    } catch (e: any) {
      addToast(`停止失败: ${e?.message ?? e}`, 'error')
    } finally {
      setLoading(false)
    }
  }

  const send = async () => {
    const text = message.trim()
    if (!job?.job_id || !text) return
    const to: Direction = from === 'ue1' ? 'ue2' : 'ue1'
    setSending(true)
    try {
      const res = await apiClient.sendUERANSIMMessage(job.job_id, { from, to, message: text })
      applyJob(res.job)
      setMessage('')
      addToast(res.transport === 'udp_over_ue_tunnel' ? '消息已通过 UE 隧道发送' : '消息已写入对端面板', 'success')
    } catch (e: any) {
      addToast(`消息发送失败: ${e?.response?.data?.message ?? e?.message ?? e}`, 'error')
    } finally {
      setSending(false)
    }
  }

  const containers = Array.isArray(selected?.runtime_containers) ? selected.runtime_containers : []
  const jobRuntimeContainers = Array.isArray(job?.containers) ? job.containers : []
  const jobContainers = jobRuntimeContainers.length > 0 ? jobRuntimeContainers : containers
  const ues = Array.isArray(job?.ues) ? job.ues : []
  const status = String(job?.status ?? 'idle')
  const activeMode = String(job?.mode ?? flow) as ValidationFlow
  const canSend = activeMode === 'smoke' && status === 'success' && ues.length >= 2
  const selectedLabel = selected ? deploymentLabel(selected, deployments) : ''
  const isRunning = status === 'running' || status === 'queued'
  const report = job?.reschedule_report && typeof job.reschedule_report === 'object' ? job.reschedule_report : null
  const diff = report?.diff ?? null
  const movedNfs = Array.isArray(diff?.nf_moves) ? diff.nf_moves : []
  const stoppedNfs = Array.isArray(diff?.stopped_nfs) ? diff.stopped_nfs : []
  const showReschedule = activeMode === 'reschedule' || flow === 'reschedule'

  return (
    <div className="h-full grid grid-cols-12 gap-2.5 overflow-hidden">
      <div
        className="col-span-12 xl:col-span-4 h-full p-3.5 rounded-2xl overflow-hidden flex flex-col"
        style={{
          background: 'linear-gradient(160deg, rgba(11,17,30,0.78), rgba(8,13,24,0.66))',
          border: '1px solid rgba(112,168,208,0.28)',
          backdropFilter: 'blur(14px)',
        }}
      >
        <div className="flex items-start justify-between gap-2 mb-3">
          <div>
            <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold inline-flex items-center gap-1.5">
              <RadioTower className="w-4 h-4 text-cyan-300" />
              功能验证
            </div>
            <div className="text-[10px] text-slate-400 mt-0.5">选择服务就绪核心网，执行 UE 功能或重调度恢复验证</div>
          </div>
          <button
            onClick={loadDeployments}
            disabled={loading}
            className="h-7 w-7 rounded-lg inline-flex items-center justify-center text-cyan-100 border border-cyan-500/30 bg-cyan-500/10 disabled:opacity-50"
            title="刷新核心网"
          >
            <RefreshCw className={`w-3.5 h-3.5 ${loading ? 'animate-spin' : ''}`} />
          </button>
        </div>

        <div className="space-y-2">
          <div>
            <label className="block text-[10px] text-slate-400 mb-1.5">验证流程</label>
            <div className="grid grid-cols-2 gap-1.5 rounded-lg border border-slate-700/80 bg-slate-950/35 p-1">
              <button
                onClick={() => selectFlow('smoke')}
                disabled={isRunning}
                className={`h-8 rounded-md text-[11px] font-semibold inline-flex items-center justify-center gap-1.5 ${
                  flow === 'smoke' ? 'text-cyan-100 bg-cyan-500/20 border border-cyan-400/30' : 'text-slate-300 border border-transparent'
                } disabled:opacity-50`}
              >
                <RadioTower className="w-3.5 h-3.5" />
                双 UE 功能
              </button>
              <button
                onClick={() => selectFlow('reschedule')}
                disabled={isRunning}
                className={`h-8 rounded-md text-[11px] font-semibold inline-flex items-center justify-center gap-1.5 ${
                  flow === 'reschedule' ? 'text-cyan-100 bg-cyan-500/20 border border-cyan-400/30' : 'text-slate-300 border border-transparent'
                } disabled:opacity-50`}
              >
                <GitCompare className="w-3.5 h-3.5" />
                重调度恢复
              </button>
            </div>
          </div>

          <label className="block text-[10px] text-slate-400">核心网部署</label>
          <select
            value={selectedId}
            onChange={(e) => selectDeployment(e.target.value)}
            className="w-full h-9 rounded-lg bg-slate-950/70 border border-slate-700/80 text-slate-100 text-[12px] px-2 outline-none"
          >
            {deployments.map((dep) => {
              const id = String(dep?.deployment_id ?? dep?.backend_deployment_id ?? '')
              const label = deploymentLabel(dep, deployments)
              return (
                <option key={id} value={id}>
                  {label} · {id.slice(0, 10)} {dep?.eligible_for_ueransim ? ' · UE可验证' : ' · 未就绪'}
                </option>
              )
            })}
          </select>

          {selected && (
            <div className="rounded-lg border border-cyan-400/20 bg-cyan-500/10 px-2 py-1.5 text-[10px] text-cyan-100">
              当前验证对象: <span className="font-semibold">{selectedLabel}</span>
              <span className="text-cyan-200/60 ml-1 font-mono">{selectedId.slice(0, 18)}</span>
            </div>
          )}

          {selected && (
            <div className="grid grid-cols-2 gap-1.5 text-[10px]">
              <div className={`rounded-lg px-2 py-1.5 border ${pillClass(Boolean(selected.service_ready))}`}>
                服务状态: {selected.service_ready ? '就绪' : '未就绪'}
              </div>
              <div className={`rounded-lg px-2 py-1.5 border ${pillClass(Boolean(selected.ready_for_ueransim))}`}>
                UE条件: {selected.ready_for_ueransim ? '满足' : '缺失'}
              </div>
              <div className="rounded-lg px-2 py-1.5 border border-slate-700/70 bg-slate-950/30 text-slate-300">
                容器: {Number(selected.containers_running ?? 0)}/{Number(selected.containers_total ?? 0)}
              </div>
              <div className="rounded-lg px-2 py-1.5 border border-slate-700/70 bg-slate-950/30 text-slate-300">
                NF: {Number(selected.core_nfs_running ?? 0)}/{Number(selected.core_nfs_total ?? 0)}
              </div>
            </div>
          )}

          <div className="flex items-center gap-2">
            <button
              onClick={start}
              disabled={loading || !selected?.eligible_for_ueransim || isRunning}
              className="h-9 flex-1 rounded-lg bg-cyan-500/15 border border-cyan-400/30 text-cyan-100 text-[12px] font-semibold inline-flex items-center justify-center gap-1.5 disabled:opacity-45"
            >
              <Activity className="w-3.5 h-3.5" />
              {flow === 'reschedule' ? '启动重调度验证' : '启动验证'}
            </button>
            <button
              onClick={stop}
              disabled={loading || !job?.job_id}
              className="h-9 px-3 rounded-lg bg-rose-500/18 border border-rose-400/35 text-rose-100 text-[12px] font-semibold inline-flex items-center gap-1.5 disabled:opacity-45"
              title="停止当前验证并删除本次启动的 gNB/UE 容器"
            >
              <Square className="w-3.5 h-3.5" />
              停止验证
            </button>
          </div>
        </div>

        <div className="mt-3 min-h-0 flex-1 overflow-auto rounded-xl border border-slate-700/60 bg-slate-950/25">
          <table className="w-full text-[10px]">
            <thead className="sticky top-0 bg-slate-900/95 text-slate-300">
              <tr>
                <th className="px-2 py-2 text-left">NF</th>
                <th className="px-2 py-2 text-left">节点</th>
                <th className="px-2 py-2 text-left">容器</th>
                <th className="px-2 py-2 text-left">进程</th>
              </tr>
            </thead>
            <tbody>
              {jobContainers.map((c: any, idx: number) => (
                <tr key={`${c?.nf_type}-${idx}`} className="border-t border-slate-800/80 text-slate-200">
                  <td className="px-2 py-2 font-mono text-cyan-100">{String(c?.nf_type ?? '-').toUpperCase()}</td>
                  <td className="px-2 py-2">{String(c?.node ?? '-')}</td>
                  <td className="px-2 py-2 font-mono max-w-[150px] truncate" title={String(c?.container ?? '')}>
                    {String(c?.container ?? '-')}
                  </td>
                  <td className="px-2 py-2">
                    <span className={c?.daemon_running ? 'text-emerald-300' : 'text-amber-300'}>
                      {c?.daemon_running ? '运行' : '待核验'}
                    </span>
                  </td>
                </tr>
              ))}
              {jobContainers.length === 0 && (
                <tr>
                  <td colSpan={4} className="px-3 py-8 text-center text-slate-500">暂无核心网容器信息</td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </div>

      <div className="col-span-12 xl:col-span-8 h-full min-h-0 grid grid-rows-[176px_minmax(0,1fr)] gap-2.5 overflow-hidden">
        <div
          className="rounded-2xl p-2.5 overflow-hidden"
          style={{
            background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
            border: '1px solid rgba(112,168,208,0.28)',
            backdropFilter: 'blur(14px)',
          }}
        >
          <div className="flex items-center gap-2 mb-2">
            <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold">验证流水线</div>
            <div className="ml-auto text-[10px] text-slate-400">
              {job?.job_id ? `${job.job_id} · ${fmtTime(job.started_at)}` : '等待启动'}
            </div>
            <button
              onClick={stop}
              disabled={loading || !job?.job_id}
              className="h-7 px-2.5 rounded-lg bg-rose-500/15 border border-rose-400/30 text-rose-100 text-[10px] font-semibold inline-flex items-center gap-1 disabled:opacity-45"
              title="停止当前验证并删除本次启动的所有 UE 容器"
            >
              <Square className="w-3 h-3" />
              停止验证
            </button>
          </div>
          <div className="h-1.5 rounded-full bg-slate-800 overflow-hidden mb-2">
            <div className="h-full bg-cyan-400 transition-all" style={{ width: `${Number(job?.progress ?? 0)}%` }} />
          </div>
          <div className="grid grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-1.5 max-h-[118px] overflow-y-auto pr-1">
            {(Array.isArray(job?.steps) ? job.steps : []).map((s: any) => (
              <div key={String(s?.key)} className="rounded-md border border-slate-700/70 bg-slate-950/25 px-2 py-1.5 min-h-[50px]">
                <div className="flex items-center gap-1.5 text-[10px] text-slate-100">
                  {stepIcon(String(s?.status ?? 'pending'))}
                  <span>{String(s?.label ?? '-')}</span>
                </div>
                <div className="text-[8.5px] text-slate-400 mt-0.5 leading-3 break-words">{String(s?.detail ?? '')}</div>
              </div>
            ))}
            {!job?.steps && (
              <div className="col-span-full text-[11px] text-slate-500 py-3">启动验证后将显示 UE 注册、PDU Session、业务面和消息面板状态。</div>
            )}
          </div>
        </div>

        <div className="min-h-0 grid grid-cols-12 gap-2.5 overflow-hidden">
          <div
            className="col-span-12 lg:col-span-8 rounded-2xl p-3.5 min-h-0 flex flex-col overflow-hidden"
            style={{
              background: 'linear-gradient(160deg, rgba(11,17,30,0.78), rgba(8,13,24,0.66))',
              border: '1px solid rgba(112,168,208,0.28)',
              backdropFilter: 'blur(14px)',
            }}
          >
            <div className="flex items-center gap-2 mb-2">
              <Terminal className="w-4 h-4 text-cyan-300" />
              <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold">UE 日志与通信</div>
              <div className={`ml-auto text-[10px] px-2 py-1 rounded border ${job?.data_plane_verified ? pillClass(true) : 'text-slate-300 bg-slate-800/40 border-slate-600/50'}`}>
                {job?.data_plane_verified ? '业务面已验证' : '等待业务面验证'}
              </div>
            </div>

            <div className="grid grid-cols-2 gap-2 mb-2">
              {ues.map((ue: any) => (
                <div key={String(ue?.key)} className="rounded-xl border border-slate-700/70 bg-slate-950/30 p-2 min-w-0">
                  <div className="flex items-center gap-1.5 text-[11px] text-slate-100 mb-1">
                    <RadioTower className="w-3.5 h-3.5 text-cyan-300" />
                    {String(ue?.label ?? ue?.key)} · {String(ue?.ip || '待分配')}
                  </div>
                  <div className="text-[9px] text-slate-400 font-mono truncate" title={String(ue?.container ?? '')}>{String(ue?.container ?? '-')}</div>
                  <div className="mt-2 flex gap-1.5 text-[9px]">
                    <span className={`px-1.5 py-0.5 rounded border ${pillClass(Boolean(ue?.registered))}`}>注册 {ue?.registered ? '成功' : '等待'}</span>
                    <span className={`px-1.5 py-0.5 rounded border ${pillClass(Boolean(ue?.pdu_session))}`}>PDU {ue?.pdu_session ? '成功' : '等待'}</span>
                  </div>
                </div>
              ))}
              {ues.length === 0 && <div className="col-span-2 text-[11px] text-slate-500 py-6 text-center">UE 容器启动后显示终端状态</div>}
            </div>

            {activeMode === 'smoke' ? (
              <div className="flex items-center gap-2 mb-2">
                <select
                  value={from}
                  onChange={(e) => setFrom(e.target.value as Direction)}
                  className="h-8 rounded-lg bg-slate-950/70 border border-slate-700/80 text-slate-100 text-[11px] px-2 outline-none"
                  disabled={!canSend}
                >
                  <option value="ue1">UE-1 → UE-2</option>
                  <option value="ue2">UE-2 → UE-1</option>
                </select>
                <input
                  value={message}
                  onChange={(e) => setMessage(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') send()
                  }}
                  disabled={!canSend}
                  maxLength={512}
                  className="h-8 min-w-0 flex-1 rounded-lg bg-slate-950/70 border border-slate-700/80 text-slate-100 text-[11px] px-2 outline-none disabled:opacity-50"
                  placeholder={canSend ? '输入要发送到另一个 UE 的消息' : '验证成功后可发送 UE 间消息'}
                />
                <button
                  onClick={send}
                  disabled={!canSend || sending || !message.trim()}
                  className="h-8 px-3 rounded-lg bg-cyan-500/15 border border-cyan-400/30 text-cyan-100 text-[11px] inline-flex items-center gap-1.5 disabled:opacity-45"
                >
                  <Send className="w-3.5 h-3.5" />
                  发送
                </button>
              </div>
            ) : (
              <div className="mb-2 rounded-lg border border-slate-700/70 bg-slate-950/30 px-2 py-2 text-[10px] text-slate-400">
                重调度恢复流程使用单 UE 验证注册、PDU Session 和恢复后重新接入，双 UE 消息面板仅在“双 UE 功能”流程中启用。
              </div>
            )}

            <div className="min-h-0 flex-1 grid grid-cols-2 gap-2 overflow-hidden">
              {ues.map((ue: any) => (
                <div key={`logs-${ue?.key}`} className="min-h-0 rounded-xl border border-slate-700/70 bg-black/35 overflow-hidden flex flex-col">
                  <div className="px-2 py-1.5 text-[10px] text-cyan-100 border-b border-slate-800/80">
                    {String(ue?.label ?? ue?.key)} 关键日志 / 接收消息
                  </div>
                  <pre className="min-h-0 flex-1 overflow-auto p-2 text-[9px] leading-4 text-slate-300 whitespace-pre-wrap font-mono">
{`${String(ue?.received_messages ?? '').trim()}\n\n--- UE LOG ---\n${String(ue?.log_tail ?? '').trim()}`.trim() || '等待日志...'}
                  </pre>
                </div>
              ))}
            </div>
          </div>

          <div
            className="col-span-12 lg:col-span-4 rounded-2xl p-3.5 min-h-0 flex flex-col overflow-hidden"
            style={{
              background: 'linear-gradient(160deg, rgba(9,18,31,0.76), rgba(6,13,24,0.66))',
              border: '1px solid rgba(112,168,208,0.28)',
              backdropFilter: 'blur(14px)',
            }}
          >
            <div className="flex items-center gap-2 mb-2">
              <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold">验证日志</div>
              {showReschedule && (
                <span className="ml-auto text-[10px] px-2 py-0.5 rounded border border-cyan-400/25 bg-cyan-500/10 text-cyan-100">重调度流程</span>
              )}
            </div>
            {showReschedule && report?.state === 'waiting_manual_fault' && (
              <div className="mb-2 rounded-lg border border-amber-400/35 bg-amber-500/12 px-2 py-2 text-[10px] text-amber-100 flex gap-2">
                <AlertTriangle className="w-3.5 h-3.5 text-amber-300 mt-0.5 flex-none" />
                <div>基线单 UE 验证已通过。请在“故障控制”页手动注入卫星或链路故障，本页面会自动检测原核心网停服与重调度恢复。</div>
              </div>
            )}
            <div className="grid grid-cols-2 gap-2 text-[10px] mb-2">
              <div className="rounded-lg border border-slate-700/70 bg-slate-950/30 px-2 py-1.5 text-slate-300">状态: {status}</div>
              <div className="rounded-lg border border-slate-700/70 bg-slate-950/30 px-2 py-1.5 text-slate-300">进度: {Number(job?.progress ?? 0)}%</div>
              <div className="rounded-lg border border-slate-700/70 bg-slate-950/30 px-2 py-1.5 text-slate-300">AMF: {String(job?.amf_ip ?? '-')}</div>
              <div className="rounded-lg border border-slate-700/70 bg-slate-950/30 px-2 py-1.5 text-slate-300">gNB: {String(job?.gnb_ip ?? '-')}</div>
            </div>
            {showReschedule && report && (
              <div className="mb-2 rounded-xl border border-slate-700/70 bg-slate-950/30 p-2 text-[10px] text-slate-300 max-h-[190px] overflow-auto">
                <div className="flex items-center gap-1.5 text-cyan-100 font-semibold mb-1.5">
                  <GitCompare className="w-3.5 h-3.5" />
                  恢复对比
                </div>
                <div className="grid grid-cols-2 gap-1.5 mb-2">
                  <div className="rounded-md bg-slate-900/70 border border-slate-700/70 px-2 py-1">
                    原部署: {Number(report?.baseline?.core_nfs_running ?? 0)}/{Number(report?.baseline?.core_nfs_total ?? 0)} NF
                  </div>
                  <div className="rounded-md bg-slate-900/70 border border-slate-700/70 px-2 py-1">
                    恢复后: {Number(report?.recovered?.core_nfs_running ?? 0)}/{Number(report?.recovered?.core_nfs_total ?? 0)} NF
                  </div>
                  <div className="rounded-md bg-slate-900/70 border border-slate-700/70 px-2 py-1">
                    恢复耗时: {report?.recovery_time_ms != null ? `${Number(report.recovery_time_ms)} ms` : '-'}
                  </div>
                  <div className="rounded-md bg-slate-900/70 border border-slate-700/70 px-2 py-1">
                    范围: {diff?.scope === 'overall' ? '整体重调度' : diff?.scope === 'partial' ? '局部重调度' : '-'}
                  </div>
                </div>
                {stoppedNfs.length > 0 && (
                  <div className="mb-2">
                    <div className="text-amber-200 mb-1">故障时停止网元</div>
                    <div className="space-y-1">
                      {stoppedNfs.slice(0, 6).map((nf: any, idx: number) => (
                        <div key={`stopped-${idx}`} className="font-mono text-[9px] text-slate-400">
                          {String(nf?.nf_type ?? '-').toUpperCase()} @ {String(nf?.node ?? '-')} · {String(nf?.container ?? '-')}
                        </div>
                      ))}
                    </div>
                  </div>
                )}
                {movedNfs.length > 0 && (
                  <div>
                    <div className="text-cyan-200 mb-1">网元重放置</div>
                    <div className="space-y-1">
                      {movedNfs.slice(0, 10).map((nf: any, idx: number) => (
                        <div key={`move-${idx}`} className={String(nf?.status) === 'moved' ? 'text-emerald-200' : 'text-slate-500'}>
                          <span className="font-mono">{String(nf?.nf_type ?? '-').toUpperCase()}</span>
                          <span className="text-slate-500">: </span>
                          <span className="font-mono">{String(nf?.from_node ?? '-')}</span>
                          <span className="text-slate-500"> → </span>
                          <span className="font-mono">{String(nf?.to_node ?? '-')}</span>
                        </div>
                      ))}
                    </div>
                  </div>
                )}
              </div>
            )}
            <pre className="min-h-0 flex-1 overflow-auto rounded-xl border border-slate-700/70 bg-black/35 p-2 text-[9px] leading-4 text-slate-300 whitespace-pre-wrap font-mono">
{Array.isArray(job?.logs) && job.logs.length > 0 ? job.logs.join('\n') : '等待启动验证...'}
            </pre>
          </div>
        </div>
      </div>
    </div>
  )
}
