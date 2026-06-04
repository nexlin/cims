import { useState, useEffect, useCallback, useRef } from 'react'
import { useSearchParams } from 'react-router-dom'
import { statsApi, type Subscriber, type SubscribersResponse } from '../../../api/stats'
import { useToast } from '../../../components/Toast'

type StatusFilter = 'active' | 'online' | 'all'
const LIMIT = 50

function OnlineDot({ online }: { online: boolean }) {
  return (
    <span style={{
      display: 'inline-block', width: 8, height: 8, borderRadius: '50%',
      background: online ? 'var(--success)' : 'var(--text-muted)', marginRight: 6
    }} />
  )
}

function fmtTime(iso: string | null): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

export default function ServiceStatusPage() {
  const { show } = useToast()
  const [searchParams, setSearchParams] = useSearchParams()

  // 검색어가 URL ?q= 로 들어오면 자동으로 '전체' 조회로 진입 (deep-link 보존)
  const initialQ = searchParams.get('q') || ''
  const [status, setStatus] = useState<StatusFilter>(initialQ ? 'all' : 'active')
  const [searchInput, setSearchInput] = useState(initialQ)
  const [q, setQ] = useState(initialQ)
  const [page, setPage] = useState(1)
  const [data, setData] = useState<SubscribersResponse | null>(null)
  const [loading, setLoading] = useState(true)

  // 검색어 디바운스 → 서버사이드 q
  useEffect(() => {
    const t = setTimeout(() => {
      setQ(searchInput.trim())
      setPage(1)
      if (searchInput.trim()) setSearchParams({ q: searchInput.trim() }, { replace: true })
      else { searchParams.delete('q'); setSearchParams(searchParams, { replace: true }) }
    }, 350)
    return () => clearTimeout(t)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [searchInput])

  const load = useCallback(async (showSpinner: boolean) => {
    if (showSpinner) setLoading(true)
    try {
      const res = await statsApi.subscribers({ status, q, page, limit: LIMIT })
      setData(res)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [status, q, page, show])

  // 필터/검색/페이지 변경 시 즉시 재조회 + 5초 자동 갱신(스피너 없이)
  const loadRef = useRef(load)
  loadRef.current = load
  useEffect(() => {
    load(true)
    const iv = setInterval(() => loadRef.current(false), 5000)
    return () => clearInterval(iv)
  }, [load])

  const counts = data?.counts ?? { all: 0, online: 0, active: 0 }
  const subs: Subscriber[] = data?.subscribers ?? []
  const total = data?.total ?? 0
  const totalPages = Math.max(1, Math.ceil(total / LIMIT))

  function changeStatus(s: StatusFilter) {
    setStatus(s)
    setPage(1)
  }

  const tab = (s: StatusFilter, label: string, n: number) => (
    <button className={`btn btn--sm ${status === s ? 'btn--primary' : 'btn--ghost'}`}
      onClick={() => changeStatus(s)}>{label} ({n})</button>
  )

  return (
    <div>
      <div className="toolbar">
        {tab('active', '이용 중', counts.active)}
        {tab('online', '접속 중', counts.online)}
        {tab('all', '전체', counts.all)}
        <input className="search-input" placeholder="이름/번호 검색"
          value={searchInput}
          onChange={e => setSearchInput(e.target.value)}
          style={{ maxWidth: 200 }} />
        <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: 12 }}>5초 자동 갱신</span>
      </div>

      {status === 'active' && (
        <div style={{ color: 'var(--text-muted)', fontSize: 12, margin: '0 0 8px' }}>
          현재 통화·그룹 참여 중인 가입자만 표시합니다. 특정 가입자를 찾으려면 ‘전체’ 또는 검색을 사용하세요.
        </div>
      )}

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : subs.length === 0 ? (
        <div className="empty">
          {status === 'active' ? '현재 서비스를 이용 중인 가입자가 없습니다'
            : q ? '검색 결과가 없습니다' : '가입자가 없습니다'}
        </div>
      ) : (
        <div className="panel">
          <table className="data-table">
            <thead>
              <tr>
                <th>이름</th>
                <th>VoLTE 번호</th>
                <th>VoLTE 접속</th>
                <th>VoLTE 통화 상태</th>
                <th>PTT 번호</th>
                <th>PTT 접속</th>
                <th>PTT 서비스 상태</th>
              </tr>
            </thead>
            <tbody>
              {subs.map(s => (
                <tr key={s.person_id}>
                  <td style={{ fontWeight: 600 }}>{s.name}</td>

                  <td className="ts">{s.volte?.msisdn || '-'}</td>

                  <td>
                    {s.volte ? (
                      <><OnlineDot online={s.volte.online} />{s.volte.online ? '접속' : '미접속'}</>
                    ) : <span className="ts">-</span>}
                  </td>

                  <td>
                    {s.volte?.calls && s.volte.calls.length > 0 ? (
                      s.volte.calls.map((c, i) => (
                        <div key={i} style={{ marginBottom: i < s.volte!.calls.length - 1 ? 4 : 0 }}>
                          <span className={`badge ${c.state === 'active' ? 'badge--green' : 'badge--blue'}`}>
                            {c.state === 'active' ? '통화 중' : '호출 중'}
                          </span>
                          <span style={{ fontSize: 12, marginLeft: 6 }}>
                            {c.role === 'caller' ? '→' : '←'} {c.peer}
                          </span>
                          <span className="ts" style={{ marginLeft: 6 }}>{fmtTime(c.invite_time)}</span>
                        </div>
                      ))
                    ) : (
                      <span className="ts">{s.volte?.online ? '대기' : '-'}</span>
                    )}
                  </td>

                  <td className="ts">{s.ptt?.msisdn || '-'}</td>

                  <td>
                    {s.ptt ? (
                      <><OnlineDot online={s.ptt.online} />{s.ptt.online ? '접속' : '미접속'}</>
                    ) : <span className="ts">-</span>}
                  </td>

                  <td>
                    {s.ptt?.groups && s.ptt.groups.length > 0 ? (
                      s.ptt.groups.map((g, i) => (
                        <div key={i} style={{ marginBottom: i < s.ptt!.groups.length - 1 ? 6 : 0 }}>
                          <span className="badge badge--green">참여 중</span>
                          <span style={{ fontSize: 12, marginLeft: 6, fontWeight: 600 }}>그룹 {g.group_id}</span>
                          <span className="ts" style={{ marginLeft: 6 }}>
                            ({g.active_members}/{g.total_members}명)
                          </span>
                          {g.floor_holder && (
                            <span style={{ fontSize: 11, marginLeft: 6, color: 'var(--primary)' }}>
                              화자: {g.floor_holder}
                            </span>
                          )}
                        </div>
                      ))
                    ) : (
                      <span className="ts">{s.ptt?.online ? '대기' : '-'}</span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>

          {totalPages > 1 && (
            <div className="toolbar" style={{ justifyContent: 'flex-end', borderTop: '1px solid var(--border)' }}>
              <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>
                총 {total.toLocaleString()}건 · {page}/{totalPages} 페이지
              </span>
              <button className="btn btn--sm btn--ghost" disabled={page <= 1}
                onClick={() => setPage(p => Math.max(1, p - 1))}>이전</button>
              <button className="btn btn--sm btn--ghost" disabled={page >= totalPages}
                onClick={() => setPage(p => Math.min(totalPages, p + 1))}>다음</button>
            </div>
          )}
        </div>
      )}
    </div>
  )
}
