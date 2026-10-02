// 관제 세션의 전화 평면 — dialog 감시·대표번호·픽업·전달 (android_dispatch_tablet.md §6.7,
//                                                    dispatch_center.md §4·§5.2)
//
// 관제석은 자기 통화만 보는 게 아니라 **감시 범위의 통화를 본다**(BLF). 그 소스는 RFC 4235 dialog
// 이벤트이며, 인가는 서버가 역할 `monitorCall` 로 판정한다 — 앱은 `dispatch.members[]` 를 구독할 뿐이다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.DialogInfo
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** dialog 한 줄 — 그룹원 띠·대표번호 대기열·⑥ 내역의 소스. */
data class DialogRow(
    /** 감시 대상 AoR(entity) — 내선 또는 대표번호. */
    val watched: String,
    val id: String,
    val info: DialogInfo,
    val firstSeenMs: Long = System.currentTimeMillis(),
    val stateSinceMs: Long = System.currentTimeMillis(),
    /** 한 번이라도 confirmed 였는가 — 대표번호 부재(전원 무응답) 판정. */
    val wasConfirmed: Boolean = false,
    /** 이 행을 처음 본 때 — ⑥ 내역의 «시작». */
    val startedAtMs: Long = System.currentTimeMillis(),
    /** 처음 confirmed 가 된 때 — ⑥ 내역의 «응답». 못 받았으면 null. */
    val confirmedAtMs: Long? = null,
    /** 대표번호 행에서만: 같은 발신자와 확립된 그룹원 회선(포크 승자). */
    val answeredBy: String = "",
) {
    val key: String get() = "$watched|$id"
    val state: String get() = info.state
    val isEarly: Boolean get() = info.state in setOf("early", "proceeding", "trying")
    val isConfirmed: Boolean get() = info.state == "confirmed"
    val isTerminated: Boolean get() = info.state == "terminated"

    /**
     * 화면에 세울 수 있는 행인가 — **아는 상태만** 참이다(RFC 4235 §4.1 dialog state).
     *
     * 모르는 값(빈 문자열 포함)을 «진행 중» 으로 두면 상태 전이가 오지 않아 영원히 남는다.
     */
    val isLive: Boolean get() = isEarly || isConfirmed
    /** 착신 leg 인가 — 픽업 대상 판정(RFC 4235 direction). */
    val isIncomingLeg: Boolean get() = info.direction == "recipient"
    val elapsedMs: Long get() = System.currentTimeMillis() - stateSinceMs

    companion object {
        /**
         * 처음 본 dialog 의 행. **이미 confirmed 로** 보였으면(로그인·재구독 때 진행 중이던 통화의 full 스냅샷) 응답된 통화다 —
         * 그렇게 세우지 않으면 그 통화가 끝날 때 «부재»·«전원 무응답» 으로 적히고 오늘 부재 집계에 든다(데스크톱 `OnDialog`).
         */
        fun of(d: DialogInfo): DialogRow = DialogRow(d.watched, d.id, d).let {
            if (d.state == "confirmed") it.copy(wasConfirmed = true, confirmedAtMs = it.startedAtMs) else it
        }
    }

    fun apply(d: DialogInfo): DialogRow = copy(
        info = d,
        stateSinceMs = if (d.state != info.state) System.currentTimeMillis() else stateSinceMs,
        // 응답 시각은 **처음 confirmed 가 된 때**다 — 보류·재개로 상태가 오가도 바뀌지 않는다.
        confirmedAtMs = confirmedAtMs ?: if (d.state == "confirmed") System.currentTimeMillis() else null,
        wasConfirmed = wasConfirmed || d.state == "confirmed")
}

/**
 * ⑥ 통화 내역 한 줄.
 *
 * 콘솔·[이력] 화면과 **같은 축**을 든다 — 시작 · 응답 · 종료 · 통화시간(dispatch_desktop_ui.md §4.6).
 * 한 줄만 보고도 «언제 걸려 와서 얼마나 울렸고 몇 분 통화했는지» 를 알 수 있어야 한다.
 */
