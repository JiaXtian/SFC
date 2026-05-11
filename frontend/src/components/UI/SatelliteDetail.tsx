import { useEffect, useState } from 'react'
import { X, Cpu, HardDrive, Navigation, Zap } from 'lucide-react'
import { apiClient } from '@/api/client'
import { useStore } from '@/store/useStore'
import { resolveSfcLabel } from '@/utils/sfcLabel'

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
  const [runtimeDetail, setRuntimeDetail] = useState<any | null>(null)

  useEffect(() => {
    const satId = String(selectedSatellite?.id ?? '')
    if (!satId) return

    let active = true
    const pull = async () => {
      try {
        const detail = await apiClient.getSatellite(satId)
        if (active) setRuntimeDetail(detail)
      } catch {
        if (active) setRuntimeDetail(null)
      }
    }

    pull()
    const timer = window.setInterval(pull, 4000)
    return () => {
      active = false
      window.clearInterval(timer)
    }
  }, [selectedSatellite?.id])

  if (!selectedSatellite) return null

  const sat = runtimeDetail && String(runtimeDetail?.id ?? '') === String(selectedSatellite?.id ?? '')
    ? { ...selectedSatellite, ...runtimeDetail }
    : selectedSatellite
  const satFaultTag = String((sat as any)?.fault_tag ?? '')
  const isFault = isNodeFault(sat)
  const containerState = String((sat as any)?.container_state ?? 'stopped')
  const runningCoreNfTypes = Array.isArray((sat as any)?.running_core_nf_types)
    ? (sat as any).running_core_nf_types.map((x: any) => String(x))
    : []
  const coreBusinessLoad = (sat as any)?.core_business_load ?? {}
  const coreLoadIndex = Number((sat as any)?.core_network_load ?? coreBusinessLoad?.load_index ?? 0)
  const signalingLoad = Number(coreBusinessLoad?.signaling_load ?? 0)
  const sessionLoad = Number(coreBusinessLoad?.session_load ?? 0)
  const userPlaneLoad = Number(coreBusinessLoad?.user_plane_load ?? 0)
  const mobilityLoad = Number(coreBusinessLoad?.mobility_load ?? 0)
  const policyLoad = Number(coreBusinessLoad?.policy_load ?? 0)
  const authLoad = Number(coreBusinessLoad?.auth_load ?? 0)


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
  const runningCoreNfs = safeDeployments
    .flatMap((d: any) => {
      const label = resolveSfcLabel(safeDeployments as any, {
        deploymentId: String(d?.deployment_id ?? ''),
        sessionId: String(d?.session_id ?? ''),
        requestId: String(d?.request_id ?? ''),
      })
      const per = Array.isArray(d?.per_vnf) ? d.per_vnf : []
      return per
        .filter((p: any) => String(p?.node ?? '') === String(sat?.id ?? ''))
        .map((p: any) => ({
          deployment_id: String(d?.deployment_id ?? ''),
          core_id: label,
          nf_type: String(p?.nf_type ?? p?.core_nf ?? p?.vnf ?? ''),
        }))
    })
    .filter((x: any) => x && x.nf_type) as Array<{ deployment_id: string; core_id: string; nf_type: string }>
  const runningDisplayNfs = runningCoreNfs.length > 0
    ? runningCoreNfs
    : runningCoreNfTypes.map((nfType: string, i: number) => ({ deployment_id: `runtime-${i}`, core_id: '-', nf_type: nfType }))
  const runningCoreNfCount = Number((sat as any)?.running_core_nf_count ?? runningDisplayNfs.length ?? 0)

  return (
    <div
      className="absolute right-[348px] top-12 z-10 rounded-2xl overflow-hidden shadow-2xl"
      style={{
        width: 318,
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

        <div className="flex flex-wrap items-center gap-x-2 gap-y-1 text-[10px] leading-4 pr-5">
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
          <span className="text-cyan-300">
            · 容器 {containerState}
          </span>
          {runningCoreNfCount > 0 && (
            <span className="text-yellow-400">
              · {runningCoreNfCount} 网元
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
            <Navigation className="w-3 h-3" />SGP4 轨道参数
          </div>

          <div className="grid grid-cols-2 gap-x-3 gap-y-1 text-[10px]">
            {[
              ['模型', orbital?.propagation_model ?? 'SGP4'],
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
                'RAAN',
                orbital?.raan != null ? `${Number(orbital.raan).toFixed(2)}°` : '-'
              ],
              [
                '平近点角',
                orbital?.mean_anomaly_deg != null ? `${Number(orbital.mean_anomaly_deg).toFixed(2)}°` : '-'
              ],
              [
                '偏心率',
                orbital?.eccentricity != null ? Number(orbital.eccentricity).toFixed(6) : '-'
              ],
              [
                '平均运动',
                orbital?.mean_motion_rev_per_day != null ? `${Number(orbital.mean_motion_rev_per_day).toFixed(6)} rev/d` : '-'
              ],
              [
                '周期',
                orbital?.period_minutes != null ? `${Number(orbital.period_minutes).toFixed(2)}min` : '-'
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
          <div className="mt-2 text-[9px] text-slate-500 font-mono">
            {orbital?.epoch_iso && <div>Epoch {String(orbital.epoch_iso)}</div>}
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
            运行中核心网网元 ({runningCoreNfCount})
          </div>

          {runningDisplayNfs.length > 0 ? (
            <div className="space-y-1.5">
              {runningDisplayNfs.map((item, i: number) => (
                <div
                  key={`${item.deployment_id}-${item.nf_type}-${i}`}
                  className="px-2.5 py-2 rounded-lg"
                  style={{
                    background:
                      'linear-gradient(135deg, rgba(0,255,136,0.1), rgba(0,200,100,0.1))',
                    border: '1px solid rgba(0,255,136,0.25)'
                  }}
                >
                  <div className="grid grid-cols-[1fr_auto] items-center gap-2">
                    <span className="text-[11px] font-semibold text-green-300 font-mono">
                      {item.nf_type.toUpperCase()}
                    </span>
                    <span className="text-[9px] px-1.5 py-0.5 rounded-full text-cyan-200 bg-cyan-900/30 font-mono">
                      {item.core_id}
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
            <Zap className="w-3 h-3" />
            核心网业务负载 ({(coreLoadIndex * 100).toFixed(1)}%)
          </div>
          <div className="space-y-2">
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500">Signaling</span>
                <span className="text-gray-300 font-mono">{(signalingLoad * 100).toFixed(1)}%</span>
              </div>
              <Bar val={signalingLoad} max={1} />
            </div>
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500">Session</span>
                <span className="text-gray-300 font-mono">{(sessionLoad * 100).toFixed(1)}%</span>
              </div>
              <Bar val={sessionLoad} max={1} />
            </div>
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500">User Plane</span>
                <span className="text-gray-300 font-mono">{(userPlaneLoad * 100).toFixed(1)}%</span>
              </div>
              <Bar val={userPlaneLoad} max={1} />
            </div>
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500">Mobility</span>
                <span className="text-gray-300 font-mono">{(mobilityLoad * 100).toFixed(1)}%</span>
              </div>
              <Bar val={mobilityLoad} max={1} />
            </div>
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500">Policy</span>
                <span className="text-gray-300 font-mono">{(policyLoad * 100).toFixed(1)}%</span>
              </div>
              <Bar val={policyLoad} max={1} />
            </div>
            <div>
              <div className="flex justify-between text-[10px] mb-1">
                <span className="text-gray-500">Auth</span>
                <span className="text-gray-300 font-mono">{(authLoad * 100).toFixed(1)}%</span>
              </div>
              <Bar val={authLoad} max={1} />
            </div>
          </div>
        </div>

      </div>
    </div>
  )
}
