// [일반통화] 탭 화면 — ③ 일반통화 + ⑥ 통화 내역 (android_dispatch_tablet.md §6.3)
//
// 탭 폭이 전부(1280)라 ③ 를 **2열**로 편다 — 좌: 그룹원 띠·대표번호 대기열 / 우: 오늘 데스크·내 통화.
// 데스크톱은 1열이지만 태블릿은 세로 스크롤을 줄이는 쪽이 낫다.
// DTMF·전달은 한 통화에 묶인 조작이라 **카드 안에서** 편다(§6.6).
package com.cims.ue.dispatch.ui.call

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

private val hhmmss = SimpleDateFormat("HH:mm:ss", Locale.KOREA)

private fun fmt(ms: Long): String {
    val t = ms / 1000
    return if (t >= 3600) "%d:%02d:%02d".format(t / 3600, (t % 3600) / 60, t % 60)
    else "%02d:%02d".format(t / 60, t % 60)
}

/** [일반통화] 탭 — 위 ③(2열) / 아래 ⑥. */
@Composable
fun CallTab(vm: CallDeskViewModel, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxSize()) {
        Row(Modifier.weight(0.6f).fillMaxWidth()) {
            Column(Modifier.weight(1f).fillMaxHeight().padding(8.dp)) {
                QuickDial(vm)
                Spacer(Modifier.height(8.dp))
                SectionTitle("관제 그룹원")
                Members(vm)
                Spacer(Modifier.height(8.dp))
                SectionTitle("대표번호 대기열")
                Queue(vm)
            }
            VerticalDivider()
            Column(Modifier.weight(1f).fillMaxHeight().padding(8.dp)) {
                SectionTitle("오늘 데스크")
                Tally(vm)
                Spacer(Modifier.height(8.dp))
                SectionTitle("내 통화")
                MyCalls(vm)
            }
        }
        HorizontalDivider()
        Column(Modifier.weight(0.4f).fillMaxWidth().padding(8.dp)) {
            SectionTitle("⑥ 통화 내역")
            CallLog(vm)
        }
    }
}

@Composable
private fun SectionTitle(t: String) =
    Text(t, fontWeight = FontWeight.Bold, fontSize = 13.sp, modifier = Modifier.padding(bottom = 4.dp))

// ── 빠른 발신 ────────────────────────────────────────────────────────────────
@Composable
private fun QuickDial(vm: CallDeskViewModel) {
    val number by vm.dialNumber.collectAsStateWithLifecycle()
    // 주소록은 **관측해서** 읽는다 — 비관측 읽기는 늦게 도착한 주소록을 화면에 못 싣는다.
    val book by vm.book.collectAsStateWithLifecycle()
    var sheet by remember { mutableStateOf(false) }

    Row(verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        OutlinedTextField(
            value = number, onValueChange = vm::setDialNumber,
            placeholder = { Text("번호·내선", fontSize = 12.sp) },
            singleLine = true, modifier = Modifier.weight(1f),
            // 입력한 번호의 주인을 바로 보여 준다 — 잘못 건 전화를 줄인다.
            supportingText = {
                val who = if (number.isBlank()) "" else book.nameOf(number)
                if (who.isNotBlank()) Text(who, fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.primary)
            },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Phone, imeAction = ImeAction.Go))
        // 다이얼패드·주소록·최근 — 셋 다 «번호를 골라 건다» 라 시트 하나에 탭으로 둔다(§6.6)
        OutlinedButton(onClick = { sheet = true }) { Text("주소록") }
        Button(onClick = vm::dial, enabled = number.isNotBlank()) { Text("발신") }
        // 그룹 픽업 — 번호 없이 피처코드만(가장 오래 울린 호를 서버가 고른다)
        OutlinedButton(onClick = { vm.pickup() }) { Text("픽업") }
    }

    if (sheet) DialSheet(vm) { sheet = false }
}

