// «영상» 절·«새 영상» 배너 미리보기 — 표본 자료, 세션·엔진 없이 (android_dispatch_tablet.md §6.14)
//
// 영상 칸의 그림은 엔진이 Surface 에 그리므로 여기서는 **자리 표시**만 선다(«영상 기다리는 중…»). 기기에서는
//   adb shell am start -n com.cims.ue.dispatch/.debug.PreviewActivity --es name video-panel   (또는 video-banner)
package com.cims.ue.dispatch.ui.ptt

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.session.VideoOrient
import com.cims.ue.dispatch.ui.AppScreen
import com.cims.ue.dispatch.ui.AppShellContent
import com.cims.ue.dispatch.ui.DispatchBody
import com.cims.ue.dispatch.ui.DispatchTabs
import com.cims.ue.dispatch.ui.ListToggle
import com.cims.ue.dispatch.ui.NavBadges
import com.cims.ue.dispatch.ui.PreviewFrame
import com.cims.ue.dispatch.ui.PreviewFull
import com.cims.ue.dispatch.ui.PttPane
import com.cims.ue.dispatch.ui.RegDot
import com.cims.ue.dispatch.ui.TopBarUi
import com.cims.ue.dispatch.ui.VideoBannerContent
import com.cims.ue.dispatch.ui.VideoBannerUi
import com.cims.ue.dispatch.ui.pageOf

private val SENDERS = listOf(
    VideoSenderUi("tel:5003", "박현장", "현장지휘 · 00:34", receiving = true),
    VideoSenderUi("tel:5004", "최순찰", "00:03", action = "바꿔 보기"))

/** ① 보내는 사람 없음. */
private val IDLE = VideoSectionUi(sub = "채널 참여와 함께 연결됨", showSend = true, sendEnabled = true, sendStarts = true, noSender = true)

/** ② 누가 보냄 — 고르기 전. */
private val NOTIFIED = IDLE.copy(noSender = false, countText = "보내는 중 2 (한 번에 1개)",
    senders = listOf(SENDERS[0].copy(receiving = false), SENDERS[1].copy(action = "보기")))

/** ③ [보기] 뒤 — 영상 칸 + 내 송출까지. */
private val WATCHING = VideoSectionUi(
    sub = "채널 참여와 함께 연결됨", receiving = true, receivingId = "tel:5003", caption = "박현장 · 현장지휘 · 00:34",
    showSend = true, sendText = "보내기 끝", sendEnabled = true, sendActive = true, sending = true,
    sendingCaption = "내 영상 보내는 중 · 00:12 · 보는 사람 2", micNote = true, canSwitchCamera = true,
    countText = "보내는 중 2 · 보는 중 1 (한 번에 1개)", senders = SENDERS)

/** 편성 영상 채널 — 영상 호가 없다([영상 보내기] 가 연다). */
private val PREARRANGED = VideoSectionUi(sub = "편성 영상 그룹 — [영상 보내기] 로 영상 호를 엽니다", showSend = true, sendEnabled = true,
    sendStarts = true)

private val HEAD = ChannelHeadUi(id = "g1", title = "순찰1", badge = "멤버", joined = true, isMemberGroup = true,
    canTarget = true, targeted = true, unread = 3, groupId = "g1")

private val PEOPLE = listOf(
    PersonRowUi("5001", "김관제", "PTT 5001 · 관제과 1팀", isMe = true, chair = true),
    PersonRowUi("5003", "박현장", "PTT 5003 · 현장과", speaking = true),
    PersonRowUi("5004", "최순찰", "PTT 5004 · 현장과"))

private val CARDS = listOf(
    MineCardUi("g1", "1. 순찰1", "순찰1", sub = "발언 박현장 00:14", roster = "김관제(나) · 박현장 · 최순찰 +4", meta = "참가 7 · 12:31",
        speaking = true, active = true, joined = true, control = CardControl.TARGET, on = true, selected = true,
        videoCount = 2, videoWatching = true),
    MineCardUi("g2", "2. 상황실", "상황실", sub = "발언 없음", roster = "이당직 · 서상황", meta = "참가 3 · 05:02",
        active = true, joined = true, control = CardControl.TARGET, videoCount = 1),
    MineCardUi("g3", "3. 교통1", "교통1", sub = "대기 · 멤버 12", roster = "미참여", control = CardControl.JOIN))

