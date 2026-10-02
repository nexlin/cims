// [이력] 목록의 순수 논리 — 행 표시값·통화 결과 태그·빈 세션 묶음·시간대 묶음·밴드·요약
// (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6)
//
// 화면(Compose)과 VM 이 같이 쓰는 값·함수만 둔다. 세션·기기를 모른다 — JVM 단위시험으로 고정한다(§9 S1-UE-TABLET-UNIT).
// 규칙의 정본은 Windows 관제 앱의 `SessionHistoryViewModel`(`HistoryRow`·`Filter`·`RebuildHours`)이고 두 앱이 같은 항목을
// 같은 낱말·같은 판정으로 보인다.
package com.cims.ue.dispatch.ui.history

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.HistoryEntry
import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.userPart
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.max
import kotlin.math.roundToInt

// ── 이름·시각·길이 표기 ─────────────────────────────────────────────────────

/**
 * 이름 풀이 — 화면과 순수 함수는 세션을 모른다. VM 이 주소록·그룹 목록을 붙여 넘기고, Preview·시험은 기본값(번호 그대로)을 쓴다.
 *
 * @param who URI·번호 → 이름(없으면 번호).
 * @param group 그룹 URI → 그룹 목록의 이름(없으면 "" — 항목의 `groupName` 으로 넘어간다).
 */
class HistoryNames(
    val who: (String) -> String = { userPart(it).ifEmpty { it } },
    val group: (String) -> String = { "" },
)

private val HHMMSS: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")
private val HHMM: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm")
private val DAY_CLOCK: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")

private fun local(ms: Long) = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault())

internal fun hhmmss(ms: Long?): String = ms?.let { local(it).format(HHMMSS) } ?: "—"
internal fun hhmm(ms: Long): String = local(ms).format(HHMM)
internal fun dayClock(ms: Long): String = local(ms).format(DAY_CLOCK)
internal fun hourOf(ms: Long): Int = local(ms).hour

/** 길이(초) — "m분 s초"·"s초", 0 이하는 "—". */
internal fun fmtDur(sec: Int): String {
    if (sec <= 0) return "—"
    val m = sec / 60
    return if (m > 0) "${m}분 ${sec % 60}초" else "${sec}초"
}

/** 발화 시간(ms) — 1분 이상은 "m분 s초", 10초 이상은 "s초", 미만은 "s.d초". */
internal fun fmtSpeech(ms: Int): String {
    if (ms <= 0) return "0초"
    if (ms >= 60_000) { val s = ms / 1000; return "${s / 60}분 ${s % 60}초" }
    if (ms >= 10_000) return "${ms / 1000}초"
    val tenths = (ms / 100.0).roundToInt()
    return if (tenths % 10 == 0) "${tenths / 10}초" else "${tenths / 10}.${tenths % 10}초"
}

/** 재생 위치 — "mm:ss". */
internal fun mmss(ms: Long): String {
    val t = (max(0L, ms) / 1000.0).roundToInt()
    return "%02d:%02d".format(t / 60, t % 60)
}

/** 같은 사람인가 — 표기 차이(`010…` ↔ `+8210…`·`tel:`/`sip:`)를 넘어 본다. 한쪽이라도 비면 아니다. */
internal fun sameUser(a: String, b: String): Boolean {
    val x = DirectoryBook.normalize(userPart(a)); val y = DirectoryBook.normalize(userPart(b))
    return x.isNotEmpty() && x == y
}

// ── 표시 사전(dispatch_desktop_ui.md §9 — 콘솔 이력과 같은 문구) ────────────

internal fun endReasonText(r: String): String = when (r) {
    "normal" -> "정상종료"
    "no_answer" -> "무응답"
    "busy" -> "통화중"
    "rejected" -> "거절"
    "error" -> "오류"
    "timeout" -> "시간초과"
    "incomplete" -> "비정상 종료(기록 없음)"
    "" -> "—"
    else -> r
}

/** 통화 결과 태그의 색조 — 응답 회색 · 통화 중/호출 중 초록 · 부재/실패/취소 주황 · 거절/오류 빨강(관제 «기록» 과 같은 상태색). */
enum class ResultTone { NEUTRAL, TALK, WARN, BAD }

