// **실제 태블릿 한 장** Preview — 왼쪽 레일 + 상단 바 + 관제 탭 줄 + 본문(면·사이드 패널) + 발언 바 (android_dispatch_tablet.md §6.3)
//
// 화면별 Preview(`*ScreenPreview.kt`)는 **본문만** 그린다 — 그 영역의 밀도를 볼 때 쓴다. 이 파일은 껍데기(레일 80 · 상단 바 64 ·
// 탭 줄 48 · 발언 바 80)까지 붙여 **기기에서 보이는 그대로** 를 보여 준다. 본문만 보면 실제보다 넉넉해 보인다.
//
// 크기는 가로 1280×800dp — 관제 태블릿 실물(§12). 차례는 시안 «관제 메뉴 재구성»(E1~E6)과 같다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.width
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
import com.cims.ue.dispatch.ui.ptt.EventPanel
import com.cims.ue.dispatch.ui.ptt.MessagesContent
import com.cims.ue.dispatch.ui.ptt.ThreadChip
import com.cims.ue.sdk.MediaSource
import com.cims.ue.sdk.RosterEntry
import com.cims.ue.dispatch.session.DeskTally
import com.cims.ue.dispatch.session.DialogRow
import com.cims.ue.dispatch.ui.call.LiveCallRow
import com.cims.ue.sdk.DialogInfo
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.ui.call.BookPanelContent
import com.cims.ue.dispatch.ui.call.CallStatusContent
import com.cims.ue.dispatch.ui.call.CallStatusWidth
import com.cims.ue.dispatch.ui.call.CallsScreenContent
import com.cims.ue.dispatch.session.MessageKind
import com.cims.ue.dispatch.session.SendState
import com.cims.ue.dispatch.ui.call.SmsPaneContent
import com.cims.ue.dispatch.ui.call.CallsUi
import com.cims.ue.dispatch.ui.history.HistoryScreenContent
import com.cims.ue.dispatch.ui.history.HistoryUi
import com.cims.ue.dispatch.ui.ptt.ChannelHeadUi
import com.cims.ue.dispatch.ui.ptt.ChannelRowUi
import com.cims.ue.dispatch.ui.ptt.ChannelPanelContent
import com.cims.ue.dispatch.ui.ptt.ChannelsPaneContent
import com.cims.ue.dispatch.ui.ptt.MineCardUi
import com.cims.ue.dispatch.ui.ptt.CardControl
import com.cims.ue.dispatch.ui.ptt.PersonRowUi
import com.cims.ue.dispatch.ui.ptt.UserRowUi
import com.cims.ue.dispatch.ui.ptt.AddChannelPanelContent
import com.cims.ue.dispatch.ui.groups.NewGroupPanelContent
import com.cims.ue.dispatch.ui.groups.EditForm
import com.cims.ue.dispatch.ui.groups.MemberRow
import com.cims.ue.dispatch.ui.ptt.ScopeFilter
import com.cims.ue.dispatch.ui.ptt.TalkBarContent
import com.cims.ue.dispatch.ui.ptt.TalkTargetChip
import com.cims.ue.dispatch.ui.ptt.CardKind
import com.cims.ue.dispatch.ui.ptt.ChannelCard
import com.cims.ue.sdk.FloorState
import androidx.compose.ui.Modifier
import androidx.compose.material.icons.filled.Contacts

private val TOP = TopBarUi(
    displayName = "김관제",
    deskLine = "관제1과 · 대표 7000",
    registrations = listOf(RegDot.ON, RegDot.PENDING))

private val BADGES = NavBadges(unread = 3, adminDirty = true)

/** 발언 대상 칩 하나 — floor 상태가 «승인/대기/요청» 표시를 정한다. */
private fun chip(title: String, floor: FloorState) = TalkTargetChip(
    ChannelCard(id = title, kind = CardKind.MEMBER, title = title,
        session = previewSession(
            info = previewCallInfo(isMcptt = true, groupId = title),
            floor = previewFloor(state = floor),
            speaker = if (floor == FloorState.SPEAKING) "나" else "").copy(grantedSec = 30)))

/** 발언 중인 발언 바 — 대상 둘(하나 승인·하나 대기). 실제로 가장 자주 보는 상태다. */
@Composable
private fun TalkingBar() = TalkBarContent(
    targets = listOf(chip("순찰1", FloorState.SPEAKING), chip("상황실", FloorState.QUEUED)))

