import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'
import basicSsl from '@vitejs/plugin-basic-ssl'

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), 'VITE_')
  const mcpttTarget = env.VITE_MCPTT_TARGET || 'http://127.0.0.1:4430'
  const cwrtcTarget = env.VITE_CWRTC_TARGET || 'ws://127.0.0.1:8080'
  return {
    plugins: [react(), basicSsl()],
    server: {
      host: '0.0.0.0',
      port: 3000,
      proxy: {
        '/mcptt': {
          target: mcpttTarget,
          changeOrigin: true,
          secure: false,
          rewrite: (path: string) => path.replace(/^\/mcptt/, ''),
        },
        '/cwrtc': {
          target: cwrtcTarget,
          ws: true,
          changeOrigin: true,
        },
      },
    },
  }
})