data class CallLogRow(
    /** **종료 시각** — 정렬 키이자 «종료» 열. */
    val atMs: Long,
    /** **표시용** 이름(없으면 번호). 다시 걸 때 이 값을 쓰지 않는다. */
    val peer: String,
    val text: String,
    val kind: CallLogKind,
    /** 내가 아닌 감시 대상의 통화 — 데스크 집계에서 뺀다. */
    val others: Boolean = false,
    /**
     * **다시 걸 때 쓰는 번호.** 표시(`peer`)와 갈라 둔다 — 이름이 잡히면 `peer` 는 "이순경" 이 되고,
     * 그걸로 다이얼하면 걸리지 않는다(identifier_model.md — 동작은 id 로, 표시는 이름으로).
     */
    val number: String = "",
    /** 시작 — 착신은 링잉 시작, 발신은 INVITE. */
    val startedAtMs: Long = atMs,
    /** 응답 — 못 받았으면 null(부재·미응답). */
    val answeredAtMs: Long? = null,
    /**
     * **대표번호를 거쳐 온 호인가** — ⑥ 의 «대표번호» 필터 축(데스크톱 `ActivityRow.IsPilot`).
     *
     * 직접 착신과 가르는 이유는 책임이 다르기 때문이다 — 대표번호 호는 그룹 전원이 울리고 누가 받았는지가
     * 따로 있다. 섞어 보면 «내가 놓친 것» 과 «동료가 받은 것» 이 구분되지 않는다.
     */
    val viaPilot: Boolean = false,
    /**
     * 세션이 붙이는 순번(`addCallLog`) — **목록 키**다. 시각·상대로 키를 만들면 포크 leg 여럿이 같은 ms 에 끝나거나 서버 이력이
     * 초 단위로 같은 두 사람의 줄을 줄 때 키가 겹쳐 화면이 죽는다. 0 = 아직 안 붙었다(미리보기 픽스처).
     */
    val id: Long = 0,
) {
    val endedAtMs: Long get() = atMs

    /** 목록에서 이 행을 가리키는 키 — 순번이 있으면 그것, 없으면 시각·상대(픽스처). */
    val rowKey: String get() = if (id != 0L) "#$id" else atMs.toString() + number + peer

    /** **통화 시간** = 응답~종료. 응답하지 못한 호는 0 이다(울린 시간과 섞지 않는다). */
    val durationSec: Int
        get() = answeredAtMs?.let { ((atMs - it) / 1000L).toInt().coerceAtLeast(0) } ?: 0

    /** **울린 시간** = 시작~응답(못 받았으면 시작~종료). 부재가 얼마나 방치됐는지가 보인다. */
    val ringSec: Int
        get() = (((answeredAtMs ?: atMs) - startedAtMs) / 1000L).toInt().coerceAtLeast(0)

    val answered: Boolean get() = answeredAtMs != null

    /** "이순경 1002" — 이름이 없거나 번호와 같으면 하나만(§3.2 신원 표시). */
    val label: String get() = when {
        number.isBlank() -> peer
        peer.isBlank() || peer == number -> number
        else -> "$peer $number"
    }

    /**
     * 행에서 곧바로 다시 걸고 문자를 보낼 수 있는가 — 상대가 있는 1:1 통화(착신·부재·발신·전달)만. 당겨받기는 남의 호를
     * 가져온 것이고 감청은 통화 당사자가 아니다(데스크톱 `ActivityRow.CanRedial` 과 같은 종류).
     */
    val canRedial: Boolean get() = number.isNotBlank() &&
        kind in setOf(CallLogKind.ANSWERED, CallLogKind.MISSED, CallLogKind.OUTGOING, CallLogKind.TRANSFER)
}

/**
 * ⑥ 통화내역 CSV — 화면 표와 같은 열(시작·상대·번호·종류·응답·종료·통화·울림 + 대표번호 경유·감시 대상·비고), 시간순.
 * 엑셀이 한글을 깨지 않게 UTF-8 BOM 은 쓰는 쪽이 붙인다(데스크톱 `ActivityLog.ExportCsv` 와 같은 규약).
 */
internal fun callLogCsv(rows: List<CallLogRow>, zone: java.util.TimeZone = java.util.TimeZone.getDefault()): String {
    val t = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).apply { timeZone = zone }
    fun q(s: String) = "\"" + s.replace("\"", "\"\"") + "\""
    val sb = StringBuilder("start,peer,number,kind,answer,end,talk_sec,ring_sec,via_pilot,others,note\r\n")
    rows.sortedBy { it.startedAtMs }.forEach { r ->
        sb.append(t.format(java.util.Date(r.startedAtMs))).append(',')
            .append(q(r.peer)).append(',').append(q(r.number)).append(',').append(q(callLogKindText(r.kind))).append(',')
            .append(r.answeredAtMs?.let { t.format(java.util.Date(it)) }.orEmpty()).append(',')
            .append(t.format(java.util.Date(r.endedAtMs))).append(',')
            .append(r.durationSec).append(',').append(r.ringSec).append(',')
            .append(if (r.viaPilot) 1 else 0).append(',').append(if (r.others) 1 else 0).append(',')
            .append(q(r.text)).append("\r\n")
    }
    return sb.toString()
}

/** 종류 낱말 — ⑥ 표와 CSV 가 같은 말을 쓴다. */
internal fun callLogKindText(k: CallLogKind): String = when (k) {
    CallLogKind.ANSWERED -> "착신 응답"
    CallLogKind.MISSED -> "부재"
    CallLogKind.OUTGOING -> "발신"
    CallLogKind.PICKUP -> "당겨받기"
    CallLogKind.TRANSFER -> "전달"
    CallLogKind.MONITOR -> "감청"
    CallLogKind.SMS -> "문자"
}

