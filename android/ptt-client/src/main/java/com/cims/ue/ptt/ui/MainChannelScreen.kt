package com.cims.ue.ptt.ui

import android.os.SystemClock
import android.text.format.DateFormat
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
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
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
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.core.message.MsgDirection
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.ChannelRole
import com.cims.ue.ptt.GroupCallState
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.ListenPolicy
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.PttService
import com.cims.ue.ptt.R
import com.cims.ue.ptt.floor.FloorIndicator
import com.cims.ue.ptt.floor.FloorState
import kotlinx.coroutines.delay
import java.util.Date

/** 주채널 화면 — 카드 없이 전면 배치: 채널 정보/발언 상태/영상 + 하단 인라인 채팅.
 *  주채널 외 참여 채널은 전체채널 탭에서 확인. [주채널 선택] → 시트에서 즉시 지정.
 *  수신 음량은 채널 상세 화면에서, SOS 는 하드웨어 버튼으로 조작. */
@Composable
fun MainChannelScreen(
    st: PttUiState,
    svc: PttService?,
    onOpenThread: (String) -> Unit,
) {
    var picker by remember { mutableStateOf(false) }
    var routeSheet by remember { mutableStateOf(false) }

    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize().imePadding().padding(horizontal = 16.dp, vertical = 10.dp)) {
            val (regColor, regText) = when (st.reg) {
                is RegState.Registered -> Ct.Mint to "접속"
                RegState.Registering -> Ct.Amber to "연결 중"
                is RegState.Failed -> Ct.Red to "등록 실패"
                else -> Ct.TextFaint to "미접속"
            }
            ScreenHeader(
                label = st.ctl?.mcpttId?.let { PttController.fmtNumber(PttController.bareId(it)) },
                title = "주채널",
                trailing = {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        StatusDot(regColor)
                        Text(regText, color = regColor, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                    }
                },
            )
            Spacer(Modifier.height(10.dp))

            val primary = st.primary
            if (primary != null) {
                PrimaryChannelPanel(st, svc, primary, onOpenThread,
                    onSelect = { picker = true }, onRouteSelect = { routeSheet = true },
                    modifier = Modifier.weight(1f))
            } else {
                Column(Modifier.weight(1f).fillMaxWidth(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center) {
                    Icon(painterResource(R.drawable.ic_connected), contentDescription = null,
                        tint = Ct.TextFaint, modifier = Modifier.size(40.dp))
                    Spacer(Modifier.height(12.dp))
                    Text("참여 중인 채널이 없습니다", color = Ct.TextDim, fontSize = 14.sp)
                    Spacer(Modifier.height(14.dp))
                    MintButton("주채널 선택", Modifier.fillMaxWidth(0.6f)) { picker = true }
                }
            }

            Text(st.status, color = Ct.TextFaint, fontSize = 11.sp,
                modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp))
        }

        if (picker) ChannelSelectSheet(st, onDismiss = { picker = false })
        if (routeSheet) AudioRouteSheet(st, onDismiss = { routeSheet = false })
    }
}

/** 주채널 전면 패널(카드 없음) — 채널명/태그/발언 상태/영상(오버레이 컨트롤)/PTT(터치 단말만) + 하단 채팅. */
@Composable
private fun PrimaryChannelPanel(
    st: PttUiState,
    svc: PttService?,
    s: GroupCallState,
    onOpenThread: (String) -> Unit,
    onSelect: () -> Unit,
    onRouteSelect: () -> Unit,
    modifier: Modifier = Modifier,
) {
    // 그룹 문서(P우선순위 배지) — ETag 캐시라 재호출 저비용
    LaunchedEffect(s.groupId) { st.ctl?.loadGroupDetail(s.groupId) }

    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(st.groupName(s.groupId), color = Ct.Text, fontSize = 19.sp, fontWeight = FontWeight.Bold)
            st.groupDocs[s.groupId]?.priority?.let { PillBadge("P$it", Ct.Mint) }
            if (s.emergency) PillBadge("긴급", Ct.Red, filled = true)
            Spacer(Modifier.weight(1f))
            Text("주채널 선택", color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.clip(RoundedCornerShape(8.dp)).background(Ct.MintDim)
                    .clickable(onClick = onSelect)
                    .padding(horizontal = 10.dp, vertical = 5.dp))
        }
        Spacer(Modifier.height(6.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            TagChip("음성", R.drawable.ic_voice)
            TagChip("구성원 (${s.participants.size})")
            Spacer(Modifier.weight(1f))
            SpeakingIndicator(s)
        }
        Spacer(Modifier.height(10.dp))
        SpeakerStatusStrip(st, s)
        Spacer(Modifier.height(8.dp))
        VideoPanel(st, onRouteSelect)

        // 화면 PTT 바 — 터치 단말만(하드웨어 PTT 버튼 단말은 표시하지 않음)
        val hwPtt by HwPtt.present.collectAsState()
        if (!hwPtt) {
            Spacer(Modifier.height(8.dp))
            // Floor Taken 의 Permission=0(청취 전용 leg — broadcast 그룹·ambient)이면 버튼을 막는다
            // (TS 24.380 §8.2.3.7) — 눌러도 Deny 만 돌아온다.
            PttBar(floor = st.floor, enabled = st.inCall, listenOnly = !s.canRequestFloor,
                queuePosition = s.queuePosition, modifier = Modifier.fillMaxWidth(),
                onDown = { st.ctl?.pttDown() }, onUp = { st.ctl?.pttUp() })
        }
        Spacer(Modifier.height(10.dp))

        InlineChat(st, svc, s.groupId, onOpenThread, Modifier.weight(1f))
    }
}

