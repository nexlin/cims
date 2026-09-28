// 서비스 > 단말 현황 — 조회 전용(mcptt_management_views.md §4). 누가 어떤 단말로 로그인·등록돼 어느 그룹에 참여 중인가.
//   ① 필터(전체 · 접속 중 · 미접속, 단말 유형) + 이름·번호 검색  ② 단말 목록(이름 · 번호 · 유형 · 모델·앱 · 로그인 · 등록 ·
//   참여 그룹 · 최근 관측)  ③ 선택 단말 상세 4영역 — 단말 정보 · MCPTT 서비스 상태 · 그룹 참여 · 오늘 이용.
//   원천 = oam-svc GET /stats/service/ptt-terminals[/{msisdn}]. 단말 속성은 REGISTER 의 +sip.instance·User-Agent.
//   IMEI 는 개인 식별 정보 — 관리자가 아니면 서버가 가운데를 가려 보낸다.
import { useCallback, useEffect, useRef, useState } from 'react'
import { RefreshCw, History } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot } from '@core/components/custom/status-dot'
import {
  statsApi, type PttTerminalStateFilter, type PttTerminalsResponse, type PttTerminalResponse, type PttTerminalDevice,
} from '@core/api/stats'

const POLL_MS = 10000
const FILTERS: Array<{ id: PttTerminalStateFilter; label: string }> = [
  { id: 'all', label: '전체' }, { id: 'online', label: '접속 중' }, { id: 'offline', label: '미접속' },
]
const TYPES: Array<{ id: string; label: string }> = [
  { id: '', label: '모든 유형' }, { id: 'dispatch', label: '관제조작반' }, { id: 'handheld', label: '휴대 단말' },
  { id: 'sim', label: '시험 단말' }, { id: 'unknown', label: '기타' },
]
const TYPE_LABEL: Record<string, string> = { dispatch: '관제조작반', handheld: '휴대 단말', sim: '시험 단말', unknown: '기타' }
const when = (v: string | null | undefined) => (v ? v.replace('T', ' ').slice(0, 19) : '—')
const secText = (s: number) => (s >= 60 ? `${Math.floor(s / 60)}분 ${s % 60}초` : `${s}초`)
const appText = (d: { app: string; app_version: string }) => (d.app ? `${d.app}${d.app_version ? ` ${d.app_version}` : ''}` : '')

export default function PttTerminalsPage() {
  const [filter, setFilter] = useState<PttTerminalStateFilter>('all')
  const [ttype, setTtype] = useState('')
  const [q, setQ] = useState('')
  const [data, setData] = useState<PttTerminalsResponse | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [sel, setSel] = useState<string | null>(null)
  const qRef = useRef(q)
  qRef.current = q

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await statsApi.pttTerminals(filter, qRef.current.trim(), ttype)); setErr(null) }
    catch (e: unknown) { setErr(String(e)) } finally { setLoading(false) }
  }, [filter, ttype])
  useEffect(() => { void load(); const t = setInterval(() => void load(), POLL_MS); return () => clearInterval(t) }, [load])
  // 검색어는 입력이 멈춘 뒤 한 번
  useEffect(() => { const t = setTimeout(() => void load(), 300); return () => clearTimeout(t) }, [q, load])

  const rows = data?.terminals || []
  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2 border-b border-border px-3 py-2">
        <ToggleGroup type="single" value={filter} onValueChange={v => { if (v) setFilter(v as PttTerminalStateFilter) }}
          className="shrink-0 justify-start rounded-md bg-muted p-[3px]">
          {FILTERS.map(f => <ToggleGroupItem key={f.id} value={f.id}>{f.label} <span className="ml-1 text-muted-foreground">{data?.counts?.[f.id] ?? 0}</span></ToggleGroupItem>)}
        </ToggleGroup>
        <ToggleGroup type="single" value={ttype || 'all'} onValueChange={v => { if (v) setTtype(v === 'all' ? '' : v) }}
          className="shrink-0 justify-start rounded-md bg-muted p-[3px]">
          {TYPES.map(t => <ToggleGroupItem key={t.id || 'all'} value={t.id || 'all'}>{t.label}</ToggleGroupItem>)}
        </ToggleGroup>
        <Input value={q} onChange={e => setQ(e.target.value)} placeholder="이름·MCPTT 번호" className="h-8 w-56" />
        <Button variant="outline" size="sm" onClick={() => void load()} disabled={loading}><RefreshCw size={14} className={loading ? 'animate-spin' : ''} /> 새로고침</Button>
        <span className="ml-auto text-xs text-muted-foreground">조회 전용 — 단말 속성은 REGISTER 의 단말 ID·User-Agent</span>
      </div>
      {err && <div className="border-b border-border px-3 py-1.5 text-xs text-destructive">조회 실패 — {err}</div>}
      <div className="flex min-h-0 flex-1">
        <div className="scroll-fill min-w-0 flex-1 overflow-auto">
          {rows.length === 0 ? (
            <EmptyState title={data ? '조건에 맞는 단말이 없습니다' : '불러오는 중…'} description={data && (filter !== 'all' || ttype) ? '필터를 전체로 바꿔 보세요' : undefined} />
          ) : (
            <DataTable sticky>
              <thead><tr>
                <Th>이름</Th><Th width={100}>유형</Th><Th>모델 · 앱</Th>
                <Th width={80} title="IdMS 로그인(유효한 refresh token)">로그인</Th>
                <Th width={90} title="SIP 등록(접속)">등록</Th><Th width={60}>전송</Th>
                <Th align="right" width={70} title="참여(affiliation, TS 24.379 §9) 중인 그룹 수">참여</Th>
                <Th width={150} title="최근 단말 관측 — 해상도 1 시간">최근 관측</Th>
              </tr></thead>
              <tbody>
                {rows.map(r => (
                  <TrLink key={r.msisdn} selected={sel === r.msisdn} onClick={() => setSel(s => (s === r.msisdn ? null : r.msisdn))}>
                    <Td><div className="font-medium">{orDash(r.name)}</div><div className="font-mono text-xs text-muted-foreground">{r.msisdn}</div></Td>
                    <Td>{r.type ? TYPE_LABEL[r.type] || r.type : <span className="text-muted-foreground">—</span>}</Td>
                    <Td>
                      <div>{orDash(r.model || null)}{r.os ? <span className="text-muted-foreground"> · {r.os}</span> : null}</div>
                      <div className="text-xs text-muted-foreground">{appText(r) || '단말 속성 미수집'}{r.devices > 1 ? ` · 단말 ${r.devices}대` : ''}</div>
                    </Td>
                    <Td>{r.logged_in ? <Badge variant="successSoft">로그인</Badge> : <span className="text-muted-foreground">—</span>}</Td>
                    <Td><StatusDot tone={r.registered ? 'success' : 'neutral'} label={r.registered ? '접속 중' : '미접속'} title={r.register_time || undefined} /></Td>
                    <Td mono>{orDash(r.transport || null)}</Td>
                    <Td align="right">{r.affiliated_count}</Td>
                    <Td mono>{when(r.last_seen)}</Td>
                  </TrLink>
                ))}
              </tbody>
            </DataTable>
          )}
        </div>
        {sel && <TerminalDetail msisdn={sel} onClose={() => setSel(null)} />}
      </div>
    </div>
  )
}

