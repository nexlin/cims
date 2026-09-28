// 서비스 > MCPTT 그룹 정보 — 조회 전용(mcptt_management_views.md §3). 편집은 구성 › PTT 그룹.
//   ① 필터(전체 · 활동 중 · 발언 중 · 긴급 허용) + 그룹명·ID·멤버 검색  ② 그룹 목록(상태 · 멤버/등록/참여 · 오늘 세션)
//   ③ 선택 그룹 상세 — 속성 · 멤버별 등록/참여(affiliation, TS 24.379 §9)/세션 참가/발언 · 오늘 이용 요약.
//   원천 = oam-svc GET /stats/service/ptt-groups[/{id}] — 진행 중 세션·발언자는 5 초 주기로 다시 읽는다.
import { useCallback, useEffect, useRef, useState } from 'react'
import { RefreshCw, Pencil, History, Mic } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot, type StatusTone } from '@core/components/custom/status-dot'
import { statsApi, type PttGroupStateFilter, type PttGroupLiveState, type PttGroupsStatusResponse, type PttGroupStatusResponse } from '@core/api/stats'

const POLL_MS = 5000
const FILTERS: Array<{ id: PttGroupStateFilter; label: string }> = [
  { id: 'all', label: '전체' }, { id: 'active', label: '활동 중' }, { id: 'talking', label: '발언 중' }, { id: 'emergency', label: '긴급 허용' },
]
const STATE: Record<PttGroupLiveState, { label: string; tone: StatusTone }> = {
  talking: { label: '발언 중', tone: 'success' }, active: { label: '세션 진행', tone: 'info' }, idle: { label: '유휴', tone: 'neutral' },
}
const TYPE_LABEL: Record<string, string> = { prearranged: '편성', chat: '채팅' }
const POLICY_LABEL: Record<string, string> = { single: '단일', dual: '듀얼', multi: '멀티' }
const secLabel = (v: number | null | undefined) => (v == null ? '—' : v === 0 ? '미사용' : `${v}초`)

