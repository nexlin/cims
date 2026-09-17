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
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.answer
import com.cims.ue.dispatch.session.reject
import com.cims.ue.dispatch.ui.ptt.fmtElapsed
import kotlinx.coroutines.launch

/** 착신 종류별 색(§3.2) — 대표번호 주황 · 직접 파랑 · 사설콜 청록. */
private fun bannerColor(s: SessionItem, isPilot: Boolean): Color = when {
    s.kind == SessionKind.PTT_PRIVATE -> Color(0xFF14B8A6)
    isPilot -> Color(0xFFF59E0B)
    else -> Color(0xFF3B82F6)
}

@Composable
fun IncomingBanners(
    session: DispatchSession,
    onAnswered: () -> Unit,
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
                    onAnswer = { scope.launch { session.answer(c.callId); onAnswered() } },
                    onReject = { scope.launch { session.reject(c.callId) } })
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
    val color = bannerColor(call, pilot)
    // 경과는 1초마다 다시 그린다 — 얼마나 울리고 있는지가 받을지 말지의 판단 재료다.
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(call.callId) {
        while (true) { kotlinx.coroutines.delay(1000); tick++ }
    }

    Surface(color = color.copy(alpha = 0.22f), modifier = Modifier.fillMaxWidth()) {
        Row(
            Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Box(Modifier.size(10.dp).background(color, RoundedCornerShape(5.dp)))
            Column(Modifier.weight(1f)) {
                Text(
                    buildString {
                        append(if (pilot) "대표번호 ${session.dispatch.pilotId} 착신" else "착신")
                        if (call.kind == SessionKind.PTT_PRIVATE) append(" · 사설콜")
                    },
                    fontSize = 12.sp, color = color, fontWeight = FontWeight.Bold)
                Text(session.displayLabel(call.info.remoteUri),
                    fontSize = 19.sp, fontWeight = FontWeight.Bold)
            }
            @Suppress("UNUSED_EXPRESSION") tick     // 1초 틱을 이 조합에 묶는다
            Text(fmtElapsed(call.elapsedMs), fontSize = 14.sp)
            Button(onClick = onAnswer, modifier = Modifier.height(48.dp).widthIn(min = 104.dp)) {
                Text("응답", fontSize = 16.sp, fontWeight = FontWeight.Bold)
            }
            OutlinedButton(onClick = onReject, modifier = Modifier.height(48.dp)) { Text("거절") }
        }
    }
}
