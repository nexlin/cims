package com.cims.ue.ptt

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeOff
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material.icons.filled.HearingDisabled
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.csc.GroupSummary
import com.cims.ue.ptt.floor.FloorState
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow

class MainActivity : ComponentActivity() {
    private var svc by mutableStateOf<PttService?>(null)
    private val conn = object : ServiceConnection {
        override fun onServiceConnected(n: ComponentName?, b: IBinder?) { svc = (b as? PttService.LocalBinder)?.service }
        override fun onServiceDisconnected(n: ComponentName?) { svc = null }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        HwPtt.init(this)
        PttService.start(this)
        bindService(Intent(this, PttService::class.java), conn, Context.BIND_AUTO_CREATE)
        setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) { PttScreen(svc, onStopSip = { svc?.stopSip(); svc = null }) } } }
    }

    override fun onDestroy() {
        runCatching { unbindService(conn) }
        super.onDestroy()
    }

    /** 하드웨어 PTT 키(측면 버튼) — down=발언 요청, up=해제. 화면 버튼과 동일 경로.
     *  SOS(2번째 측면 키)=긴급 개시 — 해제는 오조작 방지 위해 화면 배너에서만. */
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        // 키코드 실측용 (persist.log.tag=I 단말이 있어 Log.i)
        if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0)
            android.util.Log.i("HwPtt", "keyDown code=${event.keyCode} scan=${event.scanCode} dev=${event.deviceId}")
        when (HwPtt.classify(event.keyCode, event.scanCode)) {
            HwPtt.Kind.SOS -> {
                if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                    svc?.controller?.startEmergency()
                return true
            }
            HwPtt.Kind.PTT -> {
                HwPtt.markSeen(this)
                when (event.action) {
                    android.view.KeyEvent.ACTION_DOWN -> if (event.repeatCount == 0) svc?.controller?.pttDown()
                    android.view.KeyEvent.ACTION_UP -> svc?.controller?.pttUp()
                }
                return true
            }
            HwPtt.Kind.NONE -> Unit
        }
        return super.dispatchKeyEvent(event)
    }
}

// PTT 상태 색상 언어(제안서 §3): IDLE=파랑, REQUESTING=주황, SPEAKING=초록, LISTENING=회색
private val ColEmergency = Color(0xFFC62828)
private val ColIdle = Color(0xFF1565C0)
private val ColRequest = Color(0xFFF9A825)
private val ColSpeak = Color(0xFF2E7D32)
private val ColListen = Color(0xFF757575)
private val ColDisabled = Color(0xFFB0BEC5)
private val ColChipBg = Color(0xFFD7E6F7)

