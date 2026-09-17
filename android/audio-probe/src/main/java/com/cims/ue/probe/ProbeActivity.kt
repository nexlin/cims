// 오디오 라우팅 탐침 — 화면 v2 (docs/design/features/android_dispatch_tablet.md §8, F5)
//
// v1 은 귀로 판정하게 했는데 태블릿은 통화/미디어 스피커가 물리적으로 붙어 있어 구분이 안 된다.
// v2 는 **타임라인**으로 판정하고, 귀는 "이어폰에서 나나 안 나나" 만 본다 — 그래서 한 번에 한 트랙만
// 내는 모드를 뒀다.
package com.cims.ue.probe

import android.Manifest
import android.content.pm.PackageManager
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class ProbeActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val am = getSystemService(AudioManager::class.java)
        setContent { MaterialTheme { ProbeScreen(AudioProbe(am), this) } }
    }
}

@Composable
private fun ProbeScreen(probe: AudioProbe, activity: ComponentActivity) {
    val scope = rememberCoroutineScope()
    var running by remember { mutableStateOf(false) }
    var result by remember { mutableStateOf<ProbeResult?>(null) }
    var sinks by remember { mutableStateOf(probe.outputs()) }
    var commDevs by remember { mutableStateOf(probe.communicationDevices()) }
    var mode by remember { mutableStateOf(PlayMode.BOTH) }
    var callSel by remember { mutableStateOf<Sink?>(null) }
    var radioSel by remember { mutableStateOf<Sink?>(null) }

    // BLUETOOTH_CONNECT 는 API 31+ 에서 런타임 권한이다 — 없으면 BT 장치 이름·통신 장치 제어가 막힌다.
    var btGranted by remember {
        mutableStateOf(
            Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
                ContextCompat.checkSelfPermission(activity, Manifest.permission.BLUETOOTH_CONNECT) ==
                    PackageManager.PERMISSION_GRANTED)
    }
    val askBt = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        btGranted = ok
        sinks = probe.outputs(); commDevs = probe.communicationDevices()
    }
    LaunchedEffect(Unit) {
        if (!btGranted) askBt.launch(Manifest.permission.BLUETOOTH_CONNECT)
    }

    fun refresh() {
        sinks = probe.outputs(); commDevs = probe.communicationDevices()
        if (callSel != null && sinks.none { it.id == callSel!!.id }) callSel = null
        if (radioSel != null && sinks.none { it.id == radioSel!!.id }) radioSel = null
    }

    Column(Modifier.fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState()),
           verticalArrangement = Arrangement.spacedBy(10.dp)) {

        Text("CIMS 오디오 라우팅 탐침 v2", style = MaterialTheme.typography.headlineSmall)
        Text("판정은 타임라인으로 한다. 귀로는 «이어폰에서 나는가» 만 본다 — 그래서 한 번에 한 트랙만 내는 모드가 있다.",
             style = MaterialTheme.typography.bodySmall)

        if (!btGranted) {
            Card { Column(Modifier.padding(12.dp)) {
                Text("블루투스 권한이 없습니다 — BT 장치를 고를 수 없습니다.", fontWeight = FontWeight.Bold)
                TextButton(onClick = { askBt.launch(Manifest.permission.BLUETOOTH_CONNECT) }) { Text("권한 허용") }
            } }
        }

        HorizontalDivider()

        Text("출력 장치 ${sinks.size}개 · 통신용 ${commDevs.size}개", fontWeight = FontWeight.Bold)
        sinks.forEach { s ->
            val comm = if (commDevs.any { it.id == s.id }) "  [통신용]" else ""
            Text("  • $s$comm", fontFamily = FontFamily.Monospace, fontSize = 12.sp)
        }
        TextButton(onClick = { refresh() }, enabled = !running) { Text("장치 다시 읽기") }

        HorizontalDivider()

        Text("① 재생 모드", fontWeight = FontWeight.Bold)
        PlayMode.entries.forEach { m ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                RadioButton(selected = mode == m, onClick = { mode = m }, enabled = !running)
                Text(m.label)
            }
        }

        if (mode != PlayMode.RADIO_ONLY) {
            Text("② 통화(440Hz) 대상 — 통신용 목록에서만 고를 수 있다", fontWeight = FontWeight.Bold)
            PickRow(sinks.filter { s -> commDevs.any { it.id == s.id } }, callSel, running) { callSel = it }
        }
        if (mode != PlayMode.CALL_ONLY) {
            Text("③ 무전(880Hz) 대상 — 트랙별 지정(setPreferredDevice)", fontWeight = FontWeight.Bold)
            PickRow(sinks, radioSel, running) { radioSel = it }
        }

        Button(
            onClick = {
                running = true
                scope.launch {
                    val r = withContext(Dispatchers.IO) { probe.run(mode, callSel, radioSel) }
                    result = r; running = false; refresh()
                }
            },
            enabled = !running,
            modifier = Modifier.fillMaxWidth()
        ) { Text(if (running) "측정 중… (링크 대기 + 8초 재생)" else "측정") }

        result?.let { r -> ResultCard(r) }
    }
}

