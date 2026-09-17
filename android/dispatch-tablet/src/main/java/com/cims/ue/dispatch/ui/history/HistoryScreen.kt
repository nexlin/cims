// [이력] 화면 (docs/design/features/android_dispatch_tablet.md §6.5, dispatch_desktop_ui.md §4.6)
//
// 데스크톱과 같은 구성이되 태블릿 밀도로 접는다(§12) — 별창이 없으므로 PTT 는 한 화면에서
// 좌(세션 카드 1) : 우(세션 패널 3) 로 나눈다. 통화는 상세가 따로 없어 표가 전체 폭이다.
package com.cims.ue.dispatch.ui.history

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.PttFloorEvent
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.TurnBar
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private val HHMMSS: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")

internal fun hhmmss(ms: Long?): String =
    ms?.let { Instant.ofEpochMilli(it).atZone(ZoneId.systemDefault()).toLocalTime().format(HHMMSS) } ?: "—"

internal fun durText(sec: Int): String =
    if (sec <= 0) "—" else "%d:%02d".format(sec / 60, sec % 60)

@Composable
fun HistoryScreen(vm: HistoryViewModel, modifier: Modifier = Modifier) {
    if (!vm.available) return Locked("관제 역할 미배정 — 이력을 볼 수 없습니다")

    val kind by vm.kind.collectAsStateWithLifecycle()
    val rows by vm.rows.collectAsStateWithLifecycle()
    val loading by vm.loading.collectAsStateWithLifecycle()
    val error by vm.error.collectAsStateWithLifecycle()

    // 화면에 처음 들어올 때 한 번 조회하고, 떠날 때 재생을 멈춘다(§4.6).
    LaunchedEffect(Unit) { if (rows.isEmpty()) vm.load() }
    DisposableEffect(Unit) { onDispose { vm.onLeave() } }

    Column(modifier.fillMaxSize()) {
        Toolbar(vm)
        HourBand(vm)
        if (error.isNotBlank()) {
            Text(error, Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
        }
        // 서버 상한에 걸려 잘렸으면 말한다 — 조용히 일부만 보여 주면 «없는 통화» 로 읽힌다.
        val truncated by vm.truncated.collectAsStateWithLifecycle()
        val hour by vm.hourFilter.collectAsStateWithLifecycle()
        if (truncated) Text(
            if (hour == null) "서버 상한 ${HistoryViewModel.QUERY_LIMIT}건에 걸려 **최근 것만** 보입니다 — 시간대 칸을 눌러 좁혀 보세요"
            else "이 시간대도 상한에 걸렸습니다 — 더 좁은 범위는 콘솔 이력에서 봅니다",
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
            color = MaterialTheme.colorScheme.tertiary, fontSize = 12.sp)
        if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        when (kind) {
            HistoryKind.PTT -> PttPane(vm, rows, Modifier.weight(1f))
            else -> CallPane(vm, rows, Modifier.weight(1f))
        }
    }
}

@Composable
private fun Locked(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

// ── 도구줄 ──────────────────────────────────────────────────────────────────

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Toolbar(vm: HistoryViewModel) {
    val kind by vm.kind.collectAsStateWithLifecycle()
    val date by vm.date.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    val rows by vm.rows.collectAsStateWithLifecycle()
    var pick by remember { mutableStateOf(false) }

    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)) {

        SingleChoiceSegmentedButtonRow {
            listOf(HistoryKind.CALL, HistoryKind.PTT).forEachIndexed { i, k ->
                SegmentedButton(
                    selected = kind == k,
                    onClick = { vm.show(k) },
                    shape = SegmentedButtonDefaults.itemShape(i, 2),
                ) { Text(k.label) }
            }
        }

        TextButton(onClick = { vm.shiftDay(-1) }) { Text("◀") }
        TextButton(onClick = { pick = true }) { Text(date.toString(), fontWeight = FontWeight.Bold) }
        TextButton(onClick = { vm.shiftDay(1) }, enabled = date.isBefore(LocalDate.now())) { Text("▶") }
        TextButton(onClick = { vm.today() }) { Text("오늘") }

        OutlinedTextField(
            value = query, onValueChange = vm::search,
            placeholder = { Text("상대·그룹·참여자", fontSize = 13.sp) },
            singleLine = true,
            modifier = Modifier.width(220.dp).height(52.dp),
            textStyle = MaterialTheme.typography.bodySmall)

        Spacer(Modifier.weight(1f))
        Text(summaryOf(kind, rows), fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        TextButton(onClick = { vm.load() }) { Text("조회") }
    }

    if (pick) {
        val state = rememberDatePickerState(
            initialSelectedDateMillis = date.atStartOfDay(ZoneId.systemDefault()).toInstant().toEpochMilli())
        DatePickerDialog(
            onDismissRequest = { pick = false },
            confirmButton = {
                TextButton(onClick = {
                    state.selectedDateMillis?.let {
                        vm.showDate(Instant.ofEpochMilli(it).atZone(ZoneId.systemDefault()).toLocalDate())
                    }
                    pick = false
                }) { Text("확인") }
            },
            dismissButton = { TextButton(onClick = { pick = false }) { Text("취소") } }
        ) { DatePicker(state = state) }
    }
}

/** 요약 — 건수·진행중·녹취·PTT 발화 합(§4.6). */
private fun summaryOf(kind: HistoryKind, rows: List<HistoryEntry>): String {
    val live = rows.count { it.isActive }
    val rec = rows.count { it.hasRecording }
    val base = "${rows.size}건 · 진행중 $live · 녹취 $rec"
    if (kind != HistoryKind.PTT) return base
    val talkSec = rows.sumOf { it.totalSpeechMs } / 1000
    return "$base · 발화 ${talkSec / 60}분"
}

// ── 시간대 밴드 ─────────────────────────────────────────────────────────────

@Composable
private fun HourBand(vm: HistoryViewModel) {
    val band by vm.band.collectAsStateWithLifecycle()
    val sel by vm.hourFilter.collectAsStateWithLifecycle()
    val max = band.maxOrNull()?.coerceAtLeast(1) ?: 1

    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp).height(34.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        (0..23).forEach { h ->
            val n = band.getOrElse(h) { 0 }
            // 진할수록 많음 — 0 칸은 누를 수 없다(§4.6).
            val alpha = if (n == 0) 0.06f else 0.20f + 0.65f * (n.toFloat() / max)
            val bg = if (sel == h) MaterialTheme.colorScheme.primary
                     else MaterialTheme.colorScheme.primary.copy(alpha = alpha)
            Column(Modifier.weight(1f).fillMaxHeight()
                .clip(RoundedCornerShape(3.dp))
                .background(bg)
                .clickable(enabled = n > 0) { vm.toggleHour(h) },
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center) {
                Text("%02d".format(h), fontSize = 9.sp)
                if (n > 0) Text("$n", fontSize = 9.sp, fontWeight = FontWeight.Bold)
            }
        }
        if (sel != null) TextButton(onClick = { vm.clearHour() },
            contentPadding = PaddingValues(horizontal = 6.dp)) { Text("전체", fontSize = 11.sp) }
    }
}

