// run 보고서 — RFC 6076 표 · 지연 분포 · 절차/예상/결과 표(ptt-test-scenario CSV 형식) · 단계별 DOC/IHS · 실패 코드 분해 ·
// 참고. 화면과 인쇄(PDF) 공용 — 검증 콘솔 VerificationPrintReport 와 같은 인쇄 규약(루트 `.tester-report`,
// 호출 페이지의 @media print 가 이것만 남긴다). Markdown 은 컨트롤러 report_markdown 이 같은 내용을 만든다.
import { useMemo } from 'react'
import { Badge } from '@core/components/ui/badge'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import type { RunDoc, ScenarioDoc, ExpectResult, RunSeries } from '@tester/api/tester'
import LineChart, { type Series } from '@tester/components/LineChart'
import { fmtNum, fmtPct, fmtTime, fmtUnix, fmtDuration, SUMMARY_ROWS, TIMER_ROWS, STEP_LABEL, summaryValue, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'

function stepText(s: NonNullable<ScenarioDoc['flow']>[number]): string {
  const who = s.who?.length ? s.who.join(', ') : s.from ? `${s.from} → ${s.to ?? ''}` : ''
  const extra: string[] = []
  if (s.after_ms) extra.push(`${s.after_ms} ms 뒤`)
  if (s.seconds != null) extra.push(`${s.seconds} s`)
  if (s.media) extra.push([s.media.audio, s.media.video].filter(Boolean).join('+'))
  if (s.payload) extra.push(`payload ${s.payload}`)
  if (s.cause) extra.push(`Q.850 cause ${s.cause}`)
  return `${STEP_LABEL[s.step] ?? s.step}${who ? ` (${who})` : ''}${extra.length ? ` — ${extra.join(', ')}` : ''}`
}

function expectText(exp: unknown): string {
  if (exp == null) return '—'
  if (typeof exp !== 'object') return String(exp)
  return Object.entries(exp as Record<string, unknown>).map(([k, v]) => `${k} ${fmtNum(v)}`).join(', ')
}

export function parseCodes(codes: unknown): [string, number][] {
  if (typeof codes !== 'string' || !codes) return []
  return codes.split(',').map(x => x.split(':')).filter(p => p.length === 2).map(([k, v]) => [k, Number(v)] as [string, number])
}

export default function RunReport({ run, scenario, series, print }: {
  run: RunDoc
  scenario?: ScenarioDoc | null
  series?: RunSeries | null
  /** 인쇄 표지용 — true 면 상단에 발행 일시·표지 정보 */
  print?: boolean
}) {
  const s = run.summary ?? {}
  const timers = run.timers ?? {}
  const er = run.expect_results ?? []
  const byStep = useMemo(() => {
    const m = new Map<number, ExpectResult[]>()
    er.forEach(r => { const a = m.get(r.step) ?? []; a.push(r); m.set(r.step, a) })
    return m
  }, [er])
  const flow = scenario?.flow ?? []
  const codes = parseCodes(s.codes)
  const stepLog = (run.step_log ?? []).filter(r => r.event === 'step' || r.event === 'start')

  const chart = useMemo(() => {
    if (!series || series.t.length < 2) return null
    const t = series.t
    const c = series.counters, g = series.gauges, tm = series.timers
    const ratio = t.map((_, i) => { const a = c.attempts?.[i] ?? 0; return a ? (100 * (c.sessions?.[i] ?? 0)) / a : null })
    const loss = t.map((_, i) => { const tot = (c.rtp_rx?.[i] ?? 0) + (c.rtp_lost?.[i] ?? 0); return tot ? (100 * (c.rtp_lost?.[i] ?? 0)) / tot : null })
    const out: Series[] = [
      { key: 'saps', label: 'SApS', values: c.attempts ?? [], color: 'var(--chart-1)' },
      { key: 'conc', label: '동시 세션', values: g.concurrent_sessions ?? [], color: 'var(--chart-2)' },
      { key: 'ser', label: 'SER', values: ratio, color: 'var(--chart-3)', unit: '%', max: 100 },
      { key: 'srd', label: 'SRD p95', values: tm.srd_ms?.p95 ?? [], color: 'var(--chart-4)', unit: 'ms' },
      { key: 'loss', label: 'RTP 손실', values: loss, color: 'var(--chart-5)', unit: '%', max: 5 },
      { key: 'cpu', label: '워커 CPU', values: g.cpu_pct ?? [], color: 'var(--chart-6)', unit: '%', max: 100 },
    ]
    const bands = stepLog.map((r, i) => ({ from: r.t, to: i + 1 < stepLog.length ? stepLog[i + 1].t : t[t.length - 1] + 1, label: `${fmtNum(r.rate, 1)} SApS` }))
    return { t, out, bands }
  }, [series, stepLog])

  return (
    <div className="tester-report flex flex-col gap-4">
      {print && (
        <div className="border-b border-border pb-2">
          <div className="text-xl font-bold">계측기 시험 보고서</div>
          <div className="text-sm text-muted-foreground">발행 {new Date().toLocaleString('ko-KR')} · oam-cims-tester</div>
        </div>
      )}
      <div className="flex flex-wrap items-center gap-2">
        <Badge variant={VERDICT_TONE[run.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[run.verdict] ?? run.verdict}</Badge>
        <span className="text-md font-semibold">{run.scenario_id}</span>
        {scenario?.title && <span className="text-sm text-muted-foreground">{scenario.title}</span>}
        {run.label && <Badge variant="brandSoft">{run.label}</Badge>}
      </div>
      <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm sm:grid-cols-[auto_1fr_auto_1fr]">
        <dt className="text-muted-foreground">run</dt><dd className="font-mono">{run.id}</dd>
        <dt className="text-muted-foreground">토폴로지 / 프로파일</dt><dd>{run.topology} / {run.profile ?? '단발'}</dd>
        <dt className="text-muted-foreground">시작 → 종료</dt><dd className="font-mono">{fmtTime(run.started_at, true)} → {fmtTime(run.ended_at, true)} ({fmtDuration(run.started_at, run.ended_at)})</dd>
        <dt className="text-muted-foreground">워커</dt><dd>{(run.workers ?? []).join(', ') || '—'}</dd>
        <dt className="text-muted-foreground">대상 빌드</dt><dd>{orDash(run.target_build)}</dd>
        <dt className="text-muted-foreground">역할 → 풀</dt><dd className="font-mono">{run.plan ? Object.entries(run.plan.roles).map(([r, p]) => `${r}→${p}`).join(', ') : '—'}{run.plan ? ` · 신원 ${run.plan.identities} · 율 ${fmtNum(run.plan.rate_total)} SApS` : ''}</dd>
        {run.stop_reason && <><dt className="text-muted-foreground">중단 사유</dt><dd className="text-destructive">{run.stop_reason}</dd></>}
        {run.bindings && Object.keys(run.bindings).length > 0 && <><dt className="text-muted-foreground">바인딩</dt><dd className="font-mono">{Object.entries(run.bindings).map(([k, v]) => `${k}=${String(v)}`).join(' ')}</dd></>}
      </dl>

      {chart && (
        <section className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">시간축 (1초 집계)</h3>
          <LineChart t={chart.t} series={chart.out} bands={chart.bands} height={180} />
        </section>
      )}

      <div className="grid gap-4 lg:grid-cols-2">
        <section className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">RFC 6076 지표</h3>
          <DataTable>
            <thead><tr><Th>지표</Th><Th align="right">값</Th></tr></thead>
            <tbody>
              {SUMMARY_ROWS.filter(([k]) => s[k] !== undefined && s[k] !== null).map(([k, label, kind]) => (
                <tr key={k}><Td>{label}</Td><Td align="right" mono>{summaryValue(s, k, kind)}</Td></tr>
              ))}
            </tbody>
          </DataTable>
        </section>
        <section className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">지연 분포</h3>
          <DataTable>
            <thead><tr><Th>지연</Th><Th align="right">n</Th><Th align="right">p50</Th><Th align="right">p95</Th><Th align="right">p99</Th><Th align="right">max</Th></tr></thead>
            <tbody>
              {TIMER_ROWS.filter(([k]) => timers[k]).map(([k, label]) => {
                const h = timers[k]
                return (
                  <tr key={k}><Td>{label}</Td><Td align="right" mono>{h.count}</Td><Td align="right" mono>{fmtNum(h.p50)}</Td>
                    <Td align="right" mono>{fmtNum(h.p95)}</Td><Td align="right" mono>{fmtNum(h.p99)}</Td><Td align="right" mono>{fmtNum(h.max)}</Td></tr>
                )
              })}
              {TIMER_ROWS.every(([k]) => !timers[k]) && <tr><Td className="text-muted-foreground">관측 없음</Td><Td /><Td /><Td /><Td /><Td /></tr>}
            </tbody>
          </DataTable>
          {codes.length > 0 && (
            <>
              <h3 className="mt-2 text-sm font-semibold text-muted-foreground">응답 코드 분해</h3>
              <div className="flex flex-wrap gap-1.5">
                {codes.map(([code, n]) => (
                  <Badge key={code} variant={code.startsWith('2') || code.startsWith('1') ? 'successSoft' : code.startsWith('5') ? 'dangerSoft' : 'warningSoft'}>{code} × {n}</Badge>
                ))}
              </div>
            </>
          )}
        </section>
      </div>

      <section className="flex flex-col gap-1.5">
        <h3 className="text-sm font-semibold text-muted-foreground">절차 · 예상 결과 · 확인 결과</h3>
        <DataTable>
          <thead><tr><Th width={40}>#</Th><Th>절차(단계)</Th><Th>예상 결과(expect)</Th><Th>확인 결과(관측)</Th><Th width={70}>판정</Th></tr></thead>
          <tbody>
            {(flow.length ? flow.map((st, i) => ({ i, st })) : Array.from(byStep.keys()).sort((a, b) => a - b).map(i => ({ i, st: null as null | NonNullable<ScenarioDoc['flow']>[number] }))).map(({ i, st }) => {
              const rs = byStep.get(i) ?? []
              const ok = rs.length === 0 ? null : rs.every(r => r.ok)
              return (
                <tr key={i}>
                  <Td mono>{i + 1}</Td>
                  <Td>{st ? stepText(st) : (STEP_LABEL[rs[0]?.kind] ?? rs[0]?.kind)}</Td>
                  <Td mono className="text-xs">{rs.length ? rs.map(r => `${r.metric}: ${expectText(r.expect)}`).join(' · ') : (st?.expect && Object.keys(st.expect).length ? Object.entries(st.expect).map(([m, e]) => `${m}: ${expectText(e)}`).join(' · ') : '—')}</Td>
                  <Td mono className="text-xs">{rs.length ? rs.map(r => `${r.metric}: ${expectText(r.observed)}`).join(' · ') : '—'}</Td>
                  <Td>{ok == null ? <Badge variant="neutralSoft">기대치 없음</Badge> : ok ? <Badge variant="successSoft">PASS</Badge> : <Badge variant="dangerSoft">FAIL</Badge>}</Td>
                </tr>
              )
            })}
          </tbody>
        </DataTable>
        {scenario?.target_evidence && scenario.target_evidence.length > 0 && (
          <div className="text-xs text-muted-foreground">대상 증거(target_evidence): {scenario.target_evidence.map(e => `${e.kind}${e.min != null ? ` ≥${e.min}` : ''}${e.max != null ? ` ≤${e.max}` : ''}${e.code ? ` ${e.code}` : ''}`).join(' · ')} — 대상 관측은 F 단계</div>
        )}
      </section>

      {stepLog.length > 1 && (
        <section className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">단계 로그 (step 프로파일 — DOC {run.doc_rate != null ? `${fmtNum(run.doc_rate, 1)} SApS` : '미확정'})</h3>
          <DataTable>
            <thead><tr><Th>시각</Th><Th align="right">율(SApS)</Th><Th align="right">시도</Th><Th align="right">IHS %</Th></tr></thead>
            <tbody>
              {stepLog.map((r, i) => (
                <tr key={i}><Td mono>{fmtUnix(r.t)}</Td><Td align="right" mono>{fmtNum(r.rate, 1)}</Td><Td align="right" mono>{fmtNum(r.attempts, 0)}</Td>
                  <Td align="right" mono>{r.ihs_pct == null ? '—' : fmtPct(r.ihs_pct)}</Td></tr>
              ))}
            </tbody>
          </DataTable>
        </section>
      )}

      {(run.notes?.length ?? 0) > 0 && (
        <section className="flex flex-col gap-1">
          <h3 className="text-sm font-semibold text-muted-foreground">참고</h3>
          <ul className="list-disc pl-5 text-sm">{run.notes!.map((n, i) => <li key={i}>{n}</li>)}</ul>
        </section>
      )}
    </div>
  )
}
