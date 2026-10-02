// 채널 상세 «영상» 절 — MCVideo 영상 채널 (android_dispatch_tablet.md §6.14, dispatch_desktop_ui.md §10.3)
//
// 영상은 카드가 아니라 **그 그룹 채널 상세의 한 절**이다 — 음성(MCPTT 그룹 호)과 나란한 서비스라(TS 23.280 §3) 같은 채널에서 따로 든다.
// «영상 참여» 가 없다(D10 — 앱이 영상 호에 함께 합류해 있다). 관제사가 하는 일은 셋: 송출을 골라 [보기](D8 — 한 번에 하나),
// [영상 보내기](D11 — 발언 바 PTT 와 따로), 소리 맞추기([영상 소리]).
//
// 영상 칸은 **볼 영상이 있을 때만** 선다(고르기 전에는 영상 RTP 가 오지 않는다). 그림은 엔진이 Surface 에 곧바로 그린다 — 엔진의
// 수신 창은 하나이고(보는 송출도 한 번에 하나) 프레임 크기를 앱에 알려 주지 않는다. 그래서 칸의 모양(세로 3:4 ↔ 가로 4:3)과 회전은
// 사람이 맞추고, 보내는 사람마다 기억한다.
//
// 화면은 두 겹이다: [VideoSection](세션을 붙이는 껍데기) + [VideoSectionContent](순수 — [VideoSectionUi] 만 받는다).
package com.cims.ue.dispatch.ui.ptt

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.os.SystemClock
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.RotateLeft
import androidx.compose.material.icons.automirrored.filled.RotateRight
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.CropLandscape
import androidx.compose.material.icons.filled.CropPortrait
import androidx.compose.material.icons.filled.Smartphone
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.NoticeLevel
import com.cims.ue.dispatch.session.VideoCall
import com.cims.ue.dispatch.session.VideoChannel
import com.cims.ue.dispatch.session.VideoOrient
import com.cims.ue.dispatch.session.VideoRules
import com.cims.ue.dispatch.session.acceptVideo
import com.cims.ue.dispatch.session.canOpenVideo
import com.cims.ue.dispatch.session.endVideo
import com.cims.ue.dispatch.session.onVideoDisplayRotation
import com.cims.ue.dispatch.session.rotateVideo
import com.cims.ue.dispatch.session.setVideoPreviewSurface
import com.cims.ue.dispatch.session.setVideoSurface
import com.cims.ue.dispatch.session.setVideoVolume
import com.cims.ue.dispatch.session.switchVideoCamera
import com.cims.ue.dispatch.session.toggleVideoSend
import com.cims.ue.dispatch.session.toggleVideoWide
import com.cims.ue.dispatch.session.userPart
import com.cims.ue.dispatch.session.videoCalls
import com.cims.ue.dispatch.session.videoCameras
import com.cims.ue.dispatch.session.videoChannels
import com.cims.ue.dispatch.session.videoMicYielded
import com.cims.ue.dispatch.session.videoNameOf
import com.cims.ue.dispatch.session.videoOrients
import com.cims.ue.dispatch.ui.Pill
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.call.RxLevelRow
import com.cims.ue.sdk.ReceptionState
import com.cims.ue.sdk.TransmissionState
import kotlinx.coroutines.delay

/** «영상 n» 목록의 송출 한 줄 — 이름 · 기능 별칭 · 경과 + [보기]/[바꿔 보기] · «보는 중». */
data class VideoSenderUi(
    val userId: String,
    val name: String,
    /** «현장지휘 · 00:34» — 별칭이 없으면 경과만. */
    val meta: String = "",
    /** 버튼 글 — «보기»·«바꿔 보기»(다른 송출을 보고 있다)·«요청 중…»·«끝내는 중…». */
    val action: String = "보기",
    /** 누를 수 있다 — 알림 상태(Notified)일 때만. */
    val canAccept: Boolean = true,
    /** 보고 있다 — 버튼 대신 «보는 중». */
    val receiving: Boolean = false,
)

