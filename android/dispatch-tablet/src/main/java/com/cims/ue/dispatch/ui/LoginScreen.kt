// 로그인 (docs/design/features/android_dispatch_tablet.md §6.1, dispatch_desktop_ui.md §6)
//
// CSC 주소와 관제석 계정만 받는다 — 나머지(접속점·인증 자료·관제 범위)는 `/provisioning/me` 가 준다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchService

@Composable
fun LoginScreen(vm: MainViewModel, onShutdown: () -> Unit = {}) {
    val settings = remember { DispatchService.session }
    var host by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("4430") }
    var id by remember { mutableStateOf("") }
    var pw by remember { mutableStateOf("") }
    var message by remember { mutableStateOf<String?>(null) }
    val busy by vm.busy.collectAsStateWithLifecycle()
    val sessionError by (vm.error ?: return).collectAsStateWithLifecycle()

    val p = Tokens.palette
    Box(Modifier.fillMaxSize().background(p.bar).windowInsetsPadding(WindowInsets.safeDrawing),
        contentAlignment = Alignment.Center) {
        Surface(color = p.paper, shape = MaterialTheme.shapes.large, border = BorderStroke(1.dp, p.line),
            modifier = Modifier.widthIn(max = 480.dp).padding(24.dp)) {
            Column(Modifier.padding(28.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Box(Modifier.size(48.dp, 40.dp).clip(RoundedCornerShape(8.dp)).background(p.ink),
                        contentAlignment = Alignment.Center) {
                        Text("CIMS", fontSize = Type.micro, fontWeight = FontWeight.Bold, color = p.onInk)
                    }
                    Text("관제 로그인", fontSize = Type.display, fontWeight = FontWeight.Bold)
                }

                // 주소·계정은 **글자 그대로** 받는다 — 자동 수정이 계정 이름을 사전 낱말로 바꿔 넣으면 로그인이 엉뚱하게 실패한다.
                OutlinedTextField(host, { host = it }, label = { Text("CSC 주소") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, autoCorrectEnabled = false,
                        capitalization = KeyboardCapitalization.None, imeAction = ImeAction.Next))
                OutlinedTextField(port, { port = it.filter(Char::isDigit) }, label = { Text("포트") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Next))
                OutlinedTextField(id, { id = it }, label = { Text("관제석 계정") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii, autoCorrectEnabled = false,
                        capitalization = KeyboardCapitalization.None, imeAction = ImeAction.Next))
                OutlinedTextField(pw, { pw = it }, label = { Text("비밀번호") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done))

                PillButton(if (busy) "로그인 중…" else "로그인", {
                        message = null
                        vm.login(host.trim(), port.toIntOrNull() ?: 4430, id.trim(), pw) { err -> message = err }
                    }, filled = true, height = 48.dp,
                    enabled = !busy && host.isNotBlank() && id.isNotBlank() && pw.isNotBlank(),
                    modifier = Modifier.fillMaxWidth())

                (message ?: sessionError)?.let { Text(it, color = p.emergency, fontSize = Type.body) }
                Text("접속점·인증 자료·관제 범위는 로그인 뒤 서버가 내려줍니다.", fontSize = Type.meta, color = p.muted)
                // 로그인 전에도 앱을 끝낼 수 있어야 한다 — 등록 유지 서비스가 이미 떠 있다.
                TextButton(onClick = onShutdown, modifier = Modifier.align(Alignment.End)) { Text("앱 종료") }
            }
        }
    }
}