@Composable
private fun PttScreen(svc: PttService?, onStopSip: () -> Unit) {
    val context = LocalContext.current
    val perm = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        svc?.ensureRegistered()
    }

    // controllerFlow — 서비스 바인드 후 늦게 생성되는 컨트롤러(SSO 재취득)도 재구성으로 반영
    val ctl: PttController? = if (svc != null) svc.controllerFlow.collectAsState().value else null
    val fbReg = remember { MutableStateFlow<RegState>(RegState.Idle) }
    val fbFloor = remember { MutableStateFlow(FloorState.IDLE) }
    val fbSpeaker = remember { MutableStateFlow<Speaker?>(null) }
    val fbStatus = remember { MutableStateFlow("서비스 연결 중…") }
    val fbGroups = remember { MutableStateFlow<List<GroupSummary>>(emptyList()) }
    val fbSel = remember { MutableStateFlow<String?>(null) }
    val fbAffil = remember { MutableStateFlow<Set<String>>(emptySet()) }
    val fbSessions = remember { MutableStateFlow<List<GroupCallState>>(emptyList()) }
    val fbPolicy = remember { MutableStateFlow(ListenPolicy.ALL) }
    val fbRoute = remember { MutableStateFlow(SipController.AUDIO_ROUTE_DEFAULT) }

    val reg by (ctl?.regState ?: fbReg).collectAsState()
    val floor by (ctl?.floorState ?: fbFloor).collectAsState()
    val speaker by (ctl?.speaker ?: fbSpeaker).collectAsState()
    val status by (ctl?.status ?: fbStatus).collectAsState()
    val groups by (ctl?.groups ?: fbGroups).collectAsState()
    val selGroup by (ctl?.selectedGroup ?: fbSel).collectAsState()
    val affiliated by (ctl?.affiliated ?: fbAffil).collectAsState()
    val sessions by (ctl?.sessions ?: fbSessions).collectAsState()
    val policy by (ctl?.listenPolicy ?: fbPolicy).collectAsState()
    val route by (ctl?.audioRoute ?: fbRoute).collectAsState()

    val hasAccount = remember { com.cims.ue.core.account.SsoProvisioner.hasAccount(context) }
    val inCall = sessions.any { it.active || it.callId >= 0 }

    // SSO: 컨트롤러 연결 시 CIMS 공유 계정의 MCPTT(TS 33.180) 토큰을 주입(별도 로그인 없음).
    LaunchedEffect(ctl) {
        val c = ctl ?: return@LaunchedEffect
        if (!com.cims.ue.core.account.SsoProvisioner.hasAccount(context)) return@LaunchedEffect
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            val am = android.accounts.AccountManager.get(context)
            val acct = com.cims.ue.core.account.CimsAccounts.get(am) ?: return@withContext
            val tok = com.cims.ue.core.account.CimsAccounts.blockingToken(
                am, acct, com.cims.ue.core.account.CimsAccounts.TOKEN_MCPTT)
            if (tok != null) c.setAccessToken(tok)
        }
    }

    Box(Modifier.fillMaxSize()) {
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        // ── 상태바: 등록 상태 + 등록/해제 ──
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            val (dotColor, regText) = when (reg) {
                RegState.Idle -> ColListen to "대기"
                RegState.Registering -> ColRequest to "등록 중…"
                is RegState.Registered -> ColSpeak to "등록됨"
                RegState.Unregistered -> ColListen to "미등록"
                is RegState.Failed -> Color(0xFFC62828) to "등록 실패"
            }
            Box(Modifier.size(10.dp).clip(CircleShape).background(dotColor))
            Spacer(Modifier.size(8.dp))
            Text(regText, style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.weight(1f))
            if (reg is RegState.Registered) {
                TextButton(onClick = onStopSip) { Text("해제") }
            } else {
                Button(onClick = { perm.launch(perms()) }) { Text("등록") }
            }
        }

        // ── 긴급 배너 — 긴급 상태 그룹이 있으면 최상단 붉은 배너(개시자에게만 해제 버튼) ──
        sessions.firstOrNull { it.emergency }?.let { e -> EmergencyBanner(e, ctl) }

        // ── 계정(CIMS SSO) 미로그인 안내 ──
        if (!hasAccount) {
            Card(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("CIMS 앱에서 먼저 로그인하세요.", style = MaterialTheme.typography.bodySmall)
                    OutlinedButton(onClick = {
                        val act = context as? android.app.Activity
                        android.accounts.AccountManager.get(context).addAccount(
                            com.cims.ue.core.account.CimsAccounts.ACCOUNT_TYPE,
                            com.cims.ue.core.account.CimsAccounts.TOKEN_MCPTT,
                            null, null, act, null, null)
                    }) { Text("CIMS 로그인 열기") }
                }
            }
        }

        // ── 그룹 목록 — 그룹별 참여/채널지정/나가기 (멀티그룹 동시 참여) ──
        Card(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("그룹", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                    TextButton(onClick = { ctl?.loadGroups() }) { Text("새로고침") }
                }
                Column(
                    Modifier.heightIn(max = 300.dp).verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    groups.forEach { g ->
                        val id = PttController.bareId(g.uri)
                        GroupRow(
                            id = id,
                            name = g.displayName ?: id,
                            memberCount = g.memberCount,
                            session = sessions.firstOrNull { it.groupId == id },
                            selected = id == selGroup,
                            affiliated = affiliated.contains(id),
                            ctl = ctl,
                        )
                    }
                }
                // 주채널 참가자
                sessions.firstOrNull { it.role == ChannelRole.PRIMARY }?.let { p ->
                    if (p.participants.isNotEmpty()) {
                        Text("참가자 ${p.participants.size} (${p.groupId})",
                            style = MaterialTheme.typography.bodySmall, color = ColListen)
                        Row(
                            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                        ) {
                            val speakerId = p.speaker?.let { PttController.bareId(it.id) }
                            p.participants.entries.sortedBy { it.key }.forEach { (pid, st) ->
                                val speaking = pid == speakerId
                                Text(
                                    text = if (speaking) "🔊 $pid" else pid,
                                    style = MaterialTheme.typography.labelMedium,
                                    color = when {
                                        speaking -> Color.White
                                        st.equals("pending", true) -> ColListen
                                        else -> Color(0xFF1A1D2E)
                                    },
                                    modifier = Modifier
                                        .clip(RoundedCornerShape(50))
                                        .background(
                                            when {
                                                speaking -> ColSpeak
                                                st.equals("pending", true) -> Color(0xFFEEEEEE)
                                                else -> ColChipBg
                                            },
                                        )
                                        .padding(horizontal = 10.dp, vertical = 4.dp),
                                )
                            }
                        }
                    }
                }
            }
        }

        // ── 발언자 카드 (중앙) ──
        Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
            SpeakerCard(speaker, floor, inCall)
        }

        // ── PTT 버튼 (하단) — 하드웨어 PTT 버튼 단말은 화면 버튼을 숨기고 안내만 ──
        val hwPtt by HwPtt.present.collectAsState()
        if (hwPtt) {
            Text("측면 PTT 버튼을 눌러 발언", style = MaterialTheme.typography.titleMedium, color = ColListen,
                modifier = Modifier.fillMaxWidth().padding(vertical = 16.dp), textAlign = TextAlign.Center)
        } else {
            Box(Modifier.fillMaxWidth().padding(vertical = 12.dp), contentAlignment = Alignment.Center) {
                PttButton(floor = floor, enabled = inCall,
                    onDown = { ctl?.pttDown() }, onUp = { ctl?.pttUp() })
            }
        }
        Text(status, style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
    }

    // ── 우하단 세로 아이콘 열(오버레이) — 스피커폰 토글 · 듣기 정책 토글 ──
    Column(
        Modifier.align(Alignment.BottomEnd).padding(end = 14.dp, bottom = 56.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        // SOS — 길게 눌러 긴급 개시(오발동 방지). 해제는 상단 배너에서.
        SosButton(active = sessions.any { it.emergency }, onActivate = { ctl?.startEmergency() })
        val speakerOn = route == SipController.AUDIO_ROUTE_SPEAKER
        // 스피커폰: on=외장 스피커 / off=일반(수화구·이어폰 자동)
        OverlayIconButton(
            icon = if (speakerOn) Icons.AutoMirrored.Filled.VolumeUp else Icons.AutoMirrored.Filled.VolumeOff,
            label = "스피커",
            active = speakerOn,
            activeBg = ColSpeak,
        ) {
            ctl?.setAudioRoute(
                if (speakerOn) SipController.AUDIO_ROUTE_DEFAULT else SipController.AUDIO_ROUTE_SPEAKER,
            )
        }
        // 듣기 정책: 전체듣기 ↔ 주·부채널만
        val all = policy == ListenPolicy.ALL
        OverlayIconButton(
            icon = if (all) Icons.Filled.Hearing else Icons.Filled.HearingDisabled,
            label = if (all) "전체듣기" else "주·부만",
            active = all,
            activeBg = ColIdle,
        ) {
            ctl?.setListenPolicy(if (all) ListenPolicy.CHANNELS_ONLY else ListenPolicy.ALL)
        }
    }
    }
}

