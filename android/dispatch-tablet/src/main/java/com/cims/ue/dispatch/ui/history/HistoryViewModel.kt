// [이력] 화면 상태 (docs/design/features/android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
//
// 끝난 통화·무전 세션의 **날짜 창 조회 + 녹취 재생**. 진행 중·오늘의 실시간 흐름은 [관제] 가 정본이고 여기는 서버가 보관한
// 사본을 본다. 조회 축(종류·날짜·시간대·검색·서비스·빈 세션 묶기)과 재생 헤드는 이 VM 이 갖고, 판정 규칙은 전부 순수 함수다
// (`HistoryRows.kt`·`HistoryAxis.kt`·`HistoryPlayback.kt` — 기기 없이 시험한다, §9 S1-UE-TABLET-UNIT).
package com.cims.ue.dispatch.ui.history

import android.view.Surface
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.PttSessionDetail
import com.cims.ue.dispatch.session.RecordingInfo
import com.cims.ue.dispatch.ui.ScreenViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File
import java.time.LocalDate
import java.time.ZoneId

/** [이력] 화면이 그리는 데 필요한 조회 축과 목록. 상세 패널·재생 바는 따로 흐른다(200 ms 마다 바뀌는 것은 재생 바뿐이다). */
@Suppress("ArrayInDataClass")   // band 는 24칸 고정 배열 — 동등성 비교 대상이 아니다
data class HistoryUi(
    /** 처음은 무전 — 관제 화면의 [무전|통화] 와 같은 순서·같은 첫 모드. */
    val kind: HistoryKind = HistoryKind.PTT,
    /** 거른 뒤의 항목(최근이 위) — 화면이 빈 세션 묶음·시간대 묶음으로 엮는다. */
    val rows: List<HistoryEntry> = emptyList(),
    val loading: Boolean = false,
    val error: String = "",
    /** 서버 상한에 걸려 잘렸는가 — 조용히 일부만 보여 주면 «없는 통화» 로 읽힌다. */
    val truncated: Boolean = false,
    /** 시간대 필터 — null 이면 하루 전체. 종류·날짜가 바뀌면 풀린다. */
    val hourFilter: Int? = null,
    val date: LocalDate = LocalDate.now(),
    val query: String = "",
    /** 시간대 밴드 24칸 — 서버 `hours`(통화 = INVITE, 무전 = 세션 시작, 절삭 전 전체). */
    val band: IntArray = IntArray(24),
    /** 무전 [발언 수] 밴드의 칸. 비어 있으면 화면이 [rows] 에서 센다(Preview). */
    val turnBand: List<HourCell> = emptyList(),
    val bandMode: BandMode = BandMode.SESSIONS,
    /** 받은 무전 항목에 서비스 축이 실려 있다 — [전체|무전|영상] 칩을 보인다. */
    val serviceAxis: Boolean = false,
    val service: ServiceFilter = ServiceFilter.ALL,
    /** 빈 세션 묶기(기본 켬). */
    val groupSilent: Boolean = true,
    /** 펼친 빈 세션 묶음의 열쇠. */
    val openBundles: Set<String> = emptySet(),
    val selected: HistoryEntry? = null,
    /** 발언 타임라인의 «틈 줄임»(기본 켬). */
    val compactGaps: Boolean = true,
) {
    /** [발언 수] 밴드의 칸을 눌렀다 — 그 시간대의 발언 있는 세션만 보인다. */
    val speechOnly: Boolean get() = kind == HistoryKind.PTT && bandMode == BandMode.TURNS && hourFilter != null
}