/** «영상» 절이 그리는 데 필요한 전부 — 세션 없이 Preview·단위시험이 선다. */
data class VideoSectionUi(
    /** 머리 옆 한 줄 — 연결 상태(«채널 참여와 함께 연결됨»·«영상 연결 중…»·편성 안내·한도·실패와 재시도). */
    val sub: String = "",
    // ── 보는 중 ──
    val receiving: Boolean = false,
    /** 보는 송출의 송출자 — 회전·모양 기억의 열쇠, 송출이 바뀌면 영상 칸을 새로 세운다. */
    val receivingId: String = "",
    /** 영상 칸 캡션 «김현장 · 현장지휘 · 00:34». */
    val caption: String = "",
    /** 영상 미디어가 열렸다(호가 m=video 를 협상) — 아니면(서버 answer port 0) 그림이 오지 않는다(영상 호 소리만). */
    val canRender: Boolean = true,
    val orient: VideoOrient = VideoOrient(),
    /** 영상 호 수신 음량 0~2(코어가 호에 기억한 값). */
    val volume: Float = 1f,
    // ── [영상 보내기] ──
    /** 버튼을 둘 것인가 — 영상 호가 있거나, 편성 채널(호가 없으면 이 버튼이 연다). */
    val showSend: Boolean = false,
    val sendText: String = "영상 보내기",
    val sendEnabled: Boolean = false,
    /** 여는 중·요청·대기·송출 중 — 같은 자리가 끄는 버튼이다(빨강). */
    val sendActive: Boolean = false,
    /** 누르면 카메라를 쓰게 된다(요청·편성 영상 호 열기) — 카메라 권한을 먼저 확인한다. */
    val sendStarts: Boolean = false,
    /** 송출 중 — 내 송출 줄(셀프뷰·경과·보는 사람)이 선다. */
    val sending: Boolean = false,
    /** «내 영상 보내는 중 · 00:12 · 보는 사람 2». */
    val sendingCaption: String = "",
    /** D12 음성 우선 — 무전을 말하는 동안 영상 호 소리를 멈췄다. */
    val micNote: Boolean = false,
    val canSwitchCamera: Boolean = false,
    // ── 목록 ──
    /** 연결됐지만 보내는 사람이 없다. */
    val noSender: Boolean = false,
    /** «보내는 중 2 · 보는 중 1 (한 번에 1개)». */
    val countText: String = "",
    val senders: List<VideoSenderUi> = emptyList(),
)

/** [영상 보내기] 버튼의 글 — 상태에 따라 같은 자리가 끄는 버튼이 된다(데스크톱 `VideoSendText`). */
internal fun videoSendText(opening: Boolean, tx: TransmissionState, queuePosition: Int, hasCamera: Boolean): String = when {
    opening -> "여는 중… · 취소"
    tx == TransmissionState.PENDING_REQUEST -> "요청 중… · 취소"
    tx == TransmissionState.QUEUED -> if (queuePosition in 1..253) "대기 ${queuePosition}번째 · 대기 취소" else "대기 중 · 대기 취소"
    tx == TransmissionState.PERMITTED -> "보내기 끝"
    tx == TransmissionState.PENDING_END -> "끝내는 중…"
    hasCamera -> "영상 보내기"
    else -> "영상 보내기 — 카메라 없음"
}

/**
 * 영상 채널 하나 → «영상» 절(순수 함수, 시험 대상). 판정은 데스크톱 `SidePanelViewModel` 의 영상 속성과 같다.
 *
 * @param canOpen 편성 영상 채널에 영상 호가 없어 [영상 보내기] 가 영상 호를 열 수 있다(`canOpenVideo`)
 * @param cameras 엔진이 아는 카메라 수 — 0 이면 [영상 보내기] 가 꺼지고, 둘 이상이면 송출 중 [카메라 전환] 이 선다
 */
