// 설정 시트 (docs/design/features/android_dispatch_tablet.md §6.9, dispatch_desktop_ui.md §7·§8)
//
// 데스크톱은 별창(`SettingsWindow`)이지만 태블릿에는 별창이 없다(§6.4) — 상단 바 메뉴에서 여는 시트다.
// **저장 버튼을 두지 않는다**: 항목마다 즉시 반영·즉시 저장한다. 관제석에서 «바꿨는데 저장을 안 눌러서
// 안 먹은» 상태를 만들지 않는다. 오디오 경로처럼 즉시 적용이 필요한 것은 세션이 다시 건다.
//
// 여기 없는 것 — 장치 선택·핫키 재배치·트레이·자동 실행은 데스크톱 어포던스이거나 아직 구현 전이다(§11).
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.sdk.platform.Route

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsSheet(session: DispatchSession, onDismiss: () -> Unit) {
    val s by session.settingsFlow.collectAsStateWithLifecycle()

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().heightIn(max = 560.dp)
            .verticalScroll(rememberScrollState()).padding(horizontal = 16.dp)) {

            Text("설정", fontSize = Type.title, fontWeight = FontWeight.Bold)
            Text("바꾸면 바로 저장되고 적용됩니다", fontSize = Type.meta,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 8.dp))

            Section("오디오")
            Text("소리를 내보낼 곳", fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Route.entries.forEach { r ->
                    FilterChip(selected = s.audioRoute == r,
                        onClick = { session.updateSettings { it.copy(audioRoute = r) } },
                        label = { Text(routeLabel(r), fontSize = Type.meta) })
                }
            }
            Hint("무전과 통화를 서로 다른 장치로 가르는 것은 아직 못 한다 — 코어에 스트림별 출력 통로가 " +
                 "필요하다(§8·§11). 지금은 둘이 같은 곳으로 나간다.")

            Section("발언·통화")
            Toggle("잠금 발언 — PTT 를 눌렀다 떼도 발언 유지 (다시 눌러 해제)",
                s.lockTalk) { v -> session.updateSettings { it.copy(lockTalk = v) } }
            Toggle("활성 통화 중 새 착신에 응답하면 기존 통화 자동 보류",
                s.autoHoldOnAnswer) { v -> session.updateSettings { it.copy(autoHoldOnAnswer = v) } }
            Hint("자동 보류를 끄면 두 통화가 동시에 들려 어느 쪽에 말하는지 알 수 없다.")

            Section("당겨받기")
            OutlinedTextField(
                value = s.pickupFeatureCode,
                onValueChange = { v -> session.updateSettings { it.copy(pickupFeatureCode = v.trim()) } },
                label = { Text("피처코드") }, singleLine = true,
                supportingText = { Text("접속서비스의 pickup_feature_code 와 같아야 한다 (기본 **)", fontSize = Type.meta) },
                modifier = Modifier.fillMaxWidth())

            Section("청취")
            OutlinedTextField(
                value = s.maxListen.toString(),
                onValueChange = { v ->
                    // 1~16 으로 죈다(데스크톱과 같은 범위, `SettingsViewModel` 의 Clamp). 빈 칸·글자는 무시한다 —
                    //   0 이 되면 청취가 통째로 막히고 그 이유가 화면에 없다.
                    v.toIntOrNull()?.coerceIn(1, 16)?.let { n -> session.updateSettings { it.copy(maxListen = n) } }
                },
                label = { Text("동시 청취 상한") }, singleLine = true,
                supportingText = {
                    Text("감청·PTT 청취를 합쳐 한 번에 열 수 있는 수 (1~16, 기본 4). 넘으면 서버가 거절하기 전에 앱이 막는다",
                         fontSize = Type.meta)
                },
                modifier = Modifier.fillMaxWidth())

            Section("서버")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = s.cscHost, onValueChange = {}, enabled = false,
                    label = { Text("CSC 주소") }, singleLine = true, modifier = Modifier.weight(2f))
                OutlinedTextField(value = s.cscPort.toString(), onValueChange = {}, enabled = false,
                    label = { Text("포트") }, singleLine = true, modifier = Modifier.weight(1f))
            }
            Hint("접속점은 로그인 화면에서 정한다 — 등록·구독이 붙어 있는 동안 바꾸면 세션이 어긋난다.")

            Toggle("서버 인증서 검증", s.verifyServer) { v ->
                session.updateSettings { it.copy(verifyServer = v) }
            }
            if (!s.verifyServer) Warn(
                "검증이 꺼져 있습니다 — 중간자 공격을 막지 못합니다. 시험 목적으로만 쓰고 운영에서는 켜 두세요. " +
                "다음 로그인부터 적용됩니다.")

            Section("진단")
            Text("로그 수준 ${s.logLevel}", fontSize = Type.body)
            Slider(value = s.logLevel.toFloat(), valueRange = 0f..5f, steps = 4,
                onValueChange = { v -> session.updateSettings { it.copy(logLevel = v.toInt()) } })
            Hint("0=끔 … 5=자세히. 엔진 기동 때 읽으므로 다음 로그인부터 적용된다.")

            Spacer(Modifier.height(20.dp))
        }
    }
}

private fun routeLabel(r: Route): String = when (r) {
    Route.EARPIECE -> "수화부"
    Route.SPEAKER -> "스피커"
    Route.HEADSET -> "유선 헤드셋"
    Route.BLUETOOTH -> "블루투스"
}

@Composable
private fun Section(title: String) {
    Spacer(Modifier.height(12.dp))
    Text(title, fontSize = Type.strong, fontWeight = FontWeight.Bold)
    HorizontalDivider(Modifier.padding(vertical = 4.dp))
}

@Composable
private fun Toggle(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f), fontSize = Type.body)
        Switch(checked = value, onCheckedChange = onChange)
    }
}

@Composable
private fun Hint(text: String) {
    Text(text, fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 2.dp, bottom = 4.dp))
}

@Composable
private fun Warn(text: String) {
    Surface(color = MaterialTheme.colorScheme.errorContainer, modifier = Modifier.fillMaxWidth()) {
        Text(text, Modifier.padding(8.dp), fontSize = Type.meta,
            color = MaterialTheme.colorScheme.onErrorContainer)
    }
}
