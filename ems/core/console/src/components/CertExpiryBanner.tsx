import { useAlarms, severityOf, SEV_RANK } from '../widgets/useAlarms'
import type { ActiveAlarm } from '../widgets/useAlarms'

/**
 * 서버 인증서 만료 경고 배너 (sip_tls_signaling.md §8.6.2 — 만료 안내 3단 중 콘솔 표면).
 *
 * 활성 `cert_expiring`(A-PRC-009) 알람이 하나라도 있으면 모든 라우트 상단에 상시 표시한다.
 * 닫기 없음 — 알람이 close 되면(갱신 성공·인증서 교체) 저절로 사라진다. 임계는 엔진과 같다
 * (경고 30일 / 위험 7일 — `agent/lib/cert.sh` 단일 정의): 활성 알람 중 최고 심각도가
 * critical/major 면 위험 톤, 그 외는 경고 톤. 데이터는 셸이 이미 구독 중인 알람 스토어
 * (`useAlarms`)를 그대로 쓴다 — 별도 폴링 없음.
 *
 * 두 축을 문구로 구분한다 — 자동 갱신 실패(`<host>/agent/cert/<module>/renew`, 기존 인증서
 * 유지 중)와 만료 임박(그 외 `cert/...` mo — CSP 접속점·CSC/OAM https·CA). 남은 일수는
 * `threshold_info.observed`(단위 일).
 */
const RENEW_MO = /\/agent\/cert\/[^/]+\/renew$/
const MAX_ITEMS = 3

function isCertAlarm(a: ActiveAlarm): boolean {
  return a.type === 'cert_expiring' || a.code === 'A-PRC-009'
}

function itemText(a: ActiveAlarm): string {
  const mo = a.source?.mo_label || a.source?.mo_instance || a.type
  const days = a.threshold_info?.observed
  const daysText = typeof days === 'number' ? `${days}일 남음` : ''
  const axis = RENEW_MO.test(a.source?.mo_instance || '') ? '자동 갱신 실패' : '만료 임박'
  return [mo, axis, daysText].filter(Boolean).join(' · ')
}

export default function CertExpiryBanner() {
  const { active } = useAlarms()
  const certs = active.filter(isCertAlarm)
  if (certs.length === 0) return null

  const worst = Math.max(...certs.map(a => SEV_RANK[severityOf(a)] ?? 0))
  const danger = worst >= SEV_RANK.major
  const shown = certs.slice(0, MAX_ITEMS)
  const more = certs.length - shown.length

  return (
    <div
      role="alert"
      className={
        danger
          ? 'bg-destructive text-destructive-foreground py-2 px-4 text-md leading-[1.5] flex gap-3 items-baseline'
          : 'bg-warning-on text-card py-2 px-4 text-md leading-[1.5] flex gap-3 items-baseline'
      }
    >
      <strong className="whitespace-nowrap">서버 인증서 만료 경고</strong>
      <span>
        {shown.map((a, i) => (
          <span key={a.alarm_id || `${a.type}-${i}`}>
            {i > 0 && ' / '}
            <code className="font-mono">{itemText(a)}</code>
          </span>
        ))}
        {more > 0 && ` 외 ${more}건`}
        {' — '}
        자동 갱신이 실패한 인증서는 기존 것을 그대로 쓰는 중입니다. 만료 전에 갱신 실패 원인을
        해소하거나 인증서를 교체하세요(활성 알람의 권고 조치 참조).
      </span>
    </div>
  )
}