internal fun videoSectionUi(
    channel: VideoChannel?,
    call: VideoCall?,
    cameras: Int,
    canOpen: Boolean,
    micYielded: Boolean,
    orients: Map<String, VideoOrient>,
    nameOf: (String) -> String,
    nowMs: Long,
): VideoSectionUi {
    val live = call?.isLive == true
    val connected = call?.isActive == true
    val opening = call?.isOpening == true
    val prearranged = channel?.isPrearranged == true
    val hasCamera = cameras > 0
    val tx = if (connected) call!!.tx else TransmissionState.NO_PERMISSION
    fun elapsed(userId: String): String = call?.since?.entries?.firstOrNull { userPart(it.key) == userPart(userId) }
        ?.let { fmtElapsed((nowMs - it.value).coerceAtLeast(0)) }.orEmpty()

    val rx = call?.receiving?.takeIf { live }
    val list = if (connected) call!!.transmitters else emptyList()
    val sending = tx == TransmissionState.PERMITTED
    return VideoSectionUi(
        sub = when {
            opening -> VideoRules.OPENING_NOTE
            connected -> if (prearranged) "편성 영상 호 연결됨" else "채널 참여와 함께 연결됨"
            live -> VideoRules.JOINING_NOTE
            // 편성 채널의 [영상 보내기] 가 꺼져 있는 까닭 — 제휴가 서야 초대 대상이 된다(데스크톱은 버튼 툴팁으로 말한다).
            prearranged && channel?.affiliated == false && channel.note == VideoRules.PREARRANGED_NOTE -> VideoRules.NOT_AFFILIATED_NOTE
            else -> channel?.note?.ifEmpty { VideoRules.JOINING_NOTE } ?: VideoRules.JOINING_NOTE
        },
        receiving = rx != null,
        receivingId = rx?.userId.orEmpty(),
        caption = rx?.let { listOf(nameOf(it.userId), it.functionalAlias, elapsed(it.userId)).filter { s -> s.isNotEmpty() }.joinToString(" · ") }.orEmpty(),
        canRender = call?.info?.video != false,
        orient = rx?.let { orients[userPart(it.userId)] } ?: VideoOrient(),
        volume = call?.info?.rxLevel ?: 1f,
        showSend = live || prearranged,
        sendText = videoSendText(opening, tx, call?.queuePosition ?: 0, hasCamera),
        sendEnabled = opening || when {
            connected -> tx != TransmissionState.PENDING_END && (tx != TransmissionState.NO_PERMISSION || hasCamera)
            live -> false                                              // 합류 중 — 성립 뒤에 누른다
            else -> canOpen
        },
        sendActive = opening || tx != TransmissionState.NO_PERMISSION,
        sendStarts = !opening && tx == TransmissionState.NO_PERMISSION,
        sending = sending,
        sendingCaption = if (!sending) "" else listOfNotNull(
            "내 영상 보내는 중",
            call?.txSinceMs?.let { fmtElapsed((nowMs - it).coerceAtLeast(0)) },
            call?.txReceivers?.size?.takeIf { it > 0 }?.let { "보는 사람 $it" }).joinToString(" · "),
        micNote = sending && micYielded,
        canSwitchCamera = sending && cameras >= 2,
        noSender = connected && list.isEmpty(),
        countText = "보내는 중 ${list.size}" + (if (rx != null) " · 보는 중 1" else "") + " (한 번에 1개)",
        senders = list.map { t ->
            VideoSenderUi(
                userId = t.userId, name = nameOf(t.userId),
                meta = listOf(t.functionalAlias, elapsed(t.userId)).filter { it.isNotEmpty() }.joinToString(" · "),
                action = VideoRules.actionText(t.state, otherReceiving = rx != null && t.state != ReceptionState.RECEIVING),
                canAccept = t.state == ReceptionState.NOTIFIED,
                receiving = t.state == ReceptionState.RECEIVING)
        })
}

