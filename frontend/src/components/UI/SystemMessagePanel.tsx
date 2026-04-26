import { useMemo, useState } from 'react'
import { Bell, ChevronLeft, ChevronRight } from 'lucide-react'
import { useStore } from '@/store/useStore'
import { toChineseFailureText } from '@/utils/failureText'
import { buildSfcLabelMaps, formatSfcSeq, resolveSfcLabel as resolveSfcSeqLabel } from '@/utils/sfcLabel'

function extractSfcLabel(raw: string): string {
  const m = /\bSFC-(\d{1,})\b/i.exec(String(raw ?? ''))
  if (!m) return ''
  const seq = Number(m[1])
  if (!Number.isFinite(seq) || seq <= 0) return ''
  return formatSfcSeq(seq)
}

function faultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    power_failure: '供电故障',
    cpu_overload: '计算过载',
    thermal_shutdown: '过热停机',
    control_plane_sync_loss: '控制面失步',
    software_crash: '软件崩溃',
    clock_drift: '时钟漂移',
    endpoint_node_fault: '端点节点故障',
    line_of_sight_loss: '视距中断',
  }
  return map[tag] ?? tag
}

function triggerLabel(trigger: string): string {
  switch (trigger) {
    case 'source_node_down': return '源节点故障'
    case 'destination_node_down': return '宿节点故障'
    case 'deployment_node_down': return '部署节点故障'
    case 'anchor_path_disconnected': return '业务路径中断'
    case 'recovery_resume': return '故障恢复后继续编排'
    case 'topology_tick_bootstrap': return '系统拓扑初始化'
    case 'session_start': return '会话启动'
    case 'manual_initial_candidate': return '手动触发初始方案'
    case 'manual': return '人工触发重算'
    case 'periodic_health_check': return '周期健康检查'
    case 'resource_or_link_fault': return '资源或链路异常'
    case 'unlabeled': return '状态变化触发'
    default: return '策略调整'
  }
}

function phaseLabel(raw: string): string {
  const v = String(raw ?? '').trim().toLowerCase()
  const map: Record<string, string> = {
    queued: '排队中',
    pending: '等待中',
    preparing: '准备中',
    starting: '启动中',
    starting_containers: '容器启动中',
    starting_core_nfs: '网元启动中',
    health_check: '健康检查中',
    running: '运行中',
    ready: '已就绪',
    degraded: '降级运行',
    rollback: '回滚中',
    rolled_back: '已回滚',
    failed: '失败',
  }
  return map[v] ?? (raw || '未知')
}

function deploymentStatusLabel(raw: string): string {
  const v = String(raw ?? '').trim().toLowerCase()
  const map: Record<string, string> = {
    completed: '已部署',
    in_progress: '部署中',
    'in-progress': '部署中',
    running: '运行中',
    degraded: '降级运行',
    rolled_back: '已回滚',
    rollback_failed: '回滚失败',
    failed: '失败',
  }
  return map[v] ?? (raw || '未知')
}

function short(text: string, max = 52) {
  if (!text) return ''
  return text.length <= max ? text : `${text.slice(0, max)}...`
}

function replaceDeployIdsWithLabel(
  input: string,
  resolveSfcLabel: (ids?: { sessionId?: string; requestId?: string; deploymentId?: string }) => string,
): string {
  if (!input) return ''
  return input.replace(/\bdeploy_[A-Za-z0-9_-]+\b/g, (match) => resolveSfcLabel({ deploymentId: match }))
}

function compactTime(raw: string) {
  const ts = Date.parse(raw)
  if (Number.isNaN(ts)) return raw || '-'
  const d = new Date(ts)
  const hh = String(d.getHours()).padStart(2, '0')
  const mm = String(d.getMinutes()).padStart(2, '0')
  const ss = String(d.getSeconds()).padStart(2, '0')
  return `${hh}:${mm}:${ss}`
}

