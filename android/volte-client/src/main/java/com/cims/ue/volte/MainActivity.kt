package com.cims.ue.volte

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.media.AudioManager
import android.media.ToneGenerator
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.provider.Settings
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.TextureView
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.automirrored.filled.Backspace
import androidx.compose.material.icons.automirrored.filled.Message
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Dialpad
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.PersonAdd
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.filled.StarBorder
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Badge
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.cims.ue.core.calllog.CallEntry
import com.cims.ue.core.calllog.CallLogStore
import com.cims.ue.core.calllog.CallType
import com.cims.ue.core.account.SsoProvisioner
import com.cims.ue.core.config.ConfigStore
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.contacts.CompanyContact
import com.cims.ue.core.contacts.CompanyDirectory
import com.cims.ue.core.contacts.CompanyDirectoryStore
import com.cims.ue.core.contacts.CompanyOrg
import com.cims.ue.core.contacts.Contact
import com.cims.ue.core.contacts.ContactStore
import com.cims.ue.core.contacts.FavoriteStore
import com.cims.ue.core.message.MessageStore
import com.cims.ue.core.message.MsgDirection
import com.cims.ue.core.sip.CallState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Locale

class MainActivity : ComponentActivity() {

    /** 착신 알림 "받기" 요청(callId, video) — HomeScreen 이 서비스 연결 후 소비. */
    private val notifAnswer = MutableStateFlow<Pair<Int, Boolean>?>(null)

    /** 문자 알림 탭 → 문자 탭으로 진입 요청. */
    private val notifOpenMessages = MutableStateFlow(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 미로그인(로그아웃 상태) — CIMS 앱 로그인 화면으로 전환해 로그인 유도.
        if (com.cims.ue.core.account.CimsAccounts.redirectToLoginIfLoggedOut(this)) {
            finish(); return
        }
        handleIntent(intent)
        setContent {
            // 시안 다크 고정 — PTT 앱과 같은 다크·민트 톤(Theme.kt).
            PhoneTheme {
                Surface(modifier = Modifier.fillMaxSize()) { App(notifAnswer, notifOpenMessages) }
            }
        }
    }

    // launchMode=singleTask — 알림 PendingIntent 가 기존 인스턴스로 들어온다.
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(i: Intent?) {
        i ?: return
        val answerId = i.getIntExtra(SipService.EXTRA_ANSWER_CALL_ID, -1)
        if (answerId >= 0) {
            notifAnswer.value = answerId to i.getBooleanExtra(SipService.EXTRA_ANSWER_VIDEO, false)
            i.removeExtra(SipService.EXTRA_ANSWER_CALL_ID)
        }
        if (i.getBooleanExtra(SipService.EXTRA_OPEN_MESSAGES, false)) {
            notifOpenMessages.value = true
            i.removeExtra(SipService.EXTRA_OPEN_MESSAGES)
        }
    }

    // 포그라운드 복귀 시 등록 재시도(keepalive) — doze/슬립 후 끊긴 등록 즉시 복구.
    // 복귀 시점에 로그아웃 상태면(화면이 recents 에 남아 있다 돌아온 경우) 로그인 유도로 전환.
    override fun onResume() {
        super.onResume()
        if (com.cims.ue.core.account.CimsAccounts.redirectToLoginIfLoggedOut(this)) {
            finish(); return
        }
        if (com.cims.ue.core.account.SsoProvisioner.hasAccount(this) || ConfigStore(this).load().isComplete()) {
            runCatching { SipService.poke(this) }
        }
    }
}

private enum class Screen { GATE, HOME, CONFIG }
private enum class Tab { CONTACTS, RECENTS, KEYPAD, MESSAGES, SETTINGS }

@Composable
private fun App(
    notifAnswer: MutableStateFlow<Pair<Int, Boolean>?>,
    notifOpenMessages: MutableStateFlow<Boolean>,
) {
    val context = LocalContext.current
    val store = remember { ConfigStore(context) }
    var config by remember { mutableStateOf(store.load()) }
    // 수동 설정 모드면 재프로비저닝 없이 저장값으로 HOME. 아니면 공유 계정 있을 때
    // 진입 시 항상 최신 정보 재취득(GATE→재프로비저닝), 계정 없으면 캐시 설정으로 HOME.
    var screen by remember { mutableStateOf(
        if (store.isManual() && config.isComplete()) Screen.HOME
        else if (com.cims.ue.core.account.SsoProvisioner.hasAccount(context)) Screen.GATE
        else if (config.isComplete()) Screen.HOME else Screen.GATE
    ) }

    when (screen) {
        // CIMS-Phone 는 자체 로그인 없음 — CIMS 공유 계정으로 자동 구성(SSO). 계정 없으면 CIMS 앱 로그인 유도.
        Screen.GATE -> SsoGateScreen(
            onProvisioned = { c ->
                // saveProvisioned = 사용자가 고른 transport 유지(서버 값은 기본값/권장).
                store.setManual(false); store.saveProvisioned(c); config = store.load(); screen = Screen.HOME
            },
            onManual = { store.setManual(true); screen = Screen.CONFIG },
        )
        Screen.CONFIG -> SettingsScreen(
            config = config,
            standalone = true,
            onApply = { c -> store.save(c); config = c },
            onDone = { screen = Screen.HOME },
            onCancel = { screen = if (config.isComplete()) Screen.HOME else Screen.GATE },
        )
        Screen.HOME -> HomeScreen(
            config = config,
            onConfigChanged = { c -> store.save(c); config = c },
            notifAnswer = notifAnswer,
            notifOpenMessages = notifOpenMessages,
        )
    }
}

// ─────────────────────────────────────── 로그인(자동 프로비저닝) ───────────────────────────────────────

@Composable
private fun SsoGateScreen(
    onProvisioned: (SipAccountConfig) -> Unit,
    onManual: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var hasAccount by remember { mutableStateOf(com.cims.ue.core.account.SsoProvisioner.hasAccount(context)) }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }

    fun provision() {
        busy = true; status = "CIMS 계정으로 구성 중…"
        scope.launch {
            val cfg = runCatching {
                withContext(Dispatchers.IO) {
                    val prof = com.cims.ue.core.account.SsoProvisioner.fetchProfile(context)
                        ?: error("CIMS 로그인 세션이 없습니다")
                    val svc = prof.service("volte") ?: error("이 계정에 VoLTE 서비스가 없습니다")
                    svc.toSipAccountConfig(
                        loginId = prof.loginId ?: "",
                        displayName = prof.displayName ?: "",
                        loginPassword = com.cims.ue.core.account.SsoProvisioner.loginPassword(context),
                        countryCode = prof.countryCode.orEmpty(),
                    )
                }
            }
            busy = false
            cfg.onSuccess { onProvisioned(it) }.onFailure { e ->
                // 재프로비저닝 일시 실패(예: CIMS 앱 bind failure) — 캐시된 설정이 있으면
                // 그걸로 즉시 진입한다(최신화는 다음 실행/서비스 몫). 캐시도 없을 때만 에러 표시.
                val cached = ConfigStore(context).load()
                if (cached.isComplete()) onProvisioned(cached)
                else status = e.message ?: "구성 실패"
            }
        }
    }

    // 진입 시 공유 계정 있으면 자동 구성(로그인 UI 없음).
    LaunchedEffect(hasAccount) { if (hasAccount && !busy) provision() }

    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text("CIMS", style = MaterialTheme.typography.displaySmall,
            fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
        Text("CIMS-Phone", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(16.dp))
        if (hasAccount) {
            Text("CIMS 계정으로 자동 구성합니다.", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(12.dp))
            if (busy) CircularProgressIndicator()
            if (status.isNotBlank()) {
                Spacer(Modifier.height(8.dp))
                Text(status, style = MaterialTheme.typography.bodySmall, textAlign = TextAlign.Center)
                if (!busy) { Spacer(Modifier.height(8.dp)); Button(onClick = { provision() }) { Text("다시 시도") } }
            }
        } else {
            Text("이 앱은 별도 로그인이 없습니다.\nCIMS 앱에서 먼저 로그인하세요.",
                style = MaterialTheme.typography.bodyMedium, textAlign = TextAlign.Center)
            Spacer(Modifier.height(16.dp))
            Button(
                onClick = {
                    val act = context as? android.app.Activity
                    android.accounts.AccountManager.get(context).addAccount(
                        com.cims.ue.core.account.CimsAccounts.ACCOUNT_TYPE,
                        com.cims.ue.core.account.CimsAccounts.TOKEN_PROVISIONING,
                        null, null, act,
                        { _ -> hasAccount = com.cims.ue.core.account.SsoProvisioner.hasAccount(context) },
                        android.os.Handler(android.os.Looper.getMainLooper()),
                    )
                },
                modifier = Modifier.fillMaxWidth().widthIn(max = 360.dp),
            ) { Text("CIMS 로그인 열기") }
        }
        Spacer(Modifier.height(20.dp))
        TextButton(onClick = onManual) { Text("수동 설정 (고급)") }
    }
}

// ─────────────────────────────────────── 전화 홈 (탭 구성) ───────────────────────────────────────

