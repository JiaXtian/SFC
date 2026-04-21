import { Globe, Grid3x3, Zap, Map, Stars, Orbit } from 'lucide-react'
import { useStore } from '@/store/useStore'
import React from 'react'
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
  const { display, setDisplay } = useStore()

  const nextRotationSpeed = () => {
    const current = display.rotationSpeed
    const next = current === 0 ? 1 : current === 1 ? 2 : 0
    setDisplay({ rotationSpeed: next })
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
          icon={<Map />}
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

        <FPSBadge />
      </div>
    </div>
  )
}
