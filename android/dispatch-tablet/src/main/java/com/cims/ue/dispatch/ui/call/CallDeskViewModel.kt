// ③ 일반통화 (docs/design/features/android_dispatch_tablet.md §6.3·§6.7, dispatch_desktop_ui.md §4.3)
//
// 관제석의 전화 면. 자기 통화만이 아니라 **감시 범위의 통화**를 본다 —
//   · 그룹원 띠   전화 그룹원의 회선 상태(BLF, RFC 4235 dialog) — 링잉이면 당겨받기, 통화 중이면 청취
//   · 대표번호 대기열  대표번호로 들어온 호(TS 24.239 Flexible Alerting 포크) — 누가 울리고 누가 받았는지
//   · 내 통화     내가 당사자인 호 — 보류·음소거·DTMF·전달
//   · 오늘 데스크  응대·부재·발신·전달·감청 집계
package com.cims.ue.dispatch.ui.call

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.CallLogKind
import com.cims.ue.dispatch.session.CallLogRow
import com.cims.ue.dispatch.session.DeskTally
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DialogRow
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.answer
import com.cims.ue.dispatch.session.dial
import com.cims.ue.dispatch.session.hangup
import com.cims.ue.dispatch.session.hold
import com.cims.ue.dispatch.session.joinMonitor
import com.cims.ue.dispatch.session.pickup
import com.cims.ue.dispatch.session.sendDtmf
import com.cims.ue.dispatch.session.setMuted
import com.cims.ue.dispatch.session.transfer
import com.cims.ue.sdk.CallState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** 그룹원 띠의 칩 하나 — 전화 그룹원의 회선 상태. */
data class MemberChip(
    val aor: String,
    val number: String,
    val name: String,
    val isMe: Boolean,
    val dialog: DialogRow? = null,
    /** 내가 이 회선을 감청 중인가. */
    val monitoring: Boolean = false,
) {
    val ringing: Boolean get() = dialog?.isEarly == true && dialog.isIncomingLeg
    val talking: Boolean get() = dialog?.isConfirmed == true
    val idle: Boolean get() = dialog == null
    val stateText: String get() = when {
        ringing -> "링잉"; talking -> "통화"; else -> "대기"
    }
    val peer: String get() = dialog?.info?.remoteIdentity?.let { userPartOf(it) }.orEmpty()
    /** 남의 링잉만 당겨받는다(내 것은 응답이다). */
    val canPickup: Boolean get() = ringing && !isMe
    /** 남의 통화만 청취한다. 인가는 서버가 한다(403 이면 거절). */
    val canMonitor: Boolean get() = talking && !isMe && !monitoring
}

/** 대표번호로 들어온 호 하나 — 포크된 leg 들을 발신자 기준으로 묶는다. */
data class QueueItem(
    val dialog: DialogRow,
    val caller: String,
    /** 지금 울리는 그룹원 표시명. */
    val ringingAt: List<String> = emptyList(),
    /** 응답한 그룹원(있으면). */
    val answeredBy: String = "",
) {
    val ringing: Boolean get() = dialog.isEarly
    val answered: Boolean get() = dialog.isConfirmed
    val elapsedMs: Long get() = dialog.elapsedMs
}

/** 내 통화 카드 — 보류·음소거·DTMF·전달. */
data class CallCard(
    val session: SessionItem,
    val dtmfOpen: Boolean = false,
    val dtmfSent: String = "",
    val transferOpen: Boolean = false,
) {
    val callId: Int get() = session.callId
    val peer: String get() = session.title.ifEmpty { userPartOf(session.info.remoteUri) }
    val incoming: Boolean get() = session.info.state == CallState.INCOMING
    val active: Boolean get() = session.isActive
    val held: Boolean get() = session.info.state == CallState.HELD
    val muted: Boolean get() = session.info.muted
    /** 대표번호로 온 호인가 — `P-Called-Party-ID`(RFC 3455). */
    val viaPilot: Boolean get() = session.info.calledParty.isNotEmpty()
    val stateText: String get() = when {
        incoming -> "착신"; held -> "보류"; active -> "통화"; else -> "연결 중"
    }
}

internal fun userPartOf(uri: String): String =
    uri.substringAfter(':', uri).substringBefore('@').substringBefore(';')

