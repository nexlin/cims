// Android 접점층 단위시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9 S1-UE-TABLET-UNIT)
//
// 접점층 대부분은 Android 프레임워크(Context·AudioManager·Keystore)를 타서 기기 없이는 못 돈다.
// 그래서 **판정 로직만 순수 타입으로 떼어** 여기서 검사한다 — 나머지는 얇은 배선이고 실기기 항목이다.
package com.cims.ue.sdk

import android.view.KeyEvent
import com.cims.ue.sdk.platform.HwPtt
import com.cims.ue.sdk.platform.PttKey
import com.cims.ue.sdk.platform.Route
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class PlatformTest {

    private val unset = HwPtt.KeyMapping.UNSET

    // ── 하드 키 분류 — 어긋나면 측면 버튼이 조용히 안 먹는다 ──
    @Test fun `미학습이면 내장 기본 keycode 를 쓴다`() {
        // 러기드 단말 실측값(309/310) + 일반 키보드 폴백(F11/F10)
        assertEquals(PttKey.TALK, unset.classify(309))
        assertEquals(PttKey.TALK, unset.classify(KeyEvent.KEYCODE_F11))
        assertEquals(PttKey.ALERT, unset.classify(310))
        assertEquals(PttKey.ALERT, unset.classify(KeyEvent.KEYCODE_F10))
    }

    @Test fun `관계없는 키는 소비하지 않는다`() {
        assertEquals(PttKey.NONE, unset.classify(KeyEvent.KEYCODE_VOLUME_UP))
        assertEquals(PttKey.NONE, unset.classify(KeyEvent.KEYCODE_BACK))
        assertEquals(PttKey.NONE, unset.classify(KeyEvent.KEYCODE_A))
    }

    @Test fun `학습값이 내장 기본을 이긴다`() {
        val m = unset.learn(PttKey.TALK, 400)
        assertEquals(PttKey.TALK, m.classify(400))
        // 그 종류의 기본값은 더 쓰지 않는다 — 학습한 기기에서 엉뚱한 키가 발언을 걸면 안 된다
        assertEquals(PttKey.NONE, m.classify(309))
        assertEquals(PttKey.NONE, m.classify(KeyEvent.KEYCODE_F11))
        // 학습하지 않은 종류는 기본값이 그대로 산다
        assertEquals(PttKey.ALERT, m.classify(310))
    }

    @Test fun `두 종류를 따로 학습할 수 있다`() {
        val m = unset.learn(PttKey.TALK, 400).learn(PttKey.ALERT, 401)
        assertEquals(PttKey.TALK, m.classify(400))
        assertEquals(PttKey.ALERT, m.classify(401))
        assertEquals(PttKey.NONE, m.classify(309))
        assertEquals(PttKey.NONE, m.classify(310))
    }

    @Test fun `같은 키를 두 종류에 학습하면 먼저 선언된 TALK 가 이긴다`() {
        // 사용자가 실수로 같은 버튼을 둘 다 학습해도 분류가 애매해지지 않는다(결정적).
        val m = unset.learn(PttKey.TALK, 400).learn(PttKey.ALERT, 400)
        assertEquals(PttKey.TALK, m.classify(400))
    }

    @Test fun `NONE 학습은 매핑을 바꾸지 않는다`() {
        assertEquals(unset, unset.learn(PttKey.NONE, 400))
    }

    @Test fun `학습값 0 이하는 미학습으로 본다`() {
        // keycode 0(KEYCODE_UNKNOWN)이 저장돼도 모든 키를 삼키면 안 된다.
        val m = HwPtt.KeyMapping(talk = 0, alert = -1)
        assertEquals(PttKey.NONE, m.classify(0))
        assertEquals(PttKey.TALK, m.classify(309))   // 기본값으로 폴백
    }

    // ── 라우트 어휘 — 코어(ue_sdk.md §4.5)와 같아야 한다 ──
    @Test fun `라우트 어휘가 코어와 같다`() {
        assertEquals(listOf("EARPIECE", "SPEAKER", "HEADSET", "BLUETOOTH"), Route.entries.map { it.name })
    }

    @Test fun `PttKey 는 NONE 을 첫 값으로 둔다`() {
        // 기본값이 "아무것도 아님" 이어야 미분류 키가 발언을 걸지 않는다.
        assertEquals(PttKey.NONE, PttKey.entries.first())
        assertNotEquals(PttKey.NONE, PttKey.TALK)
    }
}
