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
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
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
import com.cims.ue.core.sip.RegState
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
        handleIntent(intent)
        setContent {
            MaterialTheme {
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
    override fun onResume() {
        super.onResume()
        if (com.cims.ue.core.account.SsoProvisioner.hasAccount(this) || ConfigStore(this).load().isComplete()) {
            runCatching { SipService.poke(this) }
        }
    }
}

private enum class Screen { GATE, HOME, CONFIG }
private enum class Tab { CONTACTS, RECENTS, KEYPAD, MESSAGES }

@Composable
private fun App(
    notifAnswer: MutableStateFlow<Pair<Int, Boolean>?>,
    notifOpenMessages: MutableStateFlow<Boolean>,
) {
    val context = LocalContext.current
    val store = remember { ConfigStore(context) }
    var config by remember { mutableStateOf(store.load()) }
    // 공유 계정 있으면 진입 시 항상 최신 정보 재취득(GATE→재프로비저닝). 계정 없으면 캐시 설정으로 HOME.
    var screen by remember { mutableStateOf(
        if (com.cims.ue.core.account.SsoProvisioner.hasAccount(context)) Screen.GATE
        else if (config.isComplete()) Screen.HOME else Screen.GATE
    ) }

    when (screen) {
        // CIMS-Phone 는 자체 로그인 없음 — CIMS 공유 계정으로 자동 구성(SSO). 계정 없으면 CIMS 앱 로그인 유도.
        Screen.GATE -> SsoGateScreen(
            onProvisioned = { c -> store.save(c); config = c; screen = Screen.HOME },
            onManual = { screen = Screen.CONFIG },
        )
        Screen.CONFIG -> ConfigScreen(
            initial = config,
            canCancel = config.isComplete(),
            onSave = { c -> store.save(c); config = c; screen = Screen.HOME },
            onCancel = { screen = if (config.isComplete()) Screen.HOME else Screen.GATE },
        )
        Screen.HOME -> HomeScreen(
            config = config,
            onEditConfig = { screen = Screen.CONFIG },
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
                    )
                }
            }
            busy = false
            cfg.onSuccess { onProvisioned(it) }.onFailure { status = it.message ?: "구성 실패" }
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
    onEditConfig: () -> Unit,
    notifAnswer: MutableStateFlow<Pair<Int, Boolean>?>,
    notifOpenMessages: MutableStateFlow<Boolean>,
) {
    val context = LocalContext.current
    val callLog = remember { CallLogStore(context) }
    val contacts = remember { ContactStore(context) }
    val companyDir = remember { CompanyDirectoryStore(context) }
    val favorites = remember { FavoriteStore(context) }

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

    val fallbackReg = remember { MutableStateFlow<RegState>(RegState.Idle) }
    val fallbackCall = remember { MutableStateFlow<CallState>(CallState.Null) }
    val reg by (service?.regState ?: fallbackReg).collectAsState()
    val call by (service?.callState ?: fallbackCall).collectAsState()

    // 발신: 음성/영상 구분. 영상은 카메라 권한 확보 후 발신.
    var videoOn by remember { mutableStateOf(false) }
    var pendingVideoNumber by remember { mutableStateOf<String?>(null) }

    fun doDial(number: String, video: Boolean) {
        val n = number.trim()
        if (n.isBlank()) return
        videoOn = video
        service?.setVideoEnabled(video)
        callLog.add(extractNumber(n), CallType.OUTGOING)
        service?.makeCall(n)
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

    // 수신/부재중 기록: Incoming→Active=수신(연결), Incoming→Disconnected(미연결)=부재중.
    var incomingNumber by remember { mutableStateOf<String?>(null) }
    var incomingAnswered by remember { mutableStateOf(false) }
    LaunchedEffect(call) {
        when (val c = call) {
            is CallState.Incoming -> { incomingNumber = extractNumber(c.remote); incomingAnswered = false }
            is CallState.Active -> if (incomingNumber != null) incomingAnswered = true
            is CallState.Disconnected -> {
                muted = false; speakerOn = false
                incomingNumber?.let { n ->
                    callLog.add(n, if (incomingAnswered) CallType.INCOMING else CallType.MISSED)
                    incomingNumber = null
                }
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
            onAnswer = { id -> answerCall(id, false) },
            onAnswerVideo = { id -> answerCall(id, true) },
            onReject = { id -> service?.reject(id) },
            onHangup = { id -> service?.hangup(id) },
        )
    } else {
        val msgStore = remember { MessageStore(context) }
        val unread = remember(msgVersion) { msgStore.unreadTotal() }
        Scaffold(
            topBar = { HeaderBar(reg, onEditConfig) },
            bottomBar = { BottomNav(tab, unread) { tab = it } },
        ) { pad ->
            Box(Modifier.padding(pad).fillMaxSize()) {
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
                        onCall = { dial(it, false) },
                    )
                    Tab.KEYPAD -> KeypadScreen(
                        // 이름 + 전화번호 병기 (예: "테스트001 (+821300000001)")
                        myNumber = when {
                            config.displayName.isBlank() -> config.msisdn
                            config.msisdn.isBlank() -> config.displayName
                            else -> "${config.displayName} (${config.msisdn})"
                        },
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
                }
            }
        }
    }
}

@Composable
private fun HeaderBar(reg: RegState, onSettings: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 12.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        RegStatusChip(reg)
        TextButton(onClick = onSettings) { Text("설정") }
    }
}

