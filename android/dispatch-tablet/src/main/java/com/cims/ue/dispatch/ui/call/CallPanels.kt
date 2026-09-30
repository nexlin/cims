@file:OptIn(ExperimentalFoundationApi::class)
// [관제] › [통화] 의 면 넷 — ③ 일반통화 + ⑥ 통화 내역 (android_dispatch_tablet.md §6.3)
//
// 본문 폭(레일을 뺀 1200)을 다 쓰므로 «통화» 면을 **2열**로 편다 — 좌: 키패드 / 우: 대표번호 대기열·진행 중·내 통화·그룹원 띠.
// 데스크톱은 1열이지만 태블릿은 세로 스크롤을 줄이는 쪽이 낫다.
// DTMF·전달은 한 통화에 묶인 조작이라 **카드 안에서** 편다(§6.6).
package com.cims.ue.dispatch.ui.call

import com.cims.ue.dispatch.ui.CimsFilterChip

import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import com.cims.ue.dispatch.ui.CallPane
import com.cims.ue.dispatch.ui.PersonAction
import com.cims.ue.dispatch.ui.PersonMenu
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.CallLogKind
import com.cims.ue.dispatch.session.CallLogRow
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.ui.Tag
import com.cims.ue.sdk.MediaSource

private val hhmmss = SimpleDateFormat("HH:mm:ss", Locale.KOREA)

private fun fmt(ms: Long): String {
    val t = ms / 1000
    return if (t >= 3600) "%d:%02d:%02d".format(t / 3600, (t % 3600) / 60, t % 60)
    else "%02d:%02d".format(t / 60, t % 60)
}

/**
 * [통화] 메뉴 — **VM 을 붙이는 껍데기**.
 *
 * @param onPerson 사람 메뉴가 고른 행동. **여기서 처리하지 않고 올린다** — 개별·애드혹 통화·SDS 는 [무전] 의
 *   상태를 건드리므로 둘을 다 아는 곳(`MainViewModel`)이 이어야 한다(데스크톱도 `MainViewModel` 이 잇는다).
 */
@Composable
fun CallsScreen(
    vm: CallDeskViewModel,
    pane: CallPane,
    onPane: (CallPane) -> Unit,
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
    bookPane: @Composable () -> Unit = {},
    smsPane: @Composable () -> Unit = {},
    /** false = 탭 줄은 껍데기(`DispatchTabs`)가 그린다(앱 경로). */
    showTabs: Boolean = true,
    /** ⑥ [이력에서 보기] — 최상위 [이력] 으로. */
    onHistory: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val ui = CallsUi(
        dialNumber = vm.dialNumber.collectAsStateWithLifecycle().value,
        book = vm.book.collectAsStateWithLifecycle().value,
        members = vm.members.collectAsStateWithLifecycle().value,
        queue = vm.queue.collectAsStateWithLifecycle().value,
        tally = vm.tally.collectAsStateWithLifecycle().value,
        deskFilter = vm.deskFilter.collectAsStateWithLifecycle().value,
        calls = vm.calls.collectAsStateWithLifecycle().value,
        transferTarget = vm.transferTarget.collectAsStateWithLifecycle().value,
        log = vm.callLog.collectAsStateWithLifecycle().value,
        live = vm.live.collectAsStateWithLifecycle().value,
        liveHint = vm.liveHint.collectAsStateWithLifecycle().value,
        watchDiag = vm.watchDiag.collectAsStateWithLifecycle().value,
        listenHidden = vm.listenHidden,
        personFilter = vm.personFilter.collectAsStateWithLifecycle().value
            .takeIf { it.isNotBlank() }?.let { vm.personFilterLabel().ifBlank { it } }.orEmpty(),
        rxLevels = vm.rxLevels.collectAsStateWithLifecycle().value)
    val act = CallsActions(
        setDialNumber = vm::setDialNumber, dial = vm::dial, dialTo = vm::dialTo, pickup = vm::pickup,
        setRxLevel = vm::setRxLevel,
        openHistory = onHistory, logCsv = vm::logCsv,
        setDeskFilter = vm::setDeskFilter, personAt = vm::personAt, monitorMember = vm::monitor,
        answer = vm::answer, answerQueue = vm::answerQueue, reject = vm::reject, hangup = vm::hangup, toggleHold = vm::toggleHold, toggleMute = vm::toggleMute,
        openDtmf = vm::openDtmf, closeDtmf = vm::closeDtmf, sendDtmf = vm::sendDtmf,
        openTransfer = vm::openTransfer, closeTransfer = vm::closeTransfer,
        setTransferTarget = vm::setTransferTarget, pickTransferTarget = vm::pickTransferTarget,
        transfer = vm::transfer, consult = vm::consult,
        completeConsult = vm::completeConsult, cancelConsult = vm::cancelConsult,
        monitorLive = vm::monitorLive, stopMonitorLive = vm::stopMonitorLive,
        clearPersonFilter = { vm.setPersonFilter(vm.personFilter.value) })

    CallsScreenContent(ui, act, pane, onPane, onPerson,
        bookPane = bookPane, smsPane = smsPane, showTabs = showTabs, modifier = modifier)
}

