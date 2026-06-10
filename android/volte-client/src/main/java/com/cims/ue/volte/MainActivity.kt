package com.cims.ue.volte

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.cims.ue.core.codec.AmrWbLoopbackSpike
import com.cims.ue.core.codec.MediaCodecCapabilities
import com.cims.ue.core.config.ConfigStore
import com.cims.ue.core.config.SipAccountConfig
import kotlinx.coroutines.Dispatchers
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

private enum class Screen { HOME, CONFIG }

@Composable
private fun App() {
    val context = LocalContext.current
    val store = remember { ConfigStore(context) }
    var config by remember { mutableStateOf(store.load()) }
    var screen by remember { mutableStateOf(if (config.isComplete()) Screen.HOME else Screen.CONFIG) }

    when (screen) {
        Screen.CONFIG -> ConfigScreen(
            initial = config,
            canCancel = config.isComplete(),
            onSave = { c -> store.save(c); config = c; screen = Screen.HOME },
            onCancel = { screen = Screen.HOME },
        )
        Screen.HOME -> HomeScreen(
            config = config,
            onEditConfig = { screen = Screen.CONFIG },
        )
    }
}

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

@Composable
private fun HomeScreen(
    config: SipAccountConfig,
    onEditConfig: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var output by remember { mutableStateOf("") }
    var running by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("CIMS VoLTE", style = MaterialTheme.typography.titleLarge)
        Text("공개 ID(AOR): ${config.aor}", style = MaterialTheme.typography.bodyMedium)
        Text("인증 ID(IMPI): ${config.digestUsername}", style = MaterialTheme.typography.bodyMedium)
        Text(
            "서버: ${config.serverHost}:${config.serverPort}/${config.transport}   도메인=${config.domain}",
            style = MaterialTheme.typography.bodySmall,
        )
        OutlinedButton(onClick = onEditConfig) { Text("설정 편집") }

        HorizontalDivider()

        Text("MediaCodec 점검 (M0)", style = MaterialTheme.typography.titleMedium)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(enabled = !running, onClick = {
                running = true; output = "조회 중…"
                scope.launch {
                    val r = withContext(Dispatchers.Default) { MediaCodecCapabilities.summary() }
                    output = r; running = false
                }
            }) { Text("코덱 가용성") }
            Button(enabled = !running, onClick = {
                running = true; output = "스파이크 실행 중…"
                scope.launch {
                    val r = withContext(Dispatchers.Default) { AmrWbLoopbackSpike().run().report() }
                    output = r; running = false
                }
            }) { Text("AMR-WB 스파이크") }
        }

        // M1.1: PJSIP 통합 후 활성화 예정
        Button(enabled = false, onClick = {}) { Text("등록(REGISTER) — PJSIP 통합 후 (M1.1)") }

        if (running) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        Text(
            text = output,
            modifier = Modifier.verticalScroll(rememberScrollState()),
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
