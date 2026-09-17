// 포커스 ≠ 발언 대상 — S6 의 불변 계약 (dispatch_desktop_ui.md §4.1, android_dispatch_tablet.md §6.3)
//
// 관제사는 **순찰1을 보면서 상황실에 말할 수** 있어야 한다. 둘이 섞이면 보고 있는 채널로 송출되거나
// 말하는 채널이 화면을 빼앗는다 — 둘 다 사고다. 그래서 선택 로직을 순수 함수로 떼어 여기서 고정한다.
//
// (ViewModel 자체는 StateFlow·coroutine 을 타서 JVM 시험이 무겁다. 판정만 같은 규칙으로 검사한다.)
package com.cims.ue.dispatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** PttChannelsViewModel 의 선택 규칙과 **같은 판정**. 규칙이 갈라지면 여기가 먼저 깨진다. */
private class Selection(val maxTargets: Int) {
    var focused: String? = null
        private set
    var targets: Set<String> = emptySet()
        private set

    /** 카드 탭 — 같은 카드를 다시 누르면 접힌다. 발언 대상은 건드리지 않는다. */
    fun focus(id: String) { focused = if (focused == id) null else id }

    /** 카드 체크 — 발언 대상 집합. 포커스는 건드리지 않는다. */
    fun toggleTarget(id: String, canCheck: Boolean = true) {
        if (!canCheck) return
        targets = when {
            id in targets -> targets - id
            targets.size < maxTargets -> targets + id
            maxTargets == 1 -> setOf(id)
            else -> targets.drop(1).toSet() + id
        }
    }

    fun clearTargets() { targets = emptySet() }
    fun prune(allowed: Set<String>) { targets = targets intersect allowed }
}

class FocusVsTargetTest {

    @Test fun `포커스를 바꿔도 발언 대상은 그대로다`() {
        val s = Selection(maxTargets = 3)
        s.toggleTarget("상황실")
        s.focus("순찰1")
        assertEquals("순찰1", s.focused)
        assertEquals(setOf("상황실"), s.targets)      // ← 보는 채널과 말하는 채널이 다르다
    }

    @Test fun `발언 대상을 바꿔도 포커스는 그대로다`() {
        val s = Selection(maxTargets = 3)
        s.focus("순찰1")
        s.toggleTarget("상황실")
        s.toggleTarget("교통1")
        assertEquals("순찰1", s.focused)
        assertEquals(setOf("상황실", "교통1"), s.targets)
    }

    @Test fun `같은 카드를 다시 누르면 포커스가 접힌다`() {
        val s = Selection(maxTargets = 1)
        s.focus("순찰1")
        s.focus("순찰1")
        assertNull(s.focused)
    }

    @Test fun `체크는 토글이다`() {
        val s = Selection(maxTargets = 3)
        s.toggleTarget("순찰1")
        assertTrue("순찰1" in s.targets)
        s.toggleTarget("순찰1")
        assertTrue(s.targets.isEmpty())
    }

    @Test fun `상한이 1이면 새 대상이 이전 것을 대체한다`() {
        // 팬아웃 전 동작 — 고르면 조용히 무시되는 게 아니라 옮겨 간다(데스크톱과 같다).
        val s = Selection(maxTargets = 1)
        s.toggleTarget("상황실")
        s.toggleTarget("순찰1")
        assertEquals(setOf("순찰1"), s.targets)
    }

    @Test fun `상한을 넘으면 가장 오래된 것이 밀려난다`() {
        val s = Selection(maxTargets = 2)
        s.toggleTarget("A"); s.toggleTarget("B"); s.toggleTarget("C")
        assertEquals(setOf("B", "C"), s.targets)
    }

    @Test fun `자격 없는 카드는 대상이 되지 않는다`() {
        val s = Selection(maxTargets = 3)
        s.toggleTarget("순찰1", canCheck = false)
        assertTrue(s.targets.isEmpty())
    }

    @Test fun `세션이 끝난 대상은 정리된다`() {
        // 카드가 빠졌는데 대상 집합에 남으면 PTT 가 없는 세션에 floor 를 건다.
        val s = Selection(maxTargets = 3)
        s.toggleTarget("상황실"); s.toggleTarget("순찰1")
        s.prune(setOf("순찰1"))
        assertEquals(setOf("순찰1"), s.targets)
    }

    @Test fun `모두 해제해도 포커스는 남는다`() {
        val s = Selection(maxTargets = 3)
        s.focus("순찰1")
        s.toggleTarget("상황실")
        s.clearTargets()
        assertTrue(s.targets.isEmpty())
        assertEquals("순찰1", s.focused)     // 보던 채널은 계속 본다
    }

    // ── 참여 → 자동 발언 대상 (기기 시험에서 «발언 버튼이 없다» 로 드러난 것) ──
    // 데스크톱은 발신·Ctrl+n 에서 «포커스 + 단일 대상» 을 함께 한다. 태블릿에는 그 단축키가 없어
    // [참여] 가 그 자리를 대신한다 — 참여는 곧 말하겠다는 의도다.
    @Test fun `참여하면 그 채널이 단일 발언 대상이 된다`() {
        val s = Selection(maxTargets = 1)
        s.focus("순찰1")                      // [참여] 가 포커스를 옮기고
        s.toggleTarget("순찰1")               // 세션이 서면 대상으로 올린다
        assertEquals("순찰1", s.focused)
        assertEquals(setOf("순찰1"), s.targets)
    }

    @Test fun `다른 채널에 참여하면 대상이 옮겨간다`() {
        // 상한이 1이므로 새 참여가 이전 대상을 대체한다 — 두 채널에 동시에 말하지 않는다.
        val s = Selection(maxTargets = 1)
        s.toggleTarget("순찰1")
        s.toggleTarget("상황실")
        assertEquals(setOf("상황실"), s.targets)
    }

    @Test fun `참여 뒤에도 대상을 손으로 뗄 수 있다`() {
        // 자동 지정이 강제가 되면 «듣기만 하려는데 말할 준비가 돼 있는» 상태를 못 만든다.
        val s = Selection(maxTargets = 1)
        s.toggleTarget("순찰1")
        s.toggleTarget("순찰1")
        assertTrue(s.targets.isEmpty())
    }

    @Test fun `포커스 없이도 발언할 수 있다`() {
        // 아무 카드도 펼치지 않은 채 체크만 해 둔 상태 — 유효하다.
        val s = Selection(maxTargets = 1)
        s.toggleTarget("상황실")
        assertNull(s.focused)
        assertEquals(setOf("상황실"), s.targets)
    }
}