@Composable
private fun BottomNav(current: Tab, unread: Int, onSelect: (Tab) -> Unit) {
    NavigationBar {
        NavigationBarItem(
            selected = current == Tab.CONTACTS, onClick = { onSelect(Tab.CONTACTS) },
            icon = { Text("👤", fontSize = 20.sp) }, label = { Text("연락처") },
        )
        NavigationBarItem(
            selected = current == Tab.RECENTS, onClick = { onSelect(Tab.RECENTS) },
            icon = { Text("🕘", fontSize = 20.sp) }, label = { Text("최근기록") },
        )
        NavigationBarItem(
            selected = current == Tab.KEYPAD, onClick = { onSelect(Tab.KEYPAD) },
            icon = { Text("⌨", fontSize = 20.sp) }, label = { Text("키패드") },
        )
        NavigationBarItem(
            selected = current == Tab.MESSAGES, onClick = { onSelect(Tab.MESSAGES) },
            icon = {
                BadgedBox(badge = { if (unread > 0) Badge { Text("$unread") } }) {
                    Text("✉", fontSize = 20.sp)
                }
            },
            label = { Text("문자") },
        )
    }
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

        // 입력 번호 + 지우기
        Row(modifier = Modifier.fillMaxWidth().height(64.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(56.dp))
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Text(dialed, style = MaterialTheme.typography.displaySmall,
                    color = MaterialTheme.colorScheme.onSurface, maxLines = 1)
            }
            Box(Modifier.size(56.dp), contentAlignment = Alignment.Center) {
                if (dialed.isNotEmpty()) {
                    TextButton(onClick = { dialed = dialed.dropLast(1) }) { Text("⌫", fontSize = 24.sp) }
                }
            }
        }

        Spacer(Modifier.height(8.dp))
        Keypad(onDigit = { dialed += it; playDtmf(it) })
        Spacer(Modifier.height(20.dp))

        // 음성/영상 발신 구분
        Row(horizontalArrangement = Arrangement.spacedBy(40.dp)) {
            LabeledRound("음성", CALL_GREEN, "📞", enabled = dialed.isNotBlank()) { onVoice(dialed.trim()) }
            LabeledRound("영상", VIDEO_BLUE, "📹", enabled = dialed.isNotBlank()) { onVideo(dialed.trim()) }
        }
        Spacer(Modifier.height(20.dp))
    }
}

