// **실제 태블릿 한 장** Preview — 상단 바 + 본문 + 발언 바 + 하단 내비 (android_dispatch_tablet.md §6.3)
//
// 화면별 Preview(`*ScreenPreview.kt`)는 **본문만** 그린다 — 그 영역의 밀도를 볼 때 쓴다.
// 이 파일은 그 위아래 껍데기(56 + 80 + 56 = 192dp)까지 붙여 **기기에서 보이는 그대로** 를 보여 준다.
// 두 벌이 필요한 이유: 본문만 보면 실제보다 넉넉해 보이고, 껍데기까지 보면 본문 안이 작아 잘 안 보인다.
//
// 크기는 가로 1280×800dp — 관제 태블릿 실물(§12).
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Alignment
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.AdminScope
import com.cims.ue.dispatch.session.AdminView
import com.cims.ue.dispatch.session.CallLogKind
import com.cims.ue.dispatch.session.CallLogRow
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.session.MemberInfo
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.NumberInfo
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.ui.admin.AdminScreenContent
import com.cims.ue.dispatch.ui.admin.AdminUi
import com.cims.ue.dispatch.ui.call.MemberChip
import com.cims.ue.dispatch.ui.groups.DetailMember
import com.cims.ue.dispatch.ui.groups.GroupsUi
import com.cims.ue.dispatch.ui.groups.PttGroupsScreenContent
import com.cims.ue.dispatch.ui.ptt.ActivityContent
import com.cims.ue.dispatch.ui.ptt.ActivityFilter
import com.cims.ue.dispatch.ui.ptt.MessagesContent
import com.cims.ue.dispatch.ui.ptt.ThreadChip
import com.cims.ue.sdk.MediaSource
import com.cims.ue.sdk.RosterEntry
import com.cims.ue.dispatch.session.DeskTally
import com.cims.ue.dispatch.session.DialogRow
import com.cims.ue.dispatch.ui.call.LiveCallRow
import com.cims.ue.sdk.DialogInfo
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.ui.call.CallsScreenContent
import com.cims.ue.dispatch.session.MessageKind
import com.cims.ue.dispatch.session.SendState
import com.cims.ue.dispatch.ui.call.SmsPaneContent
import com.cims.ue.dispatch.ui.call.CallsUi
import com.cims.ue.dispatch.ui.history.HistoryScreenContent
import com.cims.ue.dispatch.ui.history.HistoryUi
import com.cims.ue.dispatch.ui.ptt.ChannelHeadUi
import com.cims.ue.dispatch.ui.ptt.ChannelRowUi
import com.cims.ue.dispatch.ui.ptt.ChannelScreenContent
import com.cims.ue.dispatch.ui.ptt.PttScreenContent
import com.cims.ue.dispatch.ui.ptt.PttTabs
import com.cims.ue.dispatch.ui.ptt.ScopeFilter
import com.cims.ue.dispatch.ui.ptt.TalkBarContent
import com.cims.ue.dispatch.ui.ptt.TalkTargetChip
import com.cims.ue.dispatch.ui.ptt.CardKind
import com.cims.ue.dispatch.ui.ptt.ChannelCard
import com.cims.ue.sdk.FloorState
import androidx.compose.ui.Modifier

private val TOP = TopBarUi(
    displayName = "김관제",
    deskLine = "관제1과 · 대표 7000",
    registrations = listOf(true, true, false))

private val BADGES = NavBadges(unread = 3, adminDirty = true)

/** 발언 대상 칩 하나 — floor 상태가 «승인/대기/요청» 표시를 정한다. */
private fun chip(title: String, floor: FloorState) = TalkTargetChip(
    ChannelCard(id = title, kind = CardKind.MEMBER, title = title,
        session = previewSession(
            info = previewCallInfo(isMcptt = true, groupId = title),
            floor = previewFloor(state = floor))))

/** 발언 중인 발언 바 — 대상 둘(하나 승인·하나 대기). 실제로 가장 자주 보는 상태다. */
@Composable
private fun TalkingBar() = TalkBarContent(
    targets = listOf(chip("순찰1", FloorState.SPEAKING), chip("상황실", FloorState.QUEUED)))

/** 발언 대상이 없을 때의 발언 바 — 왜 못 누르는지 적혀 있다. */
@Composable
private fun IdleBar() = TalkBarContent(targets = emptyList(), anyJoined = true)

