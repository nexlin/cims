// 관제 요약 띠 (docs/design/features/android_dispatch_tablet.md §6.2, dispatch_desktop_ui.md §3.5)
//
// 관제 밖 화면([이력]·[PTT 그룹]·[관리]) 상단에 상시. **다른 VM 의 투영이라 상태를 갖지 않는다** —
// 발언 대상·발언 상태·대기열·미읽음·내 통화·감청 시트를 한 줄로 접고, PTT 버튼만 조작을 받는다.
// 관제로 돌아가지 않고도 «지금 무슨 일이 벌어지는지» 를 보게 하는 것이 목적이다.
package com.cims.ue.dispatch.ui

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
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.ui.call.CallDeskViewModel
import com.cims.ue.dispatch.ui.ptt.PttChannelsViewModel
import com.cims.ue.dispatch.ui.ptt.PttButton
import com.cims.ue.dispatch.ui.ptt.fmtElapsed
import com.cims.ue.sdk.CallState

/** 인증서 경고 임계 — sip_tls_signaling.md §8.6 의 60/30/7 중 화면 경고는 30일. */
private const val CERT_WARN_DAYS = 30

@Composable
fun SummaryStrip(
    session: DispatchSession,
    ptt: PttChannelsViewModel?,
    calls: CallDeskViewModel?,
    lockEnabled: Boolean,
    onGoDispatch: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val targets = ptt?.targets?.collectAsStateWithLifecycle()?.value.orEmpty()
    val locked = ptt?.locked?.collectAsStateWithLifecycle()?.value == true
    val sessions by session.sessions.collectAsStateWithLifecycle()
    val messages by session.messages.collectAsStateWithLifecycle()
    val queue = calls?.queue?.collectAsStateWithLifecycle()?.value.orEmpty()

    // 관제 밖에서도 발언은 이어진다 — 떠날 때 반드시 해제한다(TalkBar 와 같은 불변, §6.3).
    DisposableEffect(ptt) { onDispose { ptt?.releaseAll() } }

    val granted = targets.count { it.granted }
    val speaking = granted > 0
    val requesting = !speaking && targets.any { it.requesting || it.queued }
    val emergency = targets.any { it.card.emergency }

    // 내 통화 — 감청 시트는 빼고 센다(감청은 따로 보인다).
    val phone = sessions.filter { it.kind == SessionKind.PHONE_CALL }
    val incoming = phone.count { it.info.state == CallState.INCOMING }
    val talking = phone.count { it.isActive }
    val held = phone.count { it.info.state == CallState.HELD }
    val sheets = sessions.count { it.kind.isSheet }
    val unread = messages.values.sumOf { list -> list.count { !it.read && !it.outgoing } }

    // 다른 사람이 말하는 중이면 그 이름 — 포커스 채널 기준(§3.5).
    val otherSpeaker = targets.firstOrNull { it.card.speaker.isNotBlank() && !it.granted }?.card
    val talkSince = targets.firstOrNull { it.granted }?.card?.session?.speakerElapsedMs
        ?: otherSpeaker?.session?.speakerElapsedMs

    Surface(tonalElevation = 2.dp, modifier = modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 10.dp, vertical = 4.dp).height(IntrinsicSize.Min),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)) {

            // ① 발언 대상
            Cell {
                if (targets.isEmpty()) Muted("발언 대상 없음")
                else {
                    Text("발언 대상 ${targets.size}", fontSize = 11.sp, fontWeight = FontWeight.Bold)
                    Text(targets.take(3).joinToString("·") { it.name } +
                         if (targets.size > 3) " +${targets.size - 3}" else "",
                        fontSize = 11.sp, maxLines = 1)
                }
                if (emergency) Text(" ⚠긴급", fontSize = 11.sp, color = MaterialTheme.colorScheme.error)
            }
            Divider()

            // ② 발언 상태
            Cell {
                val scheme = MaterialTheme.colorScheme
                when {
                    speaking -> Text("내 발언 중 $granted/${targets.size}", fontSize = 11.sp,
                        fontWeight = FontWeight.Bold, color = scheme.primary)
                    requesting -> Text(targets.firstOrNull { it.queued }?.stateText?.ifBlank { "요청 중" }
                            ?: "요청 중",
                        fontSize = 11.sp, fontWeight = FontWeight.Bold, color = scheme.tertiary)
                    otherSpeaker != null -> Text("발언 ${otherSpeaker.speaker}", fontSize = 11.sp)
                    else -> Muted("대기")
                }
                talkSince?.takeIf { it > 0 }?.let {
                    Text(" ${fmtElapsed(it)}", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }

            // ③ PTT — 발언 바와 같은 색 규약, 작은 크기
            if (ptt != null) PttButton(
                enabled = targets.isNotEmpty(), speaking = speaking, requesting = requesting, locked = locked,
                label = if (speaking) "발언" else "PTT",
                onDown = { ptt.pttDown(lockEnabled) }, onUp = { ptt.pttUp(lockEnabled) },
                width = 96.dp, height = 34.dp, compact = true)
            Divider()

            // ④ 대표번호 대기열 · 문자 미읽음
            Cell {
                val q = queue.size
                Text("대기열 $q", fontSize = 11.sp,
                    color = if (q > 0) MaterialTheme.colorScheme.tertiary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                    fontWeight = if (q > 0) FontWeight.Bold else FontWeight.Normal)
                if (unread > 0) Text(" · 미읽음 $unread", fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.tertiary)
            }
            Divider()

            // ⑤ 내 통화
            Cell {
                if (incoming + talking + held == 0) Muted("내 통화 없음")
                else Text(listOfNotNull(
                        incoming.takeIf { it > 0 }?.let { "착신 $it" },
                        talking.takeIf { it > 0 }?.let { "통화 $it" },
                        held.takeIf { it > 0 }?.let { "보류 $it" }).joinToString(" · "),
                    fontSize = 11.sp, fontWeight = FontWeight.Bold)
            }
            Divider()

            // ⑥ 감청 시트
            Cell {
                if (sheets > 0) Text("감청 $sheets", fontSize = 11.sp, fontWeight = FontWeight.Bold,
                    color = Color(0xFFB794F6))
                else Muted("감청 없음")
            }

            CertBadge(session)

            Spacer(Modifier.weight(1f))
            TextButton(onClick = onGoDispatch, contentPadding = PaddingValues(horizontal = 8.dp)) {
                Text("관제로 F1", fontSize = 11.sp)
            }
        }
    }
}

/**
 * 서버 인증서 경고 — 잔여 ≤ 30일. 관제 배너가 없는 화면에서는 이것이 유일한 표면이다.
 *
 * 닫기는 없다 — 서버가 갱신되면 사라진다(sip_tls_signaling.md §8.6.2).
 */
@Composable
private fun CertBadge(session: DispatchSession) {
    val days = remember(session) { session.cscOrNull()?.tlsPeerExpiry()?.daysLeft }
    if (days == null || days > CERT_WARN_DAYS) return
    Surface(color = MaterialTheme.colorScheme.error.copy(alpha = 0.2f), shape = RoundedCornerShape(4.dp)) {
        Text(
            if (days <= 0) "서버 인증서 만료됨 — 운영자에게 알리세요"
            else "서버 인증서 ${days}일 후 만료 — 운영자에게 알리세요",
            Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
            fontSize = 11.sp, color = MaterialTheme.colorScheme.error)
    }
}

@Composable
private fun Cell(content: @Composable RowScope.() -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, content = content)
}

@Composable
private fun Divider() {
    VerticalDivider(Modifier.height(18.dp))
}

@Composable
private fun RowScope.Muted(text: String) {
    Text(text, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
}
