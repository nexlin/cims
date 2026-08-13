// [API] 배지 — 이 위젯이 호출하는 API 정보를 보여준다. **개발자 모드에서만** 노출.
//
// 매핑: 위젯 정의(WidgetDef.apis)가 자기가 부르는 API **id 만** 선언하고, 표시되는 내용(경로·
// 파라미터·응답 필드·예시·오류·비고·인증)은 전부 백엔드 `/api/v1/api-docs` 에서 온다. 즉 콘솔은 API
// 정보를 저술하지 않는다. 모듈이 설치·가용하지 않으면 문서가 없으므로 배지도 뜨지 않는다.
// 정본: docs/design/features/api_docs.md
//
// 보기 모드: 위젯 래퍼 우상단 오버레이. 편집 모드: 위젯 카드 헤더에 인라인.
// 상세는 요청/응답/오류/비고 4개 섹션이고, 경로·curl·예시는 **내용을 먼저 보여주고** 옆의 [복사]로 담는다.

import { useEffect, useState } from 'react'
import { createPortal } from 'react-dom'
import { Code2 } from 'lucide-react'
import { useDevMode } from '../hooks/useDevMode'
import { loadApiDocs, type ApiDoc, type ApiDocAuth } from '../api/apiDocs'

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

// path 파라미터는 <name> 자리표시자로, 필수 query 는 name=<name> 로 채운 호출 예시.
function toCurl(a: ApiDoc): string {
  let path = a.path
  for (const p of a.params || []) {
    if (p.in === 'path') path = path.replace(`{${p.name}}`, `<${p.name}>`)
  }
  const qs = (a.params || [])
    .filter(p => p.in === 'query' && p.required)
    .map(p => `${p.name}=<${p.name}>`)
    .join('&')
  const m = a.method.toUpperCase() === 'GET' ? '' : `-X ${a.method.toUpperCase()} `
  const body = (a.params || []).some(p => p.in === 'body')
    ? ` \\\n  -H "Content-Type: application/json" -d '{...}'` : ''
  return `curl -sk ${m}-H "Authorization: Bearer <TOKEN>" \\\n  "https://<OAM>:4419${path}${qs ? '?' + qs : ''}"${body}`
}

function authText(auth: ApiDoc['auth']): string {
  if (!auth) return ''
  if (typeof auth === 'string') return auth
  const a = auth as ApiDocAuth
  const bits = [a.scheme ? `${a.scheme} 토큰` : '', a.role ? `최소 권한 ${a.role}` : '']
    .filter(Boolean).join(' · ')
  return bits + (a.note ? ` (${a.note})` : '')
}

/** 내용 블록 + 옆의 [복사] — 클릭 즉시 복사하지 않고, 보이는 내용을 담기만 한다. */
function CopyBlock({ label, text, mono, pre }: {
  label: string; text: string; mono?: boolean; pre?: boolean
}) {
  const [copied, setCopied] = useState(false)
  const copy = async () => {
    if (await copyText(text)) { setCopied(true); setTimeout(() => setCopied(false), 1200) }
  }
  return (
    <div style={{ marginBottom: 8 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 3 }}>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</span>
        <button className="btn btn--ghost btn--sm" onClick={copy}>{copied ? '복사됨' : '복사'}</button>
      </div>
      {pre ? (
        <pre style={{ margin: 0, padding: '8px 10px', background: 'var(--bg)',
                      border: '1px solid var(--border)', borderRadius: 4, fontSize: 11.5,
                      overflowX: 'auto', whiteSpace: 'pre' }}>{text}</pre>
      ) : (
        <code style={{ display: 'block', padding: '6px 10px', background: 'var(--bg)',
                       border: '1px solid var(--border)', borderRadius: 4,
                       fontSize: mono ? 12.5 : 12, wordBreak: 'break-all' }}>{text}</code>
      )}
    </div>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div style={{ marginTop: 12 }}>
      <div style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)',
                    letterSpacing: '0.04em', marginBottom: 6 }}>{title}</div>
      {children}
    </div>
  )
}

