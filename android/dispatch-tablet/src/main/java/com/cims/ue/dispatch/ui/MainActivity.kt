// 앱 진입 (docs/design/features/android_dispatch_tablet.md §6.1·§6.2)
//
// Activity 는 화면만 든다. 엔진·세션은 DispatchService 가 갖고 있으므로 회전·구성 변경으로 이 클래스가
// 재생성돼도 등록이 끊기지 않는다 — 복원은 스냅샷 재조회뿐이다(§6.7).
package com.cims.ue.dispatch.ui

import android.Manifest
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchService
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionState
import com.cims.ue.dispatch.ui.ptt.PttTab
import com.cims.ue.dispatch.ui.ptt.TalkBar
import com.cims.ue.dispatch.ui.call.CallTab
import com.cims.ue.dispatch.ui.admin.AdminScreen
import com.cims.ue.dispatch.ui.groups.PttGroupsScreen
import com.cims.ue.dispatch.ui.history.HistoryScreen
import com.cims.ue.dispatch.ui.monitor.MonitorSheet
import com.cims.ue.dispatch.ui.monitor.rememberMonitorSessions

class MainActivity : ComponentActivity() {

    private val vm: MainViewModel by viewModels()

    /** 사용자가 [앱 종료] 를 골랐다 — 서비스까지 내리고 Activity 를 닫는다. */
    private fun shutdown() {
        DispatchService.shutdown(this)
        finishAndRemoveTask()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 세션(엔진)은 서비스가 든다 — Activity 보다 오래 산다.
        DispatchService.start(this)
        setContent { MaterialTheme(colorScheme = darkColorScheme()) { Root(vm, ::shutdown) } }
    }

    override fun onResume() {
        super.onResume()
        vm.refresh()          // 재생성·복귀 후 화면은 코어 스냅샷에서 다시 그린다
    }

    /** 하드 키보드가 붙어 있으면 F1~F4 로 화면을, Ctrl+1~9 로 채널을 고른다(§7). */
    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        AppScreen.ofFunctionKey(keyCode)?.let { vm.show(it); return true }
        if (event?.isCtrlPressed == true && keyCode in KeyEvent.KEYCODE_1..KeyEvent.KEYCODE_9) {
            vm.focusChannel(keyCode - KeyEvent.KEYCODE_1 + 1)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }
}

@Composable
private fun Root(vm: MainViewModel, onShutdown: () -> Unit) {
    // 세션은 Service 가 만든다 — 화면이 먼저 설 수 있으므로 «준비됨» 을 관측한다(§F8).
    val session by vm.sessionFlow.collectAsStateWithLifecycle()
    if (session == null) return Waiting()
    val state by (vm.state ?: return Waiting()).collectAsStateWithLifecycle()

    // 알림·마이크 권한 — 등록 유지 서비스와 발언에 필요하다.
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { }
    LaunchedEffect(Unit) {
        val want = buildList {
            add(Manifest.permission.RECORD_AUDIO)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.POST_NOTIFICATIONS)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) add(Manifest.permission.BLUETOOTH_CONNECT)
        }
        ask.launch(want.toTypedArray())
    }

    when (state) {
        SessionState.LOGGED_OUT, SessionState.LOGGING_IN, SessionState.FAILED -> LoginScreen(vm, onShutdown)
        else -> Shell(vm, onShutdown)
    }
}

/**
 * 자격 갱신 경고 띠.
 *
 * CSC 토큰 갱신이 실패하고 있으면 **이력·관리·PTT 그룹·주소록이 곧 막힌다.** 통화·무전은 H(A1) 이라
 * 멀쩡해서, 알리지 않으면 관제사는 자격이 죽어 가는 줄 모르고 조회를 누른 순간에야 안다.
 * 되살릴 수 없는 만료는 여기 뜨지 않는다 — 그건 앱 전체가 로그인 화면으로 간다(§6.1b).
 */
