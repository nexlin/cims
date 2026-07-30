// [API] 버튼 — 지금 보고 있는 메뉴가 쓰는 API 정보를 표시한다. **개발자 모드에서만** 노출.
//
// 정보의 출처는 그 API 를 구현한 모듈의 코드다 (백엔드 각 핸들러의 `*_API_DOCS`). 이 컴포넌트는
// /api-docs?screen=<현재 경로> 로 받아 그대로 렌더만 한다 — 여기에 API 목록을 두지 않는다.
// 모듈이 설치·가용하지 않으면 응답이 비고, 그 경우 버튼 자체를 숨긴다.

import { useEffect, useState } from 'react'
import { useLocation } from 'react-router-dom'
import { Code2 } from 'lucide-react'
import { useDevMode } from '../hooks/useDevMode'
import { apiDocsApi, type ApiDoc } from '../api/apiDocs'

const METHOD_COLOR: Record<string, string> = {
  GET: 'badge--green', POST: 'badge--blue', PUT: 'badge--yellow', DELETE: 'badge--red',
}

// navigator.clipboard 는 secure context 전용 — HTTP dev 환경은 execCommand fallback.
async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) { await navigator.clipboard.writeText(text); return true }
  } catch { /* fall through */ }
  try {
    const ta = document.createElement('textarea')
    ta.value = text; ta.setAttribute('readonly', '')
    ta.style.position = 'fixed'; ta.style.left = '-9999px'
    document.body.appendChild(ta); ta.select()
    const ok = document.execCommand('copy')
    document.body.removeChild(ta)
    return ok
  } catch { return false }
}

function toCurl(a: ApiDoc): string {
  const qs = (a.params || [])
    .filter(p => p.in === 'query' && p.required)
    .map(p => `${p.name}=<${p.name}>`)
    .join('&')
  const m = a.method.toUpperCase() === 'GET' ? '' : `-X ${a.method.toUpperCase()} `
  return `curl -sk ${m}-H "Authorization: Bearer <TOKEN>" "https://<OAM>:4419${a.path}${qs ? '?' + qs : ''}"`
}

function ApiRow({ a }: { a: ApiDoc }) {
  const [open, setOpen] = useState(false)
  const [copied, setCopied] = useState('')
  const params = a.params || []
  const copy = async (what: string, text: string) => {
    if (await copyText(text)) { setCopied(what); setTimeout(() => setCopied(''), 1200) }
  }
  return (
    <div style={{ borderBottom: '1px solid var(--border)', padding: '10px 0' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
        <span className={`badge ${METHOD_COLOR[a.method.toUpperCase()] || 'badge--gray'}`}
              style={{ minWidth: 54, textAlign: 'center' }}>{a.method.toUpperCase()}</span>
        <code style={{ fontSize: 12.5, wordBreak: 'break-all' }}>{a.path}</code>
        {a.module && <span className="badge badge--gray" title="이 API 를 제공하는 모듈">{a.module}</span>}
        <span style={{ flex: 1 }} />
        <button className="btn btn--ghost btn--sm" onClick={() => copy('path', a.path)}>
          {copied === 'path' ? '복사됨' : '경로'}
        </button>
        <button className="btn btn--ghost btn--sm" onClick={() => copy('curl', toCurl(a))}>
          {copied === 'curl' ? '복사됨' : 'curl'}
        </button>
        <button className="btn btn--ghost btn--sm" onClick={() => setOpen(o => !o)}>
          {open ? '접기' : '상세'}
        </button>
      </div>
      {a.summary && (
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>{a.summary}</div>
      )}
      {open && (
        <div style={{ marginTop: 10, paddingLeft: 4 }}>
          {params.length > 0 ? (
            <table className="data-table" style={{ fontSize: 12 }}>
              <thead>
                <tr><th>파라미터</th><th>위치</th><th>타입</th><th>필수</th><th>설명</th></tr>
              </thead>
              <tbody>
                {params.map(p => (
                  <tr key={`${p.in}:${p.name}`}>
                    <td><code>{p.name}</code></td>
                    <td>{p.in}</td>
                    <td>{p.type || 'string'}{p.enum ? ` (${p.enum.join(' | ')})` : ''}</td>
                    <td>{p.required ? '예' : '—'}</td>
                    <td>{p.desc || ''}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>파라미터 없음</div>
          )}
          <div style={{ fontSize: 12, marginTop: 8, display: 'grid', gap: 3 }}>
            {a.response && <div><b>응답</b> — {a.response}</div>}
            {a.auth && <div><b>인증</b> — {a.auth}</div>}
            <div style={{ color: 'var(--text-muted)' }}><b>id</b> — <code>{a.id}</code></div>
          </div>
        </div>
      )}
    </div>
  )
}

export default function ApiDocsButton() {
  const devMode = useDevMode()
  const { pathname } = useLocation()
  // 조회 결과는 조회 대상 경로와 묶어 보관 — 메뉴가 바뀌면 새로 받기 전까지 이전 목록을 쓰지 않는다.
  const [loaded, setLoaded] = useState<{ screen: string; apis: ApiDoc[] } | null>(null)
  const [open, setOpen] = useState(false)

  // 개발자 모드가 아니면 조회하지 않는다 (평시 트래픽 0).
  useEffect(() => {
    if (!devMode) return
    let alive = true
    const done = (apis: ApiDoc[]) => { if (alive) setLoaded({ screen: pathname, apis }) }
    apiDocsApi.get(pathname)
      .then(r => done(Array.isArray(r.apis) ? r.apis : []))
      .catch(() => done([]))
    return () => { alive = false }
  }, [devMode, pathname])

  // 이 메뉴가 쓰는 API 가 없으면(= 해당 모듈 미설치/미가용, 또는 선언 없음) 버튼도 없다.
  const apis = loaded?.screen === pathname ? loaded.apis : []
  if (!devMode || apis.length === 0) return null

  return (
    <>
      <button className="btn btn--sm" onClick={() => setOpen(true)}
              title="이 메뉴가 사용하는 API 정보 (개발자 모드)"
              style={{ color: '#7c3aed', whiteSpace: 'nowrap' }}>
        <Code2 size={14} style={{ verticalAlign: '-2px', marginRight: 4 }} />
        API {apis.length}
      </button>

      {open && (
        <div className="modal-overlay" onClick={() => setOpen(false)}>
          <div className="modal-box modal-box--wide" onClick={e => e.stopPropagation()}
               style={{ width: 'min(860px, 94vw)' }}>
            <div className="modal-header">
              <span className="modal-title">{'</>'} 이 메뉴가 사용하는 API</span>
              <button className="modal-close" onClick={() => setOpen(false)}>✕</button>
            </div>
            <div className="modal-body" style={{ maxHeight: '70vh', overflowY: 'auto' }}>
              <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 10 }}>
                <code>{pathname}</code> — {apis.length}건. 각 API 를 구현한 모듈이 선언한 정보이며,
                모듈이 설치·가용할 때만 표시됩니다.
              </div>
              {apis.map(a => <ApiRow key={a.id} a={a} />)}
            </div>
          </div>
        </div>
      )}
    </>
  )
}
