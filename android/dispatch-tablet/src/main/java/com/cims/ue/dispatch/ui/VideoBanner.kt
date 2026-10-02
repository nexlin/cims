// «새 영상» 배너 — 전 화면 공통 (android_dispatch_tablet.md §6.14, dispatch_desktop_ui.md §3.2·§10.3)
//
// 영상 채널(MCVideo)에 새 송출 알림(Media Transmission Notification, TS 24.581 §6.2.5.3.2)이 오면 착신 배너와 같은 층에 선다 —
// 옅은 청록 면, 두 줄(«새 영상 · 그룹» / «누구(기능 별칭)이 영상을 보냅니다») · 경과 · [보기] · [닫기].
//
// 왜 배너인가: 수신은 manual 이다(D8 — 골라서 본다). 보내는 사람은 아무도 보지 않으면 서버가 송출을 끝내는 시한(T11 Stream Reception
// Idle, 기본 10초 — TS 24.581 §6.3.4.4.13 #8)을 기다리고 있다. 관제사가 [통화]·[이력] 에 있는 동안에도 보여야 한다.
//
// 채널마다 하나(가장 최근 송출)다. 그 송출을 보기 시작하거나 송출이 끝나면 스스로 빠진다 — [닫기] 는 배너만 내리고, 채널 상세
// «영상 n» 목록에서 다시 [보기] 할 수 있다(TS 22.281 R-5.2.6.2.2-009 NOTE 3).
package com.cims.ue.dispatch.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.widthIn
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.VideoBanner
import com.cims.ue.dispatch.session.VideoText
import com.cims.ue.dispatch.session.acceptVideoBanner
import com.cims.ue.dispatch.session.dismissVideoBanner
import com.cims.ue.dispatch.session.videoBanners

/** 배너 한 장이 그리는 데 필요한 전부 — 세션 없이 Preview·단위시험이 선다. */
data class VideoBannerUi(
    /** 채널(그룹 id) — 채널마다 하나라 이것이 열쇠다. [보기] 가 여는 채널 상세이기도 하다. */
    val groupId: String,
    /** «새 영상 · 순찰1». */
    val head: String,
    /** «김현장(현장지휘)이 영상을 보냅니다». */
    val line: String,
    val sinceMs: Long = System.currentTimeMillis(),
)

/** 새 송출 알림 → 배너. 주격 조사는 이름의 받침으로 고른다. */
internal fun VideoBanner.toBannerUi(): VideoBannerUi =
    VideoBannerUi(groupId, "새 영상 · $title", "${VideoText.withIGa(who)} 영상을 보냅니다", sinceMs)

/**
 * «새 영상» 배너 스택 — **세션을 붙이는 껍데기**.
 *
 * @param onOpen [보기] — 그 채널 상세를 연다(그 송출을 받는 것은 세션이 한다 — 보던 것이 있으면 바꿔 본다)
 */
@Composable
fun VideoBanners(session: DispatchSession, onOpen: (String) -> Unit, modifier: Modifier = Modifier) {
    val banners by session.videoBanners.collectAsStateWithLifecycle()
    VideoBannerContent(
        items = banners.map { it.toBannerUi() }, modifier = modifier,
        onView = { id -> banners.firstOrNull { it.groupId == id }?.let { b -> session.acceptVideoBanner(b); onOpen(b.groupId) } },
        onDismiss = { id -> banners.firstOrNull { it.groupId == id }?.let(session::dismissVideoBanner) })
}

/** 배너 스택 — **순수 컴포저블**. 최신이 위. */
@Composable
fun VideoBannerContent(
    items: List<VideoBannerUi>,
    modifier: Modifier = Modifier,
    onView: (String) -> Unit = {},
    onDismiss: (String) -> Unit = {},
) {
    AnimatedVisibility(visible = items.isNotEmpty(), enter = expandVertically(), exit = shrinkVertically(), modifier = modifier) {
        Column(Modifier.fillMaxWidth()) {
            items.forEach { b ->
                key(b.groupId) {
                    BannerBar(tone = BannerTone.VIDEO, line1 = b.head, line2 = b.line, sinceMs = b.sinceMs) {
                        // [보기] = 채운 청록(보고 듣는 축의 주 행동), [닫기] = 배너만 내린다.
                        PillButton("보기", { onView(b.groupId) }, kind = Pill.LISTEN_FILL, height = 44.dp,
                            modifier = Modifier.widthIn(min = 104.dp))
                        PillButton("닫기", { onDismiss(b.groupId) }, kind = Pill.LINE, height = 44.dp)
                    }
                }
            }
        }
    }
}
