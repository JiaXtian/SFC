import { X, Cpu, HardDrive, Navigation, Zap } from 'lucide-react'
import { useStore } from '@/store/useStore'

function nodeFaultTypeLabel(tag: string): string {
  const map: Record<string, string> = {
    power_failure: '供电故障',
    cpu_overload: '计算过载',
    thermal_shutdown: '过热保护停机',
    control_plane_sync_loss: '控制面同步丢失',
    software_crash: '软件崩溃',
    clock_drift: '时钟漂移',
  }
  return map[tag] ?? (tag || '无')
}

function isNodeFault(sat: any): boolean {
  const status = String(sat?.status ?? 'active').toLowerCase()
  const faultTag = String(sat?.fault_tag ?? '').trim()
  return status === 'down' || status === 'fault' || status === 'failed' || faultTag.length > 0
}

/**
 * 安全资源条组件
 */
const Bar = ({ val = 0, max = 0 }: { val?: number; max?: number }) => {
  const safeVal = Number(val ?? 0)
  const safeMax = Number(max ?? 0)

  const pct =
    safeMax > 0 ? Math.min(100, (safeVal / safeMax) * 100) : 0

  const color =
    pct > 80 ? '#f87171' : pct > 50 ? '#fbbf24' : '#00ff88'

  return (
    <div className="flex items-center gap-2.5">
      <div
        className="flex-1 h-1.5 rounded-full overflow-hidden"
        style={{ background: 'rgba(50,50,60,0.4)' }}
      >
        <div
          className="h-full rounded-full transition-all"
          style={{ width: `${pct}%`, background: color }}
        />
      </div>
      <span
        className="text-[10px] font-mono w-9 text-right font-semibold"
        style={{ color }}
      >
        {pct.toFixed(0)}%
      </span>
    </div>
  )
}