function TerminalDetail({ msisdn, onClose }: { msisdn: string; onClose: () => void }) {
  const [d, setD] = useState<PttTerminalResponse | null>(null)
  const [err, setErr] = useState<string | null>(null)
  useEffect(() => {
    let alive = true
    const load = () => statsApi.pttTerminal(msisdn).then(r => { if (alive) { setD(r); setErr(null) } }).catch(e => { if (alive) setErr(String(e)) })
    setD(null); void load()
    const t = setInterval(load, POLL_MS)
    return () => { alive = false; clearInterval(t) }
  }, [msisdn])

  const reg = d?.registration
  return (
    <aside aria-label="단말 상세" className="flex w-[520px] max-w-[50%] shrink-0 flex-col overflow-auto border-l border-border bg-card">
      <div className="flex items-start justify-between gap-2 px-4 pt-3">
        <div className="min-w-0">
          <div className="text-lg font-bold">{d?.name || msisdn}</div>
          <div className="font-mono text-xs text-muted-foreground">{msisdn}{d?.org ? ` · ${d.org}` : ''}</div>
        </div>
        <div className="flex shrink-0 gap-1">
          <Button asChild variant="ghost" size="sm"><a href="/service/history/ptt"><History size={13} /> 세션 이력</a></Button>
          <Button variant="ghost" size="sm" onClick={onClose}>닫기</Button>
        </div>
      </div>
      {err && <div className="px-4 pt-2 text-xs text-destructive">조회 실패 — {err}</div>}
      {!d && !err && <div className="px-4 pt-3 text-sm text-muted-foreground">불러오는 중…</div>}
      {d && reg && (
        <div className="flex flex-col gap-4 px-4 pb-6 pt-3">
          <Section title="단말 정보">
            {d.device ? <DeviceGrid dev={d.device} type={d.type} /> : <div className="text-sm text-muted-foreground">이 번호로 관측된 단말이 없습니다 — 단말이 등록하면 채워집니다.</div>}
            {d.devices.length > 1 && (
              <DataTable>
                <thead><tr><Th>단말 ID</Th><Th>모델 · 앱</Th><Th width={80}>상태</Th><Th width={150}>최근 관측</Th></tr></thead>
                <tbody>
                  {d.devices.map(v => (
                    <tr key={v.instance_id || '-'}>
                      <Td mono>{v.imei || v.instance_id || '—'}</Td>
                      <Td>{orDash(v.model || null)} <span className="text-muted-foreground">{appText(v)}</span></Td>
                      <Td>{v.registered ? <Badge variant="successSoft">등록 중</Badge> : <span className="text-muted-foreground">이전</span>}</Td>
                      <Td mono>{when(v.last_seen)}</Td>
                    </tr>
                  ))}
                </tbody>
              </DataTable>
            )}
          </Section>
          <Section title="MCPTT 서비스 상태">
            <div className="grid grid-cols-[120px_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-sm">
              <span className="text-muted-foreground">로그인</span>
              <span>{d.login.logged_in
                ? <>{d.login.client_id} <span className="text-muted-foreground">· {when(d.login.issued_at)} 발급 · {when(d.login.expires_at)} 만료</span></>
                : <span className="text-muted-foreground">로그인 없음</span>}</span>
              <span className="text-muted-foreground">등록</span>
              <span><StatusDot tone={reg.registered ? 'success' : 'neutral'} label={reg.registered ? `접속 중 · ${when(reg.register_time)}` : `미접속${reg.logout_time ? ` · ${when(reg.logout_time)} 해제` : ''}`} /></span>
              {reg.node ? <><span className="text-muted-foreground">접속 경로</span><span className="font-mono">{reg.node} · {reg.transport} · {reg.addr}{reg.expires ? ` · 만료 ${reg.expires}초` : ''}</span></> : null}
              <span className="text-muted-foreground">보안 · 인증</span>
              <span className="flex flex-wrap gap-1">
                <Badge variant="neutralSoft">채널 {d.security.sip_transport}</Badge>
                <Badge variant="neutralSoft">인증 {d.security.auth_scheme === 'aka' ? 'IMS AKA' : 'Digest'}</Badge>
                {d.security.service_ref && <Badge variant="neutralSoft">접속서비스 {d.security.service_ref}</Badge>}
              </span>
            </div>
          </Section>
          <Section title={`그룹 참여 ${d.groups.filter(g => g.affiliated).length} / 멤버 ${d.groups.length}`}>
            {d.groups.length === 0 ? <div className="text-sm text-muted-foreground">—</div> : (
              <DataTable>
                <thead><tr><Th>그룹</Th><Th width={70}>역할</Th><Th width={80}>참여</Th><Th width={150}>참여 시각</Th></tr></thead>
                <tbody>
                  {d.groups.map(g => (
                    <tr key={g.id}>
                      <Td><span className="font-medium">{g.name}</span> <span className="font-mono text-xs text-muted-foreground">{g.id}</span></Td>
                      <Td>{g.role === 'chair' ? <Badge variant="brandSoft">chair</Badge> : '참가자'}</Td>
                      <Td>{g.affiliated ? <Badge variant="successSoft">참여</Badge> : <span className="text-muted-foreground">—</span>}</Td>
                      <Td mono>{when(g.affiliated_at)}</Td>
                    </tr>
                  ))}
                </tbody>
              </DataTable>
            )}
          </Section>
          <Section title="오늘 이용">
            <div className="flex flex-wrap gap-4 text-sm">
              <span>세션 <b>{d.today.sessions}</b></span>
              <span>발언 <b>{d.today.turns}</b>회</span>
              <span>발언 시간 <b>{secText(d.today.talk_sum_sec)}</b></span>
              <span>긴급 <b>{d.today.emergency}</b></span>
            </div>
          </Section>
        </div>
      )}
    </aside>
  )
}

