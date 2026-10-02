// 무전 세션 패널의 순수 논리 — 발언 턴·시간축(틈 줄임)·화자 레인·참여자·이벤트·숫자 칸
// (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
//
// 자료는 셋에서 온다: 목록 항목(지표·종류·참여자 수) · 세션 상세(`GET /provisioning/history/ptt/{recordingId}` — 참여자·입퇴장·
// floor 중재) · 녹취 메타(세그먼트의 슬롯 트랙 — 발언 턴). 여기서는 그것을 화면 값으로 바꾸기만 한다. 규칙의 정본은 Windows
// `SessionHistoryViewModel.BuildPttPane`·`BuildAxisMap`·`ApplyTimelineLayers` 다.
package com.cims.ue.dispatch.ui.history

import com.cims.ue.dispatch.session.PttFloorEvent
import com.cims.ue.dispatch.session.PttSessionDetail
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.userPart
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

// ── 발언 턴 ─────────────────────────────────────────────────────────────────

/**
 * 발언 턴 — 한 화자가 한 슬롯을 점유한 구간(벽시계 ms). [multi] = 세그먼트에 턴이 여럿(동시 발언·슬롯 재사용).
 * 영상 세션에서는 «송출» 한 번이다.
 */
data class Turn(
    val seq: Int,
    val slot: Int,
    val speaker: String,
    val startMs: Long,
    val endMs: Long,
    val playable: Boolean = true,
    val multi: Boolean = false,
) {
    val durMs: Int get() = (endMs - startMs).toInt()
}

/**
 * 녹취 세그먼트 → 발언 턴(순수 함수, 시험 대상). 턴의 원자는 **슬롯 트랙의 화자 구간**(`tracks[].speakers[]`)이다 — 동시 발언이면
 * 같은 시각에 슬롯이 여럿이라 턴도 여럿이다. 트랙이 없는 세그먼트는 세그먼트 전체가 대표 화자의 한 턴이다.
 *
 * 화자 구간은 **음성 트랙**에서 읽는다. 음성 없이 영상만 있는 송출 구간(MCVideo — 송출 중 무전으로 마이크를 넘긴 동안)은 영상
 * 트랙에서 읽는다. 시작 시각이 없는 세그먼트는 축에 놓을 수 없어 뺀다.
 */
internal fun turnsOf(rec: RecordingInfo): List<Turn> {
    val out = ArrayList<Turn>()
    rec.segments.forEach { seg ->
        val base = seg.startAtMs ?: return@forEach
        val playable = seg.status != "recording"
        val tracks = seg.tracks.filter { it.kind == "audio" }.ifEmpty { seg.tracks.filter { it.kind == "video" } }
        val mine = ArrayList<Turn>()
        if (tracks.isEmpty()) mine.add(Turn(seg.seq, 0, seg.speakerId, base, base + seg.durationMs, playable))
        else tracks.forEach { t ->
            val ok = playable && t.status != "recording"
            if (t.speakers.isEmpty()) mine.add(Turn(seg.seq, t.slot, seg.speakerId, base, base + seg.durationMs, ok))
            else t.speakers.forEach { sp ->
                mine.add(Turn(seg.seq, t.slot, sp.id.ifEmpty { seg.speakerId }, base + sp.offsetMs,
                    base + sp.offsetMs + sp.durMs, ok))
            }
        }
        val multi = mine.size > 1
        mine.forEach { out.add(if (multi) it.copy(multi = true) else it) }
    }
    return out.sortedWith(compareBy({ it.startMs }, { it.slot }))
}

// ── 시간축 ──────────────────────────────────────────────────────────────────

/** 타임라인 배율 상한 — 데스크톱 `TalkZoomMax` 와 같다(1시간 세션에서 한 턴의 1초가 막대로 보이는 정도). */
internal const val TIMELINE_ZOOM_MAX = 64f

/** 한 번 누를 때의 배율 — 데스크톱 휠 한 칸과 같다. */
internal const val TIMELINE_ZOOM_STEP = 1.25f

