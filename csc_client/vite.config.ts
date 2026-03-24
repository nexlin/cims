import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    host: '0.0.0.0',
    port: 3000,
    proxy: {
      '/api': {
        target: 'https://127.0.0.1:4420',
        changeOrigin: true,
        secure: false,        // self-signed cert 허용
      },
      '/cwrtc': {
        target: 'ws://127.0.0.1:8080',
        ws: true,             // WebSocket 프록시
        changeOrigin: true,
      },
    },
  },
})
