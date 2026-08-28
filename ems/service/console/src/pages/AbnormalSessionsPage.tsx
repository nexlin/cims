import { useState, useEffect, useCallback } from 'react'
import { api } from '@core/api/client'
import { useToast } from '@core/components/Toast'

// 비정상 세션 이력 — 공개 SIP 포트(VIP)로 들어오는 인터넷발 스캔/사기 호 시도.
//   CSP 는 인증(401)으로 정상 거부하지만, 로그 오염·자원 소모를 일으키므로 가시화한다.
//   탐지 신호: 외부(공인) 발신 IP · 알려진 스캐너 UA(pplsip 등) · 사기성 번호 · 인증 반복실패.
interface AbnSession {
  sesid: string; peer_ip: string; date: string
  caller: string; callee: string; ua: string
  methods: string[]; statuses: string[]
  attempts: number; first_ts: string; last_ts: string
  got_2xx: boolean; reasons: string[]; severity: 'critical' | 'major' | 'minor'
}
interface AbnResp {
  date: string; days: number; total: number
  by_ip: Record<string, number>
  by_reason: Record<string, number>
  sessions: AbnSession[]
}

const REASON_LABEL: Record<string, { label: string; color: string }> = {
  external_ip:  { label: '외부 IP',     color: 'var(--warning)' },
  scanner_ua:   { label: '스캐너 도구',  color: 'var(--danger)' },
  fraud_number: { label: '사기 번호',    color: 'var(--danger)' },
  auth_failed:  { label: '인증 실패',    color: '#9333ea' },
}
const SEV: Record<string, { label: string; bg: string }> = {
  critical: { label: '치명', bg: '#dc2626' },
  major:    { label: '높음', bg: '#ea580c' },
  minor:    { label: '낮음', bg: '#6b7280' },
}

const RANGE = [1, 3, 7]

