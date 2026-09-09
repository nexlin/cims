import { cn } from '@core/lib/utils'
import { useState, useEffect } from 'react'

const DOCS = [
  { id: 'arch', title: '시스템 아키텍처', file: '/docs/03_System_Architecture.md' },
  { id: 'ue',   title: 'UE 연동 규격',    file: '/docs/01_UE_Interface_Guide.md' },
  { id: 'api',  title: '관리 API 명세',   file: '/docs/02_Admin_API_Guide.md' },
]

function renderMd(md: string): string {
  // 1) 코드펜스를 먼저 추출해 보호 — 인라인 `code` 정규식이 ``` 의 백틱을 잡아먹어
  //    펜스 안 다이어그램 전체가 인라인 <code> 로 말려 깨지던 문제 (아키텍처 문서).
  const blocks: string[] = []
  let s = md.replace(/^```[^\n]*\n([\s\S]*?)^```[ \t]*$/gm, (_m, body: string) => {
    const esc = body.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    blocks.push(`<pre class="md-pre"><code>${esc}</code></pre>`)
    return `\u0000B${blocks.length - 1}\u0000`
  })
  s = s
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/^### (.+)$/gm, '<h3>$1</h3>')
    .replace(/^## (.+)$/gm, '<h2>$1</h2>')
    .replace(/^# (.+)$/gm, '<h1>$1</h1>')
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/^\|(.+)\|$/gm, (_m, row) => {
      const cells = row.split('|').map((c: string) => c.trim())
      return '<tr>' + cells.map((c: string) => `<td>${c}</td>`).join('') + '</tr>'
    })
    .replace(/^([-]{3,}\|?)+$/gm, '')
    // 연속된 <tr> 묶음을 <table> 로 감쌈 — 고아 <tr> 은 HTML 파서가 태그를 버려
    // 표가 줄글로 풀리던 문제.
    .replace(/((?:<tr>.*<\/tr>\n?)+)/g, '<table class="md-table"><tbody>$1</tbody></table>')
    .replace(/^- (.+)$/gm, '<li>$1</li>')
    .replace(/^(&gt; .+)$/gm, '<blockquote>$1</blockquote>')
    .replace(/\n{2,}/g, '<br/><br/>')
    .replace(/\n/g, '<br/>')
  // 2) 보호한 코드블록 복원
  return s.replace(/\u0000B(\d+)\u0000(<br\/>)?/g, (_m, i) => blocks[Number(i)])
}

export default function DocsPage() {
  const [active, setActive] = useState(DOCS[0].id)
  const [content, setContent] = useState('')
  const [loading, setLoading] = useState(false)

  useEffect(() => {
    const doc = DOCS.find(d => d.id === active)
    if (!doc) return
    setLoading(true)
    fetch(doc.file)
      .then(r => r.text())
      .then(t => setContent(t))
      .catch(() => setContent('문서를 불러올 수 없습니다.'))
      .finally(() => setLoading(false))
  }, [active])

  return (
    <div className="flex flex-col flex-1 min-h-0">
      <div className="flex gap-1 py-2 px-3 border-b border-border bg-muted shrink-0 flex-wrap items-center">
        {DOCS.map(d => (
          <button key={d.id}
            onClick={() => setActive(d.id)}
            style={{
              padding: '4px 12px', borderRadius: 4, border: 'none', cursor: 'pointer',
              fontSize: 12, fontWeight: active === d.id ? 600 : 400,
              background: active === d.id ? 'var(--cims-info)' : 'transparent',
              color: active === d.id ? 'var(--cims-on-solid)' : 'var(--foreground)',
            }}>
            {d.title}
          </button>
        ))}
        <div className="ml-auto flex gap-1 items-center">
          {DOCS.map(d => (
            <a key={d.id} href={d.file} download
              style={{ fontSize: 10, color: 'var(--muted-foreground)', textDecoration: 'none', padding: '3px 6px', border: '1px solid var(--border)', borderRadius: 3 }}>
              {d.title}.md
            </a>
          ))}
          <a href="/docs/CIMS_Technical_Document.pptx" download
            style={{ fontSize: 11, color: 'var(--primary)', textDecoration: 'none', padding: '4px 8px', border: '1px solid var(--cims-info)', borderRadius: 4, fontWeight: 600 }}>
            PPT
          </a>
        </div>
      </div>
      <div className="flex-1 overflow-auto py-4 px-6">
        {loading ? <div className="text-muted-foreground">로딩 중...</div> :
          // 마크다운은 `dangerouslySetInnerHTML` 로 들어오므로 자식 태그에 클래스를 못 붙인다.
          // 그래서 컨테이너에서 **임의 선택자 변형**으로 토큰을 건다 — 별도 CSS 파일을 두지 않는다.
          <div className={cn(
                 'max-w-[900px] text-base leading-[1.7] text-foreground',
                 '[&_h1]:mb-3 [&_h1]:mt-6 [&_h1]:border-b-2 [&_h1]:border-primary [&_h1]:pb-1.5 [&_h1]:text-3xl [&_h1]:font-bold',
                 '[&_h2]:mb-2 [&_h2]:mt-5 [&_h2]:text-xl [&_h2]:font-bold [&_h2]:text-primary',
                 '[&_h3]:mb-1.5 [&_h3]:mt-4 [&_h3]:text-lg [&_h3]:font-semibold',
                 '[&_strong]:font-semibold',
                 '[&_li]:ml-5 [&_li]:list-disc',
                 '[&_code]:rounded-sm [&_code]:bg-secondary [&_code]:px-1.5 [&_code]:py-px [&_code]:font-mono [&_code]:text-md',
                 // 코드 블록 — 구 CSS 는 `#1e1e2e`/`#cdd6f4` 를 박아 다크 테마와 무관하게 어두웠다.
                 '[&_pre]:my-2 [&_pre]:overflow-x-auto [&_pre]:rounded-sm [&_pre]:border [&_pre]:border-border',
                 '[&_pre]:bg-secondary [&_pre]:px-3.5 [&_pre]:py-3 [&_pre]:text-sm [&_pre]:leading-[1.35]',
                 '[&_pre_code]:bg-transparent [&_pre_code]:p-0 [&_pre_code]:text-foreground',
                 '[&_table]:my-2 [&_table]:border-collapse',
                 '[&_td]:border [&_td]:border-border [&_td]:px-2.5 [&_td]:py-1 [&_td]:text-md',
                 '[&_tr:first-child_td]:bg-secondary [&_tr:first-child_td]:font-semibold',
                 '[&_blockquote]:my-2 [&_blockquote]:border-l-[3px] [&_blockquote]:border-primary',
                 '[&_blockquote]:pl-3 [&_blockquote]:text-muted-foreground',
               )}
            dangerouslySetInnerHTML={{ __html: renderMd(content) }}
          />
        }
      </div>
    </div>
  )
}