function DeviceGrid({ dev, type }: { dev: PttTerminalDevice; type: string }) {
  return (
    <div className="grid grid-cols-[120px_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-sm">
      <span className="text-muted-foreground">단말 유형</span><span>{TYPE_LABEL[type] || orDash(type || null)}</span>
      <span className="text-muted-foreground">단말 ID (IMEI)</span>
      <span className="font-mono">{dev.imei || <span className="text-muted-foreground">IMEI 없음</span>}{dev.imei_masked ? <span className="ml-1 font-sans text-xs text-muted-foreground">(가림 — 원문은 관리자)</span> : null}</span>
      <span className="text-muted-foreground">instance</span><span className="break-all font-mono text-xs">{orDash(dev.instance_id || null)}</span>
      <span className="text-muted-foreground">모델 · OS</span><span>{orDash(dev.model || null)}{dev.os ? ` · ${dev.os}` : ''}</span>
      <span className="text-muted-foreground">앱</span><span>{orDash(appText(dev) || null)}</span>
      <span className="text-muted-foreground">User-Agent</span><span className="break-all font-mono text-xs">{orDash(dev.user_agent || null)}</span>
      <span className="text-muted-foreground">처음 · 최근</span><span className="font-mono">{when(dev.first_seen)} · {when(dev.last_seen)}</span>
    </div>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-2">
      <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">{title}</span>
      {children}
    </div>
  )
}
