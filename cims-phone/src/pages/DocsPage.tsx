import { useState, useEffect } from 'react'

const DOCS = [
  { id: 'arch', title: '시스템 아키텍처', file: '/docs/03_System_Architecture.md' },
  { id: 'ue',   title: 'UE 연동 규격',    file: '/docs/01_UE_Interface_Guide.md' },
  { id: 'api',  title: '관리 API 명세',   file: '/docs/02_Admin_API_Guide.md' },
]

function renderMd(md: string): string {
  return md
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/^### (.+)$/gm, '<h3>$1</h3>')
    .replace(/^## (.+)$/gm, '<h2>$1</h2>')
    .replace(/^# (.+)$/gm, '<h1>$1</h1>')
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/^```(\w*)$/gm, '<pre><code>')
    .replace(/^```$/gm, '</code></pre>')
    .replace(/^\|(.+)\|$/gm, (_m, row) => {
      const cells = row.split('|').map((c: string) => c.trim())
      return '<tr>' + cells.map((c: string) => `<td>${c}</td>`).join('') + '</tr>'
    })
    .replace(/^([-]{3,}\|?)+$/gm, '')
    .replace(/^- (.+)$/gm, '<li>$1</li>')
    .replace(/^(&gt; .+)$/gm, '<blockquote>$1</blockquote>')
    .replace(/\n{2,}/g, '<br/><br/>')
    .replace(/\n/g, '<br/>')
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
    <div style={{ display: 'flex', flexDirection: 'column', height: 'calc(100vh - 70px)' }}>
      <div style={{ display: 'flex', gap: 4, padding: '8px 12px', borderBottom: '1px solid var(--border)', background: '#f9fafb', flexShrink: 0, flexWrap: 'wrap', alignItems: 'center' }}>
        {DOCS.map(d => (
          <button key={d.id}
            onClick={() => setActive(d.id)}
            style={{
              padding: '4px 12px', borderRadius: 4, border: 'none', cursor: 'pointer',
              fontSize: 12, fontWeight: active === d.id ? 600 : 400,
              background: active === d.id ? '#2563eb' : 'transparent',
              color: active === d.id ? '#fff' : '#6b7280',
            }}>
            {d.title}
          </button>
        ))}
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 4, alignItems: 'center' }}>
          {DOCS.map(d => (
            <a key={d.id} href={d.file} download
              style={{ fontSize: 10, color: '#6b7280', textDecoration: 'none', padding: '3px 6px', border: '1px solid #d1d5db', borderRadius: 3 }}>
              {d.title}.md
            </a>
          ))}
          <a href="/docs/CIMS_Technical_Document.pptx" download
            style={{ fontSize: 11, color: '#2563eb', textDecoration: 'none', padding: '4px 8px', border: '1px solid #2563eb', borderRadius: 4, fontWeight: 600 }}>
            PPT
          </a>
        </div>
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: '16px 24px' }}>
        {loading ? <div style={{ color: '#9ca3af' }}>로딩 중...</div> :
          <div className="docs-content"
            dangerouslySetInnerHTML={{ __html: renderMd(content) }}
          />
        }
      </div>
    </div>
  )
}
