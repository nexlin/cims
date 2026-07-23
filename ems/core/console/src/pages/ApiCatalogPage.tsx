// 연동/API — 외부 팀(별도 VoLTE/PTT 사용관리 웹)에 넘길 공유(READ) API 카탈로그.
// 백엔드 shareable_apis(Service Descriptor) 를 category별로 브라우징 + OpenAPI 3 스펙 다운로드/복사.
// 인계물의 정본은 OpenAPI 파일(코드젠/Postman import). 이 화면은 그걸 보고·받는 허브.
import { useState, useEffect, useCallback, useMemo } from 'react'
import { useToast } from '../components/Toast'
import { apiCatalogApi, type ShareableApi, type ApiCategory } from '../api/apiCatalog'

const CATEGORY_LABELS: Record<string, string> = {
  stats: '통계 / 사용량', history: '호·세션 이력', recording: '녹취', subscriber: '가입자 / 그룹 / 조직',
}
const catLabel = (c: ApiCategory) => CATEGORY_LABELS[c] ?? c

// 인증 없이 복사 실패하지 않게 가드 (비-보안 컨텍스트 대비).
async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) { await navigator.clipboard.writeText(text); return true }
  } catch { /* fall through */ }
  return false
}

function methodColor(m: string): string {
  switch (m.toUpperCase()) {
    case 'GET': return '#2563eb'
    case 'POST': return '#16a34a'
    case 'PUT': return '#d97706'
    case 'DELETE': return 'var(--danger)'
    default: return 'var(--text-muted)'
  }
}

// 엔드포인트 → 예시 curl. path 파라미터는 <name>, query 는 빈 값 스텁.
function toCurl(a: ShareableApi): string {
  const path = a.path.replace(/\{(\w+)\}/g, '<$1>')
  const q = (a.params ?? []).filter(p => (p.in ?? 'query') === 'query').map(p => `${p.name}=`).join('&')
  const url = `/api/v1${path}${q ? '?' + q : ''}`
  return `curl -H "Authorization: Bearer <TOKEN>" "${url}"`
}

