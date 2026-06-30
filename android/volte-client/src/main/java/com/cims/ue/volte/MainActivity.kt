package com.cims.ue.volte

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.TextButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.cims.ue.core.codec.AmrWbLoopbackSpike
import com.cims.ue.core.codec.MediaCodecCapabilities
import com.cims.ue.core.config.ConfigStore
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.provision.CscEndpoint
import com.cims.ue.core.provision.ProvisioningClient
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) { App() }
            }
        }
    }
}

private enum class Screen { LOGIN, HOME, CONFIG }

@Composable
private fun App() {
    val context = LocalContext.current
    val store = remember { ConfigStore(context) }
    var config by remember { mutableStateOf(store.load()) }
    var screen by remember { mutableStateOf(if (config.isComplete()) Screen.HOME else Screen.LOGIN) }

    when (screen) {
        Screen.LOGIN -> LoginScreen(
            initialCscHost = config.serverHost,
            onProvisioned = { c -> store.save(c); config = c; screen = Screen.HOME },
            onManual = { screen = Screen.CONFIG },
        )
        Screen.CONFIG -> ConfigScreen(
            initial = config,
            canCancel = config.isComplete(),
            onSave = { c -> store.save(c); config = c; screen = Screen.HOME },
            onCancel = { screen = if (config.isComplete()) Screen.HOME else Screen.LOGIN },
        )
        Screen.HOME -> HomeScreen(
            config = config,
            onEditConfig = { screen = Screen.CONFIG },
        )
    }
}

// ─────────────────────────────────────── 로그인(자동 프로비저닝) ───────────────────────────────────────

@Composable
private fun LoginScreen(
    initialCscHost: String,
    onProvisioned: (SipAccountConfig) -> Unit,
    onManual: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var cscHost by remember { mutableStateOf(initialCscHost) }
    var userName by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var showPw by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }

    val canSubmit = !busy && cscHost.isNotBlank() && userName.isNotBlank() && password.isNotBlank()
    fun submit() {
        busy = true; error = ""
        scope.launch {
            val cfg = runCatching {
                withContext(Dispatchers.IO) {
                    val pc = ProvisioningClient(CscEndpoint(host = cscHost.trim()), allowInsecureTls = true)
                    val tok = pc.login(userName.trim(), password)
                    val profile = pc.fetchProfile(tok.accessToken)
                    val svc = profile.service("volte") ?: error("이 계정에 VoLTE 서비스가 없습니다")
                    svc.toSipAccountConfig(
                        loginId = profile.loginId ?: userName.trim(),
                        displayName = profile.displayName ?: userName.trim(),
                        loginPassword = password,
                    )
                }
            }
            busy = false
            cfg.onSuccess { onProvisioned(it) }
                .onFailure { error = it.message ?: "로그인에 실패했습니다" }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .imePadding()
            .padding(horizontal = 24.dp, vertical = 32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Spacer(Modifier.height(24.dp))
        // 브랜드 헤더
        Text(
            "CIMS",
            style = MaterialTheme.typography.displaySmall,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary,
        )
        Text("VoLTE 단말 로그인", style = MaterialTheme.typography.titleMedium)
        Text(
            "로그인하면 서버·계정 정보를 자동으로 받아 설정합니다.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(20.dp))

        Card(
            modifier = Modifier.fillMaxWidth().widthIn(max = 420.dp),
        ) {
            Column(
                modifier = Modifier.padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                OutlinedTextField(
                    value = cscHost, onValueChange = { cscHost = it },
                    label = { Text("CSC 서버 주소") },
                    placeholder = { Text("예: 121.161.164.45") },
                    singleLine = true, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                )
                OutlinedTextField(
                    value = userName, onValueChange = { userName = it },
                    label = { Text("아이디") }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = password, onValueChange = { password = it },
                    label = { Text("비밀번호") }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    visualTransformation = if (showPw) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    trailingIcon = {
                        TextButton(onClick = { showPw = !showPw }) {
                            Text(if (showPw) "숨김" else "표시", style = MaterialTheme.typography.labelMedium)
                        }
                    },
                )

                if (error.isNotBlank()) {
                    Surface(
                        color = MaterialTheme.colorScheme.errorContainer,
                        shape = MaterialTheme.shapes.small,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(
                            error,
                            color = MaterialTheme.colorScheme.onErrorContainer,
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.padding(12.dp),
                        )
                    }
                }

                Button(
                    onClick = { submit() },
                    enabled = canSubmit,
                    modifier = Modifier.fillMaxWidth().height(50.dp),
                ) {
                    if (busy) {
                        CircularProgressIndicator(
                            modifier = Modifier.height(20.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary,
                        )
                    } else {
                        Text("로그인")
                    }
                }
            }
        }

        Spacer(Modifier.height(8.dp))
        TextButton(onClick = onManual) { Text("수동 설정 (고급)") }
        Text(
            "서버 프로비저닝(/provisioning/me) 준비 전에는 '수동 설정'으로 접속 정보를 직접 입력하세요.",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.widthIn(max = 420.dp),
        )
    }
}

// ─────────────────────────────────────── 통화 홈 (M1) ───────────────────────────────────────

@Composable
private fun HomeScreen(
    config: SipAccountConfig,
    onEditConfig: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // SipService 바인딩
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

    // 권한 요청 → 승인되면 등록 시도
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { service?.ensureRegistered() }

    // M1.3 영상 토글 + 카메라 권한
    var videoOn by remember { mutableStateOf(false) }
    val cameraLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) service?.setVideoEnabled(true) else videoOn = false }

    // 상태 관찰 — 서비스 바인딩 전에도 안전하도록 안정적 fallback flow 사용
    val fallbackReg = remember { MutableStateFlow<RegState>(RegState.Idle) }
    val fallbackCall = remember { MutableStateFlow<CallState>(CallState.Null) }
    val reg by (service?.regState ?: fallbackReg).collectAsState()
    val call by (service?.callState ?: fallbackCall).collectAsState()

    var dst by remember { mutableStateOf("") }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("CIMS VoLTE", style = MaterialTheme.typography.titleLarge)
        Text("공개 ID(AOR): ${config.aor}", style = MaterialTheme.typography.bodyMedium)
        Text("인증 ID(IMPI): ${config.digestUsername}", style = MaterialTheme.typography.bodySmall)
        Text(
            "서버: ${config.serverHost}:${config.serverPort}/${config.transport}   도메인=${config.domain}",
            style = MaterialTheme.typography.bodySmall,
        )

        val regText = when (val r = reg) {
            RegState.Idle -> "대기"
            RegState.Registering -> "등록 중…"
            is RegState.Registered -> "✅ 등록됨 (${r.code})"
            RegState.Unregistered -> "등록 해제됨"
            is RegState.Failed -> "❌ 등록 실패: ${r.reason}"
        }
        Text("등록: $regText", style = MaterialTheme.typography.titleMedium)

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { permLauncher.launch(requiredPermissions()) }) { Text("등록") }
            OutlinedButton(onClick = { service?.stopSip(); service = null }) { Text("해제") }
            OutlinedButton(onClick = onEditConfig) { Text("설정") }
        }

        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Switch(checked = videoOn, onCheckedChange = { on ->
                videoOn = on
                if (on) cameraLauncher.launch(Manifest.permission.CAMERA) else service?.setVideoEnabled(false)
            })
            Text("영상 통화 (H.264)")
        }

        HorizontalDivider()

        CallPanel(
            call = call,
            dst = dst,
            onDstChange = { dst = it },
            onDial = { service?.makeCall(dst.trim()) },
            onAnswer = { id -> service?.answer(id) },
            onReject = { id -> service?.reject(id) },
            onHangup = { id -> service?.hangup(id) },
        )

        // 수신 영상 렌더 — 통화 활성/발신 중 + 영상 on
        if (videoOn && (call is CallState.Active || call is CallState.Outgoing)) {
            VideoRender(onSurface = { service?.setVideoSurface(it) })
        }

        HorizontalDivider()

        CodecDiagnostics(scope)
    }
}

