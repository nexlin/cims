package com.cims.ue.ptt.ui

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.GroupCallState
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.ListenPolicy
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.PttService
import com.cims.ue.ptt.Speaker
import com.cims.ue.ptt.csc.GroupDoc
import com.cims.ue.ptt.csc.GroupSummary
import com.cims.ue.ptt.floor.FloorState
import kotlinx.coroutines.flow.MutableStateFlow

/** 화면 라우트 — 시안 구조(스플래시 → 하단내비 4탭 + 푸시 화면들). */
sealed interface Nav {
    data object Splash : Nav
    data class Home(val tab: Tab) : Nav
    data class Channel(val groupId: String) : Nav
    data class Thread(val peer: String) : Nav
}

/** 컨트롤러 상태 묶음 — 화면들에 한 번에 전달. */
class PttUiState(
    val ctl: PttController?,
    val reg: RegState,
    val floor: FloorState,
    val speaker: Speaker?,
    val status: String,
    val groups: List<GroupSummary>,
    val groupDocs: Map<String, GroupDoc>,
    val sessions: List<GroupCallState>,
    val affiliated: Set<String>,
    val policy: ListenPolicy,
    val route: Int,
    val hasAccount: Boolean,
) {
    val primary: GroupCallState? get() = sessions.firstOrNull { it.role == com.cims.ue.ptt.ChannelRole.PRIMARY }
    val inCall: Boolean get() = sessions.any { it.active || it.callId >= 0 }
    val emergencySession: GroupCallState? get() = sessions.firstOrNull { it.emergency }
    fun session(groupId: String): GroupCallState? = sessions.firstOrNull { it.groupId == groupId }
    fun groupName(groupId: String): String =
        groups.firstOrNull { PttController.bareId(it.uri) == groupId }?.displayName ?: groupId
}

