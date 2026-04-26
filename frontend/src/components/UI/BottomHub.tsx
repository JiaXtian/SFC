import { Globe, Grid3x3, Zap, Map as MapIcon, Stars, Orbit, Search } from 'lucide-react'
import { useStore } from '@/store/useStore'
import React, { useMemo, useState } from 'react'
import FPSBadge from './FPSBadge'

interface IconButtonProps {
  active: boolean
  onToggle: () => void
  icon: React.ReactElement
  color: string
  title: string
  subtitle?: string
}

function IconButton({ active, onToggle, icon, color, title, subtitle }: IconButtonProps) {
  return (
    <button
      onClick={onToggle}
      title={subtitle ? `${title}: ${subtitle}` : title}
      className="w-[34px] h-[34px] flex items-center justify-center rounded-full cursor-pointer transition-all duration-200 hover:scale-105"
      style={{
        background: active ? 'rgba(15, 32, 48, 0.78)' : 'rgba(7, 12, 20, 0.52)',
        border: active ? `1px solid ${color}aa` : '1px solid rgba(118, 145, 171, 0.22)',
        boxShadow: active
          ? `0 0 0 1px ${color}33, 0 0 14px ${color}44`
          : '0 6px 16px rgba(0,0,0,0.35)',
        color: active ? color : 'rgba(213, 225, 238, 0.7)',
        backdropFilter: 'blur(8px)',
      }}
    >
      {React.cloneElement(icon, {
        size: 16,
        strokeWidth: 2,
        className: 'block',
      })}
    </button>
  )
}

export default function BottomHub() {
  const {
    display,
    satellites,
    setDisplay,
    setSelectedSatellite,
    setSelectedLink,
    addToast,
  } = useStore()
  const [searchInput, setSearchInput] = useState('')

  const satIndex = useMemo(() => {
    const exact = new Map<string, any>()
    const all = Array.isArray(satellites) ? satellites : []
    all.forEach((sat: any) => {
      const id = String(sat?.id ?? '').trim()
      if (!id) return
      exact.set(id.toLowerCase(), sat)
    })
    return { exact, all }
  }, [satellites])

  const nextRotationSpeed = () => {
    const current = display.rotationSpeed
    const next = current === 0 ? 1 : current === 1 ? 2 : 0
    setDisplay({ rotationSpeed: next })
  }

  const runSearch = () => {
    const q = searchInput.trim().toLowerCase()
    if (!q) {
      addToast('请输入卫星节点ID，例如 SAT_000_001', 'warning')
      return
    }
    let found = satIndex.exact.get(q)
    if (!found) {
      found = satIndex.all.find((sat: any) => String(sat?.id ?? '').toLowerCase().includes(q))
    }
    if (!found) {
      addToast(`未找到卫星节点: ${searchInput.trim()}`, 'warning')
      return
    }
    setSelectedSatellite(found)
    setSelectedLink(null)
    addToast(`已高亮卫星节点 ${String(found?.id ?? '-')}`, 'success')
  }

  return (
    <div className="absolute bottom-[10px] left-1/2 -translate-x-1/2 z-[1001] pointer-events-auto select-none">
      <div className="flex items-center gap-2">
        <IconButton
          active={display.showTexture}
          onToggle={() => setDisplay({ showTexture: !display.showTexture })}
          icon={<Globe />}
          color="#7dd3fc"
          title="地球纹理"
          subtitle={display.showTexture ? '开启' : '关闭'}
        />

        <IconButton
          active={display.showBorders}
          onToggle={() => setDisplay({ showBorders: !display.showBorders })}
          icon={<MapIcon />}
          color="#a5f3fc"
          title="国家边界"
          subtitle={display.showBorders ? '开启' : '关闭'}
        />

        <IconButton
          active={display.showLatLon}
          onToggle={() => setDisplay({ showLatLon: !display.showLatLon })}
          icon={<Grid3x3 />}
          color="#7fb8ea"
          title="经纬网格"
          subtitle={display.showLatLon ? '开启' : '关闭'}
        />

        <IconButton
          active={display.showLinks}
          onToggle={() => setDisplay({ showLinks: !display.showLinks })}
          icon={<Zap />}
          color="#fbbf24"
          title="星间链路"
          subtitle={display.showLinks ? '开启' : '关闭'}
        />

        <IconButton
          active={display.showSky}
          onToggle={() => setDisplay({ showSky: !display.showSky })}
          icon={<Stars />}
          color="#c4b5fd"
          title="天空星场"
          subtitle={display.showSky ? '开启' : '关闭'}
        />

        <IconButton
          active={display.rotationSpeed > 0}
          onToggle={nextRotationSpeed}
          icon={<Orbit />}
          color="#34d399"
          title="地球旋转"
          subtitle={display.rotationSpeed === 0 ? '关闭' : `${display.rotationSpeed}x`}
        />

        {display.showLinks && (
          <div
            className="ml-1 px-2.5 h-[34px] rounded-full flex items-center gap-2"
            style={{
              background: 'rgba(7, 12, 20, 0.52)',
              border: '1px solid rgba(118, 145, 171, 0.2)',
              backdropFilter: 'blur(8px)',
            }}
          >
            <span className="text-[9px] text-slate-300 font-semibold">透明度</span>
            <input
              type="range"
              min="0.1"
              max="1"
              step="0.02"
              value={display.linkOpacity}
              onChange={e => setDisplay({ linkOpacity: parseFloat(e.target.value) })}
              className="w-16 h-1 cursor-pointer bg-transparent rounded-full appearance-none outline-none"
            />
          </div>
        )}

        <div
          className="ml-1 px-2 h-[34px] rounded-full flex items-center gap-1"
          style={{
            background: 'rgba(2, 6, 12, 0.58)',
            border: '1px solid rgba(118, 145, 171, 0.2)',
            backdropFilter: 'blur(8px)',
          }}
        >
          <input
            list="sat-search-list"
            value={searchInput}
            onChange={(e) => setSearchInput(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') runSearch()
            }}
            autoComplete="on"
            placeholder="搜索卫星节点"
            className="w-32 h-6 px-2 rounded-md bg-black/35 border-0 text-[10px] text-cyan-100 placeholder:text-slate-500 outline-none"
          />
          <button
            type="button"
            onClick={runSearch}
            title="搜索卫星"
            className="w-6 h-6 rounded-md bg-black/35 border-0 text-cyan-100 inline-flex items-center justify-center"
          >
            <Search size={12} />
          </button>
        </div>

        <datalist id="sat-search-list">
          {satellites.slice(0, 8000).map((sat: any) => (
            <option key={String(sat?.id ?? '')} value={String(sat?.id ?? '')} />
          ))}
        </datalist>

        <FPSBadge />
      </div>
    </div>
  )
}
