import { useMemo, useState } from 'react'
import { Bell, ChevronLeft, ChevronRight } from 'lucide-react'
import { useStore } from '@/store/useStore'
import { toChineseFailureText } from '@/utils/failureText'
import { resolveSfcLabel as resolveSfcSeqLabel } from '@/utils/sfcLabel'

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
    default: return trigger || '策略调整'
  }
}

function short(text: string, max = 52) {
  if (!text) return ''
  return text.length <= max ? text : `${text.slice(0, max)}...`
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

  const resolveSfcLabel = (sessionId?: string, requestId?: string) => {
    return resolveSfcSeqLabel(deployments as any, {
      sessionId: String(sessionId ?? ''),
      requestId: String(requestId ?? ''),
    })
  }

  const rows = useMemo(() => {
    const mapped = runtimeEvents
      .map((e) => {
        const time = compactTime(String(e.sim_time ?? ''))
        if (e.type === 'fault_event') {
          const nodeId = String(e.raw?.entity_id ?? '')
          const fault = faultTypeLabel(String(e.raw?.fault_type ?? e.raw?.reason ?? 'unknown'))
          return {
            id: e.id,
            time,
            tone: 'warn',
            title: '节点故障注入',
            text: short(`${nodeId} 出现${fault}`),
          }
        }

        if (e.type === 'fault_update_event') {
          const nodeId = String(e.raw?.entity_id ?? '')
          const fault = faultTypeLabel(String(e.raw?.fault_type ?? 'unknown'))
          const ttl = Number(e.raw?.ttl_ticks ?? 0)
          return {
            id: e.id,
            time,
            tone: 'info',
            title: '故障时长调整',
            text: short(`${nodeId}（${fault}）持续时间已更新，当前TTL=${ttl}`),
          }
        }
        if (e.type === 'reschedule_trigger') {
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel(sid, rid)
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
          const sfcLabel = resolveSfcLabel(sid, String(e.raw?.request_id ?? ''))
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
          if (!trig || ['session_start', 'topology_tick_bootstrap', 'manual_initial_candidate'].includes(trig)) return null
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel(sid, rid)
          const deployable = Number((e.raw as any)?.deployable_count ?? 0)
          const returned = Number((e.raw as any)?.returned_topk ?? 0)
          return {
            id: e.id,
            time,
            tone: deployable > 0 ? 'info' : 'warn',
            title: deployable > 0 ? '路径重算完成' : '路径重算未通过',
            text: deployable > 0
              ? short(`${sfcLabel} 已生成可部署方案（${deployable}/${returned}）`)
              : short(`${sfcLabel} 未找到满足约束的可部署方案`),
          }
        }

        if (e.type === 'session_update') {
          const st = String((e.raw as any)?.status ?? '')
          const sid = String((e.raw as any)?.session_id ?? '')
          const rid = String((e.raw as any)?.request_id ?? '')
          const sfcLabel = resolveSfcLabel(sid, rid)
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
          return null
        }

        if (e.type === 'deployment_update') {
          const depId = String((e.raw as any)?.deployment_id ?? '')
          const pct = Number((e.raw as any)?.progress ?? 0)
          const st = String((e.raw as any)?.status ?? '')
          const sfcLabel = resolveSfcSeqLabel(deployments as any, { deploymentId: depId })
          return {
            id: e.id,
            time,
            tone: st === 'completed' ? 'ok' : 'info',
            title: st === 'completed' ? '部署完成' : '部署进行中',
            text: short(`${sfcLabel} 当前进度 ${pct.toFixed(0)}%`),
          }
        }

        if (e.type === 'deployment_action') {
          return {
            id: e.id,
            time,
            tone: String((e.raw as any)?.level ?? 'info'),
            title: String((e.raw as any)?.title ?? '部署动作'),
            text: short(String((e.raw as any)?.detail ?? e.message)),
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