data class CallResult(val text: String, val tone: ResultTone)

/**
 * 통화 결과 — 진행 중이면 상태가, 응답 시각이 있으면 «응답» 이, 없으면 종료 사유가 정한다. 사유가 비었거나 `normal` 인데 응답이
 * 없으면 응답 전에 발신자가 끊은 것이다(«취소»).
 */
internal fun callResultOf(e: HistoryEntry): CallResult = when {
    e.state == "active" -> CallResult("통화 중", ResultTone.TALK)
    e.state == "ringing" -> CallResult("호출 중", ResultTone.TALK)
    e.answerAtMs != null -> CallResult("응답", ResultTone.NEUTRAL)
    else -> when (e.endReason) {
        "no_answer", "timeout" -> CallResult("부재", ResultTone.WARN)
        "busy" -> CallResult("실패", ResultTone.WARN)
        "rejected" -> CallResult("거절", ResultTone.BAD)
        "error", "incomplete" -> CallResult("오류", ResultTone.BAD)
        else -> CallResult("취소", ResultTone.WARN)
    }
}

// ── 행 ──────────────────────────────────────────────────────────────────────

/**
 * 목록 한 행의 표시값 — 통화(카드 두 줄·상세)·무전(카드 세 줄·세션 패널 머리) 두 종류를 서버 항목에서 미리 만든다.
 * 이름은 주소록, 그룹은 그룹 목록의 이름이다. 종류에 안 맞는 칸은 빈 값이다.
 */
data class HistoryRow(
    val e: HistoryEntry,
    /** 진행 중 — 통화 중·호출 중·세션 진행 중. */
    val live: Boolean = false,
    /** 목록 묶음 열쇠 — 시간대 밴드와 같은 축(통화 INVITE·무전 세션 시작)의 시. */
    val hour: Int = 0,
    /** 카드 오른쪽 위 시각(HH:mm) — 같은 축. */
    val startClock: String = "",
    // ── 통화 ──
    val caller: String = "",
    val callee: String = "",
    /** 이름이 있으면 그 아래 둘째 줄에 번호 — 이름이 없으면(번호가 곧 표시) 비운다. */
    val callerNumber: String = "",
    val calleeNumber: String = "",
    val result: CallResult = CallResult("", ResultTone.NEUTRAL),
    /** 카드 둘째 줄 — 통화 시간 · 울림 / 끝난 이유 · 울림. */
    val callSub: String = "",
    /** 울린 시간 — 호출~응답, 응답이 없으면 호출~종료. */
    val ringText: String = "—",
    val durationText: String = "",
    /** 진행 막대 — 울림 : 통화 비율. 응답이 없으면 울림만. */
    val ringWeight: Float = 1f,
    val talkWeight: Float = 0f,
    val endReasonText: String = "",
    // ── 무전 ──
    /** 대상 — 그룹 이름 / 1:1 `A ↔ B` / 애드혹 `A 외 n명`. */
    val target: String = "",
    val kindText: String = "",
    /** 카드 둘째 줄 — 개시 · 참여 n명. */
    val whoLine: String = "",
    /** 카드 셋째 줄 — 길이 · 발언 n회 · 말한 시간(영상 세션은 송출·보낸 시간). */
    val statLine: String = "",
    /** 시작 ~ 종료 · 길이. */
    val rangeText: String = "",
    val floorPolicyText: String = "",
    /** 영상 세션 속성 한 줄 — "chat · 동시 송출 2"(없으면 ""). */
    val mcvText: String = "",
    /** 세션 길이(초) — 시작~종료, 없으면 서버 duration. */
    val durSec: Int = 0,
    /**
     * 빈 세션 — 발언(영상 세션은 송출) 없이 끝난 무전 세션. 서버가 발언 수 0 을 실어 왔고 녹취도 없다(스캔 폴백은 발언 수가 늘 0 이라
     * 녹취로 한 번 더 가른다). 진행 중이면 아니다.
     */
    val silent: Boolean = false,
) {
    val isVideoCall: Boolean get() = e.isVideoCall
    val typeText: String get() = if (e.isVideoCall) "영상" else "음성"
    val isPrivate: Boolean get() = e.sessionKind == "private"
    val isAdhoc: Boolean get() = e.sessionKind == "adhoc"
    val hasFloorPolicy: Boolean get() = e.floorControl == "on" && e.floorPolicy.isNotEmpty()
    /** 세는 말 — 무전 "발언" · 영상 세션 "송출"(TS 24.581 전송 제어 — 발언권이 아니라 송출 허가). */
    val turnWord: String get() = if (e.isMcVideo) "송출" else "발언"
    /** 검색·재생 기록에 쓰는 한 줄. */
    val parties: String get() = when {
        e.kind != HistoryKind.PTT -> "$caller → $callee"
        e.from.isNotEmpty() -> "$target · 개시 $caller"
        else -> target
    }
    val dayClock: String get() = dayClock(e.axisAtMs)
}

