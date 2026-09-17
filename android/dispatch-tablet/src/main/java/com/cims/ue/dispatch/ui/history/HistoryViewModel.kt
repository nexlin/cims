// [이력] 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.5, dispatch_desktop_ui.md §4.6)
//
// 끝난 통화·PTT 세션의 **날짜 창 조회 + 녹취 재생**. 진행 중·오늘의 실시간 흐름은 관제 ①②⑤⑥ 가 정본이고
// 여기는 서버가 보관한 사본을 본다. 조회 축(종류·날짜·시간대·검색)은 전부 이 VM 이 갖고, 걸러 내는 규칙은
// 순수 함수로 빼 둔다 — 기기 없이 시험한다(§9 S1-UE-TABLET-UNIT).
package com.cims.ue.dispatch.ui.history

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.PttSessionDetail
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.session.TurnBar
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.File
import java.time.LocalDate
import java.time.ZoneId

/** 재생 상태 — 화면은 이 값만 보고 버튼을 그린다. */
data class PlaybackState(
    val label: String = "",
    val busy: Boolean = false,
    val note: String = "",
    val error: String = "",
)

class HistoryViewModel(
    private val s: DispatchSession,
    private val cacheDir: File,
    private val player: SegmentPlayer = SegmentPlayer(),
) : ScreenViewModel() {

    private val _kind = MutableStateFlow(HistoryKind.CALL)
    val kind: StateFlow<HistoryKind> = _kind.asStateFlow()

    private val _date = MutableStateFlow(LocalDate.now())
    val date: StateFlow<LocalDate> = _date.asStateFlow()

    private val _query = MutableStateFlow("")
    val query: StateFlow<String> = _query.asStateFlow()

    /** 시간대 필터 — null 이면 전체. 종류·날짜가 바뀌면 풀린다(§4.6). */
    private val _hourFilter = MutableStateFlow<Int?>(null)
    val hourFilter: StateFlow<Int?> = _hourFilter.asStateFlow()

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading.asStateFlow()

    private val _error = MutableStateFlow("")
    val error: StateFlow<String> = _error.asStateFlow()

    /** 조회 결과 원본(최근이 위). 화면이 보는 것은 [rows]. */
    private val _all = MutableStateFlow<List<HistoryEntry>>(emptyList())

    private val _rows = MutableStateFlow<List<HistoryEntry>>(emptyList())
    val rows: StateFlow<List<HistoryEntry>> = _rows.asStateFlow()

    /** 시간대 밴드 24칸 — 서버 `hours` 가 정본이고 없으면 항목에서 센다. */
    private val _band = MutableStateFlow(IntArray(24))
    val band: StateFlow<IntArray> = _band.asStateFlow()

    private val _selected = MutableStateFlow<HistoryEntry?>(null)
    val selected: StateFlow<HistoryEntry?> = _selected.asStateFlow()

    private val _detail = MutableStateFlow<PttSessionDetail?>(null)
    val detail: StateFlow<PttSessionDetail?> = _detail.asStateFlow()

    private val _recording = MutableStateFlow<RecordingInfo?>(null)
    val recording: StateFlow<RecordingInfo?> = _recording.asStateFlow()

    /** 발언 타임라인 막대 — 녹취 세그먼트의 화자 구간에서 만든다. */
    private val _turns = MutableStateFlow<List<TurnBar>>(emptyList())
    val turns: StateFlow<List<TurnBar>> = _turns.asStateFlow()

    private val _playback = MutableStateFlow(PlaybackState())
    val playback: StateFlow<PlaybackState> = _playback.asStateFlow()

    private var queryJob: Job? = null
    private var detailJob: Job? = null
    private var playJob: Job? = null

    /** 관제 역할이 없으면 화면이 잠긴다 — 서버가 403 을 내기 전에 앱이 먼저 접는다(§6.5). */
    val available: Boolean get() = s.hasDesk

    // ── 축 ────────────────────────────────────────────────────────────────────

    fun show(k: HistoryKind) {
        if (_kind.value == k) return
        _kind.value = k
        _hourFilter.value = null
        clearSelection()
        load(hour = null)
    }

    fun showDate(d: LocalDate) {
        val capped = if (d.isAfter(LocalDate.now())) LocalDate.now() else d   // 미래로는 못 간다
        if (_date.value == capped) return
        _date.value = capped
        _hourFilter.value = null
        clearSelection()
        load(hour = null)
    }

    fun shiftDay(days: Long) = showDate(_date.value.plusDays(days))
    fun today() = showDate(LocalDate.now())

    fun search(q: String) { _query.value = q; project() }

    /**
     * 밴드 칸 클릭 — **그 시간대를 서버에 다시 묻는다.**
     *
     * 지역 필터로만 좁히면 앞 시간대가 영영 안 보인다: 서버는 창 안에서 **최근 `limit`(1000) 건만**
     * 주는데 `hours` 는 **절삭 전 전체**로 낸다(csc `dispatch_history.finish_rows`). 그래서 밴드에는
     * 건수가 뜨는데 눌러 보면 빈 목록이고, 잘려 나간 이력과 그 녹취에 닿을 길이 없다.
     * 창을 그 한 시간으로 좁혀 다시 물으면 1000건 상한 안에 들어온다.
     */
    fun toggleHour(h: Int) {
        if (_band.value.getOrNull(h)?.let { it <= 0 } != false) return
        val next = if (_hourFilter.value == h) null else h
        _hourFilter.value = next
        load(hour = next)
    }

    fun clearHour() {
        if (_hourFilter.value == null) return
        _hourFilter.value = null
        load(hour = null)
    }

    // ── 조회 ──────────────────────────────────────────────────────────────────

    /** 조회 결과가 서버 상한에 걸려 **잘렸는가** — 그러면 화면이 전부를 보여 주지 못한다. */
    private val _truncated = MutableStateFlow(false)
    val truncated: StateFlow<Boolean> = _truncated.asStateFlow()

    /**
     * 조회. [hour] 가 있으면 그 한 시간만, 없으면 하루 전체.
     *
     * 밴드(시간대 분포)는 **하루 전체 조회에서만** 갱신한다 — 한 시간 창의 응답에는 그 칸만 들어 있어
     * 그대로 쓰면 나머지 칸이 사라진다.
     */
    fun load(hour: Int? = _hourFilter.value) {
        val m = s.management() ?: return run { _error.value = "로그인 전" }
        queryJob?.cancel()
        _error.value = ""
        _loading.value = true
        val (dayFrom, dayTo) = dayWindow(_date.value)
        val from = if (hour == null) dayFrom else dayFrom + hour * 3_600_000L
        val to = if (hour == null) dayTo else (dayFrom + (hour + 1) * 3_600_000L).coerceAtMost(dayTo)
        val k = _kind.value
        queryJob = scope.launch {
            val r = m.history(k, from, to)
            _loading.value = false
            if (!r.ok) {
                _error.value = r.reason
                _all.value = emptyList()
                if (hour == null) _band.value = IntArray(24)
                _truncated.value = false
                project()
                return@launch
            }
            val page = r.value!!
            _all.value = page.items.sortedByDescending { it.atMs }        // 최근이 위
            if (hour == null) _band.value = bandOf(page.items, page.hours)
            // 상한에 닿았으면 잘린 것이다 — 조용히 일부만 보여 주지 않는다.
            _truncated.value = page.items.size >= QUERY_LIMIT
            project()
        }
    }

    private fun project() {
        val q = _query.value.trim()
        // 시간대는 **서버가 창으로 좁혀 준다** — 여기서 또 거르면 경계(축 시각 vs 서버 기준)에서
        // 어긋나 방금 받은 행이 사라진다. 검색만 지역에서 한다.
        _rows.value = _all.value.filter { matches(it, q) }
        // 고른 세션이 걸러졌으면 선택을 놓는다 — 빈 패널이 남지 않게.
        _selected.value?.let { sel -> if (_rows.value.none { it.id == sel.id }) clearSelection() }
    }

    fun clearSelection() {
        detailJob?.cancel()
        _selected.value = null
        _detail.value = null
        _recording.value = null
        _turns.value = emptyList()
        stop()
    }

    /** 행 선택 — PTT 는 세션 상세·녹취를, 통화는 녹취만 읽는다(통화는 한 줄이 곧 상세, §4.6). */
    fun select(e: HistoryEntry) {
        if (_selected.value?.id == e.id) return
        detailJob?.cancel()
        stop()
        _selected.value = e
        _detail.value = null
        _recording.value = null
        _turns.value = emptyList()
        val m = s.management() ?: return
        val recId = e.recordingId
        detailJob = scope.launch {
            if (e.kind == HistoryKind.PTT && recId.isNotBlank()) {
                m.pttSessionDetail(recId).let { if (it.ok) _detail.value = it.value }
            }
            if (e.hasRecording && recId.isNotBlank()) {
                val r = m.recording(recId)
                if (r.ok) {
                    _recording.value = r.value
                    _turns.value = turnsOf(r.value!!)
                } else _error.value = r.reason
            }
        }
    }

    // ── 녹취 재생 ─────────────────────────────────────────────────────────────

    /** 세그먼트 하나 재생. `slot` 이 있으면 단독 발언자 트랙, 없으면 믹스. */
    fun play(seq: Int, slot: Int? = null, label: String = "", retry: Boolean = false) {
        val m = s.management() ?: return
        val rec = _recording.value ?: return
        playJob?.cancel()
        player.stop()
        _playback.value = PlaybackState(label = label, busy = true)
        playJob = scope.launch {
            val r = m.fetchSegment(recDir(), rec.id, seq, slot, retry) { note ->
                _playback.value = _playback.value.copy(note = note)
            }
            if (!r.ok) {
                _playback.value = PlaybackState(error = r.reason)
                return@launch
            }
            val started = player.play(r.value!!) { _playback.value = PlaybackState() }
            _playback.value =
                if (started) PlaybackState(label = label)
                else PlaybackState(error = "재생할 수 없습니다 — 파일이 손상됐을 수 있습니다")
            // 감청·재생은 서버 감사(E-AUD-016)가 정본이고 앱은 ⑤⑥ 줄에만 남긴다.
            if (started) notePlayback(label)
        }
    }

    /** [▶ 전체] — 재생 가능한 세그먼트를 순서대로. */
    fun playAll() {
        val rec = _recording.value ?: return
        val first = rec.segments.firstOrNull { it.playable } ?: return
        playSequence(rec.segments.filter { it.playable }.map { it.seq }, 0, first.seq)
    }

    private fun playSequence(seqs: List<Int>, index: Int, seq: Int) {
        val m = s.management() ?: return
        val rec = _recording.value ?: return
        playJob?.cancel()
        player.stop()
        _playback.value = PlaybackState(label = "전체 ${index + 1}/${seqs.size}", busy = true)
        playJob = scope.launch {
            val r = m.fetchSegment(recDir(), rec.id, seq, null, false) { note ->
                _playback.value = _playback.value.copy(note = note)
            }
            if (!r.ok) { _playback.value = PlaybackState(error = r.reason); return@launch }
            val ok = player.play(r.value!!) {
                val next = index + 1
                if (next < seqs.size) playSequence(seqs, next, seqs[next]) else _playback.value = PlaybackState()
            }
            _playback.value = if (ok) PlaybackState(label = "전체 ${index + 1}/${seqs.size}")
                              else PlaybackState(error = "재생할 수 없습니다")
        }
    }

    /** ⑤ 이벤트 줄 — 재생 사실만 남긴다(감사 정본은 서버). */
    private fun notePlayback(label: String) {
        val e = _selected.value
        val gid = e?.group?.let { g -> g.substringAfter(':', g) }.orEmpty()
        s.addActivity(gid, e?.groupName?.ifBlank { gid }.orEmpty(), "녹취 재생 · $label",
            com.cims.ue.dispatch.session.ActivityKind.SDS)
    }

    fun stop() {
        playJob?.cancel()
        player.stop()
        _playback.value = PlaybackState()
    }

    /** 화면을 떠날 때 — 재생을 멈추고 오래된 임시 파일을 정리한다(§4.6). */
    fun onLeave() {
        stop()
        s.management()?.sweepRecordings(recDir())
    }

    override fun close() {
        stop()
        player.release()          // MediaPlayer 는 스스로 안 사라진다
        super.close()
    }

    private fun recDir(): File = File(cacheDir, "rec")

    companion object {
        /** 서버 창 조회 상한(csc `dispatch_history._MAX_LIMIT`). 이만큼 왔으면 잘린 것으로 본다. */
        const val QUERY_LIMIT = 1000

        /** 그날 [00:00, 24:00) 의 epoch ms 창 — 서버 스캔 48시간 상한 안이다. */
        fun dayWindow(d: LocalDate): Pair<Long, Long> {
            val z = ZoneId.systemDefault()
            return d.atStartOfDay(z).toInstant().toEpochMilli() to
                d.plusDays(1).atStartOfDay(z).toInstant().toEpochMilli()
        }

        /** 시간대 밴드 — 서버 `hours`("00".."23" → 건수)가 정본, 없으면 항목의 축 시각으로 센다. */
        fun bandOf(items: List<HistoryEntry>, hours: Map<String, Int>): IntArray {
            val out = IntArray(24)
            if (hours.isNotEmpty()) {
                hours.forEach { (k, v) -> k.trim().toIntOrNull()?.let { if (it in 0..23) out[it] = v } }
                return out
            }
            items.forEach { out[hourOf(it)]++ }
            return out
        }

        internal fun hourOf(e: HistoryEntry): Int =
            java.time.Instant.ofEpochMilli(e.axisAtMs).atZone(ZoneId.systemDefault()).hour

        internal fun inHour(e: HistoryEntry, h: Int?): Boolean = h == null || hourOf(e) == h

        /** 검색 — 상대·그룹·참여자. 빈 질의는 전부 통과. */
        fun matches(e: HistoryEntry, q: String): Boolean {
            if (q.isBlank()) return true
            val needle = q.trim().lowercase()
            val hay = sequenceOf(e.from, e.to, e.group, e.groupName, e.text) + e.people.asSequence()
            return hay.any { it.lowercase().contains(needle) }
        }

        /**
         * 녹취 세그먼트 → 발언 막대. 세션 시작(첫 세그먼트) 기준 오프셋으로 옮겨 한 축에 놓는다.
         *
         * 턴의 원자는 **트랙의 화자 구간**이다 — 동시 발언이면 같은 시각에 슬롯이 여럿이라
         * 막대도 여럿이 된다. 트랙이 없는(구형) 세그먼트는 세그먼트 자체를 한 턴으로 본다.
         */
        fun turnsOf(rec: RecordingInfo): List<TurnBar> {
            val base = rec.startAtMs ?: rec.segments.mapNotNull { it.startAtMs }.minOrNull() ?: return emptyList()
            val out = ArrayList<TurnBar>()
            rec.segments.forEach { seg ->
                val segStart = seg.startAtMs ?: return@forEach
                val shift = (segStart - base).toInt().coerceAtLeast(0)
                if (seg.tracks.isEmpty()) {
                    out.add(TurnBar(seg.speakerId, shift, seg.durationMs, seg.seq, null))
                    return@forEach
                }
                seg.tracks.forEach { tr ->
                    tr.speakers.forEach { sp ->
                        out.add(TurnBar(sp.id, shift + sp.offsetMs, sp.durMs, seg.seq, tr.slot))
                    }
                }
            }
            return out.sortedBy { it.offsetMs }
        }

        // ── 표시 사전(dispatch_desktop_ui.md §9) ────────────────────────────

        fun endReasonText(r: String): String = when (r) {
            "normal" -> "정상종료"
            "no_answer" -> "무응답"
            "busy" -> "통화중"
            "rejected" -> "거절"
            "error" -> "오류"
            "timeout" -> "시간초과"
            "incomplete" -> "비정상 종료"
            else -> r
        }

        fun stateText(e: HistoryEntry): String = when (e.state) {
            "active" -> "통화중"
            "ringing" -> "호출중"
            "ended", "" -> "종료"
            else -> e.state
        }

        fun sessionKindText(k: String): String = when (k) {
            "group" -> "그룹"
            "private" -> "1:1"
            "adhoc" -> "임시"
            else -> k
        }

        fun callTypeText(t: String): String = when (t) {
            "volte_video" -> "영상"
            else -> "음성"
        }

        /** floor 중재 op 8종(TS 24.380) — 콘솔 PTT 이력과 같은 문구. */
        fun floorOpText(op: String): String = when (op.uppercase()) {
            "GRANT" -> "발언권 부여"
            "RELEASE" -> "발언권 반납"
            "IDLE" -> "발언 없음"
            "REVOKE" -> "발언권 회수"
            "REVOKE_END" -> "회수 완료"
            "QUEUE" -> "대기열 등록"
            "QUEUE_CANCEL" -> "대기 취소"
            "DENY" -> "거절"
            else -> op
        }

        fun eventTypeText(t: String): String = when (t) {
            "session_start" -> "세션 시작"
            "session_end" -> "세션 종료"
            "member_join" -> "입장"
            "member_leave" -> "퇴장"
            "member_invite" -> "초대"
            "config_change" -> "설정 변경"
            else -> t
        }

        fun denyReasonText(r: String): String = when (r) {
            "no_permission" -> "권한 없음"
            "busy" -> "다른 발언자"
            "queue_full" -> "대기열 가득참"
            "not_affiliated" -> "미참가"
            else -> r
        }
    }
}