/** 긴급 상태 배너 — 깜빡이는 붉은 배경 + 그룹/개시자, 개시자에게만 [해제] (서버도 개시자 취소만 수용). */
@Composable
private fun EmergencyBanner(e: GroupCallState, ctl: PttController?) {
    val blink = rememberInfiniteTransition(label = "emgBlink")
    val a by blink.animateFloat(
        initialValue = 1f, targetValue = 0.55f,
        animationSpec = infiniteRepeatable(tween(600, easing = LinearEasing), RepeatMode.Reverse),
        label = "emgAlpha",
    )
    Row(
        Modifier.fillMaxWidth().padding(vertical = 6.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(ColEmergency.copy(alpha = a))
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text("🚨 긴급 — ${e.groupId}", color = Color.White,
                style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
            Text(if (e.emergencyMine) "내가 개시 — 상황 종료 시 해제하세요" else "긴급 상황 수신 중",
                color = Color.White, style = MaterialTheme.typography.bodySmall)
        }
        if (e.emergencyMine) {
            TextButton(onClick = { ctl?.cancelEmergency() }) {
                Text("해제", color = Color.White, fontWeight = FontWeight.Bold)
            }
        }
    }
}

/** SOS 버튼(우하단 오버레이) — 오발동 방지 위해 **길게 눌러** 긴급 개시. 활성 시 붉은 점멸. */
@Composable
private fun SosButton(active: Boolean, onActivate: () -> Unit) {
    val blink = rememberInfiniteTransition(label = "sosBlink")
    val a by blink.animateFloat(
        initialValue = 1f, targetValue = 0.5f,
        animationSpec = infiniteRepeatable(tween(600, easing = LinearEasing), RepeatMode.Reverse),
        label = "sosAlpha",
    )
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(52.dp)
                .clip(CircleShape)
                .background(if (active) ColEmergency.copy(alpha = a) else Color(0xFFF2DBDB))
                .pointerInput(active) {
                    detectTapGestures(onLongPress = { if (!active) onActivate() })
                },
        ) {
            Text("SOS", color = if (active) Color.White else ColEmergency,
                style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Bold)
        }
        Text(if (active) "긴급 중" else "긴급(길게)", style = MaterialTheme.typography.labelSmall,
            color = ColEmergency, fontWeight = FontWeight.Bold)
    }
}

