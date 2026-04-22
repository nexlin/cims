import { useState, useEffect, useCallback } from 'react'
import { statsApi, type MessagesResponse, type ServiceStatsResponse } from '../api/stats'
import { useToast } from '../components/Toast'

type SubTab = 'messages' | 'service'
type Granularity = '5m' | '10m' | '1h' | '1d' | '1M' | '1y'
type SvcType = 'volte' | 'ptt'

const GRAN_LABELS: Record<Granularity, string> = {
  '5m': '5분', '10m': '10분', '1h': '1시간', '1d': '1일', '1M': '1월', '1y': '1년'
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function BarChart({ data, labelKey, valueKey, maxH = 160 }: {
  data: Array<any>; labelKey: string; valueKey: string; maxH?: number
}) {
  const vals = data.map(d => Number(d[valueKey]) || 0)
  const max = Math.max(...vals, 1)

  return (
    <div style={{ display: 'flex', alignItems: 'flex-end', gap: 2, height: maxH, padding: '0 4px' }}>
      {data.map((d, i) => {
        const v = vals[i]
        const h = Math.max(v / max * (maxH - 20), 2)
        return (
          <div key={i} style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
            <div style={{ fontSize: 10, color: 'var(--text-muted)', marginBottom: 2 }}>{v > 0 ? v : ''}</div>
            <div style={{ width: '100%', maxWidth: 32, height: h, background: 'var(--primary)', borderRadius: '2px 2px 0 0' }} />
            <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 2 }}>{String(d[labelKey])}</div>
          </div>
        )
      })}
    </div>
  )
}

function KpiCard({ label, value, unit }: { label: string; value: string | number; unit?: string }) {
  return (
    <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '14px 16px', textAlign: 'center' }}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 24, fontWeight: 700 }}>{value}<span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 2 }}>{unit}</span></div>
    </div>
  )
}

function fmtDuration(sec: number): string {
  const m = Math.floor(sec / 60)
  const s = Math.round(sec % 60)
  return `${m}:${String(s).padStart(2, '0')}`
}

