// 관제 세션의 전화 평면 — dialog 감시·대표번호·픽업·전달 (android_dispatch_tablet.md §6.7,
//                                                    dispatch_center.md §4·§5.2)
//
// 관제석은 자기 통화만 보는 게 아니라 **감시 범위의 통화를 본다**(BLF). 그 소스는 RFC 4235 dialog
// 이벤트이며, 인가는 서버가 역할 `monitorCall` 로 판정한다 — 앱은 `dispatch.members[]` 를 구독할 뿐이다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.DialogInfo

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
) {
    val endedAtMs: Long get() = atMs

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
}

enum class CallLogKind { ANSWERED, MISSED, OUTGOING, PICKUP, TRANSFER, MONITOR }

/** 오늘 데스크 집계 — ③ 상단 칩. */
data class DeskTally(
    val answered: Int = 0, val missed: Int = 0, val outgoing: Int = 0,
    val transfer: Int = 0, val monitor: Int = 0,
)

// ── 동작 ──────────────────────────────────────────────────────────────────────

/** 발신 — 번호 또는 SIP URI. 전화 계정으로 건다. */
suspend fun DispatchSession.dial(target: String): CimsResult<Unit> {
    val a = phoneAccount ?: return CimsResult.fail(-1, "전화 계정 없음")
    val r = a.dial(target.trim())
    if (r.ok) noteOperation(r.value!!.id, Operation.DIAL)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
}

/**
 * 당겨받기 — 그룹 픽업은 피처코드만, 지정 픽업은 번호까지(TS 24.239).
 * 응답은 호 상태로 온다(200 성공 / 403 권한 / 404 대상 없음 / 489 이미 응답됨).
 */
suspend fun DispatchSession.pickup(number: String = ""): CimsResult<Unit> {
    val a = phoneAccount ?: return CimsResult.fail(-1, "전화 계정 없음")
    val code = settingsSnapshot().pickupFeatureCode
    val r = a.pickup(code, number)
    if (r.ok) noteOperation(r.value!!.id, Operation.PICKUP)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
}

/** 착신 응답. */
suspend fun DispatchSession.answer(callId: Int): CimsResult<Unit> =
    engineOrNull()?.call(callId)?.answer() ?: CimsResult.fail(-1, "엔진 없음")

suspend fun DispatchSession.hangup(callId: Int): CimsResult<Unit> =
    engineOrNull()?.call(callId)?.hangup() ?: CimsResult.fail(-1, "엔진 없음")

suspend fun DispatchSession.hold(callId: Int, on: Boolean): CimsResult<Unit> {
    val c = engineOrNull()?.call(callId) ?: return CimsResult.fail(-1, "엔진 없음")
    return if (on) c.hold() else c.resume()
}

suspend fun DispatchSession.setMuted(callId: Int, muted: Boolean): CimsResult<Unit> =
    engineOrNull()?.call(callId)?.setMuted(muted) ?: CimsResult.fail(-1, "엔진 없음")

suspend fun DispatchSession.sendDtmf(callId: Int, digits: String): CimsResult<Unit> =
    engineOrNull()?.call(callId)?.sendDtmf(digits) ?: CimsResult.fail(-1, "엔진 없음")

/** 호 전달 blind — REFER(RFC 3515). 서버가 수락하면 우리 leg 은 BYE 로 끝난다. */
suspend fun DispatchSession.transfer(callId: Int, target: String): CimsResult<Unit> =
    engineOrNull()?.call(callId)?.transfer(target.trim()) ?: CimsResult.fail(-1, "엔진 없음")

/** 호 전달 attended — 상담 호의 dialog 를 Replaces 로 넘긴다. */
suspend fun DispatchSession.transferAttended(callId: Int, consultCallId: Int): CimsResult<Unit> {
    val ue = engineOrNull() ?: return CimsResult.fail(-1, "엔진 없음")
    return ue.call(callId).transferAttended(ue.call(consultCallId))
}

/**
 * 통화 청취 합류 — INVITE-with-Join(RFC 3911) + `a=recvonly`.
 *
 * 인가는 서버가 한다(역할 `monitorCall` 또는 같은 전화 그룹 BLF, dispatch_center.md §5.3).
 * 200 OK 의 `a=ssrc … label`(RFC 5576)로 두 화자가 구분돼 `MediaSources` 로 온다.
 */
