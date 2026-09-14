package com.cims.ue.core.net

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 서버(peer) 인증서 만료 관측 — SDK 코어 `cimsue::TlsPeerExpiry`(sdk/core/include/cimsue/types.h)와
 * 같은 모양·같은 임계. 단말은 서버 인증서를 갱신할 수 없으므로 이 값은 **표시 전용**이다 — 잔여가
 * 임계 아래면 서버쪽 자동 갱신(agent 일일 스윕)이 실패했다는 신호라 운영자에게 알리는 용도
 * (sip_tls_signaling.md §8.6.2 만료 안내 3단 중 단말 표면).
 *
 * 임계는 서버와 하나다(`agent/lib/cert.sh` CERT_WARN_DAYS/CERT_CRIT_DAYS): 잔여 ≤ 30일 = 경고,
 * ≤ 7일 = 위험. 갱신 계기 60일은 서버 내부값이라 단말 표시 임계가 아니다.
 *
 * 관측은 두 접속에서 온다 — SIP TLS(pjsua2 `onTransportState` 의 remoteCertInfo)와 CSC HTTPS
 * (OkHttp 응답의 handshake peer 인증서). 둘 다 [TlsPeerObserver] 가 모은다.
 */
data class TlsPeerExpiry(
    /** 인증서 notAfter, UTC epoch 초. */
    val notAfterEpoch: Long,
    /** 관측 시각, UTC epoch 초. */
    val observedEpoch: Long,
    /** peer 인증서 subject(한 줄). */
    val subject: String,
    /** 관측한 상대 `host:port`. */
    val remote: String,
) {
    enum class Level { OK, WARNING, CRITICAL }

    /** 잔여 일수(정수 절사). 만료됐으면 음수. */
    fun daysLeft(nowEpoch: Long = nowEpoch()): Int = ((notAfterEpoch - nowEpoch) / 86400L).toInt()

    fun level(nowEpoch: Long = nowEpoch()): Level {
        val d = daysLeft(nowEpoch)
        return when {
            d <= CRIT_DAYS -> Level.CRITICAL
            d <= WARN_DAYS -> Level.WARNING
            else -> Level.OK
        }
    }

    /** 만료일 `yyyy-MM-dd`(단말 시간대). */
    fun notAfterDate(): String =
        SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date(notAfterEpoch * 1000L))

    companion object {
        /** 경고 임계(일) — 서버 A-PRC-009 경고 임계와 같은 30. */
        const val WARN_DAYS = 30
        /** 위험 임계(일) — 서버 critical 과 같은 7. */
        const val CRIT_DAYS = 7

        fun nowEpoch(): Long = System.currentTimeMillis() / 1000L

        /** 두 관측 중 먼저 만료되는 쪽 — 표시는 가장 급한 것 하나로(Windows 관제 앱과 같은 규칙). */
        fun worst(a: TlsPeerExpiry?, b: TlsPeerExpiry?): TlsPeerExpiry? = when {
            a == null -> b
            b == null -> a
            b.notAfterEpoch < a.notAfterEpoch -> b
            else -> a
        }
    }
}
