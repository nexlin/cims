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
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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
    // 주소·포트·계정은 마지막 값으로 채운다 — 관제석은 늘 같은 서버·같은 자리 ID 다. 비밀번호는 저장하지 않는다.
    val session = remember { DispatchService.session }
    val last = remember { session?.settingsSnapshot() }
    var host by remember { mutableStateOf(last?.cscHost.orEmpty()) }
    var port by remember { mutableStateOf((last?.cscPort ?: 4430).toString()) }
    var id by remember { mutableStateOf(last?.loginId.orEmpty()) }
    var pw by remember { mutableStateOf("") }
    var autoLogin by remember { mutableStateOf(last?.autoLogin ?: true) }
    // 고급 — 서버 인증서 검증. 설정 시트는 로그인 뒤에야 열리므로, 검증에 걸려 로그인조차 못 하는 사이트(시험망의 사설 인증서)가
    //   여기서 풀 수 있어야 한다(데스크톱 로그인 창의 «고급»). 검증을 꺼 둔 채면 펴 둔다 — 꺼져 있다는 사실이 보이게.
    var advanced by remember { mutableStateOf(last?.verifyServer == false) }
    var verifyServer by remember { mutableStateOf(last?.verifyServer ?: true) }
    var message by remember { mutableStateOf<String?>(null) }
    val busy by vm.busy.collectAsStateWithLifecycle()
    val sessionError by (vm.error ?: return).collectAsStateWithLifecycle()

    // 자동 로그인이 도는 중에도 누를 수 있다 — 세션이 두 로그인을 한 줄로 세운다(`loginAndStart`: 사람이 넣은 자격이 우선이고,
    //   자동 로그인 재시도는 그 순간 그친다). 막아 두면 서버 주소를 고치려는 사람이 응답 시한만큼 기다린다.
    val canLogin = !busy && host.isNotBlank() && id.isNotBlank() && pw.isNotBlank()
    val resuming = !busy && vm.state?.collectAsStateWithLifecycle()?.value == com.cims.ue.dispatch.session.SessionState.LOGGING_IN
    var extraCa by remember { mutableStateOf(session?.settingsSnapshot()?.extraCaPem.orEmpty()) }
    val submit = {
        message = null
        vm.login(host.trim(), port.toIntOrNull() ?: 4430, id.trim(), pw) { err -> message = err }
    }

    val p = Tokens.palette
    Box(Modifier.fillMaxSize().background(p.bg).windowInsetsPadding(WindowInsets.safeDrawing),
        contentAlignment = Alignment.Center) {
        Surface(color = p.paper, shape = MaterialTheme.shapes.large, border = BorderStroke(1.dp, p.line),
            modifier = Modifier.widthIn(max = 480.dp).padding(24.dp)) {
            // 소프트 키보드가 뜨면 남는 높이가 카드보다 작다 — 카드 안을 스크롤하게 둔다(누르면 아래 칸·[로그인] 이 찌그러져 사라진다).
            Column(Modifier.verticalScroll(rememberScrollState()).padding(28.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Box(Modifier.size(48.dp, 40.dp).clip(RoundedCornerShape(8.dp)).background(p.primary),
                        contentAlignment = Alignment.Center) {
                        Text("CIMS", fontSize = Type.micro, fontWeight = FontWeight.Bold, color = p.onPrimary)
                    }
                    Text("관제 로그인", fontSize = Type.display, fontWeight = FontWeight.Bold)
                }

                // 주소·계정은 **글자 그대로** 받는다 — 자동 수정이 계정 이름을 사전 낱말로 바꿔 넣으면 로그인이 엉뚱하게 실패한다.
                OutlinedTextField(host, { host = it }, label = { Text("CSC 주소") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, autoCorrectEnabled = false,
                        capitalization = KeyboardCapitalization.None, imeAction = ImeAction.Next), shape = com.cims.ue.dispatch.ui.FieldShape6, colors = com.cims.ue.dispatch.ui.cimsFieldColors())
                OutlinedTextField(port, { port = it.filter(Char::isDigit) }, label = { Text("포트") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Next), shape = com.cims.ue.dispatch.ui.FieldShape6, colors = com.cims.ue.dispatch.ui.cimsFieldColors())
                OutlinedTextField(id, { id = it }, label = { Text("관제석 계정") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii, autoCorrectEnabled = false,
                        capitalization = KeyboardCapitalization.None, imeAction = ImeAction.Next), shape = com.cims.ue.dispatch.ui.FieldShape6, colors = com.cims.ue.dispatch.ui.cimsFieldColors())
                OutlinedTextField(pw, { pw = it }, label = { Text("비밀번호") },
                    singleLine = true, enabled = !busy, modifier = Modifier.fillMaxWidth(),
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                    // 키보드의 «완료» = 로그인 — 키보드를 내리고 버튼을 찾아 누르게 하지 않는다.
                    keyboardActions = KeyboardActions(onDone = { if (canLogin) submit() }), shape = com.cims.ue.dispatch.ui.FieldShape6, colors = com.cims.ue.dispatch.ui.cimsFieldColors())

                // 자동 로그인 — refresh token 만 저장한다. 앱을 종료해도 남아 다음 기동이 로그인 화면 없이 이어 로그인한다.
                //   지우는 것은 [로그아웃]·자격 만료·이것을 끄고 한 로그인뿐이다.
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = autoLogin, enabled = !busy, onCheckedChange = { v ->
                        autoLogin = v
                        session?.updateSettings { it.copy(autoLogin = v) }
                    }, colors = CheckboxDefaults.colors(checkedColor = p.primary, checkmarkColor = p.onPrimary))
                    Text("자동 로그인", fontSize = Type.body)
                }

                TextButton(onClick = { advanced = !advanced }, contentPadding = PaddingValues(horizontal = 4.dp)) {
                    Text(if (advanced) "고급 ▴" else "고급 ▾", fontSize = Type.body, color = p.muted)
                }
                if (advanced) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = verifyServer, enabled = !busy, onCheckedChange = { v ->
                            verifyServer = v
                            session?.updateSettings { it.copy(verifyServer = v) }
                        }, colors = CheckboxDefaults.colors(checkedColor = p.primary, checkmarkColor = p.onPrimary))
                        Text("서버 인증서 검증", fontSize = Type.body)
                    }
                    if (!verifyServer) Text(
                        "검증이 꺼져 있습니다 — 중간자 공격을 막지 못합니다. 시험 목적으로만 쓰고 운영에서는 켜 두세요.",
                        fontSize = Type.meta, color = p.emg)
                    // 사설 CA — 앱에 든 루트에 물리지 않은 사이트의 추가 신뢰 앵커(PEM). 검증을 끄지 않고 그 사이트만 믿는다.
                    OutlinedTextField(extraCa, { v ->
                        extraCa = v
                        session?.updateSettings { it.copy(extraCaPem = v.trim()) }
                    }, label = { Text("사설 CA PEM (선택)") }, enabled = !busy, minLines = 2, maxLines = 4,
                        textStyle = androidx.compose.ui.text.TextStyle(fontSize = Type.micro,
                            fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace),
                        modifier = Modifier.fillMaxWidth(), shape = com.cims.ue.dispatch.ui.FieldShape6, colors = com.cims.ue.dispatch.ui.cimsFieldColors())
                }

                PillButton(if (busy) "로그인 중…" else "로그인", submit, kind = Pill.INK, height = 48.dp,
                    enabled = canLogin, modifier = Modifier.fillMaxWidth())

                // 저장된 로그인으로 이어 들어가는 중 — 폼만 보이면 «왜 저절로 넘어가는지» 알 수 없다
                if (resuming) Text("저장된 로그인으로 접속 중…", fontSize = Type.body, color = p.muted)
                else (message ?: sessionError)?.let { Text(it, color = p.emg, fontSize = Type.body) }
                Text("접속점·인증 자료·관제 범위는 로그인 뒤 서버가 내려줍니다.", fontSize = Type.meta, color = p.muted)
                // 로그인 전에도 앱을 끝낼 수 있어야 한다 — 등록 유지 서비스가 이미 떠 있다.
                RectButton("앱 종료", onShutdown, modifier = Modifier.align(Alignment.End))
            }
        }
    }
}