/**
 * 눈금 간격(초) — 배율에 따라 대여섯 개가 보이게 1·2·5·10·15·30초·1·2·5·10·15·30분·1시간 중에서 고른다(데스크톱
 * `RebuildAxisTicks` 와 같은 규칙). 순수 함수(시험 대상).
 */
internal fun axisStepSec(spanMs: Int, zoom: Float): Int {
    val target = spanMs / (6.0 * zoom.coerceAtLeast(1f))
    val steps = intArrayOf(1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600)
    return steps.firstOrNull { it * 1000.0 >= target } ?: steps.last()
}

/** 눈금 — 트랙 폭 대비 위치(0~1)와 시각 표기. */
data class AxisTick(val ratio: Float, val label: String)

/** 줄인 틈 — 발언이 없어 짧게 접은 구간(트랙 폭 대비 위치·폭)과 실제 구간(벽시계 ms). */
data class AxisGap(val left: Float, val width: Float, val fromMs: Long, val toMs: Long) {
    /** 누르면 보이는 안내 — 실제 시각·길이. */
    fun tip(word: String): String =
        "$word 없음 ${hhmmss(fromMs)} ~ ${hhmmss(toMs)} · ${fmtSpeech((toMs - fromMs).toInt())} — 줄여서 표시"
}

/**
 * 발언 타임라인의 시간축 사상 — 실제 구간(세션 시작 기준 ms) → 표시 구간.
 *
 * **틈 줄임**이 켜져 있으면 발언이 없는 구간(세션 머리·꼬리 포함)을 상한 길이로 접는다 — 상한 = 발언 시간 합의 5 %(1.5~15초).
 * 그보다 짧은 틈과 발언 구간은 시간에 비례한 그대로다(막대 길이끼리는 늘 비교된다). 꺼져 있으면 한 구간(선형)이다.
 *
 * 위치는 전부 **비율(0~1)** 로 낸다 — 폭(dp)에 ms 를 곱하는 정수 셈이 없어 긴 세션에서도 넘치지 않는다.
 */