export default function AbnormalSessionsPage() {
  const { show } = useToast()
  const [data, setData] = useState<AbnResp | null>(null)
  const [date, setDate] = useState(new Date().toISOString().substring(0, 10))
  const [days, setDays] = useState(1)
  const [loading, setLoading] = useState(false)
  // 스캔 폭주 일자는 수천 행 — 전체 DOM 렌더 시 페이지가 무거워져 페이지네이션.
  const [page, setPage] = useState(0)
  const [pageSize, setPageSize] = useState(100)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      setData(await api.get<AbnResp>(`/security/abnormal-sessions?date=${date}&days=${days}`))
      setPage(0)
    }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [date, days, show])

  useEffect(() => { load() }, [load])

  const sessions = data?.sessions ?? []
  const pageCount = Math.max(1, Math.ceil(sessions.length / pageSize))
  const pageRows = sessions.slice(page * pageSize, (page + 1) * pageSize)
  const critical = sessions.filter(s => s.severity === 'critical').length
  const scanners = data?.by_reason?.scanner_ua ?? 0
  const srcIps = Object.keys(data?.by_ip ?? {}).length
  const topIps = Object.entries(data?.by_ip ?? {}).sort((a, b) => b[1] - a[1]).slice(0, 8)

  return (
    <div>
      <div className="toolbar">
        <input type="date" className="form-input" value={date} onChange={e => setDate(e.target.value)} style={{ width: 150 }} />
        <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>범위</span>
        <div style={{ display: 'flex', gap: 2 }}>
          {RANGE.map(d => (
            <button key={d} className={`btn btn--sm ${days === d ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setDays(d)}>{d}일</button>
          ))}
        </div>
        <button className="btn btn--primary btn--sm" onClick={load}>조회</button>
        {data && <span className="ts" style={{ marginLeft: 'auto' }}>총 {data.total}건 탐지</span>}
      </div>

      <div className="panel" style={{ padding: 12, marginBottom: 12, fontSize: 12, color: 'var(--text-muted)', lineHeight: 1.6 }}>
        공개 SIP 포트(VIP)로 유입되는 <b>인터넷발 스캐닝·사기 호 시도</b>입니다. CSP 는 인증(<b>401</b>)으로 정상 거부하므로
        실제 통화로 이어지지 않지만, 로그를 오염시키고 자원을 소모합니다. 신호: <b>외부(공인) 발신 IP</b> · 알려진
        <b> 스캐너 UA</b>(pplsip 등) · <b>사기성 번호</b> · <b>인증 반복실패</b>. 다발 IP 는 방화벽 차단을 권장합니다.
        {critical > 0 && <span style={{ color: 'var(--danger, #dc2626)', fontWeight: 700 }}> ⚠ 외부에서 인증 성공(2xx)한 세션이 있습니다 — 즉시 점검 필요.</span>}
      </div>

      {loading ? <div className="empty">로딩 중...</div> : data && (
        <>
          {/* 지표 4장 — 균등 폭 그리드(값 길이에 따라 카드 폭이 달라지지 않게). */}
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(150px, 1fr))',
                        gap: 12, marginBottom: 16 }}>
            <KpiCard label="탐지 세션" value={data.total} unit="건" tone={data.total > 0 ? 'warn' : 'ok'} />
            <KpiCard label="치명(외부 인증성공)" value={critical} unit="건" tone={critical > 0 ? 'warn' : 'ok'} />
            <KpiCard label="스캐너 도구" value={scanners} unit="종" />
            <KpiCard label="발신 IP 수" value={srcIps} unit="개" />
          </div>

          {topIps.length > 0 && (
            <div className="panel" style={{ padding: 12, marginBottom: 16 }}>
              <div style={{ fontSize: 12, fontWeight: 600, marginBottom: 8 }}>발신 IP 상위 (차단 후보)</div>
              <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
                {topIps.map(([ip, n]) => (
                  <span key={ip} style={{ display: 'inline-flex', alignItems: 'center', gap: 6, padding: '3px 10px', borderRadius: 14, background: 'rgba(220,38,38,0.08)', fontSize: 12, fontFamily: 'monospace' }}>
                    {ip}<b style={{ color: 'var(--danger, #dc2626)' }}>{n}</b>
                  </span>
                ))}
              </div>
            </div>
          )}

          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 92 }}>최근 시각</th>
                <th style={{ width: 56 }}>심각도</th>
                <th style={{ width: 130 }}>발신 IP</th>
                <th>발신 → 착신</th>
                <th style={{ width: 96 }}>UA</th>
                <th style={{ width: 56, textAlign: 'right' }}>시도</th>
                <th style={{ width: 110 }}>메서드/응답</th>
                <th>사유</th>
              </tr>
            </thead>
            <tbody>
              {pageRows.map((s, i) => {
                const sev = SEV[s.severity] || SEV.minor
                return (
                  <tr key={i}>
                    <td style={{ fontSize: 11 }} className="ts">{days > 1 ? `${s.date.slice(5)} ` : ''}{(s.last_ts || '').slice(0, 8)}</td>
                    <td><span className="badge" style={{ background: sev.bg, color: '#fff', fontSize: 10 }}>{sev.label}</span></td>
                    <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{s.peer_ip || '-'}</td>
                    <td style={{ fontSize: 11, fontFamily: 'monospace' }}>
                      <span style={{ color: 'var(--text-muted)' }}>{s.caller || '?'}</span>
                      <span style={{ margin: '0 4px' }}>→</span>
                      <span>{s.callee || '?'}</span>
                    </td>
                    <td style={{ fontSize: 11 }}>{s.ua || '-'}</td>
                    <td style={{ fontSize: 12, textAlign: 'right', fontWeight: s.attempts > 5 ? 700 : 400 }}>{s.attempts}</td>
                    <td style={{ fontSize: 10, color: 'var(--text-muted)' }}>
                      {s.methods.join(',') || '-'}{s.statuses.length > 0 && <span> / {s.statuses.join(',')}</span>}
                    </td>
                    <td>
                      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 3 }}>
                        {s.reasons.map(r => {
                          const rl = REASON_LABEL[r] || { label: r, color: 'var(--text-muted)' }
                          return <span key={r} className="badge" style={{ fontSize: 9, color: rl.color, border: `1px solid ${rl.color}`, background: 'transparent' }}>{rl.label}</span>
                        })}
                      </div>
                    </td>
                  </tr>
                )
              })}
              {sessions.length === 0 && <tr><td colSpan={8} className="empty-cell">탐지된 비정상 세션 없음</td></tr>}
            </tbody>
          </table>
          {sessions.length > pageSize && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 10, padding: '10px 0' }}>
              <button className="btn btn--sm" disabled={page === 0} onClick={() => setPage(p => p - 1)}>← 이전</button>
              <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
                {page * pageSize + 1}–{Math.min((page + 1) * pageSize, sessions.length)} / {sessions.length}건
                (페이지 {page + 1}/{pageCount})
              </span>
              <button className="btn btn--sm" disabled={page >= pageCount - 1} onClick={() => setPage(p => p + 1)}>다음 →</button>
              <select value={pageSize} style={{ fontSize: 12, padding: '2px 4px' }}
                      onChange={e => { setPageSize(Number(e.target.value)); setPage(0) }}>
                {[50, 100, 200, 500].map(n => <option key={n} value={n}>{n}/쪽</option>)}
              </select>
            </div>
          )}
        </>
      )}
    </div>
  )
}

// 지표 카드 — 다른 화면(누수 회수·성능 통계)과 같은 규격을 쓴다: `.panel` 바탕, 내용 세로 중앙,
// 라벨 12px / 값 24px, 값 뒤에 단위. 화면마다 카드 모양이 달라 보이지 않게 하는 것이 목적.
function KpiCard({ label, value, unit, tone }: {
  label: string; value: number | string; unit?: string; tone?: 'ok' | 'warn'
}) {
  const color = tone === 'warn' ? 'var(--danger)' : tone === 'ok' ? 'var(--success)' : 'var(--text)'
  return (
    <div className="panel" style={{ padding: 10, display: 'flex', flexDirection: 'column' }}>
      <div style={{ flex: '1 1 auto', minHeight: 0, display: 'flex', flexDirection: 'column',
                    justifyContent: 'center', alignItems: 'center', textAlign: 'center' }}>
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>
        <div style={{ fontSize: 24, fontWeight: 700, lineHeight: 1.1, color }}>
          {value}
          {unit && <span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 2 }}>{unit}</span>}
        </div>
      </div>
    </div>
  )
}
