// 내 대시보드 구성 — 콘솔 D1 개인화 (oam_base_service_split §6).
//   프로파일 템플릿 선택 + 위젯 추가/제거/순서 → 서버(OAM /console/layouts/me) 저장.
//   카탈로그는 서버가 RBAC 로 필터(권한 밖 위젯은 애초에 안 옴) + 서비스 가용성(available)을
//   표기 — 미설치/불가 위젯은 "설치 후 사용 가능"으로 안내하고 추가 비활성(장애격리 UX, D7).
//   저장 PUT 도 서버가 RBAC 재검증(레이아웃이 보안 아님 — 심층방어).
//
// **화면 = 카드 하나**(`core.my-layout`)이고 안의 세 블록(상태·프로파일·위젯 목록)은 각각 위젯이라
// 운영자가 카드 안에서 재배치할 수 있다(console_platform §3.0.1). 세 블록이 같은 편집 초안을
// 봐야 하므로 상태는 모듈 store(`myLayoutStore.ts`)로 끌어올렸다.
import { StatusDot } from '../components/custom/status-dot'
import { AlertTriangle, ArrowDown, ArrowUp, X } from 'lucide-react'
import { useConfirm } from '../components/custom/confirm'
import { useMemo } from 'react'
import { useToast } from '../components/Toast'
import { InfoDot } from '../components/InfoDot'
import { widgetUnavailableNote, type CatalogWidget, type WidgetArea } from '../api/consoleLayouts'
import { myLayout, useMyLayout } from './myLayoutStore'
import { Button } from '@core/components/ui/button'
import { Select, SelectContent, SelectGroup, SelectItem, SelectLabel, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { Badge } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

const AREA_LABEL: Record<WidgetArea, string> = { ops: '운용', admin: '관리' }

// ── 상태 · 저장 조작 ────────────────────────────────────────────────────────
export function MyLayoutHeader() {
  const { show } = useToast()
  const confirm = useConfirm()
  const s = useMyLayout(show)
  return (
    <div className="toolbar flex items-center gap-2.5 border-b border-border bg-muted px-4 py-3 flex-wrap gap-2">
      <Badge  style={{ background: s.source === 'override' ? 'var(--primary)' : 'var(--secondary)' }}>
        {s.source === 'override' ? '개인 구성' : '프로파일 기본'}
      </Badge>
      {s.dirty && <StatusDot tone="warning" label="저장되지 않은 변경" />}
      <InfoDot label="내 대시보드 구성이란?">
        구성은 서버(계정별)에 저장되어 기기·세션을 넘어 따라갑니다. 위젯 가용성은 서비스 설치/상태에
        따릅니다 — 모든 위젯 API 는 서버에서 권한을 재확인합니다.
      </InfoDot>
      <span className="ml-auto flex gap-1.5">
        <Button variant="default" onClick={() => void myLayout.save(show)}
                disabled={s.saving || !s.dirty}>저장</Button>
        <Button onClick={() => void myLayout.load(show)}
                disabled={s.saving}>되돌리기</Button>
        <Button disabled={s.saving} title="개인 구성 삭제 → 프로파일 기본"
                onClick={() => void (async () => {
                  if (await confirm({ title: '개인 구성 초기화', tone: 'danger', confirmLabel: '초기화',
                    body: '개인 구성을 삭제하고 프로파일 기본값으로 되돌릴까요?' })) myLayout.reset(show)
                })()}>
          초기화
        </Button>
      </span>
    </div>
  )
}

// ── 프로파일 템플릿 ─────────────────────────────────────────────────────────
export function MyLayoutProfile() {
  const { show } = useToast()
  const s = useMyLayout(show)
  return (
    <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-3.5 flex flex-col min-h-0">
      <div className="font-semibold mb-2 flex-none">프로파일</div>
      <div className="flex gap-2 flex-wrap items-center">
        <Select value={toSel(s.baseProfile)} onValueChange={(v: string) => myLayout.setBaseProfile(fromSel(v))}>
          <SelectTrigger className="w-[200px]"><SelectValue /></SelectTrigger>
          <SelectContent>
            {s.profiles.map(p => <SelectItem key={p.id} value={p.id}>{p.label}</SelectItem>)}
          </SelectContent>
        </Select>
        <Button onClick={() => myLayout.applyProfile(s.baseProfile)}
                title="선택한 프로파일의 기본 위젯 세트로 교체">이 프로파일 적용</Button>
        <span className="text-sm text-muted-foreground">
          설치된 서비스: {s.installed.length ? s.installed.join(', ') : '없음'}
        </span>
      </div>
    </div>
  )
}

// ── 대시보드 위젯 목록 ──────────────────────────────────────────────────────
export function MyLayoutWidgets() {
  const { show } = useToast()
  const s = useMyLayout(show)
  const byId = useMemo(() => {
    const m: Record<string, CatalogWidget> = {}
    for (const w of s.catalog) m[w.id] = w
    return m
  }, [s.catalog])

  // 추가 가능 후보 = 카탈로그 중 미배치. area 그룹핑, 가용 먼저.
  const addable = useMemo(() => {
    const remaining = s.catalog.filter(w => !s.dashboard.includes(w.id))
    const groups: { area: WidgetArea; widgets: CatalogWidget[] }[] = []
    for (const area of ['ops', 'admin'] as WidgetArea[]) {
      const ws = remaining.filter(w => w.area === area)
        .sort((a, b) => Number(b.available) - Number(a.available))
      if (ws.length) groups.push({ area, widgets: ws })
    }
    return groups
  }, [s.catalog, s.dashboard])

  return (
    <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card p-3.5 flex flex-col min-h-0">
      <div className="flex items-center gap-2 mb-2.5 flex-none">
        <div className="font-semibold">대시보드 위젯 ({s.dashboard.length})</div>
        <Select value={toSel("")} onValueChange={(v: string) => myLayout.add(fromSel(v))}>
          <SelectTrigger className="w-[240px] ml-auto text-md"><SelectValue /></SelectTrigger>
          <SelectContent>
            <SelectItem value={NONE}>+ 위젯 추가…</SelectItem>
            {addable.map(g => (
              <SelectGroup key={g.area}>
                <SelectLabel>{AREA_LABEL[g.area]}</SelectLabel>
                {g.widgets.map(w => (
                  <SelectItem key={w.id} value={w.id} disabled={!w.available}>
                    {w.title}{w.available ? '' : ' — 설치 후 사용 가능'}
                  </SelectItem>
                ))}
              </SelectGroup>
            ))}
          </SelectContent>
        </Select>
      </div>

      {s.loading ? (
        <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">불러오는 중…</div>
      ) : s.dashboard.length === 0 ? (
        <EmptyState title="위젯이 없습니다 — 위 [+ 위젯 추가] 또는 프로파일을 적용하세요." />
      ) : (
        <ul className="scroll-fill flex min-h-0 flex-1 flex-col overflow-auto" style={{ listStyle: 'none', margin: 0, padding: 0, gap: 6 }}>
          {s.dashboard.map((id, i) => {
            const w = byId[id]
            const note = w ? widgetUnavailableNote(w) : '카탈로그에 없는 위젯(권한/서비스 변경)'
            return (
              <li key={`${id}-${i}`} style={{
                display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', flex: 'none',
                border: '1px solid var(--border)', borderRadius: 6, marginBottom: 6,
                opacity: w && !w.available ? 0.7 : 1,
              }}>
                <span className="font-medium">{w?.title ?? id}</span>
                {w && <Badge className="text-xs">{AREA_LABEL[w.area]}</Badge>}
                {w?.requires_service && <span className="text-xs text-muted-foreground">{w.requires_service}</span>}
                {note && <span className="inline-flex items-center gap-1 text-xs text-warning-on">
        <AlertTriangle size={12} /> {note}</span>}
                <span className="text-xs text-muted-foreground">({id})</span>
                <span className="ml-auto flex gap-1">
                  <Button onClick={() => myLayout.move(i, -1)} disabled={i === 0} title="위로"><ArrowUp size={13} /></Button>
                  <Button onClick={() => myLayout.move(i, 1)}
                          disabled={i === s.dashboard.length - 1} title="아래로"><ArrowDown size={13} /></Button>
                  <Button className="text-destructive" onClick={() => myLayout.remove(i)} title="제거"
                           aria-label="삭제"><X size={13} /></Button>
                </span>
              </li>
            )
          })}
        </ul>
      )}
    </div>
  )
}
