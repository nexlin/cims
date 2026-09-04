import { useState, useEffect, useCallback } from 'react'
import { statsApi, type MessagesResponse, type ServiceStatsResponse,
         type CallsResponse, type CallCell } from '@core/api/stats'
import { useToast } from '@core/components/Toast'

type SubTab = 'messages' | 'service'
type Granularity = '1m' | '5m' | '10m' | '1h' | '1d' | '1w' | '1M' | '1y'
type SvcType = 'volte' | 'ptt'

// 서버 services/stats_rollup.GRANULARITIES 와 같은 목록이어야 한다 — 한쪽만 늘리면
// 화면이 서버가 모르는 단위를 보내고 조용히 옛 경로로 폴백한다.
const GRAN_LABELS: Record<Granularity, string> = {
  '1m': '1분', '5m': '5분', '10m': '10분', '1h': '1시간',
  '1d': '1일', '1w': '1주', '1M': '1월', '1y': '1년'
}

 
function BarChart({ data, labelKey, valueKey, maxH = 160 }: {
  data: Array<any>; labelKey: string; valueKey: string; maxH?: number
}) {
  // 시간(hour) 축은 0~23 연속으로 채움 — API 가 데이터 있는 버킷만 주면
  // 막대 2~3개가 축 맥락 없이 떠 보이는 문제 방지.
  if (labelKey === 'hour' && data.length > 0 && data.length < 24) {
    const byHour = new Map(data.map(d => [Number(d.hour), d]))
    data = Array.from({ length: 24 }, (_, h) => byHour.get(h) ?? { hour: h, [valueKey]: 0 })
  }
  const vals = data.map(d => Number(d[valueKey]) || 0)
  const max = Math.max(...vals, 1)

  if (data.length === 0 || vals.every(v => v === 0)) {
    return (
      <div style={{ height: maxH, display: 'flex', alignItems: 'center', justifyContent: 'center',
                    color: 'var(--text-muted)', fontSize: 13, background: 'var(--surface-2)',
                    borderRadius: 6 }}>
        해당 기간 데이터 없음
      </div>
    )
  }

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

// sub — 비율 카드의 분자/분모. 비율만 보여주면 "3건 중 2건" 인지 "3만건 중 2만건" 인지
// 구분되지 않아 같은 66.7% 를 같은 무게로 읽게 된다.
function KpiCard({ label, value, unit, sub }: {
  label: string; value: string | number; unit?: string; sub?: string
}) {
  return (
    <div style={{ flex: '1 1 140px', minWidth: 140, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '14px 16px', textAlign: 'center' }}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 24, fontWeight: 700 }}>{value}<span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 2 }}>{unit}</span></div>
      {sub && <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 4 }}>{sub}</div>}
    </div>
  )
}

/**
 * 호 지표 카드 묶음. 비율마다 분자/분모를 함께 적는다 — 같은 66.7% 라도 "3건 중 2건"과
 * "3만건 중 2만건"은 다른 정보다.
 *
 * VoLTE 와 PTT 가 내는 지표가 다르다. PTT 는 **실패한 그룹통화 시도가 원천에 없어서**
 * 성공률을 낼 수 없다(sip_statistics.md §8 Y6) — 세션 기록이 곧 성립이라 세면 항상 100%
 * 가 된다. 그래서 PTT 에는 성공률·완료율 자리를 비우고 소통률과 참여율만 낸다.
 */
function CallKpis({ cell, source, kind }: {
  cell?: CallCell
  source?: string
  kind: 'volte' | 'ptt'
}) {
  if (!cell) return null
  const scan = source === 'scan'
  return (
    <div>
      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
        {kind === 'volte' ? (
          <>
            <KpiCard label="호 시도" value={cell.attempts} unit="건" />
            <KpiCard label="성공" value={cell.sessions} unit="건" />
            <KpiCard label="성공률" value={cell.success_rate} unit="%"
                     sub={`성립 ${cell.sessions} / 시도 ${cell.attempts}`} />
            <KpiCard label="소통률" value={cell.talk_rate} unit="%"
                     sub={`통화 ${cell.talked} / 시도 ${cell.attempts}`} />
            <KpiCard label="완료율" value={cell.completion_rate} unit="%"
                     sub={`정상종료 ${cell.completed} / 성립 ${cell.sessions}`} />
          </>
        ) : (
          <>
            <KpiCard label="세션" value={cell.sessions} unit="건" />
            <KpiCard label="소통률" value={cell.talk_rate_sessions} unit="%"
                     sub={`발언있음 ${cell.talked} / 세션 ${cell.sessions}`} />
            <KpiCard label="참여율" value={cell.join_rate} unit="%"
                     sub={`참여 ${cell.legs_joined} / 초대 ${cell.legs_invited}`} />
          </>
        )}
        <KpiCard label={kind === 'ptt' ? '평균 세션 시간' : '평균 통화시간'}
                 value={fmtDuration(cell.avg_duration_sec)} />
        {kind === 'volte' && <KpiCard label="평균 접속지연" value={cell.avg_pdd_ms} unit="ms" />}
      </div>
      {(scan || cell.open > 0 || cell.late_dropped > 0) && (
        <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 6 }}>
          {scan && <span>집계 없는 구간 — 원본에서 즉석 계산했습니다. </span>}
          {cell.open > 0 && <span>진행 중 {cell.open}건(종료 후 값이 갱신됩니다). </span>}
          {cell.late_dropped > 0 && <span>보존기간 초과로 되짚지 못한 호 {cell.late_dropped}건. </span>}
        </div>
      )}
    </div>
  )
}

