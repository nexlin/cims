// [이력] 오른쪽 상세 패널 — 통화 상세 / 무전 세션 패널 (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
//
// 통화 = 머리 → 숫자 칸 넷 → 당사자 → 진행 막대 → 녹취 재생 바.
// 무전 = 머리 → 숫자 칸 → ① 참여자 → ② 발언(화자 레인 + 턴 막대) → ③ 이벤트 → 녹취 재생 바.
// 영상 세션(MCVideo 그룹 호)은 같은 패널을 낱말만 바꿔 쓴다(발언 → 송출).
//
// 전부 **순수 컴포저블**이다 — 받은 값만 그리고, 화면 안의 일시 상태(배율·층 토글·눌러 본 풀이)만 제가 든다.
@file:OptIn(ExperimentalLayoutApi::class)

package com.cims.ue.dispatch.ui.history

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.HDivider
import com.cims.ue.dispatch.ui.Initial
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.Palette
import com.cims.ue.dispatch.ui.RectButton
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type

/**
 * 화자 레인의 색 — 등장 순서로 배정한다. 데스크톱 `SpkColors`·콘솔 `SPK_COLORS` 와 **같은 12색**이다(같은 세션을 세 화면에서 보면
 * 같은 화자가 같은 색이다). 레인 색은 뜻이 아니라 구별이라 테마를 타지 않는다.
 */
private val LANE_COLORS = longArrayOf(0xFF2563EB, 0xFF16A34A, 0xFFD97706, 0xFF9333EA, 0xFFDC2626, 0xFF0891B2,
    0xFFCA8A04, 0xFFDB2777, 0xFF4F46E5, 0xFF059669, 0xFFE11D48, 0xFF0D9488).map { Color(it) }

@Suppress("UnusedReceiverParameter")
internal fun Palette.laneColor(index: Int): Color = LANE_COLORS[(if (index < 0) 0 else index) % LANE_COLORS.size]

private fun Palette.dot(tone: DotTone): Color = when (tone) {
    DotTone.TALK -> talk
    DotTone.EMG -> emg
    DotTone.HELD -> held
    DotTone.RING -> ring
    DotTone.MON -> mon
    DotTone.LISTEN -> listen
    DotTone.GRAY -> wire
}

/** 패널 머리 — 옅은 띠 위에 제목·라벨, 오른쪽 끝에 시각·버튼. */
@Composable
private fun PaneHead(content: @Composable RowScope.() -> Unit) {
    val p = Tokens.palette
    Row(Modifier.fillMaxWidth().background(p.bar).padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) { content() }
    HDivider()
}

