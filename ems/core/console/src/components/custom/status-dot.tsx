import { cn } from '@core/lib/utils'

/**
 * StatusDot — **살아있는 상태**(프로세스·노드의 현재 상태) 표시.
 * 값·분류는 Badge 를 쓴다. 둘을 섞지 않는다.
 *
 * 정본 = Figma `02 Components` Sec/StatusDot (17:18) · 톤 매핑은 DESIGN-RULES §2 고정:
 *   Success = online·running·mounted / Info = approved(등록됐으나 heartbeat 없음)
 *   Neutral = stopped·미설정        / Warning = 드리프트·미적용(**0건이면 Neutral**)
 *   Danger  = offline·unreachable·critical
 */
export type StatusTone = 'success' | 'info' | 'neutral' | 'warning' | 'danger'

const TONE: Record<StatusTone, string> = {
  success: 'bg-success',
  info: 'bg-info',
  neutral: 'bg-neutral',
  warning: 'bg-warning',
  danger: 'bg-destructive',
}

/** 서버/모듈 상태 문자열 → 톤. 새 상태가 생기면 여기만 고친다. */
export function toneForStatus(status: string): StatusTone {
  switch (status) {
    case 'online': case 'running': case 'mounted': case 'Active':
      return 'success'
    case 'approved':
      return 'info'
    case 'stopped': case 'Standby': case 'unset':
      return 'neutral'
    case 'drift':
      return 'warning'
    default:
      return 'danger'   // offline · unreachable · critical
  }
}

export function StatusDot({ status, label, tone, className }: {
  status?: string
  label?: string
  tone?: StatusTone
  className?: string
}) {
  const t = tone ?? toneForStatus(status ?? '')
  return (
    <span className={cn('inline-flex items-center gap-1.5 whitespace-nowrap', className)}>
      <span className={cn('size-1.5 shrink-0 rounded-full', TONE[t])} aria-hidden />
      <span className="text-sm text-muted-foreground">{label ?? status}</span>
    </span>
  )
}
