// 알람 카탈로그 — 코드 사전 (alarm_pipeline.md §8.1).
//   GET /alerts/catalog 소비: OAM 평가 규칙(origin=rule) + 모듈 자기보고 등록분
//   (origin=module:*, fm_catalog 보존본이라 모듈 다운 중에도 표시). 운영 사전(dictionary) —
//   code·type·severity·effect·recommended_action 열람 (vIBCF POD 의 화면 대응물).
import { useEffect, useMemo, useState } from 'react'
import { alertsApi, type AlarmCatalogItem } from '../api/alerts'

const SEV_BADGE: Record<string, string> = {
  critical: 'badge--red', major: 'badge--red', minor: 'badge--yellow',
  warning: 'badge--yellow', indeterminate: 'badge--blue',
}

export default function AlarmCatalogPage() {
  const [items, setItems] = useState<AlarmCatalogItem[]>([])
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState('')
  const [q, setQ] = useState('')

  useEffect(() => {
    alertsApi.catalog()
      .then(r => { setItems(r.catalog); setLoaded(true) })
      .catch(e => { setError((e as Error).message); setLoaded(true) })
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
                  <td>{c.type}</td>
                  <td>{sev ? <span className={`badge ${SEV_BADGE[sev] || 'badge--gray'}`}>{sev}</span> : '—'}</td>
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
  )
}
