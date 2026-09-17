// 감청·청취 전면 시트 (docs/design/features/android_dispatch_tablet.md §6.8, dispatch_desktop_ui.md §5·§12)
//
// 데스크톱은 감청 하나 = 별창이지만 태블릿에는 별창이 없다(§12) — **전면 시트 하나**에 열린 감청·청취를
// 모두 담는다. 시트를 닫아도 청취는 계속된다(상단 «감청 N» 칩이 다시 연다) — 끝내는 것은 [청취 종료]뿐이다.
//
// 두 종류의 차이는 서버가 만든다:
//   · **VoLTE 감청** = RFC 3911 Join `a=recvonly` → CMP 가 양 peer 를 SSRC 2개로 분리 인도(RFC 5576 라벨).
//     그래서 줄이 둘이다 — 믹싱은 단말이 한다.
//   · **PTT 청취** = `joinGroupCall(listenOnly)` → Permission=0 이라 발언을 요청할 수 없다.
// 앱은 그 결과를 그릴 뿐 자격을 판정하지 않는다(dispatch_center.md §5.6a).
package com.cims.ue.dispatch.ui.monitor

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.hangup
import com.cims.ue.dispatch.session.leave
import com.cims.ue.dispatch.ui.ptt.fmtElapsed
import com.cims.ue.sdk.MediaSource
import kotlinx.coroutines.launch

/** 열린 감청·청취 — 상단 칩과 시트가 같은 목록을 본다. */
@Composable
fun rememberMonitorSessions(session: DispatchSession): List<SessionItem> {
    val all by session.sessions.collectAsStateWithLifecycle()
    return all.filter { it.kind.isSheet && it.isLive }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MonitorSheet(session: DispatchSession, onDismiss: () -> Unit) {
    val rows = rememberMonitorSessions(session)
    val scope = rememberCoroutineScope()

    // 마지막 감청이 끝나면 시트도 닫는다 — 빈 시트가 화면을 덮지 않게.
    LaunchedEffect(rows.isEmpty()) { if (rows.isEmpty()) onDismiss() }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
            Text("감청·청취 ${rows.size}", fontSize = 15.sp, fontWeight = FontWeight.Bold)
            Text("시트를 닫아도 청취는 계속됩니다 — 끝내려면 [청취 종료]",
                fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(8.dp))
            rows.forEach { s ->
                when (s.kind) {
                    SessionKind.PTT_LISTEN -> PttListenCard(session, s) {
                        scope.launch { session.leave(s.callId) }
                    }
                    else -> MonitorCard(session, s) {
                        scope.launch { session.hangup(s.callId) }
                    }
                }
                Spacer(Modifier.height(10.dp))
            }
            Spacer(Modifier.height(12.dp))
        }
    }
}

/** VoLTE 감청 — 두 줄(caller/callee). RFC 5576 `label` 이 어느 쪽인지 알려 준다. */
@Composable
private fun MonitorCard(session: DispatchSession, s: SessionItem, onEnd: () -> Unit) {
    val hidden = session.dispatch.listenVisibility != "transparent"
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("감청 — ${s.title.ifBlank { userPart(s.info.remoteUri) }}",
                    fontSize = 14.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.width(8.dp))
                Tag(if (hidden) "은닉" else "투명",
                    if (hidden) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.tertiary)
                Spacer(Modifier.weight(1f))
                Text(fmtElapsed(s.elapsedMs), fontSize = 12.sp)
            }
            Spacer(Modifier.height(6.dp))
            // 소스가 둘로 갈라져 오지 않으면(구형 서버·믹스 인도) 한 줄만 보인다.
            val sources = s.info.sources
            if (sources.isEmpty()) Text("수신 중 — 소스 라벨이 아직 없습니다", fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            else sources.forEach { src -> SourceRow(src) }

            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("🎧 라우트 ${s.info.playbackRoute}", fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.weight(1f))
                TextButton(onClick = onEnd) {
                    Text("청취 종료", color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
}

/** PTT 청취 — 발언자·참가자·긴급. 발언 요청은 서버가 막는다(Permission=0). */
@Composable
private fun PttListenCard(session: DispatchSession, s: SessionItem, onEnd: () -> Unit) {
    val groups by session.groups.collectAsStateWithLifecycle()
    val g = groups.firstOrNull { it.id == s.info.groupId }
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("청취 — ${g?.name ?: s.info.groupId}", fontSize = 14.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.width(8.dp))
                Tag("청취 전용", MaterialTheme.colorScheme.primary)
                if (s.isEmergency) Tag("긴급", MaterialTheme.colorScheme.error)
                Spacer(Modifier.weight(1f))
                Text(fmtElapsed(s.elapsedMs), fontSize = 12.sp)
            }
            Spacer(Modifier.height(4.dp))
            Text(
                if (s.speaker.isNotBlank()) "발언 ${s.speaker} · ${fmtElapsed(s.speakerElapsedMs)}"
                else "발언 없음",
                fontSize = 13.sp,
                fontWeight = if (s.speaker.isNotBlank()) FontWeight.Bold else FontWeight.Normal)
            s.info.sources.forEach { src -> SourceRow(src) }
            Text("참가 ${g?.connectedCount ?: 0}명", fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text("발언 요청 불가 — 청취 전용 합류입니다", fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("🔊 라우트 ${s.info.playbackRoute}", fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.weight(1f))
                TextButton(onClick = onEnd) {
                    Text("청취 종료", color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
}

/**
 * 소스 한 줄 — 이름 · 활성 점 · 레벨 미터.
 *
 * `level` 은 아직 실시간 값이 없다(pjproject 에 SSRC 별 수신 관측 API 가 없어 SDP 라벨만 온다, §11).
 * 그때까지는 `active` 로만 켜고, API 가 생기면 이 줄의 폭만 바뀐다.
 */
@Composable
private fun SourceRow(src: MediaSource) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).clip(RoundedCornerShape(4.dp)).background(
            if (src.active) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.surfaceVariant))
        Spacer(Modifier.width(6.dp))
        Text(src.label.ifBlank { "ssrc ${src.ssrc}" }, Modifier.width(120.dp), fontSize = 12.sp, maxLines = 1)
        Box(Modifier.weight(1f).height(6.dp).clip(RoundedCornerShape(3.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)) {
            val w = if (src.level > 0f) src.level.coerceIn(0f, 1f) else if (src.active) 0.35f else 0f
            if (w > 0f) Box(Modifier.fillMaxWidth(w).fillMaxHeight()
                .background(MaterialTheme.colorScheme.primary))
        }
    }
}

@Composable
private fun Tag(text: String, color: Color) {
    Surface(color = color.copy(alpha = 0.2f), shape = RoundedCornerShape(4.dp),
        modifier = Modifier.padding(start = 4.dp)) {
        Text(text, Modifier.padding(horizontal = 5.dp, vertical = 1.dp), fontSize = 10.sp, color = color)
    }
}

private fun userPart(uri: String): String =
    uri.substringAfter(':', uri).substringBefore('@').substringBefore(';')
