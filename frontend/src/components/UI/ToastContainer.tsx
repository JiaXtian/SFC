import { X, CheckCircle, Info, AlertTriangle, XCircle } from 'lucide-react'
import { useStore } from '@/store/useStore'

export default function ToastContainer() {
  const { toasts, removeToast } = useStore()
  if (!toasts.length) return null
  const icons = { success: CheckCircle, info: Info, warning: AlertTriangle, error: XCircle }
  const colors = { success: { bg: '#f0fdf4', border: '#86efac', text: '#166534', icon: '#22c55e' },
    info:    { bg: '#eff6ff', border: '#93c5fd', text: '#1d4ed8', icon: '#3b82f6' },
    warning: { bg: '#fffbeb', border: '#fde68a', text: '#854d0e', icon: '#f59e0b' },
    error:   { bg: '#fef2f2', border: '#fca5a5', text: '#991b1b', icon: '#ef4444' },
  }
  return (
    <div style={{ position:'fixed', bottom:52, right:16, zIndex:100, display:'flex', flexDirection:'column', gap:8 }}>
      {toasts.map(t => {
        const c = colors[t.type]; const Icon = icons[t.type]
        return (
          <div key={t.id} style={{
            display:'flex', alignItems:'center', gap:10, padding:'10px 14px',
            background:c.bg, border:`1px solid ${c.border}`, borderRadius:10,
            boxShadow:'0 4px 16px rgba(0,0,0,0.1)', minWidth:240, maxWidth:320, animation:'slideIn 0.2s ease'
          }}>
            <Icon size={15} color={c.icon} style={{ flexShrink: 0 }} />
            <span style={{ fontSize:12, color:c.text, flex:1, fontWeight:500 }}>{t.message}</span>
            <button onClick={() => removeToast(t.id)} style={{ background:'none', border:'none', cursor:'pointer', padding:2, opacity:0.6 }}>
              <X size={12} color={c.text} />
            </button>
          </div>
        )
      })}
      <style>{`@keyframes slideIn{from{transform:translateX(20px);opacity:0}to{transform:translateX(0);opacity:1}}`}</style>
    </div>
  )
}