/** 우하단 오버레이 아이콘 버튼(원형 52dp + 하단 소형 라벨) — CIMS-Phone 통화 버튼과 동일 스타일. */
@Composable
private fun OverlayIconButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    label: String,
    active: Boolean,
    activeBg: Color,
    onClick: () -> Unit,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(52.dp)
                .clip(CircleShape)
                .background(if (active) activeBg else Color(0xFFE3E3E8))
                .clickable(onClick = onClick),
        ) {
            Icon(icon, contentDescription = label, modifier = Modifier.size(26.dp),
                tint = if (active) Color.White else Color(0xFF3A3D4E))
        }
        Text(label, style = MaterialTheme.typography.labelSmall,
            color = if (active) activeBg else ColListen, fontWeight = FontWeight.Bold)
    }
}

/** 그룹 행 — 참여 상태·발언자·[주]/[부] 지정·참여/나가기. */
@Composable
private fun GroupRow(
    id: String,
    name: String,
    memberCount: Int?,
    session: GroupCallState?,
    selected: Boolean,
    affiliated: Boolean,
    ctl: PttController?,
) {
    val joined = session != null
    Row(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(
                when {
                    session?.emergency == true -> Color(0xFFFBE4E4)
                    session?.role == ChannelRole.PRIMARY -> Color(0xFFE0F1E1)
                    joined -> Color(0xFFEFF4FA)
                    selected -> ColChipBg
                    else -> Color.Transparent
                },
            )
            .clickable { ctl?.selectGroup(id) }
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(name, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
                when (session?.role) {
                    ChannelRole.PRIMARY -> Badge("주", ColSpeak, Color.White)
                    ChannelRole.SECONDARY -> Badge("부", Color(0xFF1565C0), Color.White)
                    else -> Unit
                }
                if (session?.emergency == true) Badge("긴급", ColEmergency, Color.White)
                if (joined && session?.audible == false) Badge("음소거", Color(0xFF9E9E9E), Color.White)
                if (!joined && affiliated) Badge("가입", Color(0xFFDCEDC8), Color(0xFF33691E))
            }
            val sub = when {
                session?.speaker != null -> "🔊 ${displayId(session.speaker.id)} 발언 중"
                joined && session?.active == true -> "$id · 통화 중"
                joined -> "$id · 연결 중…"
                else -> "$id · ${memberCount ?: "?"}명"
            }
            Text(sub, style = MaterialTheme.typography.bodySmall,
                color = if (session?.speaker != null) Color(0xFFE65100) else ColListen)
        }
        if (joined) {
            if (session?.role != ChannelRole.PRIMARY) {
                TextButton(onClick = { ctl?.setPrimary(id) }) { Text("주") }
                TextButton(onClick = { ctl?.toggleSecondary(id) }) {
                    Text(if (session?.role == ChannelRole.SECONDARY) "부해제" else "부")
                }
            }
            IconButton(onClick = { ctl?.leaveGroup(id) }) {
                Icon(Icons.Filled.CallEnd, contentDescription = "나가기", tint = Color(0xFFD32F2F))
            }
        } else {
            Button(onClick = { ctl?.joinGroupCall(id) }) { Text("참여") }
        }
    }
}

