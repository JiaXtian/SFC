import { useState, useEffect } from 'react'
import { Sphere, Line } from '@react-three/drei'
import * as THREE from 'three'
import { useStore } from '@/store/useStore'

const R = 5
const PI = Math.PI

function latLonToVec3(lat: number, lon: number, r: number): THREE.Vector3 {
  const phi = (90 - lat) * (PI / 180)
  const theta = (lon + 180) * (PI / 180)

  const x = -r * Math.sin(phi) * Math.cos(theta)
  const y = r * Math.cos(phi)
  const z = r * Math.sin(phi) * Math.sin(theta)

  return new THREE.Vector3(x, y, z)
}

function buildVectorTexture(geojson: any): THREE.CanvasTexture {
  const canvas = document.createElement('canvas')
  canvas.width = 2048
  canvas.height = 1024
  const ctx = canvas.getContext('2d') as CanvasRenderingContext2D

  const oceanGrad = ctx.createLinearGradient(0, 0, 0, canvas.height)
  oceanGrad.addColorStop(0, '#051f33')
  oceanGrad.addColorStop(0.5, '#041a2b')
  oceanGrad.addColorStop(1, '#031321')
  ctx.fillStyle = oceanGrad
  ctx.fillRect(0, 0, canvas.width, canvas.height)

  const drawRing = (ring: number[][]) => {
    if (!ring || ring.length < 3) return
    ctx.beginPath()
    ring.forEach(([lon, lat], idx) => {
      const x = ((lon + 180) / 360) * canvas.width
      const y = ((90 - lat) / 180) * canvas.height
      if (idx === 0) ctx.moveTo(x, y)
      else ctx.lineTo(x, y)
    })
    ctx.closePath()
    ctx.fillStyle = '#0e4d56'
    ctx.strokeStyle = 'rgba(126, 205, 250, 0.85)'
    ctx.lineWidth = 0.2
    ctx.fill()
    ctx.stroke()
  }

  geojson.features.forEach((f: any) => {
    const { type, coordinates } = f.geometry
    if (type === 'Polygon') {
      coordinates.forEach((ring: number[][]) => drawRing(ring))
    } else if (type === 'MultiPolygon') {
      coordinates.forEach((poly: number[][][]) => poly.forEach((ring: number[][]) => drawRing(ring)))
    }
  })

  const tex = new THREE.CanvasTexture(canvas)
  tex.wrapS = THREE.RepeatWrapping
  tex.repeat.x = 1
  tex.colorSpace = (THREE as any).SRGBColorSpace || tex.colorSpace
  tex.flipY = true
  tex.anisotropy = 16
  tex.needsUpdate = true
  return tex
}

function EarthContent({
  showTexture,
  vectorTexture,
  showAtmosphere,
  renderQuality,
}: {
  showTexture: boolean
  vectorTexture: THREE.Texture | null
  showAtmosphere: boolean
  renderQuality: 'high' | 'balanced' | 'performance'
}) {
  const [satelliteTexture, setSatelliteTexture] = useState<THREE.Texture | null>(null)

  useEffect(() => {
    let disposed = false
    let activeTexture: THREE.Texture | null = null

    const textureUrls = [
      //'http://unpkg.com/three-globe/example/img/earth-day.jpg',
      'https://unpkg.com/three-globe/example/img/earth-blue-marble.jpg',
      'https://cdn.jsdelivr.net/gh/mrdoob/three.js@master/examples/textures/planets/earth_atmos_2048.jpg',
      'https://threejs.org/examples/textures/planets/earth_atmos_2048.jpg',
    ]

    const setupTexture = (tex: THREE.Texture) => {
      tex.wrapS = THREE.RepeatWrapping
      tex.repeat.x = 1
      if ('colorSpace' in tex) {
        ;(tex as any).colorSpace = (THREE as any).SRGBColorSpace || 'srgb'
      } else {
        ;(tex as any).encoding = (THREE as any).sRGBEncoding || 3001
      }
      tex.flipY = true
      tex.anisotropy = renderQuality === 'high' ? 16 : renderQuality === 'balanced' ? 8 : 4
      tex.needsUpdate = true
      return tex
    }

    const tryLoad = (idx: number) => {
      if (idx >= textureUrls.length || disposed) {
        setSatelliteTexture(null)
        return
      }
      const loader = new THREE.TextureLoader()
      loader.setCrossOrigin('anonymous')
      loader.load(
        textureUrls[idx],
        (tex) => {
          if (disposed) {
            tex.dispose()
            return
          }
          activeTexture = setupTexture(tex)
          setSatelliteTexture(activeTexture)
        },
        undefined,
        () => {
          tryLoad(idx + 1)
        }
      )
    }

    tryLoad(0)

    return () => {
      disposed = true
      if (activeTexture) activeTexture.dispose()
    }
  }, [renderQuality])

  const useMap = showTexture && satelliteTexture ? satelliteTexture : vectorTexture

  return (
    <group>
      <Sphere args={[R, 96, 96]}>
        {useMap ? (
          <meshStandardMaterial
            map={useMap}
            color="#ffffff"
            roughness={1}
            metalness={0}
            emissive="#042138"
            emissiveIntensity={0.1}
          />
        ) : (
          <meshPhongMaterial color="#042b4d" shininess={14} specular={new THREE.Color(0x1f4260)} />
        )}
      </Sphere>

      {showAtmosphere && (
        <>
          <Sphere args={[R + 0.035, 72, 72]}>
            <meshPhongMaterial color="#fcfcfc" transparent opacity={0.02} side={THREE.DoubleSide} depthWrite={false} />
          </Sphere>

          <Sphere args={[R + 0.16, 72, 72]}>
            <meshBasicMaterial color="#195b89" transparent opacity={0.03} side={THREE.BackSide} depthWrite={false} />
          </Sphere>
        </>
      )}
    </group>
  )
}

