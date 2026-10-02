// MCVideo 영상 채널 계약 시험 — JVM, 기기 불필요 (android_dispatch_tablet.md §6.14 · §9)
//
// 노리는 것은 **조용히 어긋나면 관제사가 모르는** 규칙들이다: 영상 호가 전화 통화로 읽히는 것, 합류가 물러나지 않고 서버를 두드리는
// 것, N2·N6 한도를 넘겨 보내는 것, 영상을 보내는 중의 마이크 경합(D12), [영상 보내기] 한 버튼의 상태 글.
package com.cims.ue.dispatch

import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ResponseText
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.Settings
import com.cims.ue.dispatch.session.TextArea
import com.cims.ue.dispatch.session.VideoBanner
import com.cims.ue.dispatch.session.VideoCall
import com.cims.ue.dispatch.session.VideoCamera
import com.cims.ue.dispatch.session.VideoChannel
import com.cims.ue.dispatch.session.VideoMicPolicy
import com.cims.ue.dispatch.session.VideoOrient
import com.cims.ue.dispatch.session.VideoRules
import com.cims.ue.dispatch.session.VideoText
import com.cims.ue.dispatch.ui.toBannerUi
import com.cims.ue.dispatch.ui.videoCameraOf
import com.cims.ue.dispatch.ui.ptt.EventKind
import com.cims.ue.dispatch.ui.ptt.MineCardUi
import com.cims.ue.dispatch.ui.ptt.kindLabel
import com.cims.ue.dispatch.ui.ptt.selfViewRotation
import com.cims.ue.dispatch.ui.ptt.videoBoxSize
import com.cims.ue.dispatch.ui.ptt.videoSectionUi
import com.cims.ue.dispatch.ui.ptt.videoSendText
import com.cims.ue.dispatch.ui.ptt.withVideo
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.McService
import com.cims.ue.sdk.McVideoGroupAttrs
import com.cims.ue.sdk.McpttCondition
import com.cims.ue.sdk.McpttInfo
import com.cims.ue.sdk.ReceptionEventKind
import com.cims.ue.sdk.ReceptionState
import com.cims.ue.sdk.TransmissionEventKind
import com.cims.ue.sdk.TransmissionState
import com.cims.ue.sdk.VideoTransmitter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class VideoPlaneTest {

    private fun info(
        id: Int = 7, groupId: String = "g1", state: CallState = CallState.ACTIVE, service: McService = McService.MCVIDEO,
        video: Boolean = true, mcptt: Boolean = false,
    ) = CallInfo(
        callId = id, accountId = 0, dir = CallDir.OUTGOING, state = state,
        remoteUri = "sip:mcvideo@d", calledParty = "", video = video, mediaActive = true,
        muted = false, listen = true, playbackRoute = 0, lastCode = 0, lastReason = "",
        sources = emptyList(), isMcptt = mcptt, groupId = groupId,
        mcptt = McpttInfo(mcptt, "", "", "", "", false, false, false, false),
        halfDuplex = mcptt, listenOnly = false, joinedDialog = "",
        condition = McpttCondition(), service = service)

    private fun tx(user: String, state: ReceptionState = ReceptionState.NOTIFIED, alias: String = "") =
        VideoTransmitter(userId = user, audioSsrc = 1, videoSsrc = 2, functionalAlias = alias, automatic = false, state = state)

    private fun call(
        state: CallState = CallState.ACTIVE, tx: TransmissionState = TransmissionState.NO_PERMISSION,
        transmitters: List<VideoTransmitter> = emptyList(), opening: Boolean = false, queue: Int = 0,
    ) = VideoCall(callId = 7, groupId = "g1", info = info(state = state), tx = tx, transmitters = transmitters,
        since = transmitters.associate { it.userId to 1_000L }, opening = opening, queuePosition = queue)

    // ── 영상 호는 전화 통화가 아니다 ──

    @Test fun `MCVideo 호는 전화 통화로 분류되지 않는다`() {
        // isMcptt 가 false 라 따로 가르지 않으면 PHONE_CALL — 통화 카드·착신 배너·통화 내역에 샌다.
        assertEquals(SessionKind.MC_VIDEO, SessionKind.of(info()))
        assertEquals(SessionKind.PHONE_CALL, SessionKind.of(info(service = McService.MCPTT, groupId = "")))
        assertEquals(SessionKind.PTT_CHANNEL, SessionKind.of(info(service = McService.MCPTT, mcptt = true)))
    }

    @Test fun `영상 호는 카드도 시트도 아니다`() {
        assertFalse(SessionKind.MC_VIDEO.isPttCard)        // ① 내 채널 카드로 서지 않는다
        assertFalse(SessionKind.MC_VIDEO.isSheet)          // 감청·청취 수에 들지 않는다
    }

    // ── 게이트 ──

    @Test fun `영상은 사이트 PSI 와 이용 자격이 둘 다 있을 때만 켠다`() {
        assertTrue(VideoRules.enabled("sip:mcvideo@d", hasProfile = true))
        assertFalse(VideoRules.enabled("", hasProfile = true))              // 서버가 MCVideo 를 켜지 않은 사이트
        assertFalse(VideoRules.enabled("sip:mcvideo@d", hasProfile = false)) // user profile 404 = 자격 없음
    }

    // ── 합류 물러남(D10) ──

    @Test fun `합류 실패는 10초부터 배로 최대 2분까지 물러난다`() {
        assertEquals(listOf(10, 20, 40, 80, 120, 120, 120), (1..7).map(VideoRules::backoffSec))
        assertEquals(3, VideoRules.REJOIN_SEC)             // 섰다가 끝난 호(TNG3·서버 해제)는 곧 다시
    }

    @Test fun `끝난 영상 호 뒤 — chat 채널만 다시 합류하고 내가 끝낸 것은 다시 합류하지 않는다`() {
        fun next(mine: Boolean = false, enabled: Boolean = true, isChannel: Boolean = true, type: String = VideoRules.CHAT,
                 connected: Boolean = true, code: Int = 200) = VideoRules.rejoinAfterEnd(mine, enabled, isChannel, type, connected, code)
        assertEquals(false, next())                         // 섰다가 정상으로 끝남 → 곧 다시(물러남 없음)
        assertEquals(true, next(connected = false, code = 404))   // 서지 못한 합류 → 물러난다
        assertEquals(true, next(code = 503))                // 섰지만 오류로 끝남 → 물러난다
        assertNull(next(mine = true))                       // 그룹이 빠짐·자격 없어짐 — 내가 끝냈다
        assertNull(next(enabled = false))
        assertNull(next(isChannel = false))
        assertNull(next(type = VideoRules.PREARRANGED))     // 편성은 앱이 열어 두지 않는다 — [영상 보내기] 가 연다
    }

    // ── 한도(N2·N6) ──

    @Test fun `동시 제휴 그룹 한도 N2 — 카드 순서대로 채우고 넘는 채널은 제휴하지 않는다`() {
        val (within, over) = VideoRules.affiliationPlan(listOf("a", "b", "c", "d", "e"), n2 = 4)
        assertEquals(listOf("a", "b", "c", "d"), within)
        assertEquals(listOf("e"), over)
        assertEquals(listOf("a") to emptyList<String>(), VideoRules.affiliationPlan(listOf("a"), VideoRules.limitOf(null)))
        assertEquals(emptyList<String>() to listOf("a"), VideoRules.affiliationPlan(listOf("a"), n2 = 0))
    }

    @Test fun `동시 영상 호 상한 N6 — 모르면 상한 없음`() {
        assertTrue(VideoRules.withinN6(live = 1, n6 = 2))
        assertFalse(VideoRules.withinN6(live = 2, n6 = 2))  // 넘겨 보내면 서버가 486 103
        assertEquals(Int.MAX_VALUE, VideoRules.limitOf(null))
        assertEquals(Int.MAX_VALUE, VideoRules.limitOf(0))
        assertEquals(4, VideoRules.limitOf(4))
    }

    // ── 그룹 문서 ──

    @Test fun `호 방식은 그룹 문서의 MCVideo 몫에서 온다`() {
        assertEquals("", VideoRules.callType(null))                                   // MCVideo 그룹이 아니다
        assertEquals(VideoRules.CHAT, VideoRules.callType(McVideoGroupAttrs(inviteMembers = false)))
        assertEquals(VideoRules.PREARRANGED, VideoRules.callType(McVideoGroupAttrs(inviteMembers = true)))
    }

    @Test fun `처음 받은 문서와 같은 문서 재조회는 변경이 아니다`() {
        val p1 = VideoRules.docPrint("\"e1\"", VideoRules.CHAT, 2, listOf("tel:1"))
        assertFalse(VideoRules.docChanged(null, p1))        // 처음 — 물러남·affiliation 을 다시 하지 않는다
        assertFalse(VideoRules.docChanged(p1, p1))
        assertTrue(VideoRules.docChanged(p1, VideoRules.docPrint("\"e2\"", VideoRules.CHAT, 2, listOf("tel:1"))))
        // ETag 가 없으면 영상에 걸리는 값들로 가른다.
        assertTrue(VideoRules.docChanged(VideoRules.docPrint("", VideoRules.CHAT, 2, listOf("tel:1")),
            VideoRules.docPrint("", VideoRules.PREARRANGED, 2, listOf("tel:1"))))
    }

    // ── 마이크 경합(D12)·소리(D6) ──

    @Test fun `영상 우선이면 영상을 보내는 동안 무전 발언을 막는다 — 긴급·임박은 예외`() {
        fun blocks(policy: String = VideoMicPolicy.VIDEO, sending: Boolean = true, emg: Boolean = false, peril: Boolean = false) =
            VideoRules.blocksTalk(policy, sending, emg, peril)
        assertTrue(blocks())
        assertFalse(blocks(policy = VideoMicPolicy.VOICE))  // 음성 우선(기본) — 막지 않는다
        assertFalse(blocks(sending = false))
        assertFalse(blocks(emg = true))                     // TS 22.280 R-8.3-004
        assertFalse(blocks(peril = true))
        assertEquals(VideoMicPolicy.VOICE, Settings().videoMicPolicy)
    }

    @Test fun `음성 우선 — 무전을 말하는 동안 송출 중인 영상 호의 음성만 멈춘다`() {
        assertTrue(VideoRules.yieldsMic(talking = true, tx = TransmissionState.PERMITTED))
        assertFalse(VideoRules.yieldsMic(talking = false, tx = TransmissionState.PERMITTED))
        assertFalse(VideoRules.yieldsMic(talking = true, tx = TransmissionState.QUEUED))   // 아직 보내지 않는다
        assertTrue(listOf(FloorState.REQUESTING, FloorState.QUEUED, FloorState.SPEAKING).all(VideoRules::talking))
        assertFalse(VideoRules.talking(FloorState.IDLE)); assertFalse(VideoRules.talking(FloorState.LISTENING)); assertFalse(VideoRules.talking(null))
    }

    @Test fun `영상을 보는 동안만 그 그룹 무전을 줄인다`() {
        assertTrue(VideoRules.ducks(receiving = true)); assertFalse(VideoRules.ducks(receiving = false))
        assertEquals(0.3f, VideoRules.DUCK_LEVEL)
    }

    // ── 호 투영 ──

    @Test fun `끝난 송출은 목록에서 빠지고 처음 본 시각은 남는다`() {
        val c = call(transmitters = listOf(tx("tel:5003"))).copy(since = mapOf("tel:5003" to 1_000L))
        val next = c.withTransmission(TransmissionState.NO_PERMISSION, 0,
            listOf(tx("sip:5003@d", ReceptionState.RECEIVING), tx("tel:5004"), tx("tel:5005", ReceptionState.ENDED)), nowMs = 9_000L)
        assertEquals(listOf("sip:5003@d", "tel:5004"), next.transmitters.map { it.userId })
        assertEquals(1_000L, next.since["sip:5003@d"])       // 표기(tel·sip)가 달라도 같은 사람 — 경과가 다시 세어지지 않는다
        assertEquals(9_000L, next.since["tel:5004"])
        assertEquals("sip:5003@d", next.receiving?.userId)
    }

    @Test fun `여는 중은 내가 연 편성 호가 성립 전일 때만`() {
        assertTrue(call(state = CallState.OUTGOING, opening = true).isOpening)
        assertFalse(call(state = CallState.ACTIVE, opening = true).isOpening)
        assertFalse(call(state = CallState.OUTGOING).isOpening)             // chat 합류 중
        assertTrue(listOf(TransmissionState.PERMITTED, TransmissionState.PENDING_REQUEST, TransmissionState.QUEUED)
            .all { call(tx = it).txBusy })
        assertFalse(call(tx = TransmissionState.PENDING_END).txBusy)
    }

    @Test fun `새 영상 배너는 그 송출이 알림 상태일 때만 남는다`() {
        val c = call(transmitters = listOf(tx("tel:5003")))
        assertTrue(VideoRules.bannerValid(c, "sip:5003@d"))
        assertFalse(VideoRules.bannerValid(c.copy(transmitters = listOf(tx("tel:5003", ReceptionState.RECEIVING))), "tel:5003"))  // 보기 시작
        assertFalse(VideoRules.bannerValid(c.copy(transmitters = emptyList()), "tel:5003"))                                    // 송출 끝
        assertFalse(VideoRules.bannerValid(null, "tel:5003"))                                                                   // 영상 호 끝
        val b = VideoBanner("g1", 7, "tel:5003", "순찰1", "김현장(현장지휘)", 5L).toBannerUi()
        assertEquals("새 영상 · 순찰1", b.head)
        assertEquals("김현장(현장지휘)이 영상을 보냅니다", b.line)
    }

    // ── 영상 칸 ──

    @Test fun `회전은 90도씩 — 세로 송출을 옆으로 돌리면 칸이 가로다`() {
        assertEquals(90, VideoRules.rotate(0, 90)); assertEquals(270, VideoRules.rotate(0, -90)); assertEquals(0, VideoRules.rotate(270, 90))
        assertFalse(VideoOrient().landscape)                              // 단말 기본 480×640 세로
        assertTrue(VideoOrient(rotation = 90).landscape)
        assertTrue(VideoOrient(wide = true).landscape)                    // 가로로 인코딩한 송출(PC 관제 앱 640×480)
        assertFalse(VideoOrient(rotation = 270, wide = true).landscape)
    }

    @Test fun `영상 칸은 세로 3대4 가 기본이고 패널 폭 안에 든다`() {
        assertEquals(180.dp to 240.dp, videoBoxSize(368.dp, landscape = false, large = false))
        assertEquals(320.dp to 240.dp, videoBoxSize(368.dp, landscape = true, large = false))
        assertEquals(330.dp to 440.dp, videoBoxSize(368.dp, landscape = false, large = true))   // 칸을 눌러 크게
        assertEquals(288.dp to 216.dp, videoBoxSize(288.dp, landscape = true, large = false))   // 좁은 패널 — 폭에 죈다
    }

    @Test fun `셀프뷰는 화면이 돈 만큼 되돌려 세운다`() {
        assertEquals(0f, selfViewRotation(0)); assertEquals(-90f, selfViewRotation(90))
        assertEquals(180f, selfViewRotation(180)); assertEquals(90f, selfViewRotation(270))
    }

    // ── «영상» 절 ──

    private fun section(channel: VideoChannel? = VideoChannel("g1", VideoRules.CHAT, affiliated = true), call: VideoCall? = null,
                        cameras: Int = 2, canOpen: Boolean = false, micYielded: Boolean = false,
                        orients: Map<String, VideoOrient> = emptyMap()) =
        videoSectionUi(channel, call, cameras, canOpen, micYielded, orients, nameOf = { "이름${it.takeLast(1)}" }, nowMs = 35_000L)

    @Test fun `영상 호가 없으면 연결 안내만 선다`() {
        val ui = section(channel = VideoChannel("g1", VideoRules.CHAT, note = "영상 연결 안 됨 — … · 20초 뒤 다시"))
        assertEquals("영상 연결 안 됨 — … · 20초 뒤 다시", ui.sub)
        assertFalse(ui.showSend); assertFalse(ui.receiving); assertFalse(ui.noSender); assertTrue(ui.senders.isEmpty())
        assertEquals(VideoRules.JOINING_NOTE, section(channel = VideoChannel("g1", VideoRules.CHAT)).sub)
        assertEquals(VideoRules.JOINING_NOTE, section(call = call(state = CallState.OUTGOING)).sub)   // INVITE 를 보냈지만 아직
    }

    @Test fun `연결됐고 보내는 사람이 없으면 영상 보내기만 선다`() {
        val ui = section(call = call())
        assertEquals("채널 참여와 함께 연결됨", ui.sub)
        assertTrue(ui.noSender); assertTrue(ui.showSend); assertTrue(ui.sendEnabled); assertTrue(ui.sendStarts)
        assertEquals("영상 보내기", ui.sendText)
        assertFalse(ui.sendActive)
    }

    @Test fun `카메라가 없으면 영상 보내기가 꺼진다 — 보기는 된다`() {
        val ui = section(call = call(transmitters = listOf(tx("tel:5003"))), cameras = 0)
        assertEquals("영상 보내기 — 카메라 없음", ui.sendText)
        assertFalse(ui.sendEnabled)
        assertTrue(ui.senders.single().canAccept)
    }

    @Test fun `송출 줄 — 보는 중과 바꿔 보기`() {
        val ui = section(call = call(transmitters = listOf(
            tx("tel:5003", ReceptionState.RECEIVING, alias = "현장지휘"), tx("tel:5004"), tx("tel:5005", ReceptionState.PENDING_REQUEST))),
            orients = mapOf("5003" to VideoOrient(rotation = 90)))
        assertTrue(ui.receiving)
        assertEquals("tel:5003", ui.receivingId)
        assertEquals("이름3 · 현장지휘 · 00:34", ui.caption)
        assertEquals(VideoOrient(rotation = 90), ui.orient)               // 보내는 사람마다 기억한 방향
        assertEquals("보내는 중 3 · 보는 중 1 (한 번에 1개)", ui.countText)
        val (a, b, c) = ui.senders
        assertTrue(a.receiving); assertEquals("현장지휘 · 00:34", a.meta)
        assertEquals("바꿔 보기", b.action); assertTrue(b.canAccept); assertEquals("00:34", b.meta)
        assertEquals("요청 중…", c.action); assertFalse(c.canAccept)
        assertEquals("보기", section(call = call(transmitters = listOf(tx("tel:5004")))).senders.single().action)
    }

    @Test fun `영상 보내기 한 버튼 — 상태마다 같은 자리가 끄는 버튼이 된다`() {
        assertEquals("여는 중… · 취소", videoSendText(opening = true, TransmissionState.PENDING_REQUEST, 0, hasCamera = true))
        assertEquals("요청 중… · 취소", videoSendText(false, TransmissionState.PENDING_REQUEST, 0, true))
        assertEquals("대기 2번째 · 대기 취소", videoSendText(false, TransmissionState.QUEUED, 2, true))
        assertEquals("대기 중 · 대기 취소", videoSendText(false, TransmissionState.QUEUED, 0, true))
        assertEquals("대기 중 · 대기 취소", videoSendText(false, TransmissionState.QUEUED, 254, true))   // 254·255 = 순번 없음
        assertEquals("보내기 끝", videoSendText(false, TransmissionState.PERMITTED, 0, true))
        assertEquals("끝내는 중…", videoSendText(false, TransmissionState.PENDING_END, 0, true))

        val sending = section(call = call(tx = TransmissionState.PERMITTED).copy(txSinceMs = 23_000L, txReceivers = setOf("5003", "5004")),
            micYielded = true)
        assertTrue(sending.sending); assertTrue(sending.sendActive); assertFalse(sending.sendStarts); assertTrue(sending.sendEnabled)
        assertEquals("내 영상 보내는 중 · 00:12 · 보는 사람 2", sending.sendingCaption)
        assertTrue(sending.micNote)                                       // D12 — 무전 중, 영상 소리는 멈춤
        assertTrue(sending.canSwitchCamera)
        assertFalse(section(call = call(tx = TransmissionState.PENDING_END)).sendEnabled)   // 끝내는 중에는 누르지 못한다
    }

    @Test fun `편성 영상 채널 — 영상 호가 없으면 영상 보내기가 영상 호를 연다`() {
        val ch = VideoChannel("g1", VideoRules.PREARRANGED, affiliated = true, note = VideoRules.PREARRANGED_NOTE)
        val idle = section(channel = ch, canOpen = true)
        assertEquals(VideoRules.PREARRANGED_NOTE, idle.sub)
        assertTrue(idle.showSend); assertTrue(idle.sendEnabled); assertTrue(idle.sendStarts)
        assertFalse(section(channel = ch, canOpen = false).sendEnabled)   // 제휴가 아직 서지 않았거나 카메라가 없다
        // 제휴가 서기 전에는 버튼이 꺼져 있는 까닭을 머리 옆에 적는다(태블릿엔 툴팁이 없다).
        assertEquals(VideoRules.NOT_AFFILIATED_NOTE, section(channel = ch.copy(affiliated = false), canOpen = false).sub)
        val opening = section(channel = ch, call = call(state = CallState.OUTGOING, tx = TransmissionState.PENDING_REQUEST, opening = true))
        assertEquals(VideoRules.OPENING_NOTE, opening.sub)
        assertEquals("여는 중… · 취소", opening.sendText)
        assertTrue(opening.sendEnabled); assertTrue(opening.sendActive); assertFalse(opening.sendStarts)
        assertEquals("편성 영상 호 연결됨", section(channel = ch, call = call()).sub)
    }

    @Test fun `영상 미디어가 열리지 않은 호는 그림이 오지 않는다`() {
        val c = call(transmitters = listOf(tx("tel:5003", ReceptionState.RECEIVING))).copy(info = info(video = false))
        assertFalse(section(call = c).canRender)
        assertTrue(section(call = call()).canRender)
    }

    // ── 카드·이벤트·문구 ──

    @Test fun `카드의 영상 n 은 보내는 중인 사람 수 — 없으면 태그도 없다`() {
        val card = MineCardUi("g1", "1. 순찰1")
        assertEquals(0, card.withVideo(emptyList()).videoCount)
        assertEquals(0, card.withVideo(listOf(call())).videoCount)
        val watching = card.withVideo(listOf(call(transmitters = listOf(tx("tel:5003", ReceptionState.RECEIVING), tx("tel:5004")))))
        assertEquals(2, watching.videoCount); assertTrue(watching.videoWatching)
        assertEquals(0, MineCardUi("g2", "2. 상황실").withVideo(listOf(call(transmitters = listOf(tx("tel:5003"))))).videoCount)
    }

    @Test fun `이벤트 종류에 영상이 있다`() {
        assertEquals("영상", kindLabel(ActivityKind.VIDEO))
        assertTrue(EventKind.entries.any { it.label == "영상" && ActivityKind.VIDEO in it.kinds })
    }

    @Test fun `영상 호 응답 문구는 데스크톱과 같은 문장이다`() {
        assertEquals("영상 그룹 멤버가 아니거나 영상(MCVideo) 이용 자격이 없습니다", ResponseText.sip(TextArea.VIDEO, 403, "Forbidden"))
        assertEquals("영상 그룹이 아니거나 영상 세션이 끝났습니다", ResponseText.sip(TextArea.VIDEO, 404, ""))
        assertEquals("동시에 참가할 수 있는 영상 호 수를 넘었습니다", ResponseText.sip(TextArea.VIDEO, 486, ""))
        assertEquals("영상 호를 열지 못했습니다 — 영상을 받을 멤버가 없습니다", ResponseText.sip(TextArea.VIDEO, 480, ""))
        assertEquals("영상 서버 자원이 없습니다 — 잠시 후 다시", ResponseText.sip(TextArea.VIDEO, 503, ""))
        assertEquals("실패 (487 Request Terminated)", ResponseText.sip(TextArea.VIDEO, 487, "Request Terminated"))
        assertEquals("영상 연결 안 됨 — 영상 그룹이 아니거나 영상 세션이 끝났습니다 · 20초 뒤 다시", VideoText.retryNote(404, 20))
        assertEquals("영상 연결 안 됨 — 호를 열지 못했습니다 · 10초 뒤 다시", VideoText.retryNote(-1, 10))
    }

    @Test fun `전송 제어 원인 문구`() {
        assertEquals("더 볼 수 없습니다 — 동시에 볼 수 있는 영상(1)이 찼습니다 · [바꿔 보기]",
            VideoText.reception(ReceptionEventKind.REJECTED, 7, "김현장"))
        assertEquals("김현장 영상을 볼 수 없습니다 — 송출이 이미 끝났습니다", VideoText.reception(ReceptionEventKind.REJECTED, 255, "김현장"))
        assertEquals("김현장 영상 보기 요청에 응답이 없습니다", VideoText.reception(ReceptionEventKind.REQUEST_TIMEOUT, 0, "김현장"))
        assertEquals("보내지 못했습니다 — 동시에 보낼 수 있는 수(2)가 찼습니다", VideoText.transmission(TransmissionEventKind.REJECTED, 1, 2))
        assertEquals("보내지 못했습니다 — 동시에 보낼 수 있는 수가 찼습니다", VideoText.transmission(TransmissionEventKind.REJECTED, 1, 0))
        assertEquals("이 그룹에서는 영상을 받기만 할 수 있습니다", VideoText.transmission(TransmissionEventKind.REJECTED, 5, 2))
        assertEquals("보내기가 멈췄습니다 — 한 번에 보낼 수 있는 시간을 넘었습니다", VideoText.transmission(TransmissionEventKind.REVOKED, 2, 2))
        assertEquals("보내기가 멈췄습니다 — 우선순위가 높은 송출이 들어왔습니다", VideoText.transmission(TransmissionEventKind.REVOKED, 4, 2))
        assertEquals("보내기가 멈췄습니다", VideoText.transmission(TransmissionEventKind.REVOKED, 99, 2))
    }

    @Test fun `주격 조사는 이름의 받침으로 고른다`() {
        assertEquals("김현장이", VideoText.withIGa("김현장"))
        assertEquals("박경수가", VideoText.withIGa("박경수"))
        assertEquals("김현장(현장지휘)이", VideoText.withIGa("김현장(현장지휘)"))   // 괄호 앞 이름의 끝 글자
        assertEquals("5003이", VideoText.withIGa("5003")); assertEquals("5002가", VideoText.withIGa("5002"))
        assertEquals("Kim이(가)", VideoText.withIGa("Kim"))
    }

    @Test fun `카메라 설정 — 엔진 장치 이름에서 앞뒤를 가른다`() {
        assertEquals(VideoCamera.FRONT, videoCameraOf("Front camera"))
        assertEquals(VideoCamera.BACK, videoCameraOf("Back camera"))
        assertNull(videoCameraOf("Colorbar generator"))
        assertEquals(VideoCamera.FRONT, Settings().videoCamera)           // 엔진의 처음 카메라와 같다
    }
}
