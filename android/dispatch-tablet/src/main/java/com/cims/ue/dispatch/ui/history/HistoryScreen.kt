// [이력] 화면 (docs/design/features/android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
//
// Windows 관제 앱의 [이력] 과 같은 구성이다 — 도구줄 · 시간대 밴드(그날의 분포이자 필터) · **왼쪽 카드 목록(시간대 묶음) +
// 오른쪽 상세 패널**. 통화도 무전도 같은 짜임이고, 영상이 있는 녹취를 틀 때만 상세 오른쪽에 영상 칸이 열린다.
// 이 파일은 껍데기(VM 을 붙인다)와 도구줄·밴드·목록이고, 상세 패널은 `HistoryDetail.kt`, 녹취 재생 바·영상 칸은
// `HistoryPlayerBar.kt` 다. 판정 규칙은 전부 순수 함수(`HistoryRows.kt`·`HistoryAxis.kt`·`HistoryPlayback.kt`)다.
package com.cims.ue.dispatch.ui.history

import android.view.Surface
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.ForwardPttKeys
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.Rect
import com.cims.ue.dispatch.ui.RectButton
import com.cims.ue.dispatch.ui.Segmented
import com.cims.ue.dispatch.ui.StatusDot
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type
import com.cims.ue.dispatch.ui.VDivider
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/** 왼쪽 목록 칸의 폭 — 카드 한 장에 대상·라벨·시각이 한 줄로 드는 값. 상세 패널이 주역이라 나머지를 다 준다. */
private val ListWidth = 360.dp

/** 도구줄의 종류 — 관제 탭 줄과 같은 [무전|통화] 순서. */
private val KINDS = listOf(HistoryKind.PTT, HistoryKind.CALL)

/** [이력] 화면 — **VM 을 붙이는 껍데기**. 그리는 일은 [HistoryScreenContent] 가 한다. */
@Composable
fun HistoryScreen(vm: HistoryViewModel, modifier: Modifier = Modifier) {
    if (!vm.available) return Locked("관제 역할 미배정 — 이력을 볼 수 없습니다")

    val ui by vm.ui.collectAsStateWithLifecycle()
    val pane by vm.pane.collectAsStateWithLifecycle()
    val player by vm.playback.collectAsStateWithLifecycle()

    // 화면에 처음 들어올 때 한 번 조회하고, 떠날 때 재생을 멈춘다(§6.11).
    LaunchedEffect(Unit) { if (vm.ui.value.rows.isEmpty()) vm.load() }
    DisposableEffect(Unit) { onDispose { vm.onLeave() } }

    val act = remember(vm) {
        HistoryActions(
            show = vm::show, load = { vm.refresh() }, search = vm::search, shiftDay = vm::shiftDay,
            showDate = vm::showDate, today = vm::today, toggleHour = vm::toggleHour, clearHour = vm::clearHour,
            setBandMode = vm::setBandMode, setService = vm::setService, setGroupSilent = vm::setGroupSilent,
            toggleBundle = vm::toggleBundle, select = vm::select, setCompactGaps = vm::setCompactGaps,
            seek = vm::seekRatio, togglePlay = vm::togglePlay, back10 = vm::back10, fwd10 = vm::fwd10,
            prevTurn = vm::prevTurn, nextTurn = vm::nextTurn, playAll = vm::playAll, stop = vm::stop, retry = vm::retry,
            playTurn = vm::playTurn, setSpeed = vm::setSpeed, setSkipGaps = vm::setSkipGaps, surface = vm::attachSurface)
    }
    HistoryScreenContent(ui, act, pane, player, vm.names, modifier)
}

