import SFCForm from '../SFCForm'

export default function StrategyControlPage() {
  return (
    <div className="h-full overflow-hidden">
      <div className="flex h-full flex-col rounded-2xl bg-white px-4 py-3">
        <div className="mb-2 text-2xl font-semibold text-black">部署策略生成</div>
        <div className="min-h-0 flex-1 overflow-hidden">
          <SFCForm />
        </div>
      </div>
    </div>
  )
}
