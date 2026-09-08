/* PttGroupActivity — 한 그룹의 활동 (PTT 그룹 › 활동 탭)
 *
 * 그룹 축 드릴다운(기간 → 일자 → 시간대 → 세션)이 여기 산다. 이력 페이지는 세션 단일
 * 평면이라 그룹 목록을 다시 그리지 않는다 — "이 그룹이 요즘 얼마나 쓰였나" 는 그룹을
 * 보고 있는 자리에서 묻는 질문이라 그룹 관리 쪽이 제자리다.
 *
 * 세션을 펼쳤을 때의 표현은 이력 페이지와 같은 것을 쓴다(@svc/components/pttSession) —
 * 세션은 어느 축으로 도달하든 같은 것이다.
 */
import { useState, useEffect, useCallback, useMemo } from 'react'
import { pttApi, type PttSession } from '@core/api/ptt'
import { recordingsApi, type RecordingSegment } from '@core/api/recordings'
import type { FlowMessage } from '@core/api/flow'
import FlowPage from '@core/pages/FlowPage'
import SegmentPlayer from '@core/components/SegmentPlayer'
import { useInlineAudio } from '@core/components/useInlineAudio'
import { useToast } from '@core/components/Toast'
import {
  DayHeatmap, ActivityHeatmap, SessionRow, RANGE_OPTIONS,
 recIdOf, dateOf, detailKey, dayOf, hourOf, thStyle,
 type DayAgg, type DetailState,
} from '@svc/components/pttSession'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { DataTable, Th } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import Modal from '@core/components/Modal'

