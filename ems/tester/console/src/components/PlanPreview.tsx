// 계획 미리보기 — POST /runs/plan(= scenarios/compile-check) 결과를 사람이 읽는 꼴로: 역할→풀→워커·신원 창 표, 예상 소요·피크 율·동시 세션(SDT),
// 워커 SIP/RTP 용량, 대상 시드 예정 컬렉션, 바인딩·환경변수, Little 검산 경고, 워커 최대 SApS 초과·알려진 CSP 과제 경고
// (test_instrument.md §7 실행 라이브 ④). run 시작 창과 시나리오 편집기의 적합성 드로어가 같이 쓴다.
import { Badge } from '@core/components/ui/badge'
import type { PlanResult } from '@tester/api/tester'
import { fmtNum } from '@tester/lib/fmt'

function dur(s?: number | null): string {
  if (s == null) return '—'
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  return m < 60 ? `${m}m ${s % 60}s` : `${Math.floor(m / 60)}h ${m % 60}m`
}

export default function PlanPreview({ plan, loading, compact }: { plan: PlanResult | null; loading?: boolean; compact?: boolean }) {
  if (!plan) return <div className="rounded-md border border-border bg-muted p-3 text-xs text-muted-foreground">{loading ? '계획 계산 중…' : '시나리오·토폴로지를 고르면 계획을 미리 봅니다'}</div>
  const inRun = (plan.workers ?? []).filter(w => w.in_run)
  return (
    <div className="flex flex-col gap-2 rounded-md border border-border bg-muted p-3 text-xs">
      <div className="flex flex-wrap items-center gap-1.5">
        <span className="font-semibold uppercase tracking-wide text-muted-foreground">계획 미리보기</span>
        {loading && <span className="text-muted-foreground">갱신 중…</span>}
        {plan.ok ? <Badge variant="successSoft">컴파일 OK</Badge> : <Badge variant="dangerSoft">오류 {plan.errors.length}</Badge>}
        {plan.warnings.length > 0 && <Badge variant="warningSoft">경고 {plan.warnings.length}</Badge>}
        {(plan.active_runs?.length ?? 0) > 0 && <Badge variant="infoSoft">진행 중 run {plan.active_runs!.length} — 끝나야 시작 가능</Badge>}
      </div>
      {plan.errors.map((e, i) => <div key={`e${i}`} className="rounded-sm border-l-2 border-destructive bg-dangersoft px-2 py-1 text-dangersoft-on">{e}</div>)}
      {plan.warnings.map((w, i) => <div key={`w${i}`} className="rounded-sm border-l-2 border-warning bg-warning-soft px-2 py-1 text-warning-on">{w}</div>)}
      {plan.roles && (
        <table className="w-full text-xs">
          <thead><tr className="text-left text-muted-foreground"><th className="py-0.5 font-medium">역할</th><th className="font-medium">풀</th><th className="font-medium">워커 · 신원 창</th><th className="text-right font-medium">신원</th></tr></thead>
          <tbody>
            {Object.entries(plan.roles).map(([r, x]) => (
              <tr key={r} className="border-t border-border">
                <td className="py-0.5 font-mono">{r}</td>
                <td className="font-mono">{x.pool} <span className="text-muted-foreground">{x.kind}{x.profile ? `/${x.profile}` : ''}</span></td>
                <td className="font-mono">{Object.entries(x.workers).map(([w, [p, b, e]]) => `${w}:${p}[${b},${e})`).join(' ')}</td>
                <td className="text-right font-mono">{fmtNum(x.total, 0)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {plan.estimate && (
        <div className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-0.5">
          <span className="text-muted-foreground">예상 소요</span><span className="font-mono">{dur(plan.estimate.duration_s)} · {plan.estimate.model === 'single' ? `단발 ${plan.max_instances ?? 1}` : plan.estimate.model}</span>
          <span className="text-muted-foreground">피크 율 / 동시 세션</span><span className="font-mono">{fmtNum(plan.estimate.peak_rate, 1)} SApS · ≈ {fmtNum(plan.estimate.concurrent, 0)} (SDT ≈ {plan.estimate.sdt_s} s)</span>
          {inRun.length > 0 && <>
            <span className="text-muted-foreground">워커 용량</span>
            <span className="font-mono">{inRun.map(w => `${w.name}: SIP ${fmtNum(w.endpoints_needed, 0)}/${fmtNum(w.capacity?.max_endpoints, 0)} · RTP ${fmtNum(w.capacity?.rtp, 0) === '—' ? '?' : fmtNum(w.capacity?.rtp, 0)} · 최대 ${fmtNum(w.capacity?.max_saps, 0)} SApS · 율 ${fmtNum(w.rate_saps, 2)}${w.max_instances ? ` · ${w.max_instances} 인스턴스` : ''}${w.up ? '' : ' (미응답)'}`).join(' / ')}</span>
          </>}
          <span className="text-muted-foreground">대상 시드</span>
          <span className="font-mono">{plan.seed && plan.seed.length ? plan.seed.map(s => `${s.collection}+${s.count}`).join(' · ') + ' (종료 시 복원)' : '없음 (UE 만)'}</span>
          <span className="text-muted-foreground">바인딩</span><span className="font-mono">{Object.entries(plan.bindings ?? {}).map(([k, v]) => `\${${k}} = ${String(v)}`).join(' · ') || '없음'}</span>
          {plan.env && plan.env.length > 0 && <><span className="text-muted-foreground">환경변수</span><span className="font-mono">{plan.env.map(e => `${e.env} — ${e.for}`).join(' · ')}</span></>}
          {plan.media && plan.media.modes.length > 0 && (plan.media.modes.some(m => m !== 'auto') || Object.keys(plan.samples ?? {}).length > 0) && <>
            <span className="text-muted-foreground">미디어 평면</span>
            <span className="font-mono">rtp {[...new Set(plan.media.modes)].join(', ')}{Object.keys(plan.samples ?? {}).length ? ` · 샘플 ${Object.entries(plan.samples ?? {}).map(([id, m]) => `${id}(${Object.keys(m).join('/')})`).join(', ')}` : ''}</span>
          </>}
          {plan.pinned && <><span className="text-muted-foreground">피어 고정</span><span className="font-mono">{plan.peer_pools?.join(', ')} → {plan.pinned}</span></>}
        </div>
      )}
      {plan.little && plan.little.first_short_rate == null && plan.ok && (
        <div className="rounded-sm border-l-2 border-success bg-success-soft px-2 py-1 text-success-on">Little 검산 통과 — 피크 {fmtNum(plan.little.peak_rate, 1)} SApS × SDT {plan.little.sdt_s} s ≈ 동시 {fmtNum(plan.little.concurrent, 0)}</div>
      )}
      {!compact && plan.notes.length > 0 && <ul className="list-disc pl-4 text-muted-foreground">{plan.notes.map((n, i) => <li key={i}>{n}</li>)}</ul>}
    </div>
  )
}