function EndpointRow({ a, onCopy }: { a: ShareableApi; onCopy: (t: string, label: string) => void }) {
  const [open, setOpen] = useState(false)
  const queryParams = (a.params ?? []).filter(p => (p.in ?? 'query') === 'query')
  const pathParams = (a.params ?? []).filter(p => p.in === 'path')
  return (
    <div style={{ border: '1px solid var(--border)', borderRadius: 6, marginBottom: 6 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '8px 10px', cursor: 'pointer' }}
           onClick={() => setOpen(o => !o)}>
        <span style={{ fontFamily: 'monospace', fontWeight: 700, fontSize: 12, color: methodColor(a.method),
                       minWidth: 44 }}>{a.method.toUpperCase()}</span>
        <code style={{ fontSize: 13 }}>{a.path}</code>
        <span style={{ color: 'var(--text-muted)', fontSize: 12, overflow: 'hidden', textOverflow: 'ellipsis',
                       whiteSpace: 'nowrap' }}>{a.summary}</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
          <button className="btn btn--sm" onClick={e => { e.stopPropagation(); onCopy(`/api/v1${a.path}`, '경로') }}
                  title="경로 복사">경로</button>
          <button className="btn btn--sm" onClick={e => { e.stopPropagation(); onCopy(toCurl(a), 'curl') }}
                  title="curl 복사">curl</button>
          <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>{open ? '▲' : '▼'}</span>
        </div>
      </div>
      {open && (
        <div style={{ padding: '4px 12px 12px', borderTop: '1px solid var(--border)', fontSize: 13 }}>
          <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap', margin: '8px 0', color: 'var(--text-muted)', fontSize: 12 }}>
            <span>인증: {a.auth ?? 'Bearer JWT'}</span>
            {a.audience && <span>대상: {a.audience}</span>}
            <span>id: <code>{a.id}</code></span>
          </div>
          {(pathParams.length > 0 || queryParams.length > 0) ? (
            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, margin: '6px 0' }}>
              <thead>
                <tr style={{ textAlign: 'left', color: 'var(--text-muted)' }}>
                  <th style={{ padding: '3px 6px' }}>파라미터</th><th style={{ padding: '3px 6px' }}>위치</th>
                  <th style={{ padding: '3px 6px' }}>타입</th><th style={{ padding: '3px 6px' }}>필수</th>
                  <th style={{ padding: '3px 6px' }}>설명</th>
                </tr>
              </thead>
              <tbody>
                {[...pathParams, ...queryParams].map(p => (
                  <tr key={`${p.in}-${p.name}`} style={{ borderTop: '1px solid var(--border)' }}>
                    <td style={{ padding: '3px 6px' }}><code>{p.name}</code></td>
                    <td style={{ padding: '3px 6px' }}>{p.in ?? 'query'}</td>
                    <td style={{ padding: '3px 6px' }}>{p.type ?? 'string'}{p.enum ? ` (${p.enum.join('|')})` : ''}</td>
                    <td style={{ padding: '3px 6px' }}>{p.required ? '✔' : ''}</td>
                    <td style={{ padding: '3px 6px', color: 'var(--text-muted)' }}>{p.desc ?? ''}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : <div style={{ color: 'var(--text-muted)', fontSize: 12, margin: '6px 0' }}>파라미터 없음</div>}
          {a.example !== undefined && (
            <div style={{ marginTop: 6 }}>
              <div style={{ color: 'var(--text-muted)', fontSize: 12, marginBottom: 2 }}>응답 예시</div>
              <pre style={{ margin: 0, padding: 8, background: 'var(--surface-2)', borderRadius: 4, fontSize: 12,
                            overflowX: 'auto' }}>{JSON.stringify(a.example, null, 2)}</pre>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

export default function ApiCatalogPage() {
  const { show } = useToast()
  const [endpoints, setEndpoints] = useState<ShareableApi[]>([])
  const [categories, setCategories] = useState<string[]>([])
  const [loading, setLoading] = useState(true)
  const [err, setErr] = useState('')
  const [q, setQ] = useState('')
  const [busy, setBusy] = useState(false)

  const load = useCallback(async () => {
    setLoading(true); setErr('')
    try {
      const res = await apiCatalogApi.get()
      setEndpoints(res.endpoints ?? [])
      setCategories(res.categories ?? [])
    } catch (e) { setErr((e as Error).message) }
    finally { setLoading(false) }
  }, [])
  useEffect(() => { void load() }, [load])

  const onCopy = useCallback(async (text: string, label: string) => {
    const ok = await copyText(text)
    show(ok ? `${label} 복사됨` : '복사 실패 (수동 복사 필요)', ok ? 'ok' : 'err')
  }, [show])

  const downloadOpenApi = async () => {
    setBusy(true)
    try {
      const doc = await apiCatalogApi.getOpenApi()
      const blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url; a.download = 'cims-openapi.json'
      document.body.appendChild(a); a.click(); document.body.removeChild(a)
      URL.revokeObjectURL(url)
      show('OpenAPI 다운로드됨 (cims-openapi.json)', 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setBusy(false) }
  }
  const copyOpenApi = async () => {
    setBusy(true)
    try {
      const doc = await apiCatalogApi.getOpenApi()
      const ok = await copyText(JSON.stringify(doc, null, 2))
      show(ok ? 'OpenAPI 클립보드에 복사됨' : '복사 실패 (수동 복사 필요)', ok ? 'ok' : 'err')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setBusy(false) }
  }

  const filtered = useMemo(() => {
    const needle = q.trim().toLowerCase()
    if (!needle) return endpoints
    return endpoints.filter(a =>
      a.path.toLowerCase().includes(needle) ||
      (a.summary ?? '').toLowerCase().includes(needle) ||
      a.id.toLowerCase().includes(needle))
  }, [endpoints, q])

  const grouped = useMemo(() => {
    const order = categories.length ? categories : ['stats', 'history', 'recording', 'subscriber']
    const seen = new Set(order)
    const extra = [...new Set(filtered.map(a => a.category))].filter(c => !seen.has(c))
    return [...order, ...extra]
      .map(cat => ({ cat, items: filtered.filter(a => a.category === cat) }))
      .filter(g => g.items.length > 0)
  }, [filtered, categories])

  if (loading) return <div className="empty" style={{ padding: 40 }}>불러오는 중…</div>

  return (
    <div style={{ display: 'grid', gap: 16, maxWidth: 1000 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, flexWrap: 'wrap' }}>
        <h2 style={{ margin: 0 }}>연동 / API</h2>
        <span style={{ color: 'var(--text-muted)', fontSize: 13 }}>외부 연동용 공개(조회) 엔드포인트 · {endpoints.length}개</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
          <button className="btn btn--sm btn--primary" onClick={downloadOpenApi} disabled={busy || !endpoints.length}>
            OpenAPI 다운로드
          </button>
          <button className="btn btn--sm btn--outline" onClick={copyOpenApi} disabled={busy || !endpoints.length}>
            복사
          </button>
        </div>
      </div>

      <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: 0 }}>
        별도 팀(VoLTE/PTT 사용관리 웹)에 넘기는 <b>조회 전용(READ)</b> API 계약입니다. 인계물 정본은
        <b> OpenAPI 3</b> 스펙 파일(위 [OpenAPI 다운로드]) — Postman/코드젠으로 import 하세요. 인증은 Bearer JWT.
      </p>

      {err && <div className="auth-error">{err}</div>}

      {endpoints.length === 0 ? (
        <div className="empty" style={{ padding: 32, textAlign: 'center', color: 'var(--text-muted)' }}>
          공개할 API가 없습니다 — 서비스(oam-svc/CSC) 미설치이거나 descriptor 에 <code>shareable_apis</code> 가 없습니다.
        </div>
      ) : (
        <>
          <input className="form-input" placeholder="경로/설명/id 검색…" value={q}
                 onChange={e => setQ(e.target.value)} style={{ maxWidth: 320 }} />
          {grouped.length === 0 ? (
            <div className="empty" style={{ padding: 20 }}>검색 결과 없음</div>
          ) : grouped.map(g => (
            <section key={g.cat} className="panel" style={{ padding: 14 }}>
              <div style={{ fontWeight: 600, marginBottom: 10 }}>{catLabel(g.cat)} <span style={{ color: 'var(--text-muted)', fontWeight: 400, fontSize: 12 }}>({g.items.length})</span></div>
              {g.items.map(a => <EndpointRow key={a.id} a={a} onCopy={onCopy} />)}
            </section>
          ))}
        </>
      )}
    </div>
  )
}
