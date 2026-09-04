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

// ── 이벤트 (wire 슬러그 → 라벨) ──────────────────────────────────────────────
//   **키는 `alarm_catalog.csv` 의 감지 행 `type`** 이다 — 모듈이 실제로 실어 보내는 값.
//   정의 행의 type(lifecycle·config_changed·ha_transition 같은 요구 분류)은 레코드에 실리지
//   않으므로 키가 아니다. 두 어휘를 섞으면 화면이 오지 않는 값에 라벨을 달고, 오는 값은
//   놓친다.
//   라벨이 없으면 `eventTypeLabel` 이 **슬러그를 그대로 반환**한다 — 에러 없이 영어가 뜨므로
//   구현 전인 감지 슬러그도 미리 채워 둔다(CSV 에 '후보'로 적힌 것 포함).
export const EVENT_TYPE_LABEL: Record<string, string> = {
  // 상태 변화 (E-STC-*)
  process_started: '프로세스 기동',
  process_stopping: '프로세스 종료',
  process_died: '프로세스 소멸',
  module_restarted: '모듈 자동 재기동',
  config_reloaded: '설정 재적재',
  ha_role_changed: 'HA 역할 전이',
  ha_switchover: 'HA 절체',
  csp_control_peer_changed: '제어 피어 변경',
  db_fallback_entered: 'DB 폴백 진입',
  db_fallback_exited: 'DB 폴백 해제',
  node_offline: '노드 관측 두절',
  // node_online 은 CSV 에서 node_offline 행이 함께 적고 있는 짝 슬러그(관측 두절/복귀 전이).
  node_online: '노드 관측 복귀',
  // 감사 (E-AUD-*)
  service_control: '서비스 제어',
  auth_audit: '인증 감사',
  node_maintenance: '운영 개입 전이',
  config_change: '설정 변경',
  user_config_changed: '가입자 설정 변경',
  group_config_changed: '그룹 설정 변경',
  listener_added: '접속점 추가',
  listener_removed: '접속점 제거',
  session_reclaimed: '세션 회수',
  cmp_session_reclaimed: 'CMP 세션 회수',
  cmp_endpoint_added: 'CMP 노드 추가',
  csc_resync_requested: '가입자 전량 재동기',
  csp_resync_sent: '재동기 요청 발신',
  deploy_job: '배포 job 결과',
  deployment_failed: '배포 실패',
  // 구 슬러그 하위호환 (알람 쪽과 같은 방식 — 옛 레코드를 읽을 때 필요)
  //   control_peer_changed → csp_control_peer_changed 로 개정됨.
  //   catalog_registered 는 CSV 에 감지 행이 없다(발화 주체 없음) — 지우면 옛 레코드가
  //   영어로 뜨므로 남긴다.
  control_peer_changed: '제어 피어 변경',
  call_monitored: '통화 감청(청취)',
  catalog_registered: '카탈로그 등록',
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

// 심각도 표시명 — 활성 알람 타일(위젯 1장 = 심각도 1종)의 라벨·위젯 제목이 같은 표를 쓴다.
export const SEVERITY_LABEL: Record<string, string> = {
  critical: 'Critical', major: 'Major', minor: 'Minor', warning: 'Warning', indeterminate: 'Indeterminate',
}

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