// ── 통화 ────────────────────────────────────────────────────────────────────

@Composable
private fun CallPane(vm: HistoryViewModel, rows: List<HistoryEntry>, modifier: Modifier) {
    val sel by vm.selected.collectAsStateWithLifecycle()
    Column(modifier.fillMaxSize()) {
        CallHeader()
        HorizontalDivider()
        LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.id }) { e -> CallRow(e, e.id == sel?.id) { vm.select(e) } }
        }
        // 녹취 있는 행을 고르면 표 아래 띠만 나온다 — 통화는 한 줄이 곧 상세다.
        sel?.takeIf { it.hasRecording }?.let { RecordingStrip(vm) }
    }
}

@Composable
private fun CallHeader() {
    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp)) {
        listOf("유형" to 60, "발신 → 착신" to 260, "상태" to 70, "시작" to 80, "응답" to 80,
               "종료" to 80, "통화시간" to 76, "종료사유" to 100).forEach { (t, w) ->
            Text(t, Modifier.width(w.dp), fontSize = 11.sp, fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun CallRow(e: HistoryEntry, selected: Boolean, onClick: () -> Unit) {
    val bg = if (selected) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent
    Row(Modifier.fillMaxWidth().background(bg).clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Text(HistoryViewModel.callTypeText(e.callType), Modifier.width(60.dp), fontSize = 12.sp)
        Text("${short(e.from)} → ${short(e.to)}", Modifier.width(260.dp), fontSize = 12.sp, maxLines = 1)
        Text(HistoryViewModel.stateText(e), Modifier.width(70.dp), fontSize = 12.sp)
        Text(hhmmss(e.inviteAtMs ?: e.atMs), Modifier.width(80.dp), fontSize = 12.sp)
        Text(hhmmss(e.answerAtMs), Modifier.width(80.dp), fontSize = 12.sp)
        Text(hhmmss(e.endAtMs), Modifier.width(80.dp), fontSize = 12.sp)
        Text(durText(e.durationSec), Modifier.width(76.dp), fontSize = 12.sp)
        Text(HistoryViewModel.endReasonText(e.endReason), Modifier.width(100.dp), fontSize = 12.sp)
        if (e.emergency) Badge("긴급", MaterialTheme.colorScheme.error)
        if (e.hasRecording) Badge("녹취", MaterialTheme.colorScheme.tertiary)
    }
}

private fun short(uri: String): String =
    uri.substringAfter(':', uri).substringBefore('@').substringBefore(';').ifBlank { uri }

@Composable
private fun Badge(text: String, color: Color) {
    Surface(color = color.copy(alpha = 0.22f), shape = RoundedCornerShape(4.dp),
        modifier = Modifier.padding(start = 6.dp)) {
        Text(text, Modifier.padding(horizontal = 5.dp, vertical = 1.dp), fontSize = 10.sp, color = color)
    }
}

// ── PTT ─────────────────────────────────────────────────────────────────────

@Composable
private fun PttPane(vm: HistoryViewModel, rows: List<HistoryEntry>, modifier: Modifier) {
    val sel by vm.selected.collectAsStateWithLifecycle()
    Row(modifier.fillMaxSize()) {
        LazyColumn(Modifier.weight(1f).fillMaxHeight()) {
            items(rows, key = { it.id }) { e -> SessionCard(e, e.id == sel?.id) { vm.select(e) } }
        }
        VerticalDivider()
        Box(Modifier.weight(3f).fillMaxHeight()) {
            sel?.let { SessionPane(vm, it) }
                ?: Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("세션을 고르세요", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
        }
    }
}

@Composable
private fun SessionCard(e: HistoryEntry, selected: Boolean, onClick: () -> Unit) {
    val bg = if (selected) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent
    Column(Modifier.fillMaxWidth().background(bg).clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(HistoryViewModel.sessionKindText(e.sessionKind), fontSize = 11.sp,
                fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
            if (e.isFullDuplex) Badge("전이중", MaterialTheme.colorScheme.primary)
            if (e.isActive) Badge("진행중", MaterialTheme.colorScheme.tertiary)
            if (e.emergency) Badge("긴급", MaterialTheme.colorScheme.error)
            if (e.hasRecording) Badge("녹취", MaterialTheme.colorScheme.tertiary)
        }
        Text(e.groupName.ifBlank { short(e.group) }.ifBlank { e.people.joinToString(" ↔ ") { short(it) } },
            fontSize = 14.sp, fontWeight = FontWeight.Bold, maxLines = 1)
        Text("${hhmmss(e.startAtMs ?: e.atMs)}~${hhmmss(e.endAtMs)} · ${durText(e.durationSec)}",
            fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text("턴 ${e.turnCount} · 화자 ${e.speakerCount} · 동시 ${e.maxConcurrent}",
            fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    HorizontalDivider()
}

@Composable
private fun SessionPane(vm: HistoryViewModel, e: HistoryEntry) {
    val detail by vm.detail.collectAsStateWithLifecycle()
    val rec by vm.recording.collectAsStateWithLifecycle()
    val turns by vm.turns.collectAsStateWithLifecycle()
    var layers by remember { mutableStateOf(setOf("floor", "member")) }

    Column(Modifier.fillMaxSize().padding(12.dp)) {
        // 머리 — 종류·상태·대상·floor 정책
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(e.groupName.ifBlank { short(e.group) }, fontSize = 16.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.width(8.dp))
            Text("${HistoryViewModel.sessionKindText(e.sessionKind)} · ${HistoryViewModel.stateText(e)}" +
                 (if (e.floorPolicy.isNotBlank()) " · ${e.floorPolicy}" else ""),
                fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.weight(1f))
            if (e.hasRecording) {
                TextButton(onClick = { vm.playAll() }) { Text("▶ 전체") }
                TextButton(onClick = { vm.stop() }) { Text("정지") }
            }
        }
        // 지표 띠
        Text("발언 턴 ${e.turnCount} · 녹취 ${rec?.segments?.size ?: 0} · 최대 동시 ${e.maxConcurrent} · " +
             "발화 구간 ${e.totalSpeechMs / 1000}초 · 발화 누적 ${e.talkMs / 1000}초 · 화자 ${e.speakerCount}",
            fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(8.dp))

        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            Section("참여자 ${detail?.participants?.size ?: 0}")
            detail?.participants?.forEach { p ->
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    Text(short(p.msisdn), Modifier.width(160.dp), fontSize = 12.sp)
                    if (p.role == "initiator") Badge("개시자", MaterialTheme.colorScheme.primary)
                    Spacer(Modifier.weight(1f))
                    Text("${hhmmss(p.joinAtMs)}~${hhmmss(p.leaveAtMs)}", fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }

            Spacer(Modifier.height(10.dp))
            Section("발언 타임라인")
            TurnTimeline(turns, rec) { t -> vm.play(t.seq, t.slot, "${short(t.speaker)} ${t.durMs / 1000}초") }

            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Section("이벤트")
                Spacer(Modifier.width(8.dp))
                FilterChip(selected = "floor" in layers,
                    onClick = { layers = layers.toggle("floor") },
                    label = { Text("발언권 ${detail?.floor?.size ?: 0}", fontSize = 11.sp) })
                Spacer(Modifier.width(4.dp))
                FilterChip(selected = "member" in layers,
                    onClick = { layers = layers.toggle("member") },
                    label = { Text("멤버 ${detail?.events?.size ?: 0}", fontSize = 11.sp) })
            }
            EventList(detail?.floor.orEmpty().takeIf { "floor" in layers }.orEmpty(),
                      detail?.events.orEmpty().takeIf { "member" in layers }.orEmpty())
        }

        if (e.hasRecording) RecordingStrip(vm)
    }
}

private fun Set<String>.toggle(k: String): Set<String> = if (k in this) this - k else this + k

@Composable
private fun Section(title: String) {
    Text(title, fontSize = 13.sp, fontWeight = FontWeight.Bold)
}

/**
 * 타임라인 위 픽셀 위치 — **Long 으로 곱한 뒤 내린다**.
 *
 * `widthDp * offsetMs` 를 Int 로 하면 56분 지점에서 Int.MAX 를 넘어 음수가 된다. 음수 padding 은
 * Compose 가 `IllegalArgumentException` 으로 거절하므로, 긴 세션을 여는 것만으로 화면이 죽는다.
 * 결과는 0..width 로 가둔다 — 계산이 어긋나도 그리기가 실패하지 않게.
 */
internal fun barPos(widthDp: Int, valueMs: Int, totalMs: Int): Int {
    if (totalMs <= 0) return 0
    val v = widthDp.toLong() * valueMs.toLong() / totalMs.toLong()
    return v.coerceIn(0L, widthDp.toLong()).toInt()
}

/** 발언 막대 — 세션 시간축 위 화자 레인. 막대를 누르면 그 턴을 재생한다(§4.6). */
@Composable
private fun TurnTimeline(turns: List<TurnBar>, rec: RecordingInfo?, onPlay: (TurnBar) -> Unit) {
    if (turns.isEmpty()) {
        Text("발언 기록이 없습니다", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    val total = (turns.maxOf { it.offsetMs + it.durMs }).coerceAtLeast(1)
    val lanes = turns.map { it.speaker }.distinct()
    val widthDp = 640
    Column(Modifier.horizontalScroll(rememberScrollState())) {
        lanes.forEach { who ->
            Row(Modifier.padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(short(who), Modifier.width(110.dp), fontSize = 11.sp, maxLines = 1)
                Box(Modifier.width(widthDp.dp).height(18.dp)
                    .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(3.dp))) {
                    turns.filter { it.speaker == who }.forEach { t ->
                        // **Long 으로 올려 곱한다.** Int 로는 640 × 3,600,000 이 Int.MAX 를 넘어
                        // 음수가 되고(56분 지점부터), `padding(start = 음수)` 는 예외를 던져
                        // 한 시간 넘는 세션을 여는 것만으로 앱이 죽는다.
                        val x = barPos(widthDp, t.offsetMs, total)
                        val w = barPos(widthDp, t.durMs, total).coerceAtLeast(3)
                        Box(Modifier.padding(start = x.dp).width(w.dp).fillMaxHeight()
                            .clip(RoundedCornerShape(3.dp))
                            .background(MaterialTheme.colorScheme.primary)
                            .clickable { onPlay(t) })
                    }
                }
            }
        }
        Text("0 ~ ${total / 1000}초" + (rec?.let { " · 세그먼트 ${it.segments.size}" } ?: ""),
            fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun EventList(floor: List<PttFloorEvent>, events: List<com.cims.ue.dispatch.session.PttEvent>) {
    data class Item(val atMs: Long, val text: String, val extra: String)
    val merged = buildList {
        floor.forEach { f ->
            add(Item(f.atMs ?: 0L, "${HistoryViewModel.floorOpText(f.op)} · ${short(f.user)}", floorExtra(f)))
        }
        events.forEach { ev ->
            add(Item(ev.atMs ?: 0L, "${HistoryViewModel.eventTypeText(ev.type)} · ${short(ev.member)}",
                ev.durationSec?.let { "${it}초" } ?: ""))
        }
    }.sortedBy { it.atMs }

    if (merged.isEmpty()) {
        Text("이벤트가 없습니다", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    merged.forEach { i ->
        Row(Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
            Text(hhmmss(i.atMs.takeIf { it > 0 }), Modifier.width(80.dp), fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(i.text, Modifier.weight(1f), fontSize = 11.sp)
            if (i.extra.isNotBlank()) Text(i.extra, fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** op 별 부가 정보 — 선점·동시·대기 순번·거절 사유·회수 유예(§4.6). */
private fun floorExtra(f: PttFloorEvent): String = buildList {
    if (f.preempt) add("선점" + (f.preemptedFrom.takeIf { it.isNotBlank() }?.let { " ← ${short(it)}" } ?: ""))
    f.talkers?.let { if (it > 1) add("동시 $it") }
    f.pos?.let { p -> f.qsize?.let { add("대기 $p/$it") } ?: add("대기 $p") }
    if (f.reason.isNotBlank()) add(HistoryViewModel.denyReasonText(f.reason))
    f.graceSec?.let { add("유예 ${it}초") }
    if (f.revoked.isNotBlank()) add("회수 ${short(f.revoked)}")
}.joinToString(" · ")

// ── 녹취 띠 ─────────────────────────────────────────────────────────────────

@Composable
private fun RecordingStrip(vm: HistoryViewModel) {
    val rec by vm.recording.collectAsStateWithLifecycle()
    val pb by vm.playback.collectAsStateWithLifecycle()
    val r = rec ?: return

    Surface(tonalElevation = 2.dp, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("녹취 ${r.segments.size}개", fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.width(10.dp))
                when {
                    pb.error.isNotBlank() -> Text(pb.error, fontSize = 11.sp, color = MaterialTheme.colorScheme.error)
                    pb.busy -> Text(pb.note.ifBlank { "받는 중…" }, fontSize = 11.sp)
                    pb.label.isNotBlank() -> Text("재생 중 · ${pb.label}", fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.primary)
                }
                Spacer(Modifier.weight(1f))
                TextButton(onClick = { vm.stop() }) { Text("정지") }
            }
            Row(Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                r.segments.forEach { seg ->
                    AssistChip(
                        onClick = {
                            vm.play(seg.seq, null, "${seg.seq} ${short(seg.speakerId)}", retry = !seg.playable)
                        },
                        label = {
                            Text("${seg.seq} ${short(seg.speakerId)} ${seg.durationMs / 1000}초" +
                                 (if (!seg.playable) " · 다시 변환" else ""), fontSize = 11.sp)
                        })
                }
            }
        }
    }
}