/** 서버 항목 → 행(순수 함수, 시험 대상). */
internal fun rowOf(e: HistoryEntry, names: HistoryNames = HistoryNames()): HistoryRow {
    val who = names.who
    val live = e.isLive
    val hour = hourOf(e.axisAtMs)
    val clock = hhmm(e.axisAtMs)
    if (e.kind == HistoryKind.PTT) {
        val group = names.group(e.group).ifBlank { e.groupName }.ifBlank { userPart(e.group) }
        val initiator = if (e.from.isNotEmpty()) who(e.from) else ""
        val peers = e.people.filter { !sameUser(it, e.from) }
        val target = when (e.sessionKind) {
            "private" -> if (peers.isNotEmpty()) "$initiator ↔ ${peers.joinToString(", ") { who(it) }}" else initiator
            "adhoc" -> "$initiator 외 ${peers.size}명"
            else -> group
        }
        val start = e.startAtMs
        val end = e.endAtMs
        val dur = if (start != null && end != null) ((end - start) / 1000).toInt().coerceAtLeast(0) else e.durationSec
        val people = if (e.people.isNotEmpty()) e.people.size else e.memberCount
        val word = if (e.isMcVideo) "송출" else "발언"
        val len = when { live -> "진행 중"; dur > 0 -> fmtDur(dur); else -> "" }
        return HistoryRow(
            e = e, live = live, hour = hour, startClock = clock, caller = initiator,
            target = target,
            kindText = when (e.sessionKind) { "private" -> "1:1"; "adhoc" -> "애드혹"; "", "group" -> "그룹"; else -> "미상" },
            whoLine = listOf(if (e.from.isNotEmpty()) "개시 $initiator" else "", if (people > 0) "참여 ${people}명" else "")
                .filter { it.isNotEmpty() }.joinToString(" · "),
            statLine = listOf(len, "$word ${e.turnCount}회",
                if (e.totalSpeechMs > 0) "${if (e.isMcVideo) "보낸 시간" else "말한 시간"} ${fmtSpeech(e.totalSpeechMs)}" else "")
                .filter { it.isNotEmpty() }.joinToString(" · "),
            rangeText = "${hhmmss(start ?: e.atMs)} ~ ${if (live) "진행중" else hhmmss(end)}" + (if (dur > 0) " · ${fmtDur(dur)}" else ""),
            floorPolicyText = when (e.floorPolicy) {
                "multi" -> "multi · 최대 ${if (e.maxTalkers > 0) e.maxTalkers.toString() else "?"}명"
                "dual" -> "dual · 2명"
                else -> "single"
            },
            mcvText = if (!e.isMcVideo) "" else listOf(
                when (e.mcvSessionType) { "chat" -> "chat"; "prearranged" -> "편성"; else -> "" },
                if (e.mcvMaxTransmitters > 0) "동시 송출 ${e.mcvMaxTransmitters}" else "").filter { it.isNotEmpty() }.joinToString(" · "),
            durSec = dur,
            silent = !live && e.hasTurnCount && e.turnCount == 0 && !e.hasRecording)
    }

    val caller = who(e.from)
    val callee = if (e.to.isNotEmpty()) who(e.to) else "—"
    // 이름 아래 번호 — 표시용 국내 표기. 이름이 없어 `who` 가 번호를 돌려준 경우(같은 값)는 두 번 적지 않는다.
    fun number(u: String): String = com.cims.ue.dispatch.session.localNumber(userPart(u)).let { n ->
        if (u.isEmpty() || n == who(u) || userPart(u) == who(u)) "" else n
    }
    val result = callResultOf(e)
    val invite = e.inviteAtMs
    val answer = e.answerAtMs
    // 울림 = 호출(INVITE)~응답, 응답이 없으면 호출~종료. 통화 = 응답~종료(진행 중이면 지금까지 — 서버 duration).
    val ringSec = if (invite != null) max(0.0, ((answer ?: e.endAtMs ?: invite) - invite) / 1000.0) else 0.0
    val talkSec = if (answer != null) max(e.durationSec.toDouble(), ((e.endAtMs ?: answer) - answer) / 1000.0) else 0.0
    val ringText = if (ringSec >= 1) fmtDur(ringSec.roundToInt()) else "—"
    val durationText = if (e.durationSec > 0) fmtDur(e.durationSec) else "—"
    val reason = endReasonText(e.endReason)
    return HistoryRow(
        e = e, live = live, hour = hour, startClock = clock,
        caller = caller, callee = callee, callerNumber = number(e.from), calleeNumber = number(e.to),
        result = result,
        callSub = when {
            result.tone == ResultTone.TALK -> if (live && answer != null) "통화 중 · 울림 $ringText" else "호출 중"
            answer != null -> "$durationText · 울림 $ringText"
            else -> "$reason · 울림 $ringText"
        },
        ringText = ringText, durationText = durationText,
        ringWeight = max(ringSec, if (answer == null) 1.0 else 0.0001).toFloat(), talkWeight = talkSec.toFloat(),
        endReasonText = reason)
}

