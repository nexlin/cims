// 세션 평면의 순수 규칙 — 긴급 경보·조건 해제 자격·편성 재조회·서버 이력 폴링 (android_dispatch_tablet.md §6.2a-1·§6.7)
//
// 여기 있는 것은 전부 «조용히 어긋나면 관제사가 모르는» 규칙이다: 내려가지 않는 경보 배너, 눌러도 거절될 [해제], 편성이 바뀌었는데
// 그대로인 감시 대상, 두 번 남는 이력. 세션(엔진·CSC) 없이 판정만 고정한다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.AlertEffect
import com.cims.ue.dispatch.session.EmergencyAlertBanner
import com.cims.ue.dispatch.session.HistoryFeed
import com.cims.ue.dispatch.session.alertEffectOf
import com.cims.ue.dispatch.session.applying
import com.cims.ue.dispatch.session.canCancelCondition
import com.cims.ue.dispatch.session.dispatchChanged
import com.cims.ue.dispatch.session.isGroupDocChange
import com.cims.ue.dispatch.session.unconfirmedAnswer
import com.cims.ue.dispatch.session.str
import com.cims.ue.dispatch.session.watchTargets
import com.cims.ue.dispatch.ui.AlertBannerUi
import com.cims.ue.dispatch.ui.alertBannerStack
import com.cims.ue.dispatch.ui.previewCallInfo
import com.cims.ue.dispatch.ui.previewSession
import com.cims.ue.dispatch.ui.toAlertBannerUi
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.Capabilities
import com.cims.ue.sdk.DispatchMember
import com.cims.ue.sdk.DispatchProfile
import com.cims.ue.sdk.DispatchTarget
import com.cims.ue.sdk.EmergencyAlert
import com.cims.ue.sdk.McpttCondition
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionPlanesTest {

    // ── 긴급 경보(TS 24.379 §12.1.1.3) ──
    private fun alert(user: String = "tel:+821000001006", alertInd: Int = 1, by: String = "", self: Boolean = false) =
        EmergencyAlert(accountId = 1, groupId = "g002", userId = user, originatedBy = by, mcOrg = "",
            alertInd = alertInd, emergencyInd = 0, imminentPerilInd = 0, self = self)

    private fun banner(user: String = "+821000001006", at: Long = 1_000) =
        EmergencyAlertBanner("g002", user, "상황실", "1006 박경장", at)

    @Test fun `경보가 오면 그룹·발신자마다 배너 하나가 맨 위에 선다`() {
        val e = alertEffectOf(alert(), "상황실", "1006 박경장", nowMs = 5_000)
        assertTrue(e is AlertEffect.Raised)
        val older = EmergencyAlertBanner("g001", "+821000001003", "순찰1", "1003 이순경", 1_000)
        val next = listOf(older, banner(at = 2_000)).applying("g002", e)
        assertEquals(listOf("g002|+821000001006", "g001|+821000001003"), next.map { it.key })   // 갈아 끼우고 위로
        assertEquals(5_000, next.first().sinceMs)                                               // 경과는 다시 센다
    }

    @Test fun `발신자의 취소가 그 배너를 내린다`() {
        val e = alertEffectOf(alert(alertInd = -1), "상황실", "", nowMs = 0)
        assertEquals(AlertEffect.Cleared(owner = "+821000001006", by = "+821000001006"), e)
        assertTrue(listOf(banner()).applying("g002", e).isEmpty())
    }

    @Test fun `제3자 취소는 원 경보 발신자의 배너를 내린다`() {
        // 취소한 사람(userId)이 아니라 originated-by 가 가리키는 사람의 배너다 — 취소한 사람으로 찾으면 안 내려간다.
        val e = alertEffectOf(alert(user = "tel:+821000009999", alertInd = -1, by = "tel:+821000001006"), "상황실", "", 0)
        assertEquals(AlertEffect.Cleared(owner = "+821000001006", by = "+821000009999"), e)
        assertTrue(listOf(banner()).applying("g002", e).isEmpty())
    }

    @Test fun `다른 그룹의 취소는 같은 번호의 배너를 건드리지 않는다`() {
        val e = AlertEffect.Cleared(owner = "+821000001006", by = "+821000001006")
        assertEquals(1, listOf(banner()).applying("g009", e).size)
    }

    @Test fun `내 경보의 에코와 경보 없는 그룹 상태 통지는 배너가 아니다`() {
        assertEquals(AlertEffect.None, alertEffectOf(alert(self = true), "상황실", "", 0))
        assertEquals(AlertEffect.None, alertEffectOf(alert(alertInd = 0), "상황실", "", 0))
    }

    @Test fun `조건 배너와 경보 배너는 한 스택에 최신이 위로 선다`() {
        val cond = AlertBannerUi("g1", com.cims.ue.dispatch.session.AlertKind.EMERGENCY, "순찰1", sinceMs = 2_000, callId = 7)
        val al = banner(at = 3_000).toAlertBannerUi(canCancel = true)
        val stack = alertBannerStack(listOf(cond), listOf(al))
        assertEquals(listOf("alert|g002|+821000001006", "cond|g1"), stack.map { it.key })
        assertTrue(stack.first().isAlert)
        assertNull(stack.first().callId)
    }

    // ── 조건 해제 자격(TS 24.379 §6.3.3.1.13.4·§6.3.3.1.13.6) ──
    private fun emg(mine: Boolean = false, peril: Boolean = false, pending: Boolean = false,
                    listenOnly: Boolean = false, state: CallState = CallState.ACTIVE) =
        previewSession(info = previewCallInfo(isMcptt = true, groupId = "g001", listenOnly = listenOnly, state = state)
            .copy(condition = McpttCondition(emergency = !peril, imminentPeril = peril, mine = mine, pending = pending)))

    @Test fun `긴급은 내가 올렸거나 해제 권한이 있을 때만 내릴 수 있다`() {
        val no = Capabilities(cancelGroupEmergency = false, cancelImminentPeril = false)
        assertTrue(canCancelCondition(emg(mine = true), no))
        assertFalse(canCancelCondition(emg(mine = false), no))
        assertTrue(canCancelCondition(emg(mine = false), Capabilities(cancelGroupEmergency = true)))
    }

    @Test fun `임박 위험은 개시자라도 해제 권한이 있어야 내린다`() {
        assertFalse(canCancelCondition(emg(mine = true, peril = true), Capabilities(cancelImminentPeril = false)))
        assertTrue(canCancelCondition(emg(peril = true), Capabilities(cancelImminentPeril = true)))
    }

    @Test fun `청취 leg·성립 전·변경 진행 중에는 조건을 내리지 않는다`() {
        val all = Capabilities()
        assertFalse(canCancelCondition(emg(mine = true, listenOnly = true), all))
        assertFalse(canCancelCondition(emg(mine = true, state = CallState.OUTGOING), all))
        assertFalse(canCancelCondition(emg(mine = true, pending = true), all))
        // 조건이 없는 세션은 내릴 것이 없다
        assertFalse(canCancelCondition(previewSession(info = previewCallInfo(isMcptt = true, groupId = "g001")), all))
    }

    // ── 멤버 확인 전 연결(RFC 4964 P-Answer-State) ──
    @Test fun `내가 건 호가 Unconfirmed 로 성립하면 한 번 적는다`() {
        val s = previewSession(info = previewCallInfo(isMcptt = true, groupId = "g001").copy(answerState = "unconfirmed"))
        assertTrue(unconfirmedAnswer(s))
        assertFalse(unconfirmedAnswer(s.copy(answerStateNoted = true)))
        assertFalse(unconfirmedAnswer(s.copy(info = s.info.copy(dir = CallDir.INCOMING))))
        assertFalse(unconfirmedAnswer(s.copy(info = s.info.copy(state = CallState.OUTGOING))))
        assertFalse(unconfirmedAnswer(s.copy(info = s.info.copy(answerState = "Confirmed"))))
    }

    // ── 편성 재조회 ──
    private fun desk(members: List<DispatchMember> = listOf(member("1002")), targets: List<String> = listOf("g010"),
                     pilot: String = "7000", scope: String = "own", present: Boolean = true) =
        DispatchProfile(present, "dg1", "관제1과", pilot, scope, "listed", "hidden", "none", "",
            members, targets.map { DispatchTarget(it, "tel:$it", it) })

    private fun member(ext: String, name: String = "이당직", aor: String = "tel:+8210000$ext") =
        DispatchMember("u$ext", name, aor, "tel:+8220000$ext", ext, "dg1")

    @Test fun `그룹원·청취 대상·범위가 같으면 편성은 바뀌지 않은 것이다`() {
        assertFalse(dispatchChanged(desk(), desk()))
        // 순서만 다른 것은 변화가 아니다
        val a = desk(members = listOf(member("1002"), member("1003")))
        val b = desk(members = listOf(member("1003"), member("1002")))
        assertFalse(dispatchChanged(a, b))
    }

    @Test fun `그룹원 이름·회선, 청취 그룹, 대표번호, 범위가 바뀌면 편성이 바뀐 것이다`() {
        assertTrue(dispatchChanged(desk(), desk(members = listOf(member("1002", name = "이순경")))))
        assertTrue(dispatchChanged(desk(), desk(members = listOf(member("1002"), member("1003")))))
        assertTrue(dispatchChanged(desk(), desk(targets = listOf("g010", "g011"))))
        assertTrue(dispatchChanged(desk(), desk(pilot = "7001")))
        assertTrue(dispatchChanged(desk(), desk(scope = "all")))
    }

    @Test fun `감시 대상은 대표번호가 먼저고 PTT 전용 가입자는 뺀다`() {
        val d = desk(members = listOf(member("1002"), member("1003", aor = ""), member("1004", aor = "sip:1004@cims")))
        assertEquals(listOf("7000", "+82100001002", "sip:1004@cims"), watchTargets(d).toList())
        assertTrue(watchTargets(desk(present = false)).isEmpty())
    }

    @Test fun `그룹 문서의 xcap-diff 만 그룹 재조회를 건다`() {
        val body = "<xcap-diff><document sel=\"org.openmobilealliance.groups/users/x/g1\"/></xcap-diff>"
        assertTrue(isGroupDocChange("application/xcap-diff+xml", body))
        assertFalse(isGroupDocChange("application/xcap-diff+xml", "<xcap-diff><document sel=\"org.3gpp.mcptt.user-profile/x\"/></xcap-diff>"))
        assertFalse(isGroupDocChange("text/plain", body))
    }

    // ── floor 이벤트 문구 ──
    private fun floor(kind: com.cims.ue.sdk.FloorEventKind, state: com.cims.ue.sdk.FloorState = com.cims.ue.sdk.FloorState.IDLE,
                      cause: String = "", pos: Int = -1) =
        com.cims.ue.sdk.FloorEvent(kind, 1, state, 0, 0, cause, 0, 1, pos, false, emptyList(), 0)

    @Test fun `대기열 위치는 1 부터 적고 모르면 낱말만 적는다`() {
        assertEquals("대기 1번째", com.cims.ue.dispatch.session.queueText(1))
        assertEquals("대기 3번째", com.cims.ue.dispatch.session.queueText(3))
        listOf(-1, 0, 254, 255).forEach { assertEquals("대기열", com.cims.ue.dispatch.session.queueText(it)) }
    }

    @Test fun `거부·회수·시간 초과는 사유와 함께 이벤트로 남는다`() {
        assertEquals("발언 요청 거부 · 대기열 가득", com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.DENIED, cause = "대기열 가득")))
        assertEquals("발언 요청 거부", com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.DENIED)))
        assertEquals("발언권 회수 · 시간 초과", com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.REVOKED, cause = "시간 초과")))
        assertEquals("발언 요청 시간 초과", com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.REQUEST_TIMEOUT)))
        assertNull(com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.GRANTED, com.cims.ue.sdk.FloorState.SPEAKING)))
        assertNull(com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.IDLE)))
    }

    @Test fun `카드의 floor 한 줄은 이벤트 종류가 정한다`() {
        assertEquals("요청 거부", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.DENIED)))
        assertEquals("발언권 회수", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.REVOKED)))
        assertEquals("대기 2번째", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.QUEUE_POSITION, com.cims.ue.sdk.FloorState.QUEUED, pos = 2)))
        assertEquals("", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.QUEUE_CANCELLED)))
        assertEquals("", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.GRANTED, com.cims.ue.sdk.FloorState.SPEAKING)))
        assertEquals("요청 시간 초과", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.REQUEST_TIMEOUT)))
    }

    @Test fun `끝난 발언에는 길이를 붙인다`() {
        assertEquals("박현장 발언 14초", com.cims.ue.dispatch.session.talkEndText("박현장", 1_000, 15_200))
        assertEquals("나 발언", com.cims.ue.dispatch.session.talkEndText("나", null, 15_000))
    }

    // ── 로스터 ──
    @Test fun `부분 갱신은 온 사람만 바꾸고 전체 스냅샷은 갈아 끼운다`() {
        val a = com.cims.ue.sdk.RosterEntry("tel:+821", "connected")
        val b = com.cims.ue.sdk.RosterEntry("tel:+822", "connected")
        val c = com.cims.ue.sdk.RosterEntry("tel:+823", "connected")
        // 한 명이 들어오는 부분 갱신 — 나머지는 그대로
        assertEquals(listOf(a, b, c), com.cims.ue.dispatch.session.mergeRoster(listOf(a, b), listOf(c), full = false))
        // 나간 사람은 빠진다
        assertEquals(listOf(b), com.cims.ue.dispatch.session.mergeRoster(listOf(a, b),
            listOf(com.cims.ue.sdk.RosterEntry("TEL:+821", "disconnected")), full = false))
        // 상태가 바뀐 사람은 새 상태로
        val held = com.cims.ue.sdk.RosterEntry("tel:+821", "on-hold")
        assertEquals(listOf(b, held), com.cims.ue.dispatch.session.mergeRoster(listOf(a, b), listOf(held), full = false))
        assertEquals(listOf(c), com.cims.ue.dispatch.session.mergeRoster(listOf(a, b), listOf(c), full = true))
    }

    @Test fun `합류와 이탈은 접속 상태의 차이다`() {
        val a = com.cims.ue.sdk.RosterEntry("tel:+821", "connected")
        val b = com.cims.ue.sdk.RosterEntry("tel:+822", "connected")
        val (joined, left) = com.cims.ue.dispatch.session.rosterMoves(listOf(a), listOf(b, com.cims.ue.sdk.RosterEntry("tel:+823", "on-hold")))
        assertEquals(listOf("tel:+822"), joined)
        assertEquals(listOf("tel:+821"), left)
    }

    // ── SDK 접두가 붙은 오류 본문 ──
    @Test fun `SDK 가 호출 이름을 앞에 붙인 그룹 오류도 세분 문구로 읽는다`() {
        val body = """putGroup 403: {"error":"not_group_owner"}"""
        assertEquals(
            com.cims.ue.dispatch.session.ResponseText.of(com.cims.ue.dispatch.session.TextArea.GROUP, 403, """{"error":"not_group_owner"}"""),
            com.cims.ue.dispatch.session.ResponseText.of(com.cims.ue.dispatch.session.TextArea.GROUP, 403, body))
        assertTrue(com.cims.ue.dispatch.session.ResponseText.of(com.cims.ue.dispatch.session.TextArea.GROUP, 403, body).contains("본인"))
    }

    // ── 외부망 문자 ──
    @Test fun `외부망 문자는 게이트웨이가 없을 때만 막는다`() {
        assertTrue(com.cims.ue.dispatch.session.smsBlocked(external = true, gateway = false))
        assertFalse(com.cims.ue.dispatch.session.smsBlocked(external = true, gateway = true))
        assertFalse(com.cims.ue.dispatch.session.smsBlocked(external = false, gateway = false))
    }

    // ── 로그인 실패 문구 ──
    @Test fun `자격 거부는 고칠 수 있는 말로, 그 밖은 사유와 코드를 붙인다`() {
        assertEquals("아이디 또는 비밀번호가 올바르지 않습니다", com.cims.ue.dispatch.ui.loginErrorText(401, "invalid_grant"))
        assertEquals("아이디 또는 비밀번호가 올바르지 않습니다", com.cims.ue.dispatch.ui.loginErrorText(403, ""))
        assertEquals("로그인 실패 — authreq: connect failed (-1)", com.cims.ue.dispatch.ui.loginErrorText(-1, "authreq: connect failed"))
    }

    // ── 서버 통합 이력 폴링 ──
    @Test fun `이력 응답을 오래된 것부터 읽고 필수 필드가 없는 항목은 건너뛴다`() {
        val json = """{"items":[
            {"id":"b","time":"2026-09-06T19:05:12+09:00","kind":"ptt","event":"ptt.session.end","from":"tel:+82100001003","group":"tel:g010","duration":75,"emergency":true},
            {"id":"a","time":"2026-09-06T19:00:00+09:00","event":"message.sds","from":"tel:+82100001003","group":"tel:g010","text":"확인"},
            {"id":"","time":"2026-09-06T19:06:00+09:00"},
            {"id":"c","time":"어제"}
        ],"next":"2026-09-06T19:05:12"}"""
        val (items, next) = HistoryFeed.parse("message", json)
        assertEquals(listOf("a", "b"), items.map { it.id })
        assertEquals("2026-09-06T19:05:12", next)
        assertEquals("message", items[0].kind)                    // 항목에 kind 가 없으면 물은 종류
        assertEquals("ptt", items[1].kind)
        assertEquals(75, items[1].durationSec)
        assertTrue(items[1].emergency)
        assertEquals(java.time.OffsetDateTime.parse("2026-09-06T19:05:12+09:00").toInstant().toEpochMilli(), items[1].atMs)
    }

    @Test fun `깨진 이력 본문은 빈 결과다`() {
        val (items, next) = HistoryFeed.parse("ptt", "<html>502</html>")
        assertTrue(items.isEmpty())
        assertEquals("", next)
    }

    @Test fun `offset 없는 시각은 기기 시간대로 읽는다`() {
        val utc = java.time.ZoneId.of("UTC")
        assertEquals(0L, HistoryFeed.parseTime("1970-01-01T00:00:00", utc))
        assertNull(HistoryFeed.parseTime(""))
        assertNull(HistoryFeed.parseTime("12시"))
    }

    @Test fun `서버가 이력을 내지 않거나 범위가 없으면 폴링을 끈다`() {
        listOf(404, 501, 405, 403).forEach { assertTrue("$it", HistoryFeed.unavailable(it)) }
        listOf(-1, 401, 500, 503).forEach { assertFalse("$it", HistoryFeed.unavailable(it)) }
    }

    @Test fun `닿지 않으면 주기를 두 배씩 늘리고 30초에서 멈춘다`() {
        assertEquals(HistoryFeed.INTERVAL_MS, HistoryFeed.delayMs(0))
        assertEquals(HistoryFeed.INTERVAL_MS * 2, HistoryFeed.delayMs(1))
        assertEquals(HistoryFeed.MAX_BACKOFF_MS, HistoryFeed.delayMs(5))
        assertEquals(HistoryFeed.MAX_BACKOFF_MS, HistoryFeed.delayMs(50))
    }

    @Test fun `이력 event 는 이벤트 종류와 한 낱말로 옮긴다`() {
        assertEquals(ActivityKind.TALK, HistoryFeed.activityKind("ptt.talk"))
        assertEquals(ActivityKind.LEAVE, HistoryFeed.activityKind("ptt.session.end"))
        assertEquals(ActivityKind.EMERGENCY, HistoryFeed.activityKind("ptt.emergency"))
        assertEquals(ActivityKind.JOIN, HistoryFeed.activityKind("ptt.session.start"))
        assertEquals("세션 종료", HistoryFeed.eventText("ptt.session.end"))
        assertEquals("ptt.unknown", HistoryFeed.eventText("ptt.unknown"))
        assertEquals("1:15", HistoryFeed.durationText(75))
        assertEquals("", HistoryFeed.durationText(0))
    }

    // ── 세션 시작·종료 줄(데스크톱 `Create`·`Remove` 와 같은 낱말) ──
    @Test fun `세션 시작 줄은 종류마다 다르고 편성 그룹의 보통 호에는 없다`() {
        fun t(k: com.cims.ue.dispatch.session.SessionKind, incoming: Boolean = false, bc: Boolean = false, n: Int = 0, who: String = "") =
            com.cims.ue.dispatch.session.sessionStartText(k, incoming, bc, n, who)
        assertEquals("개별 통화 · 발신", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE))
        assertEquals("개별 통화 · 착신", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE, incoming = true))
        assertEquals("청취 시작", t(com.cims.ue.dispatch.session.SessionKind.PTT_LISTEN))
        assertEquals("애드혹 그룹 · 3명", t(com.cims.ue.dispatch.session.SessionKind.PTT_ADHOC, n = 3))
        assertEquals("애드혹 그룹 · 착신", t(com.cims.ue.dispatch.session.SessionKind.PTT_ADHOC, incoming = true))
        assertEquals("일제 통화 개시", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL, bc = true))
        assertEquals("일제 통화 · 박현장", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL, incoming = true, bc = true, who = "박현장"))
        assertEquals("일제 통화 개시", t(com.cims.ue.dispatch.session.SessionKind.PTT_ADHOC, bc = true, n = 2))
        assertNull("편성 그룹의 보통 호는 로스터가 «세션 시작» 으로 남긴다", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL))
        assertNull(t(com.cims.ue.dispatch.session.SessionKind.PHONE_CALL))
    }

    @Test fun `세션 종료 줄은 종류와 길이를 적고 성립 못 한 것은 실패 코드를 적는다`() {
        fun t(k: com.cims.ue.dispatch.session.SessionKind, ms: Long? = 75_000, bc: Boolean = false, n: Int = 0, code: Int = 200, mine: Boolean = false) =
            com.cims.ue.dispatch.session.sessionEndText(k, bc, ms, n, code, mine)
        assertEquals("세션 종료 · 1:15", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL))
        assertEquals("청취 종료 · 1:15", t(com.cims.ue.dispatch.session.SessionKind.PTT_LISTEN))
        assertEquals("개별 통화 종료 · 1:15", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE))
        assertEquals("애드혹 종료 · 1:15 · 참가 3", t(com.cims.ue.dispatch.session.SessionKind.PTT_ADHOC, n = 3))
        assertEquals("일제 통화 종료 · 0:09", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL, ms = 9_000, bc = true))
        // 성립하지 못한 호 — 길이 대신 실패 코드
        assertEquals("세션 실패 403", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL, ms = null, code = 403))
        assertEquals("개별 통화 종료 · 실패 480", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE, ms = null, code = 480))
        // 내가 거둔 호(CANCEL → 487)는 실패가 아니다
        assertEquals("세션 종료", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL, ms = null, code = 487, mine = true))
        assertEquals("개별 통화 종료", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE, ms = null, code = 487, mine = true))
        // 성립한 뒤의 종료 코드는 실패가 아니다
        assertEquals("세션 종료 · 1:15", t(com.cims.ue.dispatch.session.SessionKind.PTT_CHANNEL, code = 408))
    }

    // ── 표시용 번호 ──
    @Test fun `홈 국가 번호는 국내 표기로 보이고 그 밖은 그대로다`() {
        val d = { n: String -> com.cims.ue.dispatch.session.DirectoryBook.displayNumber(n, "82") }
        assertEquals("01012345678", d("+821012345678"))
        assertEquals("0510001002", d("+82510001002"))
        assertEquals("국제 번호는 그대로", "+14155550100", d("+14155550100"))
        assertEquals("내선·그룹 id 는 그대로", "1003", d("1003"))
        assertEquals("g001", d("g001"))
        assertEquals("너무 짧으면 바꾸지 않는다", "+821", d("+821"))
        assertEquals("", d(""))
    }

    // ── 발신 입력 정규화(데스크톱 `CallOriginateViewModel.Resolve`) ──
    @Test fun `번호칸에 친 것은 구분자를 빼고 주소록의 원 번호로 건다`() {
        val book = com.cims.ue.dispatch.session.DirectoryBook(entries = listOf(
            com.cims.ue.dispatch.session.DirectoryEntry("HQ", "박현장", "+821012345678"),
            com.cims.ue.dispatch.session.DirectoryEntry("HQ", "이당직", "1002"),
            com.cims.ue.dispatch.session.DirectoryEntry("HQ", "김동명", "1003"),
            com.cims.ue.dispatch.session.DirectoryEntry("FD", "김동명", "1004")))
        val d = { t: String -> com.cims.ue.dispatch.session.dialTargetOf(t, book) }
        assertEquals("국내 표기로 쳐도 저장된 가입 id 로", "+821012345678", d("010-1234-5678"))
        assertEquals("+821012345678", d(" 010 1234 5678 "))
        assertEquals("이름이면 그 사람 번호", "+821012345678", d("박현장"))
        assertEquals("1002", d("이당직"))
        assertEquals("동명이인이면 친 대로 둔다", "김동명", d("김동명"))
        assertEquals("주소록에 없는 번호는 구분자만 뺀다", "0299998888", d("02-9999-8888"))
        assertEquals("피처코드는 그대로", "*81003", d("*8 1003"))
        assertEquals("URI 는 그대로", "sip:1003@voip.test", d("sip:1003@voip.test"))
        assertEquals("", d("  "))
    }

    // ── 오늘 데스크 집계 ──
    @Test fun `오늘 집계는 오늘 끝난 내 줄만 세고 타인 통화는 뺀다`() {
        val K = com.cims.ue.dispatch.session.CallLogKind.ANSWERED
        fun row(at: Long, kind: com.cims.ue.dispatch.session.CallLogKind, others: Boolean = false) =
            com.cims.ue.dispatch.session.CallLogRow(atMs = at, peer = "p", text = "", kind = kind, others = others)
        val rows = listOf(
            row(2_000, K), row(2_100, com.cims.ue.dispatch.session.CallLogKind.PICKUP),
            row(2_200, com.cims.ue.dispatch.session.CallLogKind.MISSED),
            row(2_300, com.cims.ue.dispatch.session.CallLogKind.OUTGOING),
            row(2_400, K, others = true),                       // 타인 통화 — 세지 않는다
            row(900, com.cims.ue.dispatch.session.CallLogKind.MISSED))   // 어제 — 세지 않는다
        val t = com.cims.ue.dispatch.session.deskTallyOf(rows, sinceMs = 1_000)
        assertEquals(2, t.answered)
        assertEquals(1, t.missed)
        assertEquals(1, t.outgoing)
        assertEquals(0, t.transfer)
    }

    // ── 문자 대화 키 ──
    @Test fun `문자 대화 키는 정규형이라 국내 표기와 국제 표기가 한 대화다`() {
        val k = { n: String -> com.cims.ue.dispatch.session.smsKey(n) }
        assertEquals(k("01012345678"), k("+821012345678"))
        assertEquals(k("sip:+821012345678@voip.test"), k("010-1234-5678"))
        assertEquals("내선은 그대로", "1003", k("1003"))
    }

    // ── 애드혹 카드 제목 ──
    @Test fun `애드혹 제목은 초대한 사람 앞 둘과 나머지 수 — 남이 연 것은 개시자`() {
        val t = { invited: List<String>, by: String -> com.cims.ue.dispatch.ui.ptt.adhocTitle(invited, by) }
        assertEquals("박현장", t(listOf("박현장"), ""))
        assertEquals("박현장 · 최순찰", t(listOf("박현장", "최순찰"), ""))
        assertEquals("박현장 · 최순찰 +2", t(listOf("박현장", "최순찰", "이당직", "정정비"), ""))
        assertEquals("애드혹 · 이당직", t(emptyList(), "이당직"))
        assertEquals("애드혹", t(emptyList(), ""))
    }

    @Test fun `문자 대화 키는 번호 모양일 때만 정규형이다`() {
        val k = { n: String -> com.cims.ue.dispatch.session.smsKey(n) }
        assertEquals("영숫자 신원은 원문 — 숫자만 남기면 다른 번호의 대화에 합쳐진다", "noc2", k("sip:noc2@voip.test"))
        assertEquals("국제 접두는 국내 표기로 읽지 않는다", "00114155550100", k("00114155550100"))
        assertEquals(k("+821012345678"), k("010 1234 5678"))
    }

    @Test fun `주소록 번호에 구분자가 있어도 걸 때는 뺀다`() {
        val book = com.cims.ue.dispatch.session.DirectoryBook(entries = listOf(
            com.cims.ue.dispatch.session.DirectoryEntry("", "홍길동", "010-1234-5678", external = true)))
        assertEquals("01012345678", com.cims.ue.dispatch.session.dialTargetOf("01012345678", book))
        assertEquals("01012345678", com.cims.ue.dispatch.session.dialTargetOf("홍길동", book))
    }

    @Test fun `받지 못한 착신과 거절된 청취의 종료 줄`() {
        fun t(k: com.cims.ue.dispatch.session.SessionKind, code: Int, incoming: Boolean) =
            com.cims.ue.dispatch.session.sessionEndText(k, false, null, 0, code, mine = false, incoming = incoming)
        // 상대가 응답 전에 거둔 착신(487)·내가 거절한 착신(486)은 실패가 아니다
        assertEquals("개별 통화 종료", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE, 487, incoming = true))
        assertEquals("개별 통화 종료", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE, 486, incoming = true))
        // 내가 건 것이 성립하지 못했으면 실패다 — 청취도
        assertEquals("개별 통화 종료 · 실패 480", t(com.cims.ue.dispatch.session.SessionKind.PTT_PRIVATE, 480, incoming = false))
        assertEquals("청취 종료 · 실패 403", t(com.cims.ue.dispatch.session.SessionKind.PTT_LISTEN, 403, incoming = false))
    }

    // 로그인·재구독 때 진행 중이던 통화는 첫 NOTIFY 가 confirmed 다 — 그렇게 세우지 않으면 끝날 때 «부재» 로 적힌다
    @Test fun `처음부터 confirmed 로 보인 dialog 는 응답된 통화다`() {
        fun info(state: String) = com.cims.ue.sdk.DialogInfo(0, "sip:1001@d", "d1", "c1", "lt", "rt", "recipient", state, "sip:02123@d", true)
        val live = com.cims.ue.dispatch.session.DialogRow.of(info("confirmed"))
        assertTrue(live.wasConfirmed)
        assertEquals(live.startedAtMs, live.confirmedAtMs)
        val ringing = com.cims.ue.dispatch.session.DialogRow.of(info("early"))
        assertFalse(ringing.wasConfirmed)
        assertNull(ringing.confirmedAtMs)
        // 울리다 응답 — 그때부터 응답된 통화다. 끝나도 그대로다
        val answered = ringing.apply(info("confirmed")).apply(info("terminated"))
        assertTrue(answered.wasConfirmed)
        assertTrue(answered.confirmedAtMs != null)
    }

    @Test fun `발언 시간이 다해 끊기면 까닭을 적는다`() {
        assertEquals("발언 시간 초과 — 발언이 끝났습니다", com.cims.ue.dispatch.session.floorFailureText(floor(com.cims.ue.sdk.FloorEventKind.TALK_LIMIT)))
        // 카드의 사유 줄에는 적지 않는다 — 발언 칩이 «거부» 로 읽힌다
        assertEquals("", com.cims.ue.dispatch.session.floorNoteOf(floor(com.cims.ue.sdk.FloorEventKind.TALK_LIMIT)))
    }

    @Test fun `URI 에서 번호를 자를 때 포트와 표시 이름의 쌍점은 스킴이 아니다`() {
        val up = { u: String -> com.cims.ue.dispatch.session.userPart(u) }
        assertEquals("1001", up("sip:1001@d;user=phone"))
        assertEquals("+8210111", up("tel:+8210111"))
        assertEquals("1001", up("1001"))
        assertEquals("포트를 번호로 읽지 않는다", "1001", up("1001@host:5060"))
        assertEquals("1001", up("sip:1001@host:5060"))
        assertEquals("표시 이름의 쌍점", "1001", up("\"1팀: 순찰\" <sip:1001@d>"))
        assertEquals("표시 이름의 골뱅이", "1001", up("\"a@b\" <sip:1001@d>"))
        assertEquals("+8210111", up("<tel:+8210111>"))
    }

    @Test fun `숫자가 없는 신원의 이름은 원문으로 찾는다`() {
        val book = com.cims.ue.dispatch.session.DirectoryBook(entries = listOf(
            com.cims.ue.dispatch.session.DirectoryEntry("", "관제1", "noc1"),
            com.cims.ue.dispatch.session.DirectoryEntry("", "관제2", "noc2")))
        assertEquals("관제2", book.nameOf("noc2"))
        assertEquals("", book.nameOf("noc9"))
    }

    @Test fun `JSON null 은 없는 값이다`() {
        val o = org.json.JSONObject("""{"a":null,"b":"x"}""")
        assertEquals("", o.str("a"))
        assertEquals("x", o.str("b"))
        assertEquals("", o.str("c"))
        assertEquals("-", o.str("c", "-"))
    }
}
