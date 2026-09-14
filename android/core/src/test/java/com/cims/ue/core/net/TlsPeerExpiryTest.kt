package com.cims.ue.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

/** 서버 인증서 만료 관측 — 임계(30/7, 서버 cert.sh 와 동일)·잔여 일수·가장 급한 것 선택. */
class TlsPeerExpiryTest {

    private val now = 1_800_000_000L
    private fun at(days: Int) = TlsPeerExpiry(now + days * 86400L, now, "CN=ctrl01", "10.0.0.1:5061")

    @Test fun daysLeftTruncates() {
        assertEquals(45, at(45).daysLeft(now))
        assertEquals(0, TlsPeerExpiry(now + 3600, now, "", "").daysLeft(now))   // 오늘 만료
        assertEquals(-2, at(-2).daysLeft(now))                                   // 이미 만료
    }

    @Test fun thresholdsMatchServer() {
        assertEquals(30, TlsPeerExpiry.WARN_DAYS)
        assertEquals(7, TlsPeerExpiry.CRIT_DAYS)
        assertEquals(TlsPeerExpiry.Level.OK, at(31).level(now))
        assertEquals(TlsPeerExpiry.Level.WARNING, at(30).level(now))
        assertEquals(TlsPeerExpiry.Level.WARNING, at(8).level(now))
        assertEquals(TlsPeerExpiry.Level.CRITICAL, at(7).level(now))
        assertEquals(TlsPeerExpiry.Level.CRITICAL, at(0).level(now))
        assertEquals(TlsPeerExpiry.Level.CRITICAL, at(-1).level(now))
    }

    @Test fun worstPicksEarliestExpiry() {
        val sip = at(400); val csc = at(20)
        assertSame(csc, TlsPeerExpiry.worst(sip, csc))
        assertSame(csc, TlsPeerExpiry.worst(csc, sip))
        assertSame(sip, TlsPeerExpiry.worst(sip, null))
        assertSame(csc, TlsPeerExpiry.worst(null, csc))
        assertNull(TlsPeerExpiry.worst(null, null))
    }

    @Test fun notAfterDateIsIsoDay() {
        // 2027-01-15T00:00:00Z = 1800057600 — 시간대에 따라 하루 차이는 허용하지 않고 형식만 본다.
        val d = TlsPeerExpiry(1_800_057_600L + 12 * 3600, now, "", "").notAfterDate()
        assertEquals(10, d.length)
        assertEquals('-', d[4]); assertEquals('-', d[7])
    }
}