class TimeAxis private constructor(
    /** 축의 시작(벽시계 ms). */
    val t0: Long,
    /** 축이 덮는 실제 길이(ms, 1초 이상). */
    val spanMs: Double,
    private val pieces: List<Piece>,
    private val dispMs: Double,
) {
    internal data class Piece(val r0: Double, val r1: Double, val d0: Double, val d1: Double, val gap: Boolean)

    val endMs: Long get() = t0 + spanMs.toLong()

    /** 틈을 하나라도 접었는가 — 접었으면 축이 시간에 비례하지 않아 눈금을 다르게 적는다. */
    val compacted: Boolean get() = pieces.any { it.gap }

    val gaps: List<AxisGap> = pieces.filter { it.gap }.map {
        AxisGap((it.d0 / dispMs).toFloat(), ((it.d1 - it.d0) / dispMs).toFloat(), t0 + it.r0.toLong(), t0 + it.r1.toLong())
    }

    /** 시각 → 트랙 폭 대비 위치(0~1). 축 밖은 끝으로 가둔다. */
    fun ratioOf(atMs: Long): Float {
        val ms = (atMs - t0).toDouble().coerceIn(0.0, spanMs)
        for (m in pieces) if (ms <= m.r1)
            return ((m.d0 + (if (m.r1 > m.r0) (ms - m.r0) / (m.r1 - m.r0) * (m.d1 - m.d0) else 0.0)) / dispMs).toFloat().coerceIn(0f, 1f)
        return 1f
    }

    /** 트랙 폭 대비 위치(0~1) → 시각 — [ratioOf] 의 역(줄인 틈 안이면 그 틈의 실제 구간에 비례). */
    fun timeAt(ratio: Float): Long {
        val d = ratio.coerceIn(0f, 1f) * dispMs
        for (m in pieces) if (d <= m.d1)
            return t0 + (m.r0 + (if (m.d1 > m.d0) (d - m.d0) / (m.d1 - m.d0) * (m.r1 - m.r0) else 0.0)).toLong()
        return endMs
    }

    /**
     * 눈금 — 선형 축은 배율에 맞춘 고른 간격([axisStepSec])을 벽시계의 그 간격 배수에 맞춰 적는다. 틈을 줄인 축은 시간에 비례하지
     * 않으므로 고른 간격 대신 **접힌 틈 뒤 발언 덩어리가 시작하는 시각**을 적는다(너무 붙은 것은 건너뛴다).
     */
    fun ticks(zoom: Float, zone: ZoneId = ZoneId.systemDefault()): List<AxisTick> {
        val out = ArrayList<AxisTick>()
        fun clock(ms: Long, pattern: DateTimeFormatter) = Instant.ofEpochMilli(ms).atZone(zone).format(pattern)
        if (compacted) {
            var last = -1.0
            val minStep = 0.11 / zoom.coerceAtLeast(1f)
            for (m in pieces) {
                if (m.gap || m.r1 - m.r0 <= 0) continue
                val r = m.d0 / dispMs
                val afterGap = pieces.any { it.gap && abs(it.r1 - m.r0) < 0.5 }
                if (!afterGap && last >= 0) continue                 // 덩어리의 첫 눈금만 — 줄인 틈 바로 뒤(또는 맨 처음)
                if (last >= 0 && r - last < minStep) continue
                out.add(AxisTick(r.toFloat(), clock(t0 + m.r0.toLong(), TICK_SEC)))
                last = r
            }
            return out
        }
        val stepSec = axisStepSec(spanMs.toInt(), zoom)
        val offsetSec = zone.rules.getOffset(Instant.ofEpochMilli(t0)).totalSeconds
        val localSec = Math.floorDiv(t0, 1000L) + offsetSec
        var at = (Math.floorDiv(localSec, stepSec.toLong()) + 1) * stepSec - offsetSec          // 벽시계의 간격 배수
        while ((at * 1000 - t0) < spanMs && out.size < MAX_TICKS) {
            out.add(AxisTick(((at * 1000 - t0) / spanMs).toFloat(), clock(at * 1000, if (stepSec >= 60) TICK_MIN else TICK_SEC)))
            at += stepSec
        }
        return out
    }

    companion object {
        /** 겹치거나 이 안에 이어진 발언은 한 덩어리로 본다(ms). */
        private const val MERGE_MS = 300.0
        /** 눈금 상한 — 긴 세션을 크게 확대해도 수천 개를 그리지 않게. */
        private const val MAX_TICKS = 400
        private val TICK_SEC: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")
        private val TICK_MIN: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm")

        /** 접는 상한(ms) — 발언 시간 합의 5 %, 1.5~15초. */
        internal fun gapCapMs(speechMs: Double): Double = (speechMs * 0.05).coerceIn(1500.0, 15_000.0)

        /**
         * @param t0 축의 시작(벽시계 ms) @param spanMs 축 길이(ms) @param turns 발언 구간(벽시계 ms 시작·끝)
         * @param compact 틈 줄임.
         */
        fun build(t0: Long, spanMs: Double, turns: List<Pair<Long, Long>>, compact: Boolean): TimeAxis {
            val span = max(1000.0, spanMs)
            val blocks = ArrayList<DoubleArray>()
            turns.map { doubleArrayOf((it.first - t0).toDouble().coerceIn(0.0, span), (it.second - t0).toDouble().coerceIn(0.0, span)) }
                .filter { it[1] > it[0] }.sortedBy { it[0] }
                .forEach { b ->
                    val lastBlock = blocks.lastOrNull()
                    if (lastBlock != null && b[0] <= lastBlock[1] + MERGE_MS) lastBlock[1] = max(lastBlock[1], b[1])
                    else blocks.add(b)
                }
            if (!compact || blocks.isEmpty()) return TimeAxis(t0, span, listOf(Piece(0.0, span, 0.0, span, false)), span)

            val cap = gapCapMs(blocks.sumOf { it[1] - it[0] })
            val pieces = ArrayList<Piece>()
            var cur = 0.0
            var disp = 0.0
            fun gap(until: Double) {
                val g = until - cur
                if (g <= 0) return
                val shown = min(g, cap)
                pieces.add(Piece(cur, until, disp, disp + shown, g > cap))
                disp += shown; cur = until
            }
            blocks.forEach { b ->
                gap(b[0])
                pieces.add(Piece(b[0], b[1], disp, disp + (b[1] - b[0]), false))
                disp += b[1] - b[0]; cur = b[1]
            }
            gap(span)
            return TimeAxis(t0, span, pieces, max(1.0, disp))
        }
    }
}

