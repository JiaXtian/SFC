import { AlertTriangle, CheckCircle2, Info, X, XCircle } from 'lucide-react'
import { useStore } from '@/store/useStore'

const iconMap = {
  info: Info,
  success: CheckCircle2,
  warning: AlertTriangle,
  error: XCircle,
} as const

const colorMap = {
  info: {
    icon: '#38bdf8',
    title: 'text-sky-200',
    border: 'rgba(56,189,248,0.45)',
  },
  success: {
    icon: '#34d399',
    title: 'text-emerald-200',
    border: 'rgba(52,211,153,0.45)',
  },
  warning: {
    icon: '#f59e0b',
    title: 'text-amber-200',
    border: 'rgba(245,158,11,0.45)',
  },
  error: {
    icon: '#fb7185',
    title: 'text-rose-200',
    border: 'rgba(251,113,133,0.45)',
  },
} as const

export default function SystemPopup() {
  const { systemPopup, closeSystemPopup } = useStore((s) => ({
    systemPopup: s.systemPopup,
    closeSystemPopup: s.closeSystemPopup,
  }))

  if (!systemPopup.open) return null

  const tone = systemPopup.type ?? 'info'
  const Icon = iconMap[tone]
  const style = colorMap[tone]

  return (
    <div className="fixed inset-0 z-[140] flex items-center justify-center px-4">
      <div className="absolute inset-0 bg-black/35" onClick={closeSystemPopup} />
      <div
        className="relative w-full max-w-[520px] rounded-xl overflow-hidden"
        style={{
          background: 'linear-gradient(160deg, rgba(8,15,28,0.96), rgba(6,12,24,0.95))',
          border: `1px solid ${style.border}`,
          boxShadow: '0 20px 60px rgba(0,0,0,0.45)',
          backdropFilter: 'blur(14px)',
        }}
      >
        <button
          type="button"
          onClick={closeSystemPopup}
          className="absolute right-2.5 top-2.5 w-7 h-7 rounded-md inline-flex items-center justify-center text-slate-400 hover:text-slate-100 hover:bg-white/10 transition"
          aria-label="close-system-popup"
        >
          <X className="w-4 h-4" />
        </button>

        <div className="px-4 pt-4 pb-3 border-b border-slate-700/60 flex items-center gap-2">
          <Icon className="w-4.5 h-4.5" color={style.icon} />
          <div className={`text-[14px] font-semibold ${style.title}`}>{systemPopup.title || '系统提示'}</div>
        </div>

        <div className="px-4 py-3.5">
          <div className="text-[13px] leading-6 text-slate-100 whitespace-pre-wrap break-words">
            {systemPopup.message}
          </div>
        </div>
      </div>
    </div>
  )
}