private val MINE = listOf(
    ChannelRowUi("g1", "1. 순찰1", "발언 김관제 00:14", "12:31", 7, 3,
        active = true, speaking = true, canTarget = true, targeted = true),
    ChannelRowUi("g2", "2. 상황실", "발언 없음", "05:02", 3, active = true, canTarget = true),
    ChannelRowUi("g3", "3. 교통1", "멤버 12", "대기"),
    ChannelRowUi("p1", "4. 김반장", "발언 없음", "02:14", active = true, canTarget = true),
    ChannelRowUi("a1", "5. 애드혹 3인", "발언 없음", "00:48", 3, active = true, canTarget = true),
)

private val SCOPED = listOf(
    ChannelRowUi("s1", "야간순찰", "청취 중 · 발언 박현장", "03:20", 5,
        active = true, speaking = true, listening = true),
    ChannelRowUi("s2", "정비반", "세션 진행 중 · 참가 2", "진행 중", 2, active = true, listening = false),
    ChannelRowUi("s3", "외곽경비", "마지막 세션", "대기", listening = false),
    ChannelRowUi("s4", "타인 개별 통화 · 이당직", "세션 진행 중 · 참가 2", "진행 중", 2,
        active = true, listening = false),
)

/**
 * 미리보기 한 장 — 껍데기 + 본문.
 *
 * 면([pttPane]·[callPane])까지 받는 이유: 껍데기의 쪽 번호가 «메뉴 + 면» 좌표라(§6.3), 면을 안 주면
 * 항상 그 메뉴의 첫 면 자리에 선다. 그러면 스와이프 이음매를 미리보기로 볼 수 없다.
 */
@Composable
private fun Screen(
    screen: AppScreen,
    bar: @Composable () -> Unit = { TalkingBar() },
    pttPane: PttPane = PttPane.CHANNELS,
    callPane: CallPane = CallPane.CALLS,
    body: @Composable ColumnScope.() -> Unit,
) = PreviewFrame {
    AppShellContent(screen = screen, top = TOP, badges = BADGES, talkBar = bar,
        pttPane = pttPane, callPane = callPane) { body() }
}

// ── 다섯 자리를 실제 크기로 ─────────────────────────────────────────────────

private const val T0 = 1_790_000_000_000L

/** 면이 VM·MediaPlayer 를 붙드는 조각은 자리만 표시한다 — 그 조각은 제 Preview 파일에 따로 있다. */
@Composable
private fun Pane(text: String) =
    Box(Modifier.fillMaxSize().padding(16.dp), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }

// ── 이력 ──
private fun hcall(i: Int, from: String, to: String, sec: Int, state: String = "answered") =
    HistoryEntry(id = "c$i", atMs = T0 - i * 900_000L, kind = HistoryKind.CALL, from = from, to = to,
        durationSec = sec, state = state, callType = "voip",
        inviteAtMs = T0 - i * 900_000L - 40_000,
        answerAtMs = if (state == "answered") T0 - i * 900_000L - 30_000 else null,
        endAtMs = T0 - i * 900_000L)

private val HCALLS = listOf(
    hcall(1, "01055551111", "1001", 143), hcall(2, "01055552222", "7000", 0, "missed"),
    hcall(3, "1001", "0212223333", 420), hcall(4, "01055554444", "1002", 62),
    hcall(5, "01055555555", "7000", 0, "rejected"), hcall(6, "1003", "01055556666", 215),
    hcall(7, "01055557777", "1001", 38), hcall(8, "01055558888", "7000", 91),
    hcall(9, "1002", "0311234567", 17), hcall(10, "01055550000", "1004", 0, "missed"))

private val HPTT = (1..5).map { i ->
    HistoryEntry(id = "p$i", atMs = T0 - i * 1_800_000L, kind = HistoryKind.PTT,
        group = "g00$i", groupName = listOf("순찰1", "상황실", "교통1", "야간순찰", "정비반")[i - 1],
        memberCount = 6 + i, turnCount = i * 7, speakerCount = 3, durationSec = i * 180,
        emergency = i == 2, hasRecording = true, recordingId = "pr$i",
        startAtMs = T0 - i * 1_800_000L - i * 180_000L)
}

private val BAND = intArrayOf(0, 0, 0, 0, 0, 0, 1, 3, 12, 18, 22, 15,
                              9, 14, 20, 24, 19, 11, 6, 3, 2, 1, 0, 0)

