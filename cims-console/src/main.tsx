import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.tsx'
import { getInitialTheme, applyTheme } from './theme'

applyTheme(getInitialTheme())   // 첫 페인트 전 테마 적용 (flash 방지)

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
