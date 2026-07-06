package com.cims.ue.ptt.ui

import android.content.Context
import android.media.AudioManager
import android.os.SystemClock
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.GroupCallState
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.ListenPolicy
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.R
import com.cims.ue.ptt.floor.FloorState
import kotlinx.coroutines.delay

/** 주채널 화면(시안 `주부채널화면.png`) — 주채널 카드(발언 상태+PTT) + 부채널 카드들. */
@Composable
fun MainChannelScreen(
    st: PttUiState,
    onOpenThread: (String) -> Unit,
    onOpenChannels: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        val (regColor, regText) = when (st.reg) {
            is RegState.Registered -> Ct.Mint to "접속"
            RegState.Registering -> Ct.Amber to "연결 중"
            is RegState.Failed -> Ct.Red to "등록 실패"
            else -> Ct.TextFaint to "미접속"
        }
        ScreenHeader(
            label = st.ctl?.mcpttId?.let { PttController.bareId(it) },
            title = st.primary?.let { st.groupName(it.groupId) } ?: "주채널",
            trailing = {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        StatusDot(regColor)
                        Text(regText, color = regColor, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                    }
                    Box(
                        Modifier.size(36.dp).clip(RoundedCornerShape(10.dp))
                            .background(Ct.SurfaceHi).clickable(onClick = onOpenSettings),
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(painterResource(R.drawable.ic_nav_settings), contentDescription = "설정",
                            tint = Ct.TextDim, modifier = Modifier.size(18.dp))
                    }
                }
            },
        )

        val primary = st.primary
        if (primary == null) {
            SectionCard {
                Column(Modifier.fillMaxWidth().padding(vertical = 18.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Icon(painterResource(R.drawable.ic_connected), contentDescription = null,
                        tint = Ct.TextFaint, modifier = Modifier.size(40.dp))
                    Text("참여 중인 채널이 없습니다", color = Ct.TextDim, fontSize = 14.sp)
                    MintButton("전체채널에서 선택", Modifier.fillMaxWidth(0.7f), onClick = onOpenChannels)
                }
            }
        } else {
            PrimaryChannelCard(st, primary, onOpenThread)
        }

        st.secondaries.forEach { s ->
            SecondaryChannelCard(st, s, onOpenThread)
        }

        // 채널 외 참여 그룹(일반) — 간단 행으로 노출
        val others = st.sessions.filter {
            it.role == com.cims.ue.ptt.ChannelRole.NONE
        }
        if (others.isNotEmpty()) {
            SectionLabel("모니터링 중 (${others.size})")
            others.forEach { s ->
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Ct.Surface)
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(st.groupName(s.groupId), color = Ct.Text, fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                    if (!s.audible) PillBadge("음소거", Ct.Gray)
                    Spacer(Modifier.width(8.dp))
                    Text("주채널 설정", color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                        modifier = Modifier.clickable { st.ctl?.setPrimary(s.groupId) })
                }
            }
        }

        // 스피커 출력/듣기 정책 — 시안 하단 여백 자리에 컨트롤 행
        AudioControlRow(st)

        Text(st.status, color = Ct.TextFaint, fontSize = 11.sp,
            modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp))
    }
}