/**
 * [통화] 본문 — 면 넷, **순수 컴포저블**(android_dispatch_tablet.md §6.3).
 *
 * 앱에서는 탭 줄([무전|통화] + 하위 탭)을 껍데기가 그리고(`showTabs = false`), 이 장은 면 하나만 그린다.
 * `showTabs = true` 는 이 장만 따로 볼 때(미리보기)의 탭 줄이다.
 *
 * 데스크톱은 ③⑥ 을 2열 + 전폭으로 한 화면에 편다. 태블릿에서 그대로 하면 다섯 목록이 한 화면을 나눠 가져
 * 각각 서너 줄이 된다 — 특히 «내 통화» 와 «내역» 은 훑는 목록이라 그러면 쓸모가 없다. 그래서 **하는 일로**
 * 나누고 각 면이 전체 높이를 쓴다: 지금 벌어지는 통화 / 거는 상대 / 지난 기록 / 듣고 있는 것.
 *
 * **감청을 위한 자리는 없다.** 감청은 통화에 붙는 leg 이라 «통화» 면 «진행 중» 행에서 켜고 끄며, 켜져
 * 있으면 그 행이 «청취 중» 이라고 말한다. PTT 청취도 같은 이유로 [무전] 의 범위 채널 행에 있다.
 *
 * **면 전환은 탭이다.** 가로 스와이프는 최상위 메뉴 이동 전용이다(§6.3).
 */
@Composable
fun CallsScreenContent(
    ui: CallsUi,
    act: CallsActions = CallsActions(),
    pane: CallPane = CallPane.CALLS,
    onPane: (CallPane) -> Unit = {},
    onPerson: (PersonAction, String) -> Unit = { _, _ -> },
    bookPane: @Composable () -> Unit = {},
    smsPane: @Composable () -> Unit = {},
    /** false = 탭 줄은 껍데기(`DispatchTabs`)가 그린다(앱 경로). true = 이 장이 하위 탭을 직접 그린다(미리보기). */
    showTabs: Boolean = true,
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxSize()) {
        if (showTabs) CallTabRow(pane, onPane)
        when (pane) {
            // 각 면은 `weight` 로 남은 높이를 받는다 — `fillMaxSize` 면 TabRow 높이만큼 넘친다.
            //
            // «통화» 면은 **2열**이다. 본문 폭 1200dp 를 한 열로 쓰면 키패드가 화면을 먹거나 목록이 짧아진다 —
            //   왼쪽은 **거는 일**(키패드), 오른쪽은 **벌어지는 일**(진행 중·내 통화·대기열·그룹원).
            CallPane.CALLS -> Row(Modifier.weight(1f)) {
                Column(Modifier.width(320.dp).fillMaxHeight().verticalScroll(rememberScrollState())) {
                    Keypad(ui, act)
                }
                VerticalDivider()
                // 오른쪽 칸은 **통째로** 스크롤한다 — 감청을 펼치거나 진행 중이 늘어도 내 통화·그룹원이 잘리지 않는다.
                Column(Modifier.weight(1f).fillMaxHeight().verticalScroll(rememberScrollState()).padding(8.dp)) {
                    SectionTitle("대표번호 대기열")
                    Queue(ui, act)
                    Spacer(Modifier.height(8.dp))
                    // 진행 중 = 감시 대상의 살아 있는 통화. **감청을 켜고 끄는 자리**다(§6.3).
                    SectionTitle("진행 중" + if (ui.live.isNotEmpty()) " ${ui.live.size}" else "")
                    LiveCalls(ui, act)
                    Spacer(Modifier.height(8.dp))
                    SectionTitle("내 통화")
                    MyCalls(ui, act, onPerson)
                    Spacer(Modifier.height(8.dp))
                    // 그룹원 **띠** — 면이 아니다. 거는 일은 «주소록» 이 받고 여기는 상태를 곁눈질하는 자리다
                    //   (데스크톱 ③ 의 그룹원 띠, §4.4). 번호·상태·당겨받기·청취만 한 줄에.
                    SectionTitle("관제 그룹원 ${ui.members.size}")
                    MembersStrip(ui, act, onPerson)
                }
            }
            CallPane.BOOK -> Box(Modifier.weight(1f)) { bookPane() }
            CallPane.MESSAGES -> Box(Modifier.weight(1f)) { smsPane() }
            CallPane.LOG -> Column(Modifier.weight(1f).padding(8.dp)) {
                SectionTitle("오늘 데스크")
                Tally(ui, act)
                Spacer(Modifier.height(8.dp))
                SectionTitle("지난 통화")
                CallLog(ui, act, onPerson)
            }
        }
    }
}

@Composable
private fun SectionTitle(t: String) =
    Text(t, fontWeight = FontWeight.Bold, fontSize = Type.title, modifier = Modifier.padding(top = 4.dp, bottom = 6.dp))

/**
 * 통화 카드 틀 — 흰 면 + 옅은 테두리. **울리는 호·착신은 굵은 검정 테두리**로 가른다(받을 것이 먼저 보이게). 긴급·임박 색은
 * 무전의 상태라 여기 쓰지 않는다. 안은 `Card` 처럼 **세로로 쌓는다**(`Surface` 는 겹쳐 쌓는다 — 감청 펼침 줄이 머리 위에 겹친다).
 */
@Composable
private fun CallCardFrame(attention: Boolean, modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    val p = com.cims.ue.dispatch.ui.Tokens.palette
    Surface(color = if (attention) p.bar else p.paper, contentColor = p.ink, shape = RoundedCornerShape(10.dp),
        border = androidx.compose.foundation.BorderStroke(if (attention) 2.dp else 1.dp, if (attention) p.ink else p.divider),
        modifier = modifier) { Column(content = content) }
}