// ── 거르기 ──────────────────────────────────────────────────────────────────

/** 무전 목록의 서비스 거르기 — 음성 무전(MCPTT) 세션 / 영상(MCVideo) 세션. 항목에 서비스 축이 실려 있을 때만 칩이 보인다. */
enum class ServiceFilter(val label: String) { ALL("전체"), PTT("무전"), MCVIDEO("영상") }

/** 무전 시간대 밴드가 세는 것 — 성립한 세션 수(서버 `hours`) / 발언 수(세션별 발언 턴의 합, 세션 시작 시간대에 넣는다). */
enum class BandMode(val label: String) { SESSIONS("세션 수"), TURNS("발언 수") }

/** 검색 — 이름·번호·그룹. 표시 이름과 원문(번호·URI)에 걸리고, 번호는 표기 차이(`010…` ↔ `+8210…`)를 넘어 본다. 빈 질의는 전부 통과. */
internal fun matches(row: HistoryRow, q: String): Boolean {
    val needle = q.trim().lowercase()
    if (needle.isEmpty()) return true
    val e = row.e
    val hay = sequenceOf(row.parties, e.from, e.to, e.group, e.groupName, e.text) + e.people.asSequence()
    if (hay.any { it.lowercase().contains(needle) }) return true
    // 번호로 본다 — 번호 모양의 질의만(이름에 든 숫자 하나로 온 번호가 걸리지 않게). 번호의 **어느 표기에든** 들었으면 걸린다
    //   (저장된 그대로 · E.164 · 국내 로컬 — 치다 만 `010333` 이 `+821033334444` 에 걸려야 한다. 주소록 제안과 같은 규칙).
    if (needle.any { !(it.isDigit() || it in "+-(). ") }) return false
    val qd = needle.filter { it.isDigit() || it == '+' }
    if (qd.isEmpty()) return false
    return (sequenceOf(e.from, e.to) + e.people.asSequence()).any { n -> DirectoryBook.numberForms(userPart(n)).any { it.contains(qd) } }
}

/**
 * 받은 항목 → 목록에 설 행(순수 함수, 시험 대상). 시간대는 여기서 거르지 않는다 — 고른 시간대는 서버가 창으로 좁혀 준다(§6.11).
 *
 * @param speechOnly [발언 수] 밴드의 칸을 눌렀다 — 그 시간대의 발언 있는 세션만(`turnCount` > 0 또는 녹취 있음).
 */