/** 카드 1줄 «영상 n» — 그 영상 채널에 영상을 보내는 중인 사람 수(없으면 태그도 없다). 보고 있으면 굵게. */
internal fun MineCardUi.withVideo(calls: List<VideoCall>): MineCardUi {
    val call = calls.firstOrNull { it.groupId == id && it.isLive } ?: return this
    return copy(videoCount = if (call.isActive) call.transmitters.size else 0, videoWatching = call.receiving != null,
        videoSending = call.sending)
}

/** 화면이 자연 방향에서 돈 각도(0·90·180·270 — `Surface.ROTATION_*` × 90). 가로로 쓰는 태블릿이라 카메라 프레임을 이 값으로 세운다. */
@Composable
private fun displayRotation(): Int {
    LocalConfiguration.current                       // 구성(방향)이 바뀌면 다시 읽는다
    return (LocalView.current.display?.rotation ?: Surface.ROTATION_0) * 90
}

/**
 * 채널 상세 «영상» 절 — **세션을 붙이는 껍데기**. 영상 채널(내 멤버 그룹 ∩ MCVideo user profile 의 그룹 목록)이 아니면 아무것도
 * 그리지 않는다. 조작 줄 아래·접속/편성 명단 위에 선다.
 */
@Composable
fun ColumnScope.VideoSection(session: DispatchSession, groupId: String) {
    val groups by session.groups.collectAsStateWithLifecycle()
    if (groups.none { it.id == groupId && it.isMember && it.mcVideo }) return
    val channels by session.videoChannels.collectAsStateWithLifecycle()
    val calls by session.videoCalls.collectAsStateWithLifecycle()
    val cameras by session.videoCameras.collectAsStateWithLifecycle()
    val micYielded by session.videoMicYielded.collectAsStateWithLifecycle()
    val orients by session.videoOrients.collectAsStateWithLifecycle()
    session.registrations.collectAsStateWithLifecycle().value   // 등록이 서면 편성 [영상 보내기] 가 켜진다 — 다시 판정한다
    val call = calls.firstOrNull { it.groupId == groupId && it.isLive }

    // 경과(송출·내 송출)는 1초마다 다시 그린다 — 영상 호가 있는 동안만.
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    if (call != null) LaunchedEffect(Unit) { while (true) { delay(1000); now = System.currentTimeMillis() } }

    val ui = videoSectionUi(channels[groupId], call, cameras.size, session.canOpenVideo(groupId), micYielded, orients,
        session::videoNameOf, now)

    val rotation = displayRotation()
    LaunchedEffect(rotation) { session.onVideoDisplayRotation(rotation) }
    // 카메라 권한 — [영상 보내기] 를 처음 누를 때 묻는다(보기만 하는 관제사에게 미리 묻지 않는다).
    val context = LocalContext.current
    val askCamera = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) session.toggleVideoSend(groupId, rotation)
        else session.notify(NoticeLevel.WARN, "카메라 권한이 없어 영상을 보낼 수 없습니다",
            "기기 설정 › 앱 › CIMS 관제 › 권한에서 카메라를 허용하세요")
    }

    VideoSectionContent(
        ui = ui,
        onAccept = { id -> session.acceptVideo(groupId, id) },
        onEnd = { session.endVideo(groupId) },
        onSend = {
            val needCamera = ui.sendStarts &&
                ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED
            if (needCamera) askCamera.launch(Manifest.permission.CAMERA) else session.toggleVideoSend(groupId, rotation)
        },
        onVolume = { v -> session.setVideoVolume(groupId, v) },
        onRotate = { delta -> if (ui.receivingId.isNotEmpty()) session.rotateVideo(ui.receivingId, delta) },
        onWide = { if (ui.receivingId.isNotEmpty()) session.toggleVideoWide(ui.receivingId) },
        onSwitchCamera = session::switchVideoCamera,
        remote = { m, onFrame -> VideoTexture(m, onSurface = session::setVideoSurface, onFrame = onFrame) },
        // 셀프뷰는 카메라 미리보기라 화면이 돈 만큼 되돌려 세우고 거울상으로 그린다(보내는 영상은 그대로).
        self = { m, onFrame -> VideoTexture(m, onSurface = session::setVideoPreviewSurface, onFrame = onFrame) },
        selfRotation = selfViewRotation(rotation))
}