/** 주채널 선택 시트 — 화면 이동 없이 그룹 리스트에서 즉시 지정.
 *  미참여 그룹이면 참여(joinGroupCall)부터 수행 후 주채널로. */
@Composable
private fun ChannelSelectSheet(st: PttUiState, onDismiss: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.55f))
        .pointerInput(Unit) { detectTapGestures { onDismiss() } }) {
        Column(
            Modifier.align(Alignment.BottomCenter).fillMaxWidth()
                .clip(RoundedCornerShape(topStart = 18.dp, topEnd = 18.dp))
                .background(Ct.Surface)
                .pointerInput(Unit) { detectTapGestures { } }   // 시트 내부 탭이 scrim 으로 새지 않게
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text("주채널 선택", color = Ct.Text, fontSize = 15.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(bottom = 8.dp))
            if (st.groups.isEmpty())
                Text("선택 가능한 채널이 없습니다", color = Ct.TextFaint, fontSize = 12.sp,
                    modifier = Modifier.padding(vertical = 10.dp))
            st.groups.forEach { g ->
                val gid = PttController.bareId(g.uri)
                val s = st.session(gid)
                val selected = s?.role == ChannelRole.PRIMARY
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp))
                        .background(if (selected) Ct.MintDim else Color.Transparent)
                        .clickable {
                            st.ctl?.let { c ->
                                if (s == null) c.joinGroupCall(gid)
                                c.setPrimary(gid)
                            }
                            onDismiss()
                        }
                        .padding(horizontal = 10.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(st.groupName(gid), color = if (selected) Ct.Mint else Ct.Text,
                        fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                    st.groupDocs[gid]?.priority?.let { PillBadge("P$it", Ct.Mint) }
                    when {
                        selected -> PillBadge("주채널", Ct.Red)
                        s != null -> PillBadge("참여 중", Ct.Gray)
                        else -> Text("미참여", color = Ct.TextFaint, fontSize = 11.sp)
                    }
                }
            }
        }
    }
}

/** 오디오 출력 선택 항목 — [AudioRouteSheet] 행. */
private data class RouteChoice(
    val label: String, val icon: Int, val route: Int, val deviceId: Int, val badge: String?)

/** 오디오 출력 선택 시트 — 이어폰 연결 시: 이어폰(장치별, 무선 다중 포함)/스피커폰/수화기. */
@Composable
private fun AudioRouteSheet(st: PttUiState, onDismiss: () -> Unit) {
    val choices = buildList {
        st.headsets.forEach { h ->
            add(RouteChoice(h.name, R.drawable.ic_headset, PttController.AUDIO_ROUTE_HEADSET, h.id,
                if (h.wireless) "무선" else "유선"))
        }
        add(RouteChoice("스피커폰", R.drawable.ic_volume_on, SipController.AUDIO_ROUTE_SPEAKER, -1, null))
        add(RouteChoice("수화기", R.drawable.ic_earpiece, SipController.AUDIO_ROUTE_EARPIECE, -1, null))
    }
    Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.55f))
        .pointerInput(Unit) { detectTapGestures { onDismiss() } }) {
        Column(
            Modifier.align(Alignment.BottomCenter).fillMaxWidth()
                .clip(RoundedCornerShape(topStart = 18.dp, topEnd = 18.dp))
                .background(Ct.Surface)
                .pointerInput(Unit) { detectTapGestures { } }
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text("오디오 출력", color = Ct.Text, fontSize = 15.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(bottom = 8.dp))
            choices.forEach { c ->
                val selected = st.route == c.route &&
                    (c.route != PttController.AUDIO_ROUTE_HEADSET || st.headsetId == c.deviceId)
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp))
                        .background(if (selected) Ct.MintDim else Color.Transparent)
                        .clickable {
                            st.ctl?.setAudioRoute(c.route, c.deviceId)
                            onDismiss()
                        }
                        .padding(horizontal = 10.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Icon(painterResource(c.icon), contentDescription = null,
                        tint = if (selected) Ct.Mint else Ct.TextDim, modifier = Modifier.size(17.dp))
                    Text(c.label, color = if (selected) Ct.Mint else Ct.Text,
                        fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                    c.badge?.let { PillBadge(it, Ct.Gray) }
                    if (selected) PillBadge("사용 중", Ct.Mint)
                }
            }
        }
    }
}