internal fun filterRows(
    all: List<HistoryEntry>,
    kind: HistoryKind,
    service: ServiceFilter = ServiceFilter.ALL,
    speechOnly: Boolean = false,
    query: String = "",
    names: HistoryNames = HistoryNames(),
): List<HistoryRow> {
    val ptt = kind == HistoryKind.PTT
    return all.mapNotNull { e ->
        if (ptt && service != ServiceFilter.ALL && (service == ServiceFilter.MCVIDEO) != e.isMcVideo) return@mapNotNull null
        if (ptt && speechOnly && e.turnCount == 0 && !e.hasRecording) return@mapNotNull null
        rowOf(e, names).takeIf { matches(it, query) }
    }
}

// ── 빈 세션 묶음 · 시간대 묶음 ──────────────────────────────────────────────

/** 목록 줄의 종류 — 카드 한 장 / 빈 세션 묶음의 머리 카드 / 펼친 묶음 안의 한 줄. */
enum class RowKind { CARD, BUNDLE, MEMBER }

/**
 * 목록에 서는 한 줄. 묶음 머리([RowKind.BUNDLE])의 [row] 는 **가장 최근 세션**이다 — 고르면 그 세션이 열린다. [members] 는 목록
 * 순서(최근이 앞)다.
 */
data class ListRow(
    val row: HistoryRow,
    val kind: RowKind = RowKind.CARD,
    val members: List<HistoryRow> = emptyList(),
    /** 펼침 상태의 열쇠 — 시간대 + 가장 이른 세션(새 세션이 위에 붙어도 이어진다). */
    val bundleKey: String = "",
    val expanded: Boolean = false,
) {
    /** 시간대 머리 건수에 더하는 세션 수 — 묶음 머리 = 묶은 수, 펼친 한 줄 = 0(머리가 셌다), 나머지 1. */
    val weight: Int get() = when (kind) { RowKind.BUNDLE -> members.size; RowKind.MEMBER -> 0; RowKind.CARD -> 1 }
    /** 카드 오른쪽 위 시각 — 묶음은 가장 이른 ~ 가장 늦은 시작 시각. */
    val clock: String get() =
        if (kind == RowKind.BUNDLE) "${members.last().startClock} ~ ${members.first().startClock}" else row.startClock
    /** 카드 셋째 줄 — 묶음은 "각 30~31초 · 발언 0회". */
    val statLine: String get() = if (kind == RowKind.BUNDLE) bundleStat(members) else row.statLine
    val tagText: String get() = "빈 세션 ${members.size}건"
    val toggleText: String get() = if (expanded) "접기" else "${members.size}건 펼치기"
    /** 펼친 묶음 안 한 줄의 오른쪽 — "발언 0회"(영상 세션은 송출). */
    val memberStat: String get() = "${row.turnWord} ${row.e.turnCount}회"
    /** 목록 항목 열쇠 — 한 세션이 묶음 머리와 펼친 줄 두 곳에 서므로 종류를 앞에 붙인다. */
    val key: String get() = when (kind) { RowKind.BUNDLE -> "b:$bundleKey"; RowKind.MEMBER -> "m:${row.e.id}"; RowKind.CARD -> "c:${row.e.id}" }

    /**
     * 이 줄이 고른 세션을 나타내는가. 접힌 묶음은 안의 어느 세션이 골라져 있어도 머리가 받는다(거르기·펼치기를 바꿔 고른 세션이
     * 묶음에 접혀 들어가도 선택이 눈에 남는다). 펼친 묶음은 안의 한 줄이 받는다.
     */
    fun holds(selectedId: String?): Boolean = selectedId != null && when (kind) {
        RowKind.BUNDLE -> !expanded && members.any { it.e.id == selectedId }
        else -> row.e.id == selectedId
    }
}

private fun bundleStat(members: List<HistoryRow>): String {
    val lo = members.minOf { it.durSec }; val hi = members.maxOf { it.durSec }
    val each = when {
        lo == hi -> fmtDur(lo)
        hi < 60 -> "$lo~${hi}초"
        else -> "${fmtDur(lo)} ~ ${fmtDur(hi)}"
    }
    return "각 $each · ${members.first().turnWord} 0회"
}

