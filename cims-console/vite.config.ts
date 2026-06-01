import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'
import basicSsl from '@vitejs/plugin-basic-ssl'

// VITE_DEV_HTTPS=1 로 설정하면 HTTPS (basic-ssl). 기본은 HTTP.
// HTTPS 는 self-signed 인증서 + Node TLS 의 대용량 업로드 병목 때문에 권장하지 않음.
// LAN 내부 dev 용이므로 HTTP + JWT 인증으로 충분.
//
// Vite 가 proxy target 을 HTTPS 로 보낼 때 `secure: false` 가 필수 (CSC 자체서명).
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), 'VITE_')
  const adminTarget = env.VITE_ADMIN_TARGET || 'https://127.0.0.1:4420'
  const useHttps = env.VITE_DEV_HTTPS === '1'
  const plugins = useHttps ? [react(), basicSsl()] : [react()]
  return {
    plugins,
    server: {
      host: '0.0.0.0',
      port: 3000,
      proxy: {
        '/api': {
          target: adminTarget,
          changeOrigin: true,
          secure: false,        // CSC 자체서명 인증서 수락
          ws: false,
          // http-proxy 내부 node http agent keep-alive 로 TLS 핸드셰이크 재사용
        },
      },
    },
  }
})
