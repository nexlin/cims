package com.cims.ue.ptt

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import com.cims.ue.ptt.floor.FloorState
import kotlinx.coroutines.flow.MutableStateFlow

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) { PttScreen() } } }
    }
}

@Composable
private fun PttScreen() {
    val context = LocalContext.current
    var svc by remember { mutableStateOf<PttService?>(null) }
    DisposableEffect(Unit) {
        val conn = object : ServiceConnection {
            override fun onServiceConnected(n: ComponentName?, b: IBinder?) { svc = (b as? PttService.LocalBinder)?.service }
            override fun onServiceDisconnected(n: ComponentName?) { svc = null }
        }
        PttService.start(context)
        context.bindService(Intent(context, PttService::class.java), conn, Context.BIND_AUTO_CREATE)
        onDispose { runCatching { context.unbindService(conn) } }
    }

    val perm = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        svc?.ensureRegistered()
    }

    val ctl = svc?.controller
    val fbReg = remember { MutableStateFlow<RegState>(RegState.Idle) }
    val fbCall = remember { MutableStateFlow<CallState>(CallState.Null) }
    val fbFloor = remember { MutableStateFlow(FloorState.IDLE) }
    val fbStatus = remember { MutableStateFlow("서비스 연결 중…") }
    val reg by (ctl?.regState ?: fbReg).collectAsState()
    val call by (ctl?.callState ?: fbCall).collectAsState()
    val floor by (ctl?.floorState ?: fbFloor).collectAsState()
    val status by (ctl?.status ?: fbStatus).collectAsState()

    var groupId by remember { mutableStateOf("") }
    val hasAccount = remember { com.cims.ue.core.account.SsoProvisioner.hasAccount(context) }

    // SSO: 컨트롤러 연결 시 CIMS 공유 계정의 MCPTT(TS 33.180) 토큰을 주입(별도 로그인 없음).
    androidx.compose.runtime.LaunchedEffect(ctl) {
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

    Column(
        Modifier.fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("CIMS MCPTT (M2)", style = MaterialTheme.typography.titleLarge)
        val regText = when (val r = reg) {
            RegState.Idle -> "대기"; RegState.Registering -> "등록 중…"
            is RegState.Registered -> "✅ 등록(${r.code})"; RegState.Unregistered -> "해제"
            is RegState.Failed -> "❌ ${r.reason}"
        }
        Text("등록: $regText", style = MaterialTheme.typography.bodyMedium)
        Text("호: ${callText(call)}", style = MaterialTheme.typography.bodyMedium)
        Text("발언권: $floor", style = MaterialTheme.typography.bodyMedium)
        Text("상태: $status", style = MaterialTheme.typography.bodySmall)

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { perm.launch(perms()) }) { Text("등록") }
            OutlinedButton(onClick = { svc?.stopSip(); svc = null }) { Text("해제") }
        }

        HorizontalDivider()
        Text("계정 (CIMS SSO)", style = MaterialTheme.typography.titleMedium)
        if (hasAccount) {
            Text("CIMS 계정으로 자동 인증 — 별도 로그인 없음", style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = { ctl?.loadGroups() }) { Text("그룹 조회") }
        } else {
            Text("CIMS 앱에서 먼저 로그인하세요.", style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = {
                val act = context as? android.app.Activity
                android.accounts.AccountManager.get(context).addAccount(
                    com.cims.ue.core.account.CimsAccounts.ACCOUNT_TYPE,
                    com.cims.ue.core.account.CimsAccounts.TOKEN_MCPTT,
                    null, null, act, null, null)
            }) { Text("CIMS 로그인 열기") }
        }

        HorizontalDivider()
        Text("그룹콜", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(groupId, { groupId = it }, label = { Text("그룹 ID (번호)") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(enabled = groupId.isNotBlank(), onClick = { ctl?.affiliate(groupId.trim(), true) }) { Text("affiliate") }
            OutlinedButton(enabled = groupId.isNotBlank(), onClick = { ctl?.affiliate(groupId.trim(), false) }) { Text("de-affiliate") }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(enabled = groupId.isNotBlank(), onClick = { ctl?.startGroupCall(groupId.trim()) }) { Text("그룹콜 시작") }
            OutlinedButton(onClick = { ctl?.hangup() }) { Text("종료") }
        }

        HorizontalDivider()
        // PTT 버튼 — 누르고 있는 동안 발언(Floor Request), 떼면 Release
        val speaking = floor == FloorState.SPEAKING
        val requesting = floor == FloorState.REQUESTING
        Text(
            text = when { speaking -> "🎙 발언 중 (떼면 종료)"; requesting -> "요청 중…"; else -> "PTT — 길게 눌러 발언" },
            color = Color.White,
            modifier = Modifier
                .fillMaxWidth().height(120.dp)
                .clip(RoundedCornerShape(16.dp))
                .background(if (speaking) Color(0xFF2E7D32) else if (requesting) Color(0xFFF9A825) else Color(0xFF1565C0))
                .pointerInput(ctl) {
                    detectTapGestures(onPress = {
                        ctl?.pttDown()
                        try { awaitRelease() } finally { ctl?.pttUp() }
                    })
                }
                .padding(24.dp),
        )
    }
}

private fun callText(c: CallState): String = when (c) {
    CallState.Null -> "없음"
    is CallState.Outgoing -> "발신 ${c.remote}"
    is CallState.Incoming -> "수신 ${c.remote}"
    is CallState.Active -> "통화 중 ${c.remote}"
    is CallState.Disconnected -> "종료(${c.code})"
}

private fun perms(): Array<String> = buildList {
    add(Manifest.permission.RECORD_AUDIO)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.POST_NOTIFICATIONS)
}.toTypedArray()