@Composable
private fun HomeScreen(
    config: SipAccountConfig,
    onConfigChanged: (SipAccountConfig) -> Unit,
    notifAnswer: MutableStateFlow<Pair<Int, Boolean>?>,
    notifOpenMessages: MutableStateFlow<Boolean>,
) {
    val context = LocalContext.current
    val callLog = remember { CallLogStore(context) }
    val contacts = remember { ContactStore(context) }
    val companyDir = remember { CompanyDirectoryStore(context) }
    val favorites = remember { FavoriteStore(context) }
    // 프로비저닝 수신값(SoT) 우선, 미수신(구서버)일 때만 내 번호에서 유도 — 같은 국가는 로컬 표기
    homeCountryCode = config.countryCode.ifBlank { countryCodeOf(config.msisdn) ?: "" }.ifBlank { null }

    // SipService 바인딩 — 등록은 서비스가 자동으로 유지한다(수동 등록 버튼 없음).
    var service by remember { mutableStateOf<SipService?>(null) }
    DisposableEffect(Unit) {
        val conn = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                service = (binder as? SipService.LocalBinder)?.service
            }
            override fun onServiceDisconnected(name: ComponentName?) { service = null }
        }
        SipService.start(context)
        context.bindService(Intent(context, SipService::class.java), conn, Context.BIND_AUTO_CREATE)
        onDispose { runCatching { context.unbindService(conn) } }
    }

    // 통화/알림 권한 — 진입 시 1회 요청(승인되면 재등록 트리거).
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { service?.ensureRegistered() }
    LaunchedEffect(Unit) { permLauncher.launch(requiredPermissions()) }

    // 화면 최상단 전역 상태배지 — '다른 앱 위에 표시' 권한 안내(1회).
    var askOverlay by remember {
        mutableStateOf(
            !Settings.canDrawOverlays(context) &&
                !context.getSharedPreferences("ui_prefs", Context.MODE_PRIVATE)
                    .getBoolean("overlay_asked", false),
        )
    }
    if (askOverlay) {
        fun done() {
            context.getSharedPreferences("ui_prefs", Context.MODE_PRIVATE)
                .edit().putBoolean("overlay_asked", true).apply()
            askOverlay = false
        }
        AlertDialog(
            onDismissRequest = { done() },
            title = { Text("화면 상단 상태 표시") },
            text = { Text("어떤 앱을 쓰고 있어도 화면 맨 위에 통화 가능 상태를 표시하려면 '다른 앱 위에 표시' 권한이 필요합니다.") },
            confirmButton = {
                Button(onClick = {
                    runCatching {
                        context.startActivity(
                            Intent(
                                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                android.net.Uri.parse("package:${context.packageName}"),
                            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                        )
                    }
                    done()
                }) { Text("허용하러 가기") }
            },
            dismissButton = { TextButton(onClick = { done() }) { Text("나중에") } },
        )
    }

    // 등록상태 표시는 화면 최상단 전역 배지(오버레이)+상태바 알림이 담당 — 인앱 칩은 두지 않는다.
    val fallbackCall = remember { MutableStateFlow<CallState>(CallState.Null) }
    val call by (service?.callState ?: fallbackCall).collectAsState()

    // 발신: 음성/영상 구분. 영상은 카메라 권한 확보 후 발신.
    var videoOn by remember { mutableStateOf(false) }
    var pendingVideoNumber by remember { mutableStateOf<String?>(null) }

    // 통화 기록용 미결 상태 — 발신은 doDial 시점에 확정(즉시 실패 호는 StateFlow 컨플레이션으로
    // Outgoing 상태가 관측되지 않을 수 있음), 수신은 Incoming 상태 관측으로 잡는다.
    var pendingNumber by remember { mutableStateOf<String?>(null) }
    var pendingIncoming by remember { mutableStateOf(false) }
    var pendingVideo by remember { mutableStateOf(false) }
    var connectedAt by remember { mutableStateOf(0L) }

    fun doDial(number: String, video: Boolean) {
        val n = number.trim()
        if (n.isBlank()) return
        videoOn = video
        pendingNumber = extractNumber(n); pendingIncoming = false
        pendingVideo = video; connectedAt = 0L
        service?.setVideoEnabled(video)
        service?.makeCall(n)   // 기록은 종료 시점에 통화시간과 함께 남긴다
    }
    val cameraLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        val num = pendingVideoNumber; pendingVideoNumber = null
        if (granted && !num.isNullOrBlank()) doDial(num, true) else videoOn = false
    }
    fun dial(number: String, video: Boolean) {
        if (number.isBlank()) return
        if (video) { pendingVideoNumber = number; cameraLauncher.launch(Manifest.permission.CAMERA) }
        else doDial(number, false)
    }

    // 착신 응답: 영상 응답은 카메라 권한 확보 후 answer(withVideo). 거부되면 음성으로만 받는다.
    var pendingVideoAnswer by remember { mutableStateOf<Int?>(null) }
    val answerCamLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        val id = pendingVideoAnswer; pendingVideoAnswer = null
        if (id != null) { videoOn = granted; service?.answer(id, granted) }
    }
    fun answerCall(id: Int, video: Boolean) {
        if (video) { pendingVideoAnswer = id; answerCamLauncher.launch(Manifest.permission.CAMERA) }
        else { videoOn = false; service?.answer(id, false) }
    }

    // 착신 알림 "받기" — 서비스 연결을 기다렸다 응답(잠금화면/백그라운드에서 알림으로 진입한 경우).
    val notifAnswerReq by notifAnswer.collectAsState()
    LaunchedEffect(service, notifAnswerReq) {
        val req = notifAnswerReq ?: return@LaunchedEffect
        if (service == null) return@LaunchedEffect
        notifAnswer.value = null
        answerCall(req.first, req.second)
    }

    // 통화중 음소거/스피커 토글 상태 — 통화 종료 시 초기화(스피커 라우팅은 서비스가 원복).
    var muted by remember { mutableStateOf(false) }
    var speakerOn by remember { mutableStateOf(false) }

    // 통화 기록 — 종료 시점에 한 건 기록: 방향(발신/수신/부재중) + 연결 통화시간 + 영상 여부.
    LaunchedEffect(call, videoOn) {
        when (val c = call) {
            is CallState.Incoming -> {
                pendingNumber = extractNumber(c.remote); pendingIncoming = true
                pendingVideo = c.video; connectedAt = 0L
            }
            is CallState.Active -> {
                if (connectedAt == 0L) connectedAt = System.currentTimeMillis()
                if (videoOn) pendingVideo = true   // 통화 중 영상 전환도 영상통화로 기록
            }
            is CallState.Disconnected -> {
                muted = false; speakerOn = false
                pendingNumber?.let { n ->
                    val dur = if (connectedAt > 0) ((System.currentTimeMillis() - connectedAt) / 1000).toInt() else 0
                    val type = when {
                        !pendingIncoming -> CallType.OUTGOING
                        connectedAt > 0 -> CallType.INCOMING
                        else -> CallType.MISSED
                    }
                    callLog.add(n, type, durationSec = dur, video = pendingVideo)
                    pendingNumber = null; pendingVideo = false
                }
                connectedAt = 0L
            }
            else -> {}
        }
    }

    var tab by remember { mutableStateOf(Tab.KEYPAD) }
    val inCall = call is CallState.Incoming || call is CallState.Outgoing || call is CallState.Active

    // 문자 알림 탭 → 문자 탭으로.
    val openMessagesReq by notifOpenMessages.collectAsState()
    LaunchedEffect(openMessagesReq) {
        if (openMessagesReq) { tab = Tab.MESSAGES; notifOpenMessages.value = false }
    }

    // 문자 저장소 변경 신호(수신/발신) — 배지·목록 갱신 트리거.
    val fallbackMsgVer = remember { MutableStateFlow(0L) }
    val msgVersion by (service?.messagesVersion ?: fallbackMsgVer).collectAsState()

    if (inCall) {
        CallScreen(
            call = call,
            videoOn = videoOn,
            muted = muted,
            speakerOn = speakerOn,
            onToggleVideo = { on ->
                videoOn = on
                if (on) cameraLauncher.launch(Manifest.permission.CAMERA) else service?.setVideoEnabled(false)
            },
            onToggleMute = { id, on -> muted = on; service?.setMuted(id, on) },
            onToggleSpeaker = { on -> speakerOn = on; service?.setSpeaker(on) },
            onSurface = { service?.setVideoSurface(it) },
            onPreviewSurface = { service?.setPreviewSurface(it) },
            onSwitchCamera = { service?.switchCamera() },
            onAnswer = { id -> answerCall(id, false) },
            onAnswerVideo = { id -> answerCall(id, true) },
            onReject = { id -> service?.reject(id) },
            onHangup = { id -> service?.hangup(id) },
        )
    } else {
        val msgStore = remember { MessageStore(context) }
        val unread = remember(msgVersion) { msgStore.unreadTotal() }
        Scaffold(
            bottomBar = { BottomNav(tab, unread) { tab = it } },
        ) { pad ->
            // top 여백 = 화면 최상단 전역 상태배지(오버레이)와 겹치지 않게 확보
            Box(Modifier.padding(pad).padding(top = 32.dp).fillMaxSize()) {
                when (tab) {
                    Tab.CONTACTS -> ContactsScreen(
                        personal = contacts,
                        company = companyDir,
                        favorites = favorites,
                        onCallVoice = { dial(it, false) },
                        onCallVideo = { dial(it, true) },
                        onSendMessage = { number, text -> service?.sendMessage(number, text) },
                    )
                    Tab.RECENTS -> RecentsScreen(
                        store = callLog,
                        contacts = contacts,
                        myLine = myLine(config),
                        onCall = { n, video -> dial(n, video) },
                    )
                    Tab.KEYPAD -> KeypadScreen(
                        myNumber = myLine(config),
                        onVoice = { dial(it, false) },
                        onVideo = { dial(it, true) },
                    )
                    Tab.MESSAGES -> MessagesScreen(
                        store = msgStore,
                        version = msgVersion,
                        nameFor = { n -> contacts.nameFor(n) },
                        onSend = { peer, text -> service?.sendMessage(peer, text) },
                        onMarkRead = { peer -> service?.markThreadRead(peer) ?: msgStore.markRead(peer) },
                    )
                    // 설정 = 탭 콘텐츠(하단 내비 유지). 항목 변경 즉시 저장·재등록 반영.
                    Tab.SETTINGS -> SettingsScreen(
                        config = config,
                        standalone = false,
                        onApply = { c ->
                            onConfigChanged(c)
                            service?.ensureRegistered()
                        },
                    )
                }
            }
        }
    }
}

/** 하단 내비 5탭 — PTT 앱과 같은 다크 바(민트 활성, 문자=안읽음 뱃지). */
@Composable
private fun BottomNav(current: Tab, unread: Int, onSelect: (Tab) -> Unit) {
    val tabs = listOf(Tab.CONTACTS, Tab.RECENTS, Tab.KEYPAD, Tab.MESSAGES, Tab.SETTINGS)
    val items = listOf(
        NavItem("연락처", Icons.Filled.Person),
        NavItem("통화이력", Icons.Filled.History),
        NavItem("키패드", Icons.Filled.Dialpad),
        NavItem("문자", Icons.AutoMirrored.Filled.Message),
        NavItem("설정", Icons.Filled.Settings),
    )
    DarkBottomNav(
        items = items,
        currentIndex = tabs.indexOf(current),
        badge = mapOf(tabs.indexOf(Tab.MESSAGES) to unread),
        onSelect = { onSelect(tabs[it]) },
    )
}

// ─────────────────────────────────────── 키패드 탭 ───────────────────────────────────────

/**
 * 키패드 DTMF 터치 톤 — 기본 전화앱처럼 키를 누를 때 해당 DTMF 음을 낸다.
 * 시스템 설정 "다이얼 시 터치음"(DTMF_TONE_WHEN_DIALING) 을 존중하고, STREAM_DTMF 로 재생.
 */
@Composable
private fun rememberDtmfTonePlayer(): (String) -> Unit {
    val context = LocalContext.current
    val gen = remember {
        runCatching { ToneGenerator(AudioManager.STREAM_DTMF, DTMF_TONE_VOLUME) }.getOrNull()
    }
    DisposableEffect(Unit) { onDispose { runCatching { gen?.release() } } }
    val enabled = remember {
        Settings.System.getInt(context.contentResolver, Settings.System.DTMF_TONE_WHEN_DIALING, 1) == 1
    }
    return remember(gen, enabled) {
        { digit: String ->
            if (enabled && gen != null) {
                val tone = when (digit) {
                    "0" -> ToneGenerator.TONE_DTMF_0; "1" -> ToneGenerator.TONE_DTMF_1
                    "2" -> ToneGenerator.TONE_DTMF_2; "3" -> ToneGenerator.TONE_DTMF_3
                    "4" -> ToneGenerator.TONE_DTMF_4; "5" -> ToneGenerator.TONE_DTMF_5
                    "6" -> ToneGenerator.TONE_DTMF_6; "7" -> ToneGenerator.TONE_DTMF_7
                    "8" -> ToneGenerator.TONE_DTMF_8; "9" -> ToneGenerator.TONE_DTMF_9
                    "*" -> ToneGenerator.TONE_DTMF_S; "#" -> ToneGenerator.TONE_DTMF_P
                    else -> -1
                }
                if (tone >= 0) runCatching { gen.startTone(tone, DTMF_TONE_MS) }
            }
        }
    }
}

