// 라이트/다크 테마 — documentElement[data-theme] 로 index.css 토큰 세트 전환.
// 저장값(localStorage) 우선, 없으면 OS 선호도(prefers-color-scheme).
export type Theme = 'light' | 'dark'

const KEY = 'cims_theme'

export function getInitialTheme(): Theme {
  const saved = localStorage.getItem(KEY)
  if (saved === 'light' || saved === 'dark') return saved
  return window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function applyTheme(t: Theme): void {
  document.documentElement.setAttribute('data-theme', t)
  try { localStorage.setItem(KEY, t) } catch { /* ignore */ }
}