/** 발언 대상이 없을 때의 발언 바 — 왜 못 누르는지 적혀 있다. */
@Composable
private fun IdleBar() = TalkBarContent(targets = emptyList(), anyJoined = true)

private val SCOPED = listOf(
    ChannelRowUi("s1", "야간순찰", "청취 중 · 발언 박현장", "03:20", 5,
        active = true, speaking = true, listening = true),
    ChannelRowUi("s2", "정비반", "세션 진행 중 · 참가 2", "진행 중", 2, active = true, listening = false),
    ChannelRowUi("s3", "외곽경비", "마지막 세션", "대기", listening = false),
    ChannelRowUi("s4", "타인 개별 통화 · 이당직", "세션 진행 중 · 참가 2", "진행 중", 2,
        active = true, listening = false),
)

/**
 * 미리보기 한 장 — 껍데기 + 본문. [관제] 면이면 [page] 가 탭 줄의 강조와 pager 의 자리를, [panel] 이 오른쪽 사이드 패널을 정한다.
 * [통화] 면이면 왼쪽 고정 칸([CallStatusContent])을 셸이 얹는다 — 앱과 같은 방식(면은 그 폭을 비운다).
 */
@Composable
private fun Screen(
    screen: AppScreen = AppScreen.DISPATCH,
    page: DispatchPage = pageOf(PttPane.CHANNELS),
    bar: @Composable () -> Unit = { TalkingBar() },
    panel: (@Composable () -> Unit)? = null,
    dark: Boolean = false,
    body: @Composable () -> Unit,
) = PreviewFrame(dark = dark) {
    AppShellContent(screen = screen, top = TOP, badges = BADGES, talkBar = bar,
        tabs = {
            DispatchTabs(page, onMode = {}, onPage = {}, badges = BADGES) {
                if (page.mode == DispatchMode.CALL) ListToggle("주소록", open = false, onClick = {},
                    leading = androidx.compose.material.icons.Icons.Filled.Contacts)
                else ListToggle("사용자", open = false, onClick = {})
            }
        }) {
        val fixed = FixedColumn(DispatchMode.CALL, CallStatusWidth + 1.dp) {
            Row(Modifier.fillMaxSize()) { CallStatusContent(ui = CUI, modifier = Modifier.weight(1f)); VDivider() }
        }
        if (screen == AppScreen.DISPATCH) DispatchBody(page, onPage = {}, panel = panel, fixed = fixed) { p ->
            if (p == page) {
                if (p.mode == DispatchMode.CALL) Row(Modifier.fillMaxSize()) {
                    Spacer(Modifier.width(CallStatusWidth + 1.dp)); Box(Modifier.weight(1f)) { body() }
                } else body()
            }
        }
        else body()
    }
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
    msg(5, "", from = "이순경").copy(id = "m5f", fileName = "현장사진_01.jpg", fileUrl = "https://csc/mcdata/fd/0", fileSize = 1_258_291),
    msg(6, "확인 중", out = true))

private val CHIPS = listOf(
    ThreadChip("g1", "순찰1", 0, T0 + 360_000, last = "나: 확인 중", group = true),
    ThreadChip("g2", "상황실", 3, T0 - 400_000, last = "이당직: 확인 후 보고 드리겠습니다", group = true),
    ThreadChip("+821012345678", "박현장", 1, T0 - 900_000, last = "교대 인원 2명 부족합니다"),
    ThreadChip("g3", "교통1", 0, T0 - 3_600_000, last = "최순찰: 교차로 정체 해소", group = true))

private val EVENTS = listOf(
    ActivityRow(T0 - 30_000, "g1", "순찰1", "박현장 발언 종료 · 14초", ActivityKind.TALK, id = 11),
    ActivityRow(T0 - 44_000, "g1", "순찰1", "박현장 발언 시작", ActivityKind.TALK, id = 10),
    ActivityRow(T0 - 60_000, "g1", "순찰1", "박현장 입장", ActivityKind.JOIN, id = 9),
    ActivityRow(T0 - 90_000, "g2", "상황실", "긴급 개시 · 이당직", ActivityKind.EMERGENCY, emergency = true, id = 8),
    ActivityRow(T0 - 120_000, "g1", "순찰1", "메시지 · 이당직: 순찰 2조 교대 요청합니다", ActivityKind.SDS, id = 7),
    ActivityRow(T0 - 150_000, "g1", "순찰1", "최순찰 퇴장", ActivityKind.LEAVE, id = 6),
    ActivityRow(T0 - 180_000, "g1", "순찰1", "발언 거부 — 우선순위 낮음", ActivityKind.ERROR, id = 5),
    ActivityRow(T0 - 210_000, "s1", "야간순찰", "이당직 발언 · 청취 중", ActivityKind.TALK, id = 4),
    ActivityRow(T0 - 240_000, "g1", "순찰1", "김관제(나) 발언 · 22초", ActivityKind.TALK, id = 3),
    ActivityRow(T0 - 270_000, "g2", "상황실", "메시지 · 서상황: 3번 출입구 CCTV 확인 요청", ActivityKind.SDS, id = 2),
    ActivityRow(T0 - 300_000, "g1", "순찰1", "한지원 입장", ActivityKind.JOIN, id = 1))

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
    )

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

