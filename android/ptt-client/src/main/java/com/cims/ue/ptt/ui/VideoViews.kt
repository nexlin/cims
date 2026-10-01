package com.cims.ue.ptt.ui

import android.os.SystemClock
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.R
import com.cims.ue.ptt.VideoCallState
import com.cims.ue.ptt.VideoMicPolicy
import com.cims.ue.sdk.ReceptionState
import com.cims.ue.sdk.TransmissionState
import com.cims.ue.sdk.VideoTransmitter
import kotlinx.coroutines.delay

// MCVideo 그룹 영상의 주채널 조각(mcvideo.md §5.5 — 영상 칸은 볼 영상이 있을 때만, 하단 탭은 늘 그대로).
//   [영상 보내기] = 발언 상태 줄 오른쪽 토글(D11) · 내 송출 카드 · «영상 n» 목록(골라 보기, manual) · [보기] 뒤 영상 칸.

/** [영상 보내기] 토글 — 켬 = 송출 요청, 끔 = 송출 끝내기(대기·요청 중이면 거둔다). 영상 호가 성립 전이면 누를 수 없다. */
@Composable
internal fun VideoSendButton(v: VideoCallState?, onToggle: (Boolean) -> Unit) {
    val enabled = v?.active == true
    val on = v?.sendOn == true
    val label = when (v?.transmission) {
        TransmissionState.PERMITTED -> "보내는 중"
        TransmissionState.PENDING_REQUEST -> "요청 중…"
        TransmissionState.QUEUED -> v.queuePosition?.let { "대기 ${it}번째" } ?: "대기 중"
        TransmissionState.PENDING_END -> "멈추는 중…"
        else -> "영상 보내기"
    }
    val fg = when { on -> Ct.OnMint; enabled -> Ct.Text; else -> Ct.TextFaint }
    Row(
        Modifier.height(38.dp).clip(RoundedCornerShape(10.dp))
            .background(if (on) Ct.Mint else Ct.Surface)
            .border(1.dp, if (on) Ct.Mint else Ct.Border, RoundedCornerShape(10.dp))
            .clickable(enabled = enabled) { onToggle(!on) }
            .padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Icon(rememberVectorPainter(Icons.Filled.Videocam), contentDescription = null, tint = fg, modifier = Modifier.size(18.dp))
        Text(label, color = fg, fontSize = 12.sp, fontWeight = if (on) FontWeight.Bold else FontWeight.SemiBold)
    }
}

/** 내 송출 카드 — 카메라 썸네일 · «내 영상 보내는 중 · 경과» · 보는 사람 · 무전 마이크 안내(D12) · 카메라 전환. 요청·대기 중이면 그 상태. */
@Composable
internal fun MySendCard(st: PttUiState, v: VideoCallState, policy: VideoMicPolicy) {
    val now = rememberNowMs()
    val hwPtt by HwPtt.present.collectAsState()
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Ct.MintDim)
            .border(1.dp, Ct.Mint, RoundedCornerShape(12.dp)).padding(10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.width(66.dp).height(88.dp).clip(RoundedCornerShape(8.dp)).background(Color.Black)) {
            if (v.sending) SelfPreview { st.ctl?.setPreviewSurface(it) }
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            when (v.transmission) {
                TransmissionState.PERMITTED -> {
                    Text("내 영상 보내는 중 · ${fmtElapsed(now - v.sendingSinceMs)}", color = Ct.Mint, fontSize = 14.sp,
                        fontWeight = FontWeight.Bold)
                    // 보는 사람 = 서버의 Media Reception Notification(TS 24.581 §9.2.16) — 알림이 와야 센다(없으면 줄을 두지 않는다)
                    if (v.receivers > 0) Text("보는 사람 ${v.receivers}", color = Ct.Text, fontSize = 12.sp)
                    val ptt = if (hwPtt) "측면 PTT" else "PTT"
                    Text(
                        when {
                            v.voiceMuted -> "무전 중 — 영상은 계속, 영상 소리는 멈춤"
                            policy == VideoMicPolicy.VIDEO_FIRST -> "영상 보내는 동안 $ptt 는 무전 마이크를 쓰지 않습니다(긴급 제외)"
                            else -> "$ptt 누르는 동안 마이크는 음성 무전, 영상은 계속"
                        },
                        color = Ct.TextDim, fontSize = 11.sp)
                }
                TransmissionState.QUEUED -> Text(
                    (v.queuePosition?.let { "대기 ${it}번째" } ?: "대기 중") + " — 앞 송출이 끝나면 보냅니다",
                    color = Ct.Amber, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                TransmissionState.PENDING_END -> Text("영상 보내기를 멈추는 중…", color = Ct.TextDim, fontSize = 13.sp)
                else -> Text("영상 보내기 요청 중…", color = Ct.TextDim, fontSize = 13.sp)
            }
        }
        if (v.sending) {
            Box(
                Modifier.size(40.dp).clip(CircleShape).background(Ct.Bg).clickable { st.ctl?.switchCamera(v.callId) },
                contentAlignment = Alignment.Center,
            ) {
                Icon(rememberVectorPainter(Icons.Filled.Cameraswitch), contentDescription = "카메라 전환", tint = Ct.Text,
                    modifier = Modifier.size(20.dp))
            }
        } else if (v.transmission == TransmissionState.QUEUED) {
            Text("대기 취소", color = Ct.Amber, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                modifier = Modifier.clip(RoundedCornerShape(8.dp)).border(1.dp, Ct.Amber, RoundedCornerShape(8.dp))
                    .clickable { st.ctl?.setVideoTransmit(v.groupId, false) }.padding(horizontal = 10.dp, vertical = 6.dp))
        }
    }
}