@Composable
private fun CredentialBanner(session: DispatchSession) {
    val why by session.credentialWarning.collectAsStateWithLifecycle()
    val text = why ?: return
    Surface(color = MaterialTheme.colorScheme.errorContainer, modifier = Modifier.fillMaxWidth()) {
        Text(text, Modifier.padding(horizontal = 14.dp, vertical = 6.dp),
            fontSize = 12.sp, color = MaterialTheme.colorScheme.onErrorContainer)
    }
}

@Composable
private fun Waiting() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
}

/** 관제 셸 — 하단 내비 넷 + 본문(§6.2). */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Shell(vm: MainViewModel, onShutdown: () -> Unit) {
    val screen by vm.screen.collectAsStateWithLifecycle()
    val tab by vm.tab.collectAsStateWithLifecycle()
    val session = DispatchService.session

    var sheetOpen by remember { mutableStateOf(false) }
    val monitors = session?.let { rememberMonitorSessions(it) }.orEmpty()
    // 마지막 감청이 끝나면 칩도 사라지므로 시트 플래그를 내려 둔다.
    LaunchedEffect(monitors.isEmpty()) { if (monitors.isEmpty()) sheetOpen = false }

    Scaffold(
        topBar = {
            val profile = session?.profile?.collectAsStateWithLifecycle()?.value
            val regs = session?.registrations?.collectAsStateWithLifecycle()?.value.orEmpty()
            TopAppBar(
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(profile?.displayName?.ifBlank { "관제" } ?: "관제", fontWeight = FontWeight.Bold)
                        session?.dispatch?.takeIf { it.present }?.let {
                            Text("${it.groupName} · 대표 ${it.pilotId}", fontSize = 13.sp)
                        }
                        // 등록 점등 — 권위는 코어 스냅샷이다
                        Text(regs.values.joinToString(" ") { r ->
                            if (r.registered) "●" else "○"
                        }, fontSize = 13.sp)
                    }
                },
                actions = {
                    // 감청 칩 — 어느 화면에서나 보이고, 눌러 전면 시트를 연다(§6.8).
                    if (monitors.isNotEmpty()) AssistChip(
                        onClick = { sheetOpen = true },
                        label = { Text("감청 ${monitors.size}", fontSize = 12.sp) })
                    SessionMenu(vm, onShutdown)
                })
        },
        bottomBar = {
            NavigationBar {
                AppScreen.entries.forEach { s ->
                    NavigationBarItem(
                        selected = screen == s,
                        onClick = { vm.show(s) },
                        icon = {
                            // [관리]의 점 배지 = 저장하지 않은 폼(§4.5). 전환은 막지 않는다.
                            if (s == AppScreen.ADMIN && vm.adminDirty)
                                BadgedBox(badge = { Badge() }) { Icon(iconOf(s), contentDescription = s.label) }
                            else Icon(iconOf(s), contentDescription = s.label)
                        },
                        label = { Text("${s.label} ${s.hotkey}") })
                }
            }
        }
    ) { pad ->
        Column(Modifier.fillMaxSize().padding(pad)) {
            // 착신 배너 — **화면과 무관하게** 상단 바 아래에 뜬다(§6.2). 이것이 유일한 전역 착신 표면이다.
            if (session != null) IncomingBanners(session, onAnswered = vm::goToCalls)
            // 자격 갱신이 흔들리는 동안 미리 알린다 — 조회를 누른 그 순간에야 튕기지 않게(§6.1b).
            if (session != null) CredentialBanner(session)
            // 관제 밖 화면에는 요약 띠가 붙는다(§6.2) — 다른 VM 의 투영이라 상태를 갖지 않는다.
            if (screen.showsSummaryStrip && session != null)
                SummaryStrip(session, vm.ptt, vm.calls, vm.lockTalk, onGoDispatch = { vm.show(AppScreen.DISPATCH) })
            when (screen) {
                AppScreen.DISPATCH -> DispatchCanvas(vm, tab)
                AppScreen.HISTORY -> vm.history?.let { HistoryScreen(it, Modifier.weight(1f)) } ?: Waiting()
                AppScreen.PTT_GROUPS -> vm.pttGroups?.let {
                    PttGroupsScreen(it, onGoDispatch = { vm.show(AppScreen.DISPATCH) }, Modifier.weight(1f))
                } ?: Waiting()
                AppScreen.ADMIN -> vm.admin?.let { AdminScreen(it, Modifier.weight(1f)) } ?: Waiting()
            }
        }
    }

    if (sheetOpen && session != null) MonitorSheet(session) { sheetOpen = false }
}