// ── 세션 패널 ───────────────────────────────────────────────────────────────

/** 숫자 칸 하나 — [hint] 는 누르면 보이는 풀이(무엇을 센 건지). */
data class Metric(val key: String, val value: String, val suffix: String = "", val hint: String = "")

/**
 * 참여자 한 줄 — 입퇴장 기록 ∪ 화자(녹취 턴). 발언 통계는 턴에서 센다(참가만 한 사람은 0).
 * [lane] = 발언 레인의 색 번호(말하지 않았으면 -1).
 */
data class ParticipantUi(
    val id: String,
    val label: String,
    val initiator: Boolean,
    /** 입장 ~ 퇴장. 기록이 없으면 "". */
    val range: String,
    val turns: Int,
    val speech: String,
    val lane: Int,
    /** 세는 말 — 무전 "발언" · 영상 세션 "송출". */
    val word: String = "발언",
) {
    val spoke: Boolean get() = lane >= 0
    /** 누르면 보이는 풀이 — 입장~퇴장 · 발언 n회 · 말한 시간. */
    val tip: String get() = (if (range.isNotEmpty()) "$range · " else "") +
        "$word ${turns}회 · ${if (word == "송출") "보낸 시간" else "말한 시간"} $speech"
}

/** 턴 막대 — 레인 폭 대비 위치·폭(0~1). 누르면 그 발언부터 재생한다. */
data class TurnBarUi(val turn: Turn, val left: Float, val width: Float)

/** 화자 레인 — [color] 는 등장 순서로 배정한 색 번호다. */
data class LaneUi(val speaker: String, val label: String, val color: Int, val bars: List<TurnBarUi>)

/** 이벤트 점의 색조 — 화면이 토큰으로 푼다. */
enum class DotTone { TALK, EMG, HELD, RING, MON, LISTEN, GRAY }

/** 이벤트 타임라인 한 줄 — floor 중재(TS 24.380 op) 또는 입퇴장. [detail] 은 op 별 부가 정보(사유·대기 순번·회수 유예). */
data class TimelineItem(val atMs: Long, val floor: Boolean, val tone: DotTone, val who: String, val text: String,
                        val detail: String = "")

/**
 * 무전 세션 패널이 그리는 값 한 벌. 세션 상세·녹취가 오기 전에도 목록 항목만으로 머리와 숫자 칸은 선다.
 *
 * @param video 영상 세션(MCVideo 그룹 호) — 같은 패널을 낱말만 바꿔 쓴다(발언 → 송출).
 */
data class SessionPaneUi(
    val row: HistoryRow,
    val video: Boolean = false,
    val metrics: List<Metric> = emptyList(),
    val participants: List<ParticipantUi> = emptyList(),
    val lanes: List<LaneUi> = emptyList(),
    val axis: TimeAxis = TimeAxis.build(row.e.axisAtMs, 1000.0, emptyList(), false),
    val turns: List<Turn> = emptyList(),
    /** 이벤트 전부(시간순) — 층 토글·같은 초 묶기는 [timelineShown]. */
    val timeline: List<TimelineItem> = emptyList(),
    val floorCount: Int = 0,
    val memberEventCount: Int = 0,
    val segmentCount: Int = 0,
    /** 세션 상세 조회의 상태 — "세션 상세 조회 중…"·실패 사유·"세션 기록 없음". */
    val status: String = "",
) {
    val turnWord: String get() = if (video) "송출" else "발언"
    val noTurnsText: String get() = if (video) "이 세션에는 녹취된 송출이 없습니다" else "이 세션에는 녹취된 발언이 없습니다"
    val axisStartText: String get() = hhmmss(axis.t0)
    val axisEndText: String get() = hhmmss(axis.endMs)
}