export default function PttGroupInfoPage() {
  const [filter, setFilter] = useState<PttGroupStateFilter>('all')
  const [q, setQ] = useState('')
  const [data, setData] = useState<PttGroupsStatusResponse | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [sel, setSel] = useState<string | null>(null)
  const qRef = useRef(q)
  qRef.current = q

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await statsApi.pttGroupsStatus(filter, qRef.current.trim())); setErr(null) }
    catch (e: unknown) { setErr(String(e)) } finally { setLoading(false) }
  }, [filter])
  useEffect(() => { void load(); const t = setInterval(() => void load(), POLL_MS); return () => clearInterval(t) }, [load])
  // 검색어는 입력이 멈춘 뒤 한 번
  useEffect(() => { const t = setTimeout(() => void load(), 300); return () => clearTimeout(t) }, [q, load])

  const groups = data?.groups || []
  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2 border-b border-border px-3 py-2">
        <ToggleGroup type="single" value={filter} onValueChange={v => { if (v) setFilter(v as PttGroupStateFilter) }}
          className="shrink-0 justify-start rounded-md bg-muted p-[3px]">
          {FILTERS.map(f => <ToggleGroupItem key={f.id} value={f.id}>{f.label} <span className="ml-1 text-muted-foreground">{data?.counts?.[f.id] ?? 0}</span></ToggleGroupItem>)}
        </ToggleGroup>
        <Input value={q} onChange={e => setQ(e.target.value)} placeholder="그룹명·ID·멤버 번호/이름" className="h-8 w-64" />
        <Button variant="outline" size="sm" onClick={() => void load()} disabled={loading}><RefreshCw size={14} className={loading ? 'animate-spin' : ''} /> 새로고침</Button>
        <span className="ml-auto text-xs text-muted-foreground">조회 전용 — 편집은 구성 › PTT 그룹</span>
      </div>
      {err && <div className="border-b border-border px-3 py-1.5 text-xs text-destructive">조회 실패 — {err}</div>}
      <div className="flex min-h-0 flex-1">
        <div className="scroll-fill min-w-0 flex-1 overflow-auto">
          {groups.length === 0 ? (
            <EmptyState title={data ? '조건에 맞는 그룹이 없습니다' : '불러오는 중…'} description={data && filter !== 'all' ? '필터를 전체로 바꿔 보세요' : undefined} />
          ) : (
            <DataTable sticky>
              <thead><tr>
                <Th>그룹</Th><Th width={70}>유형</Th><Th width={110}>상태</Th><Th>발언자</Th>
                <Th align="right" width={70}>멤버</Th><Th align="right" width={70} title="등록(접속) 중인 멤버">등록</Th>
                <Th align="right" width={70} title="그룹에 참여(affiliation) 중인 멤버 — TS 24.379 §9">참여</Th>
                <Th align="right" width={90} title="오늘 그룹 세션 수(1분 롤업)">오늘 세션</Th><Th width={90}>허용</Th>
              </tr></thead>
              <tbody>
                {groups.map(g => (
                  <TrLink key={g.id} selected={sel === g.id} onClick={() => setSel(s => (s === g.id ? null : g.id))}>
                    <Td><div className="font-medium">{g.name}</div><div className="font-mono text-xs text-muted-foreground">{g.id}</div></Td>
                    <Td>{TYPE_LABEL[g.group_type] || g.group_type}</Td>
                    <Td><StatusDot tone={STATE[g.state].tone} label={g.state === 'idle' ? STATE.idle.label : `${STATE[g.state].label} · ${g.participants}명`} /></Td>
                    <Td mono>{g.floor_holders.length ? g.floor_holders.join(', ') : orDash(null)}</Td>
                    <Td align="right">{g.member_count}</Td>
                    <Td align="right">{g.registered_count}</Td>
                    <Td align="right">{g.affiliated_count}</Td>
                    <Td align="right">{g.today_sessions}</Td>
                    <Td><span className="flex gap-1">
                      {g.emergency_call && <Badge variant="dangerSoft">긴급</Badge>}
                      {g.video_enabled && <Badge variant="infoSoft">영상</Badge>}
                      {g.encryption && <Badge variant="neutralSoft">보안</Badge>}
                      {!g.emergency_call && !g.video_enabled && !g.encryption && <span className="text-muted-foreground">—</span>}
                    </span></Td>
                  </TrLink>
                ))}
              </tbody>
            </DataTable>
          )}
        </div>
        {sel && <GroupDetail id={sel} onClose={() => setSel(null)} />}
      </div>
    </div>
  )
}

