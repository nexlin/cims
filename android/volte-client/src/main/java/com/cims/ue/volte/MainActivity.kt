package com.cims.ue.volte

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.cims.ue.core.calllog.CallEntry
import com.cims.ue.core.calllog.CallLogStore
import com.cims.ue.core.calllog.CallType
import com.cims.ue.core.account.SsoProvisioner
import com.cims.ue.core.config.ConfigStore
import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.contacts.CompanyContact
import com.cims.ue.core.contacts.CompanyDirectory
import com.cims.ue.core.contacts.CompanyDirectoryStore
import com.cims.ue.core.contacts.CompanyOrg
import com.cims.ue.core.contacts.Contact
import com.cims.ue.core.contacts.ContactStore
import com.cims.ue.core.sip.CallState
import com.cims.ue.core.sip.RegState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Locale

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) { App() }
            }
        }
    }
}

private enum class Screen { GATE, HOME, CONFIG }
private enum class Tab { CONTACTS, RECENTS, KEYPAD }

@Composable
private fun App() {
    val context = LocalContext.current
    val store = remember { ConfigStore(context) }
    var config by remember { mutableStateOf(store.load()) }
    // 공유 계정 있으면 진입 시 항상 최신 정보 재취득(GATE→재프로비저닝). 계정 없으면 캐시 설정으로 HOME.
    var screen by remember { mutableStateOf(
        if (com.cims.ue.core.account.SsoProvisioner.hasAccount(context)) Screen.GATE
        else if (config.isComplete()) Screen.HOME else Screen.GATE
    ) }

    when (screen) {
        // CIMS-Phone 는 자체 로그인 없음 — CIMS 공유 계정으로 자동 구성(SSO). 계정 없으면 CIMS 앱 로그인 유도.
        Screen.GATE -> SsoGateScreen(
            onProvisioned = { c -> store.save(c); config = c; screen = Screen.HOME },
            onManual = { screen = Screen.CONFIG },
        )
        Screen.CONFIG -> ConfigScreen(
            initial = config,
            canCancel = config.isComplete(),
            onSave = { c -> store.save(c); config = c; screen = Screen.HOME },
            onCancel = { screen = if (config.isComplete()) Screen.HOME else Screen.GATE },
        )
        Screen.HOME -> HomeScreen(
            config = config,
            onEditConfig = { screen = Screen.CONFIG },
        )
    }
}

// ─────────────────────────────────────── 로그인(자동 프로비저닝) ───────────────────────────────────────

@Composable
private fun SsoGateScreen(
    onProvisioned: (SipAccountConfig) -> Unit,
    onManual: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var hasAccount by remember { mutableStateOf(com.cims.ue.core.account.SsoProvisioner.hasAccount(context)) }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }

    fun provision() {
        busy = true; status = "CIMS 계정으로 구성 중…"
        scope.launch {
            val cfg = runCatching {
                withContext(Dispatchers.IO) {
                    val prof = com.cims.ue.core.account.SsoProvisioner.fetchProfile(context)
                        ?: error("CIMS 로그인 세션이 없습니다")
                    val svc = prof.service("volte") ?: error("이 계정에 VoLTE 서비스가 없습니다")
                    svc.toSipAccountConfig(
                        loginId = prof.loginId ?: "",
                        displayName = prof.displayName ?: "",
                        loginPassword = com.cims.ue.core.account.SsoProvisioner.loginPassword(context),
                    )
                }
            }
            busy = false
            cfg.onSuccess { onProvisioned(it) }.onFailure { status = it.message ?: "구성 실패" }
        }
    }

    // 진입 시 공유 계정 있으면 자동 구성(로그인 UI 없음).
    LaunchedEffect(hasAccount) { if (hasAccount && !busy) provision() }

    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text("CIMS", style = MaterialTheme.typography.displaySmall,
            fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
        Text("CIMS-Phone", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(16.dp))
        if (hasAccount) {
            Text("CIMS 계정으로 자동 구성합니다.", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(12.dp))
            if (busy) CircularProgressIndicator()
            if (status.isNotBlank()) {
                Spacer(Modifier.height(8.dp))
                Text(status, style = MaterialTheme.typography.bodySmall, textAlign = TextAlign.Center)
                if (!busy) { Spacer(Modifier.height(8.dp)); Button(onClick = { provision() }) { Text("다시 시도") } }
            }
        } else {
            Text("이 앱은 별도 로그인이 없습니다.\nCIMS 앱에서 먼저 로그인하세요.",
                style = MaterialTheme.typography.bodyMedium, textAlign = TextAlign.Center)
            Spacer(Modifier.height(16.dp))
            Button(
                onClick = {
                    val act = context as? android.app.Activity
                    android.accounts.AccountManager.get(context).addAccount(
                        com.cims.ue.core.account.CimsAccounts.ACCOUNT_TYPE,
                        com.cims.ue.core.account.CimsAccounts.TOKEN_PROVISIONING,
                        null, null, act,
                        { _ -> hasAccount = com.cims.ue.core.account.SsoProvisioner.hasAccount(context) },
                        android.os.Handler(android.os.Looper.getMainLooper()),
                    )
                },
                modifier = Modifier.fillMaxWidth().widthIn(max = 360.dp),
            ) { Text("CIMS 로그인 열기") }
        }
        Spacer(Modifier.height(20.dp))
        TextButton(onClick = onManual) { Text("수동 설정 (고급)") }
    }
}