/**
 * 세션 패널 만들기(순수 함수, 시험 대상) — 발언 턴(세그먼트 슬롯 트랙) · 시간축 · 화자 레인 · 참여자 · 이벤트 · 숫자 칸.
 *
 * 시간축 = 세션 시작~종료(항목) ∪ 턴·이벤트가 실제 걸친 범위. 화자 색은 턴에 처음 나온 차례다.
 */
internal fun buildSessionPane(
    row: HistoryRow,
    detail: PttSessionDetail?,
    rec: RecordingInfo?,
    compact: Boolean = true,
    names: HistoryNames = HistoryNames(),
    status: String = "",
): SessionPaneUi {
    val e = row.e
    val who = names.who
    val video = e.isMcVideo || rec?.isMcVideo == true
    val word = if (video) "송출" else "발언"
    val turns = rec?.let(::turnsOf).orEmpty()
    val order = turns.map { it.speaker }.filter { it.isNotEmpty() }.distinct()
    val parts = detail?.participants.orEmpty()
    val events = detail?.events.orEmpty()
    val floor = detail?.floor.orEmpty()

    val times = ArrayList<Long>()
    e.startAtMs?.let(times::add)
    e.endAtMs?.let(times::add)
    turns.forEach { times.add(it.startMs); times.add(it.endMs) }
    events.mapNotNullTo(times) { it.atMs }
    floor.mapNotNullTo(times) { it.atMs }
    val t0 = times.minOrNull() ?: e.atMs
    val t1 = times.maxOrNull() ?: e.atMs
    val axis = TimeAxis.build(t0, max(1000.0, (t1 - t0).toDouble()), turns.map { it.startMs to it.endMs }, compact)

    val lanes = order.mapIndexed { i, spk ->
        LaneUi(spk, who(spk), i, turns.filter { it.speaker == spk }.map { t ->
            val left = axis.ratioOf(t.startMs)
            TurnBarUi(t, left, max(0.002f, axis.ratioOf(t.endMs) - left))
        })
    }

    // 참여자 = 입퇴장 기록 ∪ 화자
    val ids = parts.map { it.msisdn }.toMutableList()
    order.forEach { spk -> if (ids.none { sameUser(it, spk) }) ids.add(spk) }
    val participants = ids.map { id ->
        val p = parts.firstOrNull { it.msisdn == id }
        val mine = turns.filter { sameUser(it.speaker, id) }
        val range = if (p != null && (p.joinAtMs != null || p.leaveAtMs != null))
            "${hhmmss(p.joinAtMs)} ~ ${p.leaveAtMs?.let(::hhmmss) ?: if (row.live) "참여중" else "—"}" else ""
        ParticipantUi(id, who(id), p?.role == "initiator" || (p == null && sameUser(id, e.from)), range,
            mine.size, fmtSpeech(mine.sumOf { it.durMs }), order.indexOfFirst { sameUser(it, id) }, word)
    }

    // 이벤트 — floor 중재 + 입퇴장, 시간순
    val timeline = ArrayList<TimelineItem>()
    floor.forEach { f ->
        val at = f.atMs ?: return@forEach
        timeline.add(TimelineItem(at, true, floorTone(f.op), if (f.user.isNotEmpty()) who(f.user) else "",
            floorOpText(f.op), floorDetail(f)))
    }
    events.forEach { ev ->
        val at = ev.atMs ?: return@forEach
        val initiator = ev.type == "member_join" && ev.role == "initiator"
        val detailText = (if (initiator) "개시자" else "") +
            (ev.durationSec?.let { (if (initiator) " · " else "") + fmtDur(it) } ?: "")
        timeline.add(TimelineItem(at, false, eventTone(ev.type), if (ev.member.isNotEmpty()) who(ev.member) else "",
            eventTypeText(ev.type), detailText))
    }
    timeline.sortBy { it.atMs }                                   // 안정 정렬 — 같은 시각이면 floor 가 앞

    // 숫자 칸 — 관제 사람이 먼저 묻는 넷(얼마나·몇 명·몇 번·얼마 동안) + 있을 때만 동시 발언·녹취
    val talkMs = if (e.talkMs > 0) e.talkMs else turns.sumOf { it.durMs }
    val count = (if (e.turnCount > 0) e.turnCount else turns.size).toString()
    val metrics = ArrayList<Metric>()
    metrics.add(Metric("길이", if (row.live) "진행 중" else fmtDur(row.durSec), hint = "세션 시작~종료"))
    metrics.add(Metric("참여", participants.size.toString(), "명",
        if (video) "입퇴장 기록과 영상을 보낸 사람을 합친 수" else "입퇴장 기록과 말한 사람을 합친 수"))
    if (video) {
        metrics.add(Metric("송출", count, "회", "영상을 보낸 구간 수 — 동시에 보낸 사람은 따로 센다 (TS 24.581 전송 제어)"))
        metrics.add(Metric("보낸 시간", fmtSpeech(if (e.totalSpeechMs > 0) e.totalSpeechMs else talkMs),
            hint = "겹친 구간은 한 번으로 센 송출 시간 · 사람별 합 ${fmtSpeech(talkMs)}"))
        if (e.maxConcurrent > 1) metrics.add(Metric("최대 동시 송출", e.maxConcurrent.toString(), "명"))
    } else {
        metrics.add(Metric("발언", count, "회", "말한 구간 수 — 동시 발언은 사람마다 따로 센다"))
        metrics.add(Metric("말한 시간", fmtSpeech(e.totalSpeechMs),
            hint = "겹친 구간은 한 번으로 센 무전 점유 시간 · 사람별 합 ${fmtSpeech(talkMs)}"))
        if (e.maxConcurrent > 1) metrics.add(Metric("최대 동시 발언", e.maxConcurrent.toString(), "명"))
    }
    val segments = rec?.segments?.size ?: 0
    if (segments > 0) metrics.add(Metric("녹취", segments.toString(), "개", "녹취 세그먼트 수"))

    return SessionPaneUi(row, video, metrics, participants, lanes, axis, turns, timeline,
        floorCount = floor.size, memberEventCount = events.size, segmentCount = segments,
        status = status.ifEmpty { if (detail == null && e.recordingId.isEmpty()) "세션 기록 없음" else "" })
}