/** «영상 n» 목록 — 지금 보내는 사람들(행 = 이름 · 기능 별칭 · 경과 + [보기]). 골라 보기(manual 수신, TS 24.581 §6.2.5.3.3). */
@Composable
internal fun VideoListCard(st: PttUiState, v: VideoCallState) {
    val now = rememberNowMs()
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Ct.Surface)
            .border(1.dp, Ct.Border, RoundedCornerShape(12.dp)).padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Icon(painterResource(R.drawable.ic_video), contentDescription = null, tint = Ct.Mint, modifier = Modifier.size(16.dp))
            Text("영상 ${v.transmitters.size}", color = Ct.Mint, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f))
            Text("골라서 보세요", color = Ct.TextDim, fontSize = 11.sp)
        }
        v.transmitters.forEach { t ->
            Spacer(Modifier.height(8.dp))
            Box(Modifier.fillMaxWidth().height(1.dp).background(Ct.Border))
            Spacer(Modifier.height(8.dp))
            TransmitterRow(st, v, t, now)
        }
    }
}

@Composable
private fun TransmitterRow(st: PttUiState, v: VideoCallState, t: VideoTransmitter, now: Long) {
    val name = st.memberName(v.groupId, t.userId)
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(Modifier.size(34.dp).clip(CircleShape).background(Ct.MintDim), contentAlignment = Alignment.Center) {
            Text(name.take(1), color = Ct.Mint, fontSize = 13.sp, fontWeight = FontWeight.Bold)
        }
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                Text(name, color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (t.functionalAlias.isNotBlank()) Text(t.functionalAlias, color = Ct.TextDim, fontSize = 12.sp, maxLines = 1)
            }
            Text("영상 보내는 중 " + (v.since[t.userId]?.let { fmtElapsed(now - it) } ?: ""), color = Ct.TextDim, fontSize = 11.sp)
        }
        when (t.state) {
            ReceptionState.PENDING_REQUEST -> Text("여는 중…", color = Ct.TextDim, fontSize = 12.sp)
            else -> Text("보기", color = Ct.OnMint, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.clip(RoundedCornerShape(8.dp)).background(Ct.Mint)
                    .clickable { st.ctl?.acceptVideo(v.groupId, t.userId) }.padding(horizontal = 16.dp, vertical = 8.dp))
        }
    }
}

