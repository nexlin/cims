// 비정상 세션 이력 — 공개 SIP 포트(VIP)로 들어오는 인터넷발 스캔/사기 호 시도.
//   CSP 는 인증(401)으로 정상 거부하지만, 로그 오염·자원 소모를 일으키므로 가시화한다.
//   탐지 신호: 외부(공인) 발신 IP · 알려진 스캐너 UA(pplsip 등) · 사기성 번호 · 인증 반복실패.
//
// **화면 = 카드 하나**(`cims.abnormal-sessions`)이고 안의 네 블록(조회 조건 · 지표 · 발신 IP 상위 ·
// 세션 표)은 각각 위젯이라 카드 안 편집으로 재배치할 수 있다(console_platform §3.0.1).
// 네 블록이 같은 조회 조건·결과를 봐야 하므로 상태는 모듈 store(`abnormalStore.ts`)에 둔다.
import { AlertTriangle, ArrowLeft, ArrowRight } from 'lucide-react'
import { useToast } from '@core/components/Toast'
import { InfoDot } from '@core/components/InfoDot'
import { abnDerived, abnormal, useAbnormal } from './abnormalStore'
import { Button } from '@core/components/ui/button'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

const REASON_LABEL: Record<string, { label: string; color: string }> = {
 external_ip:  { label: '외부 IP', color: 'var(--cims-warning)' },
 scanner_ua:   { label: '스캐너 도구', color: 'var(--destructive)' },
 fraud_number: { label: '사기 번호', color: 'var(--destructive)' },
 auth_failed:  { label: '인증 실패', color: 'var(--primary)' },
}
const SEV: Record<string, { label: string; bg: string }> = {
 critical: { label: '치명', bg: 'var(--destructive)' },
 major:    { label: '높음', bg: 'var(--cims-warning)' },
 minor:    { label: '낮음', bg: 'var(--muted-foreground)' },
}
const RANGE = [1, 3, 7]