/** 숫자 칸 — 연한 면 위에 이름(작게)·값(크게)·단위. */
@Composable
private fun MetricCell(key: String, value: String, suffix: String = "", modifier: Modifier = Modifier) {
    val p = Tokens.palette
    Column(modifier.clip(RoundedCornerShape(8.dp)).background(p.fill).padding(horizontal = 12.dp, vertical = 6.dp)) {
        Text(key, fontSize = Type.micro, color = p.muted, maxLines = 1)
        Text(buildAnnotatedString {
            append(value)
            if (suffix.isNotEmpty()) withStyle(SpanStyle(fontSize = Type.meta, fontWeight = FontWeight.Normal, color = p.muted)) { append(suffix) }
        }, fontSize = Type.head, fontWeight = FontWeight.Bold, color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun SectionTitle(title: String, note: String = "") {
    val p = Tokens.palette
    Text(buildAnnotatedString {
        append(title)
        if (note.isNotEmpty()) withStyle(SpanStyle(fontSize = Type.meta, fontWeight = FontWeight.Normal, color = p.muted)) { append("  $note") }
    }, fontSize = Type.body, fontWeight = FontWeight.Bold, color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

// ── 통화 상세 ───────────────────────────────────────────────────────────────

/** 통화 상세 — 머리(발신 → 착신 · 결과·영상·긴급 · 날짜 시각) → 숫자 칸 넷 → 당사자 → 진행 → 녹취 재생 바(아래 고정). */
@Composable
internal fun CallDetail(row: HistoryRow, player: PlayerUi, act: HistoryActions) {
    val p = Tokens.palette
    Column(Modifier.fillMaxSize()) {
        PaneHead {
            Text(buildAnnotatedString {
                append(row.caller)
                withStyle(SpanStyle(color = p.faint, fontWeight = FontWeight.Normal)) { append(" → ") }
                append(row.callee)
            }, Modifier.weight(1f, fill = false).padding(end = 5.dp), fontSize = Type.head, fontWeight = FontWeight.Bold,
                color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
            ResultTag(row.result)
            if (row.isVideoCall) Label("영상", LabelStyle.HELD)
            if (row.e.emergency) Label("긴급", LabelStyle.EMG)
            Spacer(Modifier.weight(1f))
            Text(row.dayClock, fontSize = Type.meta, color = p.muted, maxLines = 1)
        }
        Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 12.dp)) {
            // 숫자 칸 — 울린 시간(호출~응답, 응답이 없으면 끊길 때까지) · 통화 시간 · 끝난 이유 · 종류
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MetricCell("울린 시간", row.ringText, modifier = Modifier.weight(1f))
                MetricCell("통화 시간", row.durationText, modifier = Modifier.weight(1f))
                MetricCell("끝난 이유", row.endReasonText, modifier = Modifier.weight(1f))
                MetricCell("종류", row.typeText, modifier = Modifier.weight(1f))
            }
            Spacer(Modifier.height(16.dp))
            SectionTitle("당사자")
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Party("발신", row.caller, row.callerNumber, Modifier.weight(1f))
                Text("→", Modifier.padding(horizontal = 10.dp), fontSize = Type.head, color = p.faint)
                Party("착신", row.callee, row.calleeNumber, Modifier.weight(1f))
            }
            Spacer(Modifier.height(16.dp))
            // 진행 — 울림(주황) : 통화(남색) 비율 막대 + 호출·응답·종료 시각
            SectionTitle("진행")
            Spacer(Modifier.height(6.dp))
            Row(Modifier.fillMaxWidth().height(10.dp).clip(RoundedCornerShape(5.dp)).background(p.fill)) {
                if (row.ringWeight > 0f) Box(Modifier.weight(row.ringWeight).fillMaxHeight().background(p.ringEdge))
                if (row.talkWeight > 0f) Box(Modifier.weight(row.talkWeight).fillMaxHeight().background(p.primary))
            }
            Row(Modifier.fillMaxWidth().padding(top = 6.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                Stamp("호출", hhmmss(row.e.inviteAtMs ?: row.e.atMs))
                Stamp("응답", hhmmss(row.e.answerAtMs))
                Stamp("종료", hhmmss(row.e.endAtMs))
            }
        }
        HDivider()
        PlayerBarView(player, ptt = false, act = act)
    }
}

@Composable
private fun Stamp(name: String, clock: String) {
    val p = Tokens.palette
    Text(buildAnnotatedString {
        withStyle(SpanStyle(color = p.muted)) { append("$name ") }
        append(clock)
    }, fontSize = Type.meta, color = p.ink, maxLines = 1)
}

/** 당사자 이름표 — 아바타(색 = 이름에서) · 발신/착신 · 이름 · 이름 아래 번호(이름이 번호와 같으면 비운다). */
@Composable
private fun Party(role: String, name: String, number: String, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(10.dp)
    Row(modifier.clip(shape).border(1.dp, p.divider, shape).padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Initial(name, size = 36.dp)
        Column {
            Text(role, fontSize = Type.micro, color = p.muted)
            Text(name, fontSize = Type.strong, fontWeight = FontWeight.Bold, color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (number.isNotEmpty()) Text(number, fontSize = Type.meta, color = p.muted, maxLines = 1)
        }
    }
}

// ── 무전 세션 패널 ──────────────────────────────────────────────────────────

/**
 * 무전 세션 패널 — 머리(대상 · 종류·영상·전이중·진행 중·긴급·floor 정책 · 시작~종료 · [처음부터]·[정지]) → 숫자 칸 →
 * ① 참여자 → ② 발언(영상 세션은 «송출») → ③ 이벤트 → 녹취 재생 바(아래 고정).
 */
@Composable
internal fun SessionPane(pane: SessionPaneUi, player: PlayerUi, compactGaps: Boolean, act: HistoryActions) {
    // 재생 중에는 재생 바의 값이 200 ms 마다 바뀐다 — 머리와 본문은 그 값에서 필요한 것만 받아, 바뀌지 않으면 다시 그리지 않는다.
    Column(Modifier.fillMaxSize()) {
        SessionHead(pane, canPlay = player.has, canStop = player.mediaOpen || player.playing, act = act)
        SessionBody(pane, compactGaps, act, Modifier.weight(1f).fillMaxWidth())
        HDivider()
        PlayerBarView(player, ptt = true, act = act)
    }
}

@Composable
private fun SessionHead(pane: SessionPaneUi, canPlay: Boolean, canStop: Boolean, act: HistoryActions) {
    val p = Tokens.palette
    val row = pane.row
    PaneHead {
        Text(row.target, Modifier.weight(1f, fill = false).padding(end = 5.dp), fontSize = Type.head, fontWeight = FontWeight.Bold,
            color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Label(row.kindText)
        if (pane.video) Label("영상", LabelStyle.TEAL)
        if (row.mcvText.isNotEmpty()) Label(row.mcvText, bold = false)
        if (row.e.isFullDuplex) Label("전이중", LabelStyle.HELD)
        if (row.live) Label("진행 중", LabelStyle.TALK)
        if (row.e.emergency) Label("긴급", LabelStyle.EMG)
        if (row.hasFloorPolicy) Label(row.floorPolicyText, bold = false)
        Text(row.rangeText, Modifier.weight(1f).padding(start = 3.dp), fontSize = Type.meta, color = p.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        RectButton("▶ 처음부터", act.playAll, height = 28.dp, enabled = canPlay)
        RectButton("정지", act.stop, height = 28.dp, enabled = canStop)
    }
}

/** 세션 패널 본문 — 숫자 칸 · ① 참여자 · ② 발언 · ③ 이벤트. 이벤트는 수백 줄이 될 수 있어 보이는 줄만 만든다. */
@Composable
private fun SessionBody(pane: SessionPaneUi, compactGaps: Boolean, act: HistoryActions, modifier: Modifier) {
    val p = Tokens.palette
    val row = pane.row
    // 눌러 본 풀이 — 터치에는 툴팁이 없어, 숫자 칸·참여자·줄인 틈을 누르면 그 풀이를 한 줄로 적는다.
    var note by remember(row.e.id) { mutableStateOf("") }
    // 이벤트 층 토글은 세션을 바꿔도 남는다(데스크톱과 같다 — 발언권 줄을 끄고 여러 세션을 넘겨 본다)
    var showFloor by remember { mutableStateOf(true) }
    var showMember by remember { mutableStateOf(true) }
    val timeline = remember(pane.timeline, showFloor, showMember) { timelineShown(pane.timeline, showFloor, showMember) }

    LazyColumn(modifier, contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp)) {
        item(key = "metrics") {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                pane.metrics.forEach { m ->
                    MetricCell(m.key, m.value, m.suffix,
                        Modifier.widthIn(min = 96.dp).clip(RoundedCornerShape(8.dp))
                            .clickable(enabled = m.hint.isNotEmpty()) { note = "${m.key} — ${m.hint}" })
                }
                if (pane.status.isNotEmpty()) Text(pane.status, Modifier.align(Alignment.CenterVertically),
                    fontSize = Type.meta, color = p.muted)
            }
            if (note.isNotEmpty()) Text(note, Modifier.padding(top = 4.dp), fontSize = Type.meta, color = p.muted, maxLines = 2,
                overflow = TextOverflow.Ellipsis)
            Spacer(Modifier.height(10.dp))
        }
        // ① 참여자 — 입퇴장 기록 ∪ 화자. 말한 사람은 아래 발언 레인과 같은 색 점. 이름표를 누르면 입장~퇴장·발언이 위에 적힌다.
        item(key = "participants") {
            SectionTitle("참여자", pane.participants.size.toString())
            Spacer(Modifier.height(6.dp))
            if (pane.participants.isEmpty()) Text("참여자 기록이 없습니다", fontSize = Type.meta, color = p.muted)
            else FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                pane.participants.forEach { who -> ParticipantChip(who) { note = "${who.label} — ${who.tip}" } }
            }
            Spacer(Modifier.height(12.dp))
        }
        // ② 발언 — 화자 레인 위 턴 막대. 막대를 누르면 그 발언부터 재생.
        item(key = "lanes") {
            Lanes(pane, compactGaps, act, onNote = { note = it })
            Spacer(Modifier.height(12.dp))
        }
        // ③ 이벤트 — 발언권 중재 + 입퇴장, 시간순
        item(key = "eventHead") {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                SectionTitle("이벤트")
                Spacer(Modifier.weight(1f))
                FilterPill("발언권 ${pane.floorCount}", showFloor, onClick = { showFloor = !showFloor })
                FilterPill("입퇴장 ${pane.memberEventCount}", showMember, onClick = { showMember = !showMember })
            }
            Spacer(Modifier.height(4.dp))
            if (timeline.isEmpty()) Text("표시할 항목이 없습니다", fontSize = Type.meta, color = p.muted)
        }
        items(timeline) { ev -> EventRow(ev) }
    }
}

/** 참여자 이름표 — 아바타 · 이름 · 개시 태그 · 말한 사람은 레인 색 점 + "발언 n회". */
@Composable
private fun ParticipantChip(who: ParticipantUi, onClick: () -> Unit) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(15.dp)
    Row(Modifier.clip(shape).background(p.canvas).border(1.dp, p.divider, shape).clickable(onClick = onClick)
            .padding(start = 3.dp, end = 10.dp, top = 3.dp, bottom = 3.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Initial(who.label, size = 24.dp)
        Text(who.label, Modifier.widthIn(max = 150.dp), fontSize = Type.meta, fontWeight = FontWeight.SemiBold, color = p.ink,
            maxLines = 1, overflow = TextOverflow.Ellipsis)
        if (who.initiator) Label("개시", LabelStyle.INK)
        if (who.spoke) {
            Box(Modifier.size(8.dp).clip(CircleShape).background(p.laneColor(who.lane)))
            Text("${who.word} ${who.turns}회", fontSize = Type.meta, color = p.muted, maxLines = 1)
        }
    }
}

/** 화자 이름 칸의 폭 — 레인 트랙은 그 오른쪽을 다 쓴다. */
private val LaneLabelWidth = 120.dp
private val LaneHeight = 24.dp

/**
 * ② 발언 타임라인 — 왼쪽 화자 이름(고정) | 오른쪽 트랙(가로 스크롤, 폭 = 보이는 폭 × 배율).
 *
 * **배율은 버튼으로** 바꾼다(×1.25, 최대 ×64 — 데스크톱 Ctrl+휠·[−][+] 와 같은 범위). 핀치를 쓰지 않는 것은 막대 누름(그 발언
 * 재생)·가로 스크롤과 제스처가 겹치기 때문이다. 배율을 바꿔도 **보던 가운데가 그대로** 있게 스크롤을 옮기고, 세션을 바꾸면 ×1 로
 * 돌아간다. **[틈 줄임]**(기본 켬) = 말이 없는 구간을 짧게 접어 말한 구간을 넓게 본다 — 접힌 틈은 레인 전체 높이의 띠 «⋯» 이고
 * 누르면 실제 시각·길이가 적힌다.
 */
@Composable
private fun Lanes(pane: SessionPaneUi, compactGaps: Boolean, act: HistoryActions, onNote: (String) -> Unit) {
    val p = Tokens.palette
    // 세션을 바꾸거나 [틈 줄임] 을 바꾸면 ×1 로 돌아간다 — 축이 바뀌어 앞의 배율·위치가 뜻이 없다(데스크톱과 같다)
    var zoom by remember(pane.row.e.id, compactGaps) { mutableFloatStateOf(1f) }
    val scroll = rememberScrollState()
    var pendingScroll by remember { mutableStateOf<Int?>(null) }
    // 새 폭이 그려진 뒤에 옮긴다 — 먼저 옮기면 옛 폭의 끝에서 잘린다.
    LaunchedEffect(zoom) {
        val target = pendingScroll ?: return@LaunchedEffect
        withFrameNanos { }
        scroll.scrollTo(target)
        pendingScroll = null
    }
    fun zoomTo(next: Float) {
        val z = next.coerceIn(1f, TIMELINE_ZOOM_MAX)
        if (z == zoom) return
        val half = scroll.viewportSize / 2
        pendingScroll = (((scroll.value + half) * (z / zoom)) - half).toInt().coerceAtLeast(0)
        zoom = z
    }

    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(Modifier.weight(1f)) {
            SectionTitle(pane.turnWord, if (pane.video) "막대를 누르면 그 송출 재생(영상)" else "막대를 누르면 그 발언 재생")
        }
        if (pane.lanes.isNotEmpty()) {
            Text("${pane.axisStartText} ~ ${pane.axisEndText}", fontSize = Type.micro, color = p.muted, maxLines = 1)
            FilterPill("틈 줄임", compactGaps, onClick = { act.setCompactGaps(!compactGaps) })
            RectButton("−", { zoomTo(zoom / TIMELINE_ZOOM_STEP) }, height = 28.dp, enabled = zoom > 1f)
            Text(if (zoom > 1.001f) "×${"%.1f".format(zoom)}" else "×1", Modifier.width(36.dp), fontSize = Type.micro, color = p.muted,
                maxLines = 1)
            RectButton("+", { zoomTo(zoom * TIMELINE_ZOOM_STEP) }, height = 28.dp, enabled = zoom < TIMELINE_ZOOM_MAX)
            if (zoom > 1.001f) RectButton("1:1", { zoomTo(1f) }, height = 28.dp)
        }
    }
    Spacer(Modifier.height(4.dp))
    if (pane.lanes.isEmpty()) {
        Box(Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(p.canvas).padding(vertical = 12.dp),
            contentAlignment = Alignment.Center) { Text(pane.noTurnsText, fontSize = Type.meta, color = p.muted) }
        return
    }
    val ticks = remember(pane.axis, zoom) { pane.axis.ticks(zoom) }
    val gaps = pane.axis.gaps
    Row {
        Column(Modifier.width(LaneLabelWidth).padding(top = 18.dp)) {
            pane.lanes.forEach { lane ->
                Row(Modifier.height(LaneHeight), verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Box(Modifier.size(8.dp).clip(RoundedCornerShape(2.dp)).background(p.laneColor(lane.color)))
                    Text(lane.label, fontSize = Type.meta, color = p.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
        BoxWithConstraints(Modifier.weight(1f)) {
            val track = maxWidth * zoom
            Column(Modifier.horizontalScroll(scroll)) {
                // 눈금 행
                Box(Modifier.width(track).height(18.dp)) {
                    ticks.forEach { t ->
                        Row(Modifier.offset(x = track * t.ratio)) {
                            Box(Modifier.width(1.dp).height(18.dp).background(p.divider))
                            Text(t.label, Modifier.padding(start = 2.dp), fontSize = Type.micro, color = p.muted, maxLines = 1,
                                softWrap = false)
                        }
                    }
                }
                Box(Modifier.width(track).height(LaneHeight * pane.lanes.size)) {
                    Column {
                        pane.lanes.forEach { lane -> LaneTrack(lane, p.laneColor(lane.color), act.playTurn) }
                    }
                    // 줄인 틈 — 말이 없어 짧게 접은 구간. 레인 전체 높이의 띠 + 가운데 «⋯».
                    gaps.forEach { g ->
                        Box(Modifier.offset(x = track * g.left).width(track * g.width).fillMaxHeight().background(p.paper)
                                .clickable { onNote(g.tip(pane.turnWord)) },
                            contentAlignment = Alignment.Center) {
                            Box(Modifier.align(Alignment.CenterStart).width(1.dp).fillMaxHeight().background(p.line))
                            Box(Modifier.align(Alignment.CenterEnd).width(1.dp).fillMaxHeight().background(p.line))
                            Text("⋯", fontSize = Type.meta, color = p.muted, maxLines = 1, softWrap = false)
                        }
                    }
                }
            }
        }
    }
}

/**
 * 레인 한 줄 — 턴 막대를 한 번에 그리고(긴 세션은 막대가 수백 개다), 누른 자리의 막대를 찾아 그 발언을 재생한다.
 * 위치는 비율(0~1) × 트랙 폭이다. 너무 좁은 막대는 3dp 로 그리고, 누르는 범위는 손가락에 맞춰 조금 넓게 본다.
 */
@Composable
private fun LaneTrack(lane: LaneUi, color: Color, onPlay: (Turn) -> Unit) {
    val p = Tokens.palette
    Canvas(Modifier.fillMaxWidth().height(LaneHeight).pointerInput(lane) {
        detectTapGestures { at ->
            val w = size.width.toFloat()
            val slack = 6.dp.toPx()
            val hit = lane.bars.lastOrNull { b ->
                val x0 = b.left * w
                at.x >= x0 - slack && at.x <= x0 + maxOf(b.width * w, 3.dp.toPx()) + slack
            }
            if (hit != null && hit.turn.playable) onPlay(hit.turn)
        }
    }) {
        drawRoundRect(p.canvas, topLeft = Offset(0f, 1.dp.toPx()), size = Size(size.width, size.height - 2.dp.toPx()),
            cornerRadius = CornerRadius(3.dp.toPx()))
        val top = 4.dp.toPx()
        val h = size.height - 2 * top
        lane.bars.forEach { b ->
            drawRoundRect(if (b.turn.playable) color else color.copy(alpha = 0.45f),
                topLeft = Offset(b.left * size.width, top),
                size = Size(maxOf(b.width * size.width, 3.dp.toPx()), h), cornerRadius = CornerRadius(2.dp.toPx()))
        }
    }
}

/** 이벤트 한 줄 — 시각 · 색 점 · 누가 · 무엇 · 부가 정보(사유·대기 순번·회수 유예). */
@Composable
private fun EventRow(ev: TimelineItem) {
    val p = Tokens.palette
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(hhmmss(ev.atMs), Modifier.width(64.dp), fontSize = Type.meta, color = p.muted, maxLines = 1)
        Box(Modifier.size(8.dp).clip(CircleShape).background(p.dot(ev.tone)))
        Text(buildAnnotatedString {
            if (ev.who.isNotEmpty()) withStyle(SpanStyle(fontWeight = FontWeight.SemiBold)) { append("${ev.who} ") }
            append(ev.text)
            if (ev.detail.isNotEmpty()) withStyle(SpanStyle(fontSize = Type.micro, color = p.muted)) { append("  ${ev.detail}") }
        }, Modifier.weight(1f).padding(start = 8.dp), fontSize = Type.meta, color = p.ink, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
    }
}