/** [이력] 화면의 조작 — 조회 축과 녹취 재생. Preview·시험은 기본값(아무 일도 하지 않는다)으로 선다. */
data class HistoryActions(
    val show: (HistoryKind) -> Unit = {},
    val load: () -> Unit = {},
    val search: (String) -> Unit = {},
    val shiftDay: (Long) -> Unit = {},
    val showDate: (LocalDate) -> Unit = {},
    val today: () -> Unit = {},
    val toggleHour: (Int) -> Unit = {},
    val clearHour: () -> Unit = {},
    val setBandMode: (BandMode) -> Unit = {},
    val setService: (ServiceFilter) -> Unit = {},
    val setGroupSilent: (Boolean) -> Unit = {},
    val toggleBundle: (String) -> Unit = {},
    val select: (HistoryEntry) -> Unit = {},
    val setCompactGaps: (Boolean) -> Unit = {},
    // ── 녹취 재생 ──
    /** 막대를 눌렀다 — 막대 폭 대비 위치(0~1). */
    val seek: (Float) -> Unit = {},
    val togglePlay: () -> Unit = {},
    val back10: () -> Unit = {},
    val fwd10: () -> Unit = {},
    val prevTurn: () -> Unit = {},
    val nextTurn: () -> Unit = {},
    val playAll: () -> Unit = {},
    val stop: () -> Unit = {},
    val retry: () -> Unit = {},
    val playTurn: (Turn) -> Unit = {},
    val setSpeed: (Int) -> Unit = {},
    val setSkipGaps: (Boolean) -> Unit = {},
    /** 영상 칸의 그리기 면이 생겼다/사라졌다(null). */
    val surface: (Surface?) -> Unit = {},
)

/**
 * [이력] 본문 — **순수 컴포저블**. 세션·VM 없이 선다(Preview).
 *
 * @param pane 고른 무전 세션의 패널. null 이면 고른 항목만으로 머리·숫자 칸을 세운다.
 * @param player 녹취 재생 바의 값.
 * @param names 이름 풀이(사람 = 주소록, 그룹 = 그룹 목록).
 */
@Composable
fun HistoryScreenContent(
    ui: HistoryUi,
    act: HistoryActions = HistoryActions(),
    pane: SessionPaneUi? = null,
    player: PlayerUi = PlayerUi(),
    names: HistoryNames = remember { HistoryNames() },
    modifier: Modifier = Modifier,
) {
    val p = Tokens.palette
    val ptt = ui.kind == HistoryKind.PTT
    // 하루 상한(1000건)까지 오는 목록이라 행·묶음은 자료가 바뀔 때만 다시 만든다.
    val rows = remember(ui.rows, names) { ui.rows.map { rowOf(it, names) } }
    val list = remember(rows, ui.kind, ui.groupSilent, ui.openBundles) {
        historyListOf(rows, ui.kind, ui.groupSilent, ui.openBundles)
    }
    val selected = remember(ui.selected, names) { ui.selected?.let { rowOf(it, names) } }

    Column(modifier.fillMaxSize().background(p.paper)) {
        Toolbar(ui, act)
        HourBand(ui, act)
        Notices(ui)
        Row(Modifier.weight(1f).fillMaxWidth()) {
            ListColumn(list, ui, act, Modifier.width(ListWidth).fillMaxHeight())
            VDivider()
            Row(Modifier.weight(1f).fillMaxHeight()) {
                Box(Modifier.weight(1f).fillMaxHeight()) {
                    when {
                        selected == null -> Hint(if (ptt) "왼쪽에서 세션을 고르면 참여자·발언·이벤트가 여기 나옵니다"
                                                 else "왼쪽에서 통화를 고르면 여기 나옵니다")
                        ptt -> SessionPane(
                            pane?.takeIf { it.row.e.id == selected.e.id }
                                ?: remember(selected, ui.compactGaps) { buildSessionPane(selected, null, null, ui.compactGaps, names) },
                            player, ui.compactGaps, act)
                        else -> CallDetail(selected, player, act)
                    }
                }
                // 영상 칸 — 영상이 있는 녹취(영상 통화 · MCVideo 송출 구간)를 틀 때만 열린다. 소리만 있는 녹취는 칸 없이 소리만 난다.
                if (player.video) {
                    VDivider()
                    VideoPane(player, act, Modifier.width(VideoPaneWidth).fillMaxHeight())
                }
            }
        }
    }
}

@Composable
private fun Locked(text: String) = Hint(text)

@Composable
internal fun Hint(text: String, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body, color = Tokens.palette.muted)
    }
}