// ─────────────────────────────────────── 전화 홈 (탭 구성) ───────────────────────────────────────

@Composable
private fun HomeScreen(
    config: SipAccountConfig,
    onEditConfig: () -> Unit,
) {
    val context = LocalContext.current
    val callLog = remember { CallLogStore(context) }
    val contacts = remember { ContactStore(context) }
    val companyDir = remember { CompanyDirectoryStore(context) }

    // SipService 바인딩 — 등록은 서비스가 자동으로 유지한다(수동 등록 버튼 없음).
    var service by remember { mutableStateOf<SipService?>(null) }
    DisposableEffect(Unit) {
        val conn = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                service = (binder as? SipService.LocalBinder)?.service
            }
            override fun onServiceDisconnected(name: ComponentName?) { service = null }
        }
        SipService.start(context)
        context.bindService(Intent(context, SipService::class.java), conn, Context.BIND_AUTO_CREATE)
        onDispose { runCatching { context.unbindService(conn) } }
    }

    // 통화/알림 권한 — 진입 시 1회 요청(승인되면 재등록 트리거).
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { service?.ensureRegistered() }
    LaunchedEffect(Unit) { permLauncher.launch(requiredPermissions()) }

    val fallbackReg = remember { MutableStateFlow<RegState>(RegState.Idle) }
    val fallbackCall = remember { MutableStateFlow<CallState>(CallState.Null) }
    val reg by (service?.regState ?: fallbackReg).collectAsState()
    val call by (service?.callState ?: fallbackCall).collectAsState()

    // 발신: 음성/영상 구분. 영상은 카메라 권한 확보 후 발신.
    var videoOn by remember { mutableStateOf(false) }
    var pendingVideoNumber by remember { mutableStateOf<String?>(null) }

    fun doDial(number: String, video: Boolean) {
        val n = number.trim()
        if (n.isBlank()) return
        videoOn = video
        service?.setVideoEnabled(video)
        callLog.add(extractNumber(n), CallType.OUTGOING)
        service?.makeCall(n)
    }
    val cameraLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        val num = pendingVideoNumber; pendingVideoNumber = null
        if (granted && !num.isNullOrBlank()) doDial(num, true) else videoOn = false
    }
    fun dial(number: String, video: Boolean) {
        if (number.isBlank()) return
        if (video) { pendingVideoNumber = number; cameraLauncher.launch(Manifest.permission.CAMERA) }
        else doDial(number, false)
    }

    // 수신/부재중 기록: Incoming→Active=수신(연결), Incoming→Disconnected(미연결)=부재중.
    var incomingNumber by remember { mutableStateOf<String?>(null) }
    var incomingAnswered by remember { mutableStateOf(false) }
    LaunchedEffect(call) {
        when (val c = call) {
            is CallState.Incoming -> { incomingNumber = extractNumber(c.remote); incomingAnswered = false }
            is CallState.Active -> if (incomingNumber != null) incomingAnswered = true
            is CallState.Disconnected -> {
                incomingNumber?.let { n ->
                    callLog.add(n, if (incomingAnswered) CallType.INCOMING else CallType.MISSED)
                    incomingNumber = null
                }
            }
            else -> {}
        }
    }

    var tab by remember { mutableStateOf(Tab.KEYPAD) }
    val inCall = call is CallState.Incoming || call is CallState.Outgoing || call is CallState.Active

    if (inCall) {
        CallScreen(
            call = call,
            videoOn = videoOn,
            onToggleVideo = { on ->
                videoOn = on
                if (on) cameraLauncher.launch(Manifest.permission.CAMERA) else service?.setVideoEnabled(false)
            },
            onSurface = { service?.setVideoSurface(it) },
            onAnswer = { id -> service?.answer(id) },
            onReject = { id -> service?.reject(id) },
            onHangup = { id -> service?.hangup(id) },
        )
    } else {
        Scaffold(
            topBar = { HeaderBar(reg, onEditConfig) },
            bottomBar = { BottomNav(tab) { tab = it } },
        ) { pad ->
            Box(Modifier.padding(pad).fillMaxSize()) {
                when (tab) {
                    Tab.CONTACTS -> ContactsScreen(
                        personal = contacts,
                        company = companyDir,
                        onCallVoice = { dial(it, false) },
                        onCallVideo = { dial(it, true) },
                    )
                    Tab.RECENTS -> RecentsScreen(
                        store = callLog,
                        contacts = contacts,
                        onCall = { dial(it, false) },
                    )
                    Tab.KEYPAD -> KeypadScreen(
                        myNumber = config.displayName.ifBlank { config.msisdn },
                        onVoice = { dial(it, false) },
                        onVideo = { dial(it, true) },
                    )
                }
            }
        }
    }
}