/** 카메라 미리보기를 화면에 세우는 회전 — 미리보기 버퍼는 기기의 자연 방향 기준이라 화면이 돈 만큼 되돌린다. */
internal fun selfViewRotation(displayRotation: Int): Float = when (((displayRotation % 360) + 360) % 360) {
    90 -> -90f
    270 -> 90f
    180 -> 180f
    else -> 0f
}

/**
 * 영상 칸의 크기 — 세로 3:4(단말 480×640)가 기본, 가로면 4:3. 작게 = 높이 240(패널 안에서 조작 줄까지 한눈에 든다),
 * [large] = 패널 폭까지 키운다(칸을 눌러 바꾼다 — 절 안에서 스크롤한다).
 */
internal fun videoBoxSize(maxWidth: Dp, landscape: Boolean, large: Boolean): Pair<Dp, Dp> =
    if (landscape) {
        val w = if (large) maxWidth else minOf(maxWidth, 320.dp)
        w to w * 3 / 4
    } else {
        val h = minOf(if (large) 440.dp else 240.dp, maxWidth * 4 / 3)
        h * 3 / 4 to h
    }

/**
 * «영상» 절 본문 — **순수 컴포저블**(영상 소리 펼침·칸 크게 보기·첫 장 여부만 제 상태).
 *
 * @param remote 수신 영상 칸의 그림 — (자리, 프레임이 그려질 때마다 부르는 콜백). 없으면 자리 표시만(Preview·시험)
 * @param self 셀프뷰의 그림 — 같은 모양. 없으면 자리 표시만
 * @param selfRotation 셀프뷰를 화면에 세우는 회전(°)
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ColumnScope.VideoSectionContent(
    ui: VideoSectionUi,
    onAccept: (String) -> Unit = {},
    onEnd: () -> Unit = {},
    onSend: () -> Unit = {},
    onVolume: (Float) -> Unit = {},
    onRotate: (Int) -> Unit = {},
    onWide: () -> Unit = {},
    onSwitchCamera: () -> Unit = {},
    remote: (@Composable (Modifier, () -> Unit) -> Unit)? = null,
    self: (@Composable (Modifier, () -> Unit) -> Unit)? = null,
    selfRotation: Float = 0f,
) {
    val p = Tokens.palette
    var volumeOpen by remember { mutableStateOf(false) }
    var volume by remember(ui.receiving) { mutableStateOf(ui.volume) }
    var large by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(10.dp)
    // 높이 — 영상을 보는 동안은 명단보다 영상이 먼저다(남는 높이의 대부분, 명단은 한 줄쯤 남고 스크롤된다). 보지 않을 때는 내용만큼,
    //   송출이 많으면 정해 둔 높이까지만 서고 절 안에서 스크롤한다 — 어느 쪽도 명단을 화면 밖으로 밀지 않는다.
    Column(
        (if (ui.receiving) Modifier.weight(5f, fill = false) else Modifier.heightIn(max = 270.dp))
            .padding(start = 16.dp, end = 16.dp, bottom = 10.dp).fillMaxWidth()
            .clip(shape).background(p.paper).border(BorderStroke(1.dp, p.line), shape)
            .verticalScroll(rememberScrollState())
            .padding(start = 12.dp, end = 12.dp, top = 10.dp, bottom = 8.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Icon(Icons.Filled.Videocam, contentDescription = null, tint = p.listen, modifier = Modifier.size(16.dp))
            Text("영상", fontSize = Type.body, fontWeight = FontWeight.Bold)
            Text(ui.sub, fontSize = Type.meta, color = p.muted, maxLines = 2, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
        }

        // 보는 중 — 영상 칸. 칸을 누르면 크게/작게, 오른쪽 위에서 모양(세로·가로)과 회전(90° 씩)을 맞춘다.
        if (ui.receiving) BoxWithConstraints(Modifier.fillMaxWidth().padding(top = 8.dp), contentAlignment = Alignment.Center) {
            val (w, h) = videoBoxSize(maxWidth, ui.orient.landscape, large)
            VideoBox(ui, w, h, remote, onTap = { large = !large }, onRotate = onRotate, onWide = onWide)
        }

        FlowRow(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (ui.receiving) {
                PillButton("그만 보기", onEnd, kind = Pill.LINE, height = 32.dp)
                PillButton("영상 소리", { volumeOpen = !volumeOpen }, kind = if (volumeOpen) Pill.INK else Pill.LINE, height = 32.dp)
            }
            // [영상 보내기](D11) — 여는 중·요청·대기·송출 중이면 같은 자리가 끄는 버튼(빨강).
            if (ui.showSend) PillButton(ui.sendText, onSend, kind = if (ui.sendActive) Pill.RED else Pill.LINE, height = 32.dp,
                enabled = ui.sendEnabled, leading = if (ui.sendActive) null else Icons.Filled.Videocam)
        }

        // 내 송출 — 셀프뷰(거울상 — 보내는 영상은 그대로) · 경과 · 보는 사람 n · D12 안내.
        if (ui.sending) Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            SelfBox(self, selfRotation)
            Column(Modifier.weight(1f)) {
                Text(ui.sendingCaption, fontSize = Type.body, fontWeight = FontWeight.SemiBold)
                if (ui.micNote) Text("무전 중 — 영상은 계속, 영상 소리는 멈춤", fontSize = Type.meta, color = p.muted,
                    modifier = Modifier.padding(top = 4.dp))
            }
            if (ui.canSwitchCamera) OverlayButton(Icons.Filled.Cameraswitch, "카메라 전환(앞·뒤)", onSwitchCamera, onVideo = false)
        }

        // [영상 소리] — 영상 호 수신 음량(무전과의 상대 음량 — TS 22.280 R-8.3-002). 막대는 끄는 대로, 값은 코어가 호에 기억한다.
        if (ui.receiving && volumeOpen) Box(Modifier.padding(top = 4.dp)) {
            RxLevelRow(volume) { v -> volume = v; onVolume(v) }
        }

        if (ui.noSender) Text("보내는 사람 없음 — 누가 보내면 여기에 목록이 생깁니다", fontSize = Type.meta, color = p.muted,
            modifier = Modifier.padding(top = 6.dp))

        // «영상 n» — 보내는 중인 송출 목록.
        if (ui.senders.isNotEmpty()) {
            Text(ui.countText, fontSize = Type.meta, color = p.ink2, modifier = Modifier.padding(top = 8.dp, bottom = 2.dp))
            ui.senders.forEach { s -> SenderRow(s) { onAccept(s.userId) } }
        }
    }
}

/**
 * 영상 칸 — 그림 + 자리 표시(첫 장 전·영상 미디어 없음) + 캡션 + 모양·회전 조작. 바탕은 테마와 무관한 검정이다(그림이 없는 자리).
 *
 * 그림(TextureView)은 **돌리기 전 크기**로 두고 한가운데에서 돌린다 — 90°·270° 면 칸이 가로로 바뀌어 그림이 칸을 채운다.
 */