@Composable
fun AppRoot(svc: PttService?, onStopSip: () -> Unit) {
    val context = LocalContext.current
    val ctl: PttController? = if (svc != null) svc.controllerFlow.collectAsState().value else null

    // 컨트롤러 미생성 구간(SSO 재취득 중) 폴백 flow — 늦게 생성돼도 재구성으로 반영
    val fbReg = remember { MutableStateFlow<RegState>(RegState.Idle) }
    val fbFloor = remember { MutableStateFlow(FloorState.IDLE) }
    val fbSpeaker = remember { MutableStateFlow<Speaker?>(null) }
    val fbStatus = remember { MutableStateFlow("서비스 연결 중…") }
    val fbGroups = remember { MutableStateFlow<List<GroupSummary>>(emptyList()) }
    val fbGroupDocs = remember { MutableStateFlow<Map<String, GroupDoc>>(emptyMap()) }
    val fbAffil = remember { MutableStateFlow<Set<String>>(emptySet()) }
    val fbSessions = remember { MutableStateFlow<List<GroupCallState>>(emptyList()) }
    val fbPolicy = remember { MutableStateFlow(ListenPolicy.ALL) }
    val fbRoute = remember { MutableStateFlow(SipController.AUDIO_ROUTE_DEFAULT) }

    val st = PttUiState(
        ctl = ctl,
        reg = (ctl?.regState ?: fbReg).collectAsState().value,
        floor = (ctl?.floorState ?: fbFloor).collectAsState().value,
        speaker = (ctl?.speaker ?: fbSpeaker).collectAsState().value,
        status = (ctl?.status ?: fbStatus).collectAsState().value,
        groups = (ctl?.groups ?: fbGroups).collectAsState().value,
        groupDocs = (ctl?.groupDocs ?: fbGroupDocs).collectAsState().value,
        sessions = (ctl?.sessions ?: fbSessions).collectAsState().value,
        affiliated = (ctl?.affiliated ?: fbAffil).collectAsState().value,
        policy = (ctl?.listenPolicy ?: fbPolicy).collectAsState().value,
        route = (ctl?.audioRoute ?: fbRoute).collectAsState().value,
        hasAccount = remember(ctl) { com.cims.ue.core.account.SsoProvisioner.hasAccount(context) },
    )

    // SSO: 컨트롤러 연결 시 CIMS 공유 계정의 MCPTT(TS 33.180) 토큰을 주입(별도 로그인 없음).
    LaunchedEffect(ctl) {
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

    // 마이크/알림 권한 — 홈 진입 시 1회 요청
    val perm = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        svc?.ensureRegistered()
    }

    var nav by remember { mutableStateOf<Nav>(Nav.Splash) }
    var showKeyConfig by remember { mutableStateOf(false) }

    LaunchedEffect(nav) {
        if (nav is Nav.Home && ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            perm.launch(buildList {
                add(Manifest.permission.RECORD_AUDIO)
                if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
            }.toTypedArray())
        }
    }

    // 화면 최상단 전역 상태 아이콘 배지(다른 앱 위에 표시) — 권한 안내 1회(CIMS-Phone 과 동일 패턴).
    var askOverlay by remember {
        mutableStateOf(
            !android.provider.Settings.canDrawOverlays(context) &&
                !context.getSharedPreferences("ui_prefs", android.content.Context.MODE_PRIVATE)
                    .getBoolean("overlay_asked", false),
        )
    }
    if (askOverlay && nav is Nav.Home) {
        fun done() {
            context.getSharedPreferences("ui_prefs", android.content.Context.MODE_PRIVATE)
                .edit().putBoolean("overlay_asked", true).apply()
            askOverlay = false
        }
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { done() },
            title = { androidx.compose.material3.Text("화면 상단 상태 표시") },
            text = { androidx.compose.material3.Text("어떤 앱을 쓰고 있어도 화면 맨 위에 PTT 가능 상태 아이콘을 표시하려면 '다른 앱 위에 표시' 권한이 필요합니다.") },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = {
                    runCatching {
                        context.startActivity(
                            android.content.Intent(
                                android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                android.net.Uri.parse("package:${context.packageName}"),
                            ).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK),
                        )
                    }
                    done()
                }) { androidx.compose.material3.Text("허용하러 가기") }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(onClick = { done() }) {
                    androidx.compose.material3.Text("나중에")
                }
            },
        )
    }

    BackHandler(enabled = nav !is Nav.Home && nav !is Nav.Splash) {
        nav = when (nav) {
            is Nav.Channel -> Nav.Home(Tab.CHANNELS)
            is Nav.Thread -> Nav.Home(Tab.MESSAGES)
            else -> Nav.Home(Tab.MAIN)
        }
    }
    BackHandler(enabled = (nav as? Nav.Home)?.tab?.let { it != Tab.MAIN } == true) {
        nav = Nav.Home(Tab.MAIN)
    }

    Box(Modifier.fillMaxSize().background(Ct.Bg)) {
        when (val n = nav) {
            Nav.Splash -> SplashScreen(st, onReady = { nav = Nav.Home(Tab.MAIN) })
            is Nav.Home -> HomeScaffold(
                st = st, svc = svc, tab = n.tab,
                onTab = { nav = Nav.Home(it) },
                onOpenChannel = { nav = Nav.Channel(it) },
                onOpenThread = { nav = Nav.Thread(it) },
                onStopSip = onStopSip,
                onOpenKeyConfig = { showKeyConfig = true },
            )
            is Nav.Channel -> ChannelDetailScreen(
                st = st, groupId = n.groupId,
                onBack = { nav = Nav.Home(Tab.CHANNELS) },
                onOpenThread = { nav = Nav.Thread(it) },
            )
            is Nav.Thread -> MessageThreadScreen(
                st = st, svc = svc, peer = n.peer,
                onBack = { nav = Nav.Home(Tab.MESSAGES) },
            )
        }

        // 하드웨어 버튼 설정 오버레이 — 같은 Activity 윈도우(물리 키가 dispatchKeyEvent 로 유입되게)
        if (showKeyConfig) KeyConfigOverlay(onDismiss = { HwPtt.cancelLearn(); showKeyConfig = false })
    }
}

/** 하단내비 4탭 홈 — 탭 콘텐츠 + 긴급 배너 오버레이. */
@Composable
private fun HomeScaffold(
    st: PttUiState,
    svc: PttService?,
    tab: Tab,
    onTab: (Tab) -> Unit,
    onOpenChannel: (String) -> Unit,
    onOpenThread: (String) -> Unit,
    onStopSip: () -> Unit,
    onOpenKeyConfig: () -> Unit,
) {
    val msgTick = svc?.messageTick?.collectAsState()?.value ?: 0
    val unread = remember(msgTick, svc) { svc?.messages?.threads()?.sumOf { it.unread } ?: 0 }

    Column(Modifier.fillMaxSize().statusBarsPadding()) {
        // 긴급 상태 — 어느 탭에서든 최상단 붉은 점멸 배너
        st.emergencySession?.let { e -> EmergencyBanner(e, st.ctl, Modifier.padding(horizontal = 16.dp)) }

        Box(Modifier.weight(1f)) {
            when (tab) {
                Tab.MAIN -> MainChannelScreen(st, svc, onOpenThread = onOpenThread)
                Tab.CHANNELS -> ChannelsScreen(st, onOpenChannel = onOpenChannel, onOpenThread = onOpenThread)
                Tab.MESSAGES -> MessagesScreen(st, svc, onOpenThread = onOpenThread)
                Tab.SETTINGS -> SettingsScreen(st, svc, onStopSip = onStopSip, onOpenKeyConfig = onOpenKeyConfig)
            }
        }
        AppBottomNav(current = tab, badge = mapOf(Tab.MESSAGES to unread), onSelect = onTab)
    }
}
