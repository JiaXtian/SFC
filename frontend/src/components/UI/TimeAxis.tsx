import { Pause, Play, TimerReset } from 'lucide-react'
import type { CSSProperties } from 'react'
import { useMemo } from 'react'
import { useStore } from '@/store/useStore'
import { apiClient } from '@/api/client'

function formatElapsed(sec: number) {
  const s = Math.max(0, Math.floor(sec))
  const hh = Math.floor(s / 3600).toString().padStart(2, '0')
  const mm = Math.floor((s % 3600) / 60).toString().padStart(2, '0')
  const ss = Math.floor(s % 60).toString().padStart(2, '0')
  return `${hh}:${mm}:${ss}`
}

export default function TimeAxis() {
  const {
    autoDynamics,
    setAutoDynamics,
    simulation,
    setSimulationViewMode,
    setPlaybackCursor,
  } = useStore()

  const history = simulation.history
  const historyCursor = simulation.history_cursor
  const historyMax = Math.max(0, history.length - 1)
  const timelineLabel = useMemo(() => {
    if (simulation.view_mode === 'playback' && history.length > 0 && historyCursor >= 0) {
      const frame = history[historyCursor]
      return `${frame?.sim_time ?? '-'}`
    }
    return `${simulation.sim_time || '-'}`
  }, [simulation.view_mode, simulation.sim_time, simulation.topology_version, history, historyCursor])

  const syncBackendDynamicConfig = (samplingSec: number, speed: number) => {
    apiClient.startDynamicSimulation({
      sampling_interval_sec: samplingSec,
      simulation_speed: speed,
    }).catch(() => {})
  }

  const pillStyle: CSSProperties = {
    background: 'rgba(125,125,125,0.16)',
    border: '1px solid rgba(20,20,20,0.82)',
    backdropFilter: 'blur(10px)',
    WebkitBackdropFilter: 'blur(10px)',
  }

  return (
    <>
      <div className="absolute left-1/2 -translate-x-1/2 bottom-[14px] z-[1002] pointer-events-auto">
        <div
          className="w-[min(65vw,840px)] h-[8px] rounded-full px-1 flex items-center"
          style={pillStyle}
        >
          <input
            type="range"
            min={0}
            max={historyMax}
            value={history.length === 0 ? 0 : Math.max(0, historyCursor)}
            disabled={history.length === 0}
            onChange={(e) => {
              setSimulationViewMode('playback')
              setPlaybackCursor(Number(e.target.value))
            }}
            className="w-full h-[3px] accent-cyan-300"
          />
        </div>
      </div>

      <div className="absolute left-1/2 -translate-x-1/2 bottom-[30px] z-[1002] pointer-events-auto flex items-center gap-1.5">
        <button
          onClick={() => setAutoDynamics({ playing: !autoDynamics.playing, enabled: true })}
          className="h-7 px-2 rounded-full text-[11px] text-cyan-100 inline-flex items-center gap-1"
          style={pillStyle}
          title={autoDynamics.playing ? '暂停动态播放' : '开始动态播放'}
        >
          {autoDynamics.playing ? <Pause className="w-3 h-3" /> : <Play className="w-3 h-3" />}
          {autoDynamics.playing ? '暂停' : '播放'}
        </button>

        <button
          onClick={() => {
            setAutoDynamics({ elapsed_sec: 0 })
            setSimulationViewMode('realtime')
          }}
          className="h-7 px-2 rounded-full text-[11px] text-slate-200 inline-flex items-center gap-1"
          style={pillStyle}
          title="重置时间轴"
        >
          <TimerReset className="w-3 h-3" />
          重置
        </button>

        <div className="h-7 px-2 rounded-full text-[10px] text-slate-200 font-mono flex items-center" style={pillStyle}>
          T+ {formatElapsed(autoDynamics.elapsed_sec)}
        </div>

        <label className="h-7 px-2.5 rounded-full text-[10px] text-slate-200 flex items-center gap-1.5 min-w-[132px]" style={pillStyle}>
          倍速
          <input
            type="range"
            min={0.5}
            max={8}
            step={0.1}
            value={autoDynamics.time_scale}
            onChange={(e) => {
              const speed = Math.max(0.5, Math.min(8, Number(e.target.value) || 1))
              setAutoDynamics({ time_scale: speed })
              syncBackendDynamicConfig(autoDynamics.resource_update_sec, speed)
            }}
            className="w-16 h-[3px] accent-sky-300"
          />
          <span className="font-mono text-sky-200">{autoDynamics.time_scale.toFixed(1)}x</span>
        </label>

        <label className="h-7 px-2 rounded-full text-[10px] text-slate-200 flex items-center gap-1" style={pillStyle}>
          资源s
          <input
            type="number"
            min={10}
            max={120}
            step={1}
            value={autoDynamics.resource_update_sec}
            onChange={(e) => {
              const sampling = Math.max(10, Math.min(120, Number(e.target.value) || 15))
              setAutoDynamics({ resource_update_sec: sampling })
              syncBackendDynamicConfig(sampling, autoDynamics.time_scale)
            }}
            className="w-10 px-1 py-0 rounded bg-black/30 border border-black/70 text-slate-100"
          />
        </label>

        <div className="h-7 px-2 rounded-full text-[10px] text-slate-300 font-mono flex items-center" style={pillStyle}>
          {timelineLabel}
        </div>
      </div>
    </>
  )
}