@Composable
private fun KeypadScreen(
    myNumber: String,
    onVoice: (String) -> Unit,
    onVideo: (String) -> Unit,
) {
    var dialed by remember { mutableStateOf("") }
    val playDtmf = rememberDtmfTonePlayer()

    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (myNumber.isNotBlank()) {
            Spacer(Modifier.height(4.dp))
            Text("내 번호 $myNumber", style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        Spacer(Modifier.weight(1f))

        // 입력 번호 표시
        Box(Modifier.fillMaxWidth().height(56.dp), contentAlignment = Alignment.Center) {
            Text(dialed, style = MaterialTheme.typography.displaySmall,
                color = MaterialTheme.colorScheme.onSurface, maxLines = 1)
        }

        Spacer(Modifier.height(4.dp))
        Keypad(onDigit = { dialed += it; playDtmf(it) })
        Spacer(Modifier.height(16.dp))

        // 하단 액션 — 영상(좌) · 음성(중앙, 초록) · ⌫(우)
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.size(56.dp).clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surfaceVariant)
                    .clickable(enabled = dialed.isNotBlank()) { onVideo(dialed.trim()) },
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Filled.Videocam, contentDescription = "영상통화",
                    tint = if (dialed.isNotBlank()) CALL_GREEN else MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Box(
                Modifier.size(72.dp).clip(CircleShape)
                    .background(if (dialed.isNotBlank()) CALL_GREEN else CALL_GREEN.copy(alpha = 0.4f))
                    .clickable(enabled = dialed.isNotBlank()) { onVoice(dialed.trim()) },
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Filled.Call, contentDescription = "음성통화", tint = Color.White,
                    modifier = Modifier.size(32.dp))
            }
            Box(
                Modifier.size(56.dp).clip(CircleShape)
                    .clickable(enabled = dialed.isNotEmpty()) { dialed = dialed.dropLast(1) },
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.AutoMirrored.Filled.Backspace, contentDescription = "지우기",
                    tint = if (dialed.isNotEmpty()) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.3f))
            }
        }
        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun Keypad(onDigit: (String) -> Unit) {
    // (숫자, 한글, 영문) — 일반 전화앱과 동일한 서브라벨
    val rows = listOf(
        listOf(Triple("1", "ㄱㅋ", ".QZ"), Triple("2", "ㄴ", "ABC"), Triple("3", "ㄷㅌ", "DEF")),
        listOf(Triple("4", "ㄹ", "GHI"), Triple("5", "ㅁ", "JKL"), Triple("6", "ㅂㅍ", "MNO")),
        listOf(Triple("7", "ㅅ", "PRS"), Triple("8", "ㅇ", "TUV"), Triple("9", "ㅈㅊ", "WXY")),
        listOf(Triple("*", "", ","), Triple("0", "ㅎ", "+"), Triple("#", "", ";")),
    )
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        rows.forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                row.forEach { (digit, kor, eng) -> KeypadKey(digit, kor, eng) { onDigit(digit) } }
            }
        }
    }
}

/** 플랫 키 — 배경 원 없이 큰 숫자 + 우측 한글/영문 서브라벨 (일반 전화앱 스타일). */
@Composable
private fun KeypadKey(digit: String, kor: String, eng: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .width(92.dp)
            .height(62.dp)
            .clip(RoundedCornerShape(14.dp))
            .clickable { onClick() },
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(digit, fontSize = 34.sp, color = MaterialTheme.colorScheme.onBackground)
        if (kor.isNotBlank() || eng.isNotBlank()) {
            Spacer(Modifier.width(5.dp))
            Column {
                if (kor.isNotBlank()) Text(kor, fontSize = 10.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (eng.isNotBlank()) Text(eng, fontSize = 10.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

// ─────────────────────────────────────── 연락처 탭 ───────────────────────────────────────

/** 상세 화면 대상 — 행을 누르면 바로 발신하지 않고 정보+작업(음성/영상/문자/즐겨찾기)을 띄운다. */
private data class DetailTarget(val name: String, val number: String, val org: String?)

@Composable
private fun ContactsScreen(
    personal: ContactStore,
    company: CompanyDirectoryStore,
    favorites: FavoriteStore,
    onCallVoice: (String) -> Unit,
    onCallVideo: (String) -> Unit,
    onSendMessage: (String, String) -> Unit,
) {
    var seg by remember { mutableStateOf(1) }            // 0=즐겨찾기 1=회사 2=개인
    var detail by remember { mutableStateOf<DetailTarget?>(null) }
    var favVersion by remember { mutableStateOf(0) }     // 즐겨찾기 변경 트리거
    var query by remember { mutableStateOf("") }         // 공통 검색(최상단)
    val showAddPersonal = remember { mutableStateOf(false) }

    Column(Modifier.fillMaxSize()) {
        // 최상단 검색 — 라운드 필드(일반 전화앱 스타일), 모든 세그먼트에 공통 적용.
        SearchField(query) { query = it }

        // 언더라인 탭 + (개인 탭) 우측 끝 연락처 추가
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            UnderlineTab("즐겨찾기", seg == 0) { seg = 0 }
            Spacer(Modifier.width(20.dp))
            UnderlineTab("회사", seg == 1) { seg = 1 }
            Spacer(Modifier.width(20.dp))
            UnderlineTab("개인", seg == 2) { seg = 2 }
            Spacer(Modifier.weight(1f))
            if (seg == 2) {
                Box(
                    Modifier.size(36.dp).clip(CircleShape).clickable { showAddPersonal.value = true },
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Filled.PersonAdd, contentDescription = "연락처 추가",
                        tint = MaterialTheme.colorScheme.onSurface, modifier = Modifier.size(22.dp))
                }
            }
        }
        when (seg) {
            0 -> FavoritesScreen(favorites, favVersion, query, onOpen = { detail = it }, onFavChanged = { favVersion++ })
            1 -> CompanyContacts(company, favorites, favVersion, query, onOpen = { detail = it }, onFavChanged = { favVersion++ })
            else -> PersonalContacts(personal, favorites, favVersion, query, showAddPersonal, onOpen = { detail = it }, onFavChanged = { favVersion++ })
        }
    }

    detail?.let { t ->
        ContactDetailDialog(
            target = t, favorites = favorites,
            onDismiss = { detail = null },
            onVoice = { onCallVoice(t.number); detail = null },
            onVideo = { onCallVideo(t.number); detail = null },
            onSendMessage = { text -> onSendMessage(t.number, text) },
            onFavChanged = { favVersion++ },
        )
    }
}

/** 최상단 라운드 검색 필드 — 돋보기 + placeholder + 지우기(입력 시). */
@Composable
private fun SearchField(query: String, onChange: (String) -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp).height(44.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Spacer(Modifier.width(12.dp))
        Icon(Icons.Filled.Search, contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.size(20.dp))
        Spacer(Modifier.width(8.dp))
        BasicTextField(
            value = query,
            onValueChange = onChange,
            singleLine = true,
            textStyle = LocalTextStyle.current.copy(
                color = MaterialTheme.colorScheme.onSurface, fontSize = 15.sp),
            cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
            modifier = Modifier.weight(1f),
            decorationBox = { inner ->
                if (query.isEmpty()) {
                    Text("이름·전화번호 검색", fontSize = 15.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                inner()
            },
        )
        if (query.isNotEmpty()) {
            Box(
                Modifier.size(36.dp).clip(CircleShape).clickable { onChange("") },
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Filled.Close, contentDescription = "지우기",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.size(18.dp))
            }
        }
        Spacer(Modifier.width(4.dp))
    }
}

/** 언더라인 텍스트 탭 — 선택=굵게+밑줄 (일반 전화앱 스타일). */
@Composable
private fun UnderlineTab(label: String, selected: Boolean, onClick: () -> Unit) {
    // IntrinsicSize.Max = 한 줄 전체 텍스트 폭 (CJK 는 Min 이 글자 하나 폭이라 잘림).
    Column(Modifier.width(IntrinsicSize.Max).clickable { onClick() },
        horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label, fontSize = 14.sp,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
            color = if (selected) MaterialTheme.colorScheme.onSurface
            else MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1, softWrap = false)
        Spacer(Modifier.height(5.dp))
        Box(
            Modifier.fillMaxWidth().height(2.dp)
                .background(if (selected) MaterialTheme.colorScheme.onSurface else Color.Transparent),
        )
    }
}

/** 공용 연락처 행 — 별표 토글 + 탭(상세에서 발신). [trailing] 으로 추가 버튼(개인=수정). */
@Composable
private fun ContactListRow(
    name: String, line2: String, depth: Int, isFav: Boolean,
    onTap: () -> Unit, onToggleFav: () -> Unit,
    trailing: (@Composable () -> Unit)? = null,
) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onTap() }
            .padding(start = (depth * 16).dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.surfaceVariant),
            contentAlignment = Alignment.Center,
        ) { Text(name.take(1).ifBlank { "?" }, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        Spacer(Modifier.size(12.dp))
        Column(Modifier.weight(1f)) {
            Text(name.ifBlank { line2 }, style = MaterialTheme.typography.bodyLarge)
            Text(line2, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        TextButton(onClick = onToggleFav) {
            Icon(if (isFav) Icons.Filled.Star else Icons.Filled.StarBorder, contentDescription = "즐겨찾기",
                tint = if (isFav) FAV_GOLD else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(22.dp))
        }
        trailing?.invoke()
    }
}

// ── 즐겨찾기 ──
@Composable
private fun FavoritesScreen(
    favorites: FavoriteStore, favVersion: Int, query: String,
    onOpen: (DetailTarget) -> Unit, onFavChanged: () -> Unit,
) {
    val q = query.trim()
    val list = remember(favVersion, q) {
        favorites.all().filter { q.isBlank() || it.name.contains(q, true) || it.number.contains(q) }
    }
    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        if (list.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (q.isBlank()) "즐겨찾기가 없습니다.\n연락처에서 ★ 로 추가하세요." else "검색 결과가 없습니다.",
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else LazyColumn(Modifier.fillMaxSize()) {
            items(list, key = { it.number }) { f ->
                ContactListRow(f.name, fmtNumber(f.number), depth = 0, isFav = true,
                    onTap = { onOpen(DetailTarget(f.name, f.number, null)) },
                    onToggleFav = { favorites.toggle(f.name, f.number); onFavChanged() })
                HorizontalDivider()
            }
        }
    }
}

/** 회사 연락처 — 서버 프로비저닝 제공, 읽기전용. 조직 칩 + 팀별 sticky 섹션 + 검색 + 동기화(버전 기반). */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
private fun CompanyContacts(
    store: CompanyDirectoryStore, favorites: FavoriteStore, favVersion: Int, query: String,
    onOpen: (DetailTarget) -> Unit, onFavChanged: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var dir by remember { mutableStateOf(store.load()) }
    var lastSync by remember { mutableStateOf(store.lastSyncedAt()) }
    var loading by remember { mutableStateOf(false) }
    var note by remember { mutableStateOf("") }
    var curOrg by remember { mutableStateOf<String?>(null) }   // 폴더 탐색 현재 위치 (null=최상위)
    val favSet = remember(favVersion) { favorites.all().map { it.number }.toSet() }

    fun sync() {
        if (loading) return
        loading = true; note = ""
        scope.launch {
            val res = withContext(Dispatchers.IO) { SsoProvisioner.fetchDirectory(context, store.etag()) }
            val now = System.currentTimeMillis()
            when {
                res == null -> note = "동기화 실패 — 네트워크/로그인을 확인하세요."
                res.changed && res.dir != null -> { store.replace(res.dir!!, res.etag, now); dir = res.dir!!; lastSync = now; note = "최신으로 갱신했습니다." }
                else -> { store.touchSynced(now); lastSync = now; note = "이미 최신입니다." }
            }
            loading = false
        }
    }
    LaunchedEffect(Unit) { sync() }   // 진입 시 버전 확인(변경 시에만 다운로드)

    // 당겨서 새로고침(PTT 앱과 동일 패턴) — 별도 동기화 버튼 없음.
    PullToRefreshBox(
        isRefreshing = loading,
        onRefresh = { sync() },
        modifier = Modifier.fillMaxSize(),
    ) {
    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                (if (lastSync > 0) "마지막 동기화: ${formatTime(lastSync)}" else "동기화 안 됨 — 아래로 당겨 동기화") +
                    (if (note.isNotBlank()) " · $note" else ""),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f).padding(vertical = 6.dp),
            )
        }

        val q = query.trim()
        val orgName = remember(dir) { dir.orgs.associate { it.code to it.name } }
        if (dir.members.isEmpty()) {
            // 빈 상태도 스크롤 가능해야 당겨서 새로고침 제스처가 동작한다.
            Box(Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                contentAlignment = Alignment.Center) {
                Text(note.ifBlank { "회사 연락처가 없습니다.\n아래로 당겨 동기화하세요." },
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 120.dp))
            }
        } else if (q.isNotBlank()) {
            val hits = remember(dir, q) {
                dir.members.filter { it.name.contains(q, true) || it.number.contains(q) }.sortedBy { it.name }
            }
            if (hits.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("검색 결과가 없습니다.", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            } else LazyColumn(Modifier.fillMaxSize()) {
                items(hits, key = { "hit:${it.number}" }) { m ->
                    val on = orgName[m.orgCode] ?: m.orgCode
                    ContactListRow(m.name, "${fmtNumber(m.number)} · $on", depth = 0, isFav = m.number in favSet,
                        onTap = { onOpen(DetailTarget(m.name, m.number, on)) },
                        onToggleFav = { favorites.toggle(m.name, m.number); onFavChanged() })
                    HorizontalDivider()
                }
            }
        } else {
            // 계단식 조직 칩(범위 선택) + 팀별 sticky 섹션(전체 경로) 평면 리스트.
            val orgByCode = remember(dir) { dir.orgs.associateBy { it.code } }
            val byParent = remember(dir) { dir.orgs.groupBy { it.parent } }
            val membersByOrg = remember(dir) { dir.members.groupBy { it.orgCode } }
            val roots = remember(dir) {
                dir.orgs.filter { it.parent.isBlank() || it.parent !in orgByCode }.sortedBy { it.sort }
            }
            // 선택 경로 (최상위→현재) — 칩 계단 전개·뒤로가기용
            val path = remember(dir, curOrg) {
                val p = ArrayList<CompanyOrg>()
                var c = curOrg
                while (c != null) {
                    val o = orgByCode[c] ?: break
                    p.add(0, o)
                    c = o.parent.takeIf { it in orgByCode }
                }
                p
            }
            // 시스템 뒤로가기 = 한 단계 위 범위로 (상세 화면이 열려 있으면 그쪽 BackHandler 가 우선)
            BackHandler(enabled = curOrg != null) {
                curOrg = orgByCode[curOrg!!]?.parent?.takeIf { it in orgByCode }
            }

            // 조직 선택 — 현재 범위(경로) 버튼 → 바텀시트(단계별 펼침 트리)
            val subtreeCount = remember(dir) {
                val cache = HashMap<String, Int>()
                fun cnt(code: String): Int = cache.getOrPut(code) {
                    (membersByOrg[code]?.size ?: 0) + (byParent[code]?.sumOf { cnt(it.code) } ?: 0)
                }
                dir.orgs.forEach { cnt(it.code) }
                cache
            }
            var orgMenuOpen by remember { mutableStateOf(false) }
            Row(
                Modifier.clip(RoundedCornerShape(8.dp)).clickable { orgMenuOpen = true }
                    .padding(vertical = 8.dp, horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    if (path.isEmpty()) "전체 조직" else path.joinToString(" > ") { it.name },
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.primary,
                )
                Icon(Icons.Filled.ArrowDropDown, contentDescription = "조직 선택",
                    tint = MaterialTheme.colorScheme.primary)
            }
            if (orgMenuOpen) {
                OrgPickerSheet(
                    roots = roots, byParent = byParent, subtreeCount = subtreeCount,
                    current = curOrg,
                    initialExpanded = path.map { it.code }.toSet(),
                    onSelect = { curOrg = it; orgMenuOpen = false },
                    onDismiss = { orgMenuOpen = false },
                )
            }

            // 섹션 = 선택 범위(하위 포함) 내 직접 구성원 보유 조직, 라벨 = 전체 경로
            val sections = remember(dir, curOrg) {
                fun pathLabel(code: String): String {
                    val names = ArrayList<String>()
                    var c: String? = code
                    while (c != null) {
                        val o = orgByCode[c] ?: break
                        names.add(0, o.name)
                        c = o.parent.takeIf { it in orgByCode }
                    }
                    return names.joinToString(" > ")
                }
                val out = ArrayList<Pair<String, List<CompanyContact>>>()
                fun walk(o: CompanyOrg) {
                    membersByOrg[o.code]?.takeIf { it.isNotEmpty() }
                        ?.let { out.add(pathLabel(o.code) to it.sortedBy { m -> m.name }) }
                    byParent[o.code]?.sortedBy { it.sort }?.forEach { walk(it) }
                }
                if (curOrg == null) {
                    roots.forEach { walk(it) }
                    val orphan = dir.members.filter { it.orgCode.isBlank() || it.orgCode !in orgByCode }
                    if (orphan.isNotEmpty()) out.add("(조직 미지정)" to orphan.sortedBy { it.name })
                } else orgByCode[curOrg]?.let { walk(it) }
                out
            }

            if (sections.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("구성원이 없습니다.", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            } else LazyColumn(Modifier.fillMaxSize()) {
                sections.forEach { (label, mems) ->
                    stickyHeader(key = "hdr:$label") { DirSectionHeader(label, mems.size) }
                    items(mems, key = { "m:${it.orgCode}:${it.number}" }) { m ->
                        ContactListRow(m.name, fmtNumber(m.number), depth = 0, isFav = m.number in favSet,
                            onTap = { onOpen(DetailTarget(m.name, m.number, orgName[m.orgCode])) },
                            onToggleFav = { favorites.toggle(m.name, m.number); onFavChanged() })
                        HorizontalDivider()
                    }
                }
            }
        }
    }
    }   // PullToRefreshBox
}

