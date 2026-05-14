import { X, Network, Gauge, Timer, Activity } from 'lucide-react'
import { useStore } from '@/store/useStore'

function faultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    optical_signal_loss: '光链路信号丢失',
    beam_misalignment: '波束失准',
    interference_jamming: '干扰/压制',
    routing_blackhole: '路由黑洞',
    transceiver_failure: '收发器故障',
    line_degradation: '链路退化',
    endpoint_node_fault: '端点节点故障',
    line_of_sight_loss: '视距中断',
    topology_inconsistent: '拓扑不一致',
    resource_or_link_fault: '资源/链路故障',
  }
  return map[tag] ?? (tag || '无')
}

const Bar = ({ used = 0, total = 0 }: { used?: number; total?: number }) => {
  const safeUsed = Number(used ?? 0)
  const safeTotal = Number(total ?? 0)
  const utilPct = safeTotal > 0 ? Math.min(100, (safeUsed / safeTotal) * 100) : 0
  const color = utilPct >= 85 ? '#f87171' : utilPct >= 60 ? '#fbbf24' : '#00ff88'

  return (
    <div className="flex items-center gap-2.5">
      <div className="flex-1 h-1.5 rounded-full overflow-hidden" style={{ background: 'rgba(50,50,60,0.4)' }}>
        <div className="h-full rounded-full transition-all" style={{ width: `${utilPct}%`, background: color }} />
      </div>
      <span className="text-[10px] font-mono w-9 text-right font-semibold" style={{ color }}>
        {utilPct.toFixed(0)}%
      </span>
    </div>
  )
}

export default function LinkDetailPanel() {
  const { selectedLink, setSelectedLink } = useStore()
  if (!selectedLink) return null

  const total = Number(selectedLink.bandwidth_gbps ?? 0)
  const avail = Number(selectedLink.bandwidth_available_gbps ?? 0)
  const used = Math.max(0, total - avail)
  const status = selectedLink.status ?? 'active'
  const rel = Number(selectedLink.reliability ?? 0.999)
  const utilPct = total > 0 ? (used / total) * 100 : 0
  const faultTag = String((selectedLink as any).fault_tag ?? '')
  const bwHealthLabel = utilPct >= 85 ? '资源紧张' : utilPct >= 60 ? '资源较少' : '资源充足'
  const bwHealthColor = utilPct >= 85 ? '#f87171' : utilPct >= 60 ? '#fbbf24' : '#22c55e'

  const statusLabel = status === 'down' ? '断开' : status === 'congested' ? '拥塞' : '正常'
  const statusColor = status === 'down' ? '#ef4444' : status === 'congested' ? '#f59e0b' : '#22c55e'
  const isFault = status === 'down'

  return (
    <div
      className="absolute top-[calc(68vh+2px)] z-10 rounded-lg overflow-hidden shadow-2xl"
      style={{
        right: 'calc(clamp(320px, 22vw, 470px) + 12px)',
        width: 300,
        maxHeight: 'calc(32vh - 18px)',
        background: isFault
          ? 'linear-gradient(180deg, rgba(35,14,20,0.9), rgba(15,8,13,0.86))'
          : 'linear-gradient(180deg, rgba(13,22,30,0.88), rgba(7,13,20,0.84))',
        border: isFault ? '1px solid rgba(248,113,113,0.48)' : '1px solid rgba(34,211,238,0.35)',
        backdropFilter: 'blur(18px)',
        boxShadow: isFault
          ? '0 18px 42px rgba(0,0,0,0.48), inset 0 1px 0 rgba(248,113,113,0.08)'
          : '0 18px 42px rgba(0,0,0,0.48), inset 0 1px 0 rgba(125,211,252,0.08)',
      }}
    >
      <div
        className="relative px-3 py-2.5"
        style={{
          background: 'linear-gradient(135deg, rgba(34,211,238,0.15), rgba(14,116,144,0.12))',
          ...(isFault ? { background: 'linear-gradient(135deg, rgba(239,68,68,0.18), rgba(153,27,27,0.16))' } : {}),
          borderBottom: '1px solid rgba(100,100,120,0.15)'
        }}
      >
        <button
          onClick={() => setSelectedLink(null)}
          className="absolute right-2 top-2 p-1 rounded-lg hover:bg-white/10 transition"
        >
          <X className="w-3.5 h-3.5 text-gray-500" />
        </button>

        <div className="text-[11px] font-bold text-cyan-300 font-mono mb-1 pr-5 break-all leading-4">
          {selectedLink.source} ↔ {selectedLink.target}
        </div>

        <div className="flex items-center gap-2 text-[10px]">
          <div className="w-2 h-2 rounded-full" style={{ background: statusColor }} />
          <span className="font-medium" style={{ color: statusColor }}>{statusLabel}</span>
        </div>
      </div>

      <div className="px-3 py-2.5 space-y-2 overflow-y-auto" style={{ maxHeight: 'calc(32vh - 86px)', minHeight: 142 }}>
        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-1.5 font-semibold">
            <Network className="w-3 h-3" />链路资源
          </div>

          <div>
            <div className="flex justify-between text-[10px] mb-1">
              <span className="text-gray-500 flex items-center gap-1">
                <Gauge className="w-3 h-3" />可用带宽
              </span>
              <span className="text-gray-300 font-mono">
                {avail.toFixed(2)} / {total.toFixed(2)} Gbps
              </span>
            </div>
            <Bar used={used} total={total} />
            <div className="mt-1 text-[10px] font-medium" style={{ color: bwHealthColor }}>
              带宽状态：{bwHealthLabel}
            </div>
          </div>
        </div>

        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-1.5 font-semibold">
            <Activity className="w-3 h-3" />链路参数
          </div>
          <div className="grid grid-cols-2 gap-x-3 gap-y-1 text-[10px]">
            <div className="flex justify-between">
              <span className="text-gray-600">已用带宽</span>
              <span className="text-gray-300 font-mono">{used.toFixed(2)} Gbps</span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-600">链路类型</span>
              <span className="text-gray-300 font-mono">{selectedLink.link_type}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-600">时延</span>
              <span className="text-gray-300 font-mono">{Number(selectedLink.latency_ms ?? 0).toFixed(2)} ms</span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-600">可靠性</span>
              <span className="text-gray-300 font-mono">{(rel * 100).toFixed(2)}%</span>
            </div>
            <div className="flex justify-between col-span-2">
              <span className="text-gray-600">断开原因</span>
              <span className="text-gray-300 font-mono">
                {status === 'down'
                  ? (faultTag ? faultTypeLabel(faultTag) : '距离不满足或链路暂不可用')
                  : '无'}
              </span>
            </div>
          </div>
        </div>

        <div className="text-[10px] text-gray-500 flex items-center gap-1">
          <Timer className="w-3 h-3" />点击其他链路可切换查看
        </div>
      </div>
    </div>
  )
}
