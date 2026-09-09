import { useState, useEffect, useCallback } from 'react'
import { statsApi, type MessagesResponse, type ServiceStatsResponse,
 type CallsResponse, type CallCell } from '@core/api/stats'
import { useToast } from '@core/components/Toast'
import { RotateCw } from 'lucide-react'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { fromSel, toSel } from '@core/components/custom/select-value'

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
 color: 'var(--muted-foreground)', fontSize: 13, background: 'var(--secondary)',
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
          <div className="flex-1 flex flex-col items-center" key={i}>
            <div className="text-xs text-muted-foreground mb-0.5">{v > 0 ? v : ''}</div>
            <div style={{ width: '100%', maxWidth: 32, height: h, background: 'var(--primary)', borderRadius: '2px 2px 0 0' }} />
            <div className="text-xs text-muted-foreground mt-0.5">{String(d[labelKey])}</div>
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
    <div className="flex-[1_1_140px] min-w-[140px] bg-card border border-border rounded-md py-3.5 px-4 text-center">
      <div className="text-sm text-muted-foreground mb-1">{label}</div>
      <div className="text-3xl font-bold">{value}<span className="text-sm text-muted-foreground ml-0.5">{unit}</span></div>
      {sub && <div className="text-xs text-muted-foreground mt-1">{sub}</div>}
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
      <div className="flex gap-3 flex-wrap">
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
        <div className="text-xs text-muted-foreground mt-1.5">
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
      <div className="toolbar flex items-center gap-2.5 border-b border-border bg-muted px-4 py-3 flex-wrap">
        <ToggleGroup type="single" value={subTab} className="shrink-0 justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setSubTab(v as typeof subTab)}>
          <ToggleGroupItem value="service">서비스 통계</ToggleGroupItem>
          <ToggleGroupItem value="messages">메시지 통계</ToggleGroupItem>
        </ToggleGroup>

        <div className="w-[1px] h-[24px] bg-border my-0 mx-2"/>

        {/* 시간 단위 */}
        <ToggleGroup type="single" value={gran} className="shrink-0 justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setGran(v as Granularity)}>
          {(Object.entries(GRAN_LABELS) as [Granularity, string][]).map(([g, label]) => (
            <ToggleGroupItem key={g} value={g}>{label}</ToggleGroupItem>
          ))}
        </ToggleGroup>

        <div className="w-[1px] h-[24px] bg-border my-0 mx-2"/>

        <Input className="w-[150px]" type="date" value={date}
 onChange={e => setDate(e.target.value)}/>

        {subTab === 'service' && (
          <Select value={toSel(svcType)} onValueChange={(v: string) => setSvcType(fromSel(v) as SvcType)}>
            <SelectTrigger className="w-[100px]"><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value="volte">VoIP</SelectItem>
              <SelectItem value="ptt">PTT</SelectItem>
            </SelectContent>
          </Select>
        )}
      </div>

      {/* 재조회 중에도 기존 데이터 유지 — 전체가 '로딩 중' 으로 갈리는 레이아웃 점프 방지 */}
      {loading && !msgData && !svcData && <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div>}
      {loading && (msgData || svcData) && (
        <div className="text-sm text-muted-foreground py-0.5 px-1"><RotateCw size={12} style={{ verticalAlign: '-2px' }} /> 갱신 중…</div>
      )}

      {/* 메시지 통계 */}
      {subTab === 'messages' && msgData && (
        <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-4">
          <div className="font-semibold mb-3">메시지 통계 — {msgData.date}</div>
          <BarChart data={msgData.buckets} labelKey="hour" valueKey="total" />
          <div className="flex gap-4 mt-3 text-md">
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

          <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-4">
            <div className="font-semibold mb-3">호 시도 수 추이</div>
            <BarChart
 data={svcData.volte.buckets}
 labelKey={svcData.volte.buckets[0]?.hour !== undefined ? 'hour' : 'date'}
 valueKey="attempts" />
          </div>

          {Object.keys(svcData.volte.end_reasons).length > 0 && (
            <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-4">
              <div className="font-semibold mb-3">종료 사유 분포</div>
              {Object.entries(svcData.volte.end_reasons).sort((a, b) => b[1] - a[1]).map(([reason, cnt]) => {
 const pct = svcData.volte!.total_attempts > 0
                  ? Math.round(cnt / svcData.volte!.total_attempts * 100) : 0
 return (
                  <div className="flex items-center gap-2 mb-1.5" key={reason}>
                    <div className="w-[80px] text-md">{reason || 'unknown'}</div>
                    <div className="flex-1 bg-secondary rounded-sm h-[18px]">
                      <div style={{ width: `${pct}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: pct > 0 ? 4 : 0 }} />
                    </div>
                    <div className="w-[60px] text-sm text-right text-muted-foreground">{cnt}건 ({pct}%)</div>
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

          <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-4">
            <div className="font-semibold mb-3">그룹콜 수 추이</div>
            <BarChart
 data={svcData.ptt.buckets}
 labelKey={svcData.ptt.buckets[0]?.hour !== undefined ? 'hour' : 'date'}
 valueKey="calls" />
          </div>

          {Object.keys(svcData.ptt.by_group).length > 0 && (
            <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-4">
              <div className="font-semibold mb-3">그룹별 사용 빈도</div>
              {Object.entries(svcData.ptt.by_group).sort((a, b) => b[1] - a[1]).map(([gid, cnt]) => (
                <div className="flex items-center gap-2 mb-1.5" key={gid}>
                  <div className="w-[80px] text-md">그룹 {gid}</div>
                  <div className="flex-1 bg-secondary rounded-sm h-[18px]">
                    <div style={{ width: `${Math.round(cnt / svcData.ptt!.total_calls * 100)}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: 4 }} />
                  </div>
                  <div className="w-[50px] text-sm text-right text-muted-foreground">{cnt}건</div>
                </div>
              ))}
            </div>
          )}
        </>
      )}
    </div>
  )
}
