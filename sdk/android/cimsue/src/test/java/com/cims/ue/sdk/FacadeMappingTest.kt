// 파사드 매핑 단위시험 — JVM, 기기·네이티브 불필요 (android_dispatch_tablet.md §9 S1-UE-TABLET-UNIT)
//
// 네이티브를 적재하지 않고 **순수 Kotlin 층**만 검사한다. 노리는 것은 두 가지다.
//   ① 코어 enum 의 서수와 Kotlin enum 의 서수가 어긋나면 상태가 조용히 뒤바뀐다 — 헤더 순서와 대조한다.
//   ② 파사드가 만든 파생 규칙(전화 회선 선택·관리 범위·잔여 일수·결과 래핑)이 계약대로인지.
package com.cims.ue.sdk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FacadeMappingTest {

    // ── ① enum 서수 — sdk/core/include/cimsue/types.h 의 선언 순서가 정본 ──
    @Test fun `enum 서수가 코어 헤더 순서와 같다`() {
        assertEquals(listOf("UDP", "TCP", "TLS"), Transport.entries.map { it.name })
        assertEquals(listOf("DIGEST", "AKA"), AuthScheme.entries.map { it.name })
        assertEquals(listOf("OFF", "OPTIONAL", "REQUIRED"), MediaSecurity.entries.map { it.name })
        assertEquals(listOf("UNREGISTERED", "REGISTERING", "REGISTERED", "FAILED"), RegState.entries.map { it.name })
        assertEquals(listOf("NULL", "OUTGOING", "INCOMING", "ACTIVE", "HELD", "DISCONNECTED"), CallState.entries.map { it.name })
        assertEquals(listOf("OUTGOING", "INCOMING"), CallDir.entries.map { it.name })
        // FloorState 는 TS 24.380 §6.2.4 상태머신 — Speaking/Listening 이 뒤바뀌면 발언 표시가 반대가 된다
        assertEquals(listOf("IDLE", "REQUESTING", "SPEAKING", "LISTENING", "QUEUED"), FloorState.entries.map { it.name })
    }

    // ── ①-b 코어 enum 과의 실제 대조 ──
    // 파사드는 `entries[swigValue()]` 로 옮긴다(Types.kt ordinalOf·CscClient.profileOf). SWIG typesafe-enum 은
    // 순수 Java 라 네이티브 없이 값을 읽을 수 있으므로, 서수가 어긋나면 여기서 잡힌다.
    // 이름 대조까지 하는 이유: 서수가 우연히 맞아도 의미가 뒤바뀌면 상태 표시가 반대가 된다.
    @Test fun `Kotlin enum 서수가 SWIG enum 값과 일치한다`() {
        fun check(kotlinNames: List<String>, swig: List<Pair<String, Int>>) {
            assertEquals(kotlinNames.size, swig.size)
            swig.forEachIndexed { i, (name, v) ->
                assertEquals("서수 불일치: $name", i, v)
                assertEquals("이름 불일치: $name", kotlinNames[i].lowercase(), name.lowercase())
            }
        }
        check(Transport.entries.map { it.name }, listOf(
            "UDP" to com.cims.ue.sdk.jni.Transport.UDP.swigValue(),
            "TCP" to com.cims.ue.sdk.jni.Transport.TCP.swigValue(),
            "TLS" to com.cims.ue.sdk.jni.Transport.TLS.swigValue()))
        check(AuthScheme.entries.map { it.name }, listOf(
            "Digest" to com.cims.ue.sdk.jni.AuthScheme.Digest.swigValue(),
            "Aka" to com.cims.ue.sdk.jni.AuthScheme.Aka.swigValue()))
        check(MediaSecurity.entries.map { it.name }, listOf(
            "Off" to com.cims.ue.sdk.jni.MediaSecurity.Off.swigValue(),
            "Optional" to com.cims.ue.sdk.jni.MediaSecurity.Optional.swigValue(),
            "Required" to com.cims.ue.sdk.jni.MediaSecurity.Required.swigValue()))
        check(RegState.entries.map { it.name }, listOf(
            "Unregistered" to com.cims.ue.sdk.jni.RegState.Unregistered.swigValue(),
            "Registering" to com.cims.ue.sdk.jni.RegState.Registering.swigValue(),
            "Registered" to com.cims.ue.sdk.jni.RegState.Registered.swigValue(),
            "Failed" to com.cims.ue.sdk.jni.RegState.Failed.swigValue()))
        check(CallState.entries.map { it.name }, listOf(
            "Null" to com.cims.ue.sdk.jni.CallState.Null.swigValue(),
            "Outgoing" to com.cims.ue.sdk.jni.CallState.Outgoing.swigValue(),
            "Incoming" to com.cims.ue.sdk.jni.CallState.Incoming.swigValue(),
            "Active" to com.cims.ue.sdk.jni.CallState.Active.swigValue(),
            "Held" to com.cims.ue.sdk.jni.CallState.Held.swigValue(),
            "Disconnected" to com.cims.ue.sdk.jni.CallState.Disconnected.swigValue()))
        check(CallDir.entries.map { it.name }, listOf(
            "Outgoing" to com.cims.ue.sdk.jni.CallDir.Outgoing.swigValue(),
            "Incoming" to com.cims.ue.sdk.jni.CallDir.Incoming.swigValue()))
        check(FloorState.entries.map { it.name }, listOf(
            "Idle" to com.cims.ue.sdk.jni.FloorState.Idle.swigValue(),
            "Requesting" to com.cims.ue.sdk.jni.FloorState.Requesting.swigValue(),
            "Speaking" to com.cims.ue.sdk.jni.FloorState.Speaking.swigValue(),
            "Listening" to com.cims.ue.sdk.jni.FloorState.Listening.swigValue(),
            "Queued" to com.cims.ue.sdk.jni.FloorState.Queued.swigValue()))
    }

    // ── ② 결과 래핑 ──
    @Test fun `CimsResult 는 실패에서 값을 내주지 않는다`() {
        val ok = CimsResult.ok(7)
        assertTrue(ok.ok); assertFalse(ok.failed); assertEquals(7, ok.getOrNull())
        val bad = CimsResult.fail<Int>(403, "forbidden")
        assertFalse(bad.ok); assertTrue(bad.failed); assertNull(bad.getOrNull())
        assertEquals(403, bad.code); assertEquals("forbidden", bad.reason)
        // map 은 성공에서만 돈다
        assertEquals(14, ok.map { it * 2 }.getOrNull())
        assertNull(bad.map { it * 2 }.getOrNull())
    }

    // ── ③ 전화 회선 선택 — 유선 voip 우선, 없으면 이동 volte (android_ue_provisioning.md §3) ──
    private fun svc(kind: String) = ServiceProfile(
        kind = kind, sipHost = "h", sipPort = 5060, transport = Transport.TLS, transports = emptyList(),
        enforced = false, mediaSecurity = MediaSecurity.OPTIONAL, domain = "d", msisdn = "+8210",
        imsi = "", authId = "", sipHa1 = "abc", mcpttId = "", authScheme = AuthScheme.DIGEST,
        akaK = "", akaOpc = "", akaAmf = "8000", secMechanisms = emptyList(), maxPayloadSdsCplaneBytes = 0)

    private fun profile(vararg kinds: String) = Profile(
        "n", "l", "KR", "csc", 4430, kinds.map { svc(it) }, DispatchProfile.NONE, false)

    @Test fun `전화 회선은 유선 voip 가 이동 volte 를 이긴다`() {
        assertEquals("voip", profile("volte", "voip").phoneService?.kind)
        assertEquals("voip", profile("voip", "volte").phoneService?.kind)   // 순서와 무관
        assertEquals("volte", profile("volte", "ptt").phoneService?.kind)   // voip 없으면 volte
        assertNull(profile("ptt").phoneService)                             // 전화 회선 없음
        assertEquals("ptt", profile("volte", "ptt").pttService?.kind)
    }

    // ── ④ 프로파일 → 계정: H(A1) 우선, 평문은 폴백 (sip_access_security.md P1) ──
    @Test fun `H(A1) 이 있으면 평문 비밀번호를 담지 않는다`() {
        val a = svc("volte").toAccountConfig(loginPw = "secret")
        assertEquals("abc", a.ha1)
        assertEquals("", a.password)
    }

    @Test fun `H(A1) 이 없을 때만 평문으로 폴백한다`() {
        val a = svc("volte").copy(sipHa1 = "").toAccountConfig(loginPw = "secret")
        assertEquals("", a.ha1)
        assertEquals("secret", a.password)
    }

    @Test fun `계정 설정이 접속 프로파일 값을 그대로 옮긴다`() {
        val a = svc("ptt").copy(transport = Transport.TLS, mediaSecurity = MediaSecurity.REQUIRED,
            mcpttId = "tel:+8250", domain = "ptt.example").toAccountConfig()
        assertEquals(Transport.TLS, a.transport)
        assertEquals(MediaSecurity.REQUIRED, a.mediaSecurity)
        assertEquals("tel:+8250", a.mcpttId)
        assertEquals("ptt.example", a.domain)
    }

    // ── ⑤ 관제 데스크 범위 — 없으면 관리 화면이 비활성 (dispatch_center.md §3.4) ──
    @Test fun `directoryAdmin 범위가 관리 권한을 정한다`() {
        val base = DispatchProfile.NONE
        assertFalse(base.canAdminDirectory)
        assertFalse(base.copy(directoryAdmin = "none").canAdminDirectory)
        assertTrue(base.copy(directoryAdmin = "own").canAdminDirectory)
        assertTrue(base.copy(directoryAdmin = "all").canAdminDirectory)
    }

    @Test fun `데스크가 없으면 소프트폰 모드다`() {
        assertFalse(DispatchProfile.NONE.present)
        assertTrue(DispatchProfile.NONE.members.isEmpty())
        assertTrue(DispatchProfile.NONE.pttTargets.isEmpty())
    }

    // ── ⑥ 인증서 만료 잔여 — 요약 띠 경고의 입력 (sip_tls_signaling.md §8.6) ──
    @Test fun `잔여 일수는 관측이 없으면 null 이다`() {
        assertNull(TlsPeerExpiry(false, 0, 0, "", "").daysLeft)
    }

    @Test fun `잔여 일수는 만료 전후로 부호가 갈린다`() {
        val now = System.currentTimeMillis() / 1000L
        assertEquals(10, TlsPeerExpiry(true, now + 10 * 86400 + 60, now, "s", "r").daysLeft)
        assertTrue(TlsPeerExpiry(true, now - 86400, now, "s", "r").daysLeft!! < 0)
    }

    // ── ⑦ 호 스냅샷의 파생 상태 ──
    private fun call(state: CallState) = CallInfo(
        1, 0, CallDir.OUTGOING, state, "sip:a@b", "", false, false, false, true, 0, 0, "",
        emptyList(), false, "", McpttInfo(false, "", "", "", "", false, false, false, false),
        false, false, "")

    @Test fun `호 스냅샷이 활성과 종료를 구분한다`() {
        assertTrue(call(CallState.ACTIVE).active)
        assertFalse(call(CallState.ACTIVE).ended)
        assertTrue(call(CallState.DISCONNECTED).ended)
        assertFalse(call(CallState.DISCONNECTED).active)
        assertFalse(call(CallState.INCOMING).active)
    }

    @Test fun `등록 스냅샷이 등록 여부를 구분한다`() {
        assertTrue(RegInfo(0, RegState.REGISTERED, 200, "", 3600).registered)
        assertFalse(RegInfo(0, RegState.REGISTERING, 0, "", 0).registered)
        assertFalse(RegInfo(0, RegState.FAILED, 403, "Forbidden", 0).registered)
    }

    // ── ⑦-b Codex 리뷰 반영: 실패 결과의 약속 ──
    @Test fun `닫힘·낡은 핸들은 크래시가 아니라 결과로 떨어진다`() {
        // 파사드는 네이티브 0 포인터 역참조(프로세스 크래시) 대신 이 코드를 돌려준다.
        // 값이 바뀌면 앱의 분기가 조용히 어긋나므로 고정한다.
        val closed = CimsResult.fail<Unit>(-99, "engine closed")
        val stale = CimsResult.fail<Unit>(-98, "stale call handle")
        assertFalse(closed.ok); assertEquals(-99, closed.code)
        assertFalse(stale.ok); assertEquals(-98, stale.code)
        assertNull(closed.getOrNull()); assertNull(stale.getOrNull())
    }

    // ── ⑧ HTTP 응답은 이진을 보존한다 (Codex 리뷰 F1 — 녹취 MP4/AAC 경로) ──
    @Test fun `HttpResponse 는 NUL 과 비 UTF-8 바이트를 보존한다`() {
        val raw = byteArrayOf(0x00, 0x01, 0xFF.toByte(), 0x00, 0x7F, 0xC3.toByte(), 0x28)
        val r = HttpResponse(200, "audio/mp4", "\"e1\"", raw)
        assertEquals(7, r.body.size)
        assertTrue(raw.contentEquals(r.body))
        assertEquals(0xFF.toByte(), r.body[2])   // String 을 거쳤다면 여기서 망가진다
        assertFalse(r.notModified)
        assertTrue(HttpResponse(304, "", "\"e1\"", ByteArray(0)).notModified)
    }

    @Test fun `HttpResponse 동등성은 본문 내용으로 정한다`() {
        val a = HttpResponse(200, "t", "e", byteArrayOf(1, 0, 2))
        val b = HttpResponse(200, "t", "e", byteArrayOf(1, 0, 2))
        val c = HttpResponse(200, "t", "e", byteArrayOf(1, 0, 3))
        assertEquals(a, b)
        assertEquals(a.hashCode(), b.hashCode())
        assertFalse(a == c)
    }
}