export default function Earth() {
  const display = useStore((s: any) => s.display)
  const [gridLines, setGridLines] = useState<THREE.Vector3[][]>([])
  const [latLineCount, setLatLineCount] = useState(0)
  const [borderLines, setBorderLines] = useState<THREE.Vector3[][]>([])
  const [vectorTexture, setVectorTexture] = useState<THREE.Texture | null>(null)

  useEffect(() => {
    const lines: THREE.Vector3[][] = []
    let latCount = 0
    for (let lat = -80; lat <= 80; lat += 10) {
      const pts = []
      for (let lon = -180; lon <= 180; lon += 5) {
        pts.push(latLonToVec3(lat, lon, R + 0.035))
      }
      lines.push(pts)
      latCount++
    }
    for (let lon = -180; lon <= 180; lon += 20) {
      const pts = []
      for (let lat = -85; lat <= 85; lat += 5) {
        pts.push(latLonToVec3(lat, lon, R + 0.035))
      }
      lines.push(pts)
    }
    setGridLines(lines)
    setLatLineCount(latCount)
  }, [])

  useEffect(() => {
    let disposed = false

    const fetchGeoJsonWithFallback = async () => {
      const sources = [
        'https://raw.githubusercontent.com/johan/world.geo.json/master/countries.geo.json',
        'https://cdn.jsdelivr.net/gh/johan/world.geo.json@master/countries.geo.json',
        'https://fastly.jsdelivr.net/gh/johan/world.geo.json@master/countries.geo.json',
      ]
      let lastErr: any = null
      for (const url of sources) {
        try {
          const res = await fetch(url)
          if (!res.ok) throw new Error(`HTTP ${res.status}`)
          return await res.json()
        } catch (e) {
          lastErr = e
        }
      }
      throw lastErr ?? new Error('failed to load country geojson')
    }

    async function loadBorders() {
      try {
        const geojson = await fetchGeoJsonWithFallback()
        if (disposed) return

        const allLines: THREE.Vector3[][] = []
        geojson.features.forEach((f: any) => {
          const { type, coordinates } = f.geometry
          if (type === 'Polygon') {
            coordinates.forEach((ring: number[][]) => {
              allLines.push(ring.map(([lon, lat]) => latLonToVec3(lat, lon, R + 0.055)))
            })
          } else if (type === 'MultiPolygon') {
            coordinates.forEach((poly: number[][][]) => {
              poly.forEach((ring: number[][]) => {
                allLines.push(ring.map(([lon, lat]) => latLonToVec3(lat, lon, R + 0.055)))
              })
            })
          }
        })
        setBorderLines(allLines)

        const tex = buildVectorTexture(geojson)
        setVectorTexture(prev => {
          prev?.dispose()
          return tex
        })
      } catch (e) {
        console.error('Borders load error:', e)
      }
    }

    loadBorders()

    return () => {
      disposed = true
    }
  }, [])

  return (
    <group>
      <EarthContent
        showTexture={display.showTexture}
        vectorTexture={vectorTexture}
        showAtmosphere={display.showAtmosphere}
        renderQuality={display.renderQuality}
      />

      {display.showLatLon &&
        gridLines.map((pts, i) => {
          const isLongitude = i >= latLineCount
          return (
            <Line
              key={`grid-${i}`}
              points={pts}
              color={isLongitude ? '#74b7ff' : '#5ea4eb'}
              lineWidth={0.26}
              transparent
              opacity={isLongitude ? 0.38 : 0.32}
              depthTest
              depthWrite={false}
            />
          )
        })}

      {display.showBorders &&
        borderLines.map((pts, i) => (
          <Line
            key={`border-${i}`}
            points={pts}
            color="#b5d8fc"
            lineWidth={0.3}
            transparent
            opacity={0.9}
            depthTest
            depthWrite={false}
          />
        ))}
    </group>
  )
}