// ── 무전 메시지·이벤트 ──
private fun msg(i: Int, text: String, out: Boolean = false, from: String = "김관제") =
    Message(id = "m$i", groupId = "g1", fromUri = "sip:100$i@cims", fromName = from,
        text = text, atMs = T0 + i * 60_000L, outgoing = out)

private val THREAD = listOf(
    msg(1, "현장 도착했습니다"), msg(2, "3번 게이트 확인 바랍니다", out = true),
    msg(3, "확인했습니다. 이상 없습니다", from = "박현장"), msg(4, "수고하셨습니다", out = true),
    msg(5, "순찰 2조 교대 요청합니다 — 인원 2명 부족합니다", from = "이당직"),
    msg(6, "확인 중", out = true))

private val CHIPS = listOf(
    ThreadChip("g1", "순찰1", 0, T0), ThreadChip("g2", "상황실", 3, T0),
    ThreadChip("+821012345678", "박현장", 1, T0), ThreadChip("g3", "교통1", 0, T0))

private val EVENTS = listOf(
    ActivityRow(T0 - 30_000, "g1", "순찰1", "발언 김관제", ActivityKind.TALK),
    ActivityRow(T0 - 60_000, "g1", "순찰1", "입장 박현장", ActivityKind.JOIN),
    ActivityRow(T0 - 90_000, "g2", "상황실", "긴급 모드 시작", ActivityKind.EMERGENCY, emergency = true),
    ActivityRow(T0 - 120_000, "g1", "순찰1", "SDS 수신 — 이당직", ActivityKind.SDS),
    ActivityRow(T0 - 150_000, "g3", "교통1", "퇴장 최순찰", ActivityKind.LEAVE),
    ActivityRow(T0 - 180_000, "g1", "순찰1", "발언 거부 — 우선순위 낮음", ActivityKind.ERROR),
    ActivityRow(T0 - 210_000, "s1", "야간순찰", "발언 이당직", ActivityKind.TALK))

// ── 통화 ──
private fun dlg(id: String, state: String, remote: String, confirmed: Boolean) = DialogRow(
    watched = "1001", id = id,
    info = DialogInfo(accountId = 0, watched = "1001", id = id, callId = "c-$id",
        localTag = "l", remoteTag = "r", direction = "recipient", state = state,
        remoteIdentity = "sip:$remote@cims", full = true),
    startedAtMs = T0 - 180_000, confirmedAtMs = if (confirmed) T0 - 150_000 else null,
    wasConfirmed = confirmed)

/**
 * 감청 leg — 청취 중인 행 안에서 펴진다.
 *
 * CMP 가 양 peer 를 SSRC 2개로 갈라 인도하므로(RFC 5576 `a=ssrc … label`) 줄이 둘이다 — 믹싱은 단말이
 * 한다(dispatch_center.md §5.4 «CC 분리 인도·귀속 보존»).
 */
private val TAP = previewSession(callId = 91,
    info = previewCallInfo(callId = 91, listen = true, joinedDialog = "c-d1",
        sources = listOf(
            MediaSource(ssrc = 0x11223344, label = "caller", active = true, level = 0f),
            MediaSource(ssrc = 0x55667788, label = "callee", active = false, level = 0f))))

/** 진행 중 — 감청을 켜고 끄는 자리. 하나는 이미 «청취 중» 이라 상세가 펴져 있다. */
private val LIVE = listOf(
    LiveCallRow(legs = listOf(dlg("d1", "confirmed", "01055551111", true)),
        aLabel = "이당직", bLabel = "김민원", viaPilot = false, mine = false,
        monitoring = true, inScope = true, tap = TAP),
    LiveCallRow(legs = listOf(dlg("d2", "confirmed", "01055552222", true)),
        aLabel = "박현장", bLabel = "이시민", viaPilot = true, mine = false,
        monitoring = false, inScope = true),
    LiveCallRow(legs = listOf(dlg("d3", "early", "01055553333", false)),
        aLabel = "대표 7000", bLabel = "최시민", viaPilot = true, mine = false,
        monitoring = false, inScope = true))

