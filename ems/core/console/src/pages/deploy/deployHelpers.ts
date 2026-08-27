import type { Agent, Deployment } from '../../api/deployment'

export const SERVICE_KINDS = [
  'csp', 'cmp', 'psp', 'pmp', 'isp', 'imp',
  'csc', 'console', 'phone', 'cwrtc',
]

export function agentStatusColor(s: Agent['status']) {
  const m: Record<Agent['status'], { bar: string; border: string }> = {
    pending:  { bar: '#b45309', border: 'var(--border)' },
    approved: { bar: '#3498db', border: '#d6e9f7' },
    online:   { bar: '#15803d', border: '#cfeee0' },
    offline:  { bar: '#6b7280', border: 'var(--border)' },
    error:    { bar: '#e74c3c', border: '#f6d2cf' },
    revoked:  { bar: '#6b7280', border: 'var(--border)' },
  }
  return m[s] || m.offline
}

export function depStatusColor(s: Deployment['status']) {
  return {
    pending: '#b45309', deploying: '#2563eb', running: '#15803d',
    stopped: '#6b7280', failed: '#c0392b', removed: '#6b7280',
  }[s] || '#bbb'
}

// 실측(live_state) 최우선 유효 상태 — 실제로 떠 있을 때만 running, 안 떠 있으면 절대
// running 으로 보이지 않는다. status(job 결과=의도)는 실측이 없을 때만(오프라인·최초 보고
// 전) 노출. reconcile 이 마스터에서 켠 cold 모듈은 job 없이도 실측(up)으로 running 이 되고,
// 백업은 실측 down 이라 stopped. deploying/pending/failed(진행/실패)는 실측 up 이 아닐 때
// 그대로 보여 "명령 수행 중"을 알린다.
export function depEffectiveStatus(d: Deployment): Deployment['status'] {
  if (d.live_state === 'up') return 'running'
  if (d.status === 'deploying' || d.status === 'pending' ||
      d.status === 'failed' || d.status === 'removed') return d.status
  if (d.live_state === 'down') return 'stopped'
  return d.status
}

export function fmtRelTime(iso: string | null) {
  if (!iso) return '—'
  const d = new Date(iso)
  const delta = Math.floor((Date.now() - d.getTime()) / 1000)
  if (delta < 60)    return `${delta}초 전`
  if (delta < 3600)  return `${Math.floor(delta/60)}분 전`
  if (delta < 86400) return `${Math.floor(delta/3600)}시간 전`
  return d.toLocaleDateString('ko-KR')
}

export function fmtSize(n: number) {
  if (n < 1024) return `${n} B`
  if (n < 1024*1024) return `${(n/1024).toFixed(1)} KB`
  if (n < 1024*1024*1024) return `${(n/1024/1024).toFixed(1)} MB`
  return `${(n/1024/1024/1024).toFixed(2)} GB`
}

export function fmtSpeed(bps: number) {
  if (bps < 1024) return `${bps.toFixed(0)} B/s`
  if (bps < 1024 * 1024) return `${(bps / 1024).toFixed(0)} KB/s`
  return `${(bps / 1024 / 1024).toFixed(1)} MB/s`
}

export function fmtEta(sec: number) {
  if (!sec || sec <= 0) return '—'
  if (sec < 1) return '<1s'
  if (sec < 60) return `${sec.toFixed(0)}s`
  return `${Math.floor(sec/60)}m ${Math.floor(sec%60)}s`
}
