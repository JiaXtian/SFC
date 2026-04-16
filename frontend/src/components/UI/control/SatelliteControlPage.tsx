import ConstellationControlPanel from '../ConstellationControlPanel'
import SatelliteRuntimePanel from '../SatelliteRuntimePanel'

export default function SatelliteControlPage() {
  return (
    <div className="satellite-page grid h-full min-h-0 grid-cols-1 gap-4 xl:grid-cols-12">
      <div className="min-h-0 overflow-hidden xl:col-span-5">
        <ConstellationControlPanel />
      </div>
      <div className="min-h-0 overflow-hidden xl:col-span-7">
        <SatelliteRuntimePanel />
      </div>
    </div>
  )
}