private val CUI = CallsUi(
    live = LIVE,
    dialNumber = "1002",
    members = listOf(
        MemberChip("1001", "1001", "김관제", true), MemberChip("1002", "1002", "이당직", false),
        MemberChip("1003", "1003", "박현장", false), MemberChip("1004", "1004", "최순찰", false),
        MemberChip("1005", "1005", "정정비", false)),
    tally = DeskTally(answered = 12, missed = 3, outgoing = 8, transfer = 2, monitor = 1),
    log = (1..10).map { i ->
        CallLogRow(atMs = T0 - i * 600_000L, peer = listOf("김민원", "이시민", "본사 총무", "박민원",
            "최시민", "정민원", "한시민", "오민원", "서시민", "남민원")[i - 1],
            text = "", kind = CallLogKind.entries[i % CallLogKind.entries.size],
            number = "0105555$i", startedAtMs = T0 - i * 600_000L - 60_000,
            answeredAtMs = if (i % 3 == 0) null else T0 - i * 600_000L - 40_000,
            viaPilot = i % 4 == 0)
    },
    liveHint = "감시 대상 5회선 · 구독 성립 5")

// ── PTT 그룹 · 관리 ──
private val GROUPS = listOf(
    ManagedGroup("g001", "tel:g001", "순찰1", 12, isOwner = true, orgCode = "OPS", isMember = true),
    ManagedGroup("g002", "tel:g002", "상황실", 8, orgCode = "OPS", isMember = true),
    ManagedGroup("g003", "tel:g003", "교통1", 15, orgCode = "OPS", canManage = false, inListenScope = true),
    ManagedGroup("g004", "tel:g004", "야간순찰", 9, orgCode = "FLD", canManage = false, inListenScope = true),
    ManagedGroup("g005", "tel:g005", "정비반", 6, isOwner = true, orgCode = "MNT"))

private val GMEMBERS = listOf(
    DetailMember("김관제", "1001", DetailMember.SPEAKING, isMe = true, isChair = true),
    DetailMember("이당직", "1002", DetailMember.JOINED),
    DetailMember("박현장", "1003", DetailMember.JOINED),
    DetailMember("최순찰", "1004", DetailMember.JOINED),
    DetailMember("정정비", "1005", DetailMember.ABSENT),
    DetailMember("한지원", "1006", DetailMember.ABSENT),
    DetailMember("오경비", "1007", DetailMember.ABSENT))

private val AMEMBERS = listOf(
    MemberInfo(1, "김관제", "u1", "OPS1", "관제사", volte = NumberInfo(msisdn = "1001")),
    MemberInfo(2, "이당직", "u2", "OPS1", "당직", volte = NumberInfo(msisdn = "1002")),
    MemberInfo(3, "박현장", "u3", "FLD", "반장", ptt = NumberInfo(msisdn = "5003")),
    MemberInfo(4, "최순찰", "u4", "FLD", "대원", ptt = NumberInfo(msisdn = "5004")),
    MemberInfo(5, "정정비", "u5", "MNT", "기사", volte = NumberInfo(msisdn = "1005")),
    MemberInfo(6, "한지원", "u6", "OPS2", "관제사", volte = NumberInfo(msisdn = "1006")),
    MemberInfo(7, "오경비", "u7", "FLD", "대원", ptt = NumberInfo(msisdn = "5007")),
    MemberInfo(8, "남교통", "u8", "FLD", "대원", ptt = NumberInfo(msisdn = "5008")))

private val AVIEW = AdminView(
    scope = AdminScope(groupId = "pg-ops", directoryWrite = "all", orgCode = "HQ"),
    orgs = listOf(OrgNode("HQ", "본부"), OrgNode("OPS", "관제과", "HQ"), OrgNode("OPS1", "1팀", "OPS"),
        OrgNode("OPS2", "2팀", "OPS"), OrgNode("FLD", "현장과", "HQ"), OrgNode("MNT", "정비과", "HQ")),
    members = AMEMBERS)

// ── 메뉴 넷 × 그 안의 모든 면 ─────────────────────────────────────────────
//   기기에서 실제로 볼 수 있는 화면을 **빠짐없이** 같은 크기로 늘어놓는다.

// [이력] — 통화 표 / PTT 세션 두 종류
@Preview(name = "1 이력 — 통화 표 (첫 화면)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceHistoryCalls() = Screen(AppScreen.HISTORY, bar = { IdleBar() }) {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.CALL, rows = HCALLS, band = BAND),
        modifier = Modifier.weight(1f))
}

@Preview(name = "1 이력 — PTT 세션", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceHistoryPtt() = Screen(AppScreen.HISTORY, bar = { IdleBar() }) {
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = HPTT, band = BAND, selected = HPTT.first()),
        sessionPane = { Pane("세션 상세 — 참여자·발언 타임라인") },
        modifier = Modifier.weight(1f))
}

