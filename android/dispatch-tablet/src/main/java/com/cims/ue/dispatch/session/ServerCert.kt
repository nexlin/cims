// 서버 인증서 만료 안내 (docs/design/features/sip_tls_signaling.md §8.6.2, android_dispatch_tablet.md §6.2a-3)
//
// 서버 leaf 는 60일에 자동 갱신된다 — **30일 경고가 뜨는 것 자체가 자동 갱신 실패**다. 관제사는 매일 앉아 있는
// 사람이라 폐쇄망에서 가장 확실한 채널이다. 앱은 고칠 수 없고 알릴 뿐이다(운영자 → 콘솔 알람 A-PRC-009).
//
// 판정은 순수 함수라 기기 없이 시험한다(데스크톱 `DispatchSession.ServerCert*` 와 같은 규칙).
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.TlsPeerExpiry
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 만료 안내 단계 — 서버 A-PRC-009 warning/critical 과 같은 계단. */
enum class CertLevel { OK, WARN, CRITICAL }

internal object ServerCert {
    /** 경고 임계(일) — 서버 A-PRC-009 warning 과 같은 30. 자동 갱신 대상이 여기 닿았다 = 자동 갱신 실패. */
    const val WARN_DAYS = 30
    /** 위험 임계(일) — 서버 critical 과 같은 7. 배너가 진한 빨강이 된다. */
    const val CRITICAL_DAYS = 7

    /** 잔여 일수 — 코어와 같은 셈(`types.h` `daysLeft` — 초 차이를 86400 으로 나눈 몫, 음수 = 만료). */
    fun daysLeft(e: TlsPeerExpiry, nowSec: Long): Int = ((e.notAfterEpoch - nowSec) / 86_400L).toInt()

    /** SIP TLS·HTTPS 중 **먼저 만료되는 것**. 관측이 없는 쪽(평문·미접속)은 빠진다. 둘 다 없으면 null. */
    fun worst(a: TlsPeerExpiry?, b: TlsPeerExpiry?): TlsPeerExpiry? =
        listOfNotNull(a, b).filter { it.valid }.minByOrNull { it.notAfterEpoch }

    fun level(e: TlsPeerExpiry, nowSec: Long): CertLevel = daysLeft(e, nowSec).let {
        when {
            it <= CRITICAL_DAYS -> CertLevel.CRITICAL
            it <= WARN_DAYS -> CertLevel.WARN
            else -> CertLevel.OK
        }
    }

    fun title(e: TlsPeerExpiry, nowSec: Long): String = daysLeft(e, nowSec).let {
        when {
            it < 0 -> "서버 인증서 만료됨"
            it == 0 -> "서버 인증서 오늘 만료"
            else -> "서버 인증서 ${it}일 후 만료"
        }
    }

    /** 배너 본문 — 운영자에게 그대로 전할 사실(어느 서버·어느 인증서·만료일)과 뜻(자동 갱신 실패·볼 알람). */
    fun subtitle(e: TlsPeerExpiry): String =
        "${e.remote} · ${e.subject} · 만료 ${day(e)} · 자동 갱신 실패 신호 — 운영자에게 알리세요 " +
            "(콘솔 알람 A-PRC-009 cert/…/renew)"

    /** 만료일(현지 날짜). */
    fun day(e: TlsPeerExpiry): String =
        SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(Date(e.notAfterEpoch * 1000L))
}
