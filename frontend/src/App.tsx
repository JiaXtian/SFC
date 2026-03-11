import { Suspense, lazy, useEffect, useMemo, useState } from 'react'
import { Canvas } from '@react-three/fiber'
import { OrbitControls, Stars } from '@react-three/drei'
import Earth from './components/Earth/Earth'
import Satellites from './components/Earth/Satellites'
import Links from './components/Earth/Links'
import TopBar from './components/UI/TopBar'
import LeftPanel from './components/UI/LeftPanel'
import RightPanel from './components/UI/RightPanel'
import SatelliteDetail from './components/UI/SatelliteDetail'
import LinkDetailPanel from './components/UI/LinkDetailPanel'
import BottomHub from './components/UI/BottomHub'
import TimeAxis from './components/UI/TimeAxis'
import { useStore } from './store/useStore'
import { useWebSocket } from './hooks/useWebSocket'
import { useAutoDynamics } from './hooks/useAutoDynamics'

const CandidateModal = lazy(() => import('./components/UI/CandidateModal'))
const MonitoringPage = lazy(() => import('./components/UI/MonitoringPage'))

export default function App() {
  useWebSocket()
  useAutoDynamics()
  const { candidateResult, setSelectedSatellite, setSelectedLink, display } = useStore()
  const [viewport, setViewport] = useState(() => ({
    w: window.innerWidth,
    h: window.innerHeight,
  }))
  const [route, setRoute] = useState<'main' | 'monitor'>(() =>
    window.location.pathname.startsWith('/monitor') ? 'monitor' : 'main'
  )

  useEffect(() => {
    const onPop = () => {
      setRoute(window.location.pathname.startsWith('/monitor') ? 'monitor' : 'main')
    }
    window.addEventListener('popstate', onPop)
    return () => window.removeEventListener('popstate', onPop)
  }, [])

  useEffect(() => {
    if (route === 'main') {
      document.body.style.cursor = 'default'
    }
  }, [route])

  useEffect(() => {
    const onResize = () => {
      setViewport({
        w: window.innerWidth,
        h: window.innerHeight,
      })
    }
    window.addEventListener('resize', onResize)
    return () => window.removeEventListener('resize', onResize)
  }, [])

  const uiScale = useMemo(() => {
    const widthScale = viewport.w / 1680
    const heightScale = viewport.h / 950
    const scale = Math.min(widthScale, heightScale)
    return Math.max(0.9, Math.min(1.34, scale))
  }, [viewport])
  const qualityConfig = useMemo(() => {
    if (display.renderQuality === 'performance') {
      return {
        dpr: 1 as number | [number, number],
        antialias: false,
        powerPreference: 'low-power' as WebGLPowerPreference,
        starCount: 2800,
      }
    }
    if (display.renderQuality === 'balanced') {
      return {
        dpr: [1, 1.5] as [number, number],
        antialias: true,
        powerPreference: 'default' as WebGLPowerPreference,
        starCount: 4500,
      }
    }
    return {
      dpr: [1, 2] as [number, number],
      antialias: true,
      powerPreference: 'high-performance' as WebGLPowerPreference,
      starCount: 7000,
    }
  }, [display.renderQuality])

  return (
    <div className="w-screen h-screen overflow-hidden"
      style={{
        ['--ui-scale' as any]: uiScale,
        background:
          'radial-gradient(1200px 520px at 50% 110%, rgba(24,72,115,0.32) 0%, rgba(3,8,16,0.8) 42%, #010206 76%, #000000 100%)',
        fontFamily: '"IBM Plex Sans", "Noto Sans SC", sans-serif',
      }}>

      {/* 3D Scene */}
      <Canvas
        dpr={qualityConfig.dpr}
        camera={{ position: [0, 0, 16], fov: 45, near: 0.1, far: 1000 }}
        gl={{
          antialias: qualityConfig.antialias,
          alpha: true,
          powerPreference: qualityConfig.powerPreference,
        }}
        onPointerMissed={() => {
          setSelectedSatellite(null)
          setSelectedLink(null)
        }}
  style={{ position: 'absolute', inset: 0, zIndex: 0 }}
      >
        <color attach="background" args={['#010206']} />
        
        <ambientLight intensity={0.24} />
        <hemisphereLight args={['#b5e7ff', '#030913', 0.3]} />
        <directionalLight position={[18, 16, 24]} intensity={1.65} color="#e2f3ff" />
        <directionalLight position={[-18, -9, -24]} intensity={0.58} color="#60a5fa" />
        <pointLight position={[-14, -10, -10]} intensity={0.36} color="#2563eb" />
        <pointLight position={[14, 8, 10]} intensity={0.32} color="#7dd3fc" />
        <pointLight position={[0, 20, 0]} intensity={0.16} color="#bfdbfe" />
        {display.showSky && <Stars radius={240} depth={75} count={qualityConfig.starCount} factor={3.2} saturation={0} fade speed={0.3} />}

        <Suspense fallback={null}> {/* 添加 Suspense 包裹 */}
          <Earth />
        </Suspense>
        <Links />
        <Satellites />

        <OrbitControls
          enableDamping dampingFactor={0.05}
          rotateSpeed={0.5} zoomSpeed={0.8}
          minDistance={6.5} maxDistance={38}
          enablePan={false}
          autoRotate={display.rotationSpeed > 0}
          autoRotateSpeed={display.rotationSpeed === 2 ? 1.5 : 0.8}
        />
      </Canvas>

      {/* UI Layer */}
      <div
        className="absolute inset-0 pointer-events-none"
        style={{
          zIndex: 12,
          transform: `scale(${uiScale})`,
          transformOrigin: 'top left',
          width: `${100 / uiScale}%`,
          height: `${100 / uiScale}%`,
        }}
      >
        <div className="pointer-events-auto"><TopBar /></div>
        <div className="pointer-events-auto"><LeftPanel /></div>
        <div className="pointer-events-auto"><SatelliteDetail /></div>
        <div className="pointer-events-auto"><LinkDetailPanel /></div>
        <div className="pointer-events-auto"><BottomHub /></div>
        <div className="pointer-events-auto"><TimeAxis /></div>
      </div>

      <div className="absolute inset-0 pointer-events-none" style={{ zIndex: 11 }}>
        <div className="pointer-events-auto"><RightPanel /></div>
        {candidateResult && (
          <Suspense fallback={null}>
            <div className="pointer-events-auto"><CandidateModal /></div>
          </Suspense>
        )}
      </div>

      {route === 'monitor' && (
        <Suspense fallback={<div className="absolute inset-0 z-[90] bg-black" />}>
          <MonitoringPage />
        </Suspense>
      )}
    </div>
  )
}