class HistoryViewModel(
    private val s: DispatchSession,
    private val cacheDir: File,
    private val player: SegmentPlayer = SegmentPlayer(),
) : ScreenViewModel() {

    /** 이름 풀이 — 사람은 주소록, 그룹은 그룹 목록. 화면과 순수 함수가 같은 것을 쓴다. */
    val names = HistoryNames(
        who = { s.displayName(it) },
        group = { uri -> s.groups.value.firstOrNull { it.uri.equals(uri, ignoreCase = true) }?.name.orEmpty() })

    private val _ui = MutableStateFlow(HistoryUi())
    val ui: StateFlow<HistoryUi> = _ui.asStateFlow()

    /** 고른 무전 세션의 패널(통화는 null — 통화 상세는 행 하나로 그린다). */
    private val _pane = MutableStateFlow<SessionPaneUi?>(null)
    val pane: StateFlow<SessionPaneUi?> = _pane.asStateFlow()

    private val _player = MutableStateFlow(PlayerUi())
    val playback: StateFlow<PlayerUi> = _player.asStateFlow()

    /** 조회 결과 원본(최근이 위) — 지금 보이는 창(하루 또는 고른 한 시간)의 것. */
    private var all: List<HistoryEntry> = emptyList()
    /** 하루 창으로 받은 항목 — [발언 수] 밴드가 센다. */
    private var dayItems: List<HistoryEntry> = emptyList()
    /** 하루 조회가 서버 상한에 잘렸는가 — 잘렸을 때만 시간대 칸이 서버에 다시 묻는다. */
    private var dayTruncated = false
    /** 시간대를 좁혀 다시 받은 항목 — 그 칸의 발언 수는 이것으로 센다(하루 상한에 잘린 날에도 맞다). */
    private val hourItems = HashMap<Int, List<HistoryEntry>>()
    private var band = IntArray(24)

    private var queryJob: Job? = null
    private var detailJob: Job? = null
    private var playJob: Job? = null
    private var tickJob: Job? = null

    /** 관제 역할이 없으면 화면이 잠긴다 — 서버가 403 을 내기 전에 앱이 먼저 접는다. */
    val available: Boolean get() = s.hasDesk

    private inline fun patch(f: HistoryUi.() -> HistoryUi) { _ui.value = _ui.value.f() }

    // ── 축 ────────────────────────────────────────────────────────────────────

    fun show(k: HistoryKind) {
        if (_ui.value.kind == k) return
        clearSelection()
        clearWindow()
        patch { copy(kind = k, hourFilter = null) }
        load(hour = null)
    }

    fun showDate(d: LocalDate) {
        val capped = if (d.isAfter(LocalDate.now())) LocalDate.now() else d   // 미래로는 못 간다
        if (_ui.value.date == capped) return
        clearSelection()
        clearWindow()
        patch { copy(date = capped, hourFilter = null) }
        load(hour = null)
    }

    /** 종류·날짜가 바뀌었다 — 앞 창의 항목을 비운다(새 응답이 오기 전에 다른 종류의 카드로 그려지지 않게). */
    private fun clearWindow() {
        all = emptyList(); dayItems = emptyList(); hourItems.clear(); band = IntArray(24)
        patch { copy(rows = emptyList(), band = this@HistoryViewModel.band, turnBand = emptyList(), truncated = false) }
    }

    fun shiftDay(days: Long) = showDate(_ui.value.date.plusDays(days))
    fun today() = showDate(LocalDate.now())

    fun search(q: String) { patch { copy(query = q) }; project() }

    fun setService(f: ServiceFilter) { patch { copy(service = f) }; project() }

    fun setGroupSilent(on: Boolean) = patch { copy(groupSilent = on) }

    /** 묶음 머리의 [n건 펼치기]/[접기] — 그 묶음만. 펼침은 날짜를 다시 조회해도 열쇠가 같으면 남는다. */
    fun toggleBundle(key: String) = patch { copy(openBundles = if (key in openBundles) openBundles - key else openBundles + key) }

    /** 밴드가 세는 것 — 고른 시간대가 있으면 거르기의 뜻도 바뀐다(전체 ↔ 발언 있는 세션만). */
    fun setBandMode(m: BandMode) {
        if (_ui.value.bandMode == m) return
        patch { copy(bandMode = m) }
        if (_ui.value.hourFilter != null) project()
    }

    /**
     * 밴드 칸 클릭 — **그 시간대를 서버에 다시 묻는다.**
     *
     * 지역 필터로만 좁히면 앞 시간대가 영영 안 보인다: 서버는 창 안에서 **최근 `limit`(1000) 건만** 주는데 `hours` 는 **절삭 전
     * 전체**로 낸다(csc `dispatch_history.finish_rows`). 그래서 밴드에는 건수가 뜨는데 눌러 보면 빈 목록이고, 잘려 나간 이력과
     * 그 녹취에 닿을 길이 없다. 창을 그 한 시간으로 좁혀 다시 물으면 상한 안에 들어온다.
     *
     * 그 모드에서 0 인 칸은 무시한다(고른 칸이면 해제).
     */
    fun toggleHour(h: Int) {
        val u = _ui.value
        val n = if (u.kind == HistoryKind.PTT && u.bandMode == BandMode.TURNS) u.turnBand.getOrNull(h)?.count ?: 0
                else u.band.getOrElse(h) { 0 }
        if (n <= 0 && u.hourFilter != h) return
        val next = if (u.hourFilter == h) null else h
        patch { copy(hourFilter = next) }
        load(hour = next)
    }

    fun clearHour() {
        if (_ui.value.hourFilter == null) return
        patch { copy(hourFilter = null) }
        load(hour = null)
    }

    // ── 조회 ──────────────────────────────────────────────────────────────────

    /**
     * 조회. [hour] 가 있으면 그 한 시간만, 없으면 하루 전체.
     *
     * 밴드(시간대 분포)는 **하루 전체 조회에서만** 갱신한다 — 한 시간 창의 응답에는 그 칸만 들어 있어 그대로 쓰면 나머지 칸이
     * 사라진다.
     */
    /** ⟳·[이력에서 보기] — 서버에 **다시 묻는다**(하루 전체부터). 고른 시간대는 남는다. */
    fun refresh() = load(hour = null)

    fun load(hour: Int? = _ui.value.hourFilter) {
        val m = s.management() ?: return patch { copy(error = "로그인 전") }
        queryJob?.cancel()
        patch { copy(error = "", loading = true) }
        val u = _ui.value
        val (dayFrom, dayTo) = dayWindow(u.date)
        // 시간대 칸은 밴드와 **같은 축**(세션·호의 시작 시각)으로 거른다. 하루 목록을 다 받았으면(상한에 안 걸렸으면) 그 목록에서
        //   고른다 — 서버에 다시 묻지 않는다(데스크톱과 같다).
        if (hour != null && dayItems.isNotEmpty() && !dayTruncated) {
            all = dayItems.filter { hourOf(it.axisAtMs) == hour }
            hourItems[hour] = all
            patch { copy(loading = false, truncated = false) }
            project()
            return
        }
        // 하루가 상한에 잘렸을 때만 그 시간대를 서버에 다시 묻는다(잘려 나간 이력에 닿는 길). 서버는 창을 **종료 시각**으로 자르므로
        //   끝을 한 시간 넓혀 받고(그 시간대에 시작해 다음 시간대에 끝난 것) 받은 것을 시작 시각으로 다시 거른다.
        val from = if (hour == null) dayFrom else dayFrom + hour * 3_600_000L
        val to = if (hour == null) dayTo else dayFrom + (hour + 2) * 3_600_000L
        val k = u.kind
        queryJob = scope.launch {
            val began = System.nanoTime()
            val r = m.history(k, from, to)
            val queryMs = (System.nanoTime() - began) / 1_000_000
            val page = r.value
            if (!r.ok || page == null) {
                all = emptyList()
                if (hour == null) { dayItems = emptyList(); hourItems.clear(); band = IntArray(24) }
                android.util.Log.w(TAG, "history window ${k.wire} ${u.date}${hourTag(hour)}: ${r.code} ${r.reason}")
                patch { copy(loading = false, error = r.reason, truncated = false) }
                project()
                return@launch
            }
            all = page.items.distinctBy { it.id }.sortedByDescending { it.atMs }      // 최근이 위
            if (hour == null) {
                dayItems = all
                dayTruncated = page.items.size >= QUERY_LIMIT
                hourItems.clear()
                band = bandOf(page.items, page.hours)
                // 다시 조회([refresh])한 뒤에도 고른 시간대는 남는다 — 새 하루 목록 위에 그 칸을 다시 건다
                _ui.value.hourFilter?.let { keep ->
                    patch { copy(loading = false, truncated = dayTruncated) }
                    load(keep)
                    return@launch
                }
            } else {
                all = all.filter { it.axisAtMs in dayFrom until dayTo && hourOf(it.axisAtMs) == hour }
                hourItems[hour] = all
            }
            // 받은 것의 구성을 남긴다 — «콘솔에는 있는데 앱에는 없다» 를 서버 응답에서 가른다
            //   (영상 통화 = callType volte_video · 영상 세션 = service mcvideo).
            android.util.Log.i(TAG, "history window ${k.wire} ${u.date}${hourTag(hour)}: ${all.size} items " +
                (if (k == HistoryKind.CALL) "(video ${all.count { it.isVideoCall }})"
                 else "(mcvideo ${all.count { it.isMcVideo }}, service axis ${if (all.any { it.service.isNotEmpty() }) "yes" else "no"})") +
                " · query $queryMs ms")
            // 상한에 닿았으면 잘린 것이다 — 조용히 일부만 보여 주지 않는다.
            patch { copy(loading = false, truncated = page.items.size >= QUERY_LIMIT) }
            project()
        }
    }

    /** 받은 항목을 화면에 — 서비스 축이 없으면 거르기를 풀고(칩이 사라진다) 밴드·목록을 다시 낸다. */
    private fun project() {
        val u = _ui.value
        val ptt = u.kind == HistoryKind.PTT
        val axis = ptt && all.any { it.service.isNotEmpty() }
        val service = if (axis) u.service else ServiceFilter.ALL
        val rows = filterRows(all, u.kind, service, u.speechOnly, u.query, names).map { it.e }
        _ui.value = u.copy(rows = rows, band = band, serviceAxis = axis, service = service,
            turnBand = if (ptt) turnCells(dayItems, band, hourItems) else emptyList())
        // 고른 세션이 걸러졌으면 선택을 놓는다 — 빈 패널이 남지 않게. 목록에 남아 있으면 선택·패널·재생이 그대로다.
        u.selected?.let { sel -> if (rows.none { it.id == sel.id }) clearSelection() }
    }

    // ── 선택 ──────────────────────────────────────────────────────────────────

    /** 고른 세션의 자료 — 틈 줄임을 켜고 끌 때 같은 자료로 패널을 다시 그린다. */
    private var selRow: HistoryRow? = null
    private var selDetail: PttSessionDetail? = null
    private var recording: RecordingInfo? = null
    private var detailStatus = ""

    fun clearSelection() {
        detailJob?.cancel()
        resetPlayer()
        selRow = null; selDetail = null; recording = null; detailStatus = ""
        _pane.value = null
        patch { copy(selected = null) }
    }

    /** 행 선택 — 무전은 세션 상세·녹취를, 통화는 녹취만 읽는다. 둘은 나란히 묻는다. */
    fun select(e: HistoryEntry) {
        if (_ui.value.selected?.id == e.id) return
        detailJob?.cancel()
        resetPlayer()
        val row = rowOf(e, names)
        selRow = row; selDetail = null; recording = null; detailStatus = ""
        patch { copy(selected = e) }
        val ptt = e.kind == HistoryKind.PTT
        val recId = e.recordingId
        val wantDetail = ptt && recId.isNotBlank()
        val wantRec = recId.isNotBlank() && e.hasRecording
        status = when {
            wantRec -> "녹취 정보 조회 중…"
            row.live -> "진행 중 — 끝나면 녹취가 잡힙니다"
            else -> "녹취 없음"
        }
        // 목록 항목만으로 머리·숫자 칸을 먼저 세운다 — 상세·녹취는 오는 대로 채운다.
        _pane.value = if (ptt) buildSessionPane(row, null, null, _ui.value.compactGaps, names,
            status = if (wantDetail) "세션 상세 조회 중…" else "") else null
        bar = playerBarOf(row, null, _pane.value, names)
        headMs = bar?.t0 ?: 0
        publish()
        val m = s.management() ?: return
        if (!wantDetail && !wantRec) return
        detailJob = scope.launch {
            val detailCall = if (wantDetail) async { m.pttSessionDetail(recId) } else null
            val recCall = if (wantRec) async { m.recording(recId) } else null
            detailCall?.await()?.let { d -> if (d.ok) selDetail = d.value else detailStatus = d.reason }
            recCall?.await()?.let { r ->
                val rec = r.value
                if (r.ok && rec != null) {
                    recording = rec
                    status = if (rec.segments.isNotEmpty()) "세그먼트 ${rec.segments.size}개 · ${rec.status}"
                             else "세그먼트 없음(녹음 진행 중이거나 미디어 없음)"
                } else status = r.reason
            }
            rebuildPane()
        }
    }

    /** 고른 세션의 패널과 재생 바를 지금 자료로 다시 만든다. 헤드는 첫 세그먼트에 선다. */
    private fun rebuildPane() {
        val row = selRow ?: return
        val ptt = row.e.kind == HistoryKind.PTT
        _pane.value = if (ptt) buildSessionPane(row, selDetail, recording, _ui.value.compactGaps, names, detailStatus) else null
        val b = playerBarOf(row, recording, _pane.value, names)
        bar = b
        headMs = b.segs.firstOrNull()?.startMs ?: b.t0
        publish()
    }

    /** 틈 줄임 — 막대 위치·눈금이 축에 달려 있어 패널만 다시 그린다(재생 바 막대는 틈 줄임과 무관하게 시간에 비례한다). */
    fun setCompactGaps(on: Boolean) {
        if (_ui.value.compactGaps == on) return
        patch { copy(compactGaps = on) }
        val row = selRow ?: return
        if (row.e.kind == HistoryKind.PTT)
            _pane.value = buildSessionPane(row, selDetail, recording, on, names, _pane.value?.status.orEmpty())
    }

    // ── 녹취 재생 — 재생 바 ───────────────────────────────────────────────────
    // 막대의 한 점(벽시계 시각)을 그 시각을 담은 세그먼트 + 파일 안 오프셋으로 풀어 튼다([PlayerBar]). 세그먼트가 끝나면 다음으로
    // 잇고, 말 없는 구간은 건너뛰기면 바로 다음 세그먼트로, 아니면 시계로 헤드만 흘려 다음 세그먼트에서 이어 튼다.

    private var bar: PlayerBar? = null
    /** 재생 헤드(벽시계 ms). */
    private var headMs = 0L
    /** 지금 재생기에 열린 파일의 세그먼트(null = 말 없는 구간·정지). */
    private var curSeq: Int? = null
    /** 여는 요청의 차례 — 받는 동안 다른 곳을 누르면 앞 요청의 결과를 버린다. */
    private var openToken = 0
    /** 받고 있는 세그먼트와 그 파일을 열 자리(ms) — 받는 동안 같은 세그먼트 안을 옮기면 자리만 바뀐다. */
    private var pendingSeq: Int? = null
    private var pendingOffsetMs = 0
    private var playing = false
    private var loading = false
    private var status = ""
    private var skipGaps = true
    private var speedIndex = 0
    private var video = false
    private var videoAspect = 4f / 3f
    /** 이 세션의 재생을 ⑤ 이벤트 줄에 남겼는가 — 세그먼트마다 남기지 않는다. */
    private var logged = false

    private val speed: Float get() = PLAY_SPEEDS[speedIndex.coerceIn(0, PLAY_SPEEDS.lastIndex)]

    private fun publish() {
        _player.value = PlayerUi(bar, headMs, playing, loading, player.isOpen, status, skipGaps, speedIndex,
            video = video, videoAspect = videoAspect)
    }

    /** 재생 의도를 바꾼다 — 재생 중에만 200 ms 틱이 돈다. */
    private fun setPlaying(on: Boolean) {
        playing = on
        if (!on) { tickJob?.cancel(); tickJob = null; return }
        if (tickJob?.isActive == true) return
        tickJob = scope.launch { while (isActive) { delay(TICK_MS); tick() } }
    }

    /** 막대의 한 점으로 — 그 시각을 담은 세그먼트를 그 오프셋에서 연다(같은 파일이면 위치만 옮긴다). [play] = null 이면 지금 상태 유지. */
    private fun seekTo(atMs: Long, play: Boolean? = null) {
        val b = bar ?: return
        if (b.segs.isEmpty()) return
        val want = play ?: playing
        val hit = b.locate(atMs, want, skipGaps)
        headMs = hit.atMs
        val seg = hit.seg
        if (seg == null) {                                        // 말 없는 구간·끝 — 파일을 닫고 시계만(tick)
            if (curSeq != null || loading) closeMedia()           // 받고 있던 것도 버린다 — 늦게 와서 헤드를 되돌리지 않게
            setPlaying(want && b.hasAfter(hit.atMs))
            publish()
            return
        }
        if (curSeq == seg.seq && player.isOpen) {
            player.seekTo(hit.offsetMs)
            if (want != playing) { if (want) player.resume(speed) else player.pause() }
            setPlaying(want)
            publish()
            return
        }
        setPlaying(want)
        // 그 세그먼트를 이미 받고 있으면 다시 묻지 않고 열 자리만 바꾼다(막대를 끌며 같은 세그먼트 안을 옮길 때).
        if (loading && pendingSeq == seg.seq) { pendingOffsetMs = hit.offsetMs; publish(); return }
        open(seg, hit.offsetMs, retry = false)
    }

    /**
     * 세그먼트 파일을 받아 연다. 받는 동안은 앞 파일을 닫아 둔다 — 누른 곳과 다른 소리가 이어 나지 않게. 받고 나서 틀지는
     * **그때의** 재생 의도를 본다(받는 동안 일시정지했으면 그 자리에 멈춘 채 연다).
     */
    private fun open(seg: SegSpan, offsetMs: Int, retry: Boolean) {
        val m = s.management()
        val rec = recording
        if (m == null || rec == null) { setPlaying(false); publish(); return }
        val token = ++openToken
        playJob?.cancel()
        player.stop()
        curSeq = null
        pendingSeq = seg.seq
        pendingOffsetMs = offsetMs
        loading = true
        // 영상 세그먼트면 받는 동안 영상 칸을 먼저 연다 — 재생기가 파일을 열 때 그리기 면이 이미 붙어 있어야 첫 장면부터 그린다.
        video = seg.hasVideo
        status = if (seg.hasVideo) "영상 받는 중…" else "오디오 받는 중…"
        publish()
        playJob = scope.launch {
            val r = m.fetchSegment(recDir(), rec.id, seg.seq, null, retry) { note ->
                if (token == openToken) { status = note; publish() }
            }
            if (token != openToken) return@launch
            loading = false
            pendingSeq = null
            val file = r.value
            if (!r.ok || file == null) { status = r.reason; video = false; setPlaying(false); publish(); return@launch }
            val ok = player.open(file, pendingOffsetMs, play = playing, speed = speed,
                onDone = { if (token == openToken) onMediaEnded() },
                onError = { if (token == openToken) onMediaFailed() },
                onVideoSize = { w, h -> if (token == openToken) { videoAspect = w.toFloat() / h; publish() } })
            if (!ok) { onMediaFailed(); return@launch }
            curSeq = seg.seq
            video = seg.hasVideo
            status = "녹취 ${bar?.segs?.size ?: 0}개 중 #${seg.seq}"
            // 감청·재생의 감사는 서버(E-AUD-016)가 정본이고 앱은 ⑤ 줄에만 남긴다.
            if (!logged) { logged = true; notePlayback(seg.seq) }
            publish()
        }
    }

    /** 200 ms 틱 — 재생기 위치로 헤드를 옮기고, 말 없는 구간이면 시계로 흘려 다음 세그먼트를 연다. */
    private fun tick() {
        val b = bar ?: return
        if (!playing || loading || b.segs.isEmpty()) return
        val cur = b.segOf(curSeq)
        if (cur != null) {
            if (player.isOpen) { headMs = cur.startMs + player.positionMs; publish() }
            return
        }
        headMs += (TICK_MS * speed).toLong()
        if (b.segAt(headMs) != null) return seekTo(headMs, true)
        if (headMs >= b.endMs) { headMs = b.endMs; setPlaying(false); status = "재생 끝" }
        publish()
    }

    /** 파일 하나가 끝났다 — 다음 세그먼트로(건너뛰기면 바로, 아니면 말 없는 구간을 시계로 지나서). */
    private fun onMediaEnded() {
        val b = bar ?: return
        val cur = b.segOf(curSeq)
        curSeq = null
        player.stop()
        if (cur == null) return publish()
        val next = b.nextAfter(cur)
        if (next == null) { headMs = cur.endMs; video = false; setPlaying(false); status = "재생 끝"; publish(); return }
        // 바로 잇는 세그먼트에도 영상이 있으면 영상 칸은 열린 채다(닫았다 여는 깜박임이 없다 — open 이 정한다).
        if (skipGaps || next.startMs <= cur.endMs) seekTo(next.startMs, true)
        else { headMs = cur.endMs; video = false; publish() }    // 시계가 말 없는 구간을 지나 tick 이 다음 세그먼트를 연다
    }

    private fun onMediaFailed() {
        curSeq = null
        player.stop()
        video = false
        setPlaying(false)
        status = "재생할 수 없습니다 — 파일이 손상됐을 수 있습니다 · [다시 변환]"
        publish()
    }

    private fun closeMedia() {
        playJob?.cancel()
        openToken++
        player.stop()
        curSeq = null
        pendingSeq = null
        loading = false
        video = false
    }

    private fun resetPlayer() {
        closeMedia()
        setPlaying(false)
        bar = null
        headMs = 0
        status = ""
        logged = false
        publish()
    }

    // 조작 — 막대(화면이 비율을 넘긴다)·버튼

    /** 막대를 눌렀다 — 그 지점부터. */
    fun seekRatio(ratio: Float) { bar?.let { seekTo(it.timeAt(ratio)) } }

    fun togglePlay() {
        val b = bar ?: return
        if (b.segs.isEmpty()) return
        if (playing) {
            setPlaying(false)
            if (player.isOpen) player.pause()
            return publish()
        }
        if (headMs >= b.endMs - 500) headMs = b.segs[0].startMs   // 끝에서 누르면 처음부터
        if (curSeq != null && player.isOpen) {
            setPlaying(true)
            player.resume(speed)
            return publish()
        }
        seekTo(headMs, true)
    }

    fun back10() = seekTo(headMs - 10_000)
    fun fwd10() = seekTo(headMs + 10_000)

    /** 이전·다음 발언 — 무전 발언 막대의 시작으로. */
    fun prevTurn() { bar?.let { seekTo(it.prevTurnStart(headMs)) } }
    fun nextTurn() { bar?.nextTurnStart(headMs)?.let { seekTo(it) } }

    /** [처음부터] — 첫 세그먼트부터 끝까지. */
    fun playAll() { bar?.segs?.firstOrNull()?.let { seekTo(it.startMs, true) } }

    /** 발언 레인의 턴 막대 — 그 발언 시작으로 옮겨 재생(믹스). 줄인 틈 축에서도 턴의 실제 시각으로 간다. */
    fun playTurn(t: Turn) = seekTo(t.startMs, true)

    /** [다시 변환] — 지금(없으면 헤드 위치의) 세그먼트의 변환 실패 표식을 지우고 다시 받아 그 자리에서 튼다. */
    fun retry() {
        val b = bar ?: return
        val hit = b.segOf(curSeq) ?: b.segAt(headMs) ?: b.segs.firstOrNull() ?: return
        setPlaying(true)
        open(hit, (headMs - hit.startMs).coerceIn(0, hit.endMs - hit.startMs).toInt(), retry = true)
    }

    fun setSpeed(index: Int) {
        speedIndex = index.coerceIn(0, PLAY_SPEEDS.lastIndex)
        if (playing) player.setSpeed(speed)
        publish()
    }

    fun setSkipGaps(on: Boolean) { skipGaps = on; publish() }

    /** 영상 칸의 그리기 면이 생겼다/사라졌다. */
    fun attachSurface(surface: Surface?) = player.setSurface(surface)

    /** [정지] — 파일을 닫고 헤드는 그 자리(영상 칸도 닫힌다). */
    fun stop() {
        closeMedia()
        setPlaying(false)
        publish()
    }

    /** ⑤ 이벤트 줄 — 재생 사실만 남긴다(감사 정본은 서버). */
    private fun notePlayback(seq: Int) {
        val row = selRow ?: return
        val gid = row.e.group.let { g -> g.substringAfter(':', g) }
        s.addActivity(gid, row.target.ifBlank { gid }, "녹취 재생 ${row.parties} #$seq", ActivityKind.NOTE)
    }

    /** 화면을 떠날 때 — 재생을 멈추고 오래된 임시 파일을 정리한다. */
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
        private const val TAG = "History"

        /** 서버 창 조회 상한(csc `dispatch_history._MAX_LIMIT`). 이만큼 왔으면 잘린 것으로 본다. */
        const val QUERY_LIMIT = 1000

        /** 재생 헤드 갱신 간격(ms). */
        private const val TICK_MS = 200L

        private fun hourTag(hour: Int?): String = if (hour == null) "" else " %02d시".format(hour)

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
            items.forEach { out[hourOf(it.axisAtMs)]++ }
            return out
        }
    }
}
