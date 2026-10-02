// ③ 일반통화 (docs/design/features/android_dispatch_tablet.md §6.3·§6.7, dispatch_desktop_ui.md §4.3)
//
// 관제석의 전화 면. 자기 통화만이 아니라 **감시 범위의 통화**를 본다 —
//   · 그룹원 띠   전화 그룹원의 회선 상태(BLF, RFC 4235 dialog) — 링잉이면 당겨받기, 통화 중이면 청취
//   · 대표번호 대기열  대표번호로 들어온 호(TS 24.239 Flexible Alerting 포크) — 누가 울리고 누가 받았는지
//   · 내 통화     내가 당사자인 호 — 보류·음소거·DTMF·전달
//   · 오늘 데스크  응대·부재·발신·전달·감청 집계
package com.cims.ue.dispatch.ui.call

import com.cims.ue.dispatch.session.userPart
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
import com.cims.ue.dispatch.session.setRxLevel
import com.cims.ue.dispatch.session.callLogCsv
import com.cims.ue.dispatch.session.reject
import com.cims.ue.dispatch.session.hold
import com.cims.ue.dispatch.session.joinMonitor
import com.cims.ue.dispatch.session.pickup
import com.cims.ue.dispatch.session.sendDtmf
import com.cims.ue.dispatch.session.toggleMuted
import com.cims.ue.dispatch.session.cancelConsult
import com.cims.ue.dispatch.session.completeConsult
import com.cims.ue.dispatch.session.startConsult
import com.cims.ue.dispatch.session.transfer
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.mergePeople
import com.cims.ue.dispatch.ui.resolvePerson
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
    /** 감청 범위가 있는가(`monitorScope != none`) — 없으면 [청취] 를 세우지 않는다(누르면 403 이다). */
    val inScope: Boolean = true,
) {
    val ringing: Boolean get() = dialog?.isEarly == true && dialog.isIncomingLeg
    val talking: Boolean get() = dialog?.isConfirmed == true
    val idle: Boolean get() = dialog == null
    val stateText: String get() = when {
        ringing -> "링잉"; talking -> "통화"; else -> "대기"
    }
    val peer: String get() = dialog?.info?.remoteIdentity?.let { userPart(it) }.orEmpty()
    /** 남의 링잉만 당겨받는다(내 것은 응답이다). */
    val canPickup: Boolean get() = ringing && !isMe
    /** 남의 통화만 청취한다. 인가는 서버가 한다(403 이면 거절). */
    val canMonitor: Boolean get() = talking && !isMe && !monitoring && inScope
}

/** 대표번호로 들어온 호 하나 — 포크된 leg 들을 발신자 기준으로 묶는다. */
data class QueueItem(
    val dialog: DialogRow,
    val caller: String,
    /** 지금 울리는 그룹원 표시명. */
    val ringingAt: List<String> = emptyList(),
    /** 응답한 그룹원(있으면). */
    val answeredBy: String = "",
    /** 이 발신자의 **내** 착신 leg(울리는 중) — 있으면 [응답] 이 그것만 받는다(데스크톱 `QueueItem.RingsMe`). */
    val myLeg: Int? = null,
) {
    /** 대표번호(user part) — [당겨받기] 가 지정 픽업 `<code><대표번호>` 로 이 호를 고른다(dispatch_center.md §4.4). */
    val pilot: String get() = userPart(dialog.watched)
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
    /** 대표번호로 온 호인가 — `P-Called-Party-ID`(RFC 3455)가 **내 대표번호**일 때(`DispatchSession.isPilot`). */
    val viaPilot: Boolean = false,
) {
    val callId: Int get() = session.callId
    val peer: String get() = session.title.ifEmpty { userPart(session.info.remoteUri) }
    val incoming: Boolean get() = session.info.state == CallState.INCOMING
    val active: Boolean get() = session.isActive
    val held: Boolean get() = session.info.state == CallState.HELD
    val muted: Boolean get() = session.info.muted
    val stateText: String get() = when {
        incoming -> "착신"; held -> "보류"; active -> "통화"; else -> "연결 중"
    }
    /** 상담 전달의 **상담 호** — «상담» 배지, [전달 완결]·[취소](데스크톱 `IsConsult`). */
    val consult: Boolean get() = session.consultFor != null
    /** 전달할 수 있는가 — 통화·보류 중이고 상담 호가 아니다(데스크톱 `CanTransfer`). */
    val canTransfer: Boolean get() = (active || held) && !consult
    /** 상담 호가 연결됐다 — 원 통화를 넘길 수 있다(데스크톱 `CanComplete`). */
    val canComplete: Boolean get() = consult && active
    /** «전달 중 → 이순경» — 전달을 걸었고 이 leg 이 끝나기를 기다린다. */
    val transferNote: String get() = session.transferNote
}

