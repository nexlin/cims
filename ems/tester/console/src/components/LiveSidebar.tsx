// 라이브 패널 왼쪽 — 단계 사다리(step 프로파일: 율 단계마다 완료 ✓·IHS %, 현 단계 진행, DOC 후보, 신원 부족 예상은 흐리게) +
// 종료 조건 게이지(stop_on IHS · 대상 CPU(F) · CSP 5xx · Little 신원 여유) (test_instrument.md §7 실행 라이브 ②).
import { Badge } from '@core/components/ui/badge'
import type { RunLive } from '@tester/api/tester'
import { fmtNum, fmtPct } from '@tester/lib/fmt'
import { stepRates, sdtSeconds, minRoleIdentities, type ProfileLike } from '@tester/lib/metrics'

function Gauge({ label, value, limit, unit, tone, note }: { label: React.ReactNode; value: number | null; limit: number | null; unit: string; tone: 'ok' | 'warn' | 'bad' | 'none'; note?: string }) {
  const pct = value != null && limit ? Math.min(100, (100 * value) / limit) : 0
  const bar = tone === 'bad' ? 'bg-destructive' : tone === 'warn' ? 'bg-warning' : 'bg-success'
  const badge = tone === 'bad' ? <Badge variant="dangerSoft">초과</Badge> : tone === 'warn' ? <Badge variant="warningSoft">{note ?? '주의'}</Badge> : tone === 'ok' ? <Badge variant="successSoft">여유</Badge> : <Badge variant="neutralSoft">{note ?? '—'}</Badge>
  return (
    <div className="flex flex-col gap-1 text-xs">
      <div className="flex items-center gap-2"><span className="min-w-0 flex-1 truncate">{label} <span className="font-mono">{value == null ? '—' : `${fmtNum(value, unit === '%' ? 2 : 0)}${unit}`}</span>{limit != null && <span className="text-muted-foreground"> / {fmtNum(limit, unit === '%' ? 1 : 0)}{unit}</span>}</span>{badge}</div>
      <div className="h-1.5 w-full overflow-hidden rounded-sm bg-neutral-soft"><div className={`h-full ${bar}`} style={{ width: `${pct}%` }} /></div>
    </div>
  )
}

export default function LiveSidebar({ live, window5xx, windowIhs, now }: {
  live: RunLive | null
  /** 최근 60 초 창 CSP 5xx 비율(%) — 라이브 버킷에서 계산 */
  window5xx: number | null
  /** 현 단계 창 IHS(%) — (실패+건너뜀)/시도 */
  windowIhs: number | null
  now: number
}) {
  const p = (live?.profile_doc ?? null) as ProfileLike | null
  const rates = stepRates(p)
  const stepLog = live?.step_log ?? []
  const steps = stepLog.filter(r => r.event === 'step')
  const cur = steps.length ? steps[steps.length - 1] : null
  const sdt = sdtSeconds(live?.plan?.steps, live?.plan?.phases)
  const ident = minRoleIdentities(live?.plan)
  const roleN = live?.plan?.roles ? Object.keys(live.plan.roles).length : 1
  const ihsLimit = p?.ihs_threshold_pct ?? null
  const so = p?.stop_on ?? {}
  const curRate = live?.rate_saps ?? cur?.rate ?? 0
  const need = Math.ceil(curRate * sdt)
  const firstShort = rates.find(r => ident != null && Math.ceil(r * sdt) > ident) ?? null

  return (
    <aside className="flex w-full flex-col gap-3 lg:w-[250px] lg:shrink-0">
      {rates.length > 0 && (
        <div>
          <div className="mb-1 flex items-center gap-2 text-sm font-semibold text-muted-foreground">단계 사다리 <Badge variant="neutralSoft">step · hold {p?.hold_s} s</Badge></div>
          <ol className="flex max-h-[360px] flex-col gap-0.5 overflow-auto text-xs">
            {rates.map(r => {
              const done = steps.find(s => Math.abs(s.rate - r) < 1e-6 && s.ihs_pct != null)
              const isCur = cur != null && Math.abs(cur.rate - r) < 1e-6 && done == null
              const short = ident != null && Math.ceil(r * sdt) > ident
              const isDoc = live?.doc_rate != null && Math.abs(live.doc_rate - r) < 1e-6
              const prog = isCur && p?.hold_s ? Math.min(100, (100 * (now - cur!.t - (live?.held_s ?? 0))) / p.hold_s) : null
              return (
                <li key={r} className={`flex items-center gap-2 rounded-sm px-1.5 py-0.5 ${isCur ? 'bg-accent' : ''} ${short && !done && !isCur ? 'opacity-50' : ''}`}>
                  <span className="w-14 font-mono">{fmtNum(r, 1)}</span>
                  {done ? <span className="text-success">✓ {fmtPct(done.ihs_pct)}</span>
                    : isCur ? <span className="flex-1"><span className="mr-1 text-info">진행</span><span className="inline-block h-1 w-16 overflow-hidden rounded-sm bg-neutral-soft align-middle"><span className="block h-full bg-info" style={{ width: `${prog ?? 0}%` }} /></span></span>
                    : <span className="text-muted-foreground">{short ? `신원 ${Math.ceil(r * sdt)} 필요` : '대기'}</span>}
                  {isDoc && <Badge variant="brandSoft">DOC</Badge>}
                </li>
              )
            })}
          </ol>
        </div>
      )}
      <div className="flex flex-col gap-2 rounded-md border border-border p-2.5">
        <div className="text-sm font-semibold text-muted-foreground">종료 조건 (stop_on)</div>
        <Gauge label="IHS 부적절 처리" value={windowIhs} limit={ihsLimit} unit="%"
               tone={windowIhs == null || ihsLimit == null ? 'none' : windowIhs > ihsLimit ? 'bad' : windowIhs > ihsLimit * 0.7 ? 'warn' : 'ok'} note={ihsLimit == null ? '프로파일 없음' : undefined} />
        <Gauge label={<>대상 CPU <Badge variant="neutralSoft">F</Badge></>} value={live?.gauges?.target_cpu_pct ?? null} limit={so.target_cpu_pct ?? null} unit="%"
               tone={live?.gauges?.target_cpu_pct == null ? 'none' : (so.target_cpu_pct != null && live.gauges.target_cpu_pct > so.target_cpu_pct) ? 'bad' : 'ok'} note="대상 관측 후속" />
        <Gauge label="CSP 5xx (60 s)" value={window5xx} limit={so.csp_5xx_pct ?? null} unit="%"
               tone={window5xx == null ? 'none' : so.csp_5xx_pct != null && window5xx > so.csp_5xx_pct ? 'bad' : window5xx > 0 ? 'warn' : 'ok'} />
        <Gauge label={<>신원 여유 (Little) <span className="font-mono text-muted-foreground">{fmtNum(curRate, 1)} × {sdt}s × {roleN}</span></>}
               value={ident != null ? need * roleN : null} limit={ident != null ? ident * roleN : null} unit=""
               tone={ident == null ? 'none' : need > ident ? 'bad' : firstShort != null ? 'warn' : 'ok'}
               note={ident == null ? '계획 없음' : firstShort != null ? `${fmtNum(firstShort, 0)} SApS 부터 부족` : undefined} />
      </div>
    </aside>
  )
}
