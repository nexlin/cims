// 시험 > 토폴로지 캔버스 — 확정 UX 목업(검토용). 호스트(색 영역) › 워커·대상 노드 카드 › 풀을 드래그앤드롭으로
// 조립하는 화면을 콘솔 안에서 그대로 보인다(test_instrument.md §4·§7). 정식 편집기(React + 컨트롤러 토폴로지 v2
// 스키마)로 대체되기 전까지의 자리 — 검사 결과·워커 상태는 모의 값이고 저장하지 않는다.
// 목업 본문은 자체 완결 HTML(`mock/topology-canvas.html`)이라 iframe srcDoc 으로 격리해 띄운다. 콘솔 테마(`data-theme`)는
// 처음엔 문서 속성으로, 이후 변경은 postMessage 로 넘긴다.
import { useCallback, useEffect, useMemo, useRef } from 'react'
import { Badge } from '@core/components/ui/badge'
import mockHtml from '@tester/mock/topology-canvas.html?raw'

export default function TesterTopologyCanvasPage() {
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
        <span className="whitespace-nowrap text-md font-semibold text-foreground">토폴로지 캔버스</span>
        <Badge variant="warningSoft">목업 · 검토용</Badge>
        <span className="text-sm text-muted-foreground">호스트 위에 워커·대상 노드, 워커 위에 풀 — 확정 UX. 검사 결과는 모의, 저장 안 됨. 정식 편집기는 토폴로지 v2 스키마 이행 뒤 이 자리에</span>
      </div>
      <iframe ref={ref} title="토폴로지 캔버스 목업" srcDoc={srcDoc} onLoad={sendTheme} className="min-h-0 w-full flex-1 border-0 bg-background" />
    </div>
  )
}
