import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

const backendTarget = process.env.VITE_BACKEND_URL || 'http://localhost:8080'
const mainScreenUrl = process.env.VITE_MAIN_SCREEN_URL || 'http://localhost:3001'

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'),
    },
  },
  define: {
    __SFC_APP_TARGET__: JSON.stringify('control'),
    'import.meta.env.VITE_MAIN_SCREEN_URL': JSON.stringify(mainScreenUrl),
  },
  server: {
    host: '0.0.0.0',
    port: 3002,
    proxy: {
      '/api': {
        target: backendTarget,
        changeOrigin: true,
      },
      '/ws': {
        target: backendTarget.replace(/^http/i, 'ws'),
        ws: true,
      },
    },
  },
  build: {
    outDir: 'dist-control',
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes('/src/components/UI/MonitoringPage')) return 'monitoring'
          if (id.includes('node_modules/@react-three') || id.includes('node_modules/three')) return 'three-core'
          if (id.includes('node_modules/lucide-react')) return 'ui-icons'
          if (id.includes('node_modules/react') || id.includes('node_modules/zustand') || id.includes('node_modules/axios')) {
            return 'app-vendor'
          }
        },
      },
    },
  },
})