// ── 도구줄 ──────────────────────────────────────────────────────────────────

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Toolbar(ui: HistoryUi, act: HistoryActions) {
    val p = Tokens.palette
    var pick by remember { mutableStateOf(false) }
    val ptt = ui.kind == HistoryKind.PTT

    Row(Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        // 관제 탭 줄과 같은 [무전|통화] — 같은 순서·같은 모양.
        Segmented(options = KINDS.map { it.label }, selected = KINDS.indexOf(ui.kind).coerceAtLeast(0),
            onSelect = { act.show(KINDS[it]) }, itemWidth = 72.dp)
        Spacer(Modifier.width(4.dp))

        // 날짜 — 하루 단위 창 조회(서버 스캔 48시간 버킷 상한). 미래로는 못 간다.
        RectButton("◀", { act.shiftDay(-1) }, height = 32.dp)
        RectButton(ui.date.toString(), { pick = true }, height = 32.dp, bold = true)
        RectButton("▶", { act.shiftDay(1) }, height = 32.dp, enabled = ui.date.isBefore(LocalDate.now()))
        RectButton("오늘", act.today, height = 32.dp)

        SearchBox(ui.query, act.search, "이름 · 번호 · 그룹", Modifier.width(170.dp))

        if (ptt) {
            // 서비스 거르기 — 음성 무전(MCPTT) 세션 / 영상(MCVideo) 세션. 받은 항목에 서비스 축이 실려 있을 때만.
            if (ui.serviceAxis || ui.rows.any { it.service.isNotEmpty() }) ServiceFilter.entries.forEach { f ->
                FilterPill(f.label, ui.service == f, onClick = { act.setService(f) })
            }
            FilterPill("빈 세션 묶기", ui.groupSilent, onClick = { act.setGroupSilent(!ui.groupSilent) })
        }
        Spacer(Modifier.weight(1f))
        IconButton(onClick = act.load, modifier = Modifier.size(36.dp)) {
            Icon(Icons.Filled.Refresh, contentDescription = "이 날짜를 다시 조회", modifier = Modifier.size(20.dp), tint = p.ink2)
        }
    }

    if (pick) {
        val state = rememberDatePickerState(
            initialSelectedDateMillis = ui.date.atStartOfDay(ZoneId.of("UTC")).toInstant().toEpochMilli())
        DatePickerDialog(
            onDismissRequest = { pick = false },
            confirmButton = {
                TextButton(onClick = {
                    // 달력은 고른 날의 UTC 자정을 준다 — UTC 로 읽어야 시간대와 상관없이 그 날짜다.
                    state.selectedDateMillis?.let { act.showDate(Instant.ofEpochMilli(it).atZone(ZoneId.of("UTC")).toLocalDate()) }
                    pick = false
                }) { Text("확인") }
            },
            dismissButton = { TextButton(onClick = { pick = false }) { Text("취소") } }
        ) { ForwardPttKeys(); DatePicker(state = state) }
    }
}

/** 한 줄 검색 입력(32) — 도구줄에 선다. */
@Composable
private fun SearchBox(value: String, onValue: (String) -> Unit, hint: String, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(8.dp)
    BasicTextField(
        value = value, onValueChange = onValue, singleLine = true,
        textStyle = TextStyle(fontSize = Type.body, color = p.ink),
        cursorBrush = SolidColor(p.primaryLine),
        modifier = modifier.height(32.dp).clip(shape).background(p.paper).border(1.dp, p.edge, shape),
        decorationBox = { inner ->
            Box(Modifier.fillMaxSize().padding(horizontal = 10.dp), contentAlignment = Alignment.CenterStart) {
                if (value.isEmpty()) Text(hint, fontSize = Type.body, color = p.faint, maxLines = 1)
                inner()
            }
        })
}

// ── 시간대 밴드 ─────────────────────────────────────────────────────────────

/**
 * 시간대 밴드 — 그날의 분포이자 필터(칸 = 그 시간대만, 다시 누르면 해제). 통화 = «시간대별 통화 횟수», 무전 = «시간대별 무전» +
 * [세션 수 | 발언 수]. 제목 글자가 모드와 상관없이 같아 전환 자리가 움직이지 않는다.
 */