// ── 메뉴 셋 × 관제의 면 × 사이드 패널 ────────────────────────────────────────
//   기기에서 실제로 볼 수 있는 화면을 **빠짐없이** 같은 크기로 늘어놓는다(시안 «관제 메뉴 재구성» E1~E6 과 같은 차례).

private val MINE_CARDS = listOf(
    MineCardUi("g1", "1. 순찰1", "순찰1", sub = "발언 나 00:14", subTone = com.cims.ue.dispatch.ui.ptt.SubTone.ME,
        roster = "김관제(나) · 이당직 · 박현장 +4", meta = "참가 7 · 12:31",
        speaking = true, active = true, joined = true, control = CardControl.TARGET, on = true),
    MineCardUi("g2", "2. 상황실", "상황실", sub = "발언 없음", roster = "이당직 · 서상황", meta = "참가 3 · 05:02", unread = 3,
        active = true, joined = true, control = CardControl.TARGET),
    MineCardUi("g3", "3. 교통1", "교통1", sub = "대기 · 멤버 12", roster = "미참여", control = CardControl.JOIN),
    MineCardUi("p1", "4. 김반장", "김반장", sub = "발언 없음", kindTag = "개별", roster = "김반장", meta = "02:14", active = true,
        joined = true, control = CardControl.MUTE),
    MineCardUi("a1", "5. 박현장, 최순찰", "애드혹", sub = "발언 요청 거부 · 대기열 가득", subTone = com.cims.ue.dispatch.ui.ptt.SubTone.WARN,
        kindTag = "애드혹", broadcast = true, roster = "박현장 · 최순찰", meta = "참가 3 · 00:48",
        active = true, joined = true, control = CardControl.TARGET),
)

private val PEOPLE = listOf(
    PersonRowUi("5001", "김관제", "PTT 5001 · 관제과 1팀", isMe = true, speaking = true, chair = true),
    PersonRowUi("5002", "이당직", "PTT 5002 · 관제과 1팀"),
    PersonRowUi("5003", "박현장", "PTT 5003 · 현장과"),
    PersonRowUi("5004", "최순찰", "PTT 5004 · 현장과"),
    PersonRowUi("5005", "정정비", "PTT 5005 · 정비과"),
    PersonRowUi("5006", "한지원", "PTT 5006 · 관제과 2팀"))

private val HEAD = ChannelHeadUi(id = "g1", title = "순찰1", badge = "멤버",
    subtitle = "참가 7 · 발언 김관제 00:14 · 12:31",
    joined = true, isMemberGroup = true, canTarget = true, targeted = true, unread = 3, groupId = "g1", canEdit = true)

private val USERS = listOf(
    UserRowUi("5002", "이당직", "관제과 1팀", "PTT 5002 · 관제과 1팀", "접속"),
    UserRowUi("5003", "박현장", "현장과", "PTT 5003 · 현장과", "접속"),
    UserRowUi("5004", "최순찰", "현장과", "PTT 5004 · 현장과", "접속"),
    UserRowUi("5006", "한지원", "관제과 2팀", "PTT 5006 · 관제과 2팀"),
    UserRowUi("5007", "오경비", "현장과", "PTT 5007 · 현장과"),
    UserRowUi("5008", "남교통", "현장과", "PTT 5008 · 현장과"),
    UserRowUi("5009", "서상황", "관제과 2팀", "PTT 5009 · 관제과 2팀", "접속"))

private val FORM = EditForm(isNew = true, uri = "tel:g008", groupId = "g008", name = "3번 게이트 대응",
    members = listOf(MemberRow("tel:5001", "김관제", "5001", isChair = true, isMe = true),
        MemberRow("tel:5002", "이당직", "5002"), MemberRow("tel:5003", "박현장", "5003"),
        MemberRow("tel:5004", "최순찰", "5004")), loaded = true)

