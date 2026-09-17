// 부하 프로파일 곡선 — LoadProfile(constant/step/ramp/soak/burst, ETSI TS 186 008) 의 시간축 율 모양을 SVG 하나로.
// 컨트롤러 오케스트레이터 규칙과 같은 해석(step = start 부터 hold_s 마다 +step 을 max 까지 · ramp = ramp_s 동안 선형 뒤 hold_s ·
// burst = burst_interval_s 마다 1 초 burst_size). 예상 소요·피크 율·총 시도 수는 곡선 아래 요약으로.
import { useMemo } from 'react'
import { fmtNum } from '@tester/lib/fmt'
import { estimateDuration, type ProfileLike } from '@tester/lib/metrics'

function dur(s: number): string {
  if (s < 60) return `${Math.round(s)}s`
  const m = Math.floor(s / 60)
  return m < 60 ? `${m}m ${Math.round(s % 60)}s` : `${Math.floor(m / 60)}h ${m % 60}m`
}

/** (t, rate) 꺾은선 — 각 구간의 시작·끝을 명시해 계단이 그대로 보이게 */
export function profileSeries(p: ProfileLike): { pts: [number, number][]; total: number; peak: number; attempts: number } {
  const pts: [number, number][] = []
  let total = 0, peak = 0, attempts = 0
  const seg = (t0: number, t1: number, r0: number, r1 = r0) => { pts.push([t0, r0], [t1, r1]); peak = Math.max(peak, r0, r1); attempts += ((r0 + r1) / 2) * (t1 - t0); total = t1 }
  switch (p.model) {
    case 'constant': case 'soak': seg(0, p.duration_s ?? 0, p.rate ?? 0); break
    case 'step': { let r = p.start ?? 0, t = 0; const st = p.step || 1, hold = p.hold_s ?? 0, mx = p.max ?? r
      for (let i = 0; i < 200 && r <= mx; i++) { seg(t, t + hold, r); t += hold; if (r >= mx) break; r = Math.min(mx, Math.round((r + st) * 100) / 100) } break }
    case 'ramp': seg(0, p.ramp_s ?? 0, p.start ?? 0, p.max ?? 0); seg(p.ramp_s ?? 0, (p.ramp_s ?? 0) + (p.hold_s ?? 0), p.max ?? 0); break
    case 'burst': { const iv = p.burst_interval_s ?? 1, d = p.duration_s ?? 0, sz = p.burst_size ?? 0
      for (let t = 0; t < d && pts.length < 2000; t += iv) { seg(t, t + 1, sz); if (t + 1 < Math.min(t + iv, d)) seg(t + 1, Math.min(t + iv, d), 0) } break }
  }
  return { pts, total, peak, attempts: Math.round(attempts) }
}

export default function ProfileCurve({ profile, height = 120 }: { profile: ProfileLike | null; height?: number }) {
  const s = useMemo(() => (profile?.model ? profileSeries(profile) : null), [profile])
  if (!profile?.model || !s || !s.pts.length || s.total <= 0) return <div className="rounded-md border border-border bg-muted p-3 text-xs text-muted-foreground">검증을 통과한 프로파일이면 시간축 율 곡선을 그립니다</div>
  const W = 640, H = height, L = 40, R = 12, T = 12, B = 22
  const x = (t: number) => L + ((W - L - R) * t) / s.total
  const y = (r: number) => T + (H - T - B) * (1 - (s.peak ? r / s.peak : 0))
  const d = s.pts.map(([t, r], i) => `${i ? 'L' : 'M'}${x(t).toFixed(1)},${y(r).toFixed(1)}`).join(' ')
  const area = `${d} L${x(s.total).toFixed(1)},${y(0)} L${x(0)},${y(0)} Z`
  const ticks = [0, 0.25, 0.5, 0.75, 1].map(f => f * s.total)
  const est = estimateDuration(profile, s.peak, null, 0)
  return (
    <div className="flex flex-col gap-1 rounded-md border border-border bg-card p-2 text-xs">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-0.5 text-muted-foreground">
        <span className="font-semibold uppercase tracking-wide">율 곡선 · {profile.model}</span>
        <span>피크 <b className="font-mono text-foreground">{fmtNum(s.peak, 1)}</b> SApS</span>
        <span>부하 구간 <b className="font-mono text-foreground">{dur(s.total)}</b>{est != null ? <> · 예상 소요 ≈ <b className="font-mono text-foreground">{dur(est)}</b></> : null}</span>
        <span>총 시도 ≈ <b className="font-mono text-foreground">{fmtNum(s.attempts, 0)}</b></span>
        {profile.ht != null && <span>ht <b className="font-mono text-foreground">{profile.ht}</b> s → 동시 ≈ <b className="font-mono text-foreground">{fmtNum(s.peak * profile.ht, 0)}</b></span>}
        {profile.model === 'step' && <span>단계 {Math.max(1, Math.floor(((profile.max ?? 0) - (profile.start ?? 0)) / (profile.step || 1)) + 1)} × hold {profile.hold_s} s</span>}
      </div>
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ height }} role="img" aria-label="부하 프로파일 율 곡선">
        {[0, 0.5, 1].map(f => <line key={f} x1={L} x2={W - R} y1={y(f * s.peak)} y2={y(f * s.peak)} stroke="var(--border)" strokeDasharray={f ? '3 3' : undefined} />)}
        {[0.5, 1].map(f => <text key={f} x={L - 4} y={y(f * s.peak) + 3} textAnchor="end" fontSize="10" fill="var(--muted-foreground)">{fmtNum(f * s.peak, 1)}</text>)}
        <path d={area} fill="var(--chart-1)" opacity={0.15} />
        <path d={d} fill="none" stroke="var(--chart-1)" strokeWidth={1.6} />
        {ticks.map(t => <g key={t}><line x1={x(t)} x2={x(t)} y1={y(0)} y2={y(0) + 4} stroke="var(--muted-foreground)" /><text x={x(t)} y={H - 6} textAnchor="middle" fontSize="10" fill="var(--muted-foreground)">{dur(t)}</text></g>)}
      </svg>
    </div>
  )
}