/**
 * 이벤트 층 거르기 + 같은 초 묶기(순수 함수, 시험 대상) — 같은 초에 같은 입퇴장이 몰리면(그룹 통화 개시·해제) 한 줄로
 * "테스트003 외 3명 입장".
 */
internal fun timelineShown(all: List<TimelineItem>, floor: Boolean, member: Boolean): List<TimelineItem> {
    val out = ArrayList<TimelineItem>()
    var run: TimelineItem? = null
    var more = 0
    fun flush() {
        val r = run ?: return
        out.add(if (more > 0) r.copy(who = "${r.who} 외 ${more}명") else r)
        run = null; more = 0
    }
    for (item in all) {
        if (!(if (item.floor) floor else member)) continue
        val r = run
        if (r != null && !item.floor && !r.floor && item.text == r.text && item.detail.isEmpty() && r.detail.isEmpty() &&
            item.who.isNotEmpty() && r.who.isNotEmpty() && item.atMs / 1000 == r.atMs / 1000) { more++; continue }
        flush()
        run = item
    }
    flush()
    return out
}

// ── 표시 사전(콘솔 PTT 이력과 같은 문구) ────────────────────────────────────

/** floor 중재 op 8종(TS 24.380). */
internal fun floorOpText(op: String): String = when (op.uppercase()) {
    "GRANT" -> "발언권 부여"
    "RELEASE" -> "발언 종료"
    "IDLE" -> "유휴"
    "REVOKE" -> "회수 통지"
    "REVOKE_END" -> "회수 확정"
    "QUEUE" -> "대기열 등록"
    "QUEUE_CANCEL" -> "대기 취소"
    "DENY" -> "거절"
    else -> op
}

