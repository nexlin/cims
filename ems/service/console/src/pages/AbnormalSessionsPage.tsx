// 비정상 세션 이력 — 공개 SIP 포트(VIP)로 들어오는 인터넷발 스캔/사기 호 시도.
//   CSP 는 인증(401)으로 정상 거부하지만, 로그 오염·자원 소모를 일으키므로 가시화한다.
//   탐지 신호: 외부(공인) 발신 IP · 알려진 스캐너 UA(pplsip 등) · 사기성 번호 · 인증 반복실패.
//
// **화면 = 카드 하나**(`cims.abnormal-sessions`)이고 안의 네 블록(조회 조건 · 지표 · 발신 IP 상위 ·
// 세션 표)은 각각 위젯이라 카드 안 편집으로 재배치할 수 있다(console_platform §3.0.1).
// 네 블록이 같은 조회 조건·결과를 봐야 하므로 상태는 모듈 store(`abnormalStore.ts`)에 둔다.
import { useToast } from '@core/components/Toast'
import { InfoDot } from '@core/components/InfoDot'
import { abnDerived, abnormal, useAbnormal } from './abnormalStore'

const REASON_LABEL: Record<string, { label: string; color: string }> = {
  external_ip:  { label: '외부 IP',     color: 'var(--cims-warning)' },
  scanner_ua:   { label: '스캐너 도구',  color: 'var(--destructive)' },
  fraud_number: { label: '사기 번호',    color: 'var(--destructive)' },
  auth_failed:  { label: '인증 실패',    color: '#9333ea' },
}
const SEV: Record<string, { label: string; bg: string }> = {
  critical: { label: '치명', bg: '#dc2626' },
  major:    { label: '높음', bg: '#ea580c' },
  minor:    { label: '낮음', bg: '#6b7280' },
}
const RANGE = [1, 3, 7]

// ── 조회 조건 ───────────────────────────────────────────────────────────────
export function AbnFilter() {
  const { show } = useToast()
  const s = useAbnormal(show)
  const { critical } = abnDerived(s)
  return (
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      <input type="date" className="form-input" value={s.date} style={{ width: 150 }}
             onChange={e => abnormal.setDate(e.target.value)} />
      <span style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>범위</span>
      <span style={{ display: 'flex', gap: 2 }}>
        {RANGE.map(d => (
          <button key={d} className={`btn btn--sm ${s.days === d ? 'btn--primary' : 'btn--ghost'}`}
                  onClick={() => abnormal.setDays(d)}>{d}일</button>
        ))}
      </span>
      <button className="btn btn--primary btn--sm" onClick={() => void abnormal.load(show)}>조회</button>
      {/* 화면의 뜻은 한 번 읽으면 되는 설명이라 ⓘ 로 접는다. */}
      <InfoDot label="비정상 세션이란?">
        공개 SIP 포트(VIP)로 유입되는 <b>인터넷발 스캐닝·사기 호 시도</b>입니다. CSP 는 인증(<b>401</b>)으로
        정상 거부하므로 실제 통화로 이어지지 않지만, 로그를 오염시키고 자원을 소모합니다.
        신호: <b>외부(공인) 발신 IP</b> · 알려진 <b>스캐너 UA</b>(pplsip 등) · <b>사기성 번호</b> ·
        <b> 인증 반복실패</b>. 다발 IP 는 방화벽 차단을 권장합니다.
      </InfoDot>
      {/* 조치가 필요한 신호는 접지 않는다 — 설명과 달리 매번 봐야 한다. */}
      {critical > 0 && (
        <span style={{ color: 'var(--destructive)', fontWeight: 700, fontSize: 12 }}>
          ⚠ 외부에서 인증 성공(2xx)한 세션 있음 — 즉시 점검
        </span>
      )}
      {s.data && <span className="ts" style={{ marginLeft: 'auto' }}>총 {s.data.total}건 탐지</span>}
    </div>
  )
}

// ── 지표 ────────────────────────────────────────────────────────────────────
// **지표 1개 = 위젯 1개.** 탐지 세션 수 · 치명 건수 · 스캐너 종류 수 · 발신 IP 개수는 서로 다른
// 축이라 하나만 놓아도 말이 된다(§3.1 "떼어내는 것"). 지표별 컴포넌트를 두지 않고 아래 선언 표
// 하나에서 팩토리로 만든다.
export interface AbnMetric {
  key: string
  label: string           // 카드에 보이는 이름
  title: string           // 편집 목록에서 고를 때의 이름
  unit: string
  value: (d: ReturnType<typeof abnDerived>, total: number) => number
  warnWhenPositive?: boolean   // 0 이 정상인 지표 — 값이 있으면 붉게
}

export const ABN_METRICS: AbnMetric[] = [
  { key: 'total',    label: '탐지 세션',          title: '비정상 세션 — 탐지 세션',
    unit: '건', value: (_, total) => total, warnWhenPositive: true },
  { key: 'critical', label: '치명(외부 인증성공)', title: '비정상 세션 — 치명(외부 인증성공)',
    unit: '건', value: d => d.critical, warnWhenPositive: true },
  { key: 'scanners', label: '스캐너 도구',        title: '비정상 세션 — 스캐너 도구',
    unit: '종', value: d => d.scanners },
  { key: 'srcIps',   label: '발신 IP 수',         title: '비정상 세션 — 발신 IP 수',
    unit: '개', value: d => d.srcIps },
]