// ── 키패드 — «거는 일» 의 자리 ───────────────────────────────────────────────
@Composable
private fun Keypad(ui: CallsUi, act: CallsActions) {
    val number = ui.dialNumber
    val book = ui.book

    Column(Modifier.fillMaxWidth().padding(12.dp),
        horizontalAlignment = Alignment.CenterHorizontally) {
        // 입력란과 키패드는 **같은 번호**를 본다 — 키보드로 쳐도, 눌러도 한 값이다.
        // 치는 동안 주소록 제안이 입력란 아래에 겹쳐 뜬다(데스크톱 ③ 번호 필드의 제안 팝업). 행을 누르면 채우고,
        //   [발신] 은 곧바로 건다. 제안은 입력란의 포커스를 뺏지 않는다 — 계속 칠 수 있어야 한다.
        val suggestions = remember(number, book) { book.suggest(number) }
        var hidden by remember { mutableStateOf("") }          // 닫은 입력값 — 값이 바뀌면 다시 뜬다
        Box {
            OutlinedTextField(
                value = number, onValueChange = act.setDialNumber,
                placeholder = { Text("번호·내선", fontSize = Type.body) },
                singleLine = true, modifier = Modifier.fillMaxWidth(),
                textStyle = LocalTextStyle.current.copy(fontSize = Type.head,
                    fontWeight = FontWeight.Bold, textAlign = TextAlign.Center),
                // 입력한 번호의 주인을 바로 보여 준다 — 잘못 건 전화를 줄인다.
                supportingText = {
                    val who = if (number.isBlank()) "" else book.nameOf(number)
                    if (who.isNotBlank()) Text(who, fontSize = Type.body,
                        color = MaterialTheme.colorScheme.primary, textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth())
                },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Phone, imeAction = ImeAction.Go))
            DropdownMenu(
                expanded = suggestions.isNotEmpty() && hidden != number && book.nameOf(number).isBlank(),
                onDismissRequest = { hidden = number },
                properties = androidx.compose.ui.window.PopupProperties(focusable = false),
                modifier = Modifier.heightIn(max = 300.dp)) {
                suggestions.forEach { e ->
                    DropdownMenuItem(
                        text = {
                            Column {
                                Text("${e.name}  ${e.msisdn}", fontSize = Type.body, fontWeight = FontWeight.Medium, maxLines = 1)
                                val path = book.orgPath(e.org)
                                if (path.isNotEmpty()) Text(path, fontSize = Type.micro,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                            }
                        },
                        onClick = { act.setDialNumber(e.msisdn) },
                        trailingIcon = {
                            Button(onClick = { act.dialTo(e.msisdn) }, contentPadding = PaddingValues(horizontal = 10.dp)) {
                                Text("발신", fontSize = Type.meta)
                            }
                        })
                }
            }
        }
        Spacer(Modifier.height(8.dp))
        listOf("123", "456", "789", "*0#").forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { d ->
                    OutlinedButton(onClick = { act.setDialNumber(number + d) },
                        modifier = Modifier.size(86.dp, 52.dp)) {
                        Text(d.toString(), fontSize = Type.head, fontWeight = FontWeight.Bold)
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(onClick = { act.setDialNumber(number.dropLast(1)) },
                enabled = number.isNotEmpty(), modifier = Modifier.height(48.dp)) { Text("←") }
            Button(onClick = act.dial, enabled = number.isNotBlank(),
                modifier = Modifier.height(48.dp).weight(1f)) {
                Text("발신", fontSize = Type.title, fontWeight = FontWeight.Bold)
            }
        }
        Spacer(Modifier.height(8.dp))
        // 그룹 픽업 — 번호 없이 피처코드만(가장 오래 울린 호를 서버가 고른다)
        OutlinedButton(onClick = { act.pickup("") }, modifier = Modifier.fillMaxWidth()) {
            Text("픽업 (가장 오래 울린 호)")
        }
    }
}

// ── 그룹원 띠 ────────────────────────────────────────────────────────────────
@Composable
private fun MembersStrip(ui: CallsUi, act: CallsActions, onPerson: (PersonAction, String) -> Unit) {
    val members = ui.members
    if (members.isEmpty()) { Hint("전화 그룹원이 없습니다"); return }

    // 열린 메뉴는 **한 번에 하나** — 어느 줄에서 열렸는지를 키로 든다. 줄마다 boolean 을 두면 스크롤로
    //   재사용될 때 엉뚱한 줄에 붙는다.
    var menuFor by remember { mutableStateOf<String?>(null) }

    Column(Modifier.fillMaxWidth().heightIn(max = 180.dp).verticalScroll(rememberScrollState())) {
        members.forEach { m ->
            Row(
                // **탭 = 빠른 발신 입력란에 채움**(데스크톱 §4.3 «대기 → 클릭 → 입력란에 채움»), 롱프레스 =
                //   사람 메뉴. 탭으로 곧바로 걸지 않는 이유는 오조작이다 — 띠는 상태를 보려고 자주 만진다.
                Modifier.fillMaxWidth().padding(vertical = 3.dp)
                    .combinedClickable(
                        onClick = { if (!m.isMe) act.setDialNumber(m.number) },
                        onLongClick = { if (!m.isMe) menuFor = m.number }),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                if (menuFor == m.number) PersonMenu(
                    person = act.personAt(m.number), expanded = true,
                    onDismiss = { menuFor = null }, onPick = onPerson)
                Dot(ringing = m.ringing, talking = m.talking)
                Text(m.number + if (m.isMe) " (나)" else "", fontSize = Type.body,
                     fontWeight = if (m.isMe) FontWeight.Bold else FontWeight.Normal)
                Text(m.name, fontSize = Type.body, modifier = Modifier.weight(1f))
                Text(m.stateText + (if (m.dialog != null) " " + fmt(m.dialog.elapsedMs) else ""), fontSize = Type.meta)
                if (m.canPickup) TextButton(onClick = { act.pickup(m.number) }) { Text("당겨받기", fontSize = Type.meta) }
                if (m.canMonitor) TextButton(onClick = { act.monitorMember(m) }) { Text("청취", fontSize = Type.meta) }
                if (m.monitoring) Text("청취 중", fontSize = Type.meta)
            }
        }
    }
}

// ── 대표번호 대기열 ──────────────────────────────────────────────────────────
@Composable
private fun Queue(ui: CallsUi, act: CallsActions) {
    val queue = ui.queue
    val book = ui.book
    if (queue.isEmpty()) { Hint("대기 중인 대표번호 호 없음"); return }

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        queue.forEach { q ->
            CallCardFrame(attention = q.ringing) {
                Column(Modifier.padding(8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(if (q.ringing) "🔔 " else "", fontSize = Type.strong)
                        Text(book.nameOf(q.caller).ifBlank { q.caller },
                             fontWeight = FontWeight.Bold, fontSize = Type.strong,
                             modifier = Modifier.weight(1f))
                        Text(fmt(q.elapsedMs), fontSize = Type.body)
                        // 포크가 내게도 닿았으면 [응답] — 내 leg 만 받는다. 아니면(다른 그룹원이 울린다) 지정 픽업으로 이 호를.
                        if (q.myLeg != null) Button(onClick = { act.answerQueue(q) }) { Text("응답", fontSize = Type.meta) }
                        if (q.ringing) TextButton(onClick = { act.pickup(q.pilot) }) { Text("당겨받기", fontSize = Type.meta) }
                    }
                    Text(
                        when {
                            q.answered && q.answeredBy.isNotEmpty() -> "응답 ${q.answeredBy}"
                            q.answered -> "응답됨"
                            q.ringingAt.isNotEmpty() -> "울림 " + q.ringingAt.joinToString(", ")
                            else -> "포크 중"
                        },
                        fontSize = Type.meta)
                }
            }
        }
    }
}