@Composable
private fun HeaderBar(reg: RegState, onSettings: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 12.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        RegStatusChip(reg)
        TextButton(onClick = onSettings) { Text("설정") }
    }
}

@Composable
private fun BottomNav(current: Tab, onSelect: (Tab) -> Unit) {
    NavigationBar {
        NavigationBarItem(
            selected = current == Tab.CONTACTS, onClick = { onSelect(Tab.CONTACTS) },
            icon = { Text("👤", fontSize = 20.sp) }, label = { Text("연락처") },
        )
        NavigationBarItem(
            selected = current == Tab.RECENTS, onClick = { onSelect(Tab.RECENTS) },
            icon = { Text("🕘", fontSize = 20.sp) }, label = { Text("최근기록") },
        )
        NavigationBarItem(
            selected = current == Tab.KEYPAD, onClick = { onSelect(Tab.KEYPAD) },
            icon = { Text("⌨", fontSize = 20.sp) }, label = { Text("키패드") },
        )
    }
}

// ─────────────────────────────────────── 키패드 탭 ───────────────────────────────────────

@Composable
private fun KeypadScreen(
    myNumber: String,
    onVoice: (String) -> Unit,
    onVideo: (String) -> Unit,
) {
    var dialed by remember { mutableStateOf("") }

    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (myNumber.isNotBlank()) {
            Spacer(Modifier.height(4.dp))
            Text("내 번호 $myNumber", style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        Spacer(Modifier.weight(1f))

        // 입력 번호 + 지우기
        Row(modifier = Modifier.fillMaxWidth().height(64.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(56.dp))
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Text(dialed, style = MaterialTheme.typography.displaySmall,
                    color = MaterialTheme.colorScheme.onSurface, maxLines = 1)
            }
            Box(Modifier.size(56.dp), contentAlignment = Alignment.Center) {
                if (dialed.isNotEmpty()) {
                    TextButton(onClick = { dialed = dialed.dropLast(1) }) { Text("⌫", fontSize = 24.sp) }
                }
            }
        }

        Spacer(Modifier.height(8.dp))
        Keypad(onDigit = { dialed += it })
        Spacer(Modifier.height(20.dp))

        // 음성/영상 발신 구분
        Row(horizontalArrangement = Arrangement.spacedBy(40.dp)) {
            LabeledRound("음성", CALL_GREEN, "📞", enabled = dialed.isNotBlank()) { onVoice(dialed.trim()) }
            LabeledRound("영상", VIDEO_BLUE, "📹", enabled = dialed.isNotBlank()) { onVideo(dialed.trim()) }
        }
        Spacer(Modifier.height(20.dp))
    }
}

@Composable
private fun Keypad(onDigit: (String) -> Unit) {
    val rows = listOf(
        listOf("1" to "", "2" to "ABC", "3" to "DEF"),
        listOf("4" to "GHI", "5" to "JKL", "6" to "MNO"),
        listOf("7" to "PQRS", "8" to "TUV", "9" to "WXYZ"),
        listOf("*" to "", "0" to "+", "#" to ""),
    )
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        rows.forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(28.dp)) {
                row.forEach { (digit, letters) -> KeypadKey(digit, letters) { onDigit(digit) } }
            }
        }
    }
}

