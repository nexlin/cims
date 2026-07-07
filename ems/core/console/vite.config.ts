import { defineConfig, loadEnv } from 'vite'
import path from 'path'
import react from '@vitejs/plugin-react'
import basicSsl from '@vitejs/plugin-basic-ssl'

// 콘솔 base/svc 분리 — core(공통)=ems/core/console, svc(서비스 팩)=ems/service/console.
//   @core → 이 프로젝트 src,  @svc → 형제 service/console/src.
//   registry.ts(@svc/manifest) 와 svc 파일들(@core/*) 이 이 alias 로 교차 참조한다.
const CORE_SRC = path.resolve(process.cwd(), 'src')
const SVC_SRC = path.resolve(process.cwd(), '../../service/console/src')
const EMS_ROOT = path.resolve(process.cwd(), '../..')   // dev server fs.allow 범위

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
    resolve: {
      alias: {
        '@core': CORE_SRC,
        '@svc': SVC_SRC,
      },
    },
    server: {
      host: '0.0.0.0',
      port: 3000,
      // svc 팩(service/console)이 프로젝트 루트 밖이라 dev 서버 파일 접근 허용 범위를 ems/ 로 확장
      fs: { allow: [EMS_ROOT] },
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