export default function SystemMessagePanel() {
  const { runtimeEvents, deployments } = useStore((s) => ({
    runtimeEvents: s.runtimeEvents,
    deployments: s.deployments,
  }))
  const [collapsed, setCollapsed] = useState(false)

  const rows = useMemo(() => {
    const baseMaps = buildSfcLabelMaps(deployments as any)
    const bySession = new Map<string, string>(baseMaps.bySession)
    const byRequest = new Map<string, string>(baseMaps.byRequest)
    const byDeployment = new Map<string, string>(baseMaps.byDeployment)
    const usedLabels = new Set<string>()
    ;[...bySession.values(), ...byRequest.values(), ...byDeployment.values()].forEach((x) => usedLabels.add(String(x)))

    const parseSeq = (label: string) => {
      const m = /^SFC-(\d{3,})$/.exec(label.trim())
      return m ? Number(m[1]) : 0
    }
    let nextSeq = Math.max(
      1,
      ...Array.from(usedLabels).map((x) => parseSeq(String(x))).filter((v) => Number.isFinite(v) && v > 0),
    ) + 1

    const allocLabel = () => {
      let label = formatSfcSeq(nextSeq++)
      while (usedLabels.has(label)) {
        label = formatSfcSeq(nextSeq++)
      }
      usedLabels.add(label)
      return label
    }

    // Fill labels for sessions/requests that have appeared in events but not yet materialized in deployments.
    ;[...runtimeEvents].reverse().forEach((e) => {
      const raw: any = e.raw ?? {}
      const sid = String(raw.session_id ?? (raw.entity_type === 'session' ? raw.entity_id : '') ?? '')
      const rid = String(raw.request_id ?? '')
      const did = String(raw.deployment_id ?? '')
      let label = ''
      if (did && byDeployment.has(did)) label = String(byDeployment.get(did))
      else if (sid && bySession.has(sid)) label = String(bySession.get(sid))
      else if (rid && byRequest.has(rid)) label = String(byRequest.get(rid))
      else if (sid || rid || did) label = allocLabel()
      if (!label) return
      if (did && !byDeployment.has(did)) byDeployment.set(did, label)
      if (sid && !bySession.has(sid)) bySession.set(sid, label)
      if (rid && !byRequest.has(rid)) byRequest.set(rid, label)
    })

    const resolveSfcLabel = (ids?: { sessionId?: string; requestId?: string; deploymentId?: string; sfcName?: string }) => {
      const fromName = extractSfcLabel(String(ids?.sfcName ?? ''))
      if (fromName) return fromName
      const sid = String(ids?.sessionId ?? '')
      const rid = String(ids?.requestId ?? '')
      const did = String(ids?.deploymentId ?? '')
      if (did && byDeployment.has(did)) return String(byDeployment.get(did))
      if (sid && bySession.has(sid)) return String(bySession.get(sid))
      if (rid && byRequest.has(rid)) return String(byRequest.get(rid))
      return resolveSfcSeqLabel(deployments as any, { sessionId: sid, requestId: rid, deploymentId: did })
    }

    const mapped = runtimeEvents
      .map((e) => {
        const time = compactTime(String(e.sim_time ?? ''))
        if (e.type === 'fault_event') {
          const nodeId = String(e.raw?.entity_id ?? '')
          const entityType = String(e.raw?.entity_type ?? 'node')
          const fault = faultTypeLabel(String(e.raw?.fault_type ?? e.raw?.reason ?? 'unknown'))
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: entityType === 'link' ? '链路故障注入' : '节点故障注入',
            text: short(`${nodeId} 出现${fault}`),
          }
        }

        if (e.type === 'fault_update_event') {
          const nodeId = String(e.raw?.entity_id ?? '')
          const fault = faultTypeLabel(String(e.raw?.fault_type ?? 'unknown'))
          const remainSec = Number(e.raw?.remaining_sec ?? 0)
          const remain = Number.isFinite(remainSec) && remainSec > 0 ? `${remainSec.toFixed(0)}s` : '已更新'
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '故障时长调整',
            text: short(`${nodeId}（${fault}）持续时间已更新，剩余 ${remain}`),
          }
        }
        if (e.type === 'reschedule_trigger') {
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel({ sessionId: sid, requestId: rid, sfcName: String((e.raw as any)?.sfc_name ?? '') })
          const trig = triggerLabel(String((e.raw as any)?.trigger ?? ''))
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: '触发重调度',
            text: short(`${sfcLabel} 因${trig}进入重调度流程`),
          }
        }

        if (e.type === 'recovery_event' && String(e.raw?.entity_type ?? '') === 'session') {
          const sid = String(e.raw?.entity_id ?? '')
          const sfcLabel = resolveSfcLabel({ sessionId: sid, requestId: String(e.raw?.request_id ?? ''), sfcName: String(e.raw?.sfc_name ?? '') })
          const trig = triggerLabel(String(e.raw?.trigger ?? ''))
          const ok = Boolean(e.raw?.success)
          return {
            id: e.id,
            time,
            tone: ok ? 'ok' : 'warn',
            title: ok ? '重调度完成' : '重调度失败',
            text: ok
              ? short(`${sfcLabel} 已恢复，触发原因：${trig}`)
              : short(`${sfcLabel} 恢复失败，请检查资源与链路状态`),
          }
        }

        if (e.type === 'recovery_event' && String(e.raw?.entity_type ?? '') === 'node') {
          const nodeId = String(e.raw?.entity_id ?? '')
          return {
            id: e.id,
            time,
            tone: 'ok',
            title: '节点恢复',
            text: short(`${nodeId} 已恢复正常运行`),
          }
        }

        if (e.type === 'decision_trace') {
          const trig = String((e.raw as any)?.trigger ?? '')
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel({ sessionId: sid, requestId: rid, sfcName: String((e.raw as any)?.sfc_name ?? '') })
          const deployable = Number((e.raw as any)?.deployable_count ?? 0)
          const returned = Number((e.raw as any)?.returned_topk ?? 0)
          const isBootstrap = ['session_start', 'topology_tick_bootstrap', 'manual_initial_candidate'].includes(trig)
          if (!trig && !sid && !rid) {
            return {
              id: e.id,
              time,
              tone: deployable > 0 ? 'ok' : 'warn',
              title: deployable > 0 ? '策略生成完成' : '策略生成失败',
              text: deployable > 0
                ? short(`已返回 ${returned} 个候选方案，可部署 ${deployable} 个`)
                : short('未返回可部署方案，请检查资源、链路与约束配置'),
            }
          }
          return {
            id: e.id,
            time,
            tone: deployable > 0 ? (isBootstrap ? 'ok' : 'info') : 'warn',
            title: deployable > 0
              ? (isBootstrap ? '初始策略构建完成' : '路径重算完成')
              : (isBootstrap ? '初始策略构建失败' : '路径重算未通过'),
            text: deployable > 0
              ? short(`${sfcLabel} 已生成可部署方案（${deployable}/${returned}）`)
              : short(`${sfcLabel} 未找到满足约束的可部署方案`),
          }
        }

        if (e.type === 'planning_result') {
          const status = String((e.raw as any)?.status ?? '')
          const deployable = Number((e.raw as any)?.deployable_count ?? 0)
          const returned = Number((e.raw as any)?.returned_topk ?? 0)
          if (status === 'success') {
            return {
              id: e.id,
              time,
              tone: 'ok',
              title: '策略规划成功',
              text: short(`可部署方案 ${deployable} 个，返回候选 ${returned} 个`),
            }
          }
          if (status === 'fallback_only') {
            return {
              id: e.id,
              time,
              tone: 'warn',
              title: '策略仅返回回退候选',
              text: short(`未满足全部SLA约束，返回候选 ${returned} 个用于定位分析`),
            }
          }
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: '策略规划失败',
            text: short(String((e.raw as any)?.message ?? e.message)),
          }
        }

        if (e.type === 'session_update') {
          const st = String((e.raw as any)?.status ?? '')
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel({ sessionId: sid, requestId: rid, sfcName: String((e.raw as any)?.sfc_name ?? '') })
          const trig = triggerLabel(String((e.raw as any)?.trigger ?? ''))
          if (st === 'redeployed') {
            return {
              id: e.id,
              time,
              tone: 'ok',
              title: '重部署完成',
              text: short(`${sfcLabel} 已切换到新路径（${trig}）`),
            }
          }
          if (st === 'deployed') {
            return {
              id: e.id,
              time,
              tone: 'ok',
              title: '部署成功',
              text: short(`${sfcLabel} 已完成部署并进入运行态`),
            }
          }
          if (st === 'decision_failed') {
            return {
              id: e.id,
              time,
              tone: 'warn',
              title: '策略计算失败',
              text: short(toChineseFailureText(String((e.raw as any)?.reason ?? '未找到可部署方案'))),
            }
          }
          if (st === 'replanning') {
            return {
              id: e.id,
              time,
              tone: 'warn',
              title: '等待可部署方案',
              text: short(`${sfcLabel} 暂无可部署方案，系统将持续重算（${trig}）`),
            }
          }
          if (st === 'stable') {
            return {
              id: e.id,
              time,
              tone: 'info',
              title: '会话保持稳定',
              text: short(`${sfcLabel} 当前路径保持稳定，无需迁移`),
            }
          }
          return null
        }

        if (e.type === 'path_recompute_trigger') {
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel({ sessionId: sid, requestId: rid, sfcName: String((e.raw as any)?.sfc_name ?? '') })
          const trig = triggerLabel(String((e.raw as any)?.trigger ?? ''))
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: '路径重算触发',
            text: short(`${sfcLabel} 因${trig}启动路径重算`),
          }
        }

        if (e.type === 'deployment_update') {
          const status = String((e.raw as any)?.status ?? '')
          const did = String((e.raw as any)?.deployment_id ?? '')
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel({
            deploymentId: did,
            sessionId: sid,
            requestId: rid,
            sfcName: String((e.raw as any)?.sfc_name ?? ''),
          })
          if (status === 'completed') {
            return {
              id: e.id,
              time,
              tone: 'ok',
              title: '部署状态更新',
              text: short(`${sfcLabel} 已完成部署`),
            }
          }
          if (status === 'rolled_back') {
            return {
              id: e.id,
              time,
              tone: 'warn',
              title: '部署已回滚',
              text: short(`${sfcLabel} 已回滚并释放资源`),
            }
          }
          if (status === 'rollback_failed') {
            return {
              id: e.id,
              time,
              tone: 'warn',
              title: '回滚失败',
              text: short(`${sfcLabel} 回滚失败，请检查资源状态`),
            }
          }
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '部署进度更新',
            text: short(`${sfcLabel} 状态：${deploymentStatusLabel(status)}`),
          }
        }

        if (e.type === 'deployment_runtime_update') {
          const did = String((e.raw as any)?.deployment_id ?? '')
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel({
            deploymentId: did,
            sessionId: sid,
            requestId: rid,
            sfcName: String((e.raw as any)?.sfc_name ?? ''),
          })
          const phase = String((e.raw as any)?.orchestration_phase ?? 'unknown')
          const progress = Number((e.raw as any)?.orchestration_progress ?? 0)
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '部署运行态更新',
            text: short(`${sfcLabel} · ${phaseLabel(phase)} (${progress}%)`),
          }
        }

        if (e.type === 'deployment_action') {
          const detail = replaceDeployIdsWithLabel(String((e.raw as any)?.detail ?? e.message), resolveSfcLabel)
          return {
            id: e.id,
            time,
            tone: String((e.raw as any)?.level ?? 'info'),
            title: String((e.raw as any)?.title ?? '部署动作'),
            text: short(detail),
          }
        }

        if (e.type === 'topology_replaced') {
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: '星座已重建',
            text: short(
              `模板 ${String((e.raw as any)?.constellation_template ?? 'unknown')} 已生效，` +
              `当前 ${String((e.raw as any)?.total_nodes ?? '-') } 节点 / ${String((e.raw as any)?.total_links ?? '-') } 链路`
            ),
          }
        }

        if (e.type === 'satellite_deleted') {
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: '卫星节点删除',
            text: short(`节点 ${String((e.raw as any)?.node_id ?? '-') } 已删除，相关部署已清理`),
          }
        }

        if (e.type === 'ws') {
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '实时通道状态',
            text: short(String(e.message ?? '').replace('实时事件通道已连接', '实时事件通道已连接')),
          }
        }

        if (e.type === 'playback_buffered') {
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '回放模式缓存',
            text: short('当前处于回放模式，实时拓扑更新已进入缓冲'),
          }
        }

        if (e.message) {
          const msg = replaceDeployIdsWithLabel(String(e.message), resolveSfcLabel)
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '系统事件',
            text: short(msg),
          }
        }

        return null
      })
      .filter(Boolean)
      .slice(0, 90) as Array<{ id: string; time: string; tone: string; title: string; text: string }>

    // Deduplicate noisy repeated messages for the same SFC and same action.
    const seen = new Set<string>()
    const deduped: Array<{ id: string; time: string; tone: string; title: string; text: string }> = []
    mapped.forEach((r) => {
      const key = `${r.title}|${r.text}`
      if (seen.has(key)) return
      seen.add(key)
      deduped.push(r)
    })
    return deduped.slice(0, 60)
  }, [runtimeEvents, deployments])

  const toneStyle = (tone: string) => {
    if (tone === 'ok') return { tx: 'text-emerald-200', dot: '#34d399' }
    if (tone === 'warn') return { tx: 'text-amber-200', dot: '#f59e0b' }
    if (tone === 'info') return { tx: 'text-sky-200', dot: '#38bdf8' }
    return { tx: 'text-cyan-100', dot: '#94a3b8' }
  }

  return (
    <div
      className="absolute left-0 z-20 pointer-events-auto flex transition-all duration-300"
      style={{
        top: 'calc(44px * var(--ui-scale, 0))',
        bottom: '0',
        width: collapsed ? 32 : 'clamp(320px, 21vw, 470px)',
      }}
    >
      <button
        onClick={() => setCollapsed((v) => !v)}
        className="absolute -right-3 top-5 w-6 h-6 rounded-full flex items-center justify-center z-20 shadow-lg"
        style={{ background: 'linear-gradient(135deg, #0f2036, #182f45)' }}
        title={collapsed ? '展开系统消息' : '收起系统消息'}
      >
        {collapsed
          ? <ChevronRight className="w-3 h-3 text-gray-300" />
          : <ChevronLeft className="w-3 h-3 text-gray-300" />}
      </button>

      {!collapsed && (
        <div
          className="h-full w-full p-3 flex flex-col rounded-r-2xl"
          style={{
            background: 'transparent',
            borderTop: '1px solid rgba(112,168,208,0.28)',
            borderRight: '1px solid rgba(112,168,208,0.28)',
            borderBottom: '1px solid rgba(112,168,208,0.28)',
            backdropFilter: 'blur(16px)',
          }}
        >
          <div className="text-[13px] uppercase tracking-wide text-cyan-100 font-semibold inline-flex items-center gap-1.5 mb-1.5">
            <Bell className="w-3.5 h-3.5 text-cyan-300" />系统监控消息
          </div>
          <div className="flex-1 overflow-y-auto pr-1">
            {rows.map((r) => {
              const st = toneStyle(r.tone)
              return (
                <div
                  key={r.id}
                  className="px-1.5 py-1.5 text-[11px]"
                  style={{
                    borderBottom: '1px solid rgba(148,163,184,0.18)',
                  }}
                >
                  <div className="flex items-center justify-between gap-2 leading-5">
                    <span className={`${st.tx} font-semibold inline-flex items-center gap-1.5`}>
                      <span className="inline-block w-1.5 h-1.5 rounded-full" style={{ background: st.dot }} />
                      {r.title}
                    </span>
                    <span className="text-slate-500 text-[10px]">{r.time}</span>
                  </div>
                  <div className="text-slate-200 mt-0.5 leading-5">{r.text}</div>
                </div>
              )
            })}
            {rows.length === 0 && <div className="text-[11px] text-slate-500">暂无关键消息</div>}
          </div>
        </div>
      )}
    </div>
  )
}