@Preview(name = "1 관제 — 무전 › 채널 (패널 닫힘)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceChannels() = Screen {
    ChannelsPaneContent(mine = MINE_CARDS, other = SCOPED, listenText = "동시 청취 1/4")
}

@Preview(name = "1 관제 — 채널 상세 패널", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceChannelPanel() = Screen(panel = {
    ChannelPanelContent(head = HEAD, info = "멤버 그룹 · 참가 7 · 12:31 · 편성 12 · 발언 김관제", memberCount = 12,
        connected = PEOPLE, members = PEOPLE)
}) {
    ChannelsPaneContent(mine = MINE_CARDS.map { if (it.id == "g1") it.copy(selected = true) else it },
        other = SCOPED, selectedId = "g1", listenText = "동시 청취 1/4")
}

@Preview(name = "1 관제 — 채널 추가 패널", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceAddChannelPanel() = Screen(panel = {
    AddChannelPanelContent(rows = USERS, picked = listOf("5002", "5003", "5004"))
}) {
    ChannelsPaneContent(mine = MINE_CARDS, other = SCOPED, listenText = "동시 청취 1/4", addOpen = true)
}

@Preview(name = "1 관제 — 새 그룹 패널", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceNewGroupPanel() = Screen(panel = { NewGroupPanelContent(form = FORM) }) {
    ChannelsPaneContent(mine = MINE_CARDS, other = SCOPED, listenText = "동시 청취 1/4")
}

@Preview(name = "1 관제 — 무전 › 메시지", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttMessages() = Screen(page = pageOf(PttPane.MESSAGES)) {
    MessagesContent(thread = THREAD, title = "순찰1", follow = true, groupId = "g1",
        threads = CHIPS, isGroup = true, members = 12, online = 7)
}

@Preview(name = "1 관제 — 무전 › 이벤트", device = PreviewFull, showBackground = true)
@Composable
private fun DevicePttEvents() = Screen(page = pageOf(PttPane.EVENTS)) {
    ActivityContent(rows = EVENTS)
}

@Preview(name = "1 관제 — 이벤트 상세 패널", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceEventPanel() = Screen(page = pageOf(PttPane.EVENTS), panel = {
    EventPanel(row = EVENTS[1], all = EVENTS, onClose = {}, onOpenChannel = {}, onHistory = {}, onReply = {})
}) {
    ActivityContent(rows = EVENTS, selectedId = 10)
}

/** 긴급·임박 — 카드·타 채널 행이 같은 낱말·같은 색(빨강·주황)을 단다(§6.2a-1). 배너는 셸의 몫이라 여기엔 없다. */
@Preview(name = "1 관제 — 긴급·임박 상태", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceAlerts() = Screen {
    ChannelsPaneContent(
        mine = listOf(MINE_CARDS[0].copy(emergency = true, sub = "발언 박현장 00:03", subTone = com.cims.ue.dispatch.ui.ptt.SubTone.NORMAL),
            MINE_CARDS[1].copy(peril = true, sub = "발언 없음")) + MINE_CARDS.drop(2),
        other = SCOPED.mapIndexed { i, r -> if (i == 0) r.copy(emergency = true) else r },
        listenText = "동시 청취 1/4")
}

/**
 * «진행 중» 구역이 **감청의 유일한 자리**다 — 행의 [청취] 로 켜고, 켜지면 그 행이 펴져 소스 귀속을 보이고,
 * [청취 종료] 로 끈다. 별도의 «감청» 면은 두지 않는다(§6.5).
 */
@Preview(name = "1 관제 — 통화 › 통화(키패드·진행 중·감청)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCalls() = Screen(page = pageOf(CallPane.CALLS), bar = { IdleBar() }) {
    CallsScreenContent(ui = CUI, pane = CallPane.CALLS, showTabs = false, modifier = Modifier.fillMaxSize())
}

/** 주소록 — 어느 통화 면에서든 오른쪽에 편다. 행 오른쪽 [발신]·[문자], 행 탭 = 사람 메뉴(§6.2b). */
@Preview(name = "1 관제 — 통화 › 주소록 패널", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsBook() = Screen(page = pageOf(CallPane.CALLS), bar = { IdleBar() },
    panel = { BookPanelContent(book = CUI.book) }) {
    CallsScreenContent(ui = CUI, pane = CallPane.CALLS, showTabs = false, modifier = Modifier.fillMaxSize())
}

/** 문자(SMS) — 왼쪽 상대 목록, 오른쪽 대화. 휴대폰 문자와 같은 구성(§6.2e). */
@Preview(name = "1 관제 — 통화 › 메시지(문자)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsSms() = Screen(page = pageOf(CallPane.MESSAGES), bar = { IdleBar() }) {
    CallsScreenContent(ui = CUI, pane = CallPane.MESSAGES, showTabs = false,
        smsPane = {
            SmsPaneContent(threads = SMS_CHIPS, thread = SMS_THREAD,
                peer = "1002", title = "이당직 · 1002")
        },
        modifier = Modifier.fillMaxSize())
}

/** 주소록에 없는 외부망 번호 — 그 스레드에서만 보내기가 막힌다(이유를 적어 준다). */
@Preview(name = "1 관제 — 통화 › 메시지(외부망)", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsSmsExternal() = Screen(page = pageOf(CallPane.MESSAGES), bar = { IdleBar() }) {
    CallsScreenContent(ui = CUI, pane = CallPane.MESSAGES, showTabs = false,
        smsPane = {
            SmsPaneContent(threads = SMS_CHIPS, thread = SMS_EXTERNAL,
                peer = "01055551111", title = "01055551111", external = true)
        },
        modifier = Modifier.fillMaxSize())
}

@Preview(name = "1 관제 — 통화 › 통화내역", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceCallsLog() = Screen(page = pageOf(CallPane.LOG), bar = { IdleBar() }) {
    CallsScreenContent(ui = CUI, pane = CallPane.LOG, showTabs = false, modifier = Modifier.fillMaxSize())
}

// [이력] — 통화 표 / PTT 세션 두 종류
@Preview(name = "2 이력 — 통화 표", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceHistoryCalls() = Screen(AppScreen.HISTORY, bar = { IdleBar() }) {
    HistoryScreenContent(HistoryUi(kind = HistoryKind.CALL, rows = HCALLS, band = BAND),
        modifier = Modifier.fillMaxSize())
}

@Preview(name = "2 이력 — PTT 세션", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceHistoryPtt() = Screen(AppScreen.HISTORY, bar = { IdleBar() }) {
    HistoryScreenContent(
        HistoryUi(kind = HistoryKind.PTT, rows = HPTT, band = BAND, selected = HPTT.first()),
        modifier = Modifier.fillMaxSize())
}

// 레일 화면 — PTT 그룹 / 관리
@Preview(name = "3 PTT 그룹", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceGroups() = Screen(AppScreen.PTT_GROUPS, bar = { IdleBar() }) {
    PttGroupsScreenContent(GroupsUi(rows = GROUPS, selected = GROUPS[0], detail = GMEMBERS),
        modifier = Modifier.fillMaxSize())
}

@Preview(name = "3 관리", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceAdmin() = Screen(AppScreen.ADMIN, bar = { IdleBar() }) {
    AdminScreenContent(AdminUi(view = AVIEW, members = AMEMBERS, org = "OPS"),
        modifier = Modifier.fillMaxSize())
}

@Preview(name = "4 어두운 테마 — 채널 상세", device = PreviewFull, showBackground = true)
@Composable
private fun DeviceDark() = Screen(dark = true, panel = {
    ChannelPanelContent(head = HEAD, info = "멤버 그룹 · 참가 7 · 12:31 · 편성 12 · 발언 김관제", memberCount = 12,
        connected = PEOPLE, members = PEOPLE)
}) {
    ChannelsPaneContent(mine = MINE_CARDS.map { if (it.id == "g1") it.copy(selected = true) else it },
        other = SCOPED, selectedId = "g1", listenText = "동시 청취 1/4")
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

// ── 기기에서 띄워 보는 미리보기(debug 빌드의 `debug/PreviewActivity`) ───────────────────
//   데스크톱의 `--ui-preview-canvas` 에 해당한다 — 로그인·서버 없이 표본으로 화면을 그려 실제 기기의 밀도·색·손짓을 본다.
//   release APK 에는 진입점이 없다(Activity 가 debug 소스 셋에만 있다).

/** 이름 → 고정 한 장. `adb shell am start -n com.cims.ue.dispatch/.debug.PreviewActivity --es name <이름>`. */
internal val DEVICE_PREVIEWS: Map<String, @Composable () -> Unit> = linkedMapOf(
    "channels" to { DeviceChannels() },
    "channel-panel" to { DeviceChannelPanel() },
    "add-channel" to { DeviceAddChannelPanel() },
    "new-group" to { DeviceNewGroupPanel() },
    "messages" to { DevicePttMessages() },
    "events" to { DevicePttEvents() },
    "event-panel" to { DeviceEventPanel() },
    "alerts" to { DeviceAlerts() },
    "calls" to { DeviceCalls() },
    "book" to { DeviceCallsBook() },
    "sms" to { DeviceCallsSms() },
    "sms-external" to { DeviceCallsSmsExternal() },
    "log" to { DeviceCallsLog() },
    "history-calls" to { DeviceHistoryCalls() },
    "history-ptt" to { DeviceHistoryPtt() },
    "groups" to { DeviceGroups() },
    "admin" to { DeviceAdmin() },
    "dark" to { DeviceDark() },
    "video-panel" to { com.cims.ue.dispatch.ui.ptt.DeviceVideoPanel() },      // 채널 상세 «영상» 절(MCVideo — 영상 칸은 자리 표시)
    "video-banner" to { com.cims.ue.dispatch.ui.ptt.DeviceVideoBanner() },    // «새 영상» 배너 + 카드 «영상 n»
    // 화면별 미리보기 — 셸 없이 그 화면만(편집 폼처럼 VM 이 있어야 서는 상태를 기기에서 본다).
    "group-edit" to { com.cims.ue.dispatch.ui.groups.PreviewGroupEdit() },
    "group-new" to { com.cims.ue.dispatch.ui.groups.PreviewGroupNew() },
    "group-big" to { com.cims.ue.dispatch.ui.groups.PreviewGroupsBig() },
    "group-panel" to { com.cims.ue.dispatch.ui.groups.PreviewNewGroupPanelAdvanced() },
    "admin-edit" to { com.cims.ue.dispatch.ui.admin.PreviewAdminEditing() },
    "admin-lines" to { com.cims.ue.dispatch.ui.admin.PreviewAdminLines() },
    "admin-org" to { com.cims.ue.dispatch.ui.admin.PreviewAdminOrg() },
    "admin-new" to { com.cims.ue.dispatch.ui.admin.PreviewAdminNew() },
    "messages-files" to { com.cims.ue.dispatch.ui.ptt.PreviewMessages() },
    "hist-ptt" to { com.cims.ue.dispatch.ui.history.PreviewHistoryPtt() },
    "hist-ptt-dark" to { com.cims.ue.dispatch.ui.history.PreviewHistoryPttLinear() },
    "hist-bundles" to { com.cims.ue.dispatch.ui.history.PreviewHistoryBundles() },
    "hist-video" to { com.cims.ue.dispatch.ui.history.PreviewHistoryVideoSession() },
    "hist-call" to { com.cims.ue.dispatch.ui.history.PreviewHistoryCall() },
    "hist-video-call" to { com.cims.ue.dispatch.ui.history.PreviewHistoryVideoCall() },
)

/**
 * 움직이는 미리보기 — 표본 자료 위에서 **이동 규칙은 실제 것**([NavState] 의 순수 함수)을 쓴다: 레일·[무전|통화]·하위 탭·좌우 스와이프·
 * 오른쪽 패널(겹침·밀려 들어옴·폭 끌기·핀)·뒤로가기. 조작(참여·발언·발신)은 동작하지 않는다. 레일 [설정] = 테마 전환.
 *
 * @param banners `alerts`(긴급·임박) | `incoming`(착신) | `cert`(서버 인증서) | 빈 값
 */
@Composable
internal fun PreviewApp(dark: Boolean = false, start: NavState = NavState(), banners: String = "") {
    var nav by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(start) }
    var darkNow by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(dark) }
    var width by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(PanelWidth) }
    androidx.activity.compose.BackHandler(enabled = nav.onBack() != null) { nav.onBack()?.let { nav = it } }
    PreviewFrame(dark = darkNow) {
        AppShellContent(
            screen = nav.screen, top = TOP.copy(monitors = 1), badges = BADGES.copy(callWaiting = 1, smsUnread = 2),
            onSelect = { nav = nav.onNav(it) }, onSettings = { darkNow = !darkNow },
            panelOpen = nav.panel != null, panelWidth = width,
            talkBar = { if (nav.mode == DispatchMode.PTT) TalkingBar() else IdleBar() },
            banners = {
                when (banners) {
                    "alerts" -> EmergencyBannerContent(listOf(
                        AlertBannerUi("g2", com.cims.ue.dispatch.session.AlertKind.EMERGENCY, "상황실", "1006 박경장", T0, canCancel = true, callId = 2),
                        AlertBannerUi("g1", com.cims.ue.dispatch.session.AlertKind.IMMINENT_PERIL, "순찰1", "", T0, canCancel = true, callId = 1),
                        com.cims.ue.dispatch.session.EmergencyAlertBanner("g3", "1021", "교통1", "1021 박현장 (교통과)", T0).toAlertBannerUi(canCancel = true)),
                        onOpen = { nav = nav.openChannel(it) })
                    "incoming" -> {
                        BannerBar(BannerTone.PILOT, "대표번호 7000 착신", "010-2222-3333", sinceMs = System.currentTimeMillis()) {
                            PillButton("응답", {}, kind = Pill.CALL, height = 44.dp); PillButton("거절", {}, kind = Pill.LINE, height = 44.dp)
                        }
                        BannerBar(BannerTone.DIRECT, "착신", "1003 박현장", sinceMs = System.currentTimeMillis()) {
                            PillButton("응답", {}, kind = Pill.CALL, height = 44.dp); PillButton("거절", {}, kind = Pill.LINE, height = 44.dp)
                        }
                    }
                    "cert" -> {
                        WarnLine("서버 인증서 12일 후 만료", "121.161.164.45:15061 · CN=ctrl01 · 만료 2026-10-13 · 자동 갱신 실패 신호 — 운영자에게 알리세요")
                        WarnLine("서버 인증서 만료됨", "121.161.164.45:15061", critical = true)
                    }
                }
            },
            tabs = {
                DispatchTabs(nav.page, onMode = { nav = nav.toMode(it) }, onPage = { nav = nav.toPage(it) },
                    badges = BADGES.copy(callWaiting = 1, smsUnread = 2)) {
                    if (nav.mode == DispatchMode.CALL) ListToggle("주소록", open = nav.panel == SidePanel.Book,
                        onClick = { nav = nav.togglePanel(SidePanel.Book) }, leading = androidx.compose.material.icons.Icons.Filled.Contacts)
                    else ListToggle("사용자", open = nav.panel == SidePanel.AddChannel || nav.panel == SidePanel.NewGroup,
                        onClick = { nav = nav.togglePanel(SidePanel.AddChannel) })
                }
            },
            notices = { m -> if (banners == "toasts") NoticeStack(listOf(
                com.cims.ue.dispatch.session.Notice(1, com.cims.ue.dispatch.session.NoticeLevel.ERROR, "청취 권한이 없는 대상입니다", "403 Forbidden"),
                com.cims.ue.dispatch.session.Notice(2, com.cims.ue.dispatch.session.NoticeLevel.WARN, "동시 청취 상한 4"),
                com.cims.ue.dispatch.session.Notice(3, com.cims.ue.dispatch.session.NoticeLevel.INFO, "선호 이어폰으로 돌아왔습니다")),
                modifier = m) },
        ) {
            when (nav.screen) {
                AppScreen.DISPATCH -> DispatchBody(
                    page = nav.page, onPage = { nav = nav.toPage(it) },
                    panel = nav.panel?.let { pn -> { PreviewPanel(pn, nav.pinned, onPin = { nav = nav.togglePin() },
                        onClose = { nav = nav.closePanel() }, onBack = { nav.panel?.parent?.let { nav = nav.copy(panel = it) } },
                        onGroup = { nav = nav.showPanel(SidePanel.NewGroup) }) } },
                    fixed = FixedColumn(DispatchMode.CALL, CallStatusWidth + 1.dp) {
                        Row(Modifier.fillMaxSize()) { CallStatusContent(ui = CUI, modifier = Modifier.weight(1f)); VDivider() }
                    },
                    panelWidth = width, onPanelWidth = { width = it },
                ) { page ->
                    val sel = (nav.panel as? SidePanel.Channel)?.id
                    when {
                        page.pttPane == PttPane.CHANNELS -> ChannelsPaneContent(
                            mine = (if (banners == "alerts") listOf(MINE_CARDS[0].copy(peril = true), MINE_CARDS[1].copy(emergency = true)) +
                                MINE_CARDS.drop(2) else MINE_CARDS).map { it.copy(selected = it.id == sel) },
                            other = if (banners == "alerts") SCOPED.mapIndexed { i, r -> if (i == 1) r.copy(emergency = true) else r } else SCOPED,
                            selectedId = sel,
                            listenText = "동시 청취 1/4", onOpen = { id -> nav = nav.togglePanel(SidePanel.Channel(id)) },
                            addOpen = nav.panel == SidePanel.AddChannel || nav.panel == SidePanel.NewGroup,
                            onAdd = { nav = nav.togglePanel(SidePanel.AddChannel) })
                        page.pttPane == PttPane.MESSAGES -> MessagesContent(thread = THREAD, title = "순찰1", follow = true,
                            groupId = "g1", threads = CHIPS, isGroup = true, members = 12, online = 7,
                            onChannelInfo = { id -> nav = nav.showPanel(SidePanel.Channel(id)) })
                        page.pttPane == PttPane.EVENTS -> ActivityContent(rows = EVENTS,
                            pinned = listOf(AlertBannerUi("g2", com.cims.ue.dispatch.session.AlertKind.EMERGENCY, "상황실", "", T0)),
                            selectedId = (nav.panel as? SidePanel.Event)?.id,
                            onSelect = { r -> nav = nav.togglePanel(SidePanel.Event(r.id)) },
                            onOpenChannel = { id -> nav = nav.openChannel(id) }, onHistory = { nav = nav.onNav(AppScreen.HISTORY) })
                        else -> Row(Modifier.fillMaxSize()) {
                            Spacer(Modifier.width(CallStatusWidth + 1.dp))
                            CallsScreenContent(ui = CUI, pane = page.callPane ?: CallPane.CALLS, showTabs = false,
                                smsPane = { SmsPaneContent(threads = SMS_CHIPS, thread = SMS_THREAD, peer = "1002", title = "이당직 · 1002") },
                                modifier = Modifier.weight(1f))
                        }
                    }
                }
                AppScreen.HISTORY -> HistoryScreenContent(
                    HistoryUi(kind = HistoryKind.PTT, rows = HPTT, band = BAND, selected = HPTT.first()),
                    modifier = Modifier.fillMaxSize())
                AppScreen.PTT_GROUPS -> PttGroupsScreenContent(GroupsUi(rows = GROUPS, selected = GROUPS[0], detail = GMEMBERS),
                    modifier = Modifier.fillMaxSize())
                AppScreen.ADMIN -> AdminScreenContent(AdminUi(view = AVIEW, members = AMEMBERS, org = "OPS"),
                    modifier = Modifier.fillMaxSize())
            }
        }
    }
}

/** 움직이는 미리보기의 패널 — 표본 내용. */
@Composable
private fun PreviewPanel(panel: SidePanel, pinned: Boolean, onPin: () -> Unit, onClose: () -> Unit, onBack: () -> Unit,
                         onGroup: () -> Unit) {
    when (panel) {
        is SidePanel.Channel -> {
            val card = MINE_CARDS.firstOrNull { it.id == panel.id }
            val other = SCOPED.firstOrNull { it.id == panel.id }
            ChannelPanelContent(
                head = when {
                    card != null -> HEAD.copy(id = card.id, title = card.name, joined = card.control != CardControl.JOIN,
                        canTarget = card.control == CardControl.TARGET, targeted = card.on, groupId = card.id.takeIf { it.startsWith("g") },
                        isMemberGroup = card.id.startsWith("g"), canBroadcast = card.control == CardControl.JOIN)
                    other != null -> ChannelHeadUi(id = other.id, title = other.title, groupId = other.id, listening = other.listening)
                    else -> ChannelHeadUi(id = panel.id, title = panel.id, gone = true)
                },
                info = if (card != null) "멤버 그룹 · 참가 7 · 12:31 · 편성 12 · 발언 김관제" else "청취 범위 · 참가 5 · 편성 9",
                memberCount = 12, connected = PEOPLE, members = PEOPLE, pinned = pinned, onPin = onPin, onClose = onClose)
        }
        SidePanel.AddChannel -> AddChannelPanelContent(rows = USERS, picked = listOf("5002", "5003"), pinned = pinned,
            onPin = onPin, onClose = onClose, onGroup = onGroup)
        SidePanel.NewGroup -> NewGroupPanelContent(form = FORM, onBack = onBack, onClose = onClose, onCancel = onBack)
        SidePanel.Book -> BookPanelContent(book = CUI.book, pinned = pinned, onPin = onPin, onClose = onClose)
        is SidePanel.Event -> EventPanel(row = EVENTS.firstOrNull { it.id == panel.id }, all = EVENTS, onClose = onClose,
            onOpenChannel = {}, onHistory = {}, onReply = {})
    }
}
