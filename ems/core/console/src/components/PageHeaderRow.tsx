import { useLocation } from 'react-router-dom'
import { ChevronRight } from 'lucide-react'
import { useMenu } from '../contexts/MenuContext'

/**
 * PageHeaderRow — breadcrumb 한 줄. 정본 = Figma `02 Components` Sec/PageHeaderRow (458:9163).
 * 전 화면 공통이고 현재 웹에는 없던 것이다 (`screens/shell.md`).
 *
 * 경로는 사이드바와 **같은 축**에서 만든다: 영역(운용/관리) › 섹션(시스템) › 라우트(시스템/인프라).
 * 활성 판정도 사이드바와 같은 규칙(`pathname === path || pathname.startsWith(path + '/')`)이라
 * 둘이 어긋나지 않는다.
 *
 * 치수는 Figma 실측: 높이 26 · 글자 11px(Caption/xs). 섹션은 링크가 아니다 —
 * 섹션 자체에는 라우트가 없고 사이드바에서 펼치는 단위이기 때문이다.
 */
export default function PageHeaderRow() {
  const { pathname } = useLocation()
  const { sections, areas } = useMenu()

  const hit = (() => {
    for (const s of sections) {
      for (const r of s.routes) {
        if (pathname === r.path || pathname.startsWith(r.path + '/')) return { s, r }
      }
    }
    return null
  })()
  if (!hit) return null

  const areaLabel = areas.find(a => a.key === (hit.s.area ?? 'admin'))?.label
  const crumbs = [areaLabel, hit.s.label].filter(Boolean) as string[]

  return (
    <nav aria-label="현재 위치"
         className="flex h-[26px] shrink-0 items-center gap-1 px-5 text-xs text-muted-foreground">
      {crumbs.map(c => (
        <span key={c} className="flex items-center gap-1">
          {c}
          <ChevronRight size={11} className="text-[var(--cims-text-disabled)]" />
        </span>
      ))}
      {/* 현재 페이지라 링크가 아니다 — 시안도 텍스트다. `aria-current` 로 위치만 알린다.
          (preflight 를 꺼 둔 동안 <a> 는 브라우저 기본 밑줄이 붙는다. 전역 앵커 리셋은
           preflight 를 켜는 T4 몫이라 여기서 전역으로 손대지 않는다.) */}
      <span aria-current="page" className="font-medium text-[var(--cims-text-header)]">
        {hit.r.title}
      </span>
    </nav>
  )
}