// ── 오늘 데스크 ──────────────────────────────────────────────────────────────
@Composable
private fun Tally(ui: CallsUi, act: CallsActions) {
    val t = ui.tally
    val f = ui.deskFilter
    // 칩은 **집계이면서 ⑥ 의 필터**다(데스크톱 `CallDeskPanel.xaml` 과 같은 값). «응대» 는 전체로 되돌리는
    //   자리라 선택 표시를 하지 않는다 — 누르면 필터가 풀린다.
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        listOf(Triple("응대", t.answered, DESK_ALL), Triple("부재", t.missed, "missed"),
               Triple("발신", t.outgoing, "outgoing"), Triple("전달", t.transfer, "transfer"),
               Triple("감청", t.monitor, "monitor")).forEach { (label, n, key) ->
            CimsFilterChip(
                selected = key != DESK_ALL && f == key,
                onClick = { act.setDeskFilter(key) },
                label = { Text("$label $n", fontSize = Type.meta) })
        }
    }
}

// ── 내 통화 ─────────────────────────────────────────────────────────────────
@Composable
private fun MyCalls(ui: CallsUi, act: CallsActions, onPerson: (PersonAction, String) -> Unit) {
    val calls = ui.calls
    val xferTarget = ui.transferTarget
    // 열린 메뉴는 한 번에 하나 — 호 id 를 키로(③ 그룹원·⑥ 내역과 같은 규칙).
    var menuFor by remember { mutableStateOf<Int?>(null) }
    if (calls.isEmpty()) { Hint("진행 중인 통화 없음"); return }

    // 게으른 목록이 아니다 — 면 오른쪽 칸이 통째로 스크롤한다(칸 안에 따로 스크롤하는 목록을 두면 아래 구역이 밀려 사라진다).
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        calls.forEach { c -> key(c.callId) {
            CallCardFrame(attention = c.incoming) {
                Column(Modifier.padding(10.dp)
                    .combinedClickable(onClick = {}, onLongClick = { menuFor = c.callId })) {
                    if (menuFor == c.callId) PersonMenu(
                        person = act.personAt(c.session.info.remoteUri), expanded = true,
                        onDismiss = { menuFor = null }, onPick = onPerson)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Dot(ringing = c.incoming, talking = c.active)
                        Spacer(Modifier.width(6.dp))
                        Text(c.peer, fontWeight = FontWeight.Bold, fontSize = Type.title,
                             modifier = Modifier.weight(1f))
                        if (c.viaPilot) Text("대표 ", fontSize = Type.meta)
                        if (c.consult) Tag("상담")
                        // 음소거는 **상태 배지로도** 보여야 한다 — 버튼 글자만 바뀌면 켜졌는지 모른다.
                        if (c.muted) Text("음소거", fontSize = Type.micro,
                            color = MaterialTheme.colorScheme.error,
                            fontWeight = FontWeight.Bold, modifier = Modifier.padding(end = 4.dp))
                        Text(c.stateText + " " + fmt(c.session.elapsedMs), fontSize = Type.body)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        // 울리는 호는 [응답]·[거절] 둘 — 끊을 통화가 아직 없다(데스크톱 카드와 같다, 거절 = 486).
                        if (c.incoming) {
                            Button(onClick = { act.answer(c) }) { Text("응답") }
                            OutlinedButton(onClick = { act.reject(c) }) {
                                Text("거절", color = MaterialTheme.colorScheme.error)
                            }
                        } else {
                            TextButton(onClick = { act.toggleHold(c) }) { Text(if (c.held) "보류 해제" else "보류", fontSize = Type.meta) }
                            TextButton(onClick = { act.toggleMute(c) }) { Text(if (c.muted) "음소거 해제" else "음소거", fontSize = Type.meta) }
                            TextButton(onClick = { if (c.dtmfOpen) act.closeDtmf() else act.openDtmf(c) }) { Text("DTMF", fontSize = Type.meta) }
                            if (c.canTransfer) TextButton(onClick = { if (c.transferOpen) act.closeTransfer() else act.openTransfer(c) }) { Text("전달", fontSize = Type.meta) }
                            // 상담 호 — 연결되면 원 통화를 넘기고(Replaces), 아니면 끊고 원 통화로 돌아간다.
                            if (c.canComplete) Button(onClick = { act.completeConsult(c) }) { Text("전달 완결", fontSize = Type.meta) }
                            if (c.consult) TextButton(onClick = { act.cancelConsult(c) }) { Text("취소", fontSize = Type.meta) }
                        }
                        Spacer(Modifier.weight(1f))
                        if (!c.incoming) TextButton(onClick = { act.hangup(c) }) { Text("종료", fontSize = Type.meta) }
                    }
                    if (c.transferNote.isNotEmpty()) Text(c.transferNote, fontSize = Type.meta,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (c.dtmfOpen) Dtmf(act, c)
                    if (c.transferOpen) Transfer(act, c, xferTarget, ui.members.filter { !it.isMe })
                }
            }
        } }
    }
}