/**
 * ⑥ **진행 중 행** — 감시 대상의 살아 있는 통화 하나(dispatch_desktop_ui.md §4.4).
 *
 * dialog 이벤트(RFC 4235)를 **세션 행으로 결합**한 결과다. 감시 대상 둘이 서로 통화하면 leg 이 둘
 * 오는데(Call-ID 가 다르다) 그건 통화 하나이므로 한 행으로 묶는다 — 어느 leg 으로도 Join 할 수 있다
 * (dispatch_center.md §5.3).
 */
data class LiveCallRow(
    /** 결합된 leg — 1개 또는 2개. 감청은 [primary] 로 건다. */
    val legs: List<DialogRow>,
    val aLabel: String,
    val bLabel: String,
    val viaPilot: Boolean,
    val mine: Boolean,
    val monitoring: Boolean,
    val inScope: Boolean,
) {
    val primary: DialogRow get() = legs.first()
    val ringing: Boolean get() = legs.any { it.isEarly }
    val talking: Boolean get() = legs.any { it.isConfirmed }
    val startedAtMs: Long get() = legs.minOf { it.startedAtMs }
    val elapsedMs: Long get() = System.currentTimeMillis() - (legs.minOfOrNull { it.stateSinceMs } ?: startedAtMs)
    val key: String get() = legs.joinToString("|") { it.key }

    val stateText: String get() = when {
        talking -> "통화"
        ringing -> "링잉"
        else -> "연결 중"
    }

    /** 지정 픽업 — 울리는 **타인** 회선만. 내 전화는 배너·카드에서 받는다. */
    val canPickup: Boolean get() = ringing && !talking && !mine
    /** 감청 — 확립된 **타인** 통화이고 청취 범위가 있을 때(최종 판정은 서버). */
    val canMonitor: Boolean get() = talking && !mine && !monitoring && inScope
    /** 픽업 대상 번호 — 울리는 착신 leg 의 감시 대상. */
    val pickupNumber: String get() =
        (legs.firstOrNull { it.isEarly && it.isIncomingLeg } ?: primary).let { userPartOf(it.watched) }
}

/**
 * dialog 행을 통화 단위로 묶는다.
 *
 * 두 leg 이 **서로를 가리키면**(각자의 `watched` 가 상대의 `remoteIdentity`) 같은 통화다. 한쪽만
 * 감시 대상이면 leg 하나로 남는다. 순수 함수로 두어 기기 없이 시험한다.
 */
/** 결합 판정에 쓰는 시각 근접 창 — 같은 통화의 두 leg 은 거의 동시에 관측된다(§4.4). */
internal const val PAIR_WINDOW_MS = 5_000L

/**
 * dialog 행을 통화 단위로 묶는다.
 *
 * 두 leg 이 **서로를 가리키고**(각자의 `watched` 가 상대의 `remoteIdentity`), **방향이 반대이며**
 * (한쪽이 initiator, 다른 쪽이 recipient), **관측 시각이 근접하고**, 같은 진행 단계일 때만 한 통화다.
 *
 * 번호 상호 일치만으로 묶으면 **같은 두 사람의 서로 다른 통화**가 섞인다 — A↔B 가 통화 중인데
 * B 가 A 에게 두 번째로 걸면, 진행 중 leg(confirmed)과 새 착신 leg(early)이 한 행이 된다.
 * 그 행은 `talking=true` 라 새 착신의 [지정 픽업]이 사라지고, 한쪽의 감청 상태가 다른 호로 번진다.
 * 애매하면 **묶지 않고 따로 세운다** — 두 줄로 보이는 편이 조작이 사라지는 것보다 낫다.
 *
 * Call-ID 단순 비교로 대체하지 않는다 — leg 마다 Call-ID 가 다르다(dispatch_center.md §5.3).
 */
internal fun combineDialogs(rows: List<DialogRow>): List<List<DialogRow>> {
    // 아는 상태(early/proceeding/trying/confirmed)만 세운다 — 모르는 값은 전이가 오지 않아 안 사라진다.
    val live = rows.filter { it.isLive }
    val used = HashSet<String>()
    val out = ArrayList<List<DialogRow>>()
    live.forEach { a ->
        if (!used.add(a.key)) return@forEach
        val mates = live.filter { b -> b.key !in used && pairs(a, b) }
        // 후보가 둘 이상이면 어느 것이 짝인지 알 수 없다 — 추측하지 않고 홀로 둔다.
        val mate = mates.singleOrNull()
        if (mate != null) { used.add(mate.key); out.add(listOf(a, mate)) } else out.add(listOf(a))
    }
    // 링잉 먼저, 그다음 시작 역순 — 손이 가야 하는 것이 위로 온다.
    return out.sortedWith(compareByDescending<List<DialogRow>> { g -> g.any { it.isEarly } }
        .thenByDescending { g -> g.minOf { it.startedAtMs } })
}

