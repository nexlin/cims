import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'
import basicSsl from '@vitejs/plugin-basic-ssl'

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), 'VITE_')
  const adminTarget = env.VITE_ADMIN_TARGET || 'http://127.0.0.1:4420'
  return {
    plugins: [react(), basicSsl()],
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
