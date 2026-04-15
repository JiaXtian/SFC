import ConstellationControlPanel from '../ConstellationControlPanel'
import SatelliteRuntimePanel from '../SatelliteRuntimePanel'

export default function SatelliteControlPage() {
  return (
    <div className="grid h-full min-h-0 grid-cols-1 gap-4 2xl:grid-cols-12">
      <div className="2xl:col-span-4 overflow-hidden">
        <ConstellationControlPanel />
      </div>
      <div className="2xl:col-span-8 overflow-hidden">
        <SatelliteRuntimePanel />
      </div>
    </div>
  )
}