// ── 그룹원 띠 ────────────────────────────────────────────────────────────────
@Composable
private fun Members(vm: CallDeskViewModel) {
    val members by vm.members.collectAsStateWithLifecycle()
    if (members.isEmpty()) { Hint("전화 그룹원이 없습니다"); return }

    Column(Modifier.verticalScroll(rememberScrollState()).heightIn(max = 150.dp)) {
        members.forEach { m ->
            Row(
                Modifier.fillMaxWidth().padding(vertical = 3.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                Dot(ringing = m.ringing, talking = m.talking)
                Text(m.number + if (m.isMe) " (나)" else "", fontSize = 12.sp,
                     fontWeight = if (m.isMe) FontWeight.Bold else FontWeight.Normal)
                Text(m.name, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(m.stateText + (if (m.dialog != null) " " + fmt(m.dialog.elapsedMs) else ""), fontSize = 11.sp)
                if (m.canPickup) TextButton(onClick = { vm.pickup(m.number) }) { Text("당겨받기", fontSize = 11.sp) }
                if (m.canMonitor) TextButton(onClick = { vm.monitor(m) }) { Text("청취", fontSize = 11.sp) }
                if (m.monitoring) Text("청취 중", fontSize = 11.sp)
            }
        }
    }
}

// ── 대표번호 대기열 ──────────────────────────────────────────────────────────
@Composable
private fun Queue(vm: CallDeskViewModel) {
    val queue by vm.queue.collectAsStateWithLifecycle()
    val book by vm.book.collectAsStateWithLifecycle()
    if (queue.isEmpty()) { Hint("대기 중인 대표번호 호 없음"); return }

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        queue.forEach { q ->
            Card(colors = CardDefaults.cardColors(
                containerColor = if (q.ringing) MaterialTheme.colorScheme.tertiaryContainer
                                 else MaterialTheme.colorScheme.surfaceVariant)) {
                Column(Modifier.padding(8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(if (q.ringing) "🔔 " else "", fontSize = 13.sp)
                        Text(book.nameOf(q.caller).ifBlank { q.caller },
                             fontWeight = FontWeight.Bold, fontSize = 13.sp,
                             modifier = Modifier.weight(1f))
                        Text(fmt(q.elapsedMs), fontSize = 12.sp)
                        if (q.ringing) TextButton(onClick = { vm.pickup() }) { Text("당겨받기", fontSize = 11.sp) }
                    }
                    Text(
                        when {
                            q.answered && q.answeredBy.isNotEmpty() -> "응답 ${q.answeredBy}"
                            q.answered -> "응답됨"
                            q.ringingAt.isNotEmpty() -> "울림 " + q.ringingAt.joinToString(", ")
                            else -> "포크 중"
                        },
                        fontSize = 11.sp)
                }
            }
        }
    }
}

// ── 오늘 데스크 ──────────────────────────────────────────────────────────────
@Composable
private fun Tally(vm: CallDeskViewModel) {
    val t by vm.tally.collectAsStateWithLifecycle()
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        listOf("응대" to t.answered, "부재" to t.missed, "발신" to t.outgoing,
               "전달" to t.transfer, "감청" to t.monitor).forEach { (label, n) ->
            AssistChip(onClick = {}, label = { Text("$label $n", fontSize = 11.sp) })
        }
    }
}

// ── 내 통화 ─────────────────────────────────────────────────────────────────
@Composable
private fun MyCalls(vm: CallDeskViewModel) {
    val calls by vm.calls.collectAsStateWithLifecycle()
    val xferTarget by vm.transferTarget.collectAsStateWithLifecycle()
    if (calls.isEmpty()) { Hint("진행 중인 통화 없음"); return }

    LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        items(calls, key = { it.callId }) { c ->
            Card(colors = CardDefaults.cardColors(
                containerColor = if (c.incoming) MaterialTheme.colorScheme.tertiaryContainer
                                 else MaterialTheme.colorScheme.surfaceVariant)) {
                Column(Modifier.padding(10.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Dot(ringing = c.incoming, talking = c.active)
                        Spacer(Modifier.width(6.dp))
                        Text(c.peer, fontWeight = FontWeight.Bold, fontSize = 14.sp,
                             modifier = Modifier.weight(1f))
                        if (c.viaPilot) Text("대표 ", fontSize = 11.sp)
                        // 음소거는 **상태 배지로도** 보여야 한다 — 버튼 글자만 바뀌면 켜졌는지 모른다.
                        if (c.muted) Text("음소거", fontSize = 10.sp,
                            color = MaterialTheme.colorScheme.error,
                            fontWeight = FontWeight.Bold, modifier = Modifier.padding(end = 4.dp))
                        Text(c.stateText + " " + fmt(c.session.elapsedMs), fontSize = 12.sp)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        if (c.incoming) {
                            Button(onClick = { vm.answer(c) }) { Text("응답") }
                        } else {
                            TextButton(onClick = { vm.toggleHold(c) }) { Text(if (c.held) "보류 해제" else "보류", fontSize = 11.sp) }
                            TextButton(onClick = { vm.toggleMute(c) }) { Text(if (c.muted) "음소거 해제" else "음소거", fontSize = 11.sp) }
                            TextButton(onClick = { if (c.dtmfOpen) vm.closeDtmf() else vm.openDtmf(c) }) { Text("DTMF", fontSize = 11.sp) }
                            TextButton(onClick = { if (c.transferOpen) vm.closeTransfer() else vm.openTransfer(c) }) { Text("전달", fontSize = 11.sp) }
                        }
                        Spacer(Modifier.weight(1f))
                        TextButton(onClick = { vm.hangup(c) }) { Text("종료", fontSize = 11.sp) }
                    }
                    if (c.dtmfOpen) Dtmf(vm, c)
                    if (c.transferOpen) Transfer(vm, c, xferTarget)
                }
            }
        }
    }
}

