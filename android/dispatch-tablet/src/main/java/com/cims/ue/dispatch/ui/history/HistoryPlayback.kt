// 녹취 재생 바의 순수 논리 — 막대의 한 점(벽시계 시각) ↔ 세그먼트 + 파일 안 오프셋
// (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6 «녹취 재생 바»)
//
// 막대 하나 = 녹취 전체의 벽시계 구간이다(통화 = 첫 세그먼트 시작~마지막 끝, 무전 = 세션 시간축 — 발언 레인과 같은 축. 틈 줄임과
// 무관하게 시간에 비례한다). 서버는 세그먼트마다 MP4 한 파일을 주므로, 막대의 한 점을 «그 시각을 담은 세그먼트 + 그 안의 오프셋»
// 으로 풀어야 튼다. 세그먼트 사이(말 없는 구간)는 건너뛰기면 다음 세그먼트로 뛰고, 아니면 재생 헤드만 시계로 흘려 다음
// 세그먼트에서 이어 튼다 — 그 판정이 [PlayerBar] 다. 재생기·내려받기는 VM 이 든다.
package com.cims.ue.dispatch.ui.history

import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.RecordingSegment
import kotlin.math.max

/** 막대 위에 놓인 세그먼트 한 개 — 벽시계 구간. */
data class SegSpan(val seg: RecordingSegment, val startMs: Long, val endMs: Long) {
    val seq: Int get() = seg.seq
    /** 이 세그먼트를 틀면 영상 칸이 열린다. */
    val hasVideo: Boolean get() = seg.showsVideo
}

/** 막대 위의 발언 한 구간 — [label] 은 화자 이름, [lane] 은 발언 레인의 색 번호. */
data class TurnSpan(val startMs: Long, val endMs: Long, val label: String, val seq: Int, val lane: Int)

/** 무전 재생 바의 발언 막대 — 바 폭 대비 위치·폭(0~1), 화자 색 번호, 줄(겹치면 0/1 두 줄). */
data class PlayerBlock(val left: Float, val width: Float, val lane: Int, val row: Int)

/** 막대의 한 점을 푼 결과 — 옮겨 갈 시각, 그 시각을 담은 세그먼트(없으면 말 없는 구간·끝), 파일 안 오프셋. */
data class Located(val atMs: Long, val seg: SegSpan?, val offsetMs: Int)

/**
 * 재생 바 한 벌(불변).
 *
 * @param t0 막대의 시작(벽시계 ms) @param spanMs 막대 길이(ms, 1초 이상)
 * @param segs 세그먼트(시작 시각순) @param spans 발언 구간(무전만 — 시작 시각순) @param ptt 무전 세션의 막대인가.
 */
class PlayerBar(
    val t0: Long,
    val spanMs: Long,
    val segs: List<SegSpan>,
    val spans: List<TurnSpan> = emptyList(),
    val ptt: Boolean = false,
) {
    val endMs: Long get() = t0 + spanMs

    fun ratioOf(atMs: Long): Float = ((atMs - t0).toDouble() / spanMs).toFloat().coerceIn(0f, 1f)
    fun timeAt(ratio: Float): Long = t0 + (ratio.coerceIn(0f, 1f).toDouble() * spanMs).toLong()

    /** 그 시각을 담은 세그먼트 — 없으면 말 없는 구간이다. */
    fun segAt(atMs: Long): SegSpan? = segs.firstOrNull { atMs >= it.startMs && atMs < it.endMs }

    fun segOf(seq: Int?): SegSpan? = segs.firstOrNull { it.seq == seq }

    /**
     * 막대의 한 점으로 — 막대 밖은 끝으로 가둔다. 말 없는 구간을 눌렀고 재생하려는 중이며 건너뛰기가 켜져 있으면 **다음 세그먼트의
     * 시작**으로 옮긴다. 그 뒤에도 세그먼트가 없으면(끝·건너뛰기 끔) [Located.seg] 가 null 이다 — 헤드만 그 자리에 선다.
     */
    fun locate(atMs: Long, wantPlay: Boolean, skipGaps: Boolean): Located {
        val g = atMs.coerceIn(t0, endMs)
        val hit = segAt(g)
        if (hit != null) return Located(g, hit, (g - hit.startMs).toInt())
        val next = if (wantPlay && skipGaps) segs.firstOrNull { it.startMs > g } else null
        return if (next != null) Located(next.startMs, next, 0) else Located(g, null, 0)
    }

    /** 이 세그먼트 다음에 틀 것 — 더 늦게 시작하는 첫 세그먼트. */
    fun nextAfter(cur: SegSpan): SegSpan? = segs.firstOrNull { it.startMs > cur.startMs && it.seq != cur.seq }

    /** 그 시각 뒤에 세그먼트가 남았는가 — 말 없는 구간에서 재생을 이어 갈지. */
    fun hasAfter(atMs: Long): Boolean = segs.any { it.startMs > atMs }

    /** [이전 발언] — 헤드보다 1초 넘게 앞선 발언 중 가장 늦은 시작. 없으면 막대의 처음. */
    fun prevTurnStart(headMs: Long): Long = spans.map { it.startMs }.filter { it < headMs - 1000 }.maxOrNull() ?: t0

    /** [다음 발언] — 헤드보다 0.5초 넘게 뒤인 발언 중 가장 이른 시작. 없으면 null(그대로). */
    fun nextTurnStart(headMs: Long): Long? = spans.map { it.startMs }.filter { it > headMs + 500 }.minOrNull()

    /** 그 순간 말하고 있는 구간들(동시 발언이면 여럿). */
    fun speakingAt(atMs: Long): List<TurnSpan> = spans.filter { atMs >= it.startMs && atMs < it.endMs }

    /** 조작 줄 끝의 «지금 <이름> · 발언 #n» / «말 없는 구간». */
    fun nowText(headMs: Long): String {
        val now = speakingAt(headMs)
        return if (now.isEmpty()) "말 없는 구간"
        else "지금 ${now.map { it.label }.distinct().joinToString(" + ")} · 발언 #${now.first().seq}"
    }

    /** 막대를 짚은 지점의 말풍선 — "지금 위치 · 실제 시각"(무전은 + 그때 말한 사람). */
    fun hoverText(ratio: Float): String {
        val at = timeAt(ratio)
        val who = if (!ptt) "" else speakingAt(at).map { it.label }.distinct().let {
            if (it.isEmpty()) " · 말 없음" else " · " + it.joinToString(", ")
        }
        return "${mmss(at - t0)} · ${hhmmss(at)}$who"
    }

    /** 무전 막대 위의 발언 막대 — 앞 발언이 끝나기 전에 시작하면(동시 발언) 두 번째 줄. */
    val blocks: List<PlayerBlock> = run {
        var lane0End = Long.MIN_VALUE
        spans.map { t ->
            val row = if (t.startMs < lane0End) 1 else 0
            if (row == 0) lane0End = t.endMs
            PlayerBlock(ratioOf(t.startMs), max(0.004f, ((t.endMs - t.startMs).toDouble() / spanMs).toFloat()), t.lane, row)
        }
    }
}