export default function PttGroupActivity({ storeKey }: {
  /** 녹취 저장 키 = ptt_groups.id (surrogate). mcptt_group_id 가 바뀌어도 불변이라
   *  개명 전 이력이 끊기지 않는다. */
 storeKey: string
}) {
 const { show } = useToast()
 const audio = useInlineAudio(useCallback((m: string) => show(m, 'err'), [show]))

 const [rangeDays, setRangeDays] = useState(10)
 const [sessions, setSessions] = useState<PttSession[]>([])
 const [loading, setLoading] = useState(false)
 const [day, setDay] = useState<string | null>(null)
 const [open, setOpen] = useState<string | null>(null)
 const [detailByKey, setDetail] = useState<Map<string, DetailState>>(new Map())
 const [flow, setFlow] = useState<{ date: string; nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] } | null>(null)
 const [flowLoading, setFlowLoading] = useState(false)
 const [player, setPlayer] = useState<{ id: string; segments: RecordingSegment[]; title?: string } | null>(null)

  // ── 세션 목록 ──
 useEffect(() => {
 if (!storeKey) { setSessions([]); return }
 let cancelled = false
 setLoading(true)
 pttApi.sessions(storeKey, { days: rangeDays })
      .then(r => { if (!cancelled) setSessions(r.sessions || []) })
      .catch(() => { if (!cancelled) setSessions([]) })
      .finally(() => { if (!cancelled) setLoading(false) })
 return () => { cancelled = true }
  }, [storeKey, rangeDays])

  // 활동이 있는 최신 일자를 자동 선택 — 그룹을 열자마자 빈 화면을 보지 않게.
 useEffect(() => {
 const days = [...new Set(sessions.map(s => dayOf(s.dir)))].sort()
 setDay(days.length ? days[days.length - 1] : null)
 setOpen(null)
  }, [sessions])

 const dayAggs = useMemo<DayAgg[]>(() => {
 const byDay = new Map<string, DayAgg>()
 for (const s of sessions) {
 const d = dayOf(s.dir)
 const cur = byDay.get(d) || { day: d, turns: 0, speakers: 0, ms: 0, active: false, hasData: false }
 cur.turns += s.turn_count ?? s.segment_count ?? 0
 cur.speakers += s.speaker_count ?? 0
 cur.ms += s.total_speech_ms ?? 0
 cur.active = cur.active || s.state === 'active'
 cur.hasData = true
 byDay.set(d, cur)
    }
 const out: DayAgg[] = []
 const today = new Date()
 for (let i = rangeDays - 1; i >= 0; i--) {
 const d = new Date(today)
 d.setDate(today.getDate() - i)
 const key = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}`
 out.push(byDay.get(key) || { day: key, turns: 0, speakers: 0, ms: 0, active: false, hasData: false })
    }
 return out
  }, [sessions, rangeDays])

 const daySessions = useMemo(
    () => (day ? sessions.filter(s => dayOf(s.dir) === day)
      .sort((a, b) => (b.start_time || '').localeCompare(a.start_time || '')) : []),
 [sessions, day],
  )

  // ── 상세 lazy 로드 (이력 페이지와 같은 규약) ──
 const loadDetail = useCallback(async (dir: string) => {
 const dk = detailKey(storeKey, dir)
 if (detailByKey.get(dk)?.loaded) return
 setDetail(prev => new Map(prev).set(dk, { events: [], participants: [], floor: [], segments: [], loading: true, loaded: false }))
 const recId = recIdOf(storeKey, dir)
 const dt = dateOf(dir) || undefined
 try {
 const [ev, fl, rec] = await Promise.all([
 pttApi.events(storeKey, dir, dt),
 pttApi.floor(storeKey, dir, dt).catch(() => ({ floor: [] })),
 recId ? recordingsApi.get(recId).catch(() => ({ segments: [] })) : Promise.resolve({ segments: [] }),
      ])
 setDetail(prev => new Map(prev).set(dk, {
 events: ev.events || [], participants: ev.participants || [], floor: fl.floor || [],
 segments: (rec as { segments?: RecordingSegment[] }).segments || [],
 loading: false, loaded: true,
      }))
    } catch {
 setDetail(prev => new Map(prev).set(dk, { events: [], participants: [], floor: [], segments: [], loading: false, loaded: true }))
    }
  }, [storeKey, detailByKey])

 const toggle = (dir: string) => setOpen(prev => {
 if (prev === dir) return null
 loadDetail(dir)
 return dir
  })

 const playAll = async (dir: string) => {
 const recId = recIdOf(storeKey, dir)
 if (!recId) { show('세션키가 올바르지 않습니다', 'err'); return }
 try {
 const rec = await recordingsApi.get(recId)
 if (rec.segments?.length) setPlayer({ id: recId, segments: rec.segments })
 else show('녹취 세그먼트가 없습니다', 'err')
    } catch (e: unknown) { show(String(e), 'err') }
  }
 const openFlow = async (dir: string) => {
 setFlowLoading(true)
 const dt = dateOf(dir)
 try {
 const resp = await pttApi.flow(storeKey, dir, dt || undefined)
 setFlow({ date: dt, nodes: resp.nodes, messages: resp.messages })
    } catch (e: unknown) {
 show(String(e), 'err')
 setFlow({ date: dt })
    } finally { setFlowLoading(false) }
  }

 if (!storeKey) {
 return <EmptyState title="아직 통화 기록이 없는 그룹입니다 — 첫 그룹콜 이후 활동이 쌓입니다." className="p-[24px] text-[12.5px]" />
  }

 return (
    <div className="flex flex-col gap-2.5">
      <div className="flex items-center gap-2 flex-wrap">
        <span className="text-sm text-muted-foreground font-semibold">최근</span>
        <ToggleGroup type="single" value={String(rangeDays)} className="shrink-0 justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setRangeDays(Number(v))}>
          {RANGE_OPTIONS.map(d => (
            <ToggleGroupItem key={d} value={String(d)}>{d}일</ToggleGroupItem>
          ))}
        </ToggleGroup>
        <span className="ml-auto text-sm text-muted-foreground">
          {loading ? '조회 중…' : `세션 ${sessions.length}건`}
        </span>
      </div>

      <DayHeatmap days={dayAggs} selectedDay={day} onPick={setDay} />

      {day ? (
        <>
          <ActivityHeatmap sessions={daySessions} selectedDir={open} onPick={toggle} />

          {daySessions.length === 0 && !loading ? (
            <EmptyState title="이 날짜에 세션이 없습니다" className="p-[16px]" />
          ) : (
            <div className="border border-border rounded-md overflow-hidden">
              <div className="overflow-x-auto">
                <DataTable sticky className="[&_td]:text-sm">
                  <thead>
                    <tr className="bg-muted text-left">
                      <Th style={{ ...thStyle, width: 24, cursor: 'default' }}></Th>
                      <Th style={{ ...thStyle, cursor: 'default' }}>세션</Th>
                      <Th style={{ ...thStyle, cursor: 'default' }}>시각</Th>
                      <Th style={{ ...thStyle, cursor: 'default', textAlign: 'center' }}>상태</Th>
                      <Th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>발언 턴</Th>
                      <Th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>화자</Th>
                      <Th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>동시</Th>
                      <Th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>발화</Th>
                      <Th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>동작</Th>
                    </tr>
                  </thead>
                  <tbody>
                    {daySessions.map(s => (
                      <SessionRow
 key={s.dir} sess={s} storeKey={storeKey}
 isOpen={open === s.dir}
 detail={detailByKey.get(detailKey(storeKey, s.dir))}
                        // floor 축은 세션 당시 스냅샷이 정본. 그룹은 DB 에 floor_control
                        //   컬럼이 없다(세션마다 SDP 협상) — 미기록이면 반이중으로 본다.
 isDuplex={s.floor_control === 'off'}
 audio={audio} flowLoading={flowLoading}
 onToggle={() => toggle(s.dir)}
 onFlow={() => openFlow(s.dir)}
 onPlayAll={() => playAll(s.dir)}
                      />
                    ))}
                  </tbody>
                </DataTable>
              </div>
            </div>
          )}
          <div className="text-[11.5px] text-muted-foreground">
            선택 일자 {day.slice(4, 6)}/{day.slice(6, 8)} · {daySessions.length}세션
            {daySessions.length > 0 && ` · ${new Set(daySessions.map(s => hourOf(s.dir))).size}개 시간대`}
          </div>
        </>
      ) : (
        <div className="flex min-h-0 flex-1 items-center justify-center text-center text-muted-foreground p-[20px]">{loading ? '조회 중…' : '최근 활동이 없습니다 — 기간을 넓혀 보세요'}</div>
      )}

      {audio.node}

      {player && (
        <Modal title={player.title || '녹취 재생'} onClose={() => setPlayer(null)} width={800}>
          <SegmentPlayer segments={player.segments} recordingId={player.id} callType="ptt"
                         onClose={() => setPlayer(null)} />
        </Modal>
      )}

      {flow && (
        <FlowPage callId={storeKey} date={flow.date} callType="ptt" onClose={() => setFlow(null)}
 prefetchedNodes={flow.nodes} prefetchedMessages={flow.messages} />
      )}
    </div>
  )
}