/** DTMF 3×4 — 통화 카드 안에서 편다(맥락이 보여야 한다). */
@Composable
private fun Dtmf(vm: CallDeskViewModel, c: CallCard) {
    Column(Modifier.padding(top = 6.dp)) {
        if (c.dtmfSent.isNotEmpty()) Text("보냄: ${c.dtmfSent}", fontSize = 11.sp)
        listOf("123", "456", "789", "*0#").forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                row.forEach { d ->
                    OutlinedButton(
                        onClick = { vm.sendDtmf(c, d.toString()) },
                        modifier = Modifier.width(52.dp).height(40.dp),
                        contentPadding = PaddingValues(0.dp)
                    ) { Text(d.toString()) }
                }
            }
        }
    }
}

/** 호 전달 blind — REFER(RFC 3515). attended 는 상담 호가 필요해 후속(§11). */
@Composable
private fun Transfer(vm: CallDeskViewModel, c: CallCard, target: String) {
    Row(Modifier.padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        OutlinedTextField(
            value = target, onValueChange = vm::setTransferTarget,
            placeholder = { Text("전달 대상", fontSize = 12.sp) },
            singleLine = true, modifier = Modifier.weight(1f),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Phone, imeAction = ImeAction.Go))
        Button(onClick = { vm.transfer(c) }, enabled = target.isNotBlank()) { Text("전달") }
    }
}

