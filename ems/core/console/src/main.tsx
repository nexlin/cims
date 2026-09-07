import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
// 폰트 — 자체 호스팅(온프레미스 콘솔이라 CDN 을 쓰지 않는다). 시안 지정 2종:
//   본문 = Pretendard Variable(동적 서브셋 — 화면에 실제로 쓰인 글자 조각만 받는다.
//          통짜 woff2 는 2.0MB 인데 서브셋은 필요한 몇 조각만 로드된다)
//   코드/IP/경로/버전 = JetBrains Mono Variable
// (docs/design/console_design_system.md §2, cims-design-handoff/tokens/fonts.md)
import 'pretendard/dist/web/variable/pretendardvariable-dynamic-subset.css'
import '@fontsource-variable/jetbrains-mono'
import './index.css'
import App from './App.tsx'
import { getInitialTheme, applyTheme } from './theme'

applyTheme(getInitialTheme())   // 첫 페인트 전 테마 적용 (flash 방지)

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