/**
 * 보고 있는 영상 칸 — 주채널 안([크게] = 채팅 자리까지, 하단 탭은 그대로). 표면은 코어 수신 창 하나(`setVideoSurface`), 단말 인코딩
 * 480x640 세로라 3:4 로 칸 가운데. 캡션 «이름 · 기능 별칭 · 경과», [그만 보기]·[크게|작게], 다른 송출이 있으면 «이름 · 바꿔 보기».
 */
@Composable
internal fun VideoViewer(st: PttUiState, v: VideoCallState, rx: VideoTransmitter, large: Boolean, onLarge: () -> Unit,
                         modifier: Modifier = Modifier) {
    val now = rememberNowMs()
    val other = v.transmitters.lastOrNull { it.userId != rx.userId && it.state != ReceptionState.RECEIVING }
    Box(modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Color.Black)) {
        RemoteVideo(Modifier.align(Alignment.Center).fillMaxHeight().aspectRatio(VIDEO_VIEW_ASPECT), onTap = onLarge) {
            st.ctl?.setVideoSurface(it)
        }
        // [보기]·바꿔 보기 직후 키프레임까지 잠깐 덮는다 — 앞 송출의 마지막 프레임이 비치지 않게(CMP 가 수신 시작 때 송출자에게 PLI)
        if (!rememberRevealed(rx.userId)) Box(Modifier.matchParentSize().background(Color.Black))
        val caption = buildString {
            append(st.memberName(v.groupId, rx.userId))
            if (rx.functionalAlias.isNotBlank()) append(" · ").append(rx.functionalAlias)
            v.since[rx.userId]?.let { append(" · ").append(fmtElapsed(now - it)) }
        }
        Text(caption, color = Color.White, fontSize = 11.sp,
            modifier = Modifier.align(Alignment.TopStart).padding(8.dp).clip(RoundedCornerShape(6.dp))
                .background(Color.Black.copy(alpha = 0.6f)).padding(horizontal = 8.dp, vertical = 3.dp))
        Row(Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(8.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            ViewerButton("그만 보기") { st.ctl?.stopVideo(v.groupId, rx.userId) }
            ViewerButton(if (large) "작게" else "크게", onClick = onLarge)
            Spacer(Modifier.weight(1f))
            other?.let { o ->
                ViewerButton("${st.memberName(v.groupId, o.userId)} · 바꿔 보기", accent = true) { st.ctl?.acceptVideo(v.groupId, o.userId) }
            }
        }
    }
}

/** 영상 위 글자 단추 — 검정 바탕 위라 흰 글자(테마 무관), [accent] = 민트 테두리. */
@Composable
private fun ViewerButton(text: String, accent: Boolean = false, onClick: () -> Unit) {
    Text(text, color = if (accent) Ct.Mint else Color.White, fontSize = 12.sp, fontWeight = if (accent) FontWeight.Bold else FontWeight.Normal,
        maxLines = 1, overflow = TextOverflow.Ellipsis,
        modifier = Modifier.clip(RoundedCornerShape(8.dp)).background(Color.Black.copy(alpha = 0.6f))
            .then(if (accent) Modifier.border(1.dp, Ct.Mint, RoundedCornerShape(8.dp)) else Modifier)
            .clickable(onClick = onClick).padding(horizontal = 10.dp, vertical = 7.dp))
}

/** 보는 송출이 바뀌면 [VIDEO_REVEAL_MS] 동안 덮는다. */
@Composable
internal fun rememberRevealed(transmitter: String?): Boolean {
    var shown by remember { mutableStateOf(false) }
    LaunchedEffect(transmitter) {
        shown = false
        if (transmitter != null) { delay(VIDEO_REVEAL_MS); shown = true }
    }
    return shown
}

/** 1 초마다 바뀌는 지금(elapsedRealtime) — 경과 표시. */
@Composable
private fun rememberNowMs(): Long {
    var now by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(Unit) { while (true) { delay(1000); now = SystemClock.elapsedRealtime() } }
    return now
}

private fun fmtElapsed(ms: Long): String {
    val s = (ms / 1000).coerceAtLeast(0)
    return "%d:%02d".format(s / 60, s % 60)
}

private const val VIDEO_REVEAL_MS = 400L
private const val VIDEO_VIEW_ASPECT = 3f / 4f