function fmtDuration(sec: number): string {
  const m = Math.floor(sec / 60)
  const s = Math.round(sec % 60)
  return `${m}:${String(s).padStart(2, '0')}`
}

export default function StatsPage({ initialSvcType }: { initialSvcType?: SvcType } = {}) {
  const { show } = useToast()
  const [subTab, setSubTab] = useState<SubTab>('service')
  const [gran, setGran] = useState<Granularity>('1h')
  const [date, setDate] = useState(new Date().toISOString().slice(0, 10))
  const [svcType, setSvcType] = useState<SvcType>(initialSvcType ?? 'volte')

  // 메시지 통계
  const [msgData, setMsgData] = useState<MessagesResponse | null>(null)
  // 서비스 통계
  const [svcData, setSvcData] = useState<ServiceStatsResponse | null>(null)
  const [callsData, setCallsData] = useState<CallsResponse | null>(null)
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
      // 호 지표(성공률·소통률·완료율·참여율)는 1분 집계 위의 /stats/calls 가 낸다.
      // /stats/service/* 는 그룹별 빈도처럼 집계에 없는 축만 담당한다.
      const [res, calls] = await Promise.all([
        statsApi.service(svcType, { date, granularity: gran }),
        statsApi.calls({ date, granularity: gran, svc: svcType }),
      ])
      setSvcData(res)
      setCallsData(calls)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [date, gran, svcType, show])

  useEffect(() => {
    if (subTab === 'messages') loadMessages()
    else loadService()
  }, [subTab, loadMessages, loadService])

  return (
    <div>

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

      {/* 재조회 중에도 기존 데이터 유지 — 전체가 '로딩 중' 으로 갈리는 레이아웃 점프 방지 */}
      {loading && !msgData && !svcData && <div className="empty">로딩 중...</div>}
      {loading && (msgData || svcData) && (
        <div style={{ fontSize: 12, color: 'var(--text-muted)', padding: '2px 4px' }}>↻ 갱신 중…</div>
      )}

      {/* 메시지 통계 */}
      {subTab === 'messages' && msgData && (
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
      {subTab === 'service' && svcData?.volte && (
        <>
          <CallKpis cell={callsData?.totals?.volte} source={callsData?.source} kind="volte" />

          <div className="panel" style={{ padding: 16 }}>
            <div style={{ fontWeight: 600, marginBottom: 12 }}>호 시도 수 추이</div>
            <BarChart
              data={svcData.volte.buckets}
              labelKey={svcData.volte.buckets[0]?.hour !== undefined ? 'hour' : 'date'}
              valueKey="attempts" />
          </div>

          {Object.keys(svcData.volte.end_reasons).length > 0 && (
            <div className="panel" style={{ padding: 16 }}>
              <div style={{ fontWeight: 600, marginBottom: 12 }}>종료 사유 분포</div>
              {Object.entries(svcData.volte.end_reasons).sort((a, b) => b[1] - a[1]).map(([reason, cnt]) => {
                const pct = svcData.volte!.total_attempts > 0
                  ? Math.round(cnt / svcData.volte!.total_attempts * 100) : 0
                return (
                  <div key={reason} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                    <div style={{ width: 80, fontSize: 13 }}>{reason || 'unknown'}</div>
                    <div style={{ flex: 1, background: 'var(--surface-2)', borderRadius: 4, height: 18 }}>
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
      {subTab === 'service' && svcData?.ptt && (
        <>
          <CallKpis cell={callsData?.totals?.ptt} source={callsData?.source} kind="ptt" />

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
                  <div style={{ flex: 1, background: 'var(--surface-2)', borderRadius: 4, height: 18 }}>
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