/** ⑥ 필터의 «해제» 값 — 데스크톱과 같은 문자열을 쓴다(`CallActivityViewModel.Filter`). */
internal const val DESK_ALL = "all"

/**
 * 오늘 데스크 칩 필터의 판정 — 순수 함수(시험 대상).
 *
 * 데스크톱 `CallActivityViewModel.Refilter` 와 같은 규칙이다. 태블릿의 `CallLogKind` 에는 청취 종료가 따로
 * 없으므로(데스크톱 `ListenStart`/`ListenEnd` → `MONITOR` 하나) 감청은 그 한 종류로 판정한다.
 * 당겨받기(`PICKUP`)는 «내가 받은 호» 라 집계에서 응대로 세는데(`addCallLog`), 필터에서도 같게 다룬다.
 */
/**
 * 사람 축 — 정규형으로 비교해 `010…` 과 `+8210…` 이 같게 걸린다(순수 함수, 시험 대상).
 *
 * 빈 값이면 거르지 않는다. 행의 번호가 비어 있으면(발신자 표시 제한) 사람 필터에 걸리지 않는다 —
 * 어느 사람의 것인지 알 수 없는 행을 특정 사람의 기록이라고 말할 수는 없다.
 */
internal fun keepForPerson(row: CallLogRow, number: String): Boolean {
    if (number.isBlank()) return true
    val want = DirectoryBook.normalize(number)
    return want.isNotEmpty() && DirectoryBook.normalize(row.number) == want
}

internal fun keepInDesk(row: CallLogRow, filter: String): Boolean = when (filter) {
    DESK_ALL -> true
    // 데스크톱 ⑥ 머리의 «대표번호» — `CallActivityViewModel.Refilter` 의 `pilot` 과 같은 값이다.
    //   오늘 데스크 칩에는 없고 ⑥ 머리에만 있다(집계 축이 아니라 조회 축이라서).
    "pilot" -> row.viaPilot
    "missed" -> row.kind == CallLogKind.MISSED
    "outgoing" -> row.kind == CallLogKind.OUTGOING
    "transfer" -> row.kind == CallLogKind.TRANSFER
    "monitor" -> row.kind == CallLogKind.MONITOR
    else -> true
}


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
    /**
     * 이 통화에 붙여 둔 **감청 leg**(없으면 null).
     *
     * 감청 상세(소스 귀속·라우트)를 이 행 안에서 펴기 위해 든다 — 따로 «감청» 면을 두면 같은 통화가 두
     * 군데 나오고, 어느 쪽이 최신인지 흐려진다([dispatch_center.md](../../dispatch_center.md) §5.4 의
     * «CC 분리 인도·귀속 보존» 은 **표시 요구**이므로 화면 어딘가에는 반드시 있어야 한다).
     */
    val tap: SessionItem? = null,
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

    /** 지정 픽업 — 울리는 **타인의 착신** 회선만(그룹원이 **거는 중**인 호는 당겨받을 것이 없다 — 404). 내 전화는 배너·카드에서 받는다. */
    val canPickup: Boolean get() = legs.any { it.isEarly && it.isIncomingLeg } && !talking && !mine
    /** 감청 — 확립된 **타인** 통화이고 청취 범위가 있을 때(최종 판정은 서버). */
    val canMonitor: Boolean get() = talking && !mine && !monitoring && inScope
    /** 픽업 대상 번호 — 울리는 착신 leg 의 감시 대상. */
    val pickupNumber: String get() =
        (legs.firstOrNull { it.isEarly && it.isIncomingLeg } ?: primary).let { userPart(it.watched) }
}