/** DTMF 3×4 — 통화 카드 안에서 편다(맥락이 보여야 한다). */
@Composable
private fun Dtmf(act: CallsActions, c: CallCard) {
    Column(Modifier.padding(top = 6.dp)) {
        if (c.dtmfSent.isNotEmpty()) Text("보냄: ${c.dtmfSent}", fontSize = Type.meta)
        listOf("123", "456", "789", "*0#").forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                row.forEach { d ->
                    OutlinedButton(
                        onClick = { act.sendDtmf(c, d.toString()) },
                        modifier = Modifier.width(52.dp).height(40.dp),
                        contentPadding = PaddingValues(0.dp)
                    ) { Text(d.toString()) }
                }
            }
        }
    }
}

/**
 * 전달 칸 — 대상(내선·번호) · 그룹원 칩 · [상담 전달] · [전달](데스크톱 전달 팝오버와 같다).
 *
 * [전달] 은 blind(REFER — 받아들여지면 이 leg 은 끝난다), [상담 전달] 은 원 통화를 보류하고 대상에게 먼저 건다 — 상대가
 * 받으면 상담 호 카드의 [전달 완결] 로 넘긴다. 그룹원 칩은 대상 칸을 채울 뿐이다(누가 받을지 고르는 것은 관제사).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Transfer(act: CallsActions, c: CallCard, target: String, members: List<MemberChip>) {
    Column(Modifier.padding(top = 6.dp)) {
        OutlinedTextField(
            value = target, onValueChange = act.setTransferTarget,
            placeholder = { Text("전달 대상 — 내선·번호 · 그룹원", fontSize = Type.body) },
            singleLine = true, modifier = Modifier.fillMaxWidth(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Phone, imeAction = ImeAction.Go))
        if (members.isNotEmpty()) FlowRow(Modifier.padding(top = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            members.forEach { m ->
                AssistChip(onClick = { act.pickTransferTarget(m.number) },
                    label = { Text(m.name.ifBlank { m.number }, fontSize = Type.meta) })
            }
        }
        Row(Modifier.fillMaxWidth().padding(top = 4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.End)) {
            OutlinedButton(onClick = { act.consult(c) }, enabled = target.isNotBlank()) { Text("상담 전달") }
            Button(onClick = { act.transfer(c) }, enabled = target.isNotBlank()) { Text("전달") }
        }
    }
}

// ── ⑥ 통화 내역 ─────────────────────────────────────────────────────────────
//
// 콘솔·[이력] 화면과 같은 축을 든다 — **시작 · 응답 · 종료 · 통화시간**(§4.6). 한 줄만 보고
// «언제 걸려 와서 얼마나 울렸고 몇 분 통화했는지» 를 알 수 있어야 한다.
/**
 * 진행 중 — 감시 대상 전원의 살아 있는 통화(§4.4). **감청의 주 진입점**이다.
 *
 * «통화» 면에 두는 이유: 지금 벌어지는 일이다. 감청을 켜면 그 행이 «청취 중» 이라고 말하고 거기서 끈다 —
 * 감청을 위한 구역을 따로 두지 않는 이유가 이것이다(§6.3).
 */