export default function StatsPage() {
  const { show } = useToast()
  const [subTab, setSubTab] = useState<SubTab>('service')
  const [gran, setGran] = useState<Granularity>('1h')
  const [date, setDate] = useState(new Date().toISOString().slice(0, 10))
  const [svcType, setSvcType] = useState<SvcType>('volte')

  // 메시지 통계
  const [msgData, setMsgData] = useState<MessagesResponse | null>(null)
  // 서비스 통계
  const [svcData, setSvcData] = useState<ServiceStatsResponse | null>(null)
  const [loading, setLoading] = useState(false)

  const loadMessages = useCallback(async () => {
    setLoading(true)
    try {
      const res = await statsApi.messages({ date, granularity: gran })
      setMsgData(res)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [date, gran, show])

  const loadService = useCallback(async () => {
    setLoading(true)
    try {
      const res = await statsApi.service(svcType, { date, granularity: gran })
      setSvcData(res)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [date, gran, svcType, show])

  useEffect(() => {
    if (subTab === 'messages') loadMessages()
    else loadService()
  }, [subTab, loadMessages, loadService])

  return (
    <div className="page">

      {/* 서브탭 + 필터 */}
      <div className="toolbar" style={{ flexWrap: 'wrap' }}>
        <button className={`tab-btn${subTab === 'service' ? ' tab-btn--active' : ''}`}
          onClick={() => setSubTab('service')}>서비스 통계</button>
        <button className={`tab-btn${subTab === 'messages' ? ' tab-btn--active' : ''}`}
          onClick={() => setSubTab('messages')}>메시지 통계</button>

        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 8px' }} />

        {/* 시간 단위 */}
        {(Object.entries(GRAN_LABELS) as [Granularity, string][]).map(([g, label]) => (
          <button key={g}
            className={`btn btn--sm ${gran === g ? 'btn--primary' : 'btn--ghost'}`}
            onClick={() => setGran(g)}>
            {label}
          </button>
        ))}

        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 8px' }} />

        <input className="form-input" type="date" value={date}
          onChange={e => setDate(e.target.value)} style={{ width: 150 }} />

        {subTab === 'service' && (
          <select className="form-input" value={svcType} onChange={e => setSvcType(e.target.value as SvcType)} style={{ width: 100 }}>
            <option value="volte">VoIP</option>
            <option value="ptt">PTT</option>
          </select>
        )}
      </div>

      {loading && <div className="empty">로딩 중...</div>}

      {/* 메시지 통계 */}
      {subTab === 'messages' && msgData && !loading && (
        <div className="panel" style={{ padding: 16 }}>
          <div style={{ fontWeight: 600, marginBottom: 12 }}>메시지 통계 — {msgData.date}</div>
          <BarChart data={msgData.buckets} labelKey="hour" valueKey="total" />
          <div style={{ display: 'flex', gap: 16, marginTop: 12, fontSize: 13 }}>
            <span>VoIP INVITE: {msgData.buckets.reduce((s, b) => s + b.voip_invite, 0)}</span>
            <span>PTT INVITE: {msgData.buckets.reduce((s, b) => s + b.ptt_invite, 0)}</span>
            <span>합계: {msgData.buckets.reduce((s, b) => s + b.total, 0)}</span>
          </div>
        </div>
      )}

      {/* 서비스 통계 — VoIP */}
      {subTab === 'service' && svcData?.voip && !loading && (
        <>
          <div style={{ display: 'flex', gap: 12 }}>
            <KpiCard label="호 시도" value={svcData.voip.total_attempts} unit="건" />
            <KpiCard label="호 성공률" value={svcData.voip.success_rate} unit="%" />
            <KpiCard label="평균 통화시간" value={fmtDuration(svcData.voip.avg_duration_sec)} />
            <KpiCard label="성공" value={svcData.voip.total_success} unit="건" />
          </div>

          <div className="panel" style={{ padding: 16 }}>
            <div style={{ fontWeight: 600, marginBottom: 12 }}>호 시도 수 추이</div>
            <BarChart
              data={svcData.voip.buckets}
              labelKey={svcData.voip.buckets[0]?.hour !== undefined ? 'hour' : 'date'}
              valueKey="attempts" />
          </div>

          {Object.keys(svcData.voip.end_reasons).length > 0 && (
            <div className="panel" style={{ padding: 16 }}>
              <div style={{ fontWeight: 600, marginBottom: 12 }}>종료 사유 분포</div>
              {Object.entries(svcData.voip.end_reasons).sort((a, b) => b[1] - a[1]).map(([reason, cnt]) => {
                const pct = svcData.voip!.total_attempts > 0
                  ? Math.round(cnt / svcData.voip!.total_attempts * 100) : 0
                return (
                  <div key={reason} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                    <div style={{ width: 80, fontSize: 13 }}>{reason || 'unknown'}</div>
                    <div style={{ flex: 1, background: '#f3f4f6', borderRadius: 4, height: 18 }}>
                      <div style={{ width: `${pct}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: pct > 0 ? 4 : 0 }} />
                    </div>
                    <div style={{ width: 60, fontSize: 12, textAlign: 'right', color: 'var(--text-muted)' }}>{cnt}건 ({pct}%)</div>
                  </div>
                )
              })}
            </div>
          )}
        </>
      )}

      {/* 서비스 통계 — PTT */}
      {subTab === 'service' && svcData?.ptt && !loading && (
        <>
          <div style={{ display: 'flex', gap: 12 }}>
            <KpiCard label="그룹콜 수" value={svcData.ptt.total_calls} unit="건" />
            <KpiCard label="평균 세션 시간" value={fmtDuration(svcData.ptt.avg_duration_sec)} />
          </div>

          <div className="panel" style={{ padding: 16 }}>
            <div style={{ fontWeight: 600, marginBottom: 12 }}>그룹콜 수 추이</div>
            <BarChart
              data={svcData.ptt.buckets}
              labelKey={svcData.ptt.buckets[0]?.hour !== undefined ? 'hour' : 'date'}
              valueKey="calls" />
          </div>

          {Object.keys(svcData.ptt.by_group).length > 0 && (
            <div className="panel" style={{ padding: 16 }}>
              <div style={{ fontWeight: 600, marginBottom: 12 }}>그룹별 사용 빈도</div>
              {Object.entries(svcData.ptt.by_group).sort((a, b) => b[1] - a[1]).map(([gid, cnt]) => (
                <div key={gid} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                  <div style={{ width: 80, fontSize: 13 }}>그룹 {gid}</div>
                  <div style={{ flex: 1, background: '#f3f4f6', borderRadius: 4, height: 18 }}>
                    <div style={{ width: `${Math.round(cnt / svcData.ptt!.total_calls * 100)}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: 4 }} />
                  </div>
                  <div style={{ width: 50, fontSize: 12, textAlign: 'right', color: 'var(--text-muted)' }}>{cnt}건</div>
                </div>
              ))}
            </div>
          )}
        </>
      )}
    </div>
  )
}
