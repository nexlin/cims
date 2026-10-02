// 유지 평면의 순수 규칙 — 등록에 묶인 제휴·구독을 언제 다시 싣는가 (android_dispatch_tablet.md §6.7a)
//
// 어긋나면 관제사가 모르는 채로 망가진다: 망이 끊겼다 돌아온 뒤 [참여] 가 «그룹 멤버가 아닙니다» 로 거절되고(서버가 제휴를 내렸다),
// 한 시간 뒤 로스터·회선 감시가 조용히 멎는다(수명 3600 초). 세션(엔진·서버) 없이 판정만 고정한다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.UpkeepRules
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class UpkeepTest {

    // ── 등록 이벤트 → 다시 세울 것인가 ──
    @Test fun `처음 등록은 다시 세우지 않는다 - 로그인 절차가 건다`() {
        assertFalse(UpkeepRules.renewed(was = null, registered = true, networkChanged = false))
    }

    @Test fun `등록 갱신은 다시 세우지 않는다`() {
        assertFalse(UpkeepRules.renewed(was = true, registered = true, networkChanged = false))
    }

    @Test fun `끊겼다 다시 선 등록은 다시 세운다`() {
        assertTrue(UpkeepRules.renewed(was = false, registered = true, networkChanged = false))
    }

    @Test fun `망이 바뀐 뒤의 등록 성공은 등록됨에서 등록됨이어도 다시 세운다`() {
        assertTrue(UpkeepRules.renewed(was = true, registered = true, networkChanged = true))
        assertTrue(UpkeepRules.renewed(was = null, registered = true, networkChanged = true))
    }

    @Test fun `등록 실패는 세울 것이 없다`() {
        assertFalse(UpkeepRules.renewed(was = true, registered = false, networkChanged = true))
        assertFalse(UpkeepRules.renewed(was = false, registered = false, networkChanged = false))
    }

    // ── 수명 절반 갱신(RFC 3903 §4.1 · RFC 6665 §4.1.2.2) ──
    @Test fun `실은 적이 없으면 싣는다`() {
        assertTrue(UpkeepRules.due(atMs = 0, nowMs = 10))
    }

    @Test fun `수명 절반 전에는 다시 싣지 않고 절반이 지나면 싣는다`() {
        val at = 1_000L
        val half = UpkeepRules.LIFETIME_SEC * 500L
        assertFalse(UpkeepRules.due(at, at + half - 1))
        assertTrue(UpkeepRules.due(at, at + half))
    }

    // ── 거절·무응답 뒤 물러남 ──
    @Test fun `재시도는 1분부터 배로 늘고 30분에서 멈춘다`() {
        assertEquals(60_000L, UpkeepRules.retryDelayMs(1))
        assertEquals(120_000L, UpkeepRules.retryDelayMs(2))
        assertEquals(240_000L, UpkeepRules.retryDelayMs(3))
        assertEquals(30 * 60_000L, UpkeepRules.retryDelayMs(6))
        assertEquals(30 * 60_000L, UpkeepRules.retryDelayMs(40))
        assertEquals(60_000L, UpkeepRules.retryDelayMs(0))          // 방어 — 셈이 어긋나도 음수 시프트가 되지 않는다
    }

    // ── [참여] 403 → 제휴를 다시 싣고 한 번 더 ──
    @Test fun `멤버 그룹의 403 은 제휴를 다시 싣고 한 번 더 건다`() {
        assertTrue(UpkeepRules.rejoin(code = 403, member = true, lastRejoinAtMs = 0, nowMs = 5_000))
    }

    @Test fun `멤버가 아닌 그룹의 403 과 다른 거절은 그대로 알린다`() {
        assertFalse(UpkeepRules.rejoin(code = 403, member = false, lastRejoinAtMs = 0, nowMs = 5_000))
        assertFalse(UpkeepRules.rejoin(code = 404, member = true, lastRejoinAtMs = 0, nowMs = 5_000))
        assertFalse(UpkeepRules.rejoin(code = 488, member = true, lastRejoinAtMs = 0, nowMs = 5_000))
    }

    @Test fun `다시 건 호까지 403 이면 되풀이하지 않는다`() {
        val at = 100_000L
        assertFalse(UpkeepRules.rejoin(403, true, lastRejoinAtMs = at, nowMs = at + 500))
        assertFalse(UpkeepRules.rejoin(403, true, lastRejoinAtMs = at, nowMs = at + UpkeepRules.REJOIN_GAP_MS - 1))
        assertTrue(UpkeepRules.rejoin(403, true, lastRejoinAtMs = at, nowMs = at + UpkeepRules.REJOIN_GAP_MS))
    }
}