@Composable
private fun PickRow(items: List<Sink>, selected: Sink?, disabled: Boolean, onPick: (Sink?) -> Unit) {
    if (items.isEmpty()) { Text("  (고를 장치가 없습니다)", fontSize = 12.sp); return }
    Column {
        items.forEach { s ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                RadioButton(selected = selected?.id == s.id, onClick = { onPick(s) }, enabled = !disabled)
                Text(s.toString(), fontFamily = FontFamily.Monospace, fontSize = 12.sp)
            }
        }
    }
}

@Composable
private fun ResultCard(r: ProbeResult) {
    HorizontalDivider()
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("판정: ${r.verdict}", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
            Text("${r.device} · API ${r.sdk} · ${r.mode.label}", fontSize = 12.sp)
            HorizontalDivider()
            Text("요청 통화: ${r.requestedCall ?: "(없음)"}", fontFamily = FontFamily.Monospace, fontSize = 11.sp)
            Text("요청 무전: ${r.requestedRadio ?: "(없음)"}", fontFamily = FontFamily.Monospace, fontSize = 11.sp)
            r.commAccepted?.let { Text("setCommunicationDevice: ${if (it) "수락" else "거부"}", fontFamily = FontFamily.Monospace, fontSize = 11.sp) }
            Text("링크 확인: ${if (r.linkReady) "됨" else "안 됨"} (${r.linkWaitMs}ms 대기)",
                 fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                 fontWeight = if (r.linkReady) FontWeight.Normal else FontWeight.Bold)
            r.preferredAccepted?.let { Text("setPreferredDevice: ${if (it) "수락" else "거부"}", fontFamily = FontFamily.Monospace, fontSize = 11.sp) }

            HorizontalDivider()
            Text("타임라인 (0.5초 간격)", fontWeight = FontWeight.Bold, fontSize = 12.sp)
            Column(Modifier.horizontalScroll(rememberScrollState())) {
                Text("  시각   SCO  통신장치 / 통화트랙 / 무전트랙", fontFamily = FontFamily.Monospace, fontSize = 10.sp)
                r.timeline.forEach { t ->
                    Text("%5dms %-4s %s | %s | %s".format(
                            t.atMs, if (t.scoOn) "ON" else "off", t.commDevice, t.callRouted, t.radioRouted),
                         fontFamily = FontFamily.Monospace, fontSize = 10.sp)
                }
            }
            HorizontalDivider()
            Text("비고\n${r.detail}", fontSize = 11.sp)
        }
    }
    Text("이 화면을 캡처해 주시면 설계를 확정합니다.", style = MaterialTheme.typography.bodySmall)
}