/** 주채널 카드 — 배지·태그·송신자 표시·수신 음량·PTT 버튼(+메시지). */
@Composable
private fun PrimaryChannelCard(st: PttUiState, s: GroupCallState, onOpenThread: (String) -> Unit) {
    SectionCard {
        SectionLabel("주채널")
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            RoleBadge("주", if (s.emergency) Ct.Red else Ct.Mint, if (s.emergency) Ct.RedDim else Ct.MintDim)
            Column(Modifier.weight(1f)) {
                Text(st.groupName(s.groupId), color = Ct.Text, fontSize = 17.sp, fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(5.dp), modifier = Modifier.padding(top = 4.dp)) {
                    TagChip("음성", R.drawable.ic_voice)
                    TagChip("구성원 (${s.participants.size})")
                    if (s.emergency) PillBadge("긴급", Ct.Red)
                }
            }
            SpeakingIndicator(s)
        }

        Spacer(Modifier.height(14.dp))
        SpeakerStatusPanel(st, s)
        Spacer(Modifier.height(14.dp))

        VolumeRow()
        Spacer(Modifier.height(12.dp))

        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            val hwPtt by HwPtt.present.collectAsState()
            if (hwPtt) {
                Box(
                    Modifier.weight(1f).height(64.dp).clip(RoundedCornerShape(14.dp))
                        .background(Ct.MintDim),
                    contentAlignment = Alignment.Center,
                ) {
                    Text("측면 PTT 버튼을 눌러 발언", color = Ct.Mint, fontSize = 14.sp, fontWeight = FontWeight.Bold)
                }
            } else {
                PttBar(floor = st.floor, enabled = st.inCall, modifier = Modifier.weight(1f),
                    onDown = { st.ctl?.pttDown() }, onUp = { st.ctl?.pttUp() })
            }
            MessageSquareButton { onOpenThread(s.groupId) }
        }
    }
}

/** 부채널 카드 — 이름·송신자·[주채널 설정]. */
@Composable
private fun SecondaryChannelCard(st: PttUiState, s: GroupCallState, onOpenThread: (String) -> Unit) {
    SectionCard {
        SectionLabel("부채널")
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            RoleBadge("부", Ct.Amber, Ct.AmberDim)
            Column(Modifier.weight(1f)) {
                Text(st.groupName(s.groupId), color = Ct.Text, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(5.dp), modifier = Modifier.padding(top = 4.dp)) {
                    TagChip("음성", R.drawable.ic_voice)
                    TagChip("구성원 (${s.participants.size})")
                    if (s.emergency) PillBadge("긴급", Ct.Red)
                }
            }
            SpeakingIndicator(s)
        }
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            GhostButton("주채널 설정", Modifier.weight(1f), color = Ct.Mint) { st.ctl?.setPrimary(s.groupId) }
            MessageSquareButton { onOpenThread(s.groupId) }
        }
    }
}

/** 카드 우상단 "▶ ○○ 송신" 표시. */
@Composable
private fun SpeakingIndicator(s: GroupCallState) {
    val sp = s.speaker ?: return
    val name = if (sp.self) "나" else PttController.bareId(sp.id)
    val color = if (sp.self) Ct.Mint else Ct.Amber
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        Icon(painterResource(R.drawable.ic_voice), contentDescription = null,
            tint = color, modifier = Modifier.size(13.dp))
        Text("$name 송신", color = color, fontSize = 12.sp, fontWeight = FontWeight.Bold)
    }
}

/** 카드 중앙 상태 패널 — 시안의 영상 영역 자리(음성 전용: 발언자/경과시간/상태). */
@Composable
private fun SpeakerStatusPanel(st: PttUiState, s: GroupCallState) {
    var nowMs by remember { mutableLongStateOf(0L) }
    val sp = s.speaker
    LaunchedEffect(sp) {
        while (sp != null) {
            nowMs = SystemClock.elapsedRealtime()
            delay(1000)
        }
    }
    Box(
        Modifier.fillMaxWidth().height(96.dp).clip(RoundedCornerShape(12.dp)).background(Ct.Bg),
        contentAlignment = Alignment.Center,
    ) {
        when {
            sp != null -> {
                val elapsed = ((nowMs - sp.sinceMs).coerceAtLeast(0L)) / 1000
                Column(horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        if (sp.self) "내가 발언 중" else "${PttController.bareId(sp.id)} 발언 중",
                        color = if (sp.self) Ct.Mint else Ct.Amber,
                        fontSize = 17.sp, fontWeight = FontWeight.Bold,
                    )
                    Text("%d:%02d".format(elapsed / 60, elapsed % 60), color = Ct.TextDim, fontSize = 13.sp)
                }
            }
            st.floor == FloorState.REQUESTING -> Text("발언권 요청 중…", color = Ct.Amber,
                fontSize = 15.sp, fontWeight = FontWeight.Bold)
            s.active -> Text("대기 중 — 버튼을 눌러 발언", color = Ct.TextFaint, fontSize = 13.sp)
            else -> Text("연결 중…", color = Ct.TextFaint, fontSize = 13.sp)
        }
    }
}