@Composable
private fun LiveCalls(ui: CallsUi, act: CallsActions) {
    val live = ui.live
    if (live.isNotEmpty()) {
        live.forEach { r -> LiveRow(act, r, ui.listenHidden, ui.rxLevels) }
        return
    }
    // 비었으면 **왜** 비었는지 쓴다 — 조용히 비면 «앱 고장» 과 «편성 미비» 를 못 가른다.
    var diag by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(ui.liveHint, Modifier.weight(1f), fontSize = Type.meta,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        TextButton(onClick = { diag = !diag },
            contentPadding = PaddingValues(horizontal = 6.dp)) {
            Text(if (diag) "진단 닫기" else "진단", fontSize = Type.meta)
        }
    }
    if (diag) WatchDiag(ui)
}

@Composable
private fun CallLog(ui: CallsUi, act: CallsActions, onPerson: (PersonAction, String) -> Unit) {
    val log = ui.log
    val f = ui.deskFilter


    // ⑥ 머리 필터 — 데스크톱 `[전체|대표번호|부재]`(§4.4). 오늘 데스크 칩과 **같은 상태**를 쓴다(둘로
    //   나누면 «칩으로 건 필터» 와 «머리로 건 필터» 가 서로를 덮는다).
    Row(Modifier.fillMaxWidth().padding(bottom = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically) {
        listOf("전체" to DESK_ALL, "대표번호" to "pilot", "부재" to "missed").forEach { (label, key) ->
            CimsFilterChip(selected = if (key == DESK_ALL) f == DESK_ALL else f == key,
                onClick = { act.setDeskFilter(key) },
                label = { Text(label, fontSize = Type.meta) })
        }
        // 사람 축은 종류와 **직교**하므로 칩을 따로 세우고 ✕ 로만 푼다 — 종류 칩 사이에 끼우면
        //   «전체» 를 눌렀을 때 사람 필터까지 풀린 줄 알게 된다.
        if (ui.personFilter.isNotEmpty()) InputChip(
            selected = true, onClick = act.clearPersonFilter,
            label = { Text("${ui.personFilter} 기록", fontSize = Type.meta) },
            trailingIcon = { Text("✕", fontSize = Type.meta) })
        Spacer(Modifier.weight(1f))
        // 끝난 통화의 날짜별 조회·녹취는 [이력] 이다 — 여기는 오늘 데스크의 작업 메모리(데스크톱 ⑥ 머리와 같다).
        TextButton(onClick = act.openHistory) { Text("이력에서 보기", fontSize = Type.meta) }
        val export = com.cims.ue.dispatch.ui.rememberCsvExport()
        TextButton(onClick = {
            export("call-activity-" + SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT).format(Date()) + ".csv", act.logCsv)
        }) { Text("CSV", fontSize = Type.meta) }
    }

    if (log.isEmpty()) return

    Row(Modifier.fillMaxWidth().padding(bottom = 2.dp)) {
        listOf("시작" to 64, "상대" to 170, "종류" to 92, "응답" to 64, "종료" to 64,
               "통화" to 60, "울림" to 56).forEach { (t, w) ->
            Text(t, Modifier.width(w.dp), fontSize = Type.micro,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    HorizontalDivider()
    // 열린 메뉴는 한 번에 하나 — 행 키로 든다(③ 그룹원 띠와 같은 규칙).
    var menuFor by remember { mutableStateOf<String?>(null) }
    LazyColumn(verticalArrangement = Arrangement.spacedBy(1.dp)) {
        items(log, key = { it.atMs.toString() + it.number + it.peer }) { r ->
            val key = r.atMs.toString() + r.number
            CallLogRowView(r, onLongPress = { if (r.number.isNotBlank()) menuFor = key },
                onRedial = { onPerson(PersonAction.CALL, r.number) },
                onSms = { onPerson(PersonAction.SMS, r.number) })
            if (menuFor == key) PersonMenu(
                person = act.personAt(r.number), expanded = true,
                onDismiss = { menuFor = null }, onPick = onPerson)
        }
    }
}

@Composable
private fun CallLogRowView(r: CallLogRow, onLongPress: () -> Unit = {},
                            onRedial: () -> Unit = {}, onSms: () -> Unit = {}) {
    val missed = r.kind == CallLogKind.MISSED
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp)
            .combinedClickable(onClick = {}, onLongClick = onLongPress),
        verticalAlignment = Alignment.CenterVertically) {
        Text(hhmmss.format(Date(r.startedAtMs)), Modifier.width(64.dp), fontSize = Type.meta)
        // 이름과 번호를 같이 — 이름만 두면 누군지는 알아도 다시 걸 수가 없다.
        Column(Modifier.width(170.dp)) {
            Text(r.peer, fontSize = Type.meta, fontWeight = FontWeight.Bold, maxLines = 1)
            if (r.number.isNotBlank() && r.number != r.peer)
                Text(r.number, fontSize = Type.micro, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1)
        }
        Text(kindText(r.kind), Modifier.width(92.dp), fontSize = Type.meta,
            color = if (missed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
        Text(r.answeredAtMs?.let { hhmmss.format(Date(it)) } ?: "—", Modifier.width(64.dp), fontSize = Type.meta)
        Text(hhmmss.format(Date(r.endedAtMs)), Modifier.width(64.dp), fontSize = Type.meta)
        // 통화시간은 응답~종료다. 못 받은 호는 0 이라 «울림» 쪽만 값이 있다.
        Text(if (r.answered) durText(r.durationSec) else "—", Modifier.width(60.dp), fontSize = Type.meta,
            fontWeight = if (r.answered) FontWeight.Bold else FontWeight.Normal)
        Text(durText(r.ringSec), Modifier.width(56.dp), fontSize = Type.meta,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(r.text, Modifier.weight(1f), fontSize = Type.micro,
            color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        if (r.others) Text("감시", fontSize = Type.micro, color = MaterialTheme.colorScheme.onSurfaceVariant)
        // 행에서 곧바로 — 사람 메뉴(롱프레스)를 거치지 않는다(데스크톱 ⑥ 행의 [재발신]·[문자]).
        if (r.canRedial) {
            TextButton(onClick = onRedial, contentPadding = PaddingValues(horizontal = 6.dp)) {
                Text("재발신", fontSize = Type.meta)
            }
            TextButton(onClick = onSms, contentPadding = PaddingValues(horizontal = 6.dp)) {
                Text("문자", fontSize = Type.meta)
            }
        }
    }
}

/**
 * 감시 대상 진단 — 어느 번호의 구독이 성립했는지.
 *
 * «구독을 걸었다» 와 «구독이 성립했다» 는 다르다. `dialogWatch` 는 SUBSCRIBE 를 **보낸 것**만
 * 성공으로 돌려주고 최종 응답은 따로 온다(코어 `Engine::dialogWatch`). 성립 판정은 **NOTIFY 수신**이다
 * (RFC 6665 — 구독이 서면 즉시 NOTIFY 가 온다). 안 잡힌 번호가 위로 온다.
 */
@Composable
private fun WatchDiag(ui: CallsUi) {
    val rows = ui.watchDiag
    if (rows.isEmpty()) { Hint("구독한 감시 대상이 없습니다"); return }
    val ok = rows.count { it.established }
    Column(Modifier.fillMaxWidth().heightIn(max = 220.dp).verticalScroll(rememberScrollState())) {
        Text("구독 성립 $ok / ${rows.size} · 통화 관측 ${rows.count { it.sawDialog }}",
            fontSize = Type.meta, fontWeight = FontWeight.Bold)
        Text("✓ = 구독 성립(NOTIFY 수신) · ● = 그 회선의 통화를 관측함", fontSize = Type.micro,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        rows.forEach { r ->
            Row(Modifier.fillMaxWidth().padding(vertical = 1.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text(if (r.established) "✓" else "✗", Modifier.width(18.dp), fontSize = Type.meta,
                    color = if (r.established) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.error)
                Text(if (r.sawDialog) "●" else "·", Modifier.width(14.dp), fontSize = Type.meta,
                    color = if (r.sawDialog) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant)
                Text(r.number, Modifier.width(140.dp), fontSize = Type.meta)
                Text(r.name, Modifier.weight(1f), fontSize = Type.meta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                if (r.pilot) Text("대표", fontSize = Type.micro, color = MaterialTheme.colorScheme.tertiary)
            }
        }
    }
}

/**
 * ⑥ 진행 중 행 하나 — **감청의 유일한 자리**다.
 *
 * 내 통화도 행으로 보이되 조작은 없다 — 내 전화는 배너·내 통화 카드에서 다룬다. 타인 통화만
 * [지정 픽업]·[청취] 를 든다. 최종 인가는 서버가 한다(범위 밖이면 403).
 *
 * 청취를 켜면 **그 행이 펴져서** 소스 귀속(RFC 5576 `a=ssrc … label` 로 갈라 온 caller/callee 두 줄)과
 * 은닉 여부·라우트를 보인다. 별도의 «감청» 면을 두지 않는 이유는 같은 통화가 두 군데 나오면 어느 쪽이
 * 최신인지 흐려지기 때문이다 — 켜는 자리·보는 자리·끄는 자리가 하나다(§6.5).
 */
@Composable
private fun LiveRow(act: CallsActions, r: LiveCallRow, hidden: Boolean = true, levels: Map<Int, Float> = emptyMap()) {
    CallCardFrame(attention = r.ringing, modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Row(Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Dot(ringing = r.ringing, talking = r.talking)
            Text("${r.aLabel} ↔ ${r.bLabel}", fontSize = Type.body, fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f), maxLines = 1)
            if (r.viaPilot) Text("대표", fontSize = Type.micro, color = MaterialTheme.colorScheme.tertiary)
            if (r.mine) Text("내 통화", fontSize = Type.micro)
            Text("${r.stateText} ${fmt(r.elapsedMs)}", fontSize = Type.meta)
            if (r.canPickup) TextButton(onClick = { act.pickup(r.pickupNumber) },
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("지정 픽업", fontSize = Type.meta) }
            if (r.monitoring) {
                Text("청취 중", fontSize = Type.meta, color = MaterialTheme.colorScheme.primary)
                TextButton(onClick = { act.stopMonitorLive(r) },
                    contentPadding = PaddingValues(horizontal = 8.dp)) {
                    Text("청취 종료", fontSize = Type.meta, color = MaterialTheme.colorScheme.error)
                }
            } else if (r.canMonitor) TextButton(onClick = { act.monitorLive(r) },
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("청취", fontSize = Type.meta) }
        }
        r.tap?.let { TapDetail(it, hidden, levels[it.callId] ?: 1f) { v -> act.setRxLevel(it.callId, v) } }
    }
}

/**
 * 감청 leg 의 상세 — 청취 중인 행 안에서만 펴진다.
 *
 * CMP 가 양 peer 를 **SSRC 2개로 분리 인도**하므로(RFC 3911 Join `a=recvonly` → `RELAY_TAP_*`,
 * dispatch_center.md §5.4) 줄이 둘이다. 믹싱은 단말이 한다 — 서버가 섞어 주면 «누가 말했는지» 가
 * 사라져 귀속이 깨진다. 소스가 갈라져 오지 않는 서버에서는 한 줄로 그 사실을 적는다(지어내지 않는다).
 */
@Composable
private fun TapDetail(tap: SessionItem, hidden: Boolean, level: Float = 1f, onLevel: (Float) -> Unit = {}) {
    Column(Modifier.fillMaxWidth()
        .padding(start = 22.dp, end = 8.dp, bottom = 6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Tag(if (hidden) "은닉" else "투명",
                if (hidden) MaterialTheme.colorScheme.onSurfaceVariant
                else MaterialTheme.colorScheme.tertiary, leading = 0)
            Text("🎧 ${tap.info.playbackRoute}", fontSize = Type.micro,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        val sources = tap.info.sources
        if (sources.isEmpty()) Text("수신 중 — 소스 라벨이 아직 없습니다", fontSize = Type.meta,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        else sources.forEach { src -> SourceRow(src) }
        RxLevelRow(level, onLevel)
    }
}

/**
 * 이 leg 의 수신 음량 — 0~2(1.0 = 원음, 데스크톱 감청 창의 [음량] 과 같은 범위). 여러 leg 을 같이 들을 때 하나만 줄이거나
 * 키운다. 막대는 끄는 대로 반영하고 값은 호가 끝날 때까지 세션이 든다(`rxLevels`).
 */
@Composable
internal fun RxLevelRow(level: Float, onLevel: (Float) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text("음량", fontSize = Type.meta, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(40.dp))
        Slider(value = level, onValueChange = onLevel, valueRange = 0f..2f, modifier = Modifier.weight(1f))
        Text("${(level * 100).toInt()}%", fontSize = Type.meta, modifier = Modifier.width(48.dp),
            textAlign = TextAlign.End)
    }
}

/**
 * 감청 소스 한 줄 — 이름 · 활성 점 · 레벨 미터.
 *
 * `level` 은 아직 실시간 값이 없다(pjproject 에 SSRC 별 수신 관측 API 가 없어 SDP 라벨만 온다, §11).
 * 그때까지는 `active` 로만 켜고, API 가 생기면 이 줄의 폭만 바뀐다.
 */
@Composable
private fun SourceRow(src: MediaSource) {
    Row(Modifier.fillMaxWidth().padding(vertical = 1.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(7.dp).clip(RoundedCornerShape(4.dp)).background(
            if (src.active) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.surfaceVariant))
        Spacer(Modifier.width(6.dp))
        Text(src.label.ifBlank { "ssrc ${src.ssrc}" }, Modifier.width(110.dp),
            fontSize = Type.meta, maxLines = 1)
        Box(Modifier.weight(1f).height(5.dp).clip(RoundedCornerShape(3.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)) {
            val w = if (src.level > 0f) src.level.coerceIn(0f, 1f) else if (src.active) 0.35f else 0f
            if (w > 0f) Box(Modifier.fillMaxWidth(w).fillMaxHeight()
                .background(MaterialTheme.colorScheme.primary))
        }
    }
}

/**
 * [통화] 하위 탭 — 이 장만 따로 볼 때(`showTabs = true`, 미리보기)의 탭 줄. 앱에서는 껍데기의 `DispatchTabs` 가
 * [무전|통화] 와 하위 탭을 한 줄로 그리고, 면을 밀어도 그 줄은 제자리에 남는다(§6.3).
 */
@Composable
fun CallTabRow(pane: CallPane, onPane: (CallPane) -> Unit) {
    TabRow(selectedTabIndex = pane.ordinal) {
        CallPane.entries.forEach { p ->
            Tab(selected = pane == p, onClick = { onPane(p) },
                text = { Text(p.label, fontSize = Type.body) })
        }
    }
}

/** 종류 문구 — 데스크톱 ⑥ 최근 행의 어휘(§4.4). */
private fun kindText(k: CallLogKind): String = com.cims.ue.dispatch.session.callLogKindText(k)

/** `m:ss` — 0 이면 대시. */
internal fun durText(sec: Int): String =
    if (sec <= 0) "—" else "%d:%02d".format(sec / 60, sec % 60)

// ── 공용 ────────────────────────────────────────────────────────────────────
@Composable
private fun Dot(ringing: Boolean, talking: Boolean) {
    val c = when {
        ringing -> MaterialTheme.colorScheme.tertiary
        talking -> MaterialTheme.colorScheme.primary
        else -> MaterialTheme.colorScheme.outline
    }
    Box(Modifier.size(9.dp).clip(RoundedCornerShape(5.dp)).background(c))
}

@Composable
private fun Hint(t: String) =
    Text(t, fontSize = Type.body, color = MaterialTheme.colorScheme.outline,
         modifier = Modifier.padding(vertical = 6.dp))
