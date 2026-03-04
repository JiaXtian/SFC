import { X, Network, Gauge, Timer, Activity } from 'lucide-react'
import { useStore } from '@/store/useStore'

const Bar = ({ val = 0, max = 0 }: { val?: number; max?: number }) => {
  const safeVal = Number(val ?? 0)
  const safeMax = Number(max ?? 0)
  const pct = safeMax > 0 ? Math.min(100, (safeVal / safeMax) * 100) : 0
  const color = pct > 80 ? '#f87171' : pct > 50 ? '#fbbf24' : '#00ff88'

  return (
    <div className="flex items-center gap-2.5">
      <div className="flex-1 h-1.5 rounded-full overflow-hidden" style={{ background: 'rgba(50,50,60,0.4)' }}>
        <div className="h-full rounded-full transition-all" style={{ width: `${pct}%`, background: color }} />
      </div>
      <span className="text-[10px] font-mono w-9 text-right font-semibold" style={{ color }}>
        {pct.toFixed(0)}%
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

  const statusLabel = status === 'down' ? '故障' : status === 'congested' ? '拥塞' : '正常'
  const statusColor = status === 'down' ? '#ef4444' : status === 'congested' ? '#f59e0b' : '#22c55e'

  return (
    <div
      className="absolute right-[348px] top-[420px] z-10 rounded-2xl overflow-hidden shadow-2xl"
      style={{
        width: 260,
        background: 'linear-gradient(180deg, rgba(20,20,35,0.97), rgba(10,10,20,0.97))',
        border: '1px solid rgba(34,211,238,0.35)',
        backdropFilter: 'blur(20px)'
      }}
    >
      <div
        className="relative px-4 py-3"
        style={{
          background: 'linear-gradient(135deg, rgba(34,211,238,0.15), rgba(14,116,144,0.12))',
          borderBottom: '1px solid rgba(100,100,120,0.15)'
        }}
      >
        <button
          onClick={() => setSelectedLink(null)}
          className="absolute right-2 top-2 p-1 rounded-lg hover:bg-white/10 transition"
        >
          <X className="w-3.5 h-3.5 text-gray-500" />
        </button>

        <div className="text-xs font-bold text-cyan-300 font-mono mb-1">
          {selectedLink.source} ↔ {selectedLink.target}
        </div>

        <div className="flex items-center gap-2 text-[10px]">
          <div className="w-2 h-2 rounded-full" style={{ background: statusColor }} />
          <span className="font-medium" style={{ color: statusColor }}>{statusLabel}</span>
        </div>
      </div>

      <div className="px-4 py-3 space-y-3">
        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-2 font-semibold">
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
            <Bar val={avail} max={total} />
          </div>
        </div>

        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-2 font-semibold">
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
          </div>
        </div>

        <div className="text-[10px] text-gray-500 flex items-center gap-1">
          <Timer className="w-3 h-3" />点击其他链路可切换查看
        </div>
      </div>
    </div>
  )
}