/** [SMS] = 감시 대상끼리 주고받은 1:1 문자(서버 통합 이력이 준다 — `HistoryFeed`). 통화가 아니라 데스크 집계에 들지 않는다. */
enum class CallLogKind { ANSWERED, MISSED, OUTGOING, PICKUP, TRANSFER, MONITOR, SMS }

/** 오늘 데스크 집계 — ③ 상단 칩. */
data class DeskTally(
    val answered: Int = 0, val missed: Int = 0, val outgoing: Int = 0,
    val transfer: Int = 0, val monitor: Int = 0,
)

// ── 동작 ──────────────────────────────────────────────────────────────────────

/** 발신 — 번호 또는 SIP URI. 전화 계정으로 건다. */
suspend fun DispatchSession.dial(target: String): CimsResult<Unit> {
    val a = phoneAccount ?: return report(TextArea.CALL, CimsResult.fail(-1, "전화 계정 없음"))
    val r = a.dial(dialTargetOf(target, phoneBook.value))
    if (r.ok) noteOperation(r.value!!.id, Operation.DIAL)
    return report(TextArea.CALL, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/**
 * 번호칸에 친 것 → 걸 대상(순수 함수, 시험 대상 — 데스크톱 `CallOriginateViewModel.Resolve`·`Dial`).
 *
 * - URI(`sip:`·`tel:`·`@`)는 그대로.
 * - **이름**이면 주소록의 그 사람 번호(한 사람으로 정해질 때만 — 동명이인이면 친 대로 두어 서버가 거절하게 한다).
 * - 번호면 구분자(공백·`-`·`(`·`)`·`.`)를 뺀다. 피처코드(`*`·`#`)는 거기까지.
 * - 주소록에 있는 번호면 **저장된 원 번호**로 건다 — `010…` 으로 쳐도 가입 id(`+8210…`)로 나간다.
 */
internal fun dialTargetOf(input: String, book: DirectoryBook): String {
    val t = input.trim()
    if (t.isEmpty() || t.contains(':') || t.contains('@')) return t
    val isNumber = t.all { it.isDigit() || it in "+*#-(). " }
    if (!isNumber) return book.entries.filter { it.name == t && it.msisdn.isNotBlank() }
        .map { e -> e.msisdn.filter { it.isDigit() || it == '+' }.ifEmpty { e.msisdn } }.distinct().singleOrNull() ?: t
    val digits = t.filter { it.isDigit() || it == '+' || it == '*' || it == '#' }
    if (digits.isEmpty() || '*' in digits || '#' in digits) return digits
    val n = DirectoryBook.normalize(digits)
    // 주소록의 번호도 구분자를 뺀다 — 로컬 CSV 줄은 `010-1234-5678` 처럼 원문 그대로 들어 있다
    return book.entries.firstOrNull { it.msisdn.isNotBlank() && DirectoryBook.normalize(it.msisdn) == n }
        ?.msisdn?.filter { it.isDigit() || it == '+' }?.ifEmpty { null } ?: digits
}

/**
 * 당겨받기 — 그룹 픽업은 피처코드만, 지정 픽업은 번호까지(TS 24.239).
 * 응답은 호 상태로 온다(200 성공 / 403 권한 / 404 대상 없음 / 489 이미 응답됨).
 */
suspend fun DispatchSession.pickup(number: String = ""): CimsResult<Unit> {
    val a = phoneAccount ?: return report(TextArea.PICKUP, CimsResult.fail(-1, "전화 계정 없음"))
    val code = settingsSnapshot().pickupFeatureCode
    val r = a.pickup(code, number)
    if (r.ok) noteOperation(r.value!!.id, Operation.PICKUP)
    return report(TextArea.PICKUP, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/** 착신 응답. */
suspend fun DispatchSession.answer(callId: Int): CimsResult<Unit> =
    report(TextArea.CALL, engineOrNull()?.call(callId)?.answer() ?: CimsResult.fail(-1, "엔진 없음"))

suspend fun DispatchSession.hangup(callId: Int): CimsResult<Unit> {
    noteLocalHangup(callId)
    return report(TextArea.CALL, engineOrNull()?.call(callId)?.hangup() ?: CimsResult.fail(-1, "엔진 없음"))
}

suspend fun DispatchSession.hold(callId: Int, on: Boolean): CimsResult<Unit> {
    val c = engineOrNull()?.call(callId) ?: return report(TextArea.CALL, CimsResult.fail(-1, "엔진 없음"))
    // 보류를 풀면 그 통화가 활성 통화다 — 자동 보류가 켜져 있으면 말하던 다른 통화를 먼저 보류한다(두 통화가 동시에 들리지 않게)
    if (!on && settingsSnapshot().autoHoldOnAnswer) holdOtherCalls(callId)
    return report(TextArea.CALL, if (on) c.hold() else c.resume())
}

/**
 * 호 하나의 수신 음량(1.0 = 원음, 0~2) — 감청·청취 행의 음량 막대(데스크톱 감청 창 `Volume` → `SetRxLevel`). 끌어 가는
 * 동안 계속 오므로 토스트를 띄우지 않는다(데스크톱도 결과를 보지 않는다) — 미디어가 아직 없으면 코어가 거절하고 값만 남는다.
 */
suspend fun DispatchSession.setRxLevel(callId: Int, level: Float): CimsResult<Unit> {
    val v = level.coerceIn(0f, 2f)
    noteRxLevel(callId, v)
    return engineOrNull()?.call(callId)?.setRxLevel(v) ?: CimsResult.fail(-1, "엔진 없음")
}

suspend fun DispatchSession.setMuted(callId: Int, muted: Boolean): CimsResult<Unit> =
    report(TextArea.CALL, engineOrNull()?.call(callId)?.setMuted(muted) ?: CimsResult.fail(-1, "엔진 없음"))

/**
 * 음소거 토글 — **뒤집을 값을 명령 직전의 코어 스냅샷에서 읽고, 토글끼리 줄을 세운다.**
 *
 * 화면이 들고 있는 값(`CallInfo.muted` 사본)으로 뒤집으면, 스냅샷이 다시 오기 전에 두 번 누른 것이 같은 값
 * 두 번으로 합쳐져 **두 번 눌렀는데 한 번 누른 상태**로 남는다. 줄을 세우면 뒤 누름은 앞 명령이 끝난 뒤의 코어
 * 값을 읽는다. 명령 뒤에는 스냅샷을 다시 당긴다 — 코어가 `onCallMedia` 로도 알리지만 구형 엔진과 섞여도 화면이
 * 멎지 않게. ③ 통화 카드와 ① 전이중 개별 통화 카드가 이 한 경로를 쓴다.
 */
suspend fun DispatchSession.toggleMuted(callId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return muteToggle.toggle(
        read = { ue.callInfo(callId)?.muted },
        write = { setMuted(callId, it).also { refreshSessions() } })
}

/**
 * 뒤집기 명령의 줄 — 읽기와 쓰기 사이에 다른 뒤집기가 끼어들지 못하게 한다.
 *
 * 읽기·쓰기를 주입받는 것은 시험 때문이다 — 엔진 없이 «연타가 합쳐지지 않는다» 를 고정한다.
 */
internal class SerialToggle {
    private val lock = Mutex()

    /** [read] 가 null 이면(호가 없다) 쓰지 않는다. */
    suspend fun toggle(read: () -> Boolean?, write: suspend (Boolean) -> CimsResult<Unit>): CimsResult<Unit> =
        lock.withLock {
            val cur = read() ?: return@withLock CimsResult.fail(-1, "호 없음")
            write(!cur)
        }
}

suspend fun DispatchSession.sendDtmf(callId: Int, digits: String): CimsResult<Unit> =
    report(TextArea.CALL, engineOrNull()?.call(callId)?.sendDtmf(digits) ?: CimsResult.fail(-1, "엔진 없음"))

/**
 * 호 전달 blind — REFER(RFC 3515). 서버가 수락하면 우리 leg 은 BYE 로 끝난다. 받아들여지면 카드에 «전달 중 → …» 를
 * 적고 ⑥ 에 전달로 남긴다(데스크톱 `TransferBlind`).
 */
suspend fun DispatchSession.transfer(callId: Int, target: String): CimsResult<Unit> {
    val t = target.trim()
    val r = report(TextArea.TRANSFER,
        engineOrNull()?.call(callId)?.transfer(t) ?: CimsResult.fail(-1, "엔진 없음"))
    if (r.ok) {
        noteTransfer(callId, "전달 중 → ${displayLabel(t)}")
        addCallLog(CallLogRow(atMs = System.currentTimeMillis(), peer = displayName(t), text = "전달 (blind)",
            kind = CallLogKind.TRANSFER, number = userPart(t)))
    }
    return r
}

/**
 * 상담 전달 시작 — 원 통화를 보류하고 대상에게 상담 호를 건다(데스크톱 `StartConsult`). 상담 호 카드가 «상담» 으로 서고,
 * 연결되면 [전달 완결] 로 원 통화를 넘긴다(Replaces — RFC 3891, [completeConsult]).
 */
suspend fun DispatchSession.startConsult(originalCallId: Int, target: String): CimsResult<Unit> {
    val a = phoneAccount ?: return report(TextArea.TRANSFER, CimsResult.fail(-1, "전화 계정 없음"))
    val t = target.trim()
    if (t.isEmpty()) return report(TextArea.TRANSFER, CimsResult.fail(-1, "전달 대상을 입력하세요"))
    if (sessionOf(originalCallId)?.isActive == true) hold(originalCallId, true)
    val r = a.dial(t)
    if (r.ok) {
        val id = r.value!!.id
        noteOperation(id, Operation.TRANSFER)
        noteConsult(id, originalCallId)
    }
    return report(TextArea.TRANSFER, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/** [전달 완결] — 상담 호의 dialog 로 원 통화를 넘긴다(REFER + Replaces, 데스크톱 `CompleteConsult`). */
suspend fun DispatchSession.completeConsult(consultCallId: Int): CimsResult<Unit> {
    val consult = sessionOf(consultCallId)
    val original = consult?.consultFor
        ?: return report(TextArea.TRANSFER, CimsResult.fail(-1, "상담 호가 아닙니다"))
    val r = transferAttended(original, consultCallId)
    if (r.ok) noteTransfer(original, "전달 중 → ${consult.title.ifEmpty { userPart(consult.info.remoteUri) }}")
    return r
}

/** 상담 취소 — 상담 호를 끊고 원 통화의 보류를 푼다(데스크톱 `CancelConsult`). */
suspend fun DispatchSession.cancelConsult(consultCallId: Int): CimsResult<Unit> {
    val original = sessionOf(consultCallId)?.consultFor
    val r = hangup(consultCallId)
    if (original != null && sessionOf(original)?.info?.state == com.cims.ue.sdk.CallState.HELD) hold(original, false)
    return r
}

/** 호 전달 attended — 상담 호의 dialog 를 Replaces 로 넘긴다. */
suspend fun DispatchSession.transferAttended(callId: Int, consultCallId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return report(TextArea.TRANSFER, CimsResult.fail(-1, "엔진 없음"))
    return report(TextArea.TRANSFER, ue.call(callId).transferAttended(ue.call(consultCallId)))
}

/**
 * 통화 청취 합류 — INVITE-with-Join(RFC 3911) + `a=recvonly`.
 *
 * 인가는 서버가 한다(역할 `monitorCall` 또는 같은 전화 그룹 BLF, dispatch_center.md §5.3).
 * 200 OK 의 `a=ssrc … label`(RFC 5576)로 두 화자가 구분돼 `MediaSources` 로 온다.
 */
suspend fun DispatchSession.joinMonitor(row: DialogRow): CimsResult<Unit> {
    val a = phoneAccount ?: return report(TextArea.JOIN, CimsResult.fail(-1, "전화 계정 없음"))
    // 상한은 앱에서 먼저 본다 — 데스크톱도 같은 자리에서 막는다(`DispatchSession.JoinMonitor`).
    if (listenLimitReached())
        return report(TextArea.JOIN, CimsResult.fail(-1, "동시 청취 상한 ${settingsSnapshot().maxListen}"))
    // dialog NOTIFY 의 `entity` 는 `tel:` 일 수 있다 — 그대로 넘기면 Join INVITE 가 라우팅되지 않는다.
    val r = a.join(routableTarget(row.watched), row.info)
    if (r.ok) noteOperation(r.value!!.id, Operation.JOIN)
    return report(TextArea.JOIN, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/**
 * 감시 대상 전원 dialog 구독 — 대표번호 + `dispatch.members[]`.
 *
 * 대상 목록은 **서버가 준다**(CSC 가 `monitorCall` 을 CSP 게이트와 같은 규칙으로 해석한 결과).
 * 앱이 범위를 해석하지 않는다. PTT 전용 가입자(`volteAor` 가 빈 것)는 전화 감시에서 뺀다.
 */
/**
 * 요청 대상 정규형 — **`tel:` 을 벗긴다.**
 *
 * CSC 는 구성원 주소를 `tel:` URI 로 내고(`csc/src/services/mcptt.py` `_tel_uri`) 대표번호는 스킴 없이
 * 낸다(같은 파일, `"pilotId": r[2]`). 그런데 코어의 요청 URI 정규화는 `tel:` 을 **그대로 둔다**
 * (`sdk/core/src/account_map.cpp` `normalizeTarget`) — `tel:` 은 호스트가 없어 **라우팅할 수 없는
 * Request-URI** 라 SUBSCRIBE 가 나가지 못한다. 맨 번호로 넘기면 코어가 `sip:<번호>@<도메인>` 을 만든다.
 *
 * 이것이 «대표번호 1건만 구독되고 구성원 46건은 NOTIFY 를 하나도 못 받던» 원인이다.
 * 이미 `sip:`/`sips:` 인 주소는 그대로 둔다 — 도메인이 실려 있어 그 자체로 라우팅된다.
 *
 * 구독(`dialogWatch`)만이 아니라 **감청 Join**(`Engine::join`)도 같은 `normalizeTarget` 을 타므로
 * 같은 함정에 빠진다 — dialog NOTIFY 의 `entity` 가 `tel:` 이면 Join INVITE 가 못 나간다.
 */
internal fun routableTarget(aor: String): String {
    val t = aor.trim()
    return if (t.startsWith("tel:", ignoreCase = true)) t.substring(4).trim() else t
}

suspend fun DispatchSession.watchAll(): CimsResult<Unit> {
    val a = phoneAccount ?: return CimsResult.fail(-1, "전화 계정 없음")
    val targets = watchTargets(dispatch)
    // **목록을 먼저 세우고 그다음 구독한다.** 순서를 뒤집으면 구독 도중 도착한 NOTIFY 를 뒤이은
    // 초기화가 지운다 — RFC 6665 상 SUBSCRIBE 직후 NOTIFY 가 오므로, 대상이 수십이면 거의 전부가
    // 루프 안에서 도착한다. 그러면 실제로는 성립한 구독이 «0 성립» 으로 보인다.
    setWatched(targets)
    val gen = loginGeneration.value
    val failed = targets.filterNot { a.dialogWatch(it, true).ok }
    // 구독하는 사이 로그아웃 — 지운 계정의 구독은 전부 실패로 돌아온다. 그것을 알리면 비운 화면에 경고가 서고, 깃발이 다음
    //   로그인의 첫 경고를 삼킨다.
    if (gen != loginGeneration.value) return CimsResult.fail(-1, "로그아웃됨")
    // 실패한 회선은 «구독 중» 목록에서 뺀다 — 남겨 두면 다음 편성 재조회의 차분(`want - had`)에서 빠져 재로그인 전까지 다시
    //   걸리지 않는다. 빼 두면 60초 재조회가 다시 건다([retryWatch]).
    if (failed.isNotEmpty()) setWatched(targets - failed.toSet())
    noteWatchFailures(failed)
    return CimsResult.ok(Unit)
}

/**
 * 감시 대상 — 대표번호를 **먼저**(대기열은 이 구독 하나에 달렸다), 그다음 그룹원 회선. 구독 슬롯이 모자라면 앞쪽이 산다.
 * PTT 전용 가입자(`volteAor` 가 빈 것)는 전화 감시에서 뺀다. 순서를 지키려고 삽입 순서 집합을 쓴다.
 */
internal fun watchTargets(d: com.cims.ue.sdk.DispatchProfile): Set<String> = buildSet {
    if (!d.present) return@buildSet
    if (d.pilotId.isNotEmpty()) add(routableTarget(d.pilotId))
    d.members.forEach { m -> if (m.volteAor.isNotEmpty()) add(routableTarget(m.volteAor)) }
}

/**
 * 편성이 바뀌었다 — **차분만** 다시 건다(데스크톱 `RefreshDispatchAsync`). 빠진 회선은 구독을 풀고 그 행을 치우며, 새 회선만
 * 구독한다. 남은 회선은 건드리지 않는다 — 다시 걸면 진행 중 통화의 행이 초기 스냅샷으로 한 번 비었다 찬다.
 */
internal suspend fun DispatchSession.rewatch() {
    val a = phoneAccount ?: return
    val want = watchTargets(dispatch)
    val had = watchedTargets()
    val gen = loginGeneration.value
    setWatched(want)
    (had - want).forEach { aor ->
        a.dialogWatch(aor, false)
        setDialogs(dialogs.value.filterNot { userPart(it.watched) == userPart(aor) })
    }
    val failed = (want - had).filterNot { a.dialogWatch(it, true).ok }
    if (gen != loginGeneration.value) return               // 그사이 로그아웃 — 비운 목록·깃발을 건드리지 않는다
    if (failed.isNotEmpty()) setWatched(want - failed.toSet())
    noteWatchFailures(failed)
}

/** 걸리지 못한 감시 구독이 있으면 다시 건다 — 편성 재조회 주기(60초)마다, 편성이 바뀌지 않았어도. */
internal suspend fun DispatchSession.retryWatch() {
    if (phoneAccount != null && watchTargets(dispatch) != watchedTargets()) rewatch()
}

/**
 * 감시 구독이 곧바로 실패했다 — 줄마다 로그하되 관제사에게는 **한 번만** 알린다. 감시 누락은 조용히 넘기면 안 되는 상태다
 * (그룹원 상태·대기열이 빈 채로 «통화 없음» 처럼 보인다).
 */
private fun DispatchSession.noteWatchFailures(failed: List<String>) {
    if (failed.isEmpty()) { watchFailureNoted = false; return }
    failed.forEach { android.util.Log.w("DispatchSession", "dialogWatch $it 실패") }
    // 다시 걸 때마다(60초) 또 알리지 않는다 — 실패가 이어지는 동안은 처음 한 번만
    if (watchFailureNoted) return
    watchFailureNoted = true
    notify(NoticeLevel.WARN, "회선 감시 구독 실패 ${failed.size}건",
        "${failed.first()} 등 — 그룹원 상태·대기열이 빠질 수 있습니다(주기적으로 다시 겁니다)")
}

// ── 이벤트 접기 ───────────────────────────────────────────────────────────────

/** dialog NOTIFY → 감시 행. terminated 는 잠시 남겼다가 지운다(대기열이 사라지는 걸 보이게). */
internal fun DispatchSession.applyDialog(d: DialogInfo) {
    // **구독 성립 신호** — 코어는 dialog 가 하나도 없는 full 스냅샷을 «id·state 가 빈» DialogInfo 로 낸다
    // (`engine.cpp` "초기 full 스냅샷에 dialog 없음"). 이것을 행으로 만들면 상태 전이가 영영 오지 않아
    // ⑥ 에 «연결 중» 이 무한히 남는다(대표번호 AoR 이 그렇게 보였다). 행이 아니라 **그 AoR 에 통화가
    // 없다는 사실**이므로, 남아 있던 행을 치우고 끝낸다.
    // 판정은 **state** 로만 한다. `id` 는 RFC 4235 상 필수지만 서버가 빠뜨려도 진짜 dialog 는
    // `<state>` 를 싣는다 — id 까지 조건에 넣으면 멀쩡한 통화를 지운다.
    if (d.state.isBlank()) {
        if (d.watched.isNotEmpty())
            setDialogs(dialogs.value.filterNot { userPart(it.watched) == userPart(d.watched) })
        return
    }
    val key = d.watched + "|" + d.id
    val cur = dialogs.value
    fun prune(list: List<DialogRow>) = list.filterNot { it.isTerminated && it.elapsedMs > TERMINATED_KEEP_MS }
    if (isPilot(d.watched)) notePilotCaller(userPart(d.remoteIdentity))

    if (d.state == "terminated") {
        // dialog id(Call-ID+태그)는 양 당사자에게 같은 하나의 dialog 다 — 종료는 entity 가 무엇이든 **그 id 의 행 전부**에 적용한다.
        //   서버가 대표번호 포크 호의 종료 NOTIFY 를 다른 회선의 entity 로 붙여 보내는 경우, entity|id 키만 보면 «통화 중» 행이
        //   남는다(데스크톱 `OnDialog`). 못 보던 dialog 의 종료는 버린다.
        //   id 가 비면(서버가 빠뜨림) 그 회선의 그 행만 — 빈 id 끼리 묶으면 무관한 통화가 함께 끝난다.
        fun same(r: DialogRow) = r.key == key || (d.id.isNotEmpty() && r.id == d.id)
        val ended = cur.filter { same(it) && !it.isTerminated }
        setDialogs(prune(cur.map { r ->
            when {
                !same(r) || r.isTerminated -> r
                r.key == key -> r.apply(d)
                else -> r.apply(r.info.copy(state = "terminated"))
            }
        }))
        ended.forEach(::noteDialogEnd)                    // ⑥ 내역 — 전이만 남긴다(진행 중은 카드가 보여준다)
        return
    }

    var next = if (cur.any { it.key == key }) cur.map { if (it.key == key) it.apply(d) else it }
               else cur + DialogRow.of(d)
    // 대표번호 호의 응답자(포크 승자) — 그룹원 회선이 같은 발신자와 confirmed 되면 그 회선이 받은 것이다. 대표번호·회선의
    //   confirmed 순서는 서버가 정하지 않으므로 어느 쪽이 먼저 와도 맞물리게 양쪽에서 본다.
    val row = next.first { it.key == key }
    if (row.isConfirmed) {
        val caller = userPart(row.info.remoteIdentity)
        if (!isPilot(row.watched))
            next = next.map { p ->
                if (isPilot(p.watched) && p.answeredBy.isEmpty() && userPart(p.info.remoteIdentity) == caller)
                    p.copy(answeredBy = userPart(row.watched)) else p
            }
        else if (row.answeredBy.isEmpty())
            next.firstOrNull { !isPilot(it.watched) && it.isConfirmed && userPart(it.info.remoteIdentity) == caller }?.let { w ->
                next = next.map { if (it.key == key) it.copy(answeredBy = userPart(w.watched)) else it }
            }
    }
    setDialogs(prune(next))
}

/**
 * 끝난 dialog 의 ⑥ 내역 한 줄.
 *
 * - **대표번호 dialog 가 대표번호 호의 정본이다**(데스크톱 `RecordPilotOutcome`): 전원 무응답 = «부재» 한 줄, 동료가 받았으면
 *   «응답 <회선>» 한 줄. 둘 다 데스크의 일이라 오늘 집계에 든다. 내가 받은 호는 내 세션이 남기므로 여기서 적지 않는다.
 * - 대표번호 포크의 **그룹원 leg** 은 적지 않는다 — 받지 않은 회선마다 «부재» 가 서면 한 호가 여러 줄이 되고, 동료가 받은
 *   호가 부재로 읽힌다.
 * - 그 밖의 감시 대상 통화는 타인 통화 한 줄. **내 통화는 내 세션이 권위**라 `applyCallState` 가 남긴다 — 둘 다 남기면 같은
 *   통화가 두 줄이 된다.
 */
private fun DispatchSession.noteDialogEnd(r: DialogRow) {
    val now = System.currentTimeMillis()
    val caller = userPart(r.info.remoteIdentity)
    if (isPilot(r.watched)) {
        when {
            !r.wasConfirmed -> addCallLog(CallLogRow(
                atMs = now, peer = displayName(r.info.remoteIdentity), number = caller,
                text = "전원 무응답", kind = CallLogKind.MISSED, startedAtMs = r.startedAtMs, viaPilot = true))
            r.answeredBy.isNotEmpty() && isMine(r.answeredBy) -> Unit
            else -> addCallLog(CallLogRow(
                atMs = now, peer = displayName(r.info.remoteIdentity), number = caller,
                text = "응답 " + if (r.answeredBy.isNotEmpty()) displayLabel(r.answeredBy) else "그룹원(미상)",
                kind = CallLogKind.ANSWERED, startedAtMs = r.startedAtMs, answeredAtMs = r.confirmedAtMs, viaPilot = true))
        }
        return
    }
    if (isMine(r.watched) || (r.isIncomingLeg && isPilotFork(caller, r.startedAtMs))) return
    addCallLog(CallLogRow(
        atMs = now,
        peer = displayName(r.info.remoteIdentity),
        number = caller,
        text = if (r.wasConfirmed) "통화 종료" else "부재",
        kind = if (r.wasConfirmed) CallLogKind.ANSWERED else CallLogKind.MISSED,
        others = true,
        startedAtMs = r.startedAtMs,
        answeredAtMs = r.confirmedAtMs))
}

/** 이 발신자의 호가 대표번호 포크인가 — 대표번호 dialog 가 살아 있거나 방금까지 있었다. */
internal fun DispatchSession.isPilotFork(caller: String, legStartedAtMs: Long): Boolean {
    if (caller.isEmpty()) return false
    if (dialogs.value.any { isPilot(it.watched) && !it.isTerminated && userPart(it.info.remoteIdentity) == caller }) return true
    // 대표번호 dialog 가 방금 끝났다 — 그 dialog 가 살아 있는 동안 **시작한** leg 만 포크다. 대표번호 호가 끝난 뒤 같은 발신자가
    //   그룹원에게 곧바로 직통으로 건 호까지 삼키지 않는다.
    val seen = pilotCallerSeenAt(caller) ?: return false
    return System.currentTimeMillis() - seen < PILOT_FORK_WINDOW_MS && legStartedAtMs <= seen + 1_000L
}

private const val PILOT_FORK_WINDOW_MS = 15_000L

/** 오늘 데스크 집계 — **오늘 끝난** 내 줄에서 센다(데스크톱 `Today*`). 타인 통화(`others`)는 빼고, 동료가 받은 대표번호 호는 든다. */
internal fun deskTallyOf(rows: List<CallLogRow>, sinceMs: Long): DeskTally {
    var t = DeskTally()
    rows.forEach { r ->
        if (r.others || r.atMs < sinceMs) return@forEach
        t = when (r.kind) {
            CallLogKind.ANSWERED, CallLogKind.PICKUP -> t.copy(answered = t.answered + 1)
            CallLogKind.MISSED -> t.copy(missed = t.missed + 1)
            CallLogKind.OUTGOING -> t.copy(outgoing = t.outgoing + 1)
            CallLogKind.TRANSFER -> t.copy(transfer = t.transfer + 1)
            CallLogKind.MONITOR -> t.copy(monitor = t.monitor + 1)
            CallLogKind.SMS -> t
        }
    }
    return t
}

private const val TERMINATED_KEEP_MS = 3_000L

/**
 * 착신 거절 — 배너·카드의 [거절].
 *
 * 기본 486(Busy Here)이다. 603(Decline)은 «이 단말이 아니라 사용자가 거절했다» 는 뜻이라 서버가
 * 포크 집합을 통째로 접을 수 있어, 대표번호 병렬 호출(TS 24.239)에서는 486 이 맞다 — 다른 관제석은
 * 계속 울려야 한다.
 */
suspend fun DispatchSession.reject(callId: Int, code: Int = 486): CimsResult<Unit> {
    noteLocalHangup(callId)                  // 내가 거절한 호 — 내역에 «부재 · 거절» 로 남는다(놓친 호와 구분)
    return report(TextArea.CALL, engineOrNull()?.call(callId)?.reject(code) ?: CimsResult.fail(-1, "엔진 없음"))
}
