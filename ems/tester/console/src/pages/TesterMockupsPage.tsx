// 시험 > UX 목업 — 확정 UX 목업 다섯 개(토폴로지 캔버스 · 시나리오 캔버스 · 실행 라이브 · 결과 보고서 · 회귀 비교)를
// 한 메뉴 아래 탭으로 본다(test_instrument.md §7). 정식 화면(캔버스 편집기 · /test/runs·results·compare 재구성)이
// 들어오면 해당 탭과 목업 HTML 을 지운다. 값은 전부 모의, 저장·실행은 토스트로만 흉내낸다 — API 호출 없음.
//
// 탭 = 경로다(`/test/topology-canvas` 등 — 사이드바에는 hidden, 문서·메모의 링크가 그대로 산다). 허브 경로
// `/test/mockups` 는 첫 탭. 목업 본문은 자체 완결 HTML(`mock/*.html`)이라 iframe srcDoc 으로 격리해 띄우고,
// 콘솔 테마(`data-theme`)는 처음엔 문서 속성으로, 이후 변경은 postMessage 로 넘긴다.
import { useCallback, useEffect, useMemo, useRef } from 'react'
import { useLocation, useNavigate } from 'react-router-dom'
import { Badge } from '@core/components/ui/badge'
import topologyHtml from '@tester/mock/topology-canvas.html?raw'
import scenarioHtml from '@tester/mock/scenario-canvas.html?raw'
import runLiveHtml from '@tester/mock/run-live.html?raw'
import runReportHtml from '@tester/mock/run-report.html?raw'
import runCompareHtml from '@tester/mock/run-compare.html?raw'

export const MOCKUP_TABS = [
  { path: '/test/topology-canvas', title: '토폴로지 캔버스', html: topologyHtml,
    hint: '호스트 위에 워커·대상 노드, 워커 위에 풀 — 드래그앤드롭 조립. 검사 결과는 모의, 저장 안 됨. 정식 편집기는 토폴로지 v2 스키마 이행 뒤' },
  { path: '/test/scenario-canvas', title: '시나리오 캔버스', html: scenarioHtml,
    hint: '역할 = 레인, 단계 = 행. 기준 토폴로지를 고르면 역할→풀 해석·워커 배분·시드를 편집 시점에 미리 본다' },
  { path: '/test/run-live', title: '실행 라이브', html: runLiveHtml,
    hint: '워커·대상 띠 + 단계 사다리·종료 조건 + 지표별 소형 차트(기대치 점선) + 절차 진행·실패 이벤트(SIP 사다리) + 필터·날짜 그룹 색인 + 계획 미리보기 시작 창' },
  { path: '/test/run-report', title: '결과 보고서', html: runReportHtml,
    hint: '왼쪽 run 레일 + 판정 요약(왜 FAIL 인가) + 시간축·알람 겹침 + 지연 분포 히스토그램 + 절차표 여유 막대 + 실패 이벤트 SIP 사다리 + 대상 증거' },
  { path: '/test/run-compare', title: '회귀 비교', html: runCompareHtml,
    hint: 'run 카드(기준 끌어 바꾸기) + 지표 매트릭스(Δ 막대·허용 오차) + 시간축 겹침 + 기대치 diff + 추세 모드(시나리오×프로파일의 모든 run, 빌드별)' },
] as const

export default function TesterMockupsPage() {
  const { pathname } = useLocation()
  const nav = useNavigate()
  const tab = MOCKUP_TABS.find(t => pathname === t.path || pathname.startsWith(t.path + '/')) ?? MOCKUP_TABS[0]
  const ref = useRef<HTMLIFrameElement>(null)
  const srcDoc = useMemo(() => {
    const theme = document.documentElement.dataset.theme
    return `<!doctype html><html lang="ko"${theme ? ` data-theme="${theme}"` : ''}><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"></head><body>${tab.html}</body></html>`
  }, [tab])
  const sendTheme = useCallback(() => {
    ref.current?.contentWindow?.postMessage({ theme: document.documentElement.dataset.theme || '' }, '*')
  }, [])
  useEffect(() => {
    const mo = new MutationObserver(sendTheme)
    mo.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] })
    return () => mo.disconnect()
  }, [sendTheme])

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">UX 목업</span>
        <Badge variant="warningSoft">검토용 · 값은 모의</Badge>
        <div role="tablist" aria-label="목업" className="flex flex-wrap items-center gap-1">
          {MOCKUP_TABS.map(t => (
            <button key={t.path} role="tab" aria-selected={t === tab} onClick={() => nav(t.path)}
                    className={`h-7 rounded-md px-2.5 text-sm transition-colors ${t === tab ? 'bg-primary text-primary-foreground' : 'text-muted-foreground hover:bg-accent hover:text-foreground'}`}>
              {t.title}
            </button>
          ))}
        </div>
        <span className="text-sm text-muted-foreground">{tab.hint}</span>
      </div>
      <iframe key={tab.path} ref={ref} title={`${tab.title} 목업`} srcDoc={srcDoc} onLoad={sendTheme}
              className="min-h-0 w-full flex-1 border-0 bg-background" />
    </div>
  )
}