// ── ⑥ 통화 내역 ─────────────────────────────────────────────────────────────
//
// 콘솔·[이력] 화면과 같은 축을 든다 — **시작 · 응답 · 종료 · 통화시간**(§4.6). 한 줄만 보고
// «언제 걸려 와서 얼마나 울렸고 몇 분 통화했는지» 를 알 수 있어야 한다.
@Composable
private fun CallLog(vm: CallDeskViewModel) {
    val log by vm.callLog.collectAsStateWithLifecycle()
    val live by vm.live.collectAsStateWithLifecycle()

    // ── 진행 중 행 — 감시 대상 전원의 살아 있는 통화(§4.4). 여기가 감청의 두 번째 진입점이다.
    if (live.isNotEmpty()) {
        Text("진행 중 ${live.size}", fontSize = 11.sp, fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary)
        live.forEach { r -> LiveRow(vm, r) }
        Spacer(Modifier.height(6.dp))
    } else {
        // 비었으면 **왜** 비었는지 쓴다 — 조용히 비면 «앱 고장» 과 «편성 미비» 를 못 가른다.
        val hint by vm.liveHint.collectAsStateWithLifecycle()
        var diag by remember { mutableStateOf(false) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(hint, Modifier.weight(1f), fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            TextButton(onClick = { diag = !diag },
                contentPadding = PaddingValues(horizontal = 6.dp)) {
                Text(if (diag) "진단 닫기" else "진단", fontSize = 11.sp)
            }
        }
        if (diag) WatchDiag(vm)
        Spacer(Modifier.height(4.dp))
    }

    if (log.isEmpty()) return

    Row(Modifier.fillMaxWidth().padding(bottom = 2.dp)) {
        listOf("시작" to 64, "상대" to 170, "종류" to 92, "응답" to 64, "종료" to 64,
               "통화" to 60, "울림" to 56).forEach { (t, w) ->
            Text(t, Modifier.width(w.dp), fontSize = 10.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    HorizontalDivider()
    LazyColumn(verticalArrangement = Arrangement.spacedBy(1.dp)) {
        items(log, key = { it.atMs.toString() + it.number + it.peer }) { r -> CallLogRowView(r) }
    }
}

@Composable
private fun CallLogRowView(r: CallLogRow) {
    val missed = r.kind == CallLogKind.MISSED
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Text(hhmmss.format(Date(r.startedAtMs)), Modifier.width(64.dp), fontSize = 11.sp)
        // 이름과 번호를 같이 — 이름만 두면 누군지는 알아도 다시 걸 수가 없다.
        Column(Modifier.width(170.dp)) {
            Text(r.peer, fontSize = 11.sp, fontWeight = FontWeight.Bold, maxLines = 1)
            if (r.number.isNotBlank() && r.number != r.peer)
                Text(r.number, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1)
        }
        Text(kindText(r.kind), Modifier.width(92.dp), fontSize = 11.sp,
            color = if (missed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
        Text(r.answeredAtMs?.let { hhmmss.format(Date(it)) } ?: "—", Modifier.width(64.dp), fontSize = 11.sp)
        Text(hhmmss.format(Date(r.endedAtMs)), Modifier.width(64.dp), fontSize = 11.sp)
        // 통화시간은 응답~종료다. 못 받은 호는 0 이라 «울림» 쪽만 값이 있다.
        Text(if (r.answered) durText(r.durationSec) else "—", Modifier.width(60.dp), fontSize = 11.sp,
            fontWeight = if (r.answered) FontWeight.Bold else FontWeight.Normal)
        Text(durText(r.ringSec), Modifier.width(56.dp), fontSize = 11.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(r.text, Modifier.weight(1f), fontSize = 10.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        if (r.others) Text("감시", fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
private fun WatchDiag(vm: CallDeskViewModel) {
    val rows by vm.watchDiag.collectAsStateWithLifecycle()
    if (rows.isEmpty()) { Hint("구독한 감시 대상이 없습니다"); return }
    val ok = rows.count { it.established }
    Column(Modifier.fillMaxWidth().heightIn(max = 220.dp).verticalScroll(rememberScrollState())) {
        Text("구독 성립 $ok / ${rows.size} · 통화 관측 ${rows.count { it.sawDialog }}",
            fontSize = 11.sp, fontWeight = FontWeight.Bold)
        Text("✓ = 구독 성립(NOTIFY 수신) · ● = 그 회선의 통화를 관측함", fontSize = 10.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        rows.forEach { r ->
            Row(Modifier.fillMaxWidth().padding(vertical = 1.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text(if (r.established) "✓" else "✗", Modifier.width(18.dp), fontSize = 11.sp,
                    color = if (r.established) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.error)
                Text(if (r.sawDialog) "●" else "·", Modifier.width(14.dp), fontSize = 11.sp,
                    color = if (r.sawDialog) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant)
                Text(r.number, Modifier.width(140.dp), fontSize = 11.sp)
                Text(r.name, Modifier.weight(1f), fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                if (r.pilot) Text("대표", fontSize = 10.sp, color = MaterialTheme.colorScheme.tertiary)
            }
        }
    }
}

/**
 * ⑥ 진행 중 행 하나.
 *
 * 내 통화도 행으로 보이되 조작은 없다 — 내 전화는 배너·내 통화 카드에서 다룬다. 타인 통화만
 * [지정 픽업]·[청취] 를 든다. 최종 인가는 서버가 한다(범위 밖이면 403).
 */
@Composable
private fun LiveRow(vm: CallDeskViewModel, r: LiveCallRow) {
    Card(colors = CardDefaults.cardColors(
        containerColor = if (r.ringing) MaterialTheme.colorScheme.tertiaryContainer
                         else MaterialTheme.colorScheme.surfaceVariant),
        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Row(Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Dot(ringing = r.ringing, talking = r.talking)
            Text("${r.aLabel} ↔ ${r.bLabel}", fontSize = 12.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f), maxLines = 1)
            if (r.viaPilot) Text("대표", fontSize = 10.sp, color = MaterialTheme.colorScheme.tertiary)
            if (r.mine) Text("내 통화", fontSize = 10.sp)
            Text("${r.stateText} ${fmt(r.elapsedMs)}", fontSize = 11.sp)
            if (r.canPickup) TextButton(onClick = { vm.pickup(r.pickupNumber) },
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("지정 픽업", fontSize = 11.sp) }
            if (r.monitoring) {
                Text("청취 중", fontSize = 11.sp, color = MaterialTheme.colorScheme.primary)
                TextButton(onClick = { vm.stopMonitorLive(r) },
                    contentPadding = PaddingValues(horizontal = 8.dp)) {
                    Text("청취 종료", fontSize = 11.sp, color = MaterialTheme.colorScheme.error)
                }
            } else if (r.canMonitor) TextButton(onClick = { vm.monitorLive(r) },
                contentPadding = PaddingValues(horizontal = 8.dp)) { Text("청취", fontSize = 11.sp) }
        }
    }
}

/** 종류 문구 — 데스크톱 ⑥ 최근 행의 어휘(§4.4). */
private fun kindText(k: CallLogKind): String = when (k) {
    CallLogKind.ANSWERED -> "착신 응답"
    CallLogKind.MISSED -> "부재"
    CallLogKind.OUTGOING -> "발신"
    CallLogKind.PICKUP -> "당겨받기"
    CallLogKind.TRANSFER -> "전달"
    CallLogKind.MONITOR -> "감청"
}

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
    Text(t, fontSize = 12.sp, color = MaterialTheme.colorScheme.outline,
         modifier = Modifier.padding(vertical = 6.dp))