@Composable
private fun HourBand(ui: HistoryUi, act: HistoryActions) {
    val p = Tokens.palette
    val ptt = ui.kind == HistoryKind.PTT
    val turns = ptt && ui.bandMode == BandMode.TURNS
    val cells = remember(ui.band, ui.turnBand, ui.rows, turns) {
        when {
            !turns -> sessionCells(ui.band)
            ui.turnBand.isNotEmpty() -> ui.turnBand
            else -> turnCells(ui.rows, ui.band)
        }
    }
    val shape = RoundedCornerShape(10.dp)
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp).clip(shape).background(p.canvas).border(1.dp, p.divider, shape)
            .padding(horizontal = 10.dp, vertical = 6.dp)) {
        Row(Modifier.fillMaxWidth().height(28.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(if (ptt) "시간대별 무전" else "시간대별 통화 횟수", fontSize = Type.body, fontWeight = FontWeight.Bold, color = p.ink)
            if (ptt) Segmented(options = BandMode.entries.map { it.label }, selected = ui.bandMode.ordinal,
                onSelect = { act.setBandMode(BandMode.entries[it]) }, height = 26.dp, itemWidth = 62.dp, strong = false)
            // 칸을 누르면 무엇이 걸러지는지 — [발언 수] 에서는 그 시간대의 발언 있는 세션만.
            Text(if (turns) "칸을 누르면 그 시간의 발언 있는 세션만 · 다시 누르면 전체 · «+» = 하루 상한으로 덜 센 칸"
                 else "칸을 누르면 그 시간만 · 다시 누르면 전체",
                Modifier.weight(1f), fontSize = Type.meta, color = p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.End)
            if (ui.hourFilter != null) RectButton("전체 시간", act.clearHour, height = 26.dp)
        }
        Spacer(Modifier.height(4.dp))
        Row(Modifier.fillMaxWidth().height(34.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
            cells.forEach { c ->
                val on = ui.hourFilter == c.hour
                val cell = RoundedCornerShape(6.dp)
                // 농도 = 그날 가장 많은 칸 대비 연한 남색(글자가 늘 읽히는 범위), 고른 칸 = 남색 테두리.
                Column(Modifier.weight(1f).fillMaxHeight().clip(cell).background(p.paper)
                        .background(p.primary.copy(alpha = c.ratio))
                        .then(if (on) Modifier.border(2.5.dp, p.primaryLine, cell) else Modifier)
                        .clickable(enabled = c.count > 0 || on) { act.toggleHour(c.hour) },
                    horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
                    Text(c.countText, fontSize = Type.meta, fontWeight = FontWeight.Bold, color = p.ink, maxLines = 1)
                    Text("%02d".format(c.hour), fontSize = Type.micro, maxLines = 1,
                        color = when { on -> p.primaryInk; c.count == 0 -> p.faint; else -> p.muted })
                }
            }
        }
    }
}

/** 조회 실패·절삭 알림·진행 표시 — 있을 때만 한 줄씩. */
@Composable
private fun Notices(ui: HistoryUi) {
    val p = Tokens.palette
    if (ui.error.isNotBlank()) Text(ui.error, Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 4.dp),
        color = p.emg, fontSize = Type.body, maxLines = 2, overflow = TextOverflow.Ellipsis)
    // 절삭되면 그렇다고 쓴다 — 조용히 일부만 보여 주면 «없는 통화» 로 읽힌다.
    if (ui.truncated) Text(
        if (ui.hourFilter == null) "서버 상한 ${HistoryViewModel.QUERY_LIMIT}건에 걸려 최근 것만 보입니다 — 시간대 칸을 눌러 좁혀 보세요"
        else "이 시간대도 상한에 걸렸습니다 — 더 좁은 범위는 콘솔 이력에서 봅니다",
        Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 4.dp), color = p.ringInk, fontSize = Type.meta)
    if (ui.loading) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 4.dp).height(2.dp),
        color = p.primary, trackColor = p.hair)
    else Spacer(Modifier.height(6.dp))
}

// ── 목록 ────────────────────────────────────────────────────────────────────

