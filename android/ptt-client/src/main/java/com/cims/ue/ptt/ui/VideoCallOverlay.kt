package com.cims.ue.ptt.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.FullscreenExit
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material.icons.filled.VideocamOff
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.GroupCallState
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.R
import com.cims.ue.sdk.FloorState
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow

/**
 * 전체화면 영상 표시 상태 — 주채널 영상 세션(서버 video_enabled 그룹)이 생기면 AppRoot 가 켠다. [작게 보기]·뒤로로 끈 호는
 * 다시 자동으로 켜지 않고([minimizedCall]), 주채널 화면의 영상 칸을 누르면 다시 켠다. 코어 수신 창은 하나라 켜져 있는 동안
 * 영상 칸은 표면을 만들지 않는다.
 */
internal object VideoView {
    val full = MutableStateFlow(false)
    @Volatile var minimizedCall = -1
}

/**
 * PTT 그룹 영상 전체화면 — CIMS-Phone 영상 통화 화면과 같은 문법(세로 전체화면 영상·우하단 셀프뷰·탭으로 여닫는 우하단 버튼열).
 * 발언 상태는 늘 보인다(누가 말하는지가 무전의 핵심 정보 — 주채널 화면과 같은 [SpeakerStatusStrip]). 화면 PTT 단말은 PTT 바도
 * 늘 보인다. 버튼 = 내 영상 · 카메라 전환 · 오디오 출력 · 작게 보기 · 나가기.
 */