/** 두 leg 이 같은 통화인가. */
private fun pairs(a: DialogRow, b: DialogRow): Boolean {
    // ① 서로를 가리킨다.
    if (userPartOf(b.watched) != userPartOf(a.info.remoteIdentity)) return false
    if (userPartOf(a.watched) != userPartOf(b.info.remoteIdentity)) return false
    // ② 방향이 반대다 — 같은 통화의 두 leg 은 한쪽이 걸고 한쪽이 받는다(RFC 4235 direction).
    val da = a.info.direction
    val db = b.info.direction
    if (da.isNotEmpty() && db.isNotEmpty() && da == db) return false
    // ③ 진행 단계가 같다 — 한쪽만 confirmed 면 서로 다른 통화다.
    if (a.isConfirmed != b.isConfirmed) return false
    // ④ 관측 시각이 근접하다(§4.4 결합 규칙의 «전이 시각이 근접하면»).
    return kotlin.math.abs(a.startedAtMs - b.startedAtMs) <= PAIR_WINDOW_MS
}

class CallDeskViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _dialNumber = MutableStateFlow("")
    val dialNumber: StateFlow<String> = _dialNumber.asStateFlow()

    private val _dtmfFor = MutableStateFlow<Int?>(null)
    private val _dtmfSent = MutableStateFlow("")
    private val _transferFor = MutableStateFlow<Int?>(null)
    private val _transferTarget = MutableStateFlow("")
    val transferTarget: StateFlow<String> = _transferTarget.asStateFlow()

    /** 그룹원 띠 — 전화 그룹원(`dispatch.members` 중 내 그룹). PTT 전용 가입자는 뺀다. */
    val members: StateFlow<List<MemberChip>> =
        combine(s.dialogs, s.sessions, s.tick) { dialogs, sessions, _ ->
            val d = s.dispatch
            val me = s.profile.value?.phoneService?.msisdn.orEmpty()
            d.members
                .filter { it.volteAor.isNotEmpty() && it.groupId == d.groupId }
                .map { m ->
                    val row = dialogs.firstOrNull {
                        userPartOf(it.watched) == userPartOf(m.volteAor) && !it.isTerminated
                    }
                    MemberChip(
                        aor = m.volteAor, number = userPartOf(m.volteAor),
                        name = m.name, isMe = userPartOf(m.volteAor) == userPartOf(me),
                        dialog = row,
                        monitoring = row != null && sessions.any {
                            it.kind == SessionKind.PHONE_MONITOR && it.info.joinedDialog == row.info.callId
                        })
                }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * 대표번호 대기열 — 대표번호 entity 의 dialog 를 발신자 기준으로 묶는다.
     * 포크된 그룹원 leg 들은 «누가 울리는지» 로만 쓴다(TS 24.239 병렬 호출).
     */
    val queue: StateFlow<List<QueueItem>> =
        s.dialogs.let { f ->
            combine(f, f) { dialogs, _ ->
                dialogs.filter { s.isPilot(it.watched) && !it.isTerminated }
                    .map { pilot ->
                        val caller = userPartOf(pilot.info.remoteIdentity)
                        val peers = dialogs.filter {
                            it !== pilot && !s.isPilot(it.watched) &&
                                userPartOf(it.info.remoteIdentity) == caller
                        }
                        QueueItem(
                            dialog = pilot,
                            caller = caller,
                            ringingAt = peers.filter { it.isEarly }.map { userPartOf(it.watched) },
                            answeredBy = peers.firstOrNull { it.isConfirmed }
                                ?.let { userPartOf(it.watched) }.orEmpty())
                    }
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * ⑥ **진행 중 행** — 감시 대상 전원의 살아 있는 통화.
     *
     * ③ 그룹원 띠는 **내 전화 그룹만** 보여 준다(`groupId == dispatch.groupId`). 관제 범위의 나머지
     * 감시 대상은 `watchAll()` 이 dialog 를 구독해 놓고도 화면에 나올 자리가 없었다 — 그래서 그들의
     * 통화는 보이지도, 감청되지도 않았다. 이 목록이 그 자리다.
     */
    val live: StateFlow<List<LiveCallRow>> =
        combine(s.dialogs, s.sessions, s.tick) { dialogs, sessions, _ ->
            val inScope = s.dispatch.monitorScope != "none"
            combineDialogs(dialogs).map { legs ->
                val a = legs.first()
                val monitoring = sessions.any { se ->
                    se.kind == SessionKind.PHONE_MONITOR &&
                        legs.any { se.info.joinedDialog == it.info.callId }
                }
                LiveCallRow(
                    legs = legs,
                    aLabel = s.displayLabel(a.watched),
                    bLabel = s.displayLabel(a.info.remoteIdentity),
                    viaPilot = legs.any { s.isPilot(it.watched) },
                    mine = legs.any { s.isMine(it.watched) },
                    monitoring = monitoring,
                    inScope = inScope)
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * ⑥ 진행 중 행이 비었을 때 **왜 비었는지**. 화면이 조용히 비면 원인을 못 찾는다.
     *
     * 관제 편성은 서버가 정한다(`/provisioning/me` 의 `dispatch.members[]`) — 앱은 받은 목록만
     * 구독한다. 그래서 «감시 대상 0» 은 앱 결함이 아니라 편성 문제다.
     */
    val liveHint: StateFlow<String> =
        combine(s.watchedAors, s.notifiedAors, s.dialogSeenAors, s.subscribeError) { aors, notified, seen, err ->
            val d = s.dispatch
            val members = d.members.size
            // 편성은 됐는데 **망 주소가 비어** 구독을 못 건 구성원 — 서버 프로비저닝 문제다.
            val addressable = d.members.count { it.volteAor.isNotEmpty() }
            when {
                !d.present ->
                    "관제 역할 미배정 — 다른 구성원의 통화를 볼 수 없습니다 (콘솔 «관리 > 역할»)"
                members == 0 ->
                    "감시 대상 없음 — 관제 그룹에 구성원이 편성되지 않았습니다 (콘솔 «구성 > 전화 그룹»)"
                addressable == 0 ->
                    "구성원 ${members}명이 편성됐지만 전화 주소가 비어 있어 감시할 수 없습니다 (서버 프로비저닝)"
                // 보낸 SUBSCRIBE 수와 NOTIFY 를 받은 수가 다르면 서버가 구독을 거절한 것이다.
                notified.size < aors.size ->
                    "감시 대상 ${aors.size} · 구독 성립 ${notified.size} — " +
                        "${aors.size - notified.size}건이 NOTIFY 를 못 받았습니다" +
                        (if (err.isNotBlank()) " ($err)" else " (서버가 구독을 거절했을 수 있습니다)")
                seen.isEmpty() ->
                    "감시 대상 ${aors.size} 전부 구독 성립 — 다만 서버가 통화 변화 NOTIFY 를 " +
                        "한 번도 보내지 않았습니다 (CSP dialog 이벤트)"
                else ->
                    "감시 대상 ${aors.size} 전부 구독 성립 · 통화 관측 ${seen.size} · 진행 중 통화 없음"
            }
        }.stateIn(scope, SharingStarted.Eagerly, "")

    /** 감시 대상 한 줄 — 번호 · 이름 · 구독 성립 여부. 어느 번호가 안 잡히는지 눈으로 본다. */
    data class WatchRow(val number: String, val name: String, val established: Boolean,
                        val sawDialog: Boolean, val pilot: Boolean)

    val watchDiag: StateFlow<List<WatchRow>> =
        combine(s.watchedAors, s.notifiedAors, s.dialogSeenAors) { aors, notified, seen ->
            aors.map { aor ->
                val n = userPartOf(aor)
                WatchRow(n, s.phoneBook.value.nameOf(n), n in notified, n in seen, s.isPilot(aor))
            }.sortedWith(compareBy({ it.established }, { it.number }))   // 안 잡힌 것이 위로
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** ⑥ 행의 [청취] — 확립된 leg 으로 Join 한다(RFC 3911 `a=recvonly`). */
    fun monitorLive(row: LiveCallRow) {
        val leg = row.legs.firstOrNull { it.isConfirmed } ?: return
        scope.launch { s.joinMonitor(leg) }
    }

    /**
     * ⑥ 행의 [청취 종료] — 그 통화에 붙여 둔 감청 leg 을 끊는다.
     *
     * 시트에도 같은 조작이 있지만 **켠 자리에서 끌 수 있어야 한다** — 켜는 버튼만 있고 끄는 버튼이
     * 다른 화면에 있으면 «끊는 기능이 없다» 로 읽힌다.
     */
    fun stopMonitorLive(row: LiveCallRow) {
        val se = s.sessions.value.firstOrNull { x ->
            x.kind == SessionKind.PHONE_MONITOR && row.legs.any { x.info.joinedDialog == it.info.callId }
        } ?: return
        scope.launch { s.hangup(se.callId) }
    }

    /** 내 통화 — PTT 가 아닌 세션(감청 시트는 뺀다). */
    val calls: StateFlow<List<CallCard>> =
        combine(s.sessions, _dtmfFor, _dtmfSent, _transferFor, s.tick) { a ->
            @Suppress("UNCHECKED_CAST") val sessions = a[0] as List<SessionItem>
            val dtmfFor = a[1] as Int?
            val sent = a[2] as String
            val xferFor = a[3] as Int?
            // **살아 있는 호만.** 끝난 호는 ⑥ 내역이 맡는다 — 카드로 남으면 조작 버튼이 붙는다.
            sessions.filter { it.kind == SessionKind.PHONE_CALL && it.isLive }
                .map { CallCard(it, dtmfOpen = it.callId == dtmfFor, dtmfSent = if (it.callId == dtmfFor) sent else "",
                                transferOpen = it.callId == xferFor) }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    val tally: StateFlow<DeskTally> = s.tally
    val callLog: StateFlow<List<CallLogRow>> = s.callLog

    // ── 조작 ──
    fun setDialNumber(v: String) { _dialNumber.value = v }

    fun dial() {
        val n = _dialNumber.value.trim()
        if (n.isEmpty()) return
        scope.launch { s.dial(n); _dialNumber.value = "" }
    }

    /** 주소록·최근·다이얼패드에서 바로 건다 — 입력칸을 거치지 않는다. */
    fun dialTo(number: String) {
        val n = number.trim()
        if (n.isEmpty()) return
        scope.launch { s.dial(n); _dialNumber.value = "" }
    }

    /** **전화** 주소록(§6.2b) — 주소록 시트가 읽는다. PTT 번호는 전화로 걸리지 않으므로 섞지 않는다. */
    val book: StateFlow<DirectoryBook> = s.phoneBook

    // 이름·조직 경로는 화면이 [book] 을 관측해 직접 계산한다. VM 의 현재 값을 읽는 헬퍼를 두면
    // Compose 가 그 읽기를 추적하지 못해 늦게 도착한 주소록이 화면에 실리지 않는다.

    /** 그룹 픽업(번호 없음) 또는 지정 픽업. */
    fun pickup(number: String = "") { scope.launch { s.pickup(number) } }

    fun answer(c: CallCard) { scope.launch { s.answer(c.callId) } }
    fun hangup(c: CallCard) { scope.launch { s.hangup(c.callId) } }
    fun toggleHold(c: CallCard) { scope.launch { s.hold(c.callId, !c.held); s.refreshSessions() } }
    /**
     * 음소거 토글 — **명령 뒤 스냅샷을 다시 읽는다**.
     *
     * 코어가 `onCallMedia` 로 알려 주지만(엔진 수정), 구형 엔진과 섞여도 화면이 멎지 않게 앱에서도
     * 한 번 당긴다. 값의 권위는 언제나 코어 스냅샷이고 앱은 «눌렀으니 켜졌겠지» 로 추측하지 않는다.
     */
    fun toggleMute(c: CallCard) {
        scope.launch {
            s.setMuted(c.callId, !c.muted)
            s.refreshSessions()
        }
    }

    /** 감청 합류 — 인가는 서버가 한다(범위 밖이면 403). */
    fun monitor(m: MemberChip) {
        val row = m.dialog ?: return
        scope.launch { s.joinMonitor(row) }
    }

    // DTMF — 통화에 묶인 조작이라 카드 안에서 연다(§6.6).
    fun openDtmf(c: CallCard) { _dtmfFor.value = c.callId; _dtmfSent.value = "" }
    fun closeDtmf() { _dtmfFor.value = null; _dtmfSent.value = "" }
    fun sendDtmf(c: CallCard, digit: String) {
        _dtmfSent.value += digit
        scope.launch { s.sendDtmf(c.callId, digit) }
    }

    // 전달 — blind. attended 는 상담 호가 필요해 후속(§11).
    fun openTransfer(c: CallCard) { _transferFor.value = c.callId; _transferTarget.value = "" }
    fun closeTransfer() { _transferFor.value = null; _transferTarget.value = "" }
    fun setTransferTarget(v: String) { _transferTarget.value = v }
    fun transfer(c: CallCard) {
        val t = _transferTarget.value.trim()
        if (t.isEmpty()) return
        scope.launch { s.transfer(c.callId, t); closeTransfer() }
    }
}