/**
 * dialog 행을 통화 단위로 묶는다.
 *
 * 두 leg 이 **서로를 가리키면**(각자의 `watched` 가 상대의 `remoteIdentity`) 같은 통화다. 한쪽만
 * 감시 대상이면 leg 하나로 남는다. 순수 함수로 두어 기기 없이 시험한다.
 */
/** 결합 판정에 쓰는 시각 근접 창 — 같은 통화의 두 leg 은 거의 동시에 관측된다(§4.4). */
internal const val PAIR_WINDOW_MS = 5_000L

/** DTMF «보냄» 줄에 보이는 자릿수. */
internal const val DTMF_SHOWN = 24

/** 응답된 대표번호 호가 대기열에 남는 시간 — 누가 받았는지 읽고 나면 내려간다. */
internal const val QUEUE_ANSWERED_KEEP_MS = 3_000L

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
    if (userPart(b.watched) != userPart(a.info.remoteIdentity)) return false
    if (userPart(a.watched) != userPart(b.info.remoteIdentity)) return false
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

    init {
        // 전달·DTMF 칸은 **그 호**의 것이다 — 호가 끝나면 닫고 비운다(데스크톱은 호 카드가 들고 카드와 함께 사라진다). 호 번호는
        //   다시 쓰이므로, 남겨 두면 다음 호의 카드가 전달 칸이 열린 채로 서고 앞 통화에서 친 대상으로 전달된다.
        scope.launch {
            s.sessions.collect { list ->
                val live = list.filter { it.kind == SessionKind.PHONE_CALL && it.isLive }.mapTo(HashSet()) { it.callId }
                _transferFor.value?.let { if (it !in live) { _transferFor.value = null; _transferTarget.value = "" } }
                _dtmfFor.value?.let { if (it !in live) { _dtmfFor.value = null; _dtmfSent.value = "" } }
            }
        }
    }

    /** 그룹원 띠 — 전화 그룹원(`dispatch.members` 중 내 그룹). PTT 전용 가입자는 뺀다. */
    val members: StateFlow<List<MemberChip>> =
        combine(s.dialogs, s.sessions, s.tick) { dialogs, sessions, _ ->
            val d = s.dispatch
            val me = s.profile.value?.phoneService?.msisdn.orEmpty()
            val inScope = d.monitorScope != "none"
            d.members
                .filter { it.volteAor.isNotEmpty() && it.groupId == d.groupId }
                .map { m ->
                    // 한 회선에 통화와 링잉이 겹치면 **통화**를 보인다(데스크톱과 같다 — 먼저 온 것을 보이면 갱신마다 바뀐다)
                    val mine = dialogs.filter { userPart(it.watched) == userPart(m.volteAor) && !it.isTerminated }
                    val row = mine.firstOrNull { it.isConfirmed } ?: mine.firstOrNull()
                    MemberChip(
                        aor = m.volteAor, number = userPart(m.volteAor),
                        name = m.name, isMe = userPart(m.volteAor) == userPart(me),
                        dialog = row, inScope = inScope,
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
        combine(s.dialogs, s.sessions, s.tick) { dialogs, sessions, _ ->
            dialogs.filter { s.isPilot(it.watched) && !it.isTerminated }
                // 응답된 호는 3초 보이고 내려간다(누가 받았는지 읽을 시간) — 통화가 끝날 때까지 대기열에 두면 «기다리는 호» 가
                //   아닌 것이 주황 카드와 배지로 남는다(데스크톱 `CallDeskViewModel` 과 같다). 같은 발신자는 한 줄.
                .filterNot { it.isConfirmed && it.elapsedMs > QUEUE_ANSWERED_KEEP_MS }
                .distinctBy { userPart(it.info.remoteIdentity) }
                .map { pilot ->
                    val caller = userPart(pilot.info.remoteIdentity)
                    val peers = dialogs.filter {
                        it !== pilot && !s.isPilot(it.watched) &&
                            userPart(it.info.remoteIdentity) == caller
                    }
                    QueueItem(
                        dialog = pilot,
                        caller = caller,
                        ringingAt = peers.filter { it.isEarly }.map { userPart(it.watched) },
                        answeredBy = peers.firstOrNull { it.isConfirmed }
                            ?.let { userPart(it.watched) }.orEmpty(),
                        myLeg = myLegOf(sessions, caller))
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
            // 대표번호 dialog 는 행으로 세우지 않는다 — 대기열이 그 호를 말하고, 여기 섞으면 한 통화가 «대표 ↔ 발신자» 와
            //   «그룹원 ↔ 발신자» 두 줄이 돼 같은 통화에 감청을 두 번 걸게 된다. 그 발신자의 그룹원 leg 에 [대표] 를 붙인다.
            val pilotCallers = dialogs.filter { s.isPilot(it.watched) && !it.isTerminated }
                .map { userPart(it.info.remoteIdentity) }.toSet()
            combineDialogs(dialogs.filterNot { s.isPilot(it.watched) }).map { legs ->
                val a = legs.first()
                val tap = sessions.firstOrNull { se ->
                    se.kind == SessionKind.PHONE_MONITOR &&
                        legs.any { se.info.joinedDialog == it.info.callId }
                }
                // 왼쪽 = 건 사람, 오른쪽 = 받은 사람(RFC 4235 direction — 감시 대상이 받는 쪽이면 뒤집는다)
                val watchedLabel = s.displayLabel(a.watched)
                val remoteLabel = s.displayLabel(a.info.remoteIdentity)
                LiveCallRow(
                    legs = legs,
                    aLabel = if (a.isIncomingLeg) remoteLabel else watchedLabel,
                    bLabel = if (a.isIncomingLeg) watchedLabel else remoteLabel,
                    viaPilot = legs.any { userPart(it.info.remoteIdentity) in pilotCallers },
                    mine = legs.any { s.isMine(it.watched) },
                    monitoring = tap != null,
                    inScope = inScope,
                    tap = tap)
            }.sortedByDescending { it.monitoring }        // 청취 중인 통화가 맨 위(안정 정렬 — 그 아래는 링잉 › 최근)
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * 감청이 상대에게 숨겨지는가 — 서버가 준 역할 속성이다(`listen_visibility`, dispatch_center.md §5.6).
     * 화면이 정하지 않으므로 흐름이 아니라 값 하나로 든다.
     */
    val listenHidden: Boolean get() = s.listenHidden

    /** 감시 중인 회선의 dialog — 주소록 줄의 «통화 중»·«링잉» 이 이것을 따라 다시 그려진다. */
    val dialogs: StateFlow<List<DialogRow>> = s.dialogs
    fun lineStatusOf(number: String): String = s.lineStatusOf(number)

    /** 내 전화 번호(비교 정규형) — 주소록 목록에서 나를 뺀다. */
    val myPhoneKey: String get() =
        com.cims.ue.dispatch.session.DirectoryBook.normalize(userPart(s.profile.value?.phoneService?.msisdn.orEmpty()))

    /** ⑥ 행의 [청취] — 확립된 leg 으로 Join 한다(RFC 3911 `a=recvonly`). */
    fun monitorLive(row: LiveCallRow) {
        val leg = row.legs.firstOrNull { it.isConfirmed } ?: return
        scope.launch { s.joinMonitor(leg) }
    }

    /** ⑥ 행의 [청취 종료] — 그 통화에 붙여 둔 감청 leg 을 끊는다. 켠 자리가 곧 끄는 자리다. */
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
                .map { CallCard(it, viaPilot = s.isPilot(it.info.calledParty), dtmfOpen = it.callId == dtmfFor, dtmfSent = if (it.callId == dtmfFor) sent else "",
                                transferOpen = it.callId == xferFor) }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    val tally: StateFlow<DeskTally> = s.tally

    /**
     * 사람 메뉴가 쓰는 사람 목록 — 두 주소록을 사람 단위로 묶은 것(`mergePeople`).
     *
     * **내 회선은 뺀다.** 나에게 개별 통화·통화를 거는 항목이 목록에 있으면 안 된다.
     */
    val people: StateFlow<List<PersonEntry>> =
        combine(s.phoneBook, s.pttBook) { phone, ptt ->
            mergePeople(phone, ptt, exclude = s.myLineKeys())
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 번호·URI 하나로 사람을 찾는다 — 없으면 그 번호만 가진 항목을 만든다(주소록 밖 상대도 메뉴가 뜬다). */
    fun personAt(numberOrUri: String): PersonEntry? =
        resolvePerson(people.value, numberOrUri, s.phoneBook.value,
                      fallbackName = s.displayLabel(numberOrUri))

    /**
     * 오늘 데스크 칩이 거는 ⑥ 필터 — 데스크톱과 같은 값 집합(`all|missed|outgoing|transfer|monitor`,
     * `CallActivityViewModel.Filter`). **«응대» 칩은 `all` 이다** — 데스크톱에서도 그 칩의 뜻은 «응대만 보기»가
     * 아니라 «전체로 되돌리기» 다(`CallDeskPanel.xaml` 의 툴팁 "⑥ 전체"). 응대는 기본 목록의 대부분이라
     * 따로 거를 이유가 없고, 칩 다섯 중 하나는 해제 자리여야 한다.
     */
    private val _deskFilter = MutableStateFlow(DESK_ALL)
    val deskFilter: StateFlow<String> = _deskFilter.asStateFlow()

    private val _personFilter = MutableStateFlow("")
    /**
     * 사람 축 — «이 사람과의 기록만». 종류 필터(`deskFilter`)와 **직교**하므로 따로 든다
     * (사람 메뉴의 «통화 기록» 이 건다, §6.2f). 빈 값 = 전체.
     */
    val personFilter: StateFlow<String> = _personFilter.asStateFlow()

    /** ⑥ 에 실제로 그릴 행 — 필터 적용분. 원본은 세션이 갖는다. */
    val callLog: StateFlow<List<CallLogRow>> =
        combine(s.callLog, _deskFilter, _personFilter) { rows, f, who ->
            rows.filter { keepInDesk(it, f) && keepForPerson(it, who) }
        }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    // ── 조작 ──
    /** 같은 칩을 다시 누르면 전체로 되돌린다 — 해제 수단이 칩 말고 없다. */
    fun setDeskFilter(f: String) { _deskFilter.value = if (_deskFilter.value == f) DESK_ALL else f }

    /** 사람 메뉴의 «통화 기록» — 그 사람과의 기록만 남긴다. 같은 번호를 다시 걸면 해제. */
    fun setPersonFilter(number: String) {
        val n = userPart(number)
        _personFilter.value = if (_personFilter.value == n) "" else n
    }

    /** 사람 필터의 표시 이름 — 칩에 «이름과의 기록» 으로 적는다. */
    fun personFilterLabel(): String {
        val n = _personFilter.value
        return if (n.isEmpty()) "" else s.displayLabel(n)
    }

    fun setDialNumber(v: String) { _dialNumber.value = v }

    fun dial() {
        val n = _dialNumber.value.trim()
        if (n.isEmpty()) return
        // 걸렸을 때만 비운다 — 실패했는데 번호가 사라지면 다시 쳐야 한다(데스크톱 `Dial`)
        scope.launch { if (s.dial(n).ok && _dialNumber.value.trim() == n) _dialNumber.value = "" }
    }

    /** 주소록·최근·번호칸의 제안에서 바로 건다 — 입력칸을 거치지 않는다. 걸렸으면 치던 것을 비운다(제안 팝업도 닫힌다). */
    fun dialTo(number: String) {
        val n = number.trim()
        if (n.isEmpty()) return
        scope.launch { if (s.dial(n).ok) _dialNumber.value = "" }
    }

    /** **전화** 주소록(§6.2b) — 주소록 시트가 읽는다. PTT 번호는 전화로 걸리지 않으므로 섞지 않는다. */
    val book: StateFlow<DirectoryBook> = s.phoneBook

    // 이름·조직 경로는 화면이 [book] 을 관측해 직접 계산한다. VM 의 현재 값을 읽는 헬퍼를 두면
    // Compose 가 그 읽기를 추적하지 못해 늦게 도착한 주소록이 화면에 실리지 않는다.

    /** 그룹 픽업(번호 없음) 또는 지정 픽업. */
    fun pickup(number: String = "") { scope.launch { s.pickup(number) } }

    /** 호별 수신 음량 — 감청 상세의 음량 막대. */
    val rxLevels: StateFlow<Map<Int, Float>> = s.rxLevels
    fun setRxLevel(callId: Int, level: Float) { scope.launch { s.setRxLevel(callId, level) } }

    /** ⑥ CSV — 화면 필터와 무관하게 세션이 든 내역 전부. */
    fun logCsv(): String = callLogCsv(s.callLog.value)

    /** 대기열 [응답] — 이 발신자의 내 착신 leg 만 받는다(직접 착신과 동시에 울릴 때 다른 호를 받지 않게). */
    fun answerQueue(q: QueueItem) { q.myLeg?.let { id -> scope.launch { s.answer(id) } } }

    fun answer(c: CallCard) { scope.launch { s.answer(c.callId) } }
    fun reject(c: CallCard) { scope.launch { s.reject(c.callId) } }
    fun hangup(c: CallCard) { scope.launch { s.hangup(c.callId) } }
    fun toggleHold(c: CallCard) { scope.launch { s.hold(c.callId, !c.held); s.refreshSessions() } }
    /**
     * 음소거 토글 — 뒤집을 값은 카드 사본이 아니라 **명령 직전의 코어 스냅샷**에서 읽고, 명령 뒤 스냅샷을
     * 다시 당긴다(`toggleMuted`). 값의 권위는 언제나 코어이고 앱은 «눌렀으니 켜졌겠지» 로 추측하지 않는다.
     */
    fun toggleMute(c: CallCard) { scope.launch { s.toggleMuted(c.callId) } }

    /** 감청 합류 — 인가는 서버가 한다(범위 밖이면 403). */
    fun monitor(m: MemberChip) {
        val row = m.dialog ?: return
        scope.launch { s.joinMonitor(row) }
    }

    // DTMF — 통화에 묶인 조작이라 카드 안에서 연다(§6.6).
    fun openDtmf(c: CallCard) { _dtmfFor.value = c.callId; _dtmfSent.value = "" }
    fun closeDtmf() { _dtmfFor.value = null; _dtmfSent.value = "" }
    fun sendDtmf(c: CallCard, digit: String) {
        // 보낸 것만 적는다 — 실패한 숫자가 «보냄» 줄에 남지 않게. 길어지면 뒤 24자만 보인다(데스크톱과 같다).
        scope.launch { if (s.sendDtmf(c.callId, digit).ok) _dtmfSent.value = (_dtmfSent.value + digit).takeLast(DTMF_SHOWN) }
    }

    // 전달 — blind(REFER) 와 상담(원 통화 보류 + 상담 호 → [전달 완결], Replaces). 데스크톱 CallDeskViewModel 과 같다.
    fun openTransfer(c: CallCard) { _transferFor.value = c.callId; _transferTarget.value = "" }
    fun closeTransfer() { _transferFor.value = null; _transferTarget.value = "" }
    fun setTransferTarget(v: String) { _transferTarget.value = v }
    /** 그룹원 칩 — 대상 칸을 그 번호로 채운다(데스크톱 `PickMember`). */
    fun pickTransferTarget(number: String) { _transferTarget.value = number }

    /** blind 전달 — 받아들여지면 칸을 닫는다(실패면 토스트가 이유를 말하고 칸은 남는다). */
    fun transfer(c: CallCard) {
        val t = _transferTarget.value.trim()
        if (t.isEmpty()) return
        scope.launch { if (s.transfer(c.callId, t).ok) closeTransfer() }
    }

    /** 상담 전달 — 원 통화를 보류하고 대상에게 상담 호를 건다. */
    fun consult(c: CallCard) {
        val t = _transferTarget.value.trim()
        if (t.isEmpty()) return
        scope.launch { if (s.startConsult(c.callId, t).ok) closeTransfer() }
    }

    fun completeConsult(c: CallCard) { scope.launch { s.completeConsult(c.callId) } }
    fun cancelConsult(c: CallCard) { scope.launch { s.cancelConsult(c.callId) } }
}

/**
 * 이 발신자에게서 **내게** 울리는 착신 leg — 대표번호 포크가 이 관제석에도 닿았으면 있다. 대기열 [응답] 이 이것만 받는다.
 * 번호로 맞춘다(데스크톱 `RingsMe` 와 같다 — dialog 의 발신자와 내 호의 상대가 같은 사람).
 */
internal fun myLegOf(sessions: List<SessionItem>, caller: String): Int? =
    sessions.firstOrNull {
        it.kind == SessionKind.PHONE_CALL && it.info.state == CallState.INCOMING && userPart(it.info.remoteUri) == caller
    }?.callId