@Composable
private fun CallPanel(
    call: CallState,
    dst: String,
    onDstChange: (String) -> Unit,
    onDial: () -> Unit,
    onAnswer: (Int) -> Unit,
    onReject: (Int) -> Unit,
    onHangup: (Int) -> Unit,
) {
    Text("통화", style = MaterialTheme.typography.titleMedium)
    when (val c = call) {
        CallState.Null,
        is CallState.Disconnected -> {
            if (c is CallState.Disconnected && c.id >= 0) {
                Text("종료: ${c.code} ${c.reason}", style = MaterialTheme.typography.bodySmall)
            }
            OutlinedTextField(
                value = dst,
                onValueChange = onDstChange,
                label = { Text("상대 번호 (MSISDN)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Button(enabled = dst.isNotBlank(), onClick = onDial) { Text("발신") }
        }
        is CallState.Outgoing -> Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("발신 중… ${c.remote}")
                Button(onClick = { onHangup(c.id) }) { Text("취소") }
            }
        }
        is CallState.Incoming -> Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("수신 전화: ${c.remote}")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { onAnswer(c.id) }) { Text("받기") }
                    OutlinedButton(onClick = { onReject(c.id) }) { Text("거절") }
                }
            }
        }
        is CallState.Active -> Card {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("통화 중: ${c.remote}")
                Button(onClick = { onHangup(c.id) }) { Text("종료") }
            }
        }
    }
}

@Composable
private fun VideoRender(onSurface: (Any?) -> Unit) {
    // SurfaceView 의 Surface 를 PJSIP 영상 윈도우로 전달(M1.3). 컴포지션 이탈 시 surfaceDestroyed→null.
    AndroidView(
        modifier = Modifier.fillMaxWidth().aspectRatio(4f / 3f),
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
private fun CodecDiagnostics(scope: CoroutineScope) {
    var output by remember { mutableStateOf("") }
    var running by remember { mutableStateOf(false) }
    Text("MediaCodec 점검 (M0)", style = MaterialTheme.typography.titleMedium)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(enabled = !running, onClick = {
            running = true; output = "조회 중…"
            scope.launch {
                val r = withContext(Dispatchers.Default) { MediaCodecCapabilities.summary() }
                output = r; running = false
            }
        }) { Text("코덱 가용성") }
        OutlinedButton(enabled = !running, onClick = {
            running = true; output = "스파이크 실행 중…"
            scope.launch {
                val r = withContext(Dispatchers.Default) { AmrWbLoopbackSpike().run().report() }
                output = r; running = false
            }
        }) { Text("AMR-WB 스파이크") }
    }
    if (running) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
    if (output.isNotBlank()) Text(output, style = MaterialTheme.typography.bodySmall)
}

private fun requiredPermissions(): Array<String> = buildList {
    add(Manifest.permission.RECORD_AUDIO)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        add(Manifest.permission.POST_NOTIFICATIONS)
    }
}.toTypedArray()

// ─────────────────────────────────────── 설정 화면 ───────────────────────────────────────

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
        Text("최초 실행 — 서버와 가입자 정보를 입력하세요.", style = MaterialTheme.typography.bodySmall)

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

        Text(
            "※ 비밀번호는 현재 평문 저장(개발용). 운영 시 EncryptedSharedPreferences/Keystore 적용 예정.",
            style = MaterialTheme.typography.labelSmall,
        )
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