/** 빈 세션 묶음에 이어 붙는가 — 같은 시간대 · 같은 서비스 · 같은 그룹 · 같은 개시자의 빈 세션. */
internal fun sameSilentRun(a: HistoryRow, b: HistoryRow): Boolean =
    b.silent && b.hour == a.hour && b.e.isMcVideo == a.e.isMcVideo &&
        b.e.group.equals(a.e.group, ignoreCase = true) && sameUser(b.e.from, a.e.from)

/**
 * 빈 세션 묶기(순수 함수, 시험 대상) — 목록에서 **연달아 나오는** 같은 시간대·같은 서비스·같은 그룹·같은 개시자의 빈 세션 2건
 * 이상을 머리 한 장으로 바꾼다. 사이에 다른 세션이 끼면 거기서 끊겨 시간 순서는 그대로고, 한 건뿐이면 묶지 않는다. [open] 에
 * 든 묶음은 머리 아래에 한 줄씩 펼친다.
 *
 * 누가 호를 열고 아무도 말하지 않아 유지 시간(T4)이 지나 닫히는 일이 되풀이되면 목록이 같은 카드로 덮이는 것을 막는다.
 */
internal fun bundleSilent(shown: List<HistoryRow>, enabled: Boolean, open: Set<String> = emptySet()): List<ListRow> {
    val out = ArrayList<ListRow>(shown.size)
    var i = 0
    while (i < shown.size) {
        val row = shown[i]
        var j = i
        if (enabled && row.silent) while (j + 1 < shown.size && sameSilentRun(row, shown[j + 1])) j++
        if (j == i) { out.add(ListRow(row)); i++; continue }
        val members = shown.subList(i, j + 1).toList()
        val key = "${row.hour}|${members.last().e.id}"
        val expanded = key in open
        out.add(ListRow(row, RowKind.BUNDLE, members, key, expanded))
        if (expanded) members.forEach { out.add(ListRow(it, RowKind.MEMBER, bundleKey = key)) }
        i = j + 1
    }
    return out
}

/** 목록 항목 — 시간대 묶음 머리("HH시 · n건") 또는 줄. */
sealed interface ListItem {
    val key: String

    data class Hour(val hour: Int, val count: Int) : ListItem {
        override val key: String get() = "h:$hour"
        val title: String get() = "%02d시".format(hour)
    }

    data class Line(val row: ListRow) : ListItem {
        override val key: String get() = row.key
    }
}

/**
 * 시간대 묶음(순수 함수) — 밴드와 같은 축의 시로 묶는다. 묶음의 차례는 그 시간대가 목록에 처음 나온 차례다(최근이 위). 머리의
 * 건수는 줄 수가 아니라 **세션 수**다(빈 세션 묶음은 묶은 수).
 */
internal fun groupByHour(rows: List<ListRow>): List<ListItem> {
    val groups = LinkedHashMap<Int, MutableList<ListRow>>()
    rows.forEach { groups.getOrPut(it.row.hour) { ArrayList() }.add(it) }
    val out = ArrayList<ListItem>(rows.size + groups.size)
    groups.forEach { (hour, lines) ->
        out.add(ListItem.Hour(hour, lines.sumOf { it.weight }))
        lines.forEach { out.add(ListItem.Line(it)) }
    }
    return out
}

/** 목록 한 벌 — 항목 + 요약에 쓰는 수. */
data class HistoryList(
    val items: List<ListItem> = emptyList(),
    val shown: Int = 0,
    val live: Int = 0,
    val recorded: Int = 0,
    val video: Int = 0,
    /** 음성 무전 세션의 발화 구간 합(ms). */
    val speechMs: Long = 0,
    val silent: Int = 0,
    val bundles: Int = 0,
)

/** 거른 행 → 목록(빈 세션 묶음 → 시간대 묶음). 묶기는 무전에만 있다. */
internal fun historyListOf(rows: List<HistoryRow>, kind: HistoryKind, groupSilent: Boolean = true,
                           open: Set<String> = emptySet()): HistoryList {
    val ptt = kind == HistoryKind.PTT
    val lines = bundleSilent(rows, enabled = ptt && groupSilent, open = open)
    return HistoryList(
        items = groupByHour(lines), shown = rows.size,
        live = rows.count { it.live }, recorded = rows.count { it.e.hasRecording },
        video = if (ptt) rows.count { it.e.isMcVideo } else 0,
        speechMs = if (ptt) rows.filter { !it.e.isMcVideo }.sumOf { it.e.totalSpeechMs.toLong() } else 0,
        silent = if (ptt) rows.count { it.silent } else 0,
        bundles = lines.count { it.kind == RowKind.BUNDLE })
}