export default function SatelliteDetail() {
  const { selectedSatellite, setSelectedSatellite, deployments } = useStore()

  if (!selectedSatellite) return null

  const sat = selectedSatellite
  const satFaultTag = String((sat as any)?.fault_tag ?? '')
  const isFault = isNodeFault(sat)


  const cpuTotal = Number(sat?.cpu_total ?? 0)
  const cpuAvail = Number(sat?.cpu_available ?? 0)
  const memTotal = Number(sat?.mem_total ?? 0)
  const memAvail = Number(sat?.mem_available ?? 0)
  const diskTotal = Number((sat as any)?.disk_total ?? 0)
  const diskAvail = Number((sat as any)?.disk_available ?? 0)

  const cpuUsed = Math.max(0, cpuTotal - cpuAvail)
  const memUsed = Math.max(0, memTotal - memAvail)
  const diskUsed = Math.max(0, diskTotal - diskAvail)

  const orbital: any = sat?.orbital_params ?? {}
  const coords: any = sat?.coordinates ?? {}

  const safeDeployments = deployments ?? []


  const vnfsHere = safeDeployments.flatMap((d: any) =>
    (d?.per_vnf ?? [])
      .filter((v: any) => v?.node === sat?.id)
      .map((v: any) => ({
        ...v,
        dep: d
      }))
  )

  const trafficRoles = safeDeployments
    .map((d: any) => {
      const roles: string[] = []
      if (d?.source_node === sat?.id) roles.push('入口')
      if (d?.destination_node === sat?.id) roles.push('出口')
      if (roles.length === 0) return null
      return {
        deployment_id: d?.deployment_id,
        sfc_name: d?.sfc_name ?? d?.request_id ?? 'unknown',
        roles,
      }
    })
    .filter(Boolean) as Array<{ deployment_id: string; sfc_name: string; roles: string[] }>

  return (
    <div
      className="absolute right-[348px] top-12 z-10 rounded-2xl overflow-hidden shadow-2xl"
      style={{
        width: 260,
        background:
          'linear-gradient(180deg, rgba(20,20,35,0.97), rgba(10,10,20,0.97))',
        border: isFault ? '1px solid rgba(239,68,68,0.45)' : '1px solid rgba(0,255,136,0.3)',
        backdropFilter: 'blur(20px)'
      }}
    >
      {/* Header */}
      <div
        className="relative px-4 py-3"
        style={{
          background:
            isFault
              ? 'linear-gradient(135deg, rgba(239,68,68,0.18), rgba(153,27,27,0.16))'
              : 'linear-gradient(135deg, rgba(0,255,136,0.12), rgba(0,200,100,0.12))',
          borderBottom: '1px solid rgba(100,100,120,0.15)'
        }}
      >
        <button
          onClick={() => setSelectedSatellite(null)}
          className="absolute right-2 top-2 p-1 rounded-lg hover:bg-white/10 transition"
        >
          <X className="w-3.5 h-3.5 text-gray-500" />
        </button>

        <div className={`text-xs font-bold font-mono mb-1 ${isFault ? 'text-rose-300' : 'text-green-300'}`}>
          {sat?.id ?? 'Unknown'}
        </div>

        <div className="flex items-center gap-2 text-[10px]">
          <div
            className={`w-2 h-2 rounded-full ${isFault ? '' : 'animate-pulse'}`}
            style={{ background: isFault ? '#ef4444' : '#22c55e' }}
          />
          <span
            className="font-medium"
            style={{ color: isFault ? '#ef4444' : '#22c55e' }}
          >
            {isFault ? '故障' : '运行正常'}
          </span>
          <span className="text-slate-400">
            · 故障类型 {isFault ? nodeFaultTypeLabel(satFaultTag) : '无'}
          </span>
          {vnfsHere.length > 0 && (
            <span className="text-yellow-400">
              · {vnfsHere.length} 网元
            </span>
          )}
        </div>
      </div>

      {/* Body */}
      <div
        className="px-4 py-3 space-y-3 overflow-y-auto"
        style={{ maxHeight: 'calc(100vh - 220px)' }}
      >
        {/* 轨道参数 */}
        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-2 font-semibold">
            <Navigation className="w-3 h-3" />轨道参数
          </div>

          <div className="grid grid-cols-2 gap-x-3 gap-y-1 text-[10px]">
            {[
              ['轨道面', orbital?.plane ?? '-'],
              ['位置', orbital?.position_in_plane ?? '-'],
              [
                '高度',
                orbital?.altitude_km != null
                  ? `${Number(orbital.altitude_km).toFixed(0)}km`
                  : '-'
              ],
              [
                '倾角',
                (orbital?.inclination ?? orbital?.inclination_deg) != null
                  ? `${Number(orbital?.inclination ?? orbital?.inclination_deg).toFixed(1)}°`
                  : '-'
              ],
              [
                '纬度',
                coords?.lat != null
                  ? `${Number(coords.lat).toFixed(2)}°`
                  : '-'
              ],
              [
                '经度',
                coords?.lon != null
                  ? `${Number(coords.lon).toFixed(2)}°`
                  : '-'
              ]
            ].map(([k, v]) => (
              <div key={String(k)} className="flex justify-between">
                <span className="text-gray-600">{k}</span>
                <span className="text-gray-300 font-mono">{v}</span>
              </div>
            ))}
          </div>
        </div>

        {/* 资源 */}
        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-2 font-semibold">
            <Cpu className="w-3 h-3" />计算资源
          </div>

          <div className="space-y-2">
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500 flex items-center gap-1">
                  <Cpu className="w-3 h-3" />
                  CPU
                </span>
                <span className="text-gray-300 font-mono">
                  {cpuUsed.toFixed(1)} / {cpuTotal.toFixed(1)}
                </span>
              </div>
              <Bar val={cpuUsed} max={cpuTotal} />
            </div>

            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500 flex items-center gap-1">
                  <HardDrive className="w-3 h-3" />
                  内存
                </span>
                <span className="text-gray-300 font-mono">
                  {memUsed.toFixed(1)} / {memTotal.toFixed(1)} GB
                </span>
              </div>
              <Bar val={memUsed} max={memTotal} />
            </div>

            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500 flex items-center gap-1">
                  <HardDrive className="w-3 h-3" />
                  磁盘
                </span>
                <span className="text-gray-300 font-mono">
                  {diskUsed.toFixed(1)} / {diskTotal.toFixed(1)} GB
                </span>
              </div>
              <Bar val={diskUsed} max={diskTotal} />
            </div>
          </div>
        </div>

        {/* Core NF */}
        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-2 font-semibold">
            <Zap className="w-3 h-3" />
            运行中核心网网元 ({vnfsHere.length})
          </div>

          {vnfsHere.length > 0 ? (
            <div className="space-y-1.5">
              {vnfsHere.map((item: any, i: number) => (
                <div
                  key={i}
                  className="px-2.5 py-2 rounded-lg"
                  style={{
                    background:
                      'linear-gradient(135deg, rgba(0,255,136,0.1), rgba(0,200,100,0.1))',
                    border: '1px solid rgba(0,255,136,0.25)'
                  }}
                >
                  <div className="flex items-center justify-between mb-0.5">
                    <span className="text-[11px] font-semibold text-green-300">
                      {item?.core_nf ?? item?.vnf ?? 'Unknown'}
                    </span>

                    <span
                      className={`text-[9px] px-1.5 py-0.5 rounded-full ${
                        item?.dep?.status === 'completed'
                          ? 'text-green-400 bg-green-900/30'
                          : 'text-yellow-400 bg-yellow-900/30'
                      }`}
                    >
                      {item?.dep?.status === 'completed'
                        ? '运行中'
                        : '部署中'}
                    </span>
                  </div>

                  <div className="text-[9px] text-gray-600">
                    SFC:{' '}
                    <span className="text-gray-400">
                      {item?.dep?.sfc_name ?? '-'}
                    </span>
                  </div>

                  <div className="text-[9px] text-gray-600 mt-1 flex gap-3">
                    <span>
                      类型{' '}
                      <span className="text-gray-400 font-mono">
                        {String(item?.nf_type ?? '-')}
                      </span>
                    </span>
                    <span>
                      CPU{' '}
                      <span className="text-gray-400 font-mono">
                        {Number(item?.cpu_used ?? 0).toFixed(2)}
                      </span>
                    </span>
                    <span>
                      MEM{' '}
                      <span className="text-gray-400 font-mono">
                        {Number(item?.mem_used ?? 0).toFixed(1)}GB
                      </span>
                    </span>
                    <span>
                      DISK{' '}
                      <span className="text-gray-400 font-mono">
                        {Number(item?.disk_used ?? 0).toFixed(1)}GB
                      </span>
                    </span>
                  </div>
                </div>
              ))}
            </div>
          ) : (
            <div
              className="text-[10px] text-gray-600 text-center py-2.5 rounded-lg"
              style={{ background: 'rgba(50,50,60,0.2)' }}
            >
              暂无核心网网元部署
            </div>
          )}
        </div>

        <div>
          <div className="flex items-center gap-1.5 text-[10px] text-gray-500 uppercase tracking-wider mb-2 font-semibold">
            <Navigation className="w-3 h-3" />
            流量端点角色 ({trafficRoles.length})
          </div>

          {trafficRoles.length > 0 ? (
            <div className="space-y-1.5">
              {trafficRoles.map((item, i) => (
                <div
                  key={`${item.deployment_id}-${i}`}
                  className="px-2.5 py-2 rounded-lg"
                  style={{ background: 'rgba(59,130,246,0.09)', border: '1px solid rgba(96,165,250,0.25)' }}
                >
                  <div className="flex items-center justify-between">
                    <span className="text-[10px] text-slate-200">{item.sfc_name}</span>
                    <span className="text-[9px] text-cyan-300">{item.roles.join(' / ')}</span>
                  </div>
                </div>
              ))}
            </div>
          ) : (
            <div className="text-[10px] text-gray-600 text-center py-2.5 rounded-lg" style={{ background: 'rgba(50,50,60,0.2)' }}>
              该节点当前未作为任何 SFC 的流量入口/出口
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