suspend fun DispatchSession.joinMonitor(row: DialogRow): CimsResult<Unit> {
    val a = phoneAccount ?: return CimsResult.fail(-1, "전화 계정 없음")
    // 상한은 앱에서 먼저 본다 — 데스크톱도 같은 자리에서 막는다(`DispatchSession.JoinMonitor`).
    if (listenLimitReached()) return CimsResult.fail(-1, "동시 청취 상한 ${settingsSnapshot().maxListen}")
    // dialog NOTIFY 의 `entity` 는 `tel:` 일 수 있다 — 그대로 넘기면 Join INVITE 가 라우팅되지 않는다.
    val r = a.join(routableTarget(row.watched), row.info)
    if (r.ok) noteOperation(r.value!!.id, Operation.JOIN)
    return if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason)
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
    val d = dispatch
    val targets = buildSet {
        if (d.pilotId.isNotEmpty()) add(routableTarget(d.pilotId))
        d.members.forEach { m -> if (m.volteAor.isNotEmpty()) add(routableTarget(m.volteAor)) }
    }
    // **목록을 먼저 세우고 그다음 구독한다.** 순서를 뒤집으면 구독 도중 도착한 NOTIFY 를 뒤이은
    // 초기화가 지운다 — RFC 6665 상 SUBSCRIBE 직후 NOTIFY 가 오므로, 대상이 수십이면 거의 전부가
    // 루프 안에서 도착한다. 그러면 실제로는 성립한 구독이 «0 성립» 으로 보인다.
    setWatched(targets)
    targets.forEach { a.dialogWatch(it, true) }
    return CimsResult.ok(Unit)
}

// ── 이벤트 접기 ───────────────────────────────────────────────────────────────

/** dialog NOTIFY → 감시 행. terminated 는 잠시 남겼다가 지운다(대기열이 사라지는 걸 보이게). */
internal fun DispatchSession.applyDialog(d: DialogInfo) {
    // **구독 성립 신호** — 코어는 dialog 가 하나도 없는 full 스냅샷을 «id·state 가 빈» DialogInfo 로 낸다
    // (`engine.cpp` "초기 full 스냅샷에 dialog 없음"). 이것을 행으로 만들면 상태 전이가 영영 오지 않아
    // ⑥ 에 «연결 중» 이 무한히 남는다(대표번호 AoR 이 그렇게 보였다). 행이 아니라 **그 AoR 에 통화가
    // 없다는 사실**이므로, 남아 있던 행을 치우고 끝낸다.
    // NOTIFY 가 왔다 = 그 AoR 의 구독이 성립했다(RFC 6665). dialog 를 실었는지는 따로 센다.
    noteDialogNotify(d.watched, carriedDialog = d.state.isNotBlank())
    // 판정은 **state** 로만 한다. `id` 는 RFC 4235 상 필수지만 서버가 빠뜨려도 진짜 dialog 는
    // `<state>` 를 싣는다 — id 까지 조건에 넣으면 멀쩡한 통화를 지운다.
    if (d.state.isBlank()) {
        if (d.watched.isNotEmpty())
            setDialogs(dialogs.value.filterNot { userPart(it.watched) == userPart(d.watched) })
        return
    }
    val key = d.watched + "|" + d.id
    val cur = dialogs.value
    val prev = cur.firstOrNull { it.key == key }
    val next = when {
        prev != null -> cur.map { if (it.key == key) it.apply(d) else it }
        d.state == "terminated" -> cur                     // 못 보던 dialog 의 종료는 버린다
        else -> cur + DialogRow(d.watched, d.id, d)
    }
    setDialogs(next.filterNot { it.isTerminated && it.elapsedMs > TERMINATED_KEEP_MS })

    // ⑥ 내역 — 전이만 남긴다(진행 중은 카드가 보여준다).
    //
    // **여기서는 타인(감시 대상)만 남긴다.** 내 통화는 내 세션이 권위라 `applyCallState` 가 남긴다 —
    // 둘 다 남기면 같은 통화가 두 줄이 되고, `isMine` 이 어긋나면(그룹원 목록의 번호와 내 등록 회선이
    // 다를 때) 내 통화가 «타인» 으로 잘못 분류된다.
    if (prev != null && !prev.isTerminated && d.state == "terminated") {
        val row = next.first { it.key == key }
        if (!isMine(row.watched)) addCallLog(CallLogRow(
            atMs = System.currentTimeMillis(),
            peer = displayName(row.info.remoteIdentity),
            number = userPart(row.info.remoteIdentity),
            text = if (row.wasConfirmed) "통화 종료" else "부재",
            kind = if (row.wasConfirmed) CallLogKind.ANSWERED else CallLogKind.MISSED,
            others = true,
            startedAtMs = row.startedAtMs,
            answeredAtMs = row.confirmedAtMs,
            viaPilot = isPilot(row.watched)))
    }
}

private const val TERMINATED_KEEP_MS = 3_000L

/**
 * 착신 거절 — 배너의 [거절].
 *
 * 기본 486(Busy Here)이다. 603(Decline)은 «이 단말이 아니라 사용자가 거절했다» 는 뜻이라 서버가
 * 포크 집합을 통째로 접을 수 있어, 대표번호 병렬 호출(TS 24.239)에서는 486 이 맞다 — 다른 관제석은
 * 계속 울려야 한다.
 */
suspend fun DispatchSession.reject(callId: Int, code: Int = 486): CimsResult<Unit> =
    engineOrNull()?.call(callId)?.reject(code) ?: CimsResult.fail(-1, "엔진 없음")