// [무전] — 채널 / 채널 상세 / 메시지 / 이벤트
@Preview(name = "2 무전 — 채널", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttChannels() = Screen(AppScreen.PTT) {
    PttTabs(pane = PttPane.CHANNELS, onPane = {}, unread = 3, modifier = Modifier.weight(1f)) {
        PttScreenContent(mine = MINE, scoped = SCOPED, filter = ScopeFilter.ALL, query = "",
            listenText = "동시 청취 1/4", listenFull = false)
    }
}

@Preview(name = "2 무전 — 채널 상세", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttChannel() = Screen(AppScreen.PTT) {
    PttTabs(pane = PttPane.CHANNELS, onPane = {}, unread = 3, modifier = Modifier.weight(1f)) {
        ChannelScreenContent(
            head = ChannelHeadUi(id = "g1", title = "순찰1", badge = "멤버",
                subtitle = "참가 7 · 발언 김관제 00:14 · 12:31",
                joined = true, isMemberGroup = true, canTarget = true, targeted = true, unread = 3,
                groupId = "g1"),
            roster = (1..7).map { RosterEntry("sip:100$it@cims", "connected") },
            speaker = "1001", me = "1002", nameOf = { if (it == "1001") "김관제" else "" })
    }
}

@Preview(name = "2 무전 — 메시지", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttMessages() = Screen(AppScreen.PTT, pttPane = PttPane.MESSAGES) {
    PttTabs(pane = PttPane.MESSAGES, onPane = {}, unread = 3, modifier = Modifier.weight(1f)) {
        Box(Modifier.fillMaxSize().padding(8.dp)) {
            MessagesContent(thread = THREAD, title = "순찰1", follow = true, groupId = "g1",
                threads = CHIPS, isGroup = true)
        }
    }
}

/** 사람 스레드 — 머리가 «1:1», 입력칸이 «이 사람에게», 말풍선에 보낸 사람 이름이 없다. */
@Preview(name = "2 무전 — 메시지(1:1)", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttDirectMessage() = Screen(AppScreen.PTT, pttPane = PttPane.MESSAGES) {
    PttTabs(pane = PttPane.MESSAGES, onPane = {}, unread = 0, modifier = Modifier.weight(1f)) {
        Box(Modifier.fillMaxSize()) {
            MessagesContent(thread = DIRECT_THREAD, title = "박현장", follow = false,
                groupId = "+821012345678", threads = CHIPS, isGroup = false)
        }
    }
}

@Preview(name = "2 무전 — 이벤트", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttEvents() = Screen(AppScreen.PTT, pttPane = PttPane.EVENTS) {
    PttTabs(pane = PttPane.EVENTS, onPane = {}, unread = 3, modifier = Modifier.weight(1f)) {
        Box(Modifier.fillMaxSize().padding(8.dp)) {
            ActivityContent(rows = EVENTS, filter = ActivityFilter.ALL, follow = false)
        }
    }
}

// [통화] — 통화(키패드·진행 중) / 주소록 / 메시지 / 통화내역
/**
 * «진행 중» 구역이 **감청의 유일한 자리**다 — 행의 [청취] 로 켜고, 켜지면 그 행이 펴져 소스 귀속을
 * 보이고, [청취 종료] 로 끈다. 별도의 «감청» 면은 두지 않는다(§6.5).
 */
@Preview(name = "3 통화 — 통화(키패드·진행 중·감청)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCalls() = Screen(AppScreen.CALLS, bar = { IdleBar() }) {
    CallsScreenContent(ui = CUI, pane = CallPane.CALLS, modifier = Modifier.weight(1f))
}

@Preview(name = "3 통화 — 주소록", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsBook() = Screen(AppScreen.CALLS, bar = { IdleBar() }, callPane = CallPane.BOOK) {
    CallsScreenContent(ui = CUI, pane = CallPane.BOOK,
        bookPane = { Pane("주소록 — 조직 범위·검색·이름 목록 (행 탭 = 사람 메뉴, 오른쪽 📞 = 바로 발신)") },
        modifier = Modifier.weight(1f))
}

/** 문자(SMS) — 왼쪽 상대 목록, 오른쪽 대화. 휴대폰 문자와 같은 구성(§6.2e). */
@Preview(name = "3 통화 — 메시지(문자)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsSms() = Screen(AppScreen.CALLS, bar = { IdleBar() }, callPane = CallPane.MESSAGES) {
    CallsScreenContent(ui = CUI, pane = CallPane.MESSAGES,
        smsPane = {
            SmsPaneContent(threads = SMS_CHIPS, thread = SMS_THREAD,
                peer = "1002", title = "이당직 · 1002")
        },
        modifier = Modifier.weight(1f))
}

/** 주소록에 없는 외부망 번호 — 그 스레드에서만 보내기가 막힌다(이유를 적어 준다). */
@Preview(name = "3 통화 — 메시지(외부망)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsSmsExternal() = Screen(AppScreen.CALLS, bar = { IdleBar() }, callPane = CallPane.MESSAGES) {
    CallsScreenContent(ui = CUI, pane = CallPane.MESSAGES,
        smsPane = {
            SmsPaneContent(threads = SMS_CHIPS, thread = SMS_EXTERNAL,
                peer = "01055551111", title = "01055551111", external = true)
        },
        modifier = Modifier.weight(1f))
}

@Preview(name = "3 통화 — 통화내역", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsLog() = Screen(AppScreen.CALLS, bar = { IdleBar() }, callPane = CallPane.LOG) {
    CallsScreenContent(ui = CUI, pane = CallPane.LOG, modifier = Modifier.weight(1f))
}

// [더보기] — 목록 / PTT 그룹 / 관리
@Preview(name = "4 더보기 — 목록", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceMore() = Screen(AppScreen.MORE, bar = { IdleBar() }) {
    MoreScreen(onOpen = {}, onSettings = {}, dirty = true, modifier = Modifier.weight(1f))
}

@Preview(name = "4 더보기 — PTT 그룹", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceGroups() = Screen(AppScreen.MORE, bar = { IdleBar() }) {
    PttGroupsScreenContent(GroupsUi(rows = GROUPS, selected = GROUPS[0], detail = GMEMBERS),
        modifier = Modifier.weight(1f))
}

@Preview(name = "4 더보기 — 관리", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceAdmin() = Screen(AppScreen.MORE, bar = { IdleBar() }) {
    AdminScreenContent(AdminUi(view = AVIEW, members = AMEMBERS, org = "OPS"),
        modifier = Modifier.weight(1f))
}

@Preview(name = "5 어두운 테마 — 무전", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceDark() = PreviewFrame(dark = true) {
    AppShellContent(screen = AppScreen.PTT, top = TOP, badges = BADGES, talkBar = { TalkingBar() },
        pttPane = PttPane.CHANNELS) {
        PttTabs(pane = PttPane.CHANNELS, onPane = {}, unread = 3, modifier = Modifier.weight(1f)) {
            PttScreenContent(mine = MINE, scoped = SCOPED, filter = ScopeFilter.ALL, query = "",
                listenText = "동시 청취 1/4", listenFull = false)
        }
    }
}

// ── 문자(SMS) ──
private fun sms(n: Int, text: String, peer: String = "1002", name: String = "이당직",
                out: Boolean = false) = Message(
    id = "sms$n", groupId = peer,
    fromUri = if (out) "" else "sip:$peer@cims", fromName = if (out) "나" else name,
    text = text, atMs = T0 - (9 - n) * 60_000L, outgoing = out,
    state = if (out) SendState.SENT else SendState.SENT, kind = MessageKind.SMS)

private val SMS_CHIPS = listOf(
    ThreadChip("1002", "이당직", 0, T0 - 60_000),
    ThreadChip("1003", "박현장", 2, T0 - 600_000),
    ThreadChip("01055551111", "01055551111", 0, T0 - 3_600_000))

private val SMS_THREAD = listOf(
    sms(1, "3번 게이트 순찰 나갑니다"),
    sms(2, "확인했습니다", out = true),
    sms(3, "교대 인원 2명 부족합니다"),
    sms(4, "지원 요청해 두겠습니다. 30분 뒤 도착 예정입니다", out = true))

private val SMS_EXTERNAL = listOf(
    Message(id = "x1", groupId = "01055551111", fromUri = "sip:01055551111@cims",
        fromName = "01055551111", text = "민원 접수 확인 부탁드립니다",
        atMs = T0 - 3_600_000, outgoing = false, kind = MessageKind.SMS))

/** 1:1 무전 메시지 — 그룹과 달리 상대가 하나라 말풍선에 이름을 적지 않는다. */
private val DIRECT_THREAD = listOf(
    msg(1, "3번 게이트 도착했습니다", from = "박현장"),
    msg(2, "확인", out = true),
    msg(3, "이상 없습니다", from = "박현장"))
