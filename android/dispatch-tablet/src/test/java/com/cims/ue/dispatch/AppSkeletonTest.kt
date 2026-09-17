// 앱 골격 단위시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9 S1-UE-TABLET-UNIT)
//
// 네이티브·Android 프레임워크를 타지 않는 **판정 로직**만 검사한다. 노리는 것은 조용히 어긋나면
// 화면이 통째로 틀어지는 계약들이다 — 화면 배열, 계정 선택 규칙, 자격 폴백.
package com.cims.ue.dispatch

import android.view.KeyEvent
import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.DispatchTab
import com.cims.ue.sdk.AuthScheme
import com.cims.ue.sdk.MediaSecurity
import com.cims.ue.sdk.Profile
import com.cims.ue.sdk.DispatchProfile
import com.cims.ue.sdk.ServiceProfile
import com.cims.ue.sdk.Transport
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AppSkeletonTest {

    // ── 화면 배열 — 데스크톱 F1~F4 와 같은 순서여야 한다(dispatch_desktop_ui.md §3.4) ──
    @Test fun `하단 내비는 관제·이력·PTT그룹·관리 순이다`() {
        assertEquals(listOf("관제", "이력", "PTT 그룹", "관리"), AppScreen.entries.map { it.label })
        assertEquals(listOf("F1", "F2", "F3", "F4"), AppScreen.entries.map { it.hotkey })
    }

    @Test fun `F1~F4 가 같은 화면에 대응한다`() {
        assertEquals(AppScreen.DISPATCH, AppScreen.ofFunctionKey(KeyEvent.KEYCODE_F1))
        assertEquals(AppScreen.HISTORY, AppScreen.ofFunctionKey(KeyEvent.KEYCODE_F2))
        assertEquals(AppScreen.PTT_GROUPS, AppScreen.ofFunctionKey(KeyEvent.KEYCODE_F3))
        assertEquals(AppScreen.ADMIN, AppScreen.ofFunctionKey(KeyEvent.KEYCODE_F4))
        assertNull(AppScreen.ofFunctionKey(KeyEvent.KEYCODE_F5))
        assertNull(AppScreen.ofFunctionKey(KeyEvent.KEYCODE_A))
    }

    @Test fun `요약 띠는 관제 밖에서만 붙는다`() {
        // 관제 캔버스에는 발언 바가 이미 있어 띠가 중복된다(§6.2).
        assertFalse(AppScreen.DISPATCH.showsSummaryStrip)
        assertTrue(AppScreen.HISTORY.showsSummaryStrip)
        assertTrue(AppScreen.PTT_GROUPS.showsSummaryStrip)
        assertTrue(AppScreen.ADMIN.showsSummaryStrip)
    }

    @Test fun `관제 탭은 PTT 가 먼저다`() {
        // 첫 화면이 PTT 인 것은 «PTT 채널 중심» 이라는 UI 정본의 전제다(dispatch_desktop_ui.md §1).
        assertEquals(listOf("PTT", "일반통화"), DispatchTab.entries.map { it.label })
        assertEquals(DispatchTab.PTT, DispatchTab.entries.first())
    }

    // ── 계정 선택 규칙 — 전화 계열은 하나만 올린다 ──
    private fun svc(kind: String, ha1: String = "abc") = ServiceProfile(
        kind = kind, sipHost = "h", sipPort = 5060, transport = Transport.TLS, transports = emptyList(),
        enforced = false, mediaSecurity = MediaSecurity.OPTIONAL, domain = "d", msisdn = "+8210",
        imsi = "", authId = "", sipHa1 = ha1, mcpttId = "", authScheme = AuthScheme.DIGEST,
        akaK = "", akaOpc = "", akaAmf = "8000", secMechanisms = emptyList(), maxPayloadSdsCplaneBytes = 0)

    private fun profile(vararg kinds: String) = Profile(
        "관제1석", "disp01", "KR", "csc", 4430, kinds.map { svc(it) }, DispatchProfile.NONE, false)

    /** 세션이 등록 대상을 고르는 규칙과 같은 판정 — 전화 1 + PTT 전부. */
    private fun toRegister(p: Profile): List<String> = buildList {
        p.phoneService?.let { add(it.kind) }
        p.services.filter { it.kind == "ptt" }.forEach { add(it.kind) }
    }

    @Test fun `전화 계열은 하나만 등록한다`() {
        // volte·voip 를 둘 다 올리면 이동 번호까지 관제석에 포크되고 전화 계정 참조가 덮어써진다.
        assertEquals(listOf("voip"), toRegister(profile("volte", "voip")))
        assertEquals(listOf("volte"), toRegister(profile("volte")))
        assertEquals(listOf("voip", "ptt"), toRegister(profile("voip", "ptt")))
    }

    @Test fun `PTT 는 여러 개여도 전부 등록한다`() {
        val p = Profile("n", "l", "KR", "c", 4430,
            listOf(svc("volte"), svc("ptt"), svc("ptt")), DispatchProfile.NONE, false)
        assertEquals(listOf("volte", "ptt", "ptt"), toRegister(p))
    }

    @Test fun `전화 회선이 없으면 PTT 만 등록한다`() {
        assertEquals(listOf("ptt"), toRegister(profile("ptt")))
    }

    @Test fun `등록할 것이 없으면 빈 목록이다`() {
        assertTrue(toRegister(Profile("n", "l", "KR", "c", 4430, emptyList(), DispatchProfile.NONE, false)).isEmpty())
    }

    // ── 자격 — H(A1) 우선, 평문은 폴백(sip_access_security.md P1) ──
    @Test fun `H(A1) 이 있으면 평문을 담지 않는다`() {
        val a = svc("volte").toAccountConfig(loginPw = "secret")
        assertEquals("abc", a.ha1)
        assertEquals("", a.password)
    }

    @Test fun `H(A1) 이 없으면 평문으로 폴백한다`() {
        val a = svc("volte", ha1 = "").toAccountConfig(loginPw = "secret")
        assertEquals("secret", a.password)
    }

    @Test fun `저장 자격만으로 복귀하면 평문이 없다`() {
        // 프로세스 회수 뒤 resume 경로는 비밀번호를 갖고 있지 않다 — H(A1) 이 없는 프로파일은 등록하지 못한다.
        val a = svc("volte", ha1 = "").toAccountConfig(loginPw = "")
        assertEquals("", a.ha1)
        assertEquals("", a.password)
    }

    // ── 데스크 유무 ──
    @Test fun `데스크가 없으면 소프트폰 모드다`() {
        assertFalse(profile("volte").dispatch.present)
        assertFalse(profile("volte").dispatch.canAdminDirectory)
    }
}
