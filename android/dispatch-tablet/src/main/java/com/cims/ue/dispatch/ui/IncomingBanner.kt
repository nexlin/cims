// 착신 배너 — 전 화면 공통 (docs/design/features/android_dispatch_tablet.md §6.2, dispatch_desktop_ui.md §3.2)
//
// **왜 배너가 따로 있어야 하는가**: 착신 카드는 [관제] > [일반통화] 탭 안에만 있다. 관제사가 [PTT] 탭이나
// [이력]·[관리] 화면에 있는 동안 걸려 온 전화는 그 카드에 조용히 쌓일 뿐 어디에도 보이지 않는다 —
// 그러면 아무도 받지 않고, 발신자가 끊어(CANCEL→487) 서버 이력에는 «거절» 로 남는다.
// 배너는 화면과 무관하게 뜨는 **유일한 착신 표면**이다.
//
// 배너에서 응답하면 관제 > 일반통화 탭으로 돌아간다 — 보류·전달·종료 버튼이 거기 있다(§3.4 자동 복귀).
package com.cims.ue.dispatch.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.answer
import com.cims.ue.dispatch.session.reject
import kotlinx.coroutines.launch

/** 착신 종류 → 배너 색(§3.2) — 대표번호 옅은 주황 · 직접 옅은 파랑 · 개별 통화 옅은 청록. */
private fun bannerTone(s: SessionItem, isPilot: Boolean): BannerTone = when {
    s.kind == SessionKind.PTT_PRIVATE -> BannerTone.PTT
    isPilot -> BannerTone.PILOT
    else -> BannerTone.DIRECT
}

@Composable
fun IncomingBanners(
    session: DispatchSession,
    onAnswered: (SessionItem) -> Unit,
    modifier: Modifier = Modifier,
) {
    val calls by session.incoming.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    AnimatedVisibility(
        visible = calls.isNotEmpty(),
        enter = expandVertically(),
        exit = shrinkVertically(),
        modifier = modifier,
    ) {
        Column(Modifier.fillMaxWidth()) {
            // 최신이 위. 여럿이 울려도 전부 보인다 — 어느 것을 받을지는 관제사가 고른다.
            calls.forEach { c ->
                Banner(
                    session = session, call = c,
                    onAnswer = { scope.launch(com.cims.ue.dispatch.session.UnhandledGuard) { session.answer(c.callId); onAnswered(c) } },
                    onReject = { scope.launch(com.cims.ue.dispatch.session.UnhandledGuard) { session.reject(c.callId) } })
            }
        }
    }
}

@Composable
private fun Banner(
    session: DispatchSession,
    call: SessionItem,
    onAnswer: () -> Unit,
    onReject: () -> Unit,
) {
    val pilot = call.info.calledParty.isNotEmpty() && session.isPilot(call.info.calledParty)
    // 주소록이 늦게 서도(기동 직후의 착신) 이름이 따라 붙게 한다 — 라벨은 주소록에서 풀지만 스스로 알리지 않는다.
    val book by session.phoneBook.collectAsStateWithLifecycle()
    @Suppress("UNUSED_EXPRESSION") book
    // 경과는 1초마다 다시 그린다 — 얼마나 울리고 있는지가 받을지 말지의 판단 재료다.
    BannerBar(
        tone = bannerTone(call, pilot),
        line1 = buildString {
            append(if (pilot) "대표번호 ${com.cims.ue.dispatch.session.localNumber(com.cims.ue.dispatch.session.userPart(session.dispatch.pilotId))} 착신" else "착신")
            if (call.kind == SessionKind.PTT_PRIVATE) append(" · 개별 통화")
        },
        line2 = session.displayLabel(call.info.remoteUri),
        sinceMs = call.connectedAtMs ?: call.startedAtMs,
    ) {
        PillButton("응답", onAnswer, kind = Pill.CALL, height = 44.dp, modifier = Modifier.widthIn(min = 104.dp))
        PillButton("거절", onReject, kind = Pill.LINE, height = 44.dp)
    }
}
