// 활성 알람 — 전역 알람 store(useAlarms, SSE 라이브) 구독 뷰 (alarm_pipeline.md §8.2).
//   이력 페이지(AlertsPage)와 분리: 여기는 "지금 열린 알람" 만 — 심각도 타일 + 목록 +
//   승인/코멘트 조작. 데이터는 store 하나(대시보드 위젯·헤더 배지와 동일 fold)라 표시 일관.
import { useMemo, useState } from 'react'
import { alertsApi } from '../api/alerts'
import { useToast } from '../components/Toast'
import { useAlarms, refreshAlarms, severityOf, type ActiveAlarm } from '../widgets/useAlarms'
import {
  alarmTypeLabel, sevBadgeClass, fmtTime, formatSec, SEVERITY_ORDER,
} from '../utils/alarmLabels'

const SEV_TILE_LABEL: Record<string, string> = {
  critical: 'Critical', major: 'Major', minor: 'Minor', warning: 'Warning', indeterminate: 'Indeterminate',
}

function elapsedSince(ts?: string): string {
  const t = new Date(ts || '').getTime()
  if (isNaN(t)) return '-'
  return formatSec(Math.max(0, Math.round((Date.now() - t) / 1000)))
}

function AlarmDetail({ a, onAck, onComment }: {
  a: ActiveAlarm
  onAck: (id?: string) => void
  onComment: (id: string | undefined, text: string) => void
}) {
  const [text, setText] = useState('')
  const item = (label: string, value?: string | null) => value ? (
    <div style={{ display: 'flex', gap: 8, fontSize: 12 }}>
      <span style={{ color: 'var(--text-muted)', minWidth: 90, flexShrink: 0 }}>{label}</span>
      <span>{value}</span>
    </div>
  ) : null
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6, padding: '10px 16px 12px' }}>
      {item('alarm_id', a.alarm_id)}
      {item('eventType', a.event_type)}
      {item('probableCause', a.probable_cause)}
      {item('영향', a.effect)}
      {item('권장 조치', a.recommended_action)}
      {a.threshold_info && item('관측값', `${a.threshold_info.observed}${a.threshold_info.unit || ''} (임계 ${a.threshold_info.threshold}${a.threshold_info.unit || ''})`)}
      {(a.occurrences ?? 1) > 1 && item('재통지', `해제 없이 ${a.occurrences}회 — 최근 ${fmtTime(a.last_open_ts)}`)}
      {a.acked && item('승인', `${a.ackUser || ''}`)}
      {(a.comments?.length ?? 0) > 0 && (
        <div style={{ fontSize: 12 }}>
          <div style={{ color: 'var(--text-muted)', marginBottom: 2 }}>코멘트</div>
          {a.comments!.map((c, i) => (
            <div key={i} style={{ padding: '2px 0 2px 8px', borderLeft: '2px solid var(--border)' }}>
              <span style={{ color: 'var(--text-muted)' }}>{c.user || ''} {fmtTime(c.ts)}</span> — {c.text}
            </div>
          ))}
        </div>
      )}
      <div style={{ display: 'flex', gap: 6, alignItems: 'center', marginTop: 2 }}>
        {!a.acked && (
          <button className="btn btn--sm btn--outline" disabled={!a.alarm_id} onClick={() => onAck(a.alarm_id)}>승인</button>
        )}
        <input className="form-input" style={{ width: 280 }} placeholder="코멘트 입력 후 Enter"
               value={text} onChange={e => setText(e.target.value)}
               onKeyDown={e => {
                 if (e.key === 'Enter' && text.trim()) { onComment(a.alarm_id, text.trim()); setText('') }
               }} />
      </div>
    </div>
  )
}

