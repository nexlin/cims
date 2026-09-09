// 활성 알람 — 전역 알람 store(useAlarms, SSE 라이브) 구독 뷰 (alarm_pipeline.md §8.2).
//   이력 페이지(AlertsPage)와 분리: 여기는 "지금 열린 알람" 만 — 승인/코멘트 조작 포함.
//   배치 단위는 **심각도 타일 1장 = 위젯 1개**(Critical/Major/…) 와 **목록**이다. 타일과 목록을
//   잇는 건 배치가 아니라 페이지 파라미터 `sev` (타일이 쓰고 목록이 읽는다) — 타일을 몇 장 놓든,
//   순서를 어떻게 바꾸든 목록 필터는 그대로 걸린다.
//   데이터는 store 하나(대시보드 위젯·헤더 배지와 동일 fold)라 표시 일관.
import { useMemo, useState } from 'react'
import { alertsApi } from '../api/alerts'
import { useToast } from '../components/Toast'
import { useAlarms, refreshAlarms, severityOf, type ActiveAlarm } from '../widgets/useAlarms'
import { alarmTypeLabel, sevBadgeClass, fmtTime, formatSec, SEVERITY_LABEL } from '../utils/alarmLabels'
import { usePageParam } from '../widgets/pageParams'
import { Check, MessageSquare, RotateCw } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

function elapsedSince(ts?: string): string {
  const t = new Date(ts || '').getTime()
  if (isNaN(t)) return '-'
  return formatSec(Math.max(0, Math.round((Date.now() - t) / 1000)))
}

function AlarmDetail({ a, onAck, onComment }: {
  a: ActiveAlarm
  onAck: (id?: string) => void
  onComment: (id: string | undefined, text: string) => void
}) {
  const [text, setText] = useState('')
  const item = (label: string, value?: string | null) => value ? (
    <div className="flex gap-2 text-sm">
      <span className="text-muted-foreground min-w-[90px] shrink-0">{label}</span>
      <span>{value}</span>
    </div>
  ) : null
  return (
    <div className="flex flex-col gap-1.5 pt-2.5 px-4 pb-3">
      {item('alarm_id', a.alarm_id)}
      {item('eventType', a.event_type)}
      {item('probableCause', a.probable_cause)}
      {item('영향', a.effect)}
      {item('권장 조치', a.recommended_action)}
      {a.threshold_info && item('관측값', `${a.threshold_info.observed}${a.threshold_info.unit || ''} (임계 ${a.threshold_info.threshold}${a.threshold_info.unit || ''})`)}
      {(a.occurrences ?? 1) > 1 && item('재통지', `해제 없이 ${a.occurrences}회 — 최근 ${fmtTime(a.last_open_ts)}`)}
      {a.acked && item('승인', `${a.ackUser || ''}`)}
      {(a.comments?.length ?? 0) > 0 && (
        <div className="text-sm">
          <div className="text-muted-foreground mb-0.5">코멘트</div>
          {a.comments!.map((c, i) => (
            <div className="pt-0.5 pr-0 pb-0.5 pl-2 border-l-2 border-border" key={i}>
              <span className="text-muted-foreground">{c.user || ''} {fmtTime(c.ts)}</span> — {c.text}
            </div>
          ))}
        </div>
      )}
      <div className="flex gap-1.5 items-center mt-0.5">
        {!a.acked && (
          <Button disabled={!a.alarm_id} onClick={() => onAck(a.alarm_id)}>승인</Button>
        )}
        <Input className="w-[280px]" placeholder="코멘트 입력 후 Enter"
               value={text} onChange={e => setText(e.target.value)}
               onKeyDown={e => {
                 if (e.key === 'Enter' && text.trim()) { onComment(a.alarm_id, text.trim()); setText('') }
               }}/>
      </div>
    </div>
  )
}

// 심각도 타일 1장 — 배치 1칸을 채우고, 클릭하면 페이지 파라미터 `sev` 를 토글한다(같은 화면의
// 활성 알람 목록이 그 값으로 걸린다. 이미 켜진 타일을 다시 누르면 필터 해제).
export function AlarmSeverityTile({ sev }: { sev: string }) {
  const { active } = useAlarms()
  const [sevFilter, setSevFilter] = usePageParam('sev')
  const n = useMemo(() => active.filter(a => severityOf(a) === sev).length, [active, sev])
  const on = sevFilter === sev
  return (
    <button onClick={() => setSevFilter(on ? '' : sev)}
      title={`${SEVERITY_LABEL[sev] || sev} ${n}건${on ? ' — 필터 해제' : n ? ' — 이 심각도만 보기' : ''}`}
      style={{
        flex: '1 1 auto', minHeight: 0, minWidth: 0, textAlign: 'left', cursor: 'pointer',
        background: 'var(--card)', borderRadius: 'var(--radius)', padding: '12px 16px',
        border: on ? '1px solid var(--primary)' : '1px solid var(--border)',
      }}>
      <div className="text-sm text-muted-foreground">{SEVERITY_LABEL[sev] || sev}</div>
      <div style={{ fontSize: 24, fontWeight: 700,
                    color: n > 0 && (sev === 'critical' || sev === 'major') ? 'var(--destructive)' : 'var(--foreground)' }}>
        {n}
      </div>
    </button>
  )
}