/** 요약 — 날짜(·시간대·발언 있는 세션) · 건수 · 진행중 · 녹취 · 영상 · 발화 합 · 빈 세션 n건 → m묶음. 0 인 항은 적지 않는다. */
internal fun summaryOf(list: HistoryList, kind: HistoryKind, date: LocalDate, hour: Int?, speechOnly: Boolean): String {
    val ptt = kind == HistoryKind.PTT
    return buildString {
        append(date)
        if (hour != null) append(" %02d시".format(hour))
        if (speechOnly) append(" 발언 있는 세션")
        append(" · ${list.shown}건")
        if (list.live > 0) append(" · 진행중 ${list.live}")
        if (list.recorded > 0) append(" · 녹취 ${list.recorded}건")
        if (ptt && list.video > 0) append(" · 영상 ${list.video}건")
        if (ptt && list.speechMs > 0) append(" · 발화 합 ${fmtSpeech(list.speechMs.coerceAtMost(Int.MAX_VALUE.toLong()).toInt())}")
        if (ptt && list.silent > 0) append(" · 빈 세션 ${list.silent}건" + if (list.bundles > 0) " → ${list.bundles}묶음" else "")
    }
}

// ── 시간대 밴드 ─────────────────────────────────────────────────────────────

/**
 * 시간대 밴드 한 칸 — 값·상대 농도(0 = 없음, 0.08~0.45 = 많을수록 진하게 — 글자가 늘 읽히는 연한 남색 범위).
 * [partial] = 목록 상한으로 덜 센 칸(값 뒤 "+").
 */
data class HourCell(val hour: Int, val count: Int, val ratio: Float, val partial: Boolean = false) {
    val countText: String get() = if (count > 0 || partial) "$count${if (partial) "+" else ""}" else ""
}

private fun cellOf(hour: Int, count: Int, max: Int, partial: Boolean = false) =
    HourCell(hour, count, if (count > 0 && max > 0) 0.08f + 0.37f * count / max else 0f, partial)

/** [세션 수]·통화 — 서버 `hours`(절삭 전 전체) 그대로. */
internal fun sessionCells(band: IntArray): List<HourCell> {
    val max = band.maxOrNull() ?: 0
    return (0..23).map { h -> cellOf(h, band.getOrElse(h) { 0 }, max) }
}

/**
 * [발언 수] — 받은 세션들의 발언 턴을 **세션 시작 시간대**에 더한다(턴의 실제 시각으로 나누지 않는다 — 밴드·목록 묶음과 같은 축).
 *
 * 서버 `hours` 는 `limit` 절삭 **전** 세션 수라, 받은 세션이 그보다 적은 시간대(하루 상한을 넘은 날의 이른 시간)는 덜 센 값이다
 * → 그 칸에 "+" 를 붙인다. 그 시간대를 눌러 한 시간 창으로 다시 받았으면([hourItems]) 그 칸은 그 결과로 센다.
 *
 * @param dayItems 하루 창으로 받은 항목.
 * @param band 서버 `hours`(시간대 → 세션 수).
 * @param hourItems 시간대를 좁혀 다시 받은 항목(시 → 항목).
 */
internal fun turnCells(dayItems: List<HistoryEntry>, band: IntArray,
                       hourItems: Map<Int, List<HistoryEntry>> = emptyMap()): List<HourCell> {
    val byHour = dayItems.groupBy { hourOf(it.axisAtMs) }
    val turns = IntArray(24); val loaded = IntArray(24)
    for (h in 0..23) {
        val src = hourItems[h] ?: byHour[h].orEmpty()
        turns[h] = src.sumOf { it.turnCount }
        loaded[h] = src.size
    }
    val max = turns.maxOrNull() ?: 0
    return (0..23).map { h -> cellOf(h, turns[h], max, partial = loaded[h] < band.getOrElse(h) { 0 }) }
}