@Composable
private fun Badge(text: String, bg: Color, fg: Color) {
    Text(text, style = MaterialTheme.typography.labelSmall, color = fg,
        modifier = Modifier.clip(RoundedCornerShape(50)).background(bg).padding(horizontal = 8.dp, vertical = 2.dp))
}

/** 현재 발언자 표시 — 이름/경과시간, 내 발언=초록·타인=주황, 주채널 밖 발언은 [그룹] 태그. */
@Composable
private fun SpeakerCard(speaker: Speaker?, floor: FloorState, inCall: Boolean) {
    // 발언 경과시간 1초 틱
    var nowMs by remember { mutableLongStateOf(0L) }
    LaunchedEffect(speaker) {
        while (speaker != null) {
            nowMs = SystemClock.elapsedRealtime()
            delay(1000)
        }
    }

    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        when {
            speaker != null -> {
                val elapsed = ((nowMs - speaker.sinceMs).coerceAtLeast(0L)) / 1000
                val tag = speaker.groupId?.let { "[$it] " } ?: ""
                Text(if (speaker.self) "내가 발언 중" else "$tag${displayId(speaker.id)} 발언 중",
                    style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold,
                    color = if (speaker.self) ColSpeak else Color(0xFFE65100))
                Text("⏱ %d:%02d".format(elapsed / 60, elapsed % 60), style = MaterialTheme.typography.titleMedium)
            }
            floor == FloorState.REQUESTING -> Text("발언권 요청 중…",
                style = MaterialTheme.typography.headlineSmall, color = ColRequest)
            inCall -> Text("대기 중 — 버튼을 눌러 발언",
                style = MaterialTheme.typography.titleMedium, color = ColListen)
            else -> Text("그룹콜 없음", style = MaterialTheme.typography.titleMedium, color = ColListen)
        }
    }
}

/**
 * 원형 대형 PTT 버튼 — 누르는 동안 발언(floor request), 떼면 release.
 * 상태 색: IDLE=파랑 / REQUESTING=주황 / SPEAKING=초록+펄스 링 / LISTENING=회색(누름 무시).
 */
@Composable
private fun PttButton(floor: FloorState, enabled: Boolean, onDown: () -> Unit, onUp: () -> Unit) {
    val speaking = floor == FloorState.SPEAKING
    val color = when {
        !enabled -> ColDisabled
        speaking -> ColSpeak
        floor == FloorState.REQUESTING -> ColRequest
        floor == FloorState.LISTENING -> ColListen
        else -> ColIdle
    }
    val label = when {
        !enabled -> "PTT"
        speaking -> "발언 중\n(떼면 종료)"
        floor == FloorState.REQUESTING -> "요청 중…"
        floor == FloorState.LISTENING -> "수신 중"
        else -> "눌러서\n말하기"
    }

    // 발언 중 펄스 링 — 버튼 밖으로 퍼져나가는 원
    val pulse = rememberInfiniteTransition(label = "pulse")
    val ring by pulse.animateFloat(
        initialValue = 0f, targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(1200, easing = LinearEasing), RepeatMode.Restart),
        label = "ring",
    )

    Box(
        contentAlignment = Alignment.Center,
        modifier = Modifier
            .size(190.dp)
            .drawBehind {
                if (speaking) {
                    val r = size.minDimension / 2f * (1f + 0.25f * ring)
                    drawCircle(color = ColSpeak.copy(alpha = 0.4f * (1f - ring)), radius = r)
                }
            }
            .clip(CircleShape)
            .background(color)
            .pointerInput(enabled) {
                if (enabled) detectTapGestures(onPress = {
                    onDown()
                    try { awaitRelease() } finally { onUp() }
                })
            },
    ) {
        Text(label, color = Color.White, textAlign = TextAlign.Center,
            style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
    }
}

/** MCPTT ID 표시용 축약 — "tel:+8213..."/"sip:+8213...@domain" → 번호부. */
private fun displayId(id: String): String = PttController.bareId(id)

private fun perms(): Array<String> = buildList {
    add(Manifest.permission.RECORD_AUDIO)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.POST_NOTIFICATIONS)
}.toTypedArray()
