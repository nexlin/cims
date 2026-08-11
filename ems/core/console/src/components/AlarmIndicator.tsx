// 헤더 상시 알람 인디케이터 + 드로어 + 전이 토스트 (alarm_pipeline.md §8.2).
//   - 배지: 활성 요약(최고 severity 색 + 건수). 0건이어도 회색 배지를 상시 표시한다 —
//     "표시 없음 = 정상"과 "표시 없음 = 표시 고장"을 구분(observability_lost 철학).
//     폴링 실패 시 ! 표기.
//   - 드로어: 활성 알람 목록(승인/이동) + 최근 이벤트 탭. 어느 라우트에서든 상주.
//   - 토스트: critical/major open·moreSevere 승격만 수동 닫기 토스트 — minor 이하/close 는
//     배지 갱신만, 이벤트는 토스트 없음 (§8.2 소음 통제).
import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { alertsApi } from '../api/alerts'
import { SEV_COLOR, onAlarmTransition, refreshAlarms, severityOf, useAlarms } from '../widgets/useAlarms'
import { useToast } from './Toast'

const SEV_BADGE: Record<string, string> = {
  critical: 'badge--red', major: 'badge--red', minor: 'badge--yellow',
  warning: 'badge--yellow', indeterminate: 'badge--blue',
}

// 전이 토스트 배선 — 셸에 1개만 렌더 (AlarmIndicator 내부에서 함께 처리).
function useAlarmToasts() {
  const { show } = useToast()
  const navigate = useNavigate()
  useEffect(() => onAlarmTransition(ts => {
    for (const t of ts) {
      const sev = severityOf(t.alarm)
      const head = t.kind === 'moreSevere' ? `알람 승격(${sev})` : `알람 발생(${sev})`
      show(`${head} — ${t.alarm.message || t.alarm.type}`, 'alarm',
           { sticky: true, onClick: () => navigate('/alerts/active') })
    }
  }), [show, navigate])
}

export default function AlarmIndicator() {
  const { active, recentEvents, loaded, error } = useAlarms()
  const [open, setOpen] = useState(false)
  const [tab, setTab] = useState<'alarms' | 'events'>('alarms')
  const navigate = useNavigate()
  const { show } = useToast()
  useAlarmToasts()

  const top = active[0] ? severityOf(active[0]) : ''
  const dotColor = error ? 'var(--danger)' : (SEV_COLOR[top] || 'var(--text-muted)')

  const ack = async (alarmId?: string) => {
    if (!alarmId) return
    try {
      await alertsApi.ack(alarmId)
      show('알람 승인됨', 'ok')
      refreshAlarms()
    } catch (e) {
      show(`승인 실패: ${(e as Error).message}`, 'err')
    }
  }

  return (
    <>
      <button className="alarm-indicator" onClick={() => setOpen(o => !o)}
              title={error ? '알람 조회 실패 — 표시가 최신이 아닐 수 있음'
                           : loaded ? `활성 알람 ${active.length}건` : '알람 로드 중'}>
        <span className="dot" style={{ background: dotColor }} />
        알람 {loaded ? active.length : '…'}{error ? ' !' : ''}
      </button>
      {open && (
        <div className="alarm-drawer">
          <div className="tab-bar" style={{ padding: '8px 14px 0' }}>
            <button className={`tab-btn ${tab === 'alarms' ? 'tab-btn--active' : ''}`}
                    onClick={() => setTab('alarms')}>활성 알람 ({active.length})</button>
            <button className={`tab-btn ${tab === 'events' ? 'tab-btn--active' : ''}`}
                    onClick={() => setTab('events')}>최근 이벤트 ({recentEvents.length})</button>
            <button className="btn btn--ghost btn--sm" style={{ marginLeft: 'auto' }}
                    onClick={() => setOpen(false)}>✕</button>
          </div>
          <div className="alarm-drawer-body">
            {tab === 'alarms' && active.length === 0 && (
              <div className="empty" style={{ padding: 20 }}>활성 알람 없음</div>
            )}
            {tab === 'alarms' && active.map(a => {
              const sev = severityOf(a)
              return (
                <div key={a.alarm_id || a.type} className="alarm-drawer-row">
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span className={`badge ${SEV_BADGE[sev] || 'badge--gray'}`}>{sev}</span>
                    <span style={{ fontFamily: 'monospace', fontSize: 11 }}>{a.code}</span>
                    {(a.occurrences || 1) > 1 && (
                      <span className="badge badge--gray">×{a.occurrences}</span>
                    )}
                    <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--text-muted)' }}>{a.ts}</span>
                  </div>
                  <div style={{ marginTop: 3 }}>{a.message}</div>
                  <div style={{ marginTop: 4, display: 'flex', gap: 8, alignItems: 'center' }}>
                    <span style={{ fontSize: 11, color: 'var(--text-muted)', fontFamily: 'monospace' }}>
                      {a.source?.mo_instance}
                    </span>
                    <span style={{ marginLeft: 'auto' }} />
                    {a.acked
                      ? <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>✓ {a.ackUser || '승인'}</span>
                      : <button className="btn btn--ghost btn--sm" onClick={() => ack(a.alarm_id)}>승인</button>}
                    <button className="btn btn--ghost btn--sm"
                            onClick={() => { setOpen(false); navigate('/alerts/active') }}>이동</button>
                  </div>
                </div>
              )
            })}
            {tab === 'events' && recentEvents.length === 0 && (
              <div className="empty" style={{ padding: 20 }}>최근 24시간 이벤트 없음</div>
            )}
            {tab === 'events' && recentEvents.map((ev, i) => (
              <div key={i} className="alarm-drawer-row">
                <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                  <span className="badge badge--gray">{ev.kind}</span>
                  <span style={{ fontSize: 12 }}>{ev.type}</span>
                  <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--text-muted)' }}>{ev.ts}</span>
                </div>
                <div style={{ marginTop: 3, fontSize: 12 }}>{ev.message}</div>
              </div>
            ))}
          </div>
        </div>
      )}
    </>
  )
}
