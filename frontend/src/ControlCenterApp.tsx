import { Suspense, lazy } from 'react'
import ControlPage from '@/components/UI/ControlPage'
import SystemPopup from '@/components/UI/SystemPopup'
import { useStore } from '@/store/useStore'
import { useWebSocket } from '@/hooks/useWebSocket'
import { useAutoDynamics } from '@/hooks/useAutoDynamics'

const CandidateModal = lazy(() => import('@/components/UI/CandidateModal'))

export default function ControlCenterApp() {
  useWebSocket()
  useAutoDynamics()
  const candidateResult = useStore((s) => s.candidateResult)

  return (
    <div className="w-screen h-screen overflow-hidden">
      <Suspense fallback={<div className="absolute inset-0 z-[90] bg-black" />}>
        <ControlPage />
        {candidateResult && (
          <div className="pointer-events-auto absolute inset-0 z-[120]">
            <CandidateModal />
          </div>
        )}
      </Suspense>
      <SystemPopup />
    </div>
  )
}