/** 수신 음량 행 — 레벨미터 아이콘 + 슬라이더(통화 스트림 볼륨, 시안의 음량 슬라이더). */
@Composable
private fun VolumeRow() {
    val context = LocalContext.current
    val am = remember { context.getSystemService(Context.AUDIO_SERVICE) as AudioManager }
    val max = remember { am.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL).coerceAtLeast(1) }
    var vol by remember { mutableFloatStateOf(am.getStreamVolume(AudioManager.STREAM_VOICE_CALL).toFloat()) }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Icon(painterResource(R.drawable.ic_level_meter), contentDescription = "수신 음량",
            tint = Ct.Mint, modifier = Modifier.size(18.dp))
        Slider(
            value = vol, onValueChange = {
                vol = it
                am.setStreamVolume(AudioManager.STREAM_VOICE_CALL, it.toInt(), 0)
            },
            valueRange = 0f..max.toFloat(), steps = (max - 1).coerceAtLeast(0),
            modifier = Modifier.weight(1f).height(24.dp),
            colors = SliderDefaults.colors(
                thumbColor = Ct.Mint, activeTrackColor = Ct.Mint,
                inactiveTrackColor = Ct.SurfaceHi, activeTickColor = Color.Transparent,
                inactiveTickColor = Color.Transparent,
            ),
        )
    }
}

/** 가로형 대형 PTT 바(시안의 마이크 버튼) — 누르는 동안 발언, 떼면 해제. */
@Composable
private fun PttBar(floor: FloorState, enabled: Boolean, modifier: Modifier = Modifier,
                   onDown: () -> Unit, onUp: () -> Unit) {
    val speaking = floor == FloorState.SPEAKING
    val bg = when {
        !enabled -> Ct.GrayDim
        speaking -> Ct.Mint
        floor == FloorState.REQUESTING -> Ct.Amber
        floor == FloorState.LISTENING -> Ct.SurfaceHi
        else -> Ct.Mint
    }
    val fg = when {
        !enabled -> Ct.TextFaint
        speaking || floor == FloorState.REQUESTING || floor == FloorState.IDLE ||
            floor == FloorState.QUEUED -> Ct.OnMint
        else -> Ct.TextFaint
    }
    val pulse = rememberInfiniteTransition(label = "pttPulse")
    val ring by pulse.animateFloat(
        initialValue = 0f, targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(1100, easing = LinearEasing), RepeatMode.Restart),
        label = "pttRing",
    )
    Box(
        modifier
            .height(64.dp)
            .drawBehind {
                if (speaking) {
                    drawRoundRect(
                        color = Ct.Mint.copy(alpha = 0.35f * (1f - ring)),
                        cornerRadius = androidx.compose.ui.geometry.CornerRadius(18.dp.toPx() * (1 + ring)),
                        size = androidx.compose.ui.geometry.Size(
                            size.width * (1f + 0.05f * ring), size.height * (1f + 0.28f * ring)),
                        topLeft = androidx.compose.ui.geometry.Offset(
                            -size.width * 0.025f * ring, -size.height * 0.14f * ring),
                    )
                }
            }
            .clip(RoundedCornerShape(14.dp))
            .background(bg)
            .pointerInput(enabled) {
                if (enabled) detectTapGestures(onPress = {
                    onDown()
                    try { awaitRelease() } finally { onUp() }
                })
            },
        contentAlignment = Alignment.Center,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Icon(painterResource(R.drawable.ic_ptt), contentDescription = "PTT",
                tint = fg, modifier = Modifier.size(24.dp))
            val label = when {
                !enabled -> "채널 없음"
                speaking -> "발언 중 — 떼면 종료"
                floor == FloorState.REQUESTING -> "요청 중…"
                floor == FloorState.LISTENING -> "수신 중"
                else -> "눌러서 말하기"
            }
            Text(label, color = fg, fontSize = 14.sp, fontWeight = FontWeight.Bold)
        }
    }
}

