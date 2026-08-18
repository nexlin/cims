// 알람 카탈로그 — 코드 사전 + 활성 평가 규칙 (alarm_pipeline.md §8.1).
//   사전: GET /alerts/catalog — OAM 평가 규칙(origin=rule) + 모듈 자기보고 등록분
//   (origin=module:*, fm_catalog 보존본이라 모듈 다운 중에도 표시). 운영 dictionary —
//   code·type·severity·effect·recommended_action 열람 (vIBCF POD 의 화면 대응물).
//   규칙: GET /alerts/rules — 스위퍼가 실제 평가 중인 조건(대상·임계·주기). 이력 화면이
//   아니라 정의 화면의 관심사라 여기에 둔다.
import { useEffect, useMemo, useState } from 'react'
import { alertsApi, type AlarmCatalogItem, type AlertRulesResponse } from '../api/alerts'
import { alarmTypeLabel, sevBadgeClass, severityOf } from '../utils/alarmLabels'

export default function AlarmCatalogPage() {
  const [items, setItems] = useState<AlarmCatalogItem[]>([])
  const [rules, setRules] = useState<AlertRulesResponse | null>(null)
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState('')
  const [q, setQ] = useState('')

  useEffect(() => {
    alertsApi.catalog()
      .then(r => { setItems(r.catalog); setLoaded(true) })
      .catch(e => { setError((e as Error).message); setLoaded(true) })
    // 규칙은 부가 정보 — 실패해도 사전 표시엔 영향 없음.
    alertsApi.rules().then(setRules).catch(() => setRules(null))
  }, [])

  const filtered = useMemo(() => {
    const needle = q.trim().toLowerCase()
    const rows = needle
      ? items.filter(c => [c.code, c.type, c.metric, c.effect, c.recommended_action, c.origin]
          .some(v => (v || '').toLowerCase().includes(needle)))
      : items
    return [...rows].sort((a, b) => (a.code || '').localeCompare(b.code || ''))
  }, [items, q])

  return (
    <div className="page">
      <div className="panel" style={{ padding: 16 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 12 }}>
          <div style={{ fontWeight: 600, fontSize: 14 }}>알람 카탈로그 ({filtered.length})</div>
          <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
            정의 코드 사전 — OAM 평가 규칙 + 모듈 자기보고 등록분
          </span>
          <input className="form-input" style={{ marginLeft: 'auto', width: 240 }}
                 placeholder="코드/클래스/조치 검색" value={q} onChange={e => setQ(e.target.value)} />
        </div>
        {!loaded ? (
          <div className="empty">로딩 중…</div>
        ) : error ? (
          <div className="empty" style={{ color: 'var(--danger)' }}>조회 실패: {error}</div>
        ) : filtered.length === 0 ? (
          <div className="empty">항목 없음</div>
        ) : (
          <table className="data-table" style={{ fontSize: 13 }}>
            <thead>
              <tr>
                <th style={{ width: 110 }}>code</th>
                <th style={{ width: 150 }}>클래스(type)</th>
                <th style={{ width: 90 }}>severity</th>
                <th style={{ width: 130 }}>eventType</th>
                <th>영향 (effect)</th>
                <th>권장 조치</th>
                <th style={{ width: 110 }}>출처</th>
              </tr>
            </thead>
            <tbody>
              {filtered.map(c => {
                const sev = c.perceived_severity || ''
                return (
                  <tr key={c.code} title={c.probable_cause ? `probableCause: ${c.probable_cause}` : undefined}>
                    <td style={{ fontFamily: 'monospace', fontSize: 12 }}>{c.code}</td>
                    <td title={c.type}>{alarmTypeLabel(c.type)}</td>
                    <td>{sev ? <span className={`badge ${sevBadgeClass(sev)}`}>{sev}</span> : '—'}</td>
                    <td style={{ fontSize: 12 }}>{c.event_type || '—'}</td>
                    <td style={{ fontSize: 12 }}>{c.effect || '—'}</td>
                    <td style={{ fontSize: 12 }}>{c.recommended_action || '—'}</td>
                    <td style={{ fontSize: 11, color: 'var(--text-muted)' }}>{c.origin}</td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        )}
      </div>

      {rules && rules.rules.length > 0 && (
        <div className="panel">
          <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)',
                        display: 'flex', alignItems: 'center', gap: 8 }}>
            활성 평가 규칙 ({rules.rules.length})
            <span style={{ fontSize: 11, fontWeight: 400, color: 'var(--text-muted)' }}>
              점검 주기 {rules.sweep_sec}초 · {rules.editable ? '편집 가능' : '읽기 전용 (oam.json 설정 기반)'}
            </span>
          </div>
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 90 }}>심각도</th>
                <th style={{ width: 110 }}>코드</th>
                <th style={{ width: 130 }}>클래스</th>
                <th style={{ width: 90 }}>대상</th>
                <th>지표</th>
                <th style={{ width: 200 }}>발생 조건</th>
              </tr>
            </thead>
            <tbody>
              {rules.rules.map((r, i) => (
                <tr key={`${r.code}-${r.target || r.mo_instance || r.scope}-${i}`}
                    title={[r.effect && `영향: ${r.effect}`, r.recommended_action && `조치: ${r.recommended_action}`].filter(Boolean).join('\n')}>
                  <td><span className={`badge ${sevBadgeClass(severityOf(r))}`}>{severityOf(r)}</span></td>
                  <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{r.code || '-'}</td>
                  <td>{alarmTypeLabel(r.type)}</td>
                  <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{r.target || r.scope || '-'}</td>
                  <td>
                    {r.metric}
                    {r.mo_instance && <code style={{ marginLeft: 6, fontSize: 11, color: 'var(--text-muted)' }}>{r.mo_instance}</code>}
                  </td>
                  <td style={{ fontFamily: 'monospace', fontSize: 12 }}>
                    {r.condition}
                    {r.threshold != null && (
                      <span style={{ marginLeft: 6, color: 'var(--text-muted)', fontFamily: 'inherit' }}>
                        (threshold {r.threshold}{r.unit || ''})
                      </span>
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
