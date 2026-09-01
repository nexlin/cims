// 알람/이벤트 표시 어휘 — 단일 정의 (AlertsPage / ActiveAlarmsPage / AlarmCatalogPage 공유).
//   클래스(type) 한글 라벨은 alarm_catalog.md §4 의 25클래스 + 구 슬러그 하위호환.
//   심각도 배지/서열은 X.733 perceived severity 6단계.

// ── 알람 조건 클래스 (25종, alarm_catalog.md §4) ─────────────────────────────
export const ALARM_TYPE_LABEL: Record<string, string> = {
  // COM
  connection_lost: '연결 끊김',
  delivery_failed: '전달 실패',
  // QOS
  capacity_threshold: '용량 임계',
  quality_degraded: '품질 저하',
  threshold_crossed: '임계 초과',
  safety_critical_failure: '안전 기능 실패',
  resource_exhausted: '자원 고갈',
  capacity_degraded: '용량 미달',
  resource_leak: '자원 누수',
  overload: '과부하 방어',
  // PRC
  process_down: '프로세스 다운',
  process_unresponsive: '프로세스 무응답',
  worker_unavailable: '실행 단위 정지',
  crash_loop: '재기동 반복 실패',
  listener_unavailable: '접속점 불능',
  storage_failure: '저장소 장애',
  retention_failure: '보존 기록 실패',
  config_invalid: '설정 결함',
  config_out_of_sync: '설정 불일치',
  state_out_of_sync: '상태 부정합',
  redundancy_degraded: '이중화 저하',
  dependency_unavailable: '의존물 부재',
  observability_lost: '관측 공백',
  cert_expiring: '인증서 만료 임박',
  // SEC
  security_violation: '보안 위반 징후',
  // 구 type (하위호환 표시 — 레코드/규칙 read 시)
  csp_down: '프로세스 다운', cmp_down: '프로세스 다운', module_down: '프로세스 다운',
  db_down: '연결 끊김', rtp_high: '임계 초과', disk_high: '임계 초과',
  service_unresponsive: '프로세스 무응답',
}

export function alarmTypeLabel(t?: string): string {
  return (t && ALARM_TYPE_LABEL[t]) || t || '-'
}

// ── 이벤트 (wire 정의 슬러그 → 라벨) ─────────────────────────────────────────
export const EVENT_TYPE_LABEL: Record<string, string> = {
  process_started: '프로세스 기동',
  process_stopping: '프로세스 종료',
  process_died: '프로세스 소멸',
  config_reloaded: '설정 재적재',
  catalog_registered: '카탈로그 등록',
  session_reclaimed: '세션 회수',
  control_peer_changed: '제어 피어 변경',
}

export function eventTypeLabel(t?: string): string {
  return (t && EVENT_TYPE_LABEL[t]) || t || '-'
}

export const EVENT_KIND_LABEL: Record<string, string> = {
  stateChange: '상태 변화',
  audit: '감사',
}

// ── 심각도 (X.733 6단계) ─────────────────────────────────────────────────────
export const SEVERITY_ORDER = ['critical', 'major', 'minor', 'warning', 'indeterminate'] as const

export function sevBadgeClass(sev?: string): string {
  switch (sev) {
    case 'critical': return 'badge--red'
    case 'major': return 'badge--red'
    case 'minor': return 'badge--yellow'
    case 'warning': return 'badge--yellow'
    case 'indeterminate': return 'badge--blue'
    case 'cleared': return 'badge--gray'
    default: return 'badge--blue'
  }
}

export function severityOf(e: { perceived_severity?: string; severity?: string }): string {
  return e.perceived_severity || e.severity || 'warning'
}

// ── 시각/기간 포맷 ───────────────────────────────────────────────────────────
export function fmtTime(iso?: string): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleString('ko-KR', {
    year: '2-digit', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}

export function formatSec(sec: number): string {
  if (sec < 0) return '-'
  if (sec < 60) return `${sec}초`
  if (sec < 3600) return `${Math.floor(sec / 60)}분 ${sec % 60}초`
  if (sec < 86400) return `${Math.floor(sec / 3600)}시간 ${Math.floor((sec % 3600) / 60)}분`
  return `${Math.floor(sec / 86400)}일 ${Math.floor((sec % 86400) / 3600)}시간`
}

export function durationBetween(openTs?: string, closeTs?: string): string {
  const o = new Date(openTs || '').getTime()
  const c = new Date(closeTs || '').getTime()
  if (isNaN(o) || isNaN(c) || c < o) return '-'
  return formatSec(Math.round((c - o) / 1000))
}

// ── CSV 내보내기 (필터 결과 그대로) ──────────────────────────────────────────
export function downloadCsv(filename: string, header: string[], rows: (string | number | null | undefined)[][]) {
  const esc = (v: string | number | null | undefined) => {
    const s = v == null ? '' : String(v)
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s
  }
  const text = [header, ...rows].map(r => r.map(esc).join(',')).join('\n')
  const blob = new Blob(['﻿' + text], { type: 'text/csv;charset=utf-8' })   // BOM — Excel 한글
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = filename
  a.click()
  URL.revokeObjectURL(a.href)
}