function ApiRow({ a }: { a: ApiDoc }) {
  const [open, setOpen] = useState(false)
  const params = a.params || []
  const fields = a.response_fields || []
  const errors = a.errors || []
  const notes = a.notes || []

  return (
    <div style={{ borderBottom: '1px solid var(--border)', padding: '10px 0' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
        <span className={`badge ${METHOD_COLOR[a.method.toUpperCase()] || 'badge--gray'}`}
              style={{ minWidth: 54, textAlign: 'center' }}>{a.method.toUpperCase()}</span>
        <code style={{ fontSize: 12.5, wordBreak: 'break-all' }}>{a.path}</code>
        {a.module && <span className="badge badge--gray" title="이 API 를 제공하는 모듈">{a.module}</span>}
        <span style={{ flex: 1 }} />
        <button className="btn btn--ghost btn--sm" onClick={() => setOpen(o => !o)}>
          {open ? '접기' : '상세'}
        </button>
      </div>
      {a.summary && (
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>{a.summary}</div>
      )}

      {open && (
        <div style={{ marginTop: 6, paddingLeft: 4 }}>
          <Section title="요청">
            <CopyBlock label="경로" text={a.path} mono />
            <CopyBlock label="curl 예시" text={toCurl(a)} pre />
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
          </Section>

          <Section title="응답">
            {a.response && (
              <div style={{ fontSize: 12, marginBottom: 6 }}><code>{a.response}</code></div>
            )}
            {fields.length > 0 && (
              <table className="data-table" style={{ fontSize: 12 }}>
                <thead>
                  <tr><th>필드</th><th>타입</th><th>단위</th><th>설명</th></tr>
                </thead>
                <tbody>
                  {fields.map((f, i) => (
                    <tr key={`${f.name}-${i}`}>
                      <td><code>{f.name}</code></td>
                      <td>{f.type || 'string'}{f.enum ? ` (${f.enum.join(' | ')})` : ''}</td>
                      <td>{f.unit || '—'}</td>
                      <td>{f.desc || ''}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
            {a.example !== undefined && (
              <div style={{ marginTop: 8 }}>
                <CopyBlock label="예시 (합성 데이터)"
                           text={JSON.stringify(a.example, null, 2)} pre />
              </div>
            )}
          </Section>

          {errors.length > 0 && (
            <Section title="오류">
              <table className="data-table" style={{ fontSize: 12 }}>
                <thead><tr><th>status</th><th>조건</th><th>본문</th></tr></thead>
                <tbody>
                  {errors.map((e, i) => (
                    <tr key={`${e.status}-${i}`}>
                      <td><b>{e.status}</b></td>
                      <td>{e.when || ''}</td>
                      <td>{e.body !== undefined
                        ? <code style={{ fontSize: 11.5 }}>{JSON.stringify(e.body)}</code> : '—'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </Section>
          )}

          <Section title="비고">
            <ul style={{ margin: 0, paddingLeft: 18, fontSize: 12, display: 'grid', gap: 3 }}>
              {notes.map((n, i) => <li key={i}>{n}</li>)}
              {a.auth && <li><b>인증</b> — {authText(a.auth)}
                {typeof a.auth !== 'string' && (a.auth as ApiDocAuth).token_from && (
                  <> · 토큰 발급: <code>{(a.auth as ApiDocAuth).token_from}</code></>
                )}</li>}
              <li style={{ color: 'var(--text-muted)' }}><b>id</b> — <code>{a.id}</code></li>
            </ul>
          </Section>
        </div>
      )}
    </div>
  )
}

/** ids 로 문서를 조회해 [API n] 배지를 렌더. 개발자 모드 OFF / 문서 0건이면 null. */
export default function WidgetApiBadge({ ids, title, overlay }: {
  ids?: string[]
  title?: string          // 모달 제목에 쓸 위젯 이름
  overlay?: boolean       // true=보기 모드(위젯 우상단 절대배치), false=편집 카드 헤더 인라인
}) {
  const devMode = useDevMode()
  const [docs, setDocs] = useState<ApiDoc[] | null>(null)
  const [open, setOpen] = useState(false)
  const key = (ids || []).join(',')

  useEffect(() => {
    if (!devMode || !key) return
    let alive = true
    loadApiDocs().then(map => {
      if (alive) setDocs((ids || []).map(i => map.get(i)).filter((a): a is ApiDoc => !!a))
    })
    return () => { alive = false }
    // key 로 ids 변화를 추적 (배열 아이덴티티 대신)
  }, [devMode, key])   // eslint-disable-line react-hooks/exhaustive-deps

  if (!devMode || !key || !docs || docs.length === 0) return null

  return (
    <>
      <button className={overlay ? 'widget-api-badge widget-api-badge--overlay' : 'widget-api-badge'}
              onClick={e => { e.stopPropagation(); setOpen(true) }}
              onPointerDown={e => e.stopPropagation()}
              title="이 위젯이 사용하는 API (개발자 모드)">
        <Code2 size={12} style={{ verticalAlign: '-2px' }} /> API {docs.length}
      </button>

      {/* 모달은 document.body 로 portal — 편집 모드에서 배지가 카드 헤더(.grid-drag-handle) 안에 있어
          모달을 그 자리에 렌더하면 카드에 갇힌다(클리핑·스태킹). 또한 React 이벤트는 **DOM 이 아니라
          컴포넌트 트리**를 타고 버블링하므로, portal 뒤에도 pointer 이벤트를 여기서 끊어야 드래그
          핸들이 반응하지 않는다 (안 끊으면 모달 클릭이 위젯 이동으로 먹혀 닫기 버튼조차 안 눌린다). */}
      {open && createPortal(
        <div className="modal-overlay"
             onClick={() => setOpen(false)}
             onPointerDown={e => e.stopPropagation()}
             onPointerUp={e => e.stopPropagation()}
             onPointerMove={e => e.stopPropagation()}>
          {/* 크기 고정 — API 항목 수·상세 펼침과 무관하게 항상 같은 창. .modal-box 는 flex 컬럼 +
              max-height 라 기본값은 내용만큼 늘어난다. height 를 못박고 overflow 를 본문으로 넘긴다. */}
          <div className="modal-box modal-box--wide" onClick={e => e.stopPropagation()}
               style={{ width: 'min(940px, 96vw)',
                        height: 'min(76vh, calc(100vh - 80px))',
                        overflow: 'hidden' }}>
            <div className="modal-header">
              <span className="modal-title">{'</>'} {title || '위젯'} — 사용 API</span>
              <button className="modal-close" onClick={() => setOpen(false)}>✕</button>
            </div>
            <div className="modal-body" style={{ flex: 1, minHeight: 0, overflowY: 'auto' }}>
              <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 10 }}>
                {docs.length}건. 각 API 를 구현한 모듈이 선언한 정보이며, 모듈이 설치·가용할 때만 표시됩니다.
                예시는 합성 데이터입니다.
              </div>
              {docs.map(a => <ApiRow key={a.id} a={a} />)}
            </div>
          </div>
        </div>,
        document.body)}
    </>
  )
}
