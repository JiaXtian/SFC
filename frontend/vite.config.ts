import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'),
    },
  },
  server: {
    port: 3001,
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/ws': {
        target: 'ws://localhost:8080',
        ws: true,
      },
    },
  },
  build: {
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