/** 왼쪽 목록 — 요약 한 줄 + 시간대 묶음 카드. 보이는 카드만 만든다(하루 상한 1000건). */
@Composable
private fun ListColumn(list: HistoryList, ui: HistoryUi, act: HistoryActions, modifier: Modifier) {
    val p = Tokens.palette
    val ptt = ui.kind == HistoryKind.PTT
    val selectedId = ui.selected?.id
    Column(modifier.background(p.canvas)) {
        Text(summaryOf(list, ui.kind, ui.date, ui.hourFilter, ui.speechOnly),
            Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 6.dp, bottom = 2.dp),
            fontSize = Type.meta, color = p.muted, maxLines = 2, overflow = TextOverflow.Ellipsis)
        if (list.items.isEmpty() && !ui.loading)
            Text(if (ptt) "이 날에 맞는 무전이 없습니다" else "이 날에 맞는 통화가 없습니다",
                Modifier.padding(12.dp), fontSize = Type.body, color = p.muted)
        LazyColumn(Modifier.weight(1f).fillMaxWidth(), contentPadding = PaddingValues(bottom = 10.dp)) {
            items(list.items, key = { it.key }, contentType = { item ->
                when (item) { is ListItem.Hour -> 0; is ListItem.Line -> item.row.kind.ordinal + 1 }
            }) { item ->
                when (item) {
                    is ListItem.Hour -> HourHead(item)
                    is ListItem.Line -> {
                        val line = item.row
                        val on = line.holds(selectedId)
                        when {
                            !ptt -> CallCard(line.row, on) { act.select(line.row.e) }
                            line.kind == RowKind.MEMBER -> BundleMember(line, on) { act.select(line.row.e) }
                            else -> PttCard(line, on, onClick = { act.select(line.row.e) },
                                onToggle = { act.toggleBundle(line.bundleKey) })
                        }
                    }
                }
            }
        }
    }
}

