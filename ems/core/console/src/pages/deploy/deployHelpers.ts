import type { Agent, Deployment } from '../../api/deployment'

export const SERVICE_KINDS = [
  'csp', 'cmp', 'psp', 'pmp', 'isp', 'imp',
  'csc', 'console', 'phone', 'cwrtc',
]

export function agentStatusColor(s: Agent['status']) {
  const m: Record<Agent['status'], { bar: string; border: string }> = {
    pending:  { bar: '#f39c12', border: '#fce8cc' },
    approved: { bar: '#3498db', border: '#d6e9f7' },
    online:   { bar: '#2ecc71', border: '#cfeee0' },
    offline:  { bar: '#95a5a6', border: '#dde2e3' },
    error:    { bar: '#e74c3c', border: '#f6d2cf' },
    revoked:  { bar: '#7f8c8d', border: '#d3d7d8' },
  }
  return m[s] || m.offline
}

export function depStatusColor(s: Deployment['status']) {
  return {
    pending: '#f39c12', deploying: '#3498db', running: '#2ecc71',
    stopped: '#95a5a6', failed: '#e74c3c', removed: '#7f8c8d',
  }[s] || '#bbb'
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
