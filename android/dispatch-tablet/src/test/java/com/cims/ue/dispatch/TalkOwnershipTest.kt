// 발언 소유권 — Codex 리뷰 F1·F2 반영 (android_dispatch_tablet.md §6.3)
//
// **해제는 «지금 대상» 이 아니라 «요청한 호» 로 한다.** 누른 채 대상이 바뀌면(Ctrl+n·자동 승격·세션 종료)
// 원래 호에 floorRelease 가 가지 않아 **마이크가 열린 채 남는다**. 실제 마이크 차단은 코어 floor
// participant 가 release 로 수행하므로 앱이 놓치면 송출이 계속된다.
//
// ViewModel 은 StateFlow·coroutine 을 타 JVM 시험이 무거우므로 **소유 규칙만** 같은 판정으로 검사한다.
package com.cims.ue.dispatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** PttChannelsViewModel 의 발언 소유 규칙과 **같은 판정**. 규칙이 갈라지면 여기가 먼저 깨진다. */
private class TalkOwner {
    /** 지금 floor 를 요청해 둔 호. */
    var speaking: Set<Int> = emptySet()
        private set
    /** 해제가 나간 호 — 시험이 관찰한다. */
    val released = mutableListOf<Int>()

    fun down(targetCallIds: Set<Int>) {
        if (targetCallIds.isEmpty()) return
        (speaking - targetCallIds).forEach { release(it) }
        speaking = targetCallIds
    }

    fun up() = releaseAll()

    fun releaseAll() {
        speaking.forEach { released += it }
        speaking = emptySet()
    }

    /** 대상이 바뀌었다 — 빠진 호는 바로 푼다. */
    fun retarget(stillTarget: Set<Int>) {
        (speaking - stillTarget).forEach { release(it) }
        speaking = speaking intersect stillTarget
    }

    /** 세션이 사라졌다 — 호가 없으니 release 는 의미 없고 소유만 뗀다. */
    fun prune(live: Set<Int>) { speaking = speaking intersect live }

    private fun release(id: Int) { released += id }
}

class TalkOwnershipTest {

    @Test fun `누른 뒤 떼면 요청한 호가 해제된다`() {
        val t = TalkOwner()
        t.down(setOf(10))
        t.up()
        assertEquals(listOf(10), t.released)
        assertTrue(t.speaking.isEmpty())
    }

    @Test fun `누른 채 대상이 바뀌면 원래 호가 풀린다`() {
        // ★ F1 의 핵심. 이게 없으면 A 의 마이크가 열린 채 남는다.
        val t = TalkOwner()
        t.down(setOf(10))          // A 에 발언 요청
        t.retarget(setOf(20))      // 대상이 B 로 바뀜
        assertEquals("A 가 해제돼야 한다", listOf(10), t.released)
        assertTrue(t.speaking.isEmpty())
    }

    @Test fun `모두 해제는 선택만 지우지 않고 발언도 푼다`() {
        val t = TalkOwner()
        t.down(setOf(10, 11))
        t.releaseAll()
        assertEquals(setOf(10, 11), t.released.toSet())
    }

    @Test fun `대상이 바뀐 채 다시 누르면 이전 요청이 먼저 풀린다`() {
        val t = TalkOwner()
        t.down(setOf(10))
        t.down(setOf(20))          // 떼지 않고 다른 대상으로
        assertEquals("이전 호가 먼저 풀려야 한다", listOf(10), t.released)
        assertEquals(setOf(20), t.speaking)
    }

    @Test fun `대상 일부만 바뀌면 빠진 것만 푼다`() {
        val t = TalkOwner()
        t.down(setOf(10, 11, 12))
        t.retarget(setOf(10, 12))
        assertEquals(listOf(11), t.released)
        assertEquals(setOf(10, 12), t.speaking)
    }

    @Test fun `세션이 사라지면 해제 없이 소유만 뗀다`() {
        // 호가 이미 없으므로 floorRelease 를 보낼 곳이 없다 — 소유 목록만 정리한다.
        val t = TalkOwner()
        t.down(setOf(10, 11))
        t.prune(setOf(10))
        assertEquals(setOf(10), t.speaking)
        assertTrue("사라진 호에 해제를 보내지 않는다", t.released.isEmpty())
    }

    @Test fun `대상이 없으면 아무것도 요청하지 않는다`() {
        val t = TalkOwner()
        t.down(emptySet())
        assertTrue(t.speaking.isEmpty())
        assertTrue(t.released.isEmpty())
    }

    @Test fun `해제를 두 번 해도 한 번만 나간다`() {
        // 제스처 취소 + Composable 폐기가 겹칠 수 있다(F2) — 멱등이어야 한다.
        val t = TalkOwner()
        t.down(setOf(10))
        t.releaseAll()
        t.releaseAll()
        assertEquals(listOf(10), t.released)
    }
}
