import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), 'VITE_')
  const adminTarget = env.VITE_ADMIN_TARGET || 'https://127.0.0.1:4420'
  return {
    plugins: [react()],
    server: {
      host: '0.0.0.0',
      port: 3001,
      proxy: {
        '/api': {
          target: adminTarget,
          changeOrigin: true,
          secure: false,
        },
      },
    },
  }
})