/**
 * 고른 통화·세션의 막대를 잡는다(순수 함수, 시험 대상) — 세그먼트·발언 레인을 다 만든 뒤.
 *
 * 시작 시각이 없는 세그먼트는 앞 세그먼트의 끝에 이어 붙인다(처음이면 녹취 시작·응답·세션 시작 시각). 무전은 발언 레인이 있으면
 * 세션 시간축을 그대로 쓴다 — 레인과 막대가 같은 축이라 눈으로 맞춰 볼 수 있다.
 */
internal fun playerBarOf(row: HistoryRow, rec: RecordingInfo?, pane: SessionPaneUi? = null,
                         names: HistoryNames = HistoryNames()): PlayerBar {
    val e = row.e
    val first = rec?.startAtMs ?: e.answerAtMs ?: e.startAtMs ?: e.atMs
    var cursor = first
    val segs = rec?.segments.orEmpty().sortedBy { it.seq }.map { sg ->
        val st = sg.startAtMs ?: cursor
        // 길이를 싣지 않은 세그먼트(duration_ms 0)는 끝 시각으로 — 길이 0 으로 두면 재생 막대가 그 세그먼트를 맞히지 못한다
        val en = if (sg.durationMs > 0) st + sg.durationMs else sg.endAtMs?.takeIf { it > st } ?: st
        cursor = en
        SegSpan(sg, st, en)
    }.sortedBy { it.startMs }
    val ptt = e.kind == HistoryKind.PTT
    val axis = pane?.takeIf { ptt && it.lanes.isNotEmpty() }?.axis
    val t0 = axis?.t0 ?: segs.firstOrNull()?.startMs ?: first
    val span = axis?.spanMs?.toLong() ?: if (segs.isNotEmpty()) segs.maxOf { it.endMs } - t0 else 0L
    val laneOf = pane?.lanes?.associate { it.speaker to it.color }.orEmpty()
    val spans = if (ptt && pane != null)
        pane.turns.map { TurnSpan(it.startMs, it.endMs, names.who(it.speaker), it.seq, laneOf[it.speaker] ?: 0) }
    else emptyList()
    return PlayerBar(t0, max(1000L, span), segs, spans, ptt)
}

/** 재생 속도 — 1× · 1.5× · 2×. */
internal val PLAY_SPEEDS = floatArrayOf(1f, 1.5f, 2f)

/**
 * 재생 바가 그리는 값 한 벌 — VM 이 200 ms 마다 헤드만 바꿔 다시 낸다.
 *
 * @param playing 사용자 의도 = 재생 중(파일 받는 중·말 없는 구간도 포함). 일시정지 = false.
 * @param mediaOpen 재생기에 파일이 열려 있다 — [정지] 가 켜진다.
 * @param video 지금 트는 세그먼트에 영상이 있다 — 영상 칸이 열린다.
 */
data class PlayerUi(
    val bar: PlayerBar? = null,
    val headMs: Long = 0,
    val playing: Boolean = false,
    val loading: Boolean = false,
    val mediaOpen: Boolean = false,
    val status: String = "",
    val skipGaps: Boolean = true,
    val speedIndex: Int = 0,
    val video: Boolean = false,
    /** 영상의 가로 ÷ 세로 — 영상 칸이 비율을 지킨다. */
    val videoAspect: Float = 4f / 3f,
) {
    /** 틀 세그먼트가 있다 — 막대와 조작 줄이 보인다. */
    val has: Boolean get() = bar?.segs?.isNotEmpty() == true
    val canPlay: Boolean get() = has && !loading
    val headRatio: Float get() = bar?.ratioOf(headMs) ?: 0f
    /** 지금 위치(mm:ss) / 전체 길이. */
    val posText: String get() = mmss(headMs - (bar?.t0 ?: headMs))
    val lenText: String get() = mmss(bar?.spanMs ?: 0)
    /** 그 순간의 실제 시각. */
    val wallText: String get() = hhmmss(headMs)
    val speaking: Boolean get() = bar?.speakingAt(headMs)?.isNotEmpty() == true
    val nowText: String get() = bar?.nowText(headMs).orEmpty()
}