@Composable
private fun VideoBox(
    ui: VideoSectionUi,
    width: Dp,
    height: Dp,
    remote: (@Composable (Modifier, () -> Unit) -> Unit)?,
    onTap: () -> Unit,
    onRotate: (Int) -> Unit,
    onWide: () -> Unit,
) {
    val turned = ui.orient.rotation % 180 == 90
    val (vw, vh) = if (turned) height to width else width to height
    // 첫 장이 그려지면 자리 표시를 거둔다. 송출을 바꾼 직후에는 앞 송출의 남은 장이 한두 장 더 올 수 있어 잠깐은 세지 않는다.
    val switchedAt = remember(ui.receivingId) { SystemClock.elapsedRealtime() }
    var hasPicture by remember(ui.receivingId) { mutableStateOf(false) }
    Box(Modifier.size(width, height).clip(RoundedCornerShape(8.dp)).background(Color.Black).clickable(onClick = onTap),
        contentAlignment = Alignment.Center) {
        // 같은 자리(Surface)를 송출이 바뀌어도 그대로 쓴다 — 앞 송출의 마지막 장은 아래 자리 표시가 가린다.
        if (remote != null && ui.canRender)
            remote(Modifier.requiredSize(vw, vh).graphicsLayer { rotationZ = ui.orient.rotation.toFloat() }) {
                if (!hasPicture && SystemClock.elapsedRealtime() - switchedAt >= VIDEO_REVEAL_MS) hasPicture = true
            }
        if (!hasPicture) Column(Modifier.fillMaxSize().background(Color.Black).padding(horizontal = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
            // 단말 윤곽이 돌아 방향을 보여 준다.
            Icon(Icons.Filled.Smartphone, contentDescription = null, tint = Color.White.copy(alpha = 0.55f),
                modifier = Modifier.size(40.dp).graphicsLayer {
                    rotationZ = ui.orient.rotation.toFloat() + if (ui.orient.wide) 90f else 0f
                })
            Spacer(Modifier.height(8.dp))
            Text(if (ui.canRender) "영상 기다리는 중…" else "영상 미디어가 열리지 않았습니다 — 영상 호 소리만 들립니다",
                fontSize = Type.meta, color = Color.White.copy(alpha = 0.75f), textAlign = TextAlign.Center)
        }
        Row(Modifier.align(Alignment.TopEnd).padding(6.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            OverlayButton(if (ui.orient.wide) Icons.Filled.CropPortrait else Icons.Filled.CropLandscape,
                if (ui.orient.wide) "세로로 보낸 영상으로(3:4)" else "가로로 보낸 영상으로(4:3)", onWide)
            OverlayButton(Icons.AutoMirrored.Filled.RotateLeft, "반시계 방향 90° 회전", { onRotate(-90) })
            OverlayButton(Icons.AutoMirrored.Filled.RotateRight, "시계 방향 90° 회전", { onRotate(90) })
        }
        if (ui.caption.isNotEmpty()) Text(ui.caption, fontSize = Type.meta, color = Color.White, maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.align(Alignment.BottomStart).padding(8.dp).clip(RoundedCornerShape(4.dp))
                .background(Color.Black.copy(alpha = 0.6f)).padding(horizontal = 8.dp, vertical = 3.dp))
    }
}

/**
 * 셀프뷰(128×96) — 내 카메라. 코어가 송출 중일 때만 프레임을 넘긴다(셀프뷰만으로 카메라를 열지 않는다). 거울상으로 그린다 —
 * 화면에서 좌우가 뒤집히게, 돌려 세운 그림이면 그림의 세로축을 뒤집는다.
 */
@Composable
private fun SelfBox(self: (@Composable (Modifier, () -> Unit) -> Unit)?, rotation: Float) {
    val turned = rotation % 180f != 0f
    val (vw, vh) = if (turned) 96.dp to 128.dp else 128.dp to 96.dp
    var hasPicture by remember { mutableStateOf(false) }
    Box(Modifier.size(128.dp, 96.dp).clip(RoundedCornerShape(6.dp)).background(Color.Black), contentAlignment = Alignment.Center) {
        if (self != null) self(Modifier.requiredSize(vw, vh).graphicsLayer {
            rotationZ = rotation
            if (turned) scaleY = -1f else scaleX = -1f
        }) { hasPicture = true }
        if (!hasPicture) Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
            Text("카메라 여는 중…", fontSize = Type.meta, color = Color.White.copy(alpha = 0.75f))
        }
    }
}

/** 영상 위·송출 줄의 작은 둥근 단추(32). [onVideo] = 그림 위(반투명 검정 + 흰 아이콘), 아니면 테두리 단추. */
@Composable
private fun OverlayButton(icon: ImageVector, label: String, onClick: () -> Unit, onVideo: Boolean = true) {
    val p = Tokens.palette
    Box(
        Modifier.size(32.dp).clip(CircleShape)
            .then(if (onVideo) Modifier.background(Color.Black.copy(alpha = 0.55f)) else Modifier.background(p.paper).border(1.dp, p.line, CircleShape))
            .clickable(onClickLabel = label, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = label, tint = if (onVideo) Color.White else p.ink2, modifier = Modifier.size(18.dp))
    }
}

/** 송출 한 줄(40) — 이름 · 기능 별칭 · 경과 + [보기]/[바꿔 보기](요청 중엔 흐림) · «보는 중»(버튼이 아니라 상태 글). */
@Composable
private fun SenderRow(s: VideoSenderUi, onAccept: () -> Unit) {
    val p = Tokens.palette
    Row(
        Modifier.fillMaxWidth().height(40.dp)
            .drawBehind { drawLine(p.hair, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) },
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(s.name, fontSize = Type.body, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f, fill = false))
        Text(s.meta, fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
        if (s.receiving) Text("보는 중", fontSize = Type.body, fontWeight = FontWeight.SemiBold, color = p.listenInk,
            modifier = Modifier.padding(end = 6.dp))
        else PillButton(s.action, onAccept, kind = Pill.LISTEN_LINE, height = 32.dp, enabled = s.canAccept)
    }
}

/** 송출을 바꾼 뒤 이만큼은 앞 송출의 남은 장으로 본다(현장 앱과 같은 값). */
private const val VIDEO_REVEAL_MS = 400L

/**
 * 엔진이 그리는 자리 — TextureView 의 Surface 를 넘긴다(생기면 Surface, 사라지면 null). SurfaceView 가 아닌 것은 그림을 돌리고
 * (회전·거울상) 둥근 칸 안에 잘라 넣어야 해서다 — SurfaceView 의 그림은 뷰 변환을 따르지 않는다.
 */
@Composable
private fun VideoTexture(modifier: Modifier, onSurface: (Surface?) -> Unit, onFrame: () -> Unit) {
    val surface by rememberUpdatedState(onSurface)
    val frame by rememberUpdatedState(onFrame)
    AndroidView(modifier = modifier, factory = { ctx ->
        TextureView(ctx).apply {
            surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                // 만든 Surface 는 우리가 놓는다 — 칸이 사라질 때마다 하나씩 남지 않게
                private var made: Surface? = null
                override fun onSurfaceTextureAvailable(t: SurfaceTexture, w: Int, h: Int) {
                    made?.release()
                    surface(Surface(t).also { made = it })
                }
                override fun onSurfaceTextureSizeChanged(t: SurfaceTexture, w: Int, h: Int) {}
                override fun onSurfaceTextureDestroyed(t: SurfaceTexture): Boolean {
                    surface(null)                  // 엔진에서 먼저 뗀다(명령은 줄을 서서 간다 — 비동기)
                    // 놓는 것은 그 뒤에 — 곧바로 놓으면 엔진이 버려진 창에 한두 장 더 그린다
                    val old = made; made = null
                    if (old != null) postDelayed({ old.release() }, 700)
                    return true
                }
                override fun onSurfaceTextureUpdated(t: SurfaceTexture) = frame()
            }
        }
    })
}