@Composable
private fun Panel(ui: VideoSectionUi) = ChannelPanelContent(
    head = HEAD, info = "멤버 그룹 · 참가 7 · 12:31 · 편성 12 · 발언 박현장", memberCount = 12, connected = PEOPLE, members = PEOPLE,
    video = { VideoSectionContent(ui) })

/** 패널 한 장(400×608) — 본문 높이 안에서 «영상» 절과 명단이 어떻게 나눠 갖는지 본다. */
@Composable
private fun PanelOnly(ui: VideoSectionUi, dark: Boolean = false) = PreviewFrame(dark = dark) {
    Column(Modifier.width(400.dp).fillMaxHeight()) { Panel(ui) }
}

@Preview(name = "영상 절 — ① 보내는 사람 없음", widthDp = 400, heightDp = 608, showBackground = true)
@Composable private fun VideoIdle() = PanelOnly(IDLE)

@Preview(name = "영상 절 — ② 누가 보냄", widthDp = 400, heightDp = 608, showBackground = true)
@Composable private fun VideoNotified() = PanelOnly(NOTIFIED)

@Preview(name = "영상 절 — ③ 보는 중 + 내 송출", widthDp = 400, heightDp = 608, showBackground = true)
@Composable private fun VideoWatching() = PanelOnly(WATCHING)

@Preview(name = "영상 절 — 가로 송출(4:3) · 어둡게", widthDp = 400, heightDp = 608, showBackground = true)
@Composable private fun VideoWide() = PanelOnly(WATCHING.copy(orient = VideoOrient(wide = true), sending = false, sendActive = false,
    sendText = "영상 보내기"), dark = true)

@Preview(name = "영상 절 — 편성 영상 채널(영상 호 없음)", widthDp = 400, heightDp = 608, showBackground = true)
@Composable private fun VideoPrearranged() = PanelOnly(PREARRANGED)

/** 껍데기까지 — 레일·상단 바·탭 줄·발언 바 + 본문(«채널» 면) + 오른쪽 패널·배너 층. */
@Composable
private fun Shell(banners: List<VideoBannerUi> = emptyList(), panel: (@Composable () -> Unit)? = null) = PreviewFrame {
    val page = pageOf(PttPane.CHANNELS)
    AppShellContent(
        screen = AppScreen.DISPATCH,
        top = TopBarUi(displayName = "김관제", deskLine = "관제1과 · 대표 7000", registrations = listOf(RegDot.ON, RegDot.ON)),
        talkBar = { TalkBarContent(targets = emptyList(), anyJoined = true) },
        panelOpen = panel != null,
        banners = { VideoBannerContent(banners) },
        tabs = { DispatchTabs(page, onMode = {}, onPage = {}, badges = NavBadges()) { ListToggle("사용자", open = false, onClick = {}) } },
    ) {
        DispatchBody(page, onPage = {}, panel = panel) { p ->
            if (p == page) ChannelsPaneContent(mine = CARDS, other = emptyList(), selectedId = "g1".takeIf { panel != null })
        }
    }
}

/** 기기 미리보기 `video-panel` — «영상» 절이 선 채널 상세(영상 칸은 자리 표시). */
@Preview(name = "관제 — 채널 상세 «영상» 절", device = PreviewFull, showBackground = true)
@Composable
internal fun DeviceVideoPanel() = Shell(panel = { Panel(WATCHING) })

/** 기기 미리보기 `video-banner` — «새 영상» 배너와 카드 «영상 n». */
@Preview(name = "관제 — «새 영상» 배너", device = PreviewFull, showBackground = true)
@Composable
internal fun DeviceVideoBanner() = Shell(banners = listOf(
    VideoBannerUi("g1", "새 영상 · 순찰1", "박현장(현장지휘)이 영상을 보냅니다", System.currentTimeMillis() - 7_000),
    VideoBannerUi("g2", "새 영상 · 상황실", "서상황이 영상을 보냅니다", System.currentTimeMillis() - 31_000)))