/** 조직 선택 바텀시트 — 단계별 펼침 트리(처음엔 최상위+현재 경로만 펼침). 이름 탭=선택, ▸ 탭=펼침. */
@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
private fun OrgPickerSheet(
    roots: List<CompanyOrg>,
    byParent: Map<String, List<CompanyOrg>>,
    subtreeCount: Map<String, Int>,
    current: String?,
    initialExpanded: Set<String>,
    onSelect: (String?) -> Unit,
    onDismiss: () -> Unit,
) {
    var expanded by remember { mutableStateOf(initialExpanded) }
    val rows = remember(expanded) {
        buildList {
            fun walk(o: CompanyOrg, d: Int) {
                add(Triple(o, d, byParent[o.code].orEmpty().isNotEmpty()))
                if (o.code in expanded) byParent[o.code].orEmpty().sortedBy { it.sort }.forEach { walk(it, d + 1) }
            }
            roots.forEach { walk(it, 0) }
        }
    }
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Text("조직 선택", style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(start = 24.dp, bottom = 4.dp))
        LazyColumn(Modifier.fillMaxWidth().padding(bottom = 24.dp)) {
            item(key = "all") {
                OrgPickRow("전체 조직", count = null, depth = 0, hasChildren = false,
                    isExpanded = false, selected = current == null,
                    onToggle = {}, onPick = { onSelect(null) })
            }
            items(rows, key = { it.first.code }) { (o, d, hasKids) ->
                OrgPickRow(o.name, subtreeCount[o.code], d, hasKids,
                    isExpanded = o.code in expanded, selected = o.code == current,
                    onToggle = {
                        expanded = if (o.code in expanded) expanded - o.code else expanded + o.code
                    },
                    onPick = { onSelect(o.code) })
            }
        }
    }
}