export default function ActiveAlarmsPage() {
  const { show } = useToast()
  const { active, loaded, error, lastUpdated } = useAlarms()
  const [sevFilter, setSevFilter] = useState('')
  const [q, setQ] = useState('')
  const [expanded, setExpanded] = useState<string | null>(null)

  const counts = useMemo(() => {
    const c: Record<string, number> = {}
    for (const a of active) c[severityOf(a)] = (c[severityOf(a)] || 0) + 1
    return c
  }, [active])

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase()
    return active.filter(a => {
      if (sevFilter && severityOf(a) !== sevFilter) return false
      if (needle && ![a.code, a.type, a.message, a.source?.mo_instance]
        .some(v => (v || '').toLowerCase().includes(needle))) return false
      return true
    })
  }, [active, sevFilter, q])

  const ack = async (alarmId?: string) => {
    if (!alarmId) return
    try { await alertsApi.ack(alarmId); show('알람 승인됨', 'ok'); refreshAlarms() }
    catch (e) { show((e as Error).message, 'err') }
  }
  const comment = async (alarmId: string | undefined, text: string) => {
    if (!alarmId) return
    try { await alertsApi.comment(alarmId, text); show('코멘트 기록됨', 'ok'); refreshAlarms() }
    catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="page">
      {/* 심각도 타일 — 클릭 = 해당 단계 필터 토글 */}
      <div style={{ display: 'flex', gap: 12 }}>
        {SEVERITY_ORDER.map(sev => {
          const n = counts[sev] || 0
          const on = sevFilter === sev
          return (
            <button key={sev} onClick={() => setSevFilter(on ? '' : sev)}
              style={{
                flex: 1, textAlign: 'left', cursor: 'pointer',
                background: 'var(--surface)', borderRadius: 'var(--radius)', padding: '12px 16px',
                border: on ? '1px solid var(--primary)' : '1px solid var(--border)',
              }}>
              <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>{SEV_TILE_LABEL[sev]}</div>
              <div style={{ fontSize: 24, fontWeight: 700,
                            color: n > 0 && (sev === 'critical' || sev === 'major') ? 'var(--danger)' : 'var(--text)' }}>
                {n}
              </div>
            </button>
          )
        })}
      </div>

      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <input className="search-input" style={{ width: 260 }} placeholder="코드/소스/메시지 검색"
               value={q} onChange={e => setQ(e.target.value)} />
        <span style={{ marginLeft: 'auto', fontSize: 12, color: error ? 'var(--danger)' : 'var(--text-muted)' }}>
          {error ? '갱신 실패 — 표시가 최신이 아닐 수 있음' : lastUpdated ? `갱신 ${fmtTime(new Date(lastUpdated).toISOString())} · 라이브` : ''}
        </span>
        <button className="btn btn--ghost btn--sm" onClick={refreshAlarms}>↻</button>
      </div>

      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          활성 알람 ({rows.length}건{sevFilter || q ? ` / 전체 ${active.length}` : ''})
        </div>
        {!loaded ? (
          <div className="empty">로딩 중…</div>
        ) : rows.length === 0 ? (
          <div className="empty">{active.length === 0 ? '활성 알람 없음' : '필터 결과 없음'}</div>
        ) : (
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 90 }}>심각도</th>
                <th style={{ width: 100 }}>코드</th>
                <th style={{ width: 120 }}>클래스</th>
                <th style={{ width: 170 }}>소스</th>
                <th>메시지</th>
                <th style={{ width: 145 }}>발생 시각</th>
                <th style={{ width: 100 }}>경과</th>
                <th style={{ width: 90 }}>승인</th>
              </tr>
            </thead>
            <tbody>
              {rows.map(a => {
                const key = a.alarm_id || `${a.ts}-${a.type}`
                const open = expanded === key
                return [
                  <tr key={key} onClick={() => setExpanded(open ? null : key)}
                      style={{ cursor: 'pointer', background: open ? 'var(--hover)' : undefined }}>
                    <td><span className={`badge ${sevBadgeClass(severityOf(a))}`}>{severityOf(a)}</span></td>
                    <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{a.code || '-'}</td>
                    <td>{alarmTypeLabel(a.type)}</td>
                    <td><code style={{ fontSize: 11 }}>{a.source?.mo_instance || '-'}</code></td>
                    <td>
                      {a.message}
                      {(a.occurrences ?? 1) > 1 && (
                        <span style={{ marginLeft: 6, fontSize: 10, fontWeight: 600, color: 'var(--text-muted)',
                                       border: '1px solid var(--border)', borderRadius: 3, padding: '0 3px' }}>
                          ×{a.occurrences}
                        </span>
                      )}
                      {(a.comments?.length ?? 0) > 0 && (
                        <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--text-muted)' }}>💬{a.comments!.length}</span>
                      )}
                    </td>
                    <td className="ts">{fmtTime(a.ts)}</td>
                    <td>{elapsedSince(a.ts)}</td>
                    <td>
                      {a.acked
                        ? <span style={{ fontSize: 11, color: 'var(--success, #16a34a)' }}>✓ {a.ackUser || '승인'}</span>
                        : <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>미승인</span>}
                    </td>
                  </tr>,
                  open && (
                    <tr key={`${key}-detail`}>
                      <td colSpan={8} style={{ padding: 0, background: 'var(--hover)' }}>
                        <AlarmDetail a={a} onAck={ack} onComment={comment} />
                      </td>
                    </tr>
                  ),
                ]
              })}
            </tbody>
          </table>
        )}
      </div>
    </div>
  )
}
