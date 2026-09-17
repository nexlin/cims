// 로그인 (docs/design/features/android_dispatch_tablet.md §6.1, dispatch_desktop_ui.md §6)
//
// CSC 주소와 관제석 계정만 받는다 — 나머지(접속점·인증 자료·관제 범위)는 `/provisioning/me` 가 준다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.*
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

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Card(Modifier.widthIn(max = 480.dp).padding(24.dp)) {
            Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("CIMS 관제 로그인", style = MaterialTheme.typography.headlineSmall)

                OutlinedTextField(host, { host = it }, label = { Text("CSC 주소") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next))
                OutlinedTextField(port, { port = it.filter(Char::isDigit) }, label = { Text("포트") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Next))
                OutlinedTextField(id, { id = it }, label = { Text("관제석 계정") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next))
                OutlinedTextField(pw, { pw = it }, label = { Text("비밀번호") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done))

                Button(
                    onClick = {
                        message = null
                        vm.login(host.trim(), port.toIntOrNull() ?: 4430, id.trim(), pw) { err -> message = err }
                    },
                    enabled = !busy && host.isNotBlank() && id.isNotBlank() && pw.isNotBlank(),
                    modifier = Modifier.fillMaxWidth()
                ) { Text(if (busy) "로그인 중…" else "로그인") }

                (message ?: sessionError)?.let {
                    Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                }
                Text("접속점·인증 자료·관제 범위는 로그인 뒤 서버가 내려줍니다.",
                     style = MaterialTheme.typography.bodySmall)
                // 로그인 전에도 앱을 끝낼 수 있어야 한다 — 등록 유지 서비스가 이미 떠 있다.
                TextButton(onClick = onShutdown, modifier = Modifier.align(Alignment.End)) { Text("앱 종료") }
            }
        }
    }
}
