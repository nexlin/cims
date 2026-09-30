// 서버 인증서 만료 안내 — 단계·문구·두 관측 중 고르기 — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2a-3)
//
// 노리는 것은 **자동 갱신 실패를 놓치는** 결함이다 — 둘 중 늦게 만료되는 쪽을 보고 안심하는 것, 관측이 없는 쪽(평문
// 접속)을 «만료» 로 읽는 것, 임계가 서버(A-PRC-009 30/7)와 어긋나 콘솔 알람과 다른 날에 경고하는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.CertLevel
import com.cims.ue.dispatch.session.ServerCert
import com.cims.ue.sdk.TlsPeerExpiry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ServerCertTest {

    private val now = 1_800_000_000L
    private fun cert(days: Long, remote: String = "10.0.0.1:4430", valid: Boolean = true) =
        TlsPeerExpiry(valid, now + days * 86_400L + 3_600L, now, "CN=csc", remote)

    @Test fun `임계는 서버와 같다 — 30일 경고, 7일 위험`() {
        assertEquals(30, ServerCert.WARN_DAYS)
        assertEquals(7, ServerCert.CRITICAL_DAYS)
        assertEquals(CertLevel.OK, ServerCert.level(cert(31), now))
        assertEquals(CertLevel.WARN, ServerCert.level(cert(30), now))
        assertEquals(CertLevel.WARN, ServerCert.level(cert(8), now))
        assertEquals(CertLevel.CRITICAL, ServerCert.level(cert(7), now))
        assertEquals(CertLevel.CRITICAL, ServerCert.level(cert(-2), now))
    }

    @Test fun `먼저 만료되는 쪽을 본다 — 관측 없는 쪽은 빠진다`() {
        val sip = cert(40, "sip:5061"); val https = cert(12, "csc:4430")
        assertEquals("csc:4430", ServerCert.worst(sip, https)?.remote)
        assertEquals("sip:5061", ServerCert.worst(sip, cert(1, valid = false))?.remote)
        assertNull("둘 다 관측 전이면 없다", ServerCert.worst(null, cert(1, valid = false)))
    }

    @Test fun `문구 — 만료됨·오늘·N일 후`() {
        assertEquals("서버 인증서 12일 후 만료", ServerCert.title(cert(12), now))
        assertEquals("서버 인증서 오늘 만료", ServerCert.title(TlsPeerExpiry(true, now + 3_600L, now, "", ""), now))
        assertEquals("서버 인증서 만료됨", ServerCert.title(cert(-3), now))
    }

    @Test fun `잔여 일수는 코어와 같은 셈이다`() {
        // types.h daysLeft — 초 차이를 86400 으로 나눈 몫. 앱이 다르게 세면 콘솔 알람과 다른 날에 경고한다.
        assertEquals(12, ServerCert.daysLeft(cert(12), now))
        assertEquals(0, ServerCert.daysLeft(TlsPeerExpiry(true, now + 86_399L, now, "", ""), now))
    }
}