@Composable
private fun KeypadKey(digit: String, letters: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .size(64.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .clickable { onClick() },
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(digit, fontSize = 26.sp, color = MaterialTheme.colorScheme.onSurface)
            if (letters.isNotBlank()) {
                Text(letters, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

// ─────────────────────────────────────── 연락처 탭 ───────────────────────────────────────

@Composable
private fun ContactsScreen(
    personal: ContactStore,
    company: CompanyDirectoryStore,
    onCallVoice: (String) -> Unit,
    onCallVideo: (String) -> Unit,
) {
    var seg by remember { mutableStateOf(0) }   // 0=회사, 1=개인

    Column(Modifier.fillMaxSize()) {
        // 회사/개인 구분 세그먼트
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            SegTab("회사", seg == 0, Modifier.weight(1f)) { seg = 0 }
            SegTab("개인", seg == 1, Modifier.weight(1f)) { seg = 1 }
        }
        when (seg) {
            0 -> CompanyContacts(company, onCallVoice, onCallVideo)
            else -> PersonalContacts(personal, onCallVoice, onCallVideo)
        }
    }
}

@Composable
private fun SegTab(label: String, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val bg = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant
    val fg = if (selected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant
    Box(
        modifier.clip(CircleShape).background(bg).clickable { onClick() }.padding(vertical = 8.dp),
        contentAlignment = Alignment.Center,
    ) { Text(label, color = fg, style = MaterialTheme.typography.labelLarge) }
}

/** 회사 연락처 — 서버 프로비저닝 제공, 읽기전용. 조직 트리(접기/펼치기) + 검색. */
@Composable
private fun CompanyContacts(
    store: CompanyDirectoryStore,
    onCallVoice: (String) -> Unit,
    onCallVideo: (String) -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var dir by remember { mutableStateOf(store.load()) }
    var loading by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var query by remember { mutableStateOf("") }
    var collapsed by remember { mutableStateOf(setOf<String>()) }   // 접힌 조직 code

    fun refresh() {
        if (loading) return
        loading = true; error = ""
        scope.launch {
            val fetched = withContext(Dispatchers.IO) { SsoProvisioner.fetchDirectory(context) }
            if (fetched != null) { store.replace(fetched); dir = fetched }
            else if (dir.members.isEmpty()) error = "회사 연락처를 불러오지 못했습니다."
            loading = false
        }
    }
    LaunchedEffect(Unit) { refresh() }   // 캐시 즉시 표시 + 백그라운드 최신화

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("회사 전화번호부", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.weight(1f))
            if (loading) CircularProgressIndicator(Modifier.size(20.dp))
            else TextButton(onClick = { refresh() }) { Text("새로고침") }
        }
        OutlinedTextField(
            value = query, onValueChange = { query = it },
            label = { Text("이름·번호 검색") }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        )

        val q = query.trim()
        if (dir.members.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(error.ifBlank { "회사 연락처가 없습니다." },
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else if (q.isNotBlank()) {
            // 검색 모드 — 트리 무시, 일치 가입자 평면 표시(소속 조직명 부제).
            val orgName = remember(dir) { dir.orgs.associate { it.code to it.name } }
            val hits = remember(dir, q) {
                dir.members.filter { it.name.contains(q, true) || it.number.contains(q) }
                    .sortedBy { it.name }
            }
            if (hits.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("검색 결과가 없습니다.", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            } else LazyColumn(Modifier.fillMaxSize()) {
                items(hits, key = { "hit:${it.number}" }) { m ->
                    CompanyMemberRow(m.name, m.number, orgName[m.orgCode] ?: m.orgCode, depth = 0,
                        onCallVoice = { onCallVoice(m.number) }, onCallVideo = { onCallVideo(m.number) })
                    HorizontalDivider()
                }
            }
        } else {
            // 트리 모드
            val rows = remember(dir, collapsed) { buildDirRows(dir, collapsed) }
            LazyColumn(Modifier.fillMaxSize()) {
                items(rows, key = { it.key }) { row ->
                    when (row) {
                        is DirRow.Org -> OrgHeaderRow(row) {
                            collapsed = if (row.org.code in collapsed) collapsed - row.org.code
                                        else collapsed + row.org.code
                        }
                        is DirRow.Member -> {
                            CompanyMemberRow(row.c.name, row.c.number, subtitle = null, depth = row.depth,
                                onCallVoice = { onCallVoice(row.c.number) },
                                onCallVideo = { onCallVideo(row.c.number) })
                            HorizontalDivider()
                        }
                    }
                }
            }
        }
    }
}

/** 트리 한 행 — 조직 헤더 또는 가입자. */
private sealed interface DirRow {
    val key: String
    data class Org(val org: CompanyOrg, val depth: Int, val expanded: Boolean, val count: Int) : DirRow {
        override val key get() = "org:${org.code}"
    }
    data class Member(val c: CompanyContact, val depth: Int) : DirRow {
        override val key get() = "mem:${c.orgCode}:${c.number}"
    }
}

/** 조직 트리를 펼침 상태에 맞춰 평면 행 목록으로 전개. */
private fun buildDirRows(dir: CompanyDirectory, collapsed: Set<String>): List<DirRow> {
    val orgByCode = dir.orgs.associateBy { it.code }
    val byParent = dir.orgs.groupBy { it.parent }
    val membersByOrg = dir.members.groupBy { it.orgCode }
    val countCache = HashMap<String, Int>()
    fun subtreeCount(code: String): Int = countCache.getOrPut(code) {
        (membersByOrg[code]?.size ?: 0) + (byParent[code]?.sumOf { subtreeCount(it.code) } ?: 0)
    }
    val out = ArrayList<DirRow>()
    fun walk(org: CompanyOrg, depth: Int) {
        val expanded = org.code !in collapsed
        out.add(DirRow.Org(org, depth, expanded, subtreeCount(org.code)))
        if (expanded) {
            byParent[org.code]?.sortedBy { it.sort }?.forEach { walk(it, depth + 1) }
            membersByOrg[org.code]?.sortedBy { it.name }?.forEach { out.add(DirRow.Member(it, depth + 1)) }
        }
    }
    dir.orgs.filter { it.parent.isBlank() || it.parent !in orgByCode }.sortedBy { it.sort }
        .forEach { walk(it, 0) }
    // 조직 미지정(고아) 가입자
    val orphan = dir.members.filter { it.orgCode.isBlank() || it.orgCode !in orgByCode }
    if (orphan.isNotEmpty()) {
        out.add(DirRow.Org(CompanyOrg("", "(조직 미지정)", "", 9999), 0, true, orphan.size))
        orphan.sortedBy { it.name }.forEach { out.add(DirRow.Member(it, 1)) }
    }
    return out
}

@Composable
private fun OrgHeaderRow(row: DirRow.Org, onToggle: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onToggle() }
            .padding(start = (row.depth * 16).dp, top = 10.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(if (row.expanded) "▾" else "▸", color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.size(20.dp), textAlign = TextAlign.Center)
        Spacer(Modifier.size(4.dp))
        Text("${row.org.name} (${row.count})", style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary)
    }
}

@Composable
private fun CompanyMemberRow(
    name: String, number: String, subtitle: String?, depth: Int,
    onCallVoice: () -> Unit, onCallVideo: () -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onCallVoice() }
            .padding(start = (depth * 16).dp, top = 10.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.secondaryContainer),
            contentAlignment = Alignment.Center,
        ) { Text(name.take(1).ifBlank { "?" }, color = MaterialTheme.colorScheme.onSecondaryContainer) }
        Spacer(Modifier.size(12.dp))
        Column(Modifier.weight(1f)) {
            Text(name.ifBlank { number }, style = MaterialTheme.typography.bodyLarge)
            Text(if (subtitle != null) "$number · $subtitle" else number,
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        TextButton(onClick = onCallVideo) { Text("📹", fontSize = 18.sp) }
    }
}

/** 개인 연락처 — 단말 로컬, 추가/수정/삭제 가능. */
@Composable
private fun PersonalContacts(
    store: ContactStore,
    onCallVoice: (String) -> Unit,
    onCallVideo: (String) -> Unit,
) {
    var list by remember { mutableStateOf(store.all()) }
    var editing by remember { mutableStateOf<Contact?>(null) }
    var showAdd by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("개인 연락처", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { showAdd = true }) { Text("+ 추가") }
        }
        OutlinedTextField(
            value = query, onValueChange = { query = it },
            label = { Text("이름·번호 검색") }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        )
        val q = query.trim()
        val shown = if (q.isBlank()) list
                    else list.filter { it.name.contains(q, true) || it.number.contains(q) }
        if (shown.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (list.isEmpty()) "저장된 개인 연락처가 없습니다.\n‘+ 추가’ 로 등록하세요."
                     else "검색 결과가 없습니다.",
                    textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else {
            LazyColumn(Modifier.fillMaxSize()) {
                items(shown, key = { it.id }) { c ->
                    ContactRow(c,
                        onCallVoice = { onCallVoice(c.number) },
                        onCallVideo = { onCallVideo(c.number) },
                        onEdit = { editing = c })
                    HorizontalDivider()
                }
            }
        }
    }

    if (showAdd) {
        ContactDialog(null,
            onDismiss = { showAdd = false },
            onSave = { name, num -> store.upsert(name, num); list = store.all(); showAdd = false })
    }
    editing?.let { c ->
        ContactDialog(c,
            onDismiss = { editing = null },
            onSave = { name, num -> store.upsert(name, num, c.id); list = store.all(); editing = null },
            onDelete = { store.delete(c.id); list = store.all(); editing = null })
    }
}

@Composable
private fun ContactRow(
    c: Contact,
    onCallVoice: () -> Unit,
    onCallVideo: () -> Unit,
    onEdit: () -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onCallVoice() }.padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primaryContainer),
            contentAlignment = Alignment.Center,
        ) { Text(c.name.take(1).ifBlank { "?" }, color = MaterialTheme.colorScheme.onPrimaryContainer) }
        Spacer(Modifier.size(12.dp))
        Column(Modifier.weight(1f)) {
            Text(c.name.ifBlank { c.number }, style = MaterialTheme.typography.bodyLarge)
            if (c.name.isNotBlank()) {
                Text(c.number, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        TextButton(onClick = onCallVideo) { Text("📹", fontSize = 18.sp) }
        TextButton(onClick = onEdit) { Text("수정") }
    }
}

@Composable
private fun ContactDialog(
    initial: Contact?,
    onDismiss: () -> Unit,
    onSave: (String, String) -> Unit,
    onDelete: (() -> Unit)? = null,
) {
    var name by remember { mutableStateOf(initial?.name ?: "") }
    var number by remember { mutableStateOf(initial?.number ?: "") }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (initial == null) "연락처 추가" else "연락처 수정") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(name, { name = it }, label = { Text("이름") }, singleLine = true)
                OutlinedTextField(number, { number = it.filter { ch -> ch.isDigit() || ch == '+' } },
                    label = { Text("번호 (MSISDN)") }, singleLine = true)
            }
        },
        confirmButton = {
            Button(enabled = name.isNotBlank() && number.isNotBlank(),
                onClick = { onSave(name, number) }) { Text("저장") }
        },
        dismissButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                if (onDelete != null) TextButton(onClick = onDelete) {
                    Text("삭제", color = HANGUP_RED)
                }
                TextButton(onClick = onDismiss) { Text("취소") }
            }
        },
    )
}