@Composable
fun VideoCallOverlay(st: PttUiState, s: GroupCallState) {
    var controls by remember { mutableStateOf(true) }
    LaunchedEffect(controls) { if (controls) { delay(4000); controls = false } }    // 4 s 뒤 자동 숨김(CIMS-Phone 과 같다)
    val mine = s.floorState == FloorState.SPEAKING
    val hwPtt by HwPtt.present.collectAsState()
    val minimize = { VideoView.minimizedCall = s.callId; VideoView.full.value = false }
    BackHandler(onBack = minimize)
    // 하단 여백 — 화면 PTT 바(64dp) 위로 셀프뷰·버튼열을 올린다
    val bottom = if (hwPtt) 24.dp else 24.dp + 64.dp + 16.dp

    Box(
        Modifier.fillMaxSize().background(Color.Black)
            .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { controls = !controls },
    ) {
        // 발언자 영상 — 세로 전체화면(앱은 세로 고정, 단말 인코딩 480x640 세로)
        RemoteVideo(Modifier.fillMaxSize(), onTap = { controls = !controls }) { st.ctl?.setVideoSurface(it) }
        // 말하는 사람이 없거나 내가 말하는 동안은 영상을 덮는다 — 발언이 끝나면 마지막 프레임을 남기지 않는다
        if (!rememberRemoteLive(s)) {
            Box(Modifier.fillMaxSize().background(Color.Black))
            if (!mine) Text("발언자 영상 대기", color = Color.White.copy(alpha = 0.6f), fontSize = 13.sp,
                modifier = Modifier.align(Alignment.Center))
        }

        // 상단 — 채널·발언 상태(늘 표시)
        Column(Modifier.align(Alignment.TopStart).fillMaxWidth().statusBarsPadding()
                .padding(horizontal = 16.dp, vertical = 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(st.groupName(s.groupId), color = Color.White, fontSize = 19.sp, fontWeight = FontWeight.Bold)
                if (s.emergency) PillBadge("긴급", Ct.Red, filled = true)
            }
            Spacer(Modifier.height(6.dp))
            SpeakerStatusStrip(st, s)
        }

        // 내 영상 — 우하단(버튼열 왼쪽), 내가 발언 중이고 [내 영상]이 켜져 있을 때
        if (mine && st.videoSend) {
            Box(Modifier.align(Alignment.BottomEnd).navigationBarsPadding().padding(bottom = bottom, end = 76.dp)
                    .width(96.dp).aspectRatio(3f / 4f).clip(RoundedCornerShape(12.dp)).background(Color(0xFF222222))) {
                SelfPreview { st.ctl?.setPreviewSurface(it) }
            }
        }

        // 버튼열 — 탭하면 보이고 4 s 뒤 숨는다
        if (controls) {
            Column(Modifier.align(Alignment.BottomEnd).navigationBarsPadding().padding(end = 8.dp, bottom = bottom),
                horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(14.dp)) {
                RoundControl(rememberVectorPainter(if (st.videoSend) Icons.Filled.Videocam else Icons.Filled.VideocamOff),
                    if (st.videoSend) "내 영상 켜짐" else "내 영상 꺼짐", active = st.videoSend) { st.ctl?.setVideoSend(!st.videoSend) }
                RoundControl(rememberVectorPainter(Icons.Filled.Cameraswitch), "카메라 전환") { st.ctl?.switchCamera(s.callId) }
                val (routeIcon, routeDesc) = when (st.route) {
                    PttController.AUDIO_ROUTE_HEADSET -> R.drawable.ic_headset to "이어폰"
                    PttController.AUDIO_ROUTE_SPEAKER -> R.drawable.ic_volume_on to "스피커폰"
                    else -> R.drawable.ic_earpiece to "수화기"
                }
                // 오디오 출력 — 이어폰이 없으면 스피커폰↔수화기 토글(이어폰 선택은 주채널 화면의 시트)
                RoundControl(painterResource(routeIcon), routeDesc, active = st.route != PttController.AUDIO_ROUTE_EARPIECE) {
                    if (st.headsets.isEmpty()) st.ctl?.setAudioRoute(
                        if (st.route == PttController.AUDIO_ROUTE_SPEAKER) PttController.AUDIO_ROUTE_EARPIECE
                        else PttController.AUDIO_ROUTE_SPEAKER)
                    else minimize()
                }
                RoundControl(rememberVectorPainter(Icons.Filled.FullscreenExit), "작게 보기") { minimize() }
                RoundControl(rememberVectorPainter(Icons.Filled.CallEnd), "나가기", bg = Ct.Red) { st.ctl?.leaveGroup(s.groupId) }
            }
        }

        // 화면 PTT 바 — 터치 단말만, 늘 표시(하드웨어 PTT 단말은 측면 키)
        if (!hwPtt) {
            PttBar(floor = st.floor, enabled = st.inCall, listenOnly = !s.canRequestFloor,
                queuePosition = s.queuePosition,
                modifier = Modifier.align(Alignment.BottomStart).navigationBarsPadding()
                    .padding(start = 16.dp, end = 16.dp, bottom = 16.dp).fillMaxWidth(),
                onDown = { st.ctl?.pttDown() }, onUp = { st.ctl?.pttUp() })
        }
    }
}

/** 다른 사람이 말하는 동안만 true — 발언이 끝나면(놓음·Idle) 곧바로 false 로 영상을 덮는다. 새 발언자가 잡으면
 *  [VIDEO_REVEAL_MS] 뒤에 연다 — 그 사이 앞 발언자의 마지막 프레임이 비치지 않게(화자 송출 개시 첫 프레임 = IDR). */
@Composable
internal fun rememberRemoteLive(s: GroupCallState): Boolean {
    val talker = s.speaker?.takeIf { !it.self }?.id
    var live by remember { mutableStateOf(false) }
    LaunchedEffect(talker) {
        live = false
        if (talker != null) { delay(VIDEO_REVEAL_MS); live = true }
    }
    return live
}

private const val VIDEO_REVEAL_MS = 400L

/** 전체화면 영상 위 둥근 버튼 — 아이콘만(반투명 스크림, 활성=민트, [bg] 지정 시 그 색). */
@Composable
private fun RoundControl(icon: Painter, desc: String, active: Boolean = false, bg: Color? = null, onClick: () -> Unit) {
    val back = bg ?: if (active) Ct.Mint else Color.Black.copy(alpha = 0.45f)
    val tint = if (bg == null && active) Ct.OnMint else Color.White
    Box(
        Modifier.size(52.dp).clip(CircleShape).background(back).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = desc, tint = tint, modifier = Modifier.size(24.dp))
    }
}