export function AbnKpi({ metric }: { metric: AbnMetric }) {
  const { show } = useToast()
  const s = useAbnormal(show)
  const n = metric.value(abnDerived(s), s.data?.total ?? 0)
  return (
    <KpiCard label={metric.label} value={n} unit={metric.unit}
             tone={metric.warnWhenPositive ? (n > 0 ? 'warn' : 'ok') : undefined} />
  )
}

// ── 발신 IP 상위 (차단 후보) ────────────────────────────────────────────────
export function AbnTopIps() {
  const { show } = useToast()
  const s = useAbnormal(show)
  const { topIps } = abnDerived(s)
  return (
    <div className="panel" style={{ padding: 12, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ fontSize: 12, fontWeight: 600, marginBottom: 8, flex: 'none' }}>발신 IP 상위 (차단 후보)</div>
      {topIps.length === 0 ? <div className="empty">해당 기간 발신 IP 없음</div> : (
        <div className="scroll-fill" style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignContent: 'flex-start' }}>
          {topIps.map(([ip, n]) => (
            <span key={ip} style={{ display: 'inline-flex', alignItems: 'center', gap: 6, height: 24,
                                    padding: '3px 10px', borderRadius: 14, background: 'rgba(220,38,38,0.08)',
                                    fontSize: 12, fontFamily: 'monospace' }}>
              {ip}<b style={{ color: 'var(--destructive)' }}>{n}</b>
            </span>
          ))}
        </div>
      )}
    </div>
  )
}

// ── 세션 표 ─────────────────────────────────────────────────────────────────
export function AbnTable() {
  const { show } = useToast()
  const s = useAbnormal(show)
  const { sessions, pageRows, pageCount } = abnDerived(s)
  return (
    <div className="panel" style={{ display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      {s.loading ? <div className="empty">로딩 중...</div> : (
        <>
          <div className="scroll-fill">
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
                {pageRows.map((x, i) => {
                  const sev = SEV[x.severity] || SEV.minor
                  return (
                    <tr key={i}>
                      <td style={{ fontSize: 11 }} className="ts">{s.days > 1 ? `${x.date.slice(5)} ` : ''}{(x.last_ts || '').slice(0, 8)}</td>
                      <td><span className="badge" style={{ background: sev.bg, color: '#fff', fontSize: 10 }}>{sev.label}</span></td>
                      <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{x.peer_ip || '-'}</td>
                      <td style={{ fontSize: 11, fontFamily: 'monospace' }}>
                        <span style={{ color: 'var(--muted-foreground)' }}>{x.caller || '?'}</span>
                        <span style={{ margin: '0 4px' }}>→</span>
                        <span>{x.callee || '?'}</span>
                      </td>
                      <td style={{ fontSize: 11 }}>{x.ua || '-'}</td>
                      <td style={{ fontSize: 12, textAlign: 'right', fontWeight: x.attempts > 5 ? 700 : 400 }}>{x.attempts}</td>
                      <td style={{ fontSize: 10, color: 'var(--muted-foreground)' }}>
                        {x.methods.join(',') || '-'}{x.statuses.length > 0 && <span> / {x.statuses.join(',')}</span>}
                      </td>
                      <td>
                        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 3 }}>
                          {x.reasons.map(r => {
                            const rl = REASON_LABEL[r] || { label: r, color: 'var(--muted-foreground)' }
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
          </div>
          {sessions.length > s.pageSize && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 10,
                          padding: '8px 0', flex: 'none', borderTop: '1px solid var(--border)' }}>
              <button className="btn btn--sm" disabled={s.page === 0} onClick={() => abnormal.setPage(s.page - 1)}>← 이전</button>
              <span style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>
                {s.page * s.pageSize + 1}–{Math.min((s.page + 1) * s.pageSize, sessions.length)} / {sessions.length}건
                (페이지 {s.page + 1}/{pageCount})
              </span>
              <button className="btn btn--sm" disabled={s.page >= pageCount - 1} onClick={() => abnormal.setPage(s.page + 1)}>다음 →</button>
              <select value={s.pageSize} style={{ fontSize: 12, padding: '2px 4px' }}
                      onChange={e => abnormal.setPageSize(Number(e.target.value))}>
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
  const color = tone === 'warn' ? 'var(--destructive)' : tone === 'ok' ? 'var(--cims-success)' : 'var(--foreground)'
  return (
    <div className="panel" style={{ padding: 10, display: 'flex', flexDirection: 'column' }}>
      <div style={{ flex: '1 1 auto', minHeight: 0, display: 'flex', flexDirection: 'column',
                    justifyContent: 'center', alignItems: 'center', textAlign: 'center' }}>
        <div style={{ fontSize: 12, color: 'var(--muted-foreground)', marginBottom: 4 }}>{label}</div>
        <div style={{ fontSize: 24, fontWeight: 700, lineHeight: 1.1, color }}>
          {value}
          {unit && <span style={{ fontSize: 12, color: 'var(--muted-foreground)', marginLeft: 2 }}>{unit}</span>}
        </div>
      </div>
    </div>
  )
}