// ─────────────────────────────────────── 최근기록 탭 ───────────────────────────────────────

@Composable
private fun RecentsScreen(
    store: CallLogStore,
    contacts: ContactStore,
    onCall: (String) -> Unit,
) {
    var log by remember { mutableStateOf(store.all()) }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("최근기록", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.weight(1f))
            if (log.isNotEmpty()) TextButton(onClick = { store.clear(); log = emptyList() }) { Text("지우기") }
        }
        if (log.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("최근 통화 기록이 없습니다.", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else {
            LazyColumn(Modifier.fillMaxSize()) {
                items(log) { e ->
                    RecentRow(e, contacts.nameFor(e.number), onClick = { onCall(e.number) })
                    HorizontalDivider()
                }
            }
        }
    }
}

@Composable
private fun RecentRow(e: CallEntry, name: String?, onClick: () -> Unit) {
    val (glyph, glyphColor, missed) = when (e.type) {
        CallType.OUTGOING -> Triple("↗", MaterialTheme.colorScheme.onSurfaceVariant, false)
        CallType.INCOMING -> Triple("↙", MaterialTheme.colorScheme.onSurfaceVariant, false)
        CallType.MISSED -> Triple("↙", HANGUP_RED, true)
    }
    val typeLabel = when (e.type) {
        CallType.OUTGOING -> "발신"; CallType.INCOMING -> "수신"; CallType.MISSED -> "부재중"
    }
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onClick() }.padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(glyph, color = glyphColor, fontSize = 18.sp, modifier = Modifier.size(28.dp), textAlign = TextAlign.Center)
        Spacer(Modifier.size(8.dp))
        Column(Modifier.weight(1f)) {
            Text(name ?: e.number, style = MaterialTheme.typography.bodyLarge,
                color = if (missed) HANGUP_RED else MaterialTheme.colorScheme.onSurface)
            Text("$typeLabel · ${formatTime(e.time)}", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text("📞", fontSize = 18.sp)
    }
}