/** 시간대 묶음 머리 — "HH시 · n건"(밴드와 같은 축). 건수는 줄 수가 아니라 세션 수다. */
@Composable
private fun HourHead(h: ListItem.Hour) {
    val p = Tokens.palette
    Row(Modifier.fillMaxWidth().padding(start = 14.dp, end = 14.dp, top = 8.dp, bottom = 3.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Text(h.title, fontSize = Type.body, fontWeight = FontWeight.Bold, color = p.ink2)
        Spacer(Modifier.weight(1f))
        Text("${h.count}건", fontSize = Type.meta, color = p.muted)
    }
}

/** 통화 결과 태그 — 응답(회색) · 통화 중/호출 중(초록) · 부재/실패/취소(주황) · 거절/오류(빨강). */
@Composable
internal fun ResultTag(r: CallResult) {
    Label(r.text, when (r.tone) {
        ResultTone.NEUTRAL -> LabelStyle.FILL
        ResultTone.TALK -> LabelStyle.TALK
        ResultTone.WARN -> LabelStyle.PILOT
        ResultTone.BAD -> LabelStyle.RED
    })
}

/** 카드의 테두리·면 — 고른 카드 = 남색 테두리 2.5 + 연한 남색 면, 긴급 = 빨강 테두리 2 + 연한 빨강 면(관제 채널 카드와 같다). */
@Composable
private fun cardFrame(selected: Boolean, emergency: Boolean): Modifier {
    val p = Tokens.palette
    val shape = RoundedCornerShape(10.dp)
    val bg = when { selected -> p.primarySoft; emergency -> p.emgSoft; else -> p.paper }
    val (bw, line) = when {
        selected -> 2.5.dp to p.primaryLine
        emergency -> 2.dp to p.emg
        else -> 1.dp to p.line
    }
    return Modifier.clip(shape).background(bg).border(bw, line, shape)
}

/**
 * 통화 카드(두 줄) — 1줄 결과 태그 · 발신 → 착신 · 영상/긴급/녹취 · 호출 시각 / 2줄 통화 시간 · 울림 또는 끝난 이유 · 울림.
 * 이름이 길면 이름만 줄이고 라벨·시각은 오른쪽에 붙는다.
 */
@Composable
private fun CallCard(row: HistoryRow, selected: Boolean, onClick: () -> Unit) {
    val p = Tokens.palette
    Column(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 3.dp)
            .then(cardFrame(selected, row.e.emergency)).clickable(onClick = onClick)
            .padding(start = 12.dp, end = 10.dp, top = 8.dp, bottom = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            ResultTag(row.result)
            Spacer(Modifier.width(2.dp))
            Text(buildAnnotatedString {
                append(row.caller)
                withStyle(SpanStyle(color = p.faint, fontWeight = FontWeight.Normal)) { append(" → ") }
                append(row.callee)
            }, Modifier.weight(1f), fontSize = Type.strong, fontWeight = FontWeight.Bold, color = p.ink,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (row.isVideoCall) Label("영상", LabelStyle.HELD)
            if (row.e.emergency) Label("긴급", LabelStyle.EMG)
            if (row.e.hasRecording) Label("녹취", LabelStyle.MON)
            Text(row.startClock, Modifier.padding(start = 2.dp), fontSize = Type.meta, color = p.muted, maxLines = 1)
        }
        Text(row.callSub, Modifier.padding(start = 2.dp, top = 3.dp), fontSize = Type.meta, color = p.muted,
            maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/**
 * 무전 세션 카드(세 줄) — 1줄 상태 점 · 대상 · 라벨(영상/개별/애드혹/전이중/진행 중/긴급/녹취) · 시작 시각 / 2줄 개시·참여 n명 /
 * 3줄 길이 · 발언 n회 · 말한 시간(영상 세션은 송출·보낸 시간). 빈 세션 묶음의 머리는 뒤에 카드 두 장이 겹쳐 보이고
 * «빈 세션 n건» 라벨과 [n건 펼치기] 가 붙는다 — 고르면 가장 최근 세션이 열린다.
 */
@Composable
private fun PttCard(line: ListRow, selected: Boolean, onClick: () -> Unit, onToggle: () -> Unit) {
    val p = Tokens.palette
    val row = line.row
    val bundle = line.kind == RowKind.BUNDLE
    val shape = RoundedCornerShape(10.dp)
    Box(Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, top = 3.dp, bottom = if (bundle) 11.dp else 3.dp)) {
        if (bundle) {
            Box(Modifier.matchParentSize().offset(y = 8.dp).padding(horizontal = 10.dp).clip(shape).background(p.paper)
                .border(1.dp, p.divider, shape))
            Box(Modifier.matchParentSize().offset(y = 4.dp).padding(horizontal = 5.dp).clip(shape).background(p.paper)
                .border(1.dp, p.line, shape))
        }
        Column(Modifier.fillMaxWidth().then(cardFrame(selected, row.e.emergency)).clickable(onClick = onClick)
                .padding(start = 12.dp, end = 10.dp, top = 8.dp, bottom = 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                StatusDot(when { row.e.emergency -> p.emg; row.live -> p.talk; else -> p.muted })
                Text(row.target, Modifier.weight(1f).padding(start = 3.dp), fontSize = Type.strong, fontWeight = FontWeight.Bold,
                    color = if (row.e.emergency) p.emgInk else p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (bundle) Label(line.tagText)
                if (row.e.isMcVideo) Label("영상", LabelStyle.TEAL)
                if (row.isPrivate) Label("개별")
                if (row.isAdhoc) Label("애드혹")
                if (row.e.isFullDuplex) Label("전이중", LabelStyle.HELD)
                if (row.live) Label("진행 중", LabelStyle.TALK)
                if (row.e.emergency) Label("긴급", LabelStyle.EMG)
                if (row.e.hasRecording) Label("녹취", LabelStyle.MON)
                Text(line.clock, Modifier.padding(start = 2.dp), fontSize = Type.meta, color = p.muted, maxLines = 1)
            }
            if (row.whoLine.isNotEmpty()) Text(row.whoLine, Modifier.padding(start = 16.dp, top = 2.dp), fontSize = Type.meta,
                color = p.ink2, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Row(Modifier.padding(start = 16.dp, top = 1.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(line.statLine, Modifier.weight(1f), fontSize = Type.meta, color = p.muted, maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
                // 묶음 머리만 — 그 묶음을 펼친다/접는다.
                if (bundle) RectButton(line.toggleText + if (line.expanded) " ▴" else " ▾", onToggle, height = 26.dp,
                    kind = if (line.expanded) Rect.ON else Rect.SOFT)
            }
        }
    }
}

/** 펼친 묶음 안의 한 줄 — 시각 범위·길이 / 발언 0회. 하나씩 고를 수 있다. */
@Composable
private fun BundleMember(line: ListRow, selected: Boolean, onClick: () -> Unit) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(7.dp)
    Row(Modifier.fillMaxWidth().padding(start = 26.dp, end = 8.dp, top = 1.dp, bottom = 1.dp).clip(shape)
            .background(if (selected) p.primarySoft else p.fill)
            .border(if (selected) 1.5.dp else 1.dp, if (selected) p.primaryLine else p.divider, shape)
            .clickable(onClick = onClick).padding(horizontal = 10.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Text(line.row.rangeText, Modifier.weight(1f), fontSize = Type.meta, color = p.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        Text(line.memberStat, Modifier.padding(start = 8.dp), fontSize = Type.micro, color = p.muted, maxLines = 1)
    }
}