@Composable
private fun Keypad(onDigit: (String) -> Unit) {
    val rows = listOf(
        listOf("1" to "", "2" to "ABC", "3" to "DEF"),
        listOf("4" to "GHI", "5" to "JKL", "6" to "MNO"),
        listOf("7" to "PQRS", "8" to "TUV", "9" to "WXYZ"),
        listOf("*" to "", "0" to "+", "#" to ""),
    )
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        rows.forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(28.dp)) {
                row.forEach { (digit, letters) -> KeypadKey(digit, letters) { onDigit(digit) } }
            }
        }
    }
}

@Composable
private fun KeypadKey(digit: String, letters: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .size(64.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .clickable { onClick() },
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(digit, fontSize = 26.sp, color = MaterialTheme.colorScheme.onSurface)
            if (letters.isNotBlank()) {
                Text(letters, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
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

    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            SegTab("즐겨찾기", seg == 0, Modifier.weight(1f)) { seg = 0 }
            SegTab("회사", seg == 1, Modifier.weight(1f)) { seg = 1 }
            SegTab("개인", seg == 2, Modifier.weight(1f)) { seg = 2 }
        }
        when (seg) {
            0 -> FavoritesScreen(favorites, favVersion, onOpen = { detail = it }, onFavChanged = { favVersion++ })
            1 -> CompanyContacts(company, favorites, favVersion, onOpen = { detail = it }, onFavChanged = { favVersion++ })
            else -> PersonalContacts(personal, favorites, favVersion, onOpen = { detail = it }, onFavChanged = { favVersion++ })
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

@Composable
private fun SegTab(label: String, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val bg = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant
    val fg = if (selected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant
    Box(
        modifier.clip(CircleShape).background(bg).clickable { onClick() }.padding(vertical = 8.dp),
        contentAlignment = Alignment.Center,
    ) { Text(label, color = fg, style = MaterialTheme.typography.labelLarge) }
}

/** 공용 연락처 행 — 별표 토글 + 탭(상세). [trailing] 으로 추가 버튼(개인=수정). */
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
            Modifier.size(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primaryContainer),
            contentAlignment = Alignment.Center,
        ) { Text(name.take(1).ifBlank { "?" }, color = MaterialTheme.colorScheme.onPrimaryContainer) }
        Spacer(Modifier.size(12.dp))
        Column(Modifier.weight(1f)) {
            Text(name.ifBlank { line2 }, style = MaterialTheme.typography.bodyLarge)
            Text(line2, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        TextButton(onClick = onToggleFav) {
            Text(if (isFav) "★" else "☆", fontSize = 20.sp,
                color = if (isFav) FAV_GOLD else MaterialTheme.colorScheme.onSurfaceVariant)
        }
        trailing?.invoke()
    }
}

// ── 즐겨찾기 ──
@Composable
private fun FavoritesScreen(
    favorites: FavoriteStore, favVersion: Int,
    onOpen: (DetailTarget) -> Unit, onFavChanged: () -> Unit,
) {
    val list = remember(favVersion) { favorites.all() }
    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Text("즐겨찾기", style = MaterialTheme.typography.titleMedium,
            modifier = Modifier.padding(vertical = 6.dp))
        if (list.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("즐겨찾기가 없습니다.\n연락처에서 ★ 로 추가하세요.",
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else LazyColumn(Modifier.fillMaxSize()) {
            items(list, key = { it.number }) { f ->
                ContactListRow(f.name, f.number, depth = 0, isFav = true,
                    onTap = { onOpen(DetailTarget(f.name, f.number, null)) },
                    onToggleFav = { favorites.toggle(f.name, f.number); onFavChanged() })
                HorizontalDivider()
            }
        }
    }
}

/** 회사 연락처 — 서버 프로비저닝 제공, 읽기전용. 조직 트리 + 검색 + 동기화(버전 기반). */
@Composable
private fun CompanyContacts(
    store: CompanyDirectoryStore, favorites: FavoriteStore, favVersion: Int,
    onOpen: (DetailTarget) -> Unit, onFavChanged: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var dir by remember { mutableStateOf(store.load()) }
    var lastSync by remember { mutableStateOf(store.lastSyncedAt()) }
    var loading by remember { mutableStateOf(false) }
    var note by remember { mutableStateOf("") }
    var query by remember { mutableStateOf("") }
    var collapsed by remember { mutableStateOf(setOf<String>()) }
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

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("회사 전화번호부", style = MaterialTheme.typography.titleMedium)
                Text(
                    (if (lastSync > 0) "마지막 동기화: ${formatTime(lastSync)}" else "동기화 안 됨") +
                        (if (note.isNotBlank()) " · $note" else ""),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (loading) CircularProgressIndicator(Modifier.size(20.dp))
            else TextButton(onClick = { sync() }) { Text("동기화") }
        }
        OutlinedTextField(
            value = query, onValueChange = { query = it },
            label = { Text("이름·번호 검색") }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        )

        val q = query.trim()
        val orgName = remember(dir) { dir.orgs.associate { it.code to it.name } }
        if (dir.members.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(note.ifBlank { "회사 연락처가 없습니다." },
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
                    ContactListRow(m.name, "${m.number} · $on", depth = 0, isFav = m.number in favSet,
                        onTap = { onOpen(DetailTarget(m.name, m.number, on)) },
                        onToggleFav = { favorites.toggle(m.name, m.number); onFavChanged() })
                    HorizontalDivider()
                }
            }
        } else {
            val rows = remember(dir, collapsed) { buildDirRows(dir, collapsed) }
            LazyColumn(Modifier.fillMaxSize()) {
                items(rows, key = { it.key }) { row ->
                    when (row) {
                        is DirRow.Org -> OrgHeaderRow(row) {
                            collapsed = if (row.org.code in collapsed) collapsed - row.org.code else collapsed + row.org.code
                        }
                        is DirRow.Member -> {
                            ContactListRow(row.c.name, row.c.number, depth = row.depth, isFav = row.c.number in favSet,
                                onTap = { onOpen(DetailTarget(row.c.name, row.c.number, orgName[row.c.orgCode])) },
                                onToggleFav = { favorites.toggle(row.c.name, row.c.number); onFavChanged() })
                            HorizontalDivider()
                        }
                    }
                }
            }
        }
    }
}

/** 트리 한 행 — 조직 헤더 또는 가입자. */
private sealed interface DirRow {
    val key: String
    data class Org(val org: CompanyOrg, val depth: Int, val expanded: Boolean, val count: Int) : DirRow {
        override val key get() = "org:${org.code}"
    }
    data class Member(val c: CompanyContact, val depth: Int) : DirRow {
        override val key get() = "mem:${c.orgCode}:${c.number}"
    }
}

/** 조직 트리를 펼침 상태에 맞춰 평면 행 목록으로 전개. */
private fun buildDirRows(dir: CompanyDirectory, collapsed: Set<String>): List<DirRow> {
    val orgByCode = dir.orgs.associateBy { it.code }
    val byParent = dir.orgs.groupBy { it.parent }
    val membersByOrg = dir.members.groupBy { it.orgCode }
    val countCache = HashMap<String, Int>()
    fun subtreeCount(code: String): Int = countCache.getOrPut(code) {
        (membersByOrg[code]?.size ?: 0) + (byParent[code]?.sumOf { subtreeCount(it.code) } ?: 0)
    }
    val out = ArrayList<DirRow>()
    fun walk(org: CompanyOrg, depth: Int) {
        val expanded = org.code !in collapsed
        out.add(DirRow.Org(org, depth, expanded, subtreeCount(org.code)))
        if (expanded) {
            byParent[org.code]?.sortedBy { it.sort }?.forEach { walk(it, depth + 1) }
            membersByOrg[org.code]?.sortedBy { it.name }?.forEach { out.add(DirRow.Member(it, depth + 1)) }
        }
    }
    dir.orgs.filter { it.parent.isBlank() || it.parent !in orgByCode }.sortedBy { it.sort }.forEach { walk(it, 0) }
    val orphan = dir.members.filter { it.orgCode.isBlank() || it.orgCode !in orgByCode }
    if (orphan.isNotEmpty()) {
        out.add(DirRow.Org(CompanyOrg("", "(조직 미지정)", "", 9999), 0, true, orphan.size))
        orphan.sortedBy { it.name }.forEach { out.add(DirRow.Member(it, 1)) }
    }
    return out
}

@Composable
private fun OrgHeaderRow(row: DirRow.Org, onToggle: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onToggle() }
            .padding(start = (row.depth * 16).dp, top = 10.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(if (row.expanded) "▾" else "▸", color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.size(20.dp), textAlign = TextAlign.Center)
        Spacer(Modifier.size(4.dp))
        Text("${row.org.name} (${row.count})", style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary)
    }
}

/** 개인 연락처 — 단말 로컬, 추가/수정/삭제 가능 + 검색. */
@Composable
private fun PersonalContacts(
    store: ContactStore, favorites: FavoriteStore, favVersion: Int,
    onOpen: (DetailTarget) -> Unit, onFavChanged: () -> Unit,
) {
    var list by remember { mutableStateOf(store.all()) }
    var editing by remember { mutableStateOf<Contact?>(null) }
    var showAdd by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    val favSet = remember(favVersion) { favorites.all().map { it.number }.toSet() }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("개인 연락처", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { showAdd = true }) { Text("+ 추가") }
        }
        OutlinedTextField(
            value = query, onValueChange = { query = it },
            label = { Text("이름·번호 검색") }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        )
        val q = query.trim()
        val shown = if (q.isBlank()) list else list.filter { it.name.contains(q, true) || it.number.contains(q) }
        if (shown.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (list.isEmpty()) "저장된 개인 연락처가 없습니다.\n‘+ 추가’ 로 등록하세요." else "검색 결과가 없습니다.",
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else LazyColumn(Modifier.fillMaxSize()) {
            items(shown, key = { it.id }) { c ->
                ContactListRow(c.name, c.number, depth = 0, isFav = c.number in favSet,
                    onTap = { onOpen(DetailTarget(c.name, c.number, null)) },
                    onToggleFav = { favorites.toggle(c.name, c.number); onFavChanged() },
                    trailing = { TextButton(onClick = { editing = c }) { Text("수정") } })
                HorizontalDivider()
            }
        }
    }

    if (showAdd) {
        ContactDialog(null,
            onDismiss = { showAdd = false },
            onSave = { name, num -> store.upsert(name, num); list = store.all(); showAdd = false })
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

/** 연락처 상세 — 정보 + 음성/영상/문자/즐겨찾기. 행을 누르면 표시된다. */
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

    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surface) {
            Column(Modifier.padding(20.dp).widthIn(max = 360.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                // 헤더
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        Modifier.size(48.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primaryContainer),
                        contentAlignment = Alignment.Center,
                    ) { Text(target.name.take(1).ifBlank { "?" }, color = MaterialTheme.colorScheme.onPrimaryContainer) }
                    Spacer(Modifier.size(12.dp))
                    Column(Modifier.weight(1f)) {
                        Text(target.name.ifBlank { target.number }, style = MaterialTheme.typography.titleLarge)
                        Text(target.number + (target.org?.let { " · $it" } ?: ""),
                            style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }

                if (!composing) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                        LabeledRound("음성", CALL_GREEN, "📞") { onVoice() }
                        LabeledRound("영상", VIDEO_BLUE, "📹") { onVideo() }
                        LabeledRound("문자", MaterialTheme.colorScheme.surfaceVariant, "✉",
                            fg = MaterialTheme.colorScheme.onSurface) { composing = true }
                        LabeledRound(if (fav) "즐겨찾기" else "즐겨찾기",
                            if (fav) FAV_GOLD else MaterialTheme.colorScheme.surfaceVariant,
                            if (fav) "★" else "☆",
                            fg = if (fav) Color.White else MaterialTheme.colorScheme.onSurface) {
                            fav = favorites.toggle(target.name, target.number); onFavChanged()
                        }
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        TextButton(onClick = onDismiss) { Text("닫기") }
                    }
                } else {
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


// ─────────────────────────────────────── 최근기록 탭 ───────────────────────────────────────

@Composable
private fun RecentsScreen(
    store: CallLogStore,
    contacts: ContactStore,
    onCall: (String) -> Unit,
) {
    var log by remember { mutableStateOf(store.all()) }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("최근기록", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.weight(1f))
            if (log.isNotEmpty()) TextButton(onClick = { store.clear(); log = emptyList() }) { Text("지우기") }
        }
        if (log.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("최근 통화 기록이 없습니다.", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else {
            LazyColumn(Modifier.fillMaxSize()) {
                items(log) { e ->
                    RecentRow(e, contacts.nameFor(e.number), onClick = { onCall(e.number) })
                    HorizontalDivider()
                }
            }
        }
    }
}

@Composable
private fun RecentRow(e: CallEntry, name: String?, onClick: () -> Unit) {
    val (glyph, glyphColor, missed) = when (e.type) {
        CallType.OUTGOING -> Triple("↗", MaterialTheme.colorScheme.onSurfaceVariant, false)
        CallType.INCOMING -> Triple("↙", MaterialTheme.colorScheme.onSurfaceVariant, false)
        CallType.MISSED -> Triple("↙", HANGUP_RED, true)
    }
    val typeLabel = when (e.type) {
        CallType.OUTGOING -> "발신"; CallType.INCOMING -> "수신"; CallType.MISSED -> "부재중"
    }
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onClick() }.padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(glyph, color = glyphColor, fontSize = 18.sp, modifier = Modifier.size(28.dp), textAlign = TextAlign.Center)
        Spacer(Modifier.size(8.dp))
        Column(Modifier.weight(1f)) {
            Text(name ?: e.number, style = MaterialTheme.typography.bodyLarge,
                color = if (missed) HANGUP_RED else MaterialTheme.colorScheme.onSurface)
            Text("$typeLabel · ${formatTime(e.time)}", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text("📞", fontSize = 18.sp)
    }
}

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
                        Text(nameFor(t.peer) ?: t.peer, style = MaterialTheme.typography.bodyLarge,
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
    onAnswer: (Int) -> Unit,
    onAnswerVideo: (Int) -> Unit,
    onReject: (Int) -> Unit,
    onHangup: (Int) -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Spacer(Modifier.height(48.dp))
        val (remote, stateLine) = when (val c = call) {
            is CallState.Incoming -> extractNumber(c.remote) to (if (c.video) "영상 수신 전화" else "수신 전화")
            is CallState.Outgoing -> extractNumber(c.remote) to (if (videoOn) "영상 발신 중…" else "발신 중…")
            is CallState.Active -> extractNumber(c.remote) to "통화 중"
            else -> "" to ""
        }
        Text(remote, style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))

        if (call is CallState.Active) {
            var elapsed by remember { mutableStateOf(0) }
            LaunchedEffect(Unit) { while (true) { delay(1000); elapsed++ } }
            Text("%02d:%02d".format(elapsed / 60, elapsed % 60),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            Text(stateLine, style = MaterialTheme.typography.titleMedium,
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
                LabeledRound("거절", HANGUP_RED, "✕") { onReject(c.id) }
                LabeledRound("받기", CALL_GREEN, "📞") { onAnswer(c.id) }
                if (c.video) LabeledRound("영상", VIDEO_BLUE, "📹") { onAnswerVideo(c.id) }
            }
            is CallState.Active -> {
                // 통화중 컨트롤 — 음소거/스피커/영상 (토글 시 강조색)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                    ToggleRound("음소거", "🔇", muted) { onToggleMute(c.id, !muted) }
                    ToggleRound("스피커", "🔊", speakerOn) { onToggleSpeaker(!speakerOn) }
                    ToggleRound("영상", "📹", videoOn, activeBg = VIDEO_BLUE) { onToggleVideo(!videoOn) }
                }
                Spacer(Modifier.height(24.dp))
                LabeledRound("종료", HANGUP_RED, "📞") { onHangup(c.id) }
            }
            is CallState.Outgoing -> LabeledRound("취소", HANGUP_RED, "📞") { onHangup(c.id) }
            else -> {}
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun LabeledRound(label: String, bg: Color, glyph: String, fg: Color = Color.White, enabled: Boolean = true, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        RoundButton(label = glyph, bg = bg, fg = fg, size = 72.dp, enabled = enabled, onClick = onClick)
        Spacer(Modifier.height(8.dp))
        Text(label, style = MaterialTheme.typography.labelMedium)
    }
}

/** 통화중 토글 버튼 — off=회색, on=강조색(기본 전화앱의 음소거/스피커 토글과 동일한 패턴). */
@Composable
private fun ToggleRound(
    label: String,
    glyph: String,
    active: Boolean,
    activeBg: Color = TOGGLE_ACTIVE,
    onClick: () -> Unit,
) {
    LabeledRound(
        label = label,
        bg = if (active) activeBg else MaterialTheme.colorScheme.surfaceVariant,
        glyph = glyph,
        fg = if (active) Color.White else MaterialTheme.colorScheme.onSurface,
        onClick = onClick,
    )
}

@Composable
private fun RoundButton(
    label: String,
    bg: Color,
    fg: Color = Color.White,
    size: androidx.compose.ui.unit.Dp = 72.dp,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(size)
            .clip(CircleShape)
            .background(if (enabled) bg else bg.copy(alpha = 0.35f))
            .clickable(enabled = enabled) { onClick() },
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = fg, fontSize = 28.sp)
    }
}

@Composable
private fun RegStatusChip(reg: RegState) {
    val (dot, label) = when (reg) {
        is RegState.Registered -> CALL_GREEN to "통화 가능"
        RegState.Registering, RegState.Idle -> Color(0xFFF9A825) to "연결 중…"
        RegState.Unregistered -> Color(0xFFF9A825) to "등록 해제됨"
        is RegState.Failed -> HANGUP_RED to "오프라인"
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(Modifier.size(10.dp).clip(CircleShape).background(dot))
        Text(label, style = MaterialTheme.typography.labelMedium)
    }
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
    // 로컬 카메라 프리뷰(내 화면). PiP 위에 그려지도록 media overlay + Z-top.
    AndroidView(
        modifier = Modifier.fillMaxSize(),
        factory = { ctx ->
            SurfaceView(ctx).apply {
                setZOrderMediaOverlay(true)
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(h: SurfaceHolder) = onSurface(h.surface)
                    override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) = onSurface(h.surface)
                    override fun surfaceDestroyed(h: SurfaceHolder) = onSurface(null)
                })
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

private fun formatTime(ts: Long): String =
    SimpleDateFormat("M월 d일 a h:mm", Locale.KOREA).format(java.util.Date(ts))

private fun requiredPermissions(): Array<String> = buildList {
    add(Manifest.permission.RECORD_AUDIO)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        add(Manifest.permission.POST_NOTIFICATIONS)
    }
}.toTypedArray()

// 기본 전화앱(AOSP/Google 다이얼러) 팔레트 — 통화=밝은 초록, 종료/거절=구글 레드, 영상=구글 블루.
private val CALL_GREEN = Color(0xFF00C853)
private val HANGUP_RED = Color(0xFFEA4335)
private val VIDEO_BLUE = Color(0xFF4285F4)
private val FAV_GOLD = Color(0xFFF9A825)
private val TOGGLE_ACTIVE = Color(0xFF5F6368)   // 음소거/스피커 토글 on (다이얼러 회색 강조)

// DTMF 터치 톤 — 기본 다이얼러와 유사한 볼륨(0~100)/길이(ms).
private const val DTMF_TONE_VOLUME = 80
private const val DTMF_TONE_MS = 120

// ─────────────────────────────────────── 설정 화면 (고급/수동) ───────────────────────────────────────

@Composable
private fun ConfigScreen(
    initial: SipAccountConfig,
    canCancel: Boolean,
    onSave: (SipAccountConfig) -> Unit,
    onCancel: () -> Unit,
) {
    var host by remember { mutableStateOf(initial.serverHost) }
    var port by remember { mutableStateOf(if (initial.serverPort > 0) initial.serverPort.toString() else "") }
    var transport by remember { mutableStateOf(initial.transport) }
    var domain by remember { mutableStateOf(initial.domain) }
    var msisdn by remember { mutableStateOf(initial.msisdn) }
    var imsi by remember { mutableStateOf(initial.imsi) }
    var name by remember { mutableStateOf(initial.displayName) }
    var loginId by remember { mutableStateOf(initial.loginId) }
    var authId by remember { mutableStateOf(initial.authId) }
    var password by remember { mutableStateOf(initial.password) }
    var showError by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("접속/계정 설정", style = MaterialTheme.typography.titleLarge)
        Text("자동 구성(SSO) 대신 수동으로 입력합니다.", style = MaterialTheme.typography.bodySmall)

        ConfigField("서버 IP/호스트", host) { host = it }
        ConfigField("SIP 포트", port) { port = it.filter { ch -> ch.isDigit() } }

        Text("전송 프로토콜", style = MaterialTheme.typography.bodyMedium)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SipAccountConfig.Transport.entries.forEach { t ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = transport == t, onClick = { transport = t })
                    Text(t.name)
                }
            }
        }

        ConfigField("도메인 (홈/서비스)", domain) { domain = it }
        ConfigField("MSISDN — 공개 ID (sip:번호@도메인)", msisdn) { msisdn = it }
        ConfigField("IMSI — 인증 ID 합성용 (IMSI@도메인)", imsi) { imsi = it.filter { ch -> ch.isDigit() } }
        ConfigField("이름", name) { name = it }
        ConfigField("로그인 ID", loginId) { loginId = it }
        ConfigField("auth_id (전체 IMPI 직접 입력 — 비우면 IMSI@도메인 합성)", authId) { authId = it }
        ConfigField("비밀번호", password, isPassword = true) { password = it }

        Text(
            "※ 공개 ID(MSISDN)와 인증 ID(IMSI@도메인)는 서로 다른 값입니다. 서버는 Digest username 으로 " +
                "IMSI@도메인 정확 일치를 요구하며, 불일치 시 즉시 403 으로 거부합니다.",
            style = MaterialTheme.typography.labelSmall,
        )

        if (showError) {
            Text(
                "필수 항목을 확인하세요: 서버/포트/도메인/MSISDN/IMSI(또는 auth_id)/비밀번호",
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
            )
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                val cfg = initial.copy(
                    serverHost = host.trim(),
                    serverPort = port.toIntOrNull() ?: 0,
                    transport = transport,
                    domain = domain.trim(),
                    msisdn = msisdn.trim(),
                    imsi = imsi.trim(),
                    displayName = name.trim(),
                    loginId = loginId.trim(),
                    authId = authId.trim(),
                    password = password,
                )
                if (cfg.isComplete()) onSave(cfg) else showError = true
            }) { Text("저장") }
            if (canCancel) OutlinedButton(onClick = onCancel) { Text("취소") }
        }
    }
}

@Composable
private fun ConfigField(
    label: String,
    value: String,
    isPassword: Boolean = false,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        visualTransformation = if (isPassword) PasswordVisualTransformation() else VisualTransformation.None,
        modifier = Modifier.fillMaxWidth(),
    )
}