// 활성 알람 목록 — 검색 + 표 + 상세. 심각도 필터는 타일 위젯들이 쓰는 페이지 파라미터를 읽는다.
export function ActiveAlarmList() {
  const { show } = useToast()
  const { active, loaded, error, lastUpdated } = useAlarms()
  const [sevFilter] = usePageParam('sev')
  const [q, setQ] = useState('')
  const [expanded, setExpanded] = useState<string | null>(null)

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase()
    return active.filter(a => {
      if (sevFilter && severityOf(a) !== sevFilter) return false
      if (needle && ![a.code, a.type, a.message, a.source?.mo_instance, a.source?.mo_label]
        .some(v => (v || '').toLowerCase().includes(needle))) return false
      return true
    })
  }, [active, sevFilter, q])

  const ack = async (alarmId?: string) => {
    if (!alarmId) return
    try { await alertsApi.ack(alarmId); show('알람 승인됨', 'ok'); refreshAlarms() }
    catch (e) { show((e as Error).message, 'err') }
  }
  const comment = async (alarmId: string | undefined, text: string) => {
    if (!alarmId) return
    try { await alertsApi.comment(alarmId, text); show('코멘트 기록됨', 'ok'); refreshAlarms() }
    catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="widget-stack">

      <div className="toolbar flex-wrap gap-2">
        <Input className="flex-1 w-[260px]" placeholder="코드/소스/메시지 검색"
               value={q} onChange={e => setQ(e.target.value)}/>
        <span style={{ marginLeft: 'auto', fontSize: 12, color: error ? 'var(--destructive)' : 'var(--muted-foreground)' }}>
          {error ? '갱신 실패 — 표시가 최신이 아닐 수 있음' : lastUpdated ? `갱신 ${fmtTime(new Date(lastUpdated).toISOString())} · 라이브` : ''}
        </span>
        <Button variant="ghost" onClick={refreshAlarms} title="새로고침"><RotateCw size={14} /></Button>
      </div>

      <div className="panel">
        <div className="py-3 px-4 font-semibold text-base border-b border-border">
          활성 알람 ({rows.length}건{sevFilter || q ? ` / 전체 ${active.length}` : ''})
        </div>
        {!loaded ? (
          <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중…</div>
        ) : rows.length === 0 ? (
          <EmptyState title={active.length === 0 ? '활성 알람 없음' : '필터 결과 없음'} />
        ) : (
          <DataTable sticky>
            <thead>
              <tr>
                <Th className="w-[90px]">심각도</Th>
                <Th className="w-[100px]">코드</Th>
                <Th className="w-[120px]">클래스</Th>
                <Th className="w-[170px]">소스</Th>
                <Th>메시지</Th>
                <Th className="w-[145px]">발생 시각</Th>
                <Th className="w-[100px]">경과</Th>
                <Th className="w-[90px]">승인</Th>
              </tr>
            </thead>
            <tbody>
              {rows.map(a => {
                const key = a.alarm_id || `${a.ts}-${a.type}`
                const open = expanded === key
                return [
                  <tr key={key} onClick={() => setExpanded(open ? null : key)}
                      style={{ cursor: 'pointer', background: open ? 'var(--accent)' : undefined }}>
                    <Td><Badge variant={sevBadgeClass(severityOf(a))} >{severityOf(a)}</Badge></Td>
                    <Td className="font-mono text-xs">{a.code || '-'}</Td>
                    <Td>{alarmTypeLabel(a.type)}</Td>
                    <Td><code className="text-xs" title={a.source?.mo_instance || ''}>
                      {a.source?.mo_label || a.source?.mo_instance || '-'}</code></Td>
                    <Td>
                      {a.message}
                      {(a.occurrences ?? 1) > 1 && (
                        <Badge variant="neutralSoft" className="ml-1.5 align-[1px]">×{a.occurrences}</Badge>
                      )}
                      {(a.comments?.length ?? 0) > 0 && (
                        <span className="ml-1.5 inline-flex items-center gap-0.5 text-xs text-muted-foreground">
                      <MessageSquare size={11} />{a.comments!.length}</span>
                      )}
                    </Td>
                    <Td className="text-sm text-muted-foreground">{fmtTime(a.ts)}</Td>
                    <Td>{elapsedSince(a.ts)}</Td>
                    <Td>
                      {a.acked
                        ? <span className="inline-flex items-center gap-1 text-xs text-success">
                      <Check size={12} /> {a.ackUser || '승인'}</span>
                        : <span className="text-muted-foreground text-sm">미승인</span>}
                    </Td>
                  </tr>,
                  open && (
                    <tr key={`${key}-detail`}>
                      <Td className="p-0 bg-accent" colSpan={8}>
                        <AlarmDetail a={a} onAck={ack} onComment={comment} />
                      </Td>
                    </tr>
                  ),
                ]
              })}
            </tbody>
          </DataTable>
        )}
      </div>
    </div>
  )
}
