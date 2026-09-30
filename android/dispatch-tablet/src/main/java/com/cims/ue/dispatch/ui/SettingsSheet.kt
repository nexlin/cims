// 설정 시트 (docs/design/features/android_dispatch_tablet.md §6.9, dispatch_desktop_ui.md §7·§8)
//
// 데스크톱은 별창(`SettingsWindow`)이지만 태블릿에는 별창이 없다(§6.4) — 상단 바 메뉴에서 여는 시트다.
// **저장 버튼을 두지 않는다**: 항목마다 즉시 반영·즉시 저장한다. 관제석에서 «바꿨는데 저장을 안 눌러서
// 안 먹은» 상태를 만들지 않는다. 오디오 경로처럼 즉시 적용이 필요한 것은 세션이 다시 건다.
//
// 여기 없는 것 — 마이크 따로 고르기(통신 경로가 출력 장치의 마이크를 함께 쓴다, §8)·핫키 재배치·트레이·자동 실행은
// 플랫폼 차이이거나 데스크톱 어포던스다.
// 키보드 단축키는 두지 않는다(하드 키보드를 전제하지 않는다, §7) — 바꿀 수 있는 키는 측면 PTT 키의 학습 하나다.
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
import com.cims.ue.dispatch.session.CertLevel
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.ServerCert
import com.cims.ue.sdk.platform.PttKey
import com.cims.ue.sdk.platform.Route
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsSheet(session: DispatchSession, onDismiss: () -> Unit) {
    val s by session.settingsFlow.collectAsStateWithLifecycle()

    ModalBottomSheet(onDismissRequest = onDismiss) {
        ForwardPttKeys()                    // 발언 + 측면 키 학습이 이 창으로 온다(§7)
        Column(Modifier.fillMaxWidth().heightIn(max = 560.dp)
            .verticalScroll(rememberScrollState()).padding(horizontal = 16.dp)) {

            Text("설정", fontSize = Type.title, fontWeight = FontWeight.Bold)
            Text("바꾸면 바로 저장되고 적용됩니다", fontSize = Type.meta,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 8.dp))

            Section("화면")
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                listOf(com.cims.ue.dispatch.session.Settings.THEME_DARK to "어둡게",
                       com.cims.ue.dispatch.session.Settings.THEME_LIGHT to "밝게").forEach { (v, label) ->
                    FilterChip(selected = s.theme == v,
                        onClick = { session.updateSettings { it.copy(theme = v) } },
                        label = { Text(label, fontSize = Type.meta) })
                }
            }

            Section("오디오")
            Text("소리를 내보낼 곳", fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Route.entries.forEach { r ->
                    FilterChip(selected = s.audioRoute == r,
                        onClick = { session.updateSettings { it.copy(audioRoute = r) } },
                        label = { Text(routeLabel(r), fontSize = Type.meta) })
                }
            }
            // 이어폰이 여럿이면 하나를 고른다 — 고른 것이 선호 이어폰으로 남아, 다시 연결되면 그리로 돌아온다(§8).
            val headsets by session.audio.headsets.collectAsStateWithLifecycle()
            if (headsets.isNotEmpty()) {
                Text("이어폰", fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 6.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    headsets.forEach { h ->
                        FilterChip(
                            selected = s.preferredHeadset == h.name &&
                                (s.audioRoute == Route.HEADSET || s.audioRoute == Route.BLUETOOTH),
                            onClick = { session.selectHeadset(h) },
                            label = { Text(h.name + if (h.wireless) " · 무선" else " · 유선", fontSize = Type.meta) })
                    }
                }
            } else if (s.preferredHeadset.isNotBlank()) Hint("선호 이어폰 «${s.preferredHeadset}» — 연결되어 있지 않다")
            Toggle("선호 이어폰 자동 복귀 — 다시 연결되면 그 이어폰으로 (스피커를 고른 뒤에는 되돌리지 않는다)",
                s.autoReturnHeadset) { v -> session.updateSettings { it.copy(autoReturnHeadset = v) } }
            Hint("마이크는 따로 고르지 않는다 — Android 통신 경로는 고른 장치의 마이크를 함께 쓴다(이어폰이면 이어폰 마이크).")
            Hint("무전과 통화를 서로 다른 장치로 가르는 것은 아직 못 한다 — 코어에 스트림별 출력 통로가 " +
                 "필요하다(§8·§11). 지금은 둘이 같은 곳으로 나간다.")

            Section("발언·통화")
            Toggle("잠금 발언 — PTT 를 눌렀다 떼도 발언 유지 (다시 눌러 해제)",
                s.lockTalk) { v -> session.updateSettings { it.copy(lockTalk = v) } }
            Toggle("활성 통화 중 새 착신에 응답하면 기존 통화 자동 보류",
                s.autoHoldOnAnswer) { v -> session.updateSettings { it.copy(autoHoldOnAnswer = v) } }
            Hint("자동 보류를 끄면 두 통화가 동시에 들려 어느 쪽에 말하는지 알 수 없다.")

            Section("PTT 하드키")
            HardKeyRow(session)

            Section("주소록")
            DirectoryCsvRow(session)

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

            Section("메시지")
            OutlinedTextField(
                value = s.messageRetentionDays.toString(),
                onValueChange = { v ->
                    // 1~365 로 죈다(데스크톱과 같은 범위). 빈 칸·글자는 무시한다 — 치는 도중의 값으로 지우지 않도록
                    //   정리는 다음 기동 때 한 번만 한다.
                    v.toIntOrNull()?.coerceIn(1, 365)?.let { n -> session.updateSettings { it.copy(messageRetentionDays = n) } }
                },
                label = { Text("보관 일수") }, singleLine = true,
                supportingText = {
                    Text("앱을 다시 켤 때 이보다 오래된 SDS·문자를 지운다 (1~365, 기본 30). 서버에 이력이 없어 지운 것은 " +
                         "되살릴 수 없다", fontSize = Type.meta)
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

            ServerCertRow(session)

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

/**
 * 로컬 CSV 주소록(§6.2b) — 외부망 번호·서버에 없는 이름을 더한다. 데스크톱 `directory.csv` 와 같은 파일이다. 가져오면 앱 저장소에
 * 복사해 두므로(원본 파일의 접근 권한에 기대지 않는다) 원본을 지워도 남는다. 서버 전화번호부가 늘 앞선다.
 */
@Composable
private fun DirectoryCsvRow(session: DispatchSession) {
    val rows by session.csvContacts.collectAsStateWithLifecycle()
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    val pick = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val text = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                runCatching { context.contentResolver.openInputStream(uri)?.use { it.readBytes().toString(Charsets.UTF_8) } }
                    .getOrNull()
            }
            val n = if (text == null) -1 else session.importDirectoryCsv(text)
            session.notify(
                if (n > 0) com.cims.ue.dispatch.session.NoticeLevel.INFO else com.cims.ue.dispatch.session.NoticeLevel.ERROR,
                when {
                    n > 0 -> "CSV 주소록 ${n}줄을 더했습니다"
                    n == 0 -> "읽을 줄이 없습니다 — kind,number,name,tags 형식인지 확인하세요"
                    else -> "파일을 읽지 못했습니다"
                })
        }
    }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text("로컬 CSV", fontSize = Type.body)
            Text(if (rows.isEmpty()) "없음" else "${rows.size}줄 · 외부망 ${rows.count { it.kind == com.cims.ue.dispatch.session.CsvKind.EXTERNAL }}",
                fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        OutlinedButton(onClick = { pick.launch(arrayOf("text/*", "application/csv", "application/vnd.ms-excel")) }) {
            Text("가져오기")
        }
        TextButton(onClick = { scope.launch { session.clearDirectoryCsv() } }, enabled = rows.isNotEmpty()) { Text("지우기") }
    }
    Hint("형식 kind,number,name,tags — kind = ext(가입자·내선) | external(외부망) | ptt. 같은 번호가 서버 전화번호부에 있으면 " +
         "서버 이름이 앞서고 CSV 이름은 빈 곳만 채운다. 서버 전화번호부는 캐시해 두어 켜자마자 이름이 선다.")
}