/** 조직 피커 한 행 — [▸/▾](펼침 토글) + 이름(탭=선택) + 인원수. */
@Composable
private fun OrgPickRow(
    name: String, count: Int?, depth: Int, hasChildren: Boolean,
    isExpanded: Boolean, selected: Boolean,
    onToggle: () -> Unit, onPick: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().clickable { onPick() }
            .padding(start = (16 + depth * 20).dp, end = 24.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(40.dp).clip(CircleShape)
                .then(if (hasChildren) Modifier.clickable { onToggle() } else Modifier),
            contentAlignment = Alignment.Center,
        ) {
            if (hasChildren) {
                Icon(if (isExpanded) Icons.Filled.KeyboardArrowDown else Icons.Filled.KeyboardArrowRight,
                    contentDescription = if (isExpanded) "접기" else "펼치기",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Text(name, style = MaterialTheme.typography.bodyLarge,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
            color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.weight(1f).padding(vertical = 14.dp))
        count?.let {
            Text("${it}명", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** sticky 섹션 헤더 — 조직 전체 경로(CIMS > 본부 > 팀) + 인원수. 불투명 배경(밑으로 스크롤 통과 방지). */
@Composable
private fun DirSectionHeader(pathLabel: String, count: Int) {
    Row(
        Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.background)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(pathLabel, style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
        Text("${count}명", style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** 개인 연락처 — 단말 로컬, 추가(탭 우측 아이콘)/수정/삭제(좌로 스와이프) 가능. */
@Composable
private fun PersonalContacts(
    store: ContactStore, favorites: FavoriteStore, favVersion: Int, query: String,
    showAdd: MutableState<Boolean>,
    onOpen: (DetailTarget) -> Unit, onFavChanged: () -> Unit,
) {
    var list by remember { mutableStateOf(store.all()) }
    var editing by remember { mutableStateOf<Contact?>(null) }
    val favSet = remember(favVersion) { favorites.all().map { it.number }.toSet() }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        val q = query.trim()
        val shown = if (q.isBlank()) list else list.filter { it.name.contains(q, true) || it.number.contains(q) }
        if (shown.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (list.isEmpty()) "저장된 개인 연락처가 없습니다.\n오른쪽 위 추가 버튼으로 등록하세요." else "검색 결과가 없습니다.",
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else LazyColumn(Modifier.fillMaxSize()) {
            items(shown, key = { it.id }) { c ->
                // 좌로 스와이프 → 삭제 (빨간 배경 + 휴지통)
                val dismissState = rememberSwipeToDismissBoxState(
                    confirmValueChange = { v ->
                        if (v == SwipeToDismissBoxValue.EndToStart) {
                            store.delete(c.id); list = store.all(); true
                        } else false
                    },
                )
                SwipeToDismissBox(
                    state = dismissState,
                    enableDismissFromStartToEnd = false,
                    backgroundContent = {
                        Box(
                            Modifier.fillMaxSize().background(HANGUP_RED),
                            contentAlignment = Alignment.CenterEnd,
                        ) {
                            Icon(Icons.Filled.Delete, contentDescription = "삭제",
                                tint = Color.White, modifier = Modifier.padding(end = 24.dp))
                        }
                    },
                ) {
                    Box(Modifier.background(MaterialTheme.colorScheme.background)) {
                        ContactListRow(c.name, fmtNumber(c.number), depth = 0, isFav = c.number in favSet,
                            onTap = { onOpen(DetailTarget(c.name, c.number, null)) },
                            onToggleFav = { favorites.toggle(c.name, c.number); onFavChanged() },
                            trailing = { TextButton(onClick = { editing = c }) { Text("수정") } })
                    }
                }
                HorizontalDivider()
            }
        }
    }

    if (showAdd.value) {
        ContactDialog(null,
            onDismiss = { showAdd.value = false },
            onSave = { name, num -> store.upsert(name, num); list = store.all(); showAdd.value = false })
    }
    editing?.let { c ->
        ContactDialog(c,
            onDismiss = { editing = null },
            onSave = { name, num -> store.upsert(name, num, c.id); list = store.all(); editing = null },
            onDelete = { store.delete(c.id); list = store.all(); editing = null })
    }
}

@Composable
private fun ContactDialog(
    initial: Contact?,
    onDismiss: () -> Unit,
    onSave: (String, String) -> Unit,
    onDelete: (() -> Unit)? = null,
) {
    var name by remember { mutableStateOf(initial?.name ?: "") }
    var number by remember { mutableStateOf(initial?.number ?: "") }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (initial == null) "연락처 추가" else "연락처 수정") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(name, { name = it }, label = { Text("이름") }, singleLine = true)
                OutlinedTextField(number, { number = it.filter { ch -> ch.isDigit() || ch == '+' } },
                    label = { Text("번호 (MSISDN)") }, singleLine = true)
            }
        },
        confirmButton = {
            Button(enabled = name.isNotBlank() && number.isNotBlank(),
                onClick = { onSave(name, number) }) { Text("저장") }
        },
        dismissButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                if (onDelete != null) TextButton(onClick = onDelete) { Text("삭제", color = HANGUP_RED) }
                TextButton(onClick = onDismiss) { Text("취소") }
            }
        },
    )
}

/** 연락처 상세 — 전화앱 스타일 전체화면(아바타/이름 중앙 + 액션 버튼 + 정보). 행을 누르면 표시된다. */
@Composable
private fun ContactDetailDialog(
    target: DetailTarget, favorites: FavoriteStore,
    onDismiss: () -> Unit, onVoice: () -> Unit, onVideo: () -> Unit,
    onSendMessage: (String) -> Unit, onFavChanged: () -> Unit,
) {
    var fav by remember { mutableStateOf(favorites.isFavorite(target.number)) }
    var composing by remember { mutableStateOf(false) }
    var msg by remember { mutableStateOf("") }
    var sent by remember { mutableStateOf(false) }

    BackHandler(onBack = onDismiss)   // 시스템 뒤로가기 → 목록으로

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(Modifier.fillMaxSize()) {
            // 상단 바 — 뒤로
            Row(Modifier.fillMaxWidth().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onDismiss) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "뒤로")
                }
            }
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                // 아바타 + 이름(중앙)
                Box(
                    Modifier.size(96.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primaryContainer),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(target.name.take(1).ifBlank { "?" }, style = MaterialTheme.typography.headlineMedium,
                        color = MaterialTheme.colorScheme.onPrimaryContainer)
                }
                Spacer(Modifier.height(16.dp))
                Text(target.name.ifBlank { fmtNumber(target.number) }, style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(24.dp))

                if (!composing) {
                    // 액션 버튼 행 (음성통화/영상통화/메시지/즐겨찾기)
                    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp),
                        horizontalArrangement = Arrangement.SpaceEvenly) {
                        LabeledRound("음성통화", CALL_GREEN, Icons.Filled.Call) { onVoice() }
                        LabeledRound("영상통화", VIDEO_BLUE, Icons.Filled.Videocam) { onVideo() }
                        LabeledRound("메시지", MaterialTheme.colorScheme.surfaceVariant, Icons.AutoMirrored.Filled.Message,
                            fg = MaterialTheme.colorScheme.onSurface) { composing = true }
                        LabeledRound("즐겨찾기",
                            if (fav) FAV_GOLD else MaterialTheme.colorScheme.surfaceVariant,
                            if (fav) Icons.Filled.Star else Icons.Filled.StarBorder,
                            fg = if (fav) Color.White else MaterialTheme.colorScheme.onSurface) {
                            fav = favorites.toggle(target.name, target.number); onFavChanged()
                        }
                    }
                    Spacer(Modifier.height(28.dp))
                    // 정보 섹션 (휴대전화/소속) — 번호 우측에 초록 전화 버튼
                    HorizontalDivider()
                    ContactInfoRow("휴대전화", fmtNumber(target.number), onCall = onVoice)
                    target.org?.takeIf { it.isNotBlank() }?.let {
                        HorizontalDivider()
                        ContactInfoRow("소속", it, onCall = null)
                    }
                    HorizontalDivider()
                } else {
                    // 문자 작성
                    Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(msg, { msg = it; sent = false }, label = { Text("문자 내용") },
                            modifier = Modifier.fillMaxWidth(), minLines = 2)
                        if (sent) Text("문자를 전송했습니다.", style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary)
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = { composing = false }) { Text("뒤로") }
                            Spacer(Modifier.weight(1f))
                            Button(enabled = msg.isNotBlank(), onClick = { onSendMessage(msg.trim()); sent = true; msg = "" }) {
                                Text("보내기")
                            }
                        }
                    }
                }
            }
        }
    }
}

/** 상세 정보 행 — 라벨(작게)+값(크게), 우측 초록 전화 버튼(있으면 발신). */
@Composable
private fun ContactInfoRow(label: String, value: String, onCall: (() -> Unit)?) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(2.dp))
            Text(value, style = MaterialTheme.typography.titleMedium)
        }
        if (onCall != null) {
            Box(
                Modifier.size(40.dp).clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surfaceVariant)
                    .clickable { onCall() },
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Filled.Call, contentDescription = "전화", tint = CALL_GREEN, modifier = Modifier.size(20.dp))
            }
        }
    }
}


// ─────────────────────────────────────── 통화이력 탭 ───────────────────────────────────────

/** 통화이력 — 시안(통화이력.png): 헤더+필터 칩(전체/수신/발신/부재중)+일자 섹션+방향·통화시간.
 *  삭제 = 행 좌로 스와이프(한 건) 또는 선택 모드(헤더 [선택]·행 길게 누름 → 체크+전체 선택+선택 삭제).
 *  한번에 전체 지우기 버튼은 두지 않는다. */