/** 우상단 "▶ ○○ 송신" 칩(시안 — 어두운 민트 면 위 민트 텍스트). */
@Composable
private fun SpeakingIndicator(s: GroupCallState) {
    val sp = s.speaker ?: return
    // 동시 발언이면 대표 화자 + "외 N" (전체 명단은 발언 상태 스트립에)
    val name = (if (sp.self) "나" else PttController.fmtNumber(PttController.bareId(sp.id))) +
        if (s.talkers.size > 1) " 외 ${s.talkers.size - 1}" else ""
    Row(
        Modifier.clip(RoundedCornerShape(6.dp)).background(Ct.MintDim)
            .padding(horizontal = 7.dp, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Icon(painterResource(R.drawable.ic_voice), contentDescription = null,
            tint = Ct.Mint, modifier = Modifier.size(12.dp))
        Text("$name 송신", color = Ct.Mint, fontSize = 11.sp, fontWeight = FontWeight.Bold)
    }
}

/** 발언 상태 스트립 — 슬림 한 줄(발언자/경과시간/상태). */
@Composable
private fun SpeakerStatusStrip(st: PttUiState, s: GroupCallState) {
    var nowMs by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    val sp = s.speaker
    LaunchedEffect(sp) {
        while (sp != null) {
            nowMs = SystemClock.elapsedRealtime()
            delay(1000)
        }
    }
    Row(
        Modifier.fillMaxWidth().height(44.dp).clip(RoundedCornerShape(10.dp)).background(Ct.Surface)
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        when {
            sp != null -> {
                val elapsed = ((nowMs - sp.sinceMs).coerceAtLeast(0L)) / 1000
                // 동시 발언(dual/multi, TS 24.380 §6.2.4.3.3) — 화자가 2명 이상이면 전원을 나열한다.
                val label = if (s.talkers.size > 1)
                    s.talkers.joinToString(", ") {
                        if (it.self) "나" else PttController.fmtNumber(PttController.bareId(it.id))
                    } +
                        // G-bit(dual floor)=상위 tier 가 선점 없이 끼어든 동시 발언 (§8.2.3.15)
                        if (s.floorIndicator and FloorIndicator.DUAL_FLOOR != 0) " 동시 발언(우선)"
                        else " 동시 발언"
                else if (sp.self) "내가 발언 중"
                else "${PttController.fmtNumber(PttController.bareId(sp.id))} 발언 중"
                Text(
                    label,
                    color = if (sp.self) Ct.Mint else Ct.Amber,
                    fontSize = if (s.talkers.size > 1) 13.sp else 15.sp, fontWeight = FontWeight.Bold,
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Text("%d:%02d".format(elapsed / 60, elapsed % 60), color = Ct.TextDim, fontSize = 13.sp)
                // Granted Duration(TS 24.380 §8.2.3.3) 잔여 — 마감 전 자동 종료되므로 남은 시간을
                // 같이 보여준다. 마지막 10초는 경고색.
                if (sp.self && s.speakDeadlineMs > 0) {
                    val left = ((s.speakDeadlineMs - nowMs).coerceAtLeast(0L) + 999) / 1000
                    Text("남은 ${left}초", fontSize = 12.sp,
                        color = if (left <= 10) Ct.Amber else Ct.TextFaint)
                }
            }
            st.floor == FloorState.REQUESTING -> Text("발언권 요청 중…", color = Ct.Amber,
                fontSize = 14.sp, fontWeight = FontWeight.Bold)
            st.floor == FloorState.QUEUED -> Text(
                s.queuePosition?.let { "발언 대기 ${it}번째" } ?: "발언 대기 중",
                color = Ct.Amber, fontSize = 14.sp, fontWeight = FontWeight.Bold)
            s.active -> Text("대기 중", color = Ct.TextFaint, fontSize = 13.sp)
            else -> Text("연결 중…", color = Ct.TextFaint, fontSize = 13.sp)
        }
    }
}

/** 영상 영역 — 영상 PTT 수신 화면 자리(현재 음성 전용이라 플레이스홀더).
 *  출력(스피커폰/수화기/이어폰)·전체듣기는 영상 위 우하단 오버레이 아이콘으로만 노출. */
@Composable
private fun VideoPanel(st: PttUiState, onRouteSelect: () -> Unit, modifier: Modifier = Modifier) {
    Box(
        modifier.fillMaxWidth().height(150.dp)
            .clip(RoundedCornerShape(12.dp)).background(Color.Black),
    ) {
        Column(
            Modifier.align(Alignment.Center),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Icon(painterResource(R.drawable.ic_video), contentDescription = null,
                tint = Ct.TextFaint, modifier = Modifier.size(26.dp))
            Text("영상 없음", color = Ct.TextFaint, fontSize = 11.sp)
        }
        Row(
            Modifier.align(Alignment.BottomEnd).padding(8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // 출력 라우팅 — 이어폰 미연결: 탭=스피커폰↔수화기 토글(기본 스피커폰).
            //              이어폰 연결(무선 다중 포함): 탭=선택 시트(이어폰/스피커폰/수화기).
            val (routeIcon, routeDesc) = when (st.route) {
                PttController.AUDIO_ROUTE_HEADSET -> R.drawable.ic_headset to "이어폰"
                SipController.AUDIO_ROUTE_SPEAKER -> R.drawable.ic_volume_on to "스피커폰"
                else -> R.drawable.ic_earpiece to "수화기"
            }
            OverlayToggle(
                icon = routeIcon, desc = routeDesc,
                active = st.route != SipController.AUDIO_ROUTE_EARPIECE,
            ) {
                if (st.headsets.isEmpty()) {
                    st.ctl?.setAudioRoute(
                        if (st.route == SipController.AUDIO_ROUTE_SPEAKER) SipController.AUDIO_ROUTE_EARPIECE
                        else SipController.AUDIO_ROUTE_SPEAKER)
                } else onRouteSelect()
            }
            val all = st.policy == ListenPolicy.ALL
            OverlayToggle(
                icon = R.drawable.ic_connected,
                desc = if (all) "전체듣기" else "주채널만", active = all,
            ) {
                st.ctl?.setListenPolicy(if (all) ListenPolicy.CHANNELS_ONLY else ListenPolicy.ALL)
            }
        }
    }
}

/** 영상 위 오버레이 토글 — 아이콘만(반투명 원형 스크림, 활성=민트). */
@Composable
private fun OverlayToggle(icon: Int, desc: String, active: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.size(34.dp).clip(CircleShape)
            .background(if (active) Ct.Mint else Color.Black.copy(alpha = 0.45f))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(painterResource(icon), contentDescription = desc,
            tint = if (active) Ct.OnMint else Color.White, modifier = Modifier.size(17.dp))
    }
}

/** 가로형 대형 PTT 바(시안의 마이크 버튼) — 누르는 동안 발언, 떼면 해제. */
@Composable
private fun PttBar(floor: FloorState, enabled: Boolean, listenOnly: Boolean = false,
                   queuePosition: Int? = null, modifier: Modifier = Modifier,
                   onDown: () -> Unit, onUp: () -> Unit) {
    @Suppress("NAME_SHADOWING") val enabled = enabled && !listenOnly
    val speaking = floor == FloorState.SPEAKING
    val bg = when {
        !enabled -> Ct.GrayDim
        speaking -> Ct.Mint
        floor == FloorState.REQUESTING || floor == FloorState.QUEUED -> Ct.Amber
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
                listenOnly -> "청취 전용 채널"
                !enabled -> "채널 없음"
                speaking -> "발언 중 — 떼면 종료"
                // 대기열(§8.2.11) — 버튼을 계속 눌러 두면 순번이 오고, 떼면 대기 요청이 취소된다.
                floor == FloorState.QUEUED ->
                    queuePosition?.let { "대기 ${it}번째 — 떼면 취소" } ?: "대기 중 — 떼면 취소"
                floor == FloorState.REQUESTING -> "요청 중…"
                floor == FloorState.LISTENING -> "수신 중"
                else -> "눌러서 말하기"
            }
            Text(label, color = fg, fontSize = 14.sp, fontWeight = FontWeight.Bold)
        }
    }
}