/**
 * «서버 인증서» 행 — 마지막 핸드셰이크에서 본 서버 인증서의 잔여(§6.2a-3). 읽기 전용이다 — 단말이 고칠 수 있는 게 아니다.
 * 잔여 >30일 «N일 남음»(보조색) · ≤30일 «N일 후 만료»(호박) · ≤7일·오늘·만료(빨강). 관측이 없으면(평문 접속·접속 전) «—».
 */
@Composable
private fun ServerCertRow(session: DispatchSession) {
    val e by session.serverCert.collectAsStateWithLifecycle()
    val cert = e
    val now = System.currentTimeMillis() / 1000L
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text("서버 인증서", fontSize = Type.body)
            if (cert != null) Text("${cert.remote} · 만료 ${ServerCert.day(cert)}", fontSize = Type.meta,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (cert == null) Text("—", fontSize = Type.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
        else {
            val level = ServerCert.level(cert, now)
            Text(if (level == CertLevel.OK) "${ServerCert.daysLeft(cert, now)}일 남음" else ServerCert.title(cert, now),
                fontSize = Type.body, fontWeight = if (level == CertLevel.OK) FontWeight.Normal else FontWeight.Bold,
                color = when (level) {
                    CertLevel.OK -> MaterialTheme.colorScheme.onSurfaceVariant
                    CertLevel.WARN -> PerilAmber
                    CertLevel.CRITICAL -> MaterialTheme.colorScheme.error
                })
        }
    }
    if (cert != null && ServerCert.level(cert, now) != CertLevel.OK)
        Hint("자동 갱신 실패 신호 — 운영자에게 알리세요 (콘솔 알람 A-PRC-009)")
}

/**
 * 측면 PTT 키 — 기종마다 keycode 가 달라 **한 번 눌러 가르친다**(`HwPtt` 학습). 가르치지 않으면 내장 기본(러기드 실측
 * 309, 측면 키가 기능 키로 오는 단말의 F11)을 쓴다. 누르는 동안 발언·떼면 해제로 발언 바와 같다(§7).
 */
@Composable
private fun HardKeyRow(session: DispatchSession) {
    val hw = session.hwPtt
    val mapping by hw.mapping.collectAsStateWithLifecycle()
    val learning by hw.learning.collectAsStateWithLifecycle()
    // 시트를 닫으면 학습도 끝난다 — 남아 있으면 다음에 누른 아무 키나 발언 키가 된다.
    DisposableEffect(Unit) { onDispose { hw.cancelLearn() } }

    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text("발언 키", fontSize = Type.body)
            Text(if (mapping.talk > 0) "학습함 · keycode ${mapping.talk}" else "기본값 (러기드 측면 키 · F11)",
                fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (learning == PttKey.TALK) {
            Text("측면 PTT 버튼을 누르세요", fontSize = Type.body, fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary)
            TextButton(onClick = hw::cancelLearn) { Text("취소") }
        } else {
            OutlinedButton(onClick = { hw.startLearn(PttKey.TALK) }) { Text("버튼 학습") }
            TextButton(onClick = hw::resetMapping, enabled = mapping.talk > 0) { Text("기본값") }
        }
    }
    Hint("누르는 동안 발언, 떼면 해제 — 발언 바와 같다(잠금 발언도 따른다). 시트가 열려 있어도 먹는다.")
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
