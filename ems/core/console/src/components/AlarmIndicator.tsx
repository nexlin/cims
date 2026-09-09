// 헤더 상시 알람 인디케이터 + 드로어 + 전이 토스트 (alarm_pipeline.md §8.2).
//   - 배지: 활성 요약(최고 severity 색 + 건수). 0건이어도 회색 배지를 상시 표시한다 —
//     "표시 없음 = 정상"과 "표시 없음 = 표시 고장"을 구분(observability_lost 철학).
//     폴링 실패 시 ! 표기.
//   - 드로어: 활성 알람 목록(승인/이동) + 최근 이벤트 탭. 어느 라우트에서든 상주.
//   - 토스트: critical/major open·moreSevere 승격만 수동 닫기 토스트 — minor 이하/close 는
//     배지 갱신만, 이벤트는 토스트 없음 (§8.2 소음 통제).
import { Bell, Check, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useDismiss } from '@core/hooks/useDismiss'
import { useNavigate } from 'react-router-dom'
import { alertsApi } from '../api/alerts'
import { onAlarmTransition, refreshAlarms, severityOf, useAlarms } from '../widgets/useAlarms'
import { useToast } from './Toast'
import { Badge } from './ui/badge'
import { Button } from '@core/components/ui/button'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import type { BadgeTone } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

const SEV_BADGE: Record<string, BadgeTone> = {
 critical: 'dangerSoft', major: 'dangerSoft', minor: 'warningSoft',
 warning: 'warningSoft', indeterminate: 'brandSoft',
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
 const box = useRef<HTMLDivElement>(null)
 const [tab, setTab] = useState<'alarms' | 'events'>('alarms')
  // 톱니바퀴·계정 메뉴(Radix DropdownMenu)와 같은 조작감 — 바깥 클릭·Esc 로 닫는다.
  // 시트 2 에 Drawer 컴포넌트가 없어 직접 만든 패널이라 이 동작을 스스로 갖춘다.
 useDismiss(open, box, () => setOpen(false))
 const navigate = useNavigate()
 const { show } = useToast()
 useAlarmToasts()

 const top = active[0] ? severityOf(active[0]) : ''
  // 카운트 배지 톤 — 0건은 Neutral(경고색 금지, DESIGN-RULES §1-7), 조회 실패는 Danger.
 const badgeTone = error ? 'dangerSolid'
    : active.length === 0 ? 'neutralSoft'
    : (top === 'critical' || top === 'major') ? 'dangerSolid'
    : (top === 'minor' || top === 'warning') ? 'warningSolid'
    : 'infoSolid'

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
    // 트리거와 드로어를 한 래퍼에 둔다 — 바깥 클릭 판정의 경계다. 트리거가 안에 있어야
    // 「벨을 다시 눌러 닫기」가 바깥 클릭으로 두 번 처리되지 않는다.
    <div ref={box} className="contents">
      {/* 트리거 — 시안 AppBar 의 `util/알람`(벨 + 카운트 배지, Figma 457:5431).
          0건도 배지를 지우지 않는다: "표시 없음 = 정상"과 "표시 없음 = 표시 고장"을
          구분해야 한다(alarm_pipeline.md §8.2). 대신 DESIGN-RULES §1-7 대로 0건은
          경고색이 아니라 Neutral 로 낸다. */}
      <button className="relative flex items-center gap-1.5 rounded-md px-2 py-1.5 text-neutral hover:bg-sidebar-accent"
 onClick={() => setOpen(o => !o)}
 aria-label={loaded ? `활성 알람 ${active.length}건` : '알람 로드 중'}
 title={error ? '알람 조회 실패 — 표시가 최신이 아닐 수 있음'
                           : loaded ? `활성 알람 ${active.length}건` : '알람 로드 중'}>
        <Bell size={16} />
        <Badge variant={badgeTone}>{loaded ? active.length : '…'}{error ? ' !' : ''}</Badge>
      </button>
      {open && (
        <div role="dialog" aria-label="알람 드로어"
             className="fixed bottom-0 right-0 top-[58px] z-[150] flex w-[380px] max-w-[90vw] flex-col border-l border-border bg-card shadow-lg">
          <div className="flex gap-0.5 border-b-2 border-border pt-2 px-3.5 pb-0">
            <ToggleGroup type="single" value={tab} className="shrink-0 justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setTab(v as typeof tab)}>
              <ToggleGroupItem value="alarms">활성 알람 ({active.length})</ToggleGroupItem>
              <ToggleGroupItem value="events">최근 이벤트 ({recentEvents.length})</ToggleGroupItem>
            </ToggleGroup>
            <Button className="ml-auto" variant="ghost"
 onClick={() => setOpen(false)} aria-label="닫기"><X size={16} /></Button>
          </div>
          <div className="min-h-0 flex-1 overflow-y-auto">
            {tab === 'alarms' && active.length === 0 && (
              <EmptyState title="활성 알람 없음" className="p-[20px]" />
            )}
            {tab === 'alarms' && active.map(a => {
 const sev = severityOf(a)
 return (
                <div key={a.alarm_id || a.type} className="border-b border-border px-3.5 py-2.5 text-md">
                  <div className="flex items-center gap-1.5">
                    <Badge variant={SEV_BADGE[sev] || 'neutralSoft'} >{sev}</Badge>
                    <span className="font-mono text-xs">{a.code}</span>
                    {(a.occurrences || 1) > 1 && (
                      <Badge variant="neutralSoft" >×{a.occurrences}</Badge>
                    )}
                    <span className="ml-auto text-xs text-muted-foreground">{a.ts}</span>
                  </div>
                  <div className="mt-[3px]">{a.message}</div>
                  <div className="mt-1 flex gap-2 items-center">
                    <span className="text-xs text-muted-foreground font-mono">
                      {a.source?.mo_instance}
                    </span>
                    <span className="ml-auto"/>
                    {a.acked
                      ? <span className="inline-flex items-center gap-1 text-xs text-muted-foreground">
                        <Check size={12} /> {a.ackUser || '승인'}</span>
                      : <Button variant="ghost" onClick={() => ack(a.alarm_id)}>승인</Button>}
                    <Button variant="ghost"
 onClick={() => { setOpen(false); navigate('/alerts/active') }}>이동</Button>
                  </div>
                </div>
              )
            })}
            {tab === 'events' && recentEvents.length === 0 && (
              <EmptyState title="최근 24시간 이벤트 없음" className="p-[20px]" />
            )}
            {tab === 'events' && recentEvents.map((ev, i) => (
              <div key={i} className="border-b border-border px-3.5 py-2.5 text-md">
                <div className="flex gap-1.5 items-center">
                  <Badge variant="neutralSoft" >{ev.kind}</Badge>
                  <span className="text-sm">{ev.type}</span>
                  <span className="ml-auto text-xs text-muted-foreground">{ev.ts}</span>
                </div>
                <div className="mt-[3px] text-sm">{ev.message}</div>
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}