/** PTT 바 우측 정사각 메시지 버튼(시안). */
@Composable
private fun MessageSquareButton(onClick: () -> Unit) {
    Box(
        Modifier.size(width = 56.dp, height = 50.dp).clip(RoundedCornerShape(14.dp))
            .background(Ct.SurfaceHi).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(painterResource(R.drawable.ic_message), contentDescription = "메시지",
            tint = Ct.Mint, modifier = Modifier.size(20.dp))
    }
}

/** 스피커 출력·듣기 정책·SOS — 기존 기능 유지(시안 톤으로 재구성). */
@Composable
private fun AudioControlRow(st: PttUiState) {
    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        val speakerOn = st.route == SipController.AUDIO_ROUTE_SPEAKER
        ControlToggle(
            icon = if (speakerOn) R.drawable.ic_volume_on else R.drawable.ic_volume_off,
            label = "스피커", active = speakerOn, modifier = Modifier.weight(1f),
        ) {
            st.ctl?.setAudioRoute(
                if (speakerOn) SipController.AUDIO_ROUTE_DEFAULT else SipController.AUDIO_ROUTE_SPEAKER)
        }
        val all = st.policy == ListenPolicy.ALL
        ControlToggle(
            icon = R.drawable.ic_connected,
            label = if (all) "전체듣기" else "주·부만", active = all, modifier = Modifier.weight(1f),
        ) {
            st.ctl?.setListenPolicy(if (all) ListenPolicy.CHANNELS_ONLY else ListenPolicy.ALL)
        }
        SosControl(active = st.emergencySession != null, modifier = Modifier.weight(1f)) {
            st.ctl?.startEmergency()
        }
    }
}

@Composable
private fun ControlToggle(icon: Int, label: String, active: Boolean,
                          modifier: Modifier = Modifier, onClick: () -> Unit) {
    Column(
        modifier.clip(RoundedCornerShape(12.dp))
            .background(if (active) Ct.MintDim else Ct.Surface)
            .clickable(onClick = onClick)
            .padding(vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Icon(painterResource(icon), contentDescription = label,
            tint = if (active) Ct.Mint else Ct.TextFaint, modifier = Modifier.size(20.dp))
        Text(label, color = if (active) Ct.Mint else Ct.TextFaint, fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold)
    }
}

/** SOS — 오발동 방지 위해 길게 눌러 긴급 개시(해제는 상단 배너, 개시자만). */
@Composable
private fun SosControl(active: Boolean, modifier: Modifier = Modifier, onActivate: () -> Unit) {
    val blink = rememberInfiniteTransition(label = "sosBlink")
    val a by blink.animateFloat(
        initialValue = 1f, targetValue = 0.55f,
        animationSpec = infiniteRepeatable(tween(600, easing = LinearEasing), RepeatMode.Reverse),
        label = "sosAlpha",
    )
    Column(
        modifier.clip(RoundedCornerShape(12.dp))
            .background(if (active) Ct.Red.copy(alpha = a) else Ct.RedDim)
            .pointerInput(active) { detectTapGestures(onLongPress = { if (!active) onActivate() }) }
            .padding(vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Icon(painterResource(R.drawable.ic_warning), contentDescription = "SOS",
            tint = if (active) Color.White else Ct.Red, modifier = Modifier.size(20.dp))
        Text(if (active) "긴급 중" else "SOS (길게)", color = if (active) Color.White else Ct.Red,
            fontSize = 11.sp, fontWeight = FontWeight.Bold)
    }
}