/**
 * 사람 메뉴 — 로그아웃·앱 종료.
 *
 * 둘은 다르다. **로그아웃**은 등록을 풀고 자격을 지우지만 서비스는 남는다(다른 사람이 이어 쓴다).
 * **앱 종료**는 서비스까지 내린다 — 그러지 않으면 START_STICKY 때문에 앱을 닫아도 계속 등록 상태로 남는다.
 * 둘 다 진행 중인 통화·무전을 끊으므로 확인을 받는다.
 */
@Composable
private fun SessionMenu(vm: MainViewModel, onShutdown: () -> Unit) {
    var open by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf<String?>(null) }
    var settings by remember { mutableStateOf(false) }
    val session = DispatchService.session

    if (settings && session != null) SettingsSheet(session) { settings = false }

    IconButton(onClick = { open = true }) {
        Icon(Icons.Filled.MoreVert, contentDescription = "메뉴")
    }
    DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
        DropdownMenuItem(
            text = { Text("설정") },
            leadingIcon = { Icon(Icons.Filled.Settings, contentDescription = null) },
            onClick = { open = false; settings = true })
        HorizontalDivider()
        DropdownMenuItem(
            text = { Text("로그아웃") },
            leadingIcon = { Icon(Icons.Filled.ExitToApp, contentDescription = null) },
            onClick = { open = false; confirm = "logout" })
        HorizontalDivider()
        DropdownMenuItem(
            text = { Text("앱 종료") },
            leadingIcon = { Icon(Icons.Filled.PowerSettingsNew, contentDescription = null) },
            onClick = { open = false; confirm = "exit" })
    }

    confirm?.let { what ->
        val logout = what == "logout"
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(if (logout) "로그아웃" else "앱 종료") },
            text = {
                Text(if (logout) "등록을 풀고 로그인 화면으로 돌아갑니다. 진행 중인 통화·무전이 끊깁니다."
                     else "등록을 풀고 앱을 완전히 끝냅니다. 종료 뒤에는 착신·무전을 받지 않습니다.")
            },
            confirmButton = {
                TextButton(onClick = {
                    confirm = null
                    if (logout) vm.logout() else onShutdown()
                }) { Text(if (logout) "로그아웃" else "종료") }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("취소") } })
    }
}

/** 관제 캔버스 — 탭 둘(§6.3). 발언 바는 탭 바깥에 고정한다. */
@Composable
private fun DispatchCanvas(vm: MainViewModel, tab: DispatchTab) {
    val ptt = vm.ptt
    Column(Modifier.fillMaxSize()) {
        // 발언 바 — 탭을 옮겨도 남는다(관제사는 전화를 받으면서 무전한다, §6.3)
        if (ptt != null) TalkBar(ptt, lockEnabled = vm.lockTalk)
        TabRow(selectedTabIndex = tab.ordinal) {
            DispatchTab.entries.forEach { t ->
                Tab(selected = tab == t, onClick = { vm.showTab(t) }, text = { Text(t.label) })
            }
        }
        when (tab) {
            DispatchTab.PTT ->
                if (ptt != null && vm.scoped != null && vm.messages != null && vm.activity != null)
                    PttTab(ptt, vm.scoped!!, vm.messages!!, vm.activity!!, Modifier.weight(1f))
                else Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
            DispatchTab.CALLS ->
                vm.calls?.let { CallTab(it, Modifier.weight(1f)) }
                    ?: Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
        }
    }
}

private fun iconOf(s: AppScreen) = when (s) {
    AppScreen.DISPATCH -> Icons.Filled.Headset
    AppScreen.HISTORY -> Icons.Filled.History
    AppScreen.PTT_GROUPS -> Icons.Filled.Groups
    AppScreen.ADMIN -> Icons.Filled.Settings
}