internal fun floorTone(op: String): DotTone = when (op.uppercase()) {
    "GRANT" -> DotTone.TALK
    "REVOKE" -> DotTone.RING
    "REVOKE_END", "DENY" -> DotTone.EMG
    "QUEUE" -> DotTone.LISTEN
    else -> DotTone.GRAY
}

internal fun eventTypeText(t: String): String = when (t) {
    "session_start" -> "세션 시작"
    "session_end" -> "세션 종료"
    "member_join" -> "입장"
    "member_leave" -> "퇴장"
    "floor-grant" -> "발언 시작"
    "floor-release" -> "발언 종료"
    "member_invite" -> "초대"
    "config_change" -> "설정 변경"
    else -> t
}

internal fun eventTone(t: String): DotTone = when (t) {
    "session_start", "floor-grant" -> DotTone.TALK
    "session_end" -> DotTone.EMG
    "member_join" -> DotTone.HELD
    "member_leave" -> DotTone.RING
    "config_change" -> DotTone.MON
    "member_invite" -> DotTone.LISTEN
    else -> DotTone.GRAY
}

/** 거절 사유(CMP floor 기록의 `reason`). */
internal fun denyReasonText(r: String): String = when (r) {
    "recv_only" -> "수신전용(ambient)"
    "only_one" -> "참가자 1인"
    "broadcast" -> "broadcast 비개시자"
    "no_permission" -> "권한 없음"
    "busy" -> "다른 발언자"
    "queue_full" -> "대기열 가득참"
    "not_affiliated" -> "미참가"
    else -> r
}

/** op 별 부가 정보 — 규격 용어(TS 24.380)로 사유·대기 순번·회수 유예·선점을 펼친다. */
internal fun floorDetail(f: PttFloorEvent): String {
    val p = ArrayList<String>()
    when (f.op.uppercase()) {
        "GRANT" -> {
            if (f.preempt) p.add(if (f.preemptedFrom.isNotEmpty()) "선점 — ${userPart(f.preemptedFrom)} 회수" else "선점")
            val talkers = f.talkers ?: 0
            if (talkers > 1) p.add("동시 ${talkers}명")
            if (f.slot != null && talkers > 1) p.add("슬롯 ${f.slot}")
            if ((f.prio ?: 0) > 0) p.add("우선순위 ${f.prio}")
        }
        "DENY" -> {
            p.add(denyReasonText(f.reason))
            if (f.owner.isNotEmpty()) p.add("발언 중 ${userPart(f.owner)}")
            if (f.cause != null) p.add("cause ${f.cause}")
        }
        "QUEUE" -> f.pos?.let { pos -> p.add(if (f.qsize != null) "대기 $pos/${f.qsize}" else "대기 ${pos}번") }
        "QUEUE_CANCEL" -> {
            if (f.revoked.isNotEmpty()) p.add("${userPart(f.revoked)} 대기 해제")
            if ((f.removed ?: 0) > 0) p.add("${f.removed}건 제거")
        }
        "REVOKE" -> {
            f.graceSec?.let { p.add("${it}초 후 회수") }
            if (f.preemptedBy.isNotEmpty()) p.add("선점자 ${userPart(f.preemptedBy)}")
        }
        "REVOKE_END" -> if (f.preemptedBy.isNotEmpty()) p.add("선점자 ${userPart(f.preemptedBy)}")
        "RELEASE", "IDLE" -> if ((f.idleMs ?: 0) > 0) p.add("무음 ${fmtSpeech(f.idleMs ?: 0)}")
    }
    return p.filter { it.isNotEmpty() }.joinToString(" · ")
}