/** 주채널 인라인 채팅 — 하단 영역(말풍선 리스트 + 입력바). 헤더 확장 버튼=전체 대화 화면. */
@Composable
private fun InlineChat(st: PttUiState, svc: PttService?, groupId: String,
                       onOpenThread: (String) -> Unit, modifier: Modifier = Modifier) {
    val tick = svc?.messageTick?.collectAsState()?.value ?: 0
    val entries = remember(tick, svc, groupId) { svc?.messages?.thread(groupId) ?: emptyList() }
    var input by remember(groupId) { mutableStateOf("") }
    val listState = rememberLazyListState()

    LaunchedEffect(groupId, entries.size) {
        if (entries.isNotEmpty()) listState.scrollToItem(entries.size - 1)
        svc?.markThreadRead(groupId)
    }

    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("채팅", color = Ct.TextDim, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f))
            Icon(painterResource(R.drawable.ic_message), contentDescription = "전체 화면",
                tint = Ct.TextFaint,
                modifier = Modifier.size(16.dp).clickable { onOpenThread(groupId) })
        }
        Spacer(Modifier.height(6.dp))

        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f).fillMaxWidth()
                .clip(RoundedCornerShape(12.dp)).background(Ct.Surface)
                .padding(horizontal = 10.dp, vertical = 6.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            if (entries.isEmpty()) {
                items(1) {
                    Box(Modifier.fillMaxWidth().padding(vertical = 16.dp), contentAlignment = Alignment.Center) {
                        Text("메시지가 없습니다", color = Ct.TextFaint, fontSize = 11.sp)
                    }
                }
            }
            itemsIndexed(entries) { _, e ->
                val mine = e.direction == MsgDirection.OUT
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start,
                    verticalAlignment = Alignment.Bottom,
                ) {
                    if (mine) Text(chatTime(e.time), color = Ct.TextFaint, fontSize = 9.sp,
                        modifier = Modifier.padding(end = 5.dp, bottom = 2.dp))
                    Text(
                        if (e.attName.isNotBlank()) "📎 ${e.attName}" else e.text,
                        color = if (mine) Ct.OnMint else Ct.Text,
                        fontSize = 13.sp,
                        modifier = Modifier
                            .widthIn(max = 240.dp)
                            .clip(RoundedCornerShape(
                                topStart = 12.dp, topEnd = 12.dp,
                                bottomStart = if (mine) 12.dp else 4.dp,
                                bottomEnd = if (mine) 4.dp else 12.dp))
                            .background(if (mine) Ct.Mint else Ct.SurfaceHi)
                            .padding(horizontal = 10.dp, vertical = 6.dp),
                    )
                    if (!mine) Text(chatTime(e.time), color = Ct.TextFaint, fontSize = 9.sp,
                        modifier = Modifier.padding(start = 5.dp, bottom = 2.dp))
                }
            }
        }
        Spacer(Modifier.height(6.dp))

        // 입력바 — 첨부 + 입력 + 전송(SIP MESSAGE 그룹 fan-out)
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            AttachButton(38) { uri -> svc?.sendGroupAttachment(groupId, uri) }
            Box(
                Modifier.weight(1f).height(38.dp)
                    .clip(RoundedCornerShape(19.dp)).background(Ct.SurfaceHi)
                    .padding(horizontal = 13.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                if (input.isEmpty()) Text("메시지 입력", color = Ct.TextFaint, fontSize = 12.sp)
                BasicTextField(
                    value = input, onValueChange = { input = it },
                    textStyle = TextStyle(color = Ct.Text, fontSize = 13.sp),
                    cursorBrush = SolidColor(Ct.Mint),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            val canSend = input.isNotBlank()
            Box(
                Modifier.size(38.dp).clip(CircleShape)
                    .background(if (canSend) Ct.Mint else Ct.SurfaceHi)
                    .clickable(enabled = canSend) {
                        svc?.sendGroupMessage(groupId, input.trim())
                        input = ""
                    },
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_send), contentDescription = "전송",
                    tint = if (canSend) Ct.OnMint else Ct.TextFaint, modifier = Modifier.size(16.dp))
            }
        }
    }
}

private fun chatTime(t: Long): String = DateFormat.format("HH:mm", Date(t)).toString()
