import { useState, useEffect, useCallback } from 'react'
import { useSearchParams } from 'react-router-dom'
import { statsApi, type Subscriber } from '../../../api/stats'
import { useToast } from '../../../components/Toast'

function OnlineDot({ online }: { online: boolean }) {
  return (
    <span style={{
      display: 'inline-block', width: 8, height: 8, borderRadius: '50%',
      background: online ? '#22c55e' : '#d1d5db', marginRight: 6
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
  const [subs, setSubs] = useState<Subscriber[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState(searchParams.get('q') || '')
  const [filter, setFilter] = useState<'all' | 'online' | 'busy'>('all')

  const load = useCallback(async () => {
    try {
      const res = await statsApi.subscribers()
      setSubs(res.subscribers)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => {
    load()
    const iv = setInterval(load, 5000)
    return () => clearInterval(iv)
  }, [load])

  // 필터링
  const filtered = subs.filter(s => {
    // 검색
    if (search) {
      const q = search.toLowerCase()
      const match = s.name.toLowerCase().includes(q)
        || (s.voip?.msisdn || '').includes(q)
        || (s.ptt?.msisdn || '').includes(q)
      if (!match) return false
    }
    // 상태 필터
    if (filter === 'online') return (s.voip?.online || s.ptt?.online)
    if (filter === 'busy') return ((s.voip?.calls?.length || 0) > 0 || (s.ptt?.groups?.length || 0) > 0)
    return true
  })

  const totalOnline = subs.filter(s => s.voip?.online || s.ptt?.online).length
  const totalBusy = subs.filter(s => (s.voip?.calls?.length || 0) > 0 || (s.ptt?.groups?.length || 0) > 0).length

  return (
    <div>

      {/* 요약 + 필터 */}
      <div className="toolbar">
        <button className={`btn btn--sm ${filter === 'all' ? 'btn--primary' : 'btn--ghost'}`}
          onClick={() => setFilter('all')}>전체 ({subs.length})</button>
        <button className={`btn btn--sm ${filter === 'online' ? 'btn--primary' : 'btn--ghost'}`}
          onClick={() => setFilter('online')}>접속 중 ({totalOnline})</button>
        <button className={`btn btn--sm ${filter === 'busy' ? 'btn--primary' : 'btn--ghost'}`}
          onClick={() => setFilter('busy')}>통화 중 ({totalBusy})</button>
        <input className="search-input" placeholder="이름/번호 검색"
          value={search}
          onChange={e => {
            setSearch(e.target.value)
            // URL ?q= 동기화 (Dashboard 등에서 deep-link 진입 시 검색어 보존)
            if (e.target.value) setSearchParams({ q: e.target.value }, { replace: true })
            else { searchParams.delete('q'); setSearchParams(searchParams, { replace: true }) }
          }}
          style={{ maxWidth: 200 }} />
        <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: 12 }}>5초 자동 갱신</span>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : filtered.length === 0 ? (
        <div className="empty">가입자가 없습니다</div>
      ) : (
        <div className="panel">
          <table className="data-table">
            <thead>
              <tr>
                <th>이름</th>
                <th>VoIP 번호</th>
                <th>VoIP 접속</th>
                <th>VoIP 통화 상태</th>
                <th>PTT 번호</th>
                <th>PTT 접속</th>
                <th>PTT 서비스 상태</th>
              </tr>
            </thead>
            <tbody>
              {filtered.map(s => (
                <tr key={s.person_id}>
                  {/* 이름 */}
                  <td style={{ fontWeight: 600 }}>{s.name}</td>

                  {/* VoIP 번호 */}
                  <td className="ts">{s.voip?.msisdn || '-'}</td>

                  {/* VoIP 접속 */}
                  <td>
                    {s.voip ? (
                      <><OnlineDot online={s.voip.online} />{s.voip.online ? '접속' : '미접속'}</>
                    ) : <span className="ts">-</span>}
                  </td>

                  {/* VoIP 통화 상태 */}
                  <td>
                    {s.voip?.calls && s.voip.calls.length > 0 ? (
                      s.voip.calls.map((c, i) => (
                        <div key={i} style={{ marginBottom: i < s.voip!.calls.length - 1 ? 4 : 0 }}>
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
                      <span className="ts">{s.voip?.online ? '대기' : '-'}</span>
                    )}
                  </td>

                  {/* PTT 번호 */}
                  <td className="ts">{s.ptt?.msisdn || '-'}</td>

                  {/* PTT 접속 */}
                  <td>
                    {s.ptt ? (
                      <><OnlineDot online={s.ptt.online} />{s.ptt.online ? '접속' : '미접속'}</>
                    ) : <span className="ts">-</span>}
                  </td>

                  {/* PTT 서비스 상태 */}
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
        </div>
      )}
    </div>
  )
}