function GroupDetail({ id, onClose }: { id: string; onClose: () => void }) {
  const [d, setD] = useState<PttGroupStatusResponse | null>(null)
  const [err, setErr] = useState<string | null>(null)
  useEffect(() => {
    let alive = true
    const load = () => statsApi.pttGroupStatus(id).then(r => { if (alive) { setD(r); setErr(null) } }).catch(e => { if (alive) setErr(String(e)) })
    setD(null); void load()
    const t = setInterval(load, POLL_MS)
    return () => { alive = false; clearInterval(t) }
  }, [id])

  const g = d?.group
  return (
    <aside aria-label="그룹 상세" className="flex w-[520px] max-w-[50%] shrink-0 flex-col overflow-auto border-l border-border bg-card">
      <div className="flex items-start justify-between gap-2 px-4 pt-3">
        <div className="min-w-0">
          <div className="text-lg font-bold">{g?.name || id}</div>
          <div className="font-mono text-xs text-muted-foreground">{id}</div>
        </div>
        <div className="flex shrink-0 gap-1">
          <Button asChild variant="ghost" size="sm"><a href="/subscribers/ptt-groups"><Pencil size={13} /> 편집</a></Button>
          <Button asChild variant="ghost" size="sm"><a href="/service/history/ptt"><History size={13} /> 세션 이력</a></Button>
          <Button variant="ghost" size="sm" onClick={onClose}>닫기</Button>
        </div>
      </div>
      {err && <div className="px-4 pt-2 text-xs text-destructive">조회 실패 — {err}</div>}
      {!d && !err && <div className="px-4 pt-3 text-sm text-muted-foreground">불러오는 중…</div>}
      {d && g && (
        <div className="flex flex-col gap-4 px-4 pb-6 pt-3">
          <div className="flex flex-wrap items-center gap-3 text-sm">
            <StatusDot tone={STATE[d.state].tone} label={d.state === 'idle' ? STATE.idle.label : `${STATE[d.state].label} · 참가 ${d.participants}명`} />
            {d.floor_holders.length > 0 && <span className="flex items-center gap-1"><Mic size={13} /><span className="font-mono">{d.floor_holders.join(', ')}</span></span>}
            <span className="text-muted-foreground">오늘 세션 {d.today.sessions} · 발언 있던 세션 {d.today.talked}</span>
          </div>
          <Section title="그룹 속성">
            <div className="grid grid-cols-[120px_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-sm">
              <span className="text-muted-foreground">유형</span><span>{TYPE_LABEL[g.group_type] || g.group_type}</span>
              <span className="text-muted-foreground">우선순위</span><span>{orDash(g.priority)}</span>
              <span className="text-muted-foreground">동시 발언</span><span>{POLICY_LABEL[g.floor_policy] || g.floor_policy}{g.floor_policy === 'multi' ? ` (${g.max_talkers}명)` : ''}</span>
              <span className="text-muted-foreground">소유자</span><span>{orDash(g.owner)}</span>
              <span className="text-muted-foreground">조직</span><span className="font-mono">{orDash(g.org_code)}</span>
              <span className="text-muted-foreground">유지 시간(T4)</span><span>{secLabel(g.hang_timer_sec)}</span>
              <span className="text-muted-foreground">최대 통화 시간</span><span>{g.max_duration_sec === 0 ? '무제한' : secLabel(g.max_duration_sec)}</span>
              <span className="text-muted-foreground">허용</span>
              <span className="flex flex-wrap gap-1">
                <Badge variant={g.emergency_call ? 'dangerSoft' : 'neutralSoft'}>긴급 호출 {g.emergency_call ? '허용' : '불허'}</Badge>
                <Badge variant="neutralSoft">긴급 경보 {g.emergency_alert ? '허용' : '불허'}</Badge>
                <Badge variant="neutralSoft">영상 {g.video_enabled ? '허용' : '불허'}</Badge>
                <Badge variant="neutralSoft">보안 {g.encryption ? '사용' : '미사용'}</Badge>
                <Badge variant="neutralSoft">참여(affiliation) {g.require_affiliation ? '필요' : '불요'}</Badge>
              </span>
            </div>
          </Section>
          <Section title={`멤버 ${d.counts.members} · 등록 ${d.counts.registered} · 참여 ${d.counts.affiliated}`}>
            {d.members.length === 0 ? <div className="text-sm text-muted-foreground">—</div> : (
              <DataTable>
                <thead><tr><Th>이름</Th><Th>번호</Th><Th width={70}>역할</Th><Th width={80}>등록</Th><Th width={70}>참여</Th><Th width={90}>세션</Th></tr></thead>
                <tbody>
                  {d.members.map(m => (
                    <tr key={m.msisdn}>
                      <Td>{orDash(m.name)}</Td>
                      <Td mono>{m.msisdn}</Td>
                      <Td>{m.role === 'chair' ? <Badge variant="brandSoft">chair</Badge> : '참가자'}</Td>
                      <Td><StatusDot tone={m.registered ? 'success' : 'neutral'} label={m.registered ? '등록' : '미등록'} title={m.register_time || undefined} /></Td>
                      <Td>{m.affiliated ? <Badge variant="successSoft">참여</Badge> : <span className="text-muted-foreground">—</span>}</Td>
                      <Td>{m.talking ? <Badge variant="successSoft">발언 중</Badge> : m.in_session ? <Badge variant="infoSoft">참가</Badge> : <span className="text-muted-foreground">—</span>}</Td>
                    </tr>
                  ))}
                </tbody>
              </DataTable>
            )}
          </Section>
        </div>
      )}
    </aside>
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