// ─────────────────────────────────────── 통화 화면 ───────────────────────────────────────

@Composable
private fun CallScreen(
    call: CallState,
    videoOn: Boolean,
    onToggleVideo: (Boolean) -> Unit,
    onSurface: (Any?) -> Unit,
    onAnswer: (Int) -> Unit,
    onReject: (Int) -> Unit,
    onHangup: (Int) -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Spacer(Modifier.height(48.dp))
        val (remote, stateLine) = when (val c = call) {
            is CallState.Incoming -> extractNumber(c.remote) to (if (videoOn) "영상 수신 전화" else "수신 전화")
            is CallState.Outgoing -> extractNumber(c.remote) to (if (videoOn) "영상 발신 중…" else "발신 중…")
            is CallState.Active -> extractNumber(c.remote) to "통화 중"
            else -> "" to ""
        }
        Text(remote, style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))

        if (call is CallState.Active) {
            var elapsed by remember { mutableStateOf(0) }
            LaunchedEffect(Unit) { while (true) { delay(1000); elapsed++ } }
            Text("%02d:%02d".format(elapsed / 60, elapsed % 60),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            Text(stateLine, style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // 수신 영상 렌더 — 통화/발신 중 + 영상 on
        if (videoOn && (call is CallState.Active || call is CallState.Outgoing)) {
            Spacer(Modifier.height(16.dp))
            VideoRender(onSurface = onSurface)
        }

        Spacer(Modifier.weight(1f))

        when (val c = call) {
            is CallState.Incoming -> Row(
                modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly,
            ) {
                LabeledRound("거절", HANGUP_RED, "✕") { onReject(c.id) }
                LabeledRound("받기", CALL_GREEN, "📞") { onAnswer(c.id) }
            }
            is CallState.Active -> {
                LabeledRound(
                    if (videoOn) "영상 끄기" else "영상 켜기", VIDEO_BLUE, "📹",
                ) { onToggleVideo(!videoOn) }
                Spacer(Modifier.height(20.dp))
                LabeledRound("종료", HANGUP_RED, "📞") { onHangup(c.id) }
            }
            is CallState.Outgoing -> LabeledRound("취소", HANGUP_RED, "📞") { onHangup(c.id) }
            else -> {}
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun LabeledRound(label: String, bg: Color, glyph: String, fg: Color = Color.White, enabled: Boolean = true, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        RoundButton(label = glyph, bg = bg, fg = fg, size = 72.dp, enabled = enabled, onClick = onClick)
        Spacer(Modifier.height(8.dp))
        Text(label, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
private fun RoundButton(
    label: String,
    bg: Color,
    fg: Color = Color.White,
    size: androidx.compose.ui.unit.Dp = 72.dp,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(size)
            .clip(CircleShape)
            .background(if (enabled) bg else bg.copy(alpha = 0.35f))
            .clickable(enabled = enabled) { onClick() },
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = fg, fontSize = 28.sp)
    }
}

@Composable
private fun RegStatusChip(reg: RegState) {
    val (dot, label) = when (reg) {
        is RegState.Registered -> CALL_GREEN to "통화 가능"
        RegState.Registering, RegState.Idle -> Color(0xFFF9A825) to "연결 중…"
        RegState.Unregistered -> Color(0xFFF9A825) to "등록 해제됨"
        is RegState.Failed -> HANGUP_RED to "오프라인"
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(Modifier.size(10.dp).clip(CircleShape).background(dot))
        Text(label, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
private fun VideoRender(onSurface: (Any?) -> Unit) {
    // SurfaceView 의 Surface 를 PJSIP 영상 윈도우로 전달. 컴포지션 이탈 시 surfaceDestroyed→null.
    AndroidView(
        modifier = Modifier.fillMaxWidth().aspectRatio(4f / 3f),
        factory = { ctx ->
            SurfaceView(ctx).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(h: SurfaceHolder) = onSurface(h.surface)
                    override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) = onSurface(h.surface)
                    override fun surfaceDestroyed(h: SurfaceHolder) = onSurface(null)
                })
            }
        },
    )
}

/** SIP URI("\"이름\" <sip:번호@도메인>")에서 표시용 번호만 추출. 패턴이 없으면 원문 반환. */
private fun extractNumber(remote: String): String {
    var s = remote.trim()
    if (s.contains("<") && s.contains(">")) s = s.substringAfter("<").substringBefore(">")
    s = s.removePrefix("sip:").removePrefix("sips:").removePrefix("tel:")
    s = s.substringBefore("@").substringBefore(";")
    return s.ifBlank { remote }
}

private fun formatTime(ts: Long): String =
    SimpleDateFormat("M월 d일 a h:mm", Locale.KOREA).format(java.util.Date(ts))

private fun requiredPermissions(): Array<String> = buildList {
    add(Manifest.permission.RECORD_AUDIO)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        add(Manifest.permission.POST_NOTIFICATIONS)
    }
}.toTypedArray()

private val CALL_GREEN = Color(0xFF2E7D32)
private val HANGUP_RED = Color(0xFFC62828)
private val VIDEO_BLUE = Color(0xFF1565C0)

// ─────────────────────────────────────── 설정 화면 (고급/수동) ───────────────────────────────────────

@Composable
private fun ConfigScreen(
    initial: SipAccountConfig,
    canCancel: Boolean,
    onSave: (SipAccountConfig) -> Unit,
    onCancel: () -> Unit,
) {
    var host by remember { mutableStateOf(initial.serverHost) }
    var port by remember { mutableStateOf(if (initial.serverPort > 0) initial.serverPort.toString() else "") }
    var transport by remember { mutableStateOf(initial.transport) }
    var domain by remember { mutableStateOf(initial.domain) }
    var msisdn by remember { mutableStateOf(initial.msisdn) }
    var imsi by remember { mutableStateOf(initial.imsi) }
    var name by remember { mutableStateOf(initial.displayName) }
    var loginId by remember { mutableStateOf(initial.loginId) }
    var authId by remember { mutableStateOf(initial.authId) }
    var password by remember { mutableStateOf(initial.password) }
    var showError by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("접속/계정 설정", style = MaterialTheme.typography.titleLarge)
        Text("자동 구성(SSO) 대신 수동으로 입력합니다.", style = MaterialTheme.typography.bodySmall)

        ConfigField("서버 IP/호스트", host) { host = it }
        ConfigField("SIP 포트", port) { port = it.filter { ch -> ch.isDigit() } }

        Text("전송 프로토콜", style = MaterialTheme.typography.bodyMedium)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SipAccountConfig.Transport.entries.forEach { t ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = transport == t, onClick = { transport = t })
                    Text(t.name)
                }
            }
        }

        ConfigField("도메인 (홈/서비스)", domain) { domain = it }
        ConfigField("MSISDN — 공개 ID (sip:번호@도메인)", msisdn) { msisdn = it }
        ConfigField("IMSI — 인증 ID 합성용 (IMSI@도메인)", imsi) { imsi = it.filter { ch -> ch.isDigit() } }
        ConfigField("이름", name) { name = it }
        ConfigField("로그인 ID", loginId) { loginId = it }
        ConfigField("auth_id (전체 IMPI 직접 입력 — 비우면 IMSI@도메인 합성)", authId) { authId = it }
        ConfigField("비밀번호", password, isPassword = true) { password = it }

        Text(
            "※ 공개 ID(MSISDN)와 인증 ID(IMSI@도메인)는 서로 다른 값입니다. 서버는 Digest username 으로 " +
                "IMSI@도메인 정확 일치를 요구하며, 불일치 시 즉시 403 으로 거부합니다.",
            style = MaterialTheme.typography.labelSmall,
        )

        if (showError) {
            Text(
                "필수 항목을 확인하세요: 서버/포트/도메인/MSISDN/IMSI(또는 auth_id)/비밀번호",
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
            )
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                val cfg = initial.copy(
                    serverHost = host.trim(),
                    serverPort = port.toIntOrNull() ?: 0,
                    transport = transport,
                    domain = domain.trim(),
                    msisdn = msisdn.trim(),
                    imsi = imsi.trim(),
                    displayName = name.trim(),
                    loginId = loginId.trim(),
                    authId = authId.trim(),
                    password = password,
                )
                if (cfg.isComplete()) onSave(cfg) else showError = true
            }) { Text("저장") }
            if (canCancel) OutlinedButton(onClick = onCancel) { Text("취소") }
        }
    }
}

@Composable
private fun ConfigField(
    label: String,
    value: String,
    isPassword: Boolean = false,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        visualTransformation = if (isPassword) PasswordVisualTransformation() else VisualTransformation.None,
        modifier = Modifier.fillMaxWidth(),
    )
}