@Composable
private fun RecentsScreen(
    store: CallLogStore,
    contacts: ContactStore,
    myLine: String,
    onCall: (String, Boolean) -> Unit,
) {
    var log by remember { mutableStateOf(store.all()) }
    var filter by remember { mutableStateOf(0) }   // 0=전체 1=수신 2=발신 3=부재중
    var selecting by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf(setOf<CallEntry>()) }

    fun exitSelect() { selecting = false; selected = emptySet() }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        ScreenHeader(
            label = "CIMS", title = "통화이력",
            subtitle = myLine.ifBlank { null },
            trailing = {
                if (log.isNotEmpty()) {
                    Text(if (selecting) "취소" else "선택", color = Ct.TextDim, fontSize = 13.sp,
                        modifier = Modifier.clip(RoundedCornerShape(8.dp))
                            .clickable { if (selecting) exitSelect() else selecting = true }
                            .padding(horizontal = 8.dp, vertical = 6.dp))
                }
            },
        )
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("전체", "수신", "발신", "부재중").forEachIndexed { i, label ->
                FilterPill(label, filter == i) { filter = i }
            }
        }
        Spacer(Modifier.height(4.dp))

        val shown = when (filter) {
            1 -> log.filter { it.type == CallType.INCOMING }
            2 -> log.filter { it.type == CallType.OUTGOING }
            3 -> log.filter { it.type == CallType.MISSED }
            else -> log
        }

        // 선택 모드 액션 행 — 전체 선택(현재 필터 결과 기준) + 선택 삭제.
        if (selecting) {
            val allSelected = shown.isNotEmpty() && selected.containsAll(shown)
            Row(
                Modifier.fillMaxWidth().padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FilterPill(if (allSelected) "전체 해제" else "전체 선택", allSelected) {
                    selected = if (allSelected) selected - shown.toSet() else selected + shown
                }
                Spacer(Modifier.weight(1f))
                val n = selected.size
                Text("선택 삭제${if (n > 0) " ($n)" else ""}",
                    color = if (n > 0) Ct.Red else Ct.TextFaint,
                    fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.clip(RoundedCornerShape(8.dp))
                        .clickable(enabled = n > 0) {
                            store.removeAll(selected); log = store.all(); exitSelect()
                        }
                        .padding(horizontal = 8.dp, vertical = 6.dp))
            }
        }

        if (shown.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (log.isEmpty()) "통화 기록이 없습니다." else "해당하는 기록이 없습니다.", color = Ct.TextDim)
            }
        } else {
            // 일자별 섹션 (오늘/어제/yyyy.MM.dd) — 기록이 최신순이라 섹션도 최신순.
            val groups = shown.groupBy { dayLabel(it.time) }
            LazyColumn(Modifier.fillMaxSize()) {
                groups.forEach { (day, entries) ->
                    item(key = "day:$day") { SectionLabel(day, Modifier.padding(top = 4.dp)) }
                    items(entries, key = { "${it.time}:${it.number}:${it.type}" }) { e ->
                        if (selecting) {
                            RecentRow(e, contacts.nameFor(e.number),
                                selecting = true, checked = e in selected,
                                onClick = { selected = if (e in selected) selected - e else selected + e })
                        } else {
                            // 좌로 스와이프 → 한 건 삭제 (빨간 배경 + 휴지통, 개인 연락처와 동일 패턴)
                            val dismissState = rememberSwipeToDismissBoxState(
                                confirmValueChange = { v ->
                                    if (v == SwipeToDismissBoxValue.EndToStart) {
                                        store.remove(e); log = store.all(); true
                                    } else false
                                },
                            )
                            SwipeToDismissBox(
                                state = dismissState,
                                enableDismissFromStartToEnd = false,
                                backgroundContent = {
                                    Box(
                                        Modifier.fillMaxSize().clip(RoundedCornerShape(12.dp)).background(HANGUP_RED),
                                        contentAlignment = Alignment.CenterEnd,
                                    ) {
                                        Icon(Icons.Filled.Delete, contentDescription = "삭제",
                                            tint = Color.White, modifier = Modifier.padding(end = 24.dp))
                                    }
                                },
                            ) {
                                Box(Modifier.background(Ct.Bg)) {
                                    RecentRow(e, contacts.nameFor(e.number),
                                        onClick = { onCall(e.number, e.video) },
                                        onLongPress = { selecting = true; selected = setOf(e) })
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/** 이력 한 행 — 좌 아이콘 박스(발신=민트/부재중=적/영상=캠) + 이름·음성/영상 칩·일시 + 우 방향·통화시간.
 *  [selecting]=선택 모드(맨 앞 체크 원, 탭=토글), [onLongPress]=길게 눌러 선택 모드 진입. */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
private fun RecentRow(
    e: CallEntry,
    name: String?,
    selecting: Boolean = false,
    checked: Boolean = false,
    onClick: () -> Unit,
    onLongPress: (() -> Unit)? = null,
) {
    val missed = e.type == CallType.MISSED
    val (dirText, dirColor) = when (e.type) {
        CallType.OUTGOING -> "↑발신" to Ct.Mint
        CallType.INCOMING -> "↓수신" to Ct.TextDim
        CallType.MISSED -> "부재중" to Ct.Red
    }
    val iconTint = when (e.type) {
        CallType.OUTGOING -> Ct.Mint
        CallType.MISSED -> Ct.Red
        else -> Ct.TextDim
    }
    Row(
        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp))
            .combinedClickable(onClick = onClick, onLongClick = onLongPress)
            .padding(vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (selecting) {
            Box(
                Modifier.size(22.dp).clip(CircleShape)
                    .background(if (checked) Ct.Mint else Color.Transparent)
                    .border(1.5.dp, if (checked) Ct.Mint else Ct.Border, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                if (checked) {
                    Icon(Icons.Filled.Check, contentDescription = "선택됨",
                        tint = Ct.OnMint, modifier = Modifier.size(14.dp))
                }
            }
            Spacer(Modifier.width(10.dp))
        }
        Box(
            Modifier.size(42.dp).clip(RoundedCornerShape(12.dp))
                .background(when (e.type) {
                    CallType.OUTGOING -> Ct.MintDim
                    CallType.MISSED -> Ct.RedDim
                    else -> Ct.SurfaceHi
                })
                .border(1.dp, iconTint.copy(alpha = 0.35f), RoundedCornerShape(12.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Icon(if (e.video) Icons.Filled.Videocam else Icons.Filled.Call,
                contentDescription = null, tint = iconTint, modifier = Modifier.size(19.dp))
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(name ?: fmtNumber(e.number), color = if (missed) Ct.Red else Ct.Text,
                    fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
                TagChip(if (e.video) "영상" else "음성", tint = if (e.video) Ct.Mint else Ct.TextDim)
            }
            Spacer(Modifier.height(3.dp))
            Text(logTime(e.time), color = Ct.TextFaint, fontSize = 12.sp)
        }
        Column(horizontalAlignment = Alignment.End) {
            Text(dirText, color = dirColor, fontWeight = FontWeight.SemiBold, style = chipStyle(12))
            Spacer(Modifier.height(3.dp))
            Text(
                if (e.durationSec > 0) "%02d:%02d".format(e.durationSec / 60, e.durationSec % 60) else "—",
                color = Ct.TextFaint, fontSize = 12.sp,
            )
        }
    }
}

/** 통화이력 일자 섹션 라벨 — 오늘/어제/yyyy.MM.dd. */
private fun dayLabel(ts: Long): String {
    val fmt = SimpleDateFormat("yyyy.MM.dd", Locale.KOREA)
    val day = fmt.format(java.util.Date(ts))
    val now = System.currentTimeMillis()
    return when (day) {
        fmt.format(java.util.Date(now)) -> "오늘"
        fmt.format(java.util.Date(now - 86_400_000L)) -> "어제"
        else -> day
    }
}

/** 통화이력 행 일시 — 시안 표기(2026.06.30 09:41:23). */
private fun logTime(ts: Long): String =
    SimpleDateFormat("yyyy.MM.dd HH:mm:ss", Locale.KOREA).format(java.util.Date(ts))

// ─────────────────────────────────────── 문자 탭 ───────────────────────────────────────

/** 문자 탭 — 대화(스레드) 목록, 탭하면 대화 화면. [version] 변경 시 목록 재로딩. */
@Composable
private fun MessagesScreen(
    store: MessageStore,
    version: Long,
    nameFor: (String) -> String?,
    onSend: (String, String) -> Unit,
    onMarkRead: (String) -> Unit,
) {
    var openPeer by remember { mutableStateOf<String?>(null) }

    val peer = openPeer
    if (peer != null) {
        ConversationScreen(
            peer = peer,
            title = nameFor(peer) ?: peer,
            store = store,
            version = version,
            onSend = { text -> onSend(peer, text) },
            onBack = { openPeer = null },
        )
        // 대화 진입/새 문자 도착 시 읽음 처리
        LaunchedEffect(peer, version) { onMarkRead(peer) }
        return
    }

    val threads = remember(version) { store.threads() }
    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Text("문자", style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(vertical = 6.dp))
        if (threads.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("주고받은 문자가 없습니다.\n연락처 상세에서 문자를 보낼 수 있습니다.",
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else LazyColumn(Modifier.fillMaxSize()) {
            items(threads, key = { it.peer }) { t ->
                Row(
                    modifier = Modifier.fillMaxWidth().clickable { openPeer = t.peer }
                        .padding(vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(
                        Modifier.size(40.dp).clip(CircleShape)
                            .background(MaterialTheme.colorScheme.primaryContainer),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text((nameFor(t.peer) ?: t.peer).take(1),
                            color = MaterialTheme.colorScheme.onPrimaryContainer)
                    }
                    Spacer(Modifier.size(12.dp))
                    Column(Modifier.weight(1f)) {
                        Text(nameFor(t.peer) ?: fmtNumber(t.peer), style = MaterialTheme.typography.bodyLarge,
                            fontWeight = if (t.unread > 0) FontWeight.Bold else FontWeight.Normal)
                        Text(t.last.text, style = MaterialTheme.typography.bodySmall, maxLines = 1,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Column(horizontalAlignment = Alignment.End) {
                        Text(formatTime(t.last.time), style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (t.unread > 0) {
                            Spacer(Modifier.height(4.dp))
                            Badge { Text("${t.unread}") }
                        }
                    }
                }
                HorizontalDivider()
            }
        }
    }
}

/** 대화 화면 — 말풍선(수신 좌/발신 우) + 입력·전송. */
@Composable
private fun ConversationScreen(
    peer: String,
    title: String,
    store: MessageStore,
    version: Long,
    onSend: (String) -> Unit,
    onBack: () -> Unit,
) {
    val entries = remember(version) { store.thread(peer) }
    var input by remember { mutableStateOf("") }
    val listState = rememberLazyListState()
    LaunchedEffect(entries.size) {
        if (entries.isNotEmpty()) listState.scrollToItem(entries.size - 1)
    }

    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = onBack) { Text("←", fontSize = 20.sp) }
            Column {
                Text(title, style = MaterialTheme.typography.titleMedium)
                if (title != peer) Text(peer, style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        HorizontalDivider()
        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f).fillMaxWidth().padding(horizontal = 12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            items(entries) { e ->
                val incoming = e.direction == MsgDirection.IN
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = if (incoming) Arrangement.Start else Arrangement.End,
                ) {
                    Column(
                        Modifier.widthIn(max = 280.dp)
                            .clip(RoundedCornerShape(12.dp))
                            .background(
                                if (incoming) MaterialTheme.colorScheme.surfaceVariant
                                else MaterialTheme.colorScheme.primaryContainer,
                            )
                            .padding(horizontal = 12.dp, vertical = 8.dp),
                    ) {
                        Text(e.text, style = MaterialTheme.typography.bodyMedium)
                        Text(formatTime(e.time), style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
        Row(
            Modifier.fillMaxWidth().padding(8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedTextField(
                value = input, onValueChange = { input = it },
                modifier = Modifier.weight(1f), placeholder = { Text("문자 입력") },
                maxLines = 3,
            )
            Button(enabled = input.isNotBlank(), onClick = { onSend(input.trim()); input = "" }) {
                Text("전송")
            }
        }
    }
}

// ─────────────────────────────────────── 통화 화면 ───────────────────────────────────────

@Composable
private fun CallScreen(
    call: CallState,
    videoOn: Boolean,
    muted: Boolean,
    speakerOn: Boolean,
    onToggleVideo: (Boolean) -> Unit,
    onToggleMute: (Int, Boolean) -> Unit,
    onToggleSpeaker: (Boolean) -> Unit,
    onSurface: (Any?) -> Unit,
    onPreviewSurface: (Any?) -> Unit,
    onSwitchCamera: () -> Unit,
    onAnswer: (Int) -> Unit,
    onAnswerVideo: (Int) -> Unit,
    onReject: (Int) -> Unit,
    onHangup: (Int) -> Unit,
) {
    // 영상 통화 활성: 상대 영상 전체화면 + 화면 터치 시 오버레이 컨트롤(자동 숨김) + 내 화면 PiP.
    if (videoOn && call is CallState.Active) {
        VideoCallFullScreen(
            call = call,
            muted = muted,
            speakerOn = speakerOn,
            onToggleMute = onToggleMute,
            onToggleSpeaker = onToggleSpeaker,
            onToggleVideo = onToggleVideo,
            onSurface = onSurface,
            onPreviewSurface = onPreviewSurface,
            onSwitchCamera = onSwitchCamera,
            onHangup = onHangup,
        )
        return
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Spacer(Modifier.height(44.dp))
        val (remote, stateLine) = when (val c = call) {
            is CallState.Incoming -> extractNumber(c.remote) to (if (c.video) "영상 수신 전화" else "수신 전화")
            is CallState.Outgoing -> extractNumber(c.remote) to (if (videoOn) "영상 발신 중…" else "발신 중…")
            is CallState.Active -> extractNumber(c.remote) to "통화 중"
            else -> "" to ""
        }
        // 상단 중앙 — 상태 라벨(민트) + 이니셜 아바타 + 큰 번호 (수신 전체화면 참고 UX).
        Text(stateLine, color = Ct.Mint, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(20.dp))
        Box(
            Modifier.size(88.dp).clip(CircleShape).background(Ct.MintDim)
                .border(1.dp, Ct.Mint.copy(alpha = 0.4f), CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Text(fmtNumber(remote).take(1).ifBlank { "?" }, color = Ct.Mint,
                fontSize = 34.sp, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(18.dp))
        Text(fmtNumber(remote), style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))

        if (call is CallState.Active) {
            var elapsed by remember { mutableStateOf(0) }
            LaunchedEffect(Unit) { while (true) { delay(1000); elapsed++ } }
            Text("%02d:%02d".format(elapsed / 60, elapsed % 60),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // 영상: 상대 화면(수신 렌더) + 우하단 내 화면(로컬 카메라 프리뷰 PiP)
        if (videoOn && (call is CallState.Active || call is CallState.Outgoing)) {
            Spacer(Modifier.height(16.dp))
            Box(Modifier.fillMaxWidth().aspectRatio(4f / 3f)) {
                VideoRender(onSurface = onSurface)
                Box(
                    Modifier.align(Alignment.BottomEnd).padding(8.dp)
                        .width(100.dp).aspectRatio(3f / 4f),
                ) { PreviewRender(onSurface = onPreviewSurface) }
            }
        }

        Spacer(Modifier.weight(1f))

        when (val c = call) {
            is CallState.Incoming -> Row(
                modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly,
            ) {
                LabeledRound("거절", HANGUP_RED, Icons.Filled.CallEnd) { onReject(c.id) }
                LabeledRound("받기", CALL_GREEN, Icons.Filled.Call) { onAnswer(c.id) }
                if (c.video) LabeledRound("영상", VIDEO_BLUE, Icons.Filled.Videocam) { onAnswerVideo(c.id) }
            }
            is CallState.Active -> {
                // 통화중 컨트롤 — 음소거/스피커/영상 (토글 시 강조색)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                    ToggleRound("음소거", Icons.Filled.MicOff, muted) { onToggleMute(c.id, !muted) }
                    ToggleRound("스피커", Icons.AutoMirrored.Filled.VolumeUp, speakerOn) { onToggleSpeaker(!speakerOn) }
                    ToggleRound("영상", Icons.Filled.Videocam, videoOn, activeBg = VIDEO_BLUE) { onToggleVideo(!videoOn) }
                }
                Spacer(Modifier.height(24.dp))
                LabeledRound("종료", HANGUP_RED, Icons.Filled.CallEnd) { onHangup(c.id) }
            }
            is CallState.Outgoing -> LabeledRound("취소", HANGUP_RED, Icons.Filled.CallEnd) { onHangup(c.id) }
            else -> {}
        }
        Spacer(Modifier.height(24.dp))
    }
}

/**
 * 영상 통화 전체화면 — 상대 영상이 화면을 채우고, 화면을 터치하면 상단(번호/시간)·하단(컨트롤)
 * 오버레이가 나타났다 자동으로 사라진다(기본 전화/FaceTime 패턴). 우측 상단에 전면/후면 카메라
 * 전환 버튼, 우하단에 내 화면(로컬 카메라 프리뷰) PiP. 컨트롤: 음소거·스피커·영상·종료(아이콘만).
 */
@Composable
private fun VideoCallFullScreen(
    call: CallState.Active,
    muted: Boolean,
    speakerOn: Boolean,
    onToggleMute: (Int, Boolean) -> Unit,
    onToggleSpeaker: (Boolean) -> Unit,
    onToggleVideo: (Boolean) -> Unit,
    onSurface: (Any?) -> Unit,
    onPreviewSurface: (Any?) -> Unit,
    onSwitchCamera: () -> Unit,
    onHangup: (Int) -> Unit,
) {
    var controlsVisible by remember { mutableStateOf(true) }
    var elapsed by remember { mutableStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(1000); elapsed++ } }
    // 컨트롤 표시 후 4초 뒤 자동 숨김.
    LaunchedEffect(controlsVisible) {
        if (controlsVisible) { delay(4000); controlsVisible = false }
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable(
                indication = null,
                interactionSource = remember { MutableInteractionSource() },
            ) { controlsVisible = !controlsVisible },
    ) {
        // 상대 영상(전체화면)
        VideoRender(onSurface = onSurface)

        // 전면/후면 카메라 전환 — 우측 상단(항상 표시).
        Box(
            Modifier
                .align(Alignment.TopEnd)
                .padding(top = 48.dp, end = 12.dp)
                .size(44.dp)
                .clip(CircleShape)
                .background(Color.Black.copy(alpha = 0.4f))
                .clickable { onSwitchCamera() },
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Filled.Cameraswitch, contentDescription = "카메라 전환",
                tint = Color.White, modifier = Modifier.size(24.dp))
        }

        // 내 화면(로컬 카메라 프리뷰) PiP — 우하단(우측 버튼열 왼쪽), 라운드, 테두리 없음. 항상 표시.
        Box(
            Modifier
                .align(Alignment.BottomEnd)
                .padding(bottom = 24.dp, end = 76.dp)
                .width(96.dp).aspectRatio(3f / 4f)
                .clip(RoundedCornerShape(12.dp))
                .background(Color(0xFF222222)),
        ) { PreviewRender(onSurface = onPreviewSurface) }

        // 오버레이(터치 시 표시) — 상단 정보(좌) + 우하단 컨트롤 세로열(아이콘만).
        if (controlsVisible) {
            Column(
                Modifier
                    .align(Alignment.TopStart)
                    .padding(top = 44.dp, start = 20.dp, end = 72.dp),
            ) {
                Text(fmtNumber(extractNumber(call.remote)), color = Color.White,
                    style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Text("%02d:%02d".format(elapsed / 60, elapsed % 60),
                    color = Color.White.copy(alpha = 0.85f), style = MaterialTheme.typography.bodyMedium)
            }
            Column(
                Modifier
                    .align(Alignment.BottomEnd)
                    .padding(end = 8.dp, bottom = 24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                ToggleRound("음소거", Icons.Filled.MicOff, muted, size = 52.dp, showLabel = false) { onToggleMute(call.id, !muted) }
                ToggleRound("스피커", Icons.AutoMirrored.Filled.VolumeUp, speakerOn, size = 52.dp, showLabel = false) { onToggleSpeaker(!speakerOn) }
                ToggleRound("영상", Icons.Filled.Videocam, true, activeBg = VIDEO_BLUE, size = 52.dp, showLabel = false) { onToggleVideo(false) }
                LabeledRound("종료", HANGUP_RED, Icons.Filled.CallEnd, size = 52.dp, showLabel = false) { onHangup(call.id) }
            }
        }
    }
}

@Composable
private fun LabeledRound(label: String, bg: Color, icon: ImageVector, fg: Color = Color.White, enabled: Boolean = true, size: Dp = 72.dp, showLabel: Boolean = true, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            modifier = Modifier
                .size(size)
                .clip(CircleShape)
                .background(if (enabled) bg else bg.copy(alpha = 0.35f))
                .clickable(enabled = enabled) { onClick() },
            contentAlignment = Alignment.Center,
        ) {
            Icon(icon, contentDescription = label, tint = fg, modifier = Modifier.size(size * 0.42f))
        }
        if (showLabel) {
            Spacer(Modifier.height(6.dp))
            Text(label, style = MaterialTheme.typography.labelMedium)
        }
    }
}

/** 통화중 토글 버튼 — off=회색, on=강조색(기본 전화앱의 음소거/스피커 토글과 동일한 패턴). */
@Composable
private fun ToggleRound(
    label: String,
    icon: ImageVector,
    active: Boolean,
    activeBg: Color = TOGGLE_ACTIVE,
    size: Dp = 72.dp,
    showLabel: Boolean = true,
    onClick: () -> Unit,
) {
    LabeledRound(
        label = label,
        bg = if (active) activeBg else MaterialTheme.colorScheme.surfaceVariant,
        icon = icon,
        fg = if (active) Color.White else MaterialTheme.colorScheme.onSurface,
        size = size,
        showLabel = showLabel,
        onClick = onClick,
    )
}

@Composable
private fun VideoRender(onSurface: (Any?) -> Unit) {
    // SurfaceView 의 Surface 를 PJSIP 영상 윈도우로 전달. 컴포지션 이탈 시 surfaceDestroyed→null.
    AndroidView(
        modifier = Modifier.fillMaxSize(),
        factory = { ctx ->
            SurfaceView(ctx).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(h: SurfaceHolder) = onSurface(h.surface)
                    override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) = onSurface(h.surface)
                    override fun surfaceDestroyed(h: SurfaceHolder) = onSurface(null)
                })
            }
        },
    )
}

@Composable
private fun PreviewRender(onSurface: (Any?) -> Unit) {
    // 로컬 카메라 프리뷰(내 화면). SurfaceView 는 별도 서피스라 Compose .clip()/clipToOutline 로
    // 모서리 라운드가 안 되므로 TextureView 를 쓴다(뷰 계층 텍스처 → 부모 clip 으로 라운드 적용).
    AndroidView(
        modifier = Modifier.fillMaxSize().clip(RoundedCornerShape(12.dp)),
        factory = { ctx ->
            TextureView(ctx).apply {
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) = onSurface(Surface(st))
                    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
                    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { onSurface(null); return true }
                    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                }
            }
        },
    )
}

/** SIP URI("\"이름\" <sip:번호@도메인>")에서 표시용 번호만 추출. 패턴이 없으면 원문 반환. */
private fun extractNumber(remote: String): String {
    var s = remote.trim()
    if (s.contains("<") && s.contains(">")) s = s.substringAfter("<").substringBefore(">")
    s = s.removePrefix("sip:").removePrefix("sips:").removePrefix("tel:")
    s = s.substringBefore("@").substringBefore(";")
    return s.ifBlank { remote }
}

/** 홈 국가코드 — 프로비저닝 응답 countryCode(HomeScreen 진입 시 설정). null 이면 축약 없음. */
private var homeCountryCode: String? = null

/** ITU 자릿수 규칙 E.164 국가코드 추정 — 프로비저닝 미수신(구서버) fallback 전용.
 *  1(NANP)/7=1자리, 유효 2자리 셋, 그 외 3자리. */
private fun countryCodeOf(msisdn: String): String? {
    val d = msisdn.trim().removePrefix("tel:").removePrefix("+").filter { it.isDigit() }
    if (d.length < 4) return null
    if (d[0] == '1' || d[0] == '7') return d.take(1)
    val two = d.take(2)
    val twoDigit = setOf(
        "20", "27", "30", "31", "32", "33", "34", "36", "39", "40", "41", "43", "44", "45",
        "46", "47", "48", "49", "51", "52", "53", "54", "55", "56", "57", "58", "60", "61",
        "62", "63", "64", "65", "66", "81", "82", "84", "86", "90", "91", "92", "93", "94",
        "95", "98",
    )
    return if (two in twoDigit) two else d.take(3)
}

/** 이름 + 전화번호 병기 (예: "테스트001 (01300000001)") — 키패드·통화이력 헤더 표기. */
private fun myLine(config: SipAccountConfig): String = when {
    config.displayName.isBlank() -> fmtNumber(config.msisdn)
    config.msisdn.isBlank() -> config.displayName
    else -> "${config.displayName} (${fmtNumber(config.msisdn)})"
}

/** 홈 국가코드(+82 등)와 같은 국제표기 번호는 로컬 표기(0…)로 축약. 타국 번호는 그대로. */
private fun fmtNumber(number: String): String {
    val cc = homeCountryCode ?: return number
    val n = number.trim().removePrefix("tel:")
    val digits = n.removePrefix("+")
    return if (n.startsWith("+") && digits.startsWith(cc)) "0" + digits.removePrefix(cc) else number
}

private fun formatTime(ts: Long): String =
    SimpleDateFormat("M월 d일 a h:mm", Locale.KOREA).format(java.util.Date(ts))

private fun requiredPermissions(): Array<String> = buildList {
    add(Manifest.permission.RECORD_AUDIO)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        add(Manifest.permission.POST_NOTIFICATIONS)
    }
}.toTypedArray()

// 통화 기능색 — 받기/통화=초록, 종료/거절=빨강, 영상=파랑 (전화 관습색은 유지, 그 외 톤은 Ct 토큰).
private val CALL_GREEN = Color(0xFF00C853)
private val HANGUP_RED = Color(0xFFEA4335)
private val VIDEO_BLUE = Color(0xFF4285F4)
private val FAV_GOLD = Color(0xFFF9A825)
private val TOGGLE_ACTIVE = Color(0xFF5F6368)   // 음소거/스피커 토글 on (다이얼러 회색 강조)

// DTMF 터치 톤 — 기본 다이얼러와 유사한 볼륨(0~100)/길이(ms).
private const val DTMF_TONE_VOLUME = 80
private const val DTMF_TONE_MS = 120

// ─────────────────────── 설정 화면 (안드로이드 설정 스타일 — 카테고리 + 항목행 + 편집 다이얼로그) ───────────────────────

/**
 * 접속/계정 설정. 값의 SoT 는 CIMS 프로비저닝 — SSO 자동 구성 상태에서는 **읽기 전용**으로
 * 보여주고, "수동 설정 모드" 스위치를 켠 경우에만 편집을 허용한다(테스트용, 프로비저닝
 * 덮어쓰기 중지). 항목 변경은 즉시 [onApply](저장+재등록)로 반영 — 별도 저장 버튼 없음.
 *
 * [standalone] = GATE 수동 진입(전체화면): 하단 완료/취소 버튼 표시, 수동 모드 고정.
 */
@Composable
private fun SettingsScreen(
    config: SipAccountConfig,
    standalone: Boolean,
    onApply: (SipAccountConfig) -> Unit,
    onDone: () -> Unit = {},
    onCancel: () -> Unit = {},
) {
    val context = LocalContext.current
    val store = remember { ConfigStore(context) }
    val hasSso = remember { SsoProvisioner.hasAccount(context) }
    // 편집 가능 = 수동 모드(또는 GATE 수동 진입, CIMS 계정 자체가 없는 단말).
    var manual by remember { mutableStateOf(store.isManual() || standalone || !hasSso) }
    var reprovisioning by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    Column(Modifier.fillMaxSize()) {
        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState()),
        ) {
            Text(
                "설정", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(start = 20.dp, top = 20.dp, bottom = 4.dp),
            )

            PrefCategory("구성")
            if (hasSso && !standalone) {
                if (!manual) {
                    PrefRow(
                        title = "CIMS 계정으로 자동 구성됨",
                        summary = "아래 값은 서버 프로비저닝이 관리하며 앱 진입 시 자동 갱신됩니다.",
                    )
                }
                PrefSwitchRow(
                    title = "수동 설정 모드",
                    summary = if (manual) "자동 구성을 중지하고 아래 값을 직접 편집합니다(테스트용). " +
                        "끄면 CIMS 프로비저닝 값으로 복원됩니다."
                    else "테스트용 — 켜면 자동 구성을 중지하고 직접 편집할 수 있습니다.",
                    checked = manual,
                    enabled = !reprovisioning,
                ) { on ->
                    if (on) {
                        store.setManual(true); manual = true
                    } else {
                        // 끄기 = 즉시 재프로비저닝으로 서버 값 복원(실패 시 기존 값 유지, 다음 진입 시 복원).
                        store.setManual(false); manual = false; reprovisioning = true
                        scope.launch {
                            val cfg = runCatching {
                                withContext(Dispatchers.IO) {
                                    val prof = SsoProvisioner.fetchProfile(context)
                                        ?: error("CIMS 로그인 세션이 없습니다")
                                    val svc = prof.service("volte") ?: error("VoLTE 서비스가 없습니다")
                                    svc.toSipAccountConfig(
                                        loginId = prof.loginId ?: "",
                                        displayName = prof.displayName ?: "",
                                        loginPassword = SsoProvisioner.loginPassword(context),
                                        countryCode = prof.countryCode.orEmpty(),
                                    )
                                }
                            }.getOrNull()
                            reprovisioning = false
                            cfg?.let { store.saveProvisioned(it); onApply(store.load()) }
                        }
                    }
                }
                if (reprovisioning) {
                    PrefRow(title = "CIMS 프로비저닝 값으로 복원 중…", summary = null)
                }
            } else {
                PrefRow(
                    title = "수동 구성",
                    summary = if (hasSso) "CIMS 자동 구성 대신 직접 입력한 값을 사용합니다."
                    else "CIMS 계정 없음 — 직접 입력한 값을 사용합니다.",
                )
            }

            PrefCategory("서버")
            PrefTextRow("서버 주소", config.serverHost, manual) { onApply(config.copy(serverHost = it)) }
            PrefTextRow("SIP 포트", if (config.serverPort > 0) config.serverPort.toString() else "", manual,
                digitsOnly = true) { onApply(config.copy(serverPort = it.toIntOrNull() ?: 0)) }
            // 전송 프로토콜 — **SSO 자동 구성 상태에서도 고를 수 있다.** 서버가 알린 가용 목록이 곧
            //   선택지이고, 서버 설정은 강제가 아니라 기본값(권장)이다(sip_tls_signaling.md §7.1).
            //   목록을 안 주는 구 서버면 수동 모드에서만 편집(전 항목 노출, 포트는 위 행에서 직접 입력).
            val tpOptions = config.transports.map { it.transport }
            PrefChoiceRow(
                title = "전송 프로토콜",
                value = config.transport,
                enabled = manual || tpOptions.size >= 2,
                options = tpOptions.ifEmpty { SipAccountConfig.Transport.entries.toList() },
                labelOf = { t ->
                    config.transports.firstOrNull { it.transport == t }?.let { "${t.name} · ${it.port}" } ?: t.name
                },
            ) { t ->
                // 선택 즉시 저장 — transport 와 포트가 함께 바뀌고, 사용자 선택 표시가 남아
                //   다음 프로비저닝 재취득에도 유지된다(ConfigStore.saveTransportChoice).
                store.saveTransportChoice(t)
                onApply(store.load())
            }
            PrefTextRow("서비스 도메인", config.domain, manual) { onApply(config.copy(domain = it)) }

            PrefCategory("계정")
            PrefTextRow("이름", config.displayName, manual) { onApply(config.copy(displayName = it)) }
            PrefTextRow("내 번호 (MSISDN)", config.msisdn, manual) { onApply(config.copy(msisdn = it)) }
            PrefTextRow("IMSI", config.imsi, manual, digitsOnly = true) { onApply(config.copy(imsi = it)) }
            PrefTextRow("SIP 비밀번호", config.password, manual, isPassword = true) {
                onApply(config.copy(password = it))
            }

            PrefCategory("고급")
            PrefTextRow("인증 ID (전체 IMPI)", config.authId, manual,
                summaryOverride = config.authId.ifBlank { "미지정 — IMSI@도메인 자동 합성" }) {
                onApply(config.copy(authId = it))
            }
            Text(
                "공개 ID(MSISDN)와 인증 ID(IMSI@도메인)는 다른 값입니다. 서버는 Digest username 으로 " +
                    "IMSI@도메인 정확 일치를 요구하며, 불일치 시 즉시 403 으로 거부합니다.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            )
        }

        if (standalone) {
            if (!config.isComplete()) {
                Text(
                    "필수: 서버/포트/도메인/내 번호/IMSI(또는 인증 ID)/비밀번호",
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(horizontal = 20.dp),
                )
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Button(onClick = onDone, enabled = config.isComplete()) { Text("완료") }
                OutlinedButton(onClick = onCancel) { Text("취소") }
            }
        }
    }
}

/** 카테고리 라벨 — 안드로이드 설정의 굵은 primary 소제목. */
@Composable
private fun PrefCategory(title: String) {
    Text(
        title, style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(start = 20.dp, top = 20.dp, bottom = 4.dp),
    )
}

/** 설정 항목 행 — 제목 + 아래 회색 요약(현재값). [enabled]=false 면 흐리게·클릭 불가. */
@Composable
private fun PrefRow(
    title: String,
    summary: String?,
    enabled: Boolean = true,
    onClick: (() -> Unit)? = null,
    trailing: @Composable (() -> Unit)? = null,
) {
    val titleColor = MaterialTheme.colorScheme.onSurface.copy(alpha = if (enabled) 1f else 0.38f)
    val summaryColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.38f)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .let { if (onClick != null && enabled) it.clickable(onClick = onClick) else it }
            .padding(horizontal = 20.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, color = titleColor)
            if (!summary.isNullOrBlank()) {
                Text(summary, style = MaterialTheme.typography.bodyMedium, color = summaryColor)
            }
        }
        trailing?.invoke()
    }
}

/** 스위치 항목 행. */
@Composable
private fun PrefSwitchRow(
    title: String,
    summary: String?,
    checked: Boolean,
    enabled: Boolean = true,
    onToggle: (Boolean) -> Unit,
) {
    PrefRow(title, summary, enabled, onClick = { onToggle(!checked) }) {
        Switch(checked = checked, onCheckedChange = onToggle, enabled = enabled)
    }
}

/** 문자열 항목 행 — 탭하면 편집 다이얼로그(EditTextPreference 스타일), 확인 시 즉시 적용. */
@Composable
private fun PrefTextRow(
    title: String,
    value: String,
    enabled: Boolean,
    isPassword: Boolean = false,
    digitsOnly: Boolean = false,
    summaryOverride: String? = null,
    onChange: (String) -> Unit,
) {
    var editing by remember { mutableStateOf(false) }
    val summary = summaryOverride ?: when {
        value.isBlank() -> "미설정"
        isPassword -> "••••••••"
        else -> value
    }
    PrefRow(title, summary, enabled, onClick = { editing = true })
    if (editing) {
        var text by remember(editing) { mutableStateOf(value) }
        AlertDialog(
            onDismissRequest = { editing = false },
            title = { Text(title) },
            text = {
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = if (digitsOnly) it.filter { ch -> ch.isDigit() } else it },
                    singleLine = true,
                    visualTransformation = if (isPassword) PasswordVisualTransformation() else VisualTransformation.None,
                    modifier = Modifier.fillMaxWidth(),
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    onChange(if (isPassword) text else text.trim()); editing = false
                }) { Text("확인") }
            },
            dismissButton = { TextButton(onClick = { editing = false }) { Text("취소") } },
        )
    }
}

/** 선택 항목 행 — 탭하면 라디오 목록 다이얼로그(ListPreference 스타일). */
@Composable
private fun PrefChoiceRow(
    title: String,
    value: SipAccountConfig.Transport,
    enabled: Boolean,
    options: List<SipAccountConfig.Transport>,
    labelOf: (SipAccountConfig.Transport) -> String = { it.name },
    onChange: (SipAccountConfig.Transport) -> Unit,
) {
    var choosing by remember { mutableStateOf(false) }
    PrefRow(title, labelOf(value), enabled, onClick = { choosing = true })
    if (choosing) {
        AlertDialog(
            onDismissRequest = { choosing = false },
            title = { Text(title) },
            text = {
                Column {
                    options.forEach { t ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable { onChange(t); choosing = false }
                                .padding(vertical = 6.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = value == t, onClick = { onChange(t); choosing = false })
                            Text(labelOf(t), style = MaterialTheme.typography.bodyLarge)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { choosing = false }) { Text("취소") } },
        )
    }
}
