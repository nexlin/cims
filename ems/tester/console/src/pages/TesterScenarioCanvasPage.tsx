// 시험 > 시나리오 캔버스 — 시나리오 편집 UX 목업(검토용). 역할 = 레인, 단계 = 행인 시퀀스 캔버스 + 팔레트 + 속성 패널 +
// 하단 드로어(YAML 양방향 · 검증 · 토폴로지 적합성(compile-check 드라이런 재현) · 절차표). 기준 토폴로지는 토폴로지 캔버스
// 목업의 프리셋 3종(media01 · ims-lab · pbx-hq)을 그대로 가정하고, 동봉 시나리오 7종을 실 데이터로 싣는다(test_instrument.md §4·§7).
// 정식 편집기(컨트롤러 vocab/compile-check API)로 대체되기 전까지의 자리 — 저장·실행은 토스트로만 흉내낸다.
// 목업 본문은 자체 완결 HTML(`mock/scenario-canvas.html`)이라 iframe srcDoc 으로 격리해 띄운다. 테마 전달은 토폴로지 캔버스와 같다.
import { useCallback, useEffect, useMemo, useRef } from 'react'
import { Badge } from '@core/components/ui/badge'
import mockHtml from '@tester/mock/scenario-canvas.html?raw'

export default function TesterScenarioCanvasPage() {
  const ref = useRef<HTMLIFrameElement>(null)
  const srcDoc = useMemo(() => {
    const theme = document.documentElement.dataset.theme
    return `<!doctype html><html lang="ko"${theme ? ` data-theme="${theme}"` : ''}><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"></head><body>${mockHtml}</body></html>`
  }, [])
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
        <span className="whitespace-nowrap text-md font-semibold text-foreground">시나리오 캔버스</span>
        <Badge variant="warningSoft">목업 · 검토용</Badge>
        <span className="text-sm text-muted-foreground">역할 = 레인, 단계 = 행. 기준 토폴로지를 고르면 역할→풀 해석·워커 배분·시드를 편집 시점에 미리 본다. 저장·실행은 흉내만</span>
      </div>
      <iframe ref={ref} title="시나리오 캔버스 목업" srcDoc={srcDoc} onLoad={sendTheme} className="min-h-0 w-full flex-1 border-0 bg-background" />
    </div>
  )
}