// ── 조회 조건 ───────────────────────────────────────────────────────────────
export function AbnFilter() {
 const { show } = useToast()
 const s = useAbnormal(show)
 const { critical } = abnDerived(s)
 return (
    <div className="toolbar flex-wrap gap-2">
      <Input className="w-[150px]" type="date" value={s.date}
 onChange={e => abnormal.setDate(e.target.value)}/>
      <span className="text-sm text-muted-foreground">범위</span>
      <ToggleGroup type="single" value={String(s.days)} className="shrink-0 justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && abnormal.setDays(Number(v))}>
        {RANGE.map(d => (
          <ToggleGroupItem key={d} value={String(d)}>{d}일</ToggleGroupItem>
        ))}
      </ToggleGroup>
      <Button variant="default" onClick={() => void abnormal.load(show)}>조회</Button>
      {/* 화면의 뜻은 한 번 읽으면 되는 설명이라 ⓘ 로 접는다. */}
      <InfoDot label="비정상 세션이란?">
        공개 SIP 포트(VIP)로 유입되는 <b>인터넷발 스캐닝·사기 호 시도</b>입니다. CSP 는 인증(<b>401</b>)으로
        정상 거부하므로 실제 통화로 이어지지 않지만, 로그를 오염시키고 자원을 소모합니다.
        신호: <b>외부(공인) 발신 IP</b> · 알려진 <b>스캐너 UA</b>(pplsip 등) · <b>사기성 번호</b> ·
        <b> 인증 반복실패</b>. 다발 IP 는 방화벽 차단을 권장합니다.
      </InfoDot>
      {/* 조치가 필요한 신호는 접지 않는다 — 설명과 달리 매번 봐야 한다. */}
      {critical > 0 && (
        <span className="text-destructive font-bold text-sm">
          <AlertTriangle size={13} className="inline align-[-2px]" /> 외부에서 인증 성공(2xx)한 세션 있음 — 즉시 점검
        </span>
      )}
      {s.data && <span className="text-sm text-muted-foreground ml-auto">총 {s.data.total}건 탐지</span>}
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
  { key: 'total', label: '탐지 세션', title: '비정상 세션 — 탐지 세션',
 unit: '건', value: (_, total) => total, warnWhenPositive: true },
  { key: 'critical', label: '치명(외부 인증성공)', title: '비정상 세션 — 치명(외부 인증성공)',
 unit: '건', value: d => d.critical, warnWhenPositive: true },
  { key: 'scanners', label: '스캐너 도구', title: '비정상 세션 — 스캐너 도구',
 unit: '종', value: d => d.scanners },
  { key: 'srcIps', label: '발신 IP 수', title: '비정상 세션 — 발신 IP 수',
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
    <div className="panel p-3 flex flex-col min-h-0">
      <div className="text-sm font-semibold mb-2 flex-none">발신 IP 상위 (차단 후보)</div>
      {topIps.length === 0 ? <EmptyState title="해당 기간 발신 IP 없음" /> : (
        <div className="scroll-fill" style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignContent: 'flex-start' }}>
          {topIps.map(([ip, n]) => (
            <span key={ip} style={{ display: 'inline-flex', alignItems: 'center', gap: 6, height: 24,
 padding: '3px 10px', borderRadius: 14, background: 'rgba(220,38,38,0.08)',
 fontSize: 12, fontFamily: 'monospace' }}>
              {ip}<b className="text-destructive">{n}</b>
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
    <div className="panel flex flex-col min-h-0">
      {s.loading ? <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div> : (
        <>
          <div className="scroll-fill">
            <DataTable sticky>
              <thead>
                <tr>
                  <Th className="w-[92px]">최근 시각</Th>
                  <Th className="w-[56px]">심각도</Th>
                  <Th className="w-[130px]">발신 IP</Th>
                  <Th>발신 → 착신</Th>
                  <Th className="w-[96px]">UA</Th>
                  <Th className="w-[56px] text-right">시도</Th>
                  <Th className="w-[110px]">메서드/응답</Th>
                  <Th>사유</Th>
                </tr>
              </thead>
              <tbody>
                {pageRows.map((x, i) => {
 const sev = SEV[x.severity] || SEV.minor
 return (
                    <tr key={i}>
                      <Td  className="text-muted-foreground text-xs">{s.days > 1 ? `${x.date.slice(5)} ` : ''}{(x.last_ts || '').slice(0, 8)}</Td>
                      <Td><Badge style={{ background: sev.bg, color: 'var(--cims-on-solid)', fontSize: 10 }}>{sev.label}</Badge></Td>
                      <Td className="text-sm font-mono">{x.peer_ip || '-'}</Td>
                      <Td className="text-xs font-mono">
                        <span className="text-muted-foreground">{x.caller || '?'}</span>
                        <span className="my-0 mx-1">→</span>
                        <span>{x.callee || '?'}</span>
                      </Td>
                      <Td className="text-xs">{x.ua || '-'}</Td>
                      <Td style={{ fontSize: 12, textAlign: 'right', fontWeight: x.attempts > 5 ? 700 : 400 }}>{x.attempts}</Td>
                      <Td className="text-xs text-muted-foreground">
                        {x.methods.join(',') || '-'}{x.statuses.length > 0 && <span> / {x.statuses.join(',')}</span>}
                      </Td>
                      <Td>
                        <div className="flex flex-wrap gap-[3px]">
                          {x.reasons.map(r => {
 const rl = REASON_LABEL[r] || { label: r, color: 'var(--muted-foreground)' }
 return <Badge key={r} style={{ fontSize: 9, color: rl.color, border: `1px solid ${rl.color}`, background: 'transparent' }}>{rl.label}</Badge>
                          })}
                        </div>
                      </Td>
                    </tr>
                  )
                })}
                {sessions.length === 0 && <tr><Td colSpan={8} className="py-8 text-center text-muted-foreground">탐지된 비정상 세션 없음</Td></tr>}
              </tbody>
            </DataTable>
          </div>
          {sessions.length > s.pageSize && (
            <div className="flex items-center justify-center gap-2.5 py-2 px-0 flex-none border-t border-border">
              <Button disabled={s.page === 0} onClick={() => abnormal.setPage(s.page - 1)}><ArrowLeft /> 이전</Button>
              <span className="text-sm text-muted-foreground">
                {s.page * s.pageSize + 1}–{Math.min((s.page + 1) * s.pageSize, sessions.length)} / {sessions.length}건
                (페이지 {s.page + 1}/{pageCount})
              </span>
              <Button disabled={s.page >= pageCount - 1} onClick={() => abnormal.setPage(s.page + 1)}>다음 <ArrowRight /></Button>
              <Select value={String(s.pageSize)} onValueChange={(v: string) => abnormal.setPageSize(Number(v))}>
                <SelectTrigger className="text-sm py-0.5 px-1"><SelectValue /></SelectTrigger>
                <SelectContent>
                  {[50, 100, 200, 500].map(n => <SelectItem key={n} value={String(n)}>{n}/쪽</SelectItem>)}
                </SelectContent>
              </Select>
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
    <div className="panel p-2.5 flex flex-col">
      <div className="flex-auto min-h-0 flex flex-col justify-center items-center text-center">
        <div className="text-sm text-muted-foreground mb-1">{label}</div>
        <div style={{ fontSize: 24, fontWeight: 700, lineHeight: 1.1, color }}>
          {value}
          {unit && <span className="text-sm text-muted-foreground ml-0.5">{unit}</span>}
        </div>
      </div>
    </div>
  )
}
