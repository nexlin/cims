// 앱 진입 (docs/design/features/android_dispatch_tablet.md §6.1·§6.2)
//
// Activity 는 화면만 든다. 엔진·세션은 DispatchService 가 갖고 있으므로 회전·구성 변경으로 이 클래스가
// 재생성돼도 등록이 끊기지 않는다 — 복원은 스냅샷 재조회뿐이다(§6.7).
package com.cims.ue.dispatch.ui

import androidx.compose.material.icons.filled.Search
import android.Manifest
import android.os.Build
import androidx.compose.ui.platform.LocalContext
import com.cims.ue.sdk.platform.BatteryExemption
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
import androidx.activity.compose.BackHandler
import androidx.compose.material.icons.automirrored.filled.Message
import com.cims.ue.dispatch.ui.ptt.ChannelScreen
import com.cims.ue.dispatch.ui.ptt.Messages
import com.cims.ue.dispatch.ui.ptt.PttScreen
import com.cims.ue.dispatch.ui.ptt.PttTabRow
import com.cims.ue.dispatch.ui.ptt.PttTabs
import com.cims.ue.dispatch.ui.ptt.Activity
import com.cims.ue.dispatch.ui.ptt.TalkBar
import com.cims.ue.dispatch.ui.call.CallTabRow
import com.cims.ue.dispatch.ui.call.CallsScreen
import com.cims.ue.dispatch.ui.call.ContactsPane
import com.cims.ue.dispatch.ui.call.SmsPane
import com.cims.ue.dispatch.ui.admin.AdminScreen
import com.cims.ue.dispatch.ui.groups.PttGroupsScreen
import com.cims.ue.dispatch.ui.history.HistoryScreen

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
    //   답이 오면 배터리 최적화 예외도 묻는다 — Doze 가 망·알람·wakelock 을 막지 않게(§6.1).
    val context = LocalContext.current
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        BatteryExemption.requestOnce(context)
    }
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
            fontSize = Type.body, color = MaterialTheme.colorScheme.onErrorContainer)
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
    val channel by vm.channel.collectAsStateWithLifecycle()
    val more by vm.more.collectAsStateWithLifecycle()
    val pttPane by vm.pttPane.collectAsStateWithLifecycle()
    val callPane by vm.callPane.collectAsStateWithLifecycle()
    val session = DispatchService.session

    var searchOpen by remember { mutableStateOf(false) }
    var settingsOpen by remember { mutableStateOf(false) }
    // 미읽음 SDS — 요약 띠가 하던 «어디에 뭐가 쌓였나» 를 내비 배지가 받는다(§6.3).
    val unread = vm.ptt?.cards?.collectAsStateWithLifecycle()?.value?.sumOf { it.unread } ?: 0

    // 뒤로가기 = 연 순서의 역순으로 한 겹씩(§6.3). **되돌릴 것이 있을 때만 가로챈다** —
    //   첫 화면에서 가로채면 앱을 벗어날 방법이 없어진다(시스템 기본 동작에 맡긴다).
    // 가로채기 판정은 **되돌릴 동작 그 자체**에 묻는다(`onBack() != null`). 조건을 따로 적으면
    //   `back()` 과 어긋나고, 어긋나는 순간 뒤로가기를 먹고도 화면이 그대로여서 앱을 못 닫는다.
    val canBack = NavState(screen, channel, more, pttPane, callPane).onBack() != null
    BackHandler(enabled = canBack) { vm.back() }

    val profile = session?.profile?.collectAsStateWithLifecycle()?.value
    val regs = session?.registrations?.collectAsStateWithLifecycle()?.value.orEmpty()

    AppShellContent(
        screen = screen,
        top = TopBarUi(
            displayName = profile?.displayName.orEmpty(),
            deskLine = session?.dispatch?.takeIf { it.present }
                ?.let { "${it.groupName} · 대표 ${it.pilotId}" }.orEmpty(),
            registrations = regs.values.map { it.registered }),
        badges = NavBadges(unread = unread, adminDirty = vm.adminDirty),
        onSelect = vm::show,
        onSearch = { searchOpen = true },
        menu = { SessionMenu(vm, onShutdown) },
        talkBar = { vm.ptt?.let { TalkBar(it, lockEnabled = vm.lockTalk) } },
        banners = {
            // 착신 배너 — **화면과 무관하게** 상단 바 아래에 뜬다(§6.2a). 유일한 전역 착신 표면이다.
            if (session != null) IncomingBanners(session, onAnswered = vm::goToCalls)
            // 자격 갱신이 흔들리는 동안 미리 알린다 — 조회를 누른 그 순간에야 튕기지 않게(§6.1b).
            if (session != null) CredentialBanner(session)
        },
        pttPane = pttPane,
        callPane = callPane,
        onPage = vm::showPage,
        // 탭줄은 면 pager **위에 고정**으로 놓인다 — 면을 밀 때 줄은 제자리, 본문만 미끄러진다(§6.3).
        tabs = { page ->
            when (page.screen) {
                AppScreen.PTT -> PttTabRow(
                    page.pttPane ?: PttPane.CHANNELS, vm::setPttPane, unread)
                AppScreen.CALLS -> CallTabRow(page.callPane ?: CallPane.CALLS, vm::setCallPane)
                else -> Unit
            }
        },
    ) { page ->
        // 한 장 = 메뉴 하나의 면 하나다. 옆으로 밀면 [APP_PAGES] 의 다음 장이 오고, 그 장이 다른
        //   메뉴에 속하면 하단 내비도 따라 옮겨진다(§6.3).
        when (page.screen) {
            AppScreen.HISTORY -> vm.history?.let { HistoryScreen(it, Modifier.weight(1f)) } ?: Waiting()

            // [무전] — 면 셋. 채널을 열었으면 그 화면이 «채널» 면을 대신한다(§6.3a).
            AppScreen.PTT -> {
                val ptt = vm.ptt; val scoped = vm.scoped
                val msg = vm.messages; val act = vm.activity
                if (ptt == null || scoped == null || msg == null || act == null) Waiting()
                else Box(Modifier.weight(1f)) {
                    when (page.pttPane ?: PttPane.CHANNELS) {
                        PttPane.CHANNELS ->
                            if (channel != null) ChannelScreen(
                                id = channel!!, channels = ptt, scoped = scoped,
                                onBack = vm::closeChannel, onShowRoster = vm::showRoster)
                            else PttScreen(ptt, scoped, onOpen = vm::openChannel)
                        PttPane.MESSAGES -> Box(Modifier.fillMaxSize().padding(8.dp)) { Messages(msg) }
                        PttPane.EVENTS -> Box(Modifier.fillMaxSize().padding(8.dp)) { Activity(act) }
                    }
                }
            }

            AppScreen.CALLS -> vm.calls?.let {
                CallsScreen(it, pane = page.callPane ?: CallPane.CALLS, onPane = vm::setCallPane,
                    onPerson = vm::runPersonAction,
                    bookPane = { ContactsPane(it, vm::runPersonAction) },
                    smsPane = { vm.sms?.let { m -> SmsPane(m) } ?: Waiting() },
                    showTabs = false,
                    modifier = Modifier.weight(1f))
            } ?: Waiting()

            AppScreen.MORE -> when (more) {
                MoreItem.PTT_GROUPS -> vm.pttGroups?.let {
                    PttGroupsScreen(it, onGoDispatch = { vm.show(AppScreen.PTT) }, Modifier.weight(1f))
                } ?: Waiting()
                MoreItem.ADMIN -> vm.admin?.let { AdminScreen(it, Modifier.weight(1f)) } ?: Waiting()
                null -> MoreScreen(onOpen = vm::openMore, onSettings = { settingsOpen = true },
                    dirty = vm.adminDirty, modifier = Modifier.weight(1f))
            }
        }
    }

    if (settingsOpen && session != null) SettingsSheet(session) { settingsOpen = false }

    // 통합 검색 — 사람 목록은 ③ VM 이 이미 묶어 두었다(두 벌로 만들지 않는다).
    val searchVm = vm.calls
    if (searchOpen && session != null && searchVm != null) {
        val people by searchVm.people.collectAsStateWithLifecycle()
        val groups by session.groups.collectAsStateWithLifecycle()
        SearchSheet(
            people = people, groups = groups,
            onPerson = { a, n -> vm.runPersonAction(a, n) },
            onChannel = { id -> vm.focusChannel(id) },
            onDismiss = { searchOpen = false })
    }
}

/**
 * 세션 메뉴 — 로그아웃·앱 종료·설정.
 *
 * 데스크톱의 «사람 메뉴»(`PersonActionsViewModel` — 주소록의 한 사람에게 사설콜·SDS·통화를 거는 메뉴)와
 * 다른 것이다. 이름이 겹치지 않게 «세션 메뉴» 로 부른다.
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

internal fun navIconOf(s: AppScreen) = when (s) {
    AppScreen.HISTORY -> Icons.Filled.History
    AppScreen.PTT -> Icons.Filled.Headset
    AppScreen.CALLS -> Icons.Filled.Call
    AppScreen.MORE -> Icons.Filled.MoreHoriz
}

