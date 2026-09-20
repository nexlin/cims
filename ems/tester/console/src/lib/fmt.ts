// 계측기 팩 표시 헬퍼 — 수치·시각·판정 톤. 표 셀 문구는 전부 여기서 나온다(화면마다 다른 소수 자릿수를 막는다).
import type { BadgeTone } from '@core/components/ui/badge'
import type { StatusTone } from '@core/components/custom/status-dot'
import type { Verdict, Summary } from '@tester/api/tester'

export function fmtNum(v: unknown, digits = 2): string {
  if (v === null || v === undefined || v === '') return '—'
  if (typeof v === 'number') {
    if (!isFinite(v)) return '—'
    return Number.isInteger(v) ? String(v) : v.toFixed(digits)
  }
  return String(v)
}

export function fmtPct(v: unknown): string {
  return typeof v === 'number' ? `${v.toFixed(2)} %` : '—'
}

export function fmtMs(v: unknown): string {
  return typeof v === 'number' ? `${v.toFixed(0)} ms` : '—'
}

/** ISO → HH:MM:SS (같은 날) 또는 MM-DD HH:MM */
export function fmtTime(iso: string | null | undefined, withDate = false): string {
  if (!iso) return '—'
  const d = new Date(iso)
  if (isNaN(d.getTime())) return iso
  const p = (n: number) => String(n).padStart(2, '0')
  const hms = `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
  return withDate ? `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${hms}` : hms
}

export function fmtUnix(t: number): string {
  return fmtTime(new Date(t * 1000).toISOString())
}

export function fmtDuration(startIso?: string | null, endIso?: string | null): string {
  if (!startIso) return '—'
  const a = new Date(startIso).getTime()
  const b = endIso ? new Date(endIso).getTime() : Date.now()
  if (isNaN(a) || isNaN(b)) return '—'
  const s = Math.max(0, Math.round((b - a) / 1000))
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  if (m < 60) return `${m}m ${s % 60}s`
  return `${Math.floor(m / 60)}h ${m % 60}m`
}

export const VERDICT_TONE: Record<Verdict, BadgeTone> = {
  running: 'infoSoft', pass: 'successSoft', fail: 'dangerSoft', aborted: 'warningSoft', error: 'dangerSoft',
}

export const VERDICT_LABEL: Record<Verdict, string> = {
  running: '진행 중', pass: 'PASS', fail: 'FAIL', aborted: '중단', error: '오류',
}

export function verdictDot(v: Verdict): StatusTone {
  return v === 'pass' ? 'success' : v === 'running' ? 'info' : v === 'aborted' ? 'warning' : 'danger'
}

export const STATE_LABEL: Record<string, string> = {
  starting: '준비', provisioning: '프로비저닝', running: '실행', stopping: '중단 중', stopped: '종료',
}

/** RFC 6076 요약 키 → 라벨 (컨트롤러 report_markdown 과 같은 순서·문구) */
export const SUMMARY_ROWS: [string, string, 'int' | 'pct' | 'num' | 'str'][] = [
  ['attempts', '호 시도(attempt)', 'int'], ['sessions', '세션(성립)', 'int'], ['completed', '완료(정상 BYE)', 'int'],
  ['failed', '실패', 'int'], ['skipped', '단말 부족으로 건너뜀', 'int'],
  ['ser_pct', 'SER (Session Establishment Ratio)', 'pct'], ['scr_pct', 'SCR (Session Completion Ratio)', 'pct'],
  ['seer_pct', 'SEER (유효 성립률 — 거절 480/486/600/603 포함)', 'pct'], ['isa_pct', 'ISA (부적절 시도 — 408/500/503/504·Timer B)', 'pct'],
  ['mos_mean', 'MOS 추정 평균 (G.107)', 'num'], ['mos_min', 'MOS 추정 최솟값(최악 leg)', 'num'], ['rtcp_rx', 'RTCP SR/RR 수신', 'int'],
  ['registered_ok', '등록 성공', 'int'], ['registered_fail', '등록 실패', 'int'], ['doc_saps', 'DOC (SApS)', 'num'],
  ['rtp_rx', 'RTP 수신', 'int'], ['rtp_lost', 'RTP 손실', 'int'], ['rtp_loss_pct', 'RTP 손실 %', 'pct'],
  ['early_media_pct', 'early media 비율(183+SDP)', 'pct'], ['prack_pct', 'PRACK 비율', 'pct'],
  ['hold_resume_pct', 'hold/resume 성공 비율', 'pct'], ['refer_pct', 'REFER 성공 비율', 'pct'],
  ['q850_causes', 'Q.850 cause', 'str'], ['refer_codes', 'REFER 응답 코드', 'str'], ['codes', '응답 코드', 'str'],
  ['real_legs', '실단말(real-ue) leg', 'int'], ['real_rtp_loss_pct', '실단말 RTP 손실 %', 'pct'], ['real_mos_mean', '실단말 MOS 추정 평균', 'num'], ['real_mos_min', '실단말 MOS 추정 최솟값', 'num'],
]

export const TIMER_ROWS: [string, string][] = [
  ['rrd_ms', 'RRD (등록 지연) ms'], ['srd_ms', 'SRD (세션 요청 지연) ms'], ['sdd_ms', 'SDD (세션 해제 지연) ms'],
  ['sdt_s', 'SDT (세션 지속) s'], ['jitter_ms', '지터 ms'], ['rtp_loss_pct', 'RTP 손실 %(호별)'], ['mos', 'MOS 추정 (G.107, min 이 판정)'],
  ['real_srd_ms', '실단말 SRD ms'], ['real_jitter_ms', '실단말 지터 ms'], ['real_rtp_loss_pct', '실단말 RTP 손실 %(호별)'], ['real_mos', '실단말 MOS 추정 (min 이 판정)'],
]

export const STEP_LABEL: Record<string, string> = {
  register: '등록', invite: '발신 INVITE', answer: '응답(200)', progress: '183 early media', hold: 'hold(re-INVITE)',
  resume: 'resume(re-INVITE)', dtmf: 'DTMF(RFC 4733)', refer: 'REFER 전달', media_hold: '미디어 유지', bye: 'BYE',
  reject: '거절', deregister: '등록 해제', group_call: 'PTT 그룹콜', floor_request: 'floor 요청', floor_release: 'floor 해제',
  pickup: '당겨받기', replaces: 'INVITE-Replaces', join: 'INVITE-Join 청취', subscribe: 'SUBSCRIBE', publish: 'PUBLISH affiliation',
  media_send: 'RTP 송출', media_stop: '송출 정지', sds_send: 'SDS 송신', sds_recv: 'SDS 수신', fd_send: 'FD 파일 배포', fd_recv: 'FD 수신·다운로드',
  wait: '대기', expect: '누계 게이트',
}

export function summaryValue(s: Summary | undefined, key: string, kind: 'int' | 'pct' | 'num' | 'str'): string {
  const v = s?.[key]
  if (kind === 'pct') return fmtPct(v)
  if (kind === 'int') return fmtNum(v, 0)
  if (kind === 'num') return fmtNum(v)
  return v === null || v === undefined ? '—' : String(v)
}

