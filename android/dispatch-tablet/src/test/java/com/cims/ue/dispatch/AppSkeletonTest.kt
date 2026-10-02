// 앱 골격 단위시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §9 S1-UE-TABLET-UNIT)
//
// 네이티브·Android 프레임워크를 타지 않는 **판정 로직**만 검사한다. 노리는 것은 조용히 어긋나면
// 화면이 통째로 틀어지는 계약들이다 — 화면 배열, 계정 선택 규칙, 자격 폴백.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.DispatchMode
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.sdk.AuthScheme
import com.cims.ue.sdk.MediaSecurity
import com.cims.ue.sdk.Profile
import com.cims.ue.sdk.DispatchProfile
import com.cims.ue.sdk.ServiceProfile
import com.cims.ue.sdk.Transport
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AppSkeletonTest {

    // ── 화면 배열 — 레일은 데스크톱과 같은 넷이다(§6.3) ──
    @Test fun `레일은 관제·이력·PTT 그룹·관리 순이다`() {
        assertEquals(listOf("관제", "이력", "PTT 그룹", "관리"), AppScreen.entries.map { it.label })
    }

    @Test fun `첫 화면은 관제다`() {
        // 관제사가 가장 오래 머무는 곳이다 — 무전·통화가 [관제] 한 곳에 모였다(§6.3).
        assertEquals(AppScreen.DISPATCH, AppScreen.entries.first())
    }

    @Test fun `관제는 무전·통화 두 모드다`() {
        assertEquals(listOf("무전", "통화"), DispatchMode.entries.map { it.label })
    }

    @Test fun `감청·청취·메시지는 최상위 축이 아니다`() {
        // 감청은 통화 leg, 청취는 무전 leg, SDS 는 무전 채널의 대화 — 축을 따로 세우면 같은 것을 두 군데서 찾는다.
        assertTrue(AppScreen.entries.none { it.label in setOf("감청", "청취", "메시지", "무전", "통화") })
    }

    @Test fun `그룹원·주소록은 면이 아니다 — 고정 칸과 패널이다`() {
        // 그룹원은 «상태를 곁눈질하는 대상» 이라 [통화] 의 왼쪽 고정 칸 띠로 족하다. 주소록은 어느 통화 면에서든 여는
        //   오른쪽 패널이다 — 면이면 다이얼패드·문자를 보면서 옆에 펴 둘 수 없다(§6.3).
        assertTrue(CallPane.entries.none { it.label == "그룹원" })
        assertTrue(CallPane.entries.none { it.label == "주소록" })
        assertEquals(null, com.cims.ue.dispatch.ui.SidePanel.Book.parent)
    }

    @Test fun `청취·감청은 면으로도 두지 않는다 — 목록에 드러낸다`() {
        // 청취 중인 채널은 «채널» 면의 범위 채널 목록에 «청취 중» 으로 나오고 거기서 끈다.
        // 감청은 고정 칸 «진행 중» 행에서 켜고 끈다(§6.3).
        assertTrue(PttPane.entries.none { it.label == "청취" })
        assertTrue(CallPane.entries.none { it.label == "감청" })
    }

    @Test fun `무전 면은 채널이 먼저다`() {
        assertEquals(listOf("채널", "메시지", "이벤트"), PttPane.entries.map { it.label })
    }

    @Test fun `통화 면은 통화가 먼저다`() {
        assertEquals(listOf("통화", "메시지", "통화내역"), CallPane.entries.map { it.label })
    }

    @Test fun `PTT 그룹·관리는 레일에 바로 선다`() {
        // 데스크톱과 같다 — 메뉴를 거치지 않고 한 번에 누른다(§6.3).
        assertTrue(AppScreen.entries.map { it.label }.containsAll(listOf("PTT 그룹", "관리")))
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
