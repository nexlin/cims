// ① 내 채널 + 발언 바 (docs/design/features/android_dispatch_tablet.md §6.3·§6.7,
//                     dispatch_desktop_ui.md §4.1)
//
// **포커스 ≠ 발언 대상.** 이 파일의 가장 중요한 계약이다.
//   · 포커스(`selectedId`) = 지금 **보는** 채널. ④ 메시지·⑤ 이벤트가 따라간다. 하나뿐이다.
//   · 발언 대상(`targets`) = 지금 **말하는** 채널의 집합. 카드 체크로 고른다.
// 둘은 독립이다 — 순찰1을 보면서 상황실에 말할 수 있다. 섞으면 관제사가 엉뚱한 채널로 송출한다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.session.userPart
import com.cims.ue.dispatch.ui.PersonEntry
import com.cims.ue.dispatch.ui.mergePeople
import com.cims.ue.dispatch.ui.resolvePerson
import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.TextArea
import com.cims.ue.dispatch.session.releaseBroadcast
import com.cims.ue.dispatch.session.startAdhoc
import com.cims.ue.dispatch.session.startAdhocBroadcast
import com.cims.ue.dispatch.session.startBroadcast
import com.cims.ue.dispatch.session.startPrivateCall
import com.cims.ue.sdk.CimsResult
import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.floorRelease
import com.cims.ue.dispatch.session.floorRequest
import com.cims.ue.dispatch.session.joinGroup
import com.cims.ue.dispatch.session.leave
import com.cims.ue.dispatch.session.setEmergency
import com.cims.ue.dispatch.session.cancelCondition
import com.cims.ue.dispatch.session.toggleMuted
import com.cims.ue.dispatch.session.videoCalls
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** ① 카드의 종류 — 멤버 그룹은 항상 서 있고, 개별 통화·애드혹 그룹 통화는 세션이 있을 때만 선다. */
enum class CardKind { MEMBER, PRIVATE, ADHOC }

/**
 * ① 내 채널 카드 하나.
 *
 * 멤버 그룹 카드는 **세션이 없어도 선다**(참여하지 않은 채널도 보여야 한다). 개별 통화·애드혹 그룹 통화는
 * 내가 건 세션 자체가 카드다.
 */
data class ChannelCard(
    val id: String,
    val kind: CardKind,
    val title: String,
    val group: GroupInfo? = null,
    val session: SessionItem? = null,
    /** 미읽음 SDS — 1줄 ✉ 배지. */
    val unread: Int = 0,
) {
    val badge: String get() = when (kind) {
        CardKind.MEMBER -> "멤버"; CardKind.PRIVATE -> "개별"; CardKind.ADHOC -> "애드혹"
    }
    val joined: Boolean get() = session?.isLive == true
    val active: Boolean get() = session?.isActive == true
    /** 참여하지 않아도 로스터로 진행 중임을 안다. */
    val hasSession: Boolean get() = joined || group?.hasSession == true
    val emergency: Boolean get() = session?.isEmergency == true
    /** 지금의 긴급을 이 단말이 올렸다 — [긴급 해제] 자격의 한쪽(다른 쪽 = user profile allow-cancel-group-emergency, TS 24.379 §6.3.3.1.13.4 —
     *  [PttChannelsViewModel.capabilities]). */
    val emergencyMine: Boolean get() = session?.info?.condition?.mine == true
    val imminentPeril: Boolean get() = session?.isImminentPeril == true
    val speaking: Boolean get() = session?.isSpeaking == true
    val requesting: Boolean get() = session?.isRequesting == true
    val queued: Boolean get() = session?.isQueued == true
    val speaker: String get() = session?.speaker.orEmpty()
    val floorNote: String get() = session?.floorNote.orEmpty()
    /** 발언·요청·대기 중 — 잠금 발언은 요청해 둔 대상이 **전부** 여기서 벗어나야 풀린다. */
    val talkBusy: Boolean get() = speaking || requesting || queued
    /** 남은 발언 0~1(Floor Granted 의 Duration 기준) · 시한 임박 — 발언 바 게이지. */
    val talkGauge: Float get() = session?.talkGauge ?: 0f
    val talkLimitNear: Boolean get() = session?.talkLimitNear == true
    val speakerElapsedMs: Long get() = session?.speakerElapsedMs ?: 0L
    /** 내가 건 개별 통화(반이중)·애드혹 그룹 통화 — 세션이 서면 단일 발언 대상이 된다(«내가 건 호 우선»). */
    val isOwnOutgoing: Boolean get() = kind != CardKind.MEMBER && session?.info?.dir == com.cims.ue.sdk.CallDir.OUTGOING
    val participants: Int get() = group?.connectedCount ?: session?.adhocMembers?.size ?: 0
    val memberCount: Int get() = group?.memberCount ?: 0

    /**
     * 발언 대상이 될 수 있는가 — **참여 중 + 반이중 + 발언 요청 가능**.
     * 전이중 개별 통화는 마이크가 늘 열려 있어 floor 가 없다(음소거로 다룬다 — [canMute]). 남이 연 일제 통화의 수신 멤버는
     * Floor Taken 의 Permission 0 이라 요청할 수 없다(TS 24.380 §6.3.4.4.2 3d).
     */
    val canCheck: Boolean get() = joined && session?.isFullDuplex != true && session?.canRequestFloor != false &&
        !(isBroadcast && !isBroadcastInitiator)     // 남이 연 일제 통화 — Permission 을 받기 전(착신 직후)에도 ✓ 가 켜지지 않는다

    /** 일제 통화(TS 24.379 §4.12) — 서버가 알린 호 속성. */
    val isBroadcast: Boolean get() = session?.isBroadcast == true
    val isBroadcastInitiator: Boolean get() = session?.isBroadcastInitiator == true

    /**
     * 채널 머리 [일제 통화] — 멤버 편성 그룹에 **진행 중 통화가 없을 때만**. 일제 통화는 새 호를 여는 INVITE 로만 정해지고
     * 진행 중 호는 일제로 바꿀 수 없다(TS 24.379 §10.1.1.3.1.1 15) — 합류가 된다). 채팅 그룹은 서버가 broadcast-ind 를 무시한다.
     */
    val canBroadcast: Boolean get() =
        kind == CardKind.MEMBER && !joined && group?.hasSession != true && group?.sessionType != "chat"

    /**
     * 음소거를 다는가 — **참여 중 + 전이중**. [canCheck] 와 정확히 갈린다: 반이중은 floor 가 마이크를 열고
     * 닫으므로 음소거가 없고, 전이중은 floor 가 없어 음소거가 송출을 멈추는 유일한 수단이다.
     */
    val canMute: Boolean get() = joined && session?.isFullDuplex == true

    /** 음소거 상태 — 코어 스냅샷(`CallInfo.muted`)이 권위다. 앱은 «눌렀으니 켜졌겠지» 로 추측하지 않는다. */
    val muted: Boolean get() = session?.info?.muted == true

    /** 1줄 오른쪽 — 진행 중이면 경과, 아니면 상태. */
    val stateText: String get() = when {
        joined -> fmtElapsed(session!!.elapsedMs)
        group?.hasSession == true -> "진행(미참여)"
        else -> "대기"
    }

    /** 2줄 — 발언자·사유. 대기 중이면 멤버 수. 일제 통화면 앞에 «일제 통화 · 발언을 놓으면 종료 / 수신 전용». */
    val line2: String get() {
        val base = when {
            speaker.isNotEmpty() -> "발언 $speaker" + fmtSpeaker()
            joined && floorNote.isNotEmpty() -> floorNote
            joined && session?.isFullDuplex == true -> "전이중"      // floor 가 없다 — «발언 없음» 은 반이중의 말이다
            joined -> "발언 없음"
            kind == CardKind.MEMBER -> "멤버 $memberCount"
            else -> ""
        }
        if (!isBroadcast) return base
        val bc = if (isBroadcastInitiator) "일제 통화 · 발언을 놓으면 종료" else "일제 통화 · 수신 전용"
        return if (base.isEmpty()) bc else "$bc · $base"
    }

    private fun fmtSpeaker(): String =
        session?.speakerElapsedMs?.takeIf { it > 0 }?.let { " " + fmtElapsed(it) } ?: ""
}

/**
 * 애드혹 카드·칩의 이름 — 내가 연 것은 **초대한 사람 앞 둘 + «+n»**(데스크톱과 같다), 남이 연 것은 «애드혹 · <개시자>».
 * 전부 «애드혹» 이면 애드혹 통화가 둘일 때 카드·발언 바 칩을 구별할 수 없다.
 */
internal fun adhocTitle(invited: List<String>, initiator: String = ""): String = when {
    invited.isNotEmpty() -> invited.take(2).joinToString(" · ") + if (invited.size > 2) " +${invited.size - 2}" else ""
    initiator.isNotBlank() -> "애드혹 · $initiator"
    else -> "애드혹"
}

/**
 * 발언 대상·발언 소유의 규칙 — 순수 함수라 JVM 에서 시험한다(dispatch_desktop_ui.md §4.1).
 *
 * **동시 발언 = 단말 팬아웃.** 3GPP 에 UE 의 다중 그룹 동시 발언 절차가 없어, 대상 세션마다 floor 를 따로 요청하고 코어가
 * 승인된 세션마다 같은 마이크를 결선한다(세션별 floor participant). 그래서 대상 집합에 **상한이 없다** — 서버 변경도 없다.
 */
internal object TalkRules {
    /** 카드 ✓ — 대상 집합에 넣고 뺀다. 발언 요청을 할 수 없는 카드는 그대로. */
    fun toggle(targets: Set<String>, id: String, canCheck: Boolean = true): Set<String> = when {
        !canCheck -> targets
        id in targets -> targets - id
        else -> targets + id
    }

    /**
     * 잠금 발언이 풀려야 하나 — 요청해 둔 호가 **한 번이라도 발언·요청·대기에 들었고**([seenBusy]) 지금은 **전부** 벗어났을 때.
     * 한 채널의 회수·시한(TalkLimit·Revoked·Denied)이 나머지 채널의 발언을 풀지 않는다(데스크톱 `IsLocked && !IsTalking`).
     * 요청 직후(아직 아무도 «요청 중» 이 아니다)에는 풀지 않는다 — 그걸 끝으로 읽으면 누르자마자 잠금이 풀린다.
     */
    fun lockEnded(seenBusy: Boolean, busyNow: Boolean): Boolean = seenBusy && !busyNow
}

internal fun fmtElapsed(ms: Long): String {
    val t = ms / 1000
    return if (t >= 3600) "%d:%02d:%02d".format(t / 3600, (t % 3600) / 60, t % 60)
    else "%02d:%02d".format(t / 60, t % 60)
}

/** 발언 바의 대상 칩 — 대상별 floor 상태를 따로 보여준다(하나가 거부돼도 나머지는 살아 있다). */
data class TalkTargetChip(val card: ChannelCard) {
    val name: String get() = card.title
    val granted: Boolean get() = card.speaking
    val requesting: Boolean get() = card.requesting
    val queued: Boolean get() = card.queued
    /** 거부·회수 — floor 사유가 남아 있는데 승인·요청·대기가 아니다(칩이 빨강으로 선다). */
    val denied: Boolean get() = !granted && !requesting && !queued && card.floorNote.isNotEmpty()
    /** 내가 연 일제 통화 — 발언을 놓으면 코어가 호를 해제한다(TS 24.380 §6.2.4.6.4). 발언 바 문구가 그것을 미리 말한다. */
    val broadcastInitiator: Boolean get() = card.isBroadcastInitiator
    val stateText: String get() = when {
        granted -> "승인"
        queued -> card.floorNote.ifEmpty { "대기" }
        requesting -> "요청"
        card.floorNote.isNotEmpty() -> card.floorNote
        else -> ""
    }
}

class PttChannelsViewModel(private val s: DispatchSession) : ScreenViewModel() {

    /** 정책 게이트(user profile ruleset) — 채널 상세 [긴급 해제] 가 읽는다(내 긴급 ∨ cancelGroupEmergency). */
    val capabilities: StateFlow<com.cims.ue.sdk.Capabilities> = s.capabilities

    // ── 포커스(보는 채널) ──
    private val _selectedId = MutableStateFlow<String?>(null)
    val selectedId: StateFlow<String?> = _selectedId.asStateFlow()

    // ── 발언 대상(말하는 채널) ──
    private val _targetIds = MutableStateFlow<Set<String>>(emptySet())
    val targetIds: StateFlow<Set<String>> = _targetIds.asStateFlow()

    /** 잠금 발언 — 손을 떼도 유지. TalkLimit·Revoked 로 풀린다. */
    private val _locked = MutableStateFlow(false)
    val locked: StateFlow<Boolean> = _locked.asStateFlow()

    /**
     * **지금 floor 를 요청해 둔 호**. 발언 대상([_targetIds])과 따로 든다.
     *
     * 해제를 «현재 대상» 으로 하면, 누른 채 대상이 바뀌었을 때(자동 승격·세션 종료) 원래 요청한
     * 호에 `floorRelease` 가 가지 않아 **마이크가 열린 채 남는다**. 실제 마이크 차단은 코어의 floor
     * participant 가 release 로 수행하므로 앱이 놓치면 송출이 계속된다 — «요청한 것만 해제한다» 를 불변으로 둔다.
     */
    private var speakingCallIds: Set<Int> = emptySet()

    /** 요청해 둔 호가 발언·요청·대기에 든 것을 한 번이라도 봤나 — 잠금 발언 해제 판정([TalkRules.lockEnded]). 누를 때마다 다시 센다. */
    private var lockSeenBusy = false

    /** 이미 본 «내가 건 호» — 처음 설 때 한 번만 단일 발언 대상으로 올린다(그 뒤 관제사가 바꾼 대상을 되돌리지 않는다). */
    private val ownCallsSeen = HashSet<Int>()

    /**
     * 참여를 누른 채널 — 세션이 서면 **자동으로 단일 발언 대상**이 된다.
     *
     * 데스크톱은 발신(`Dir==Outgoing`)과 `Ctrl+n` 에서 같은 일을 한다(포커스 + 단일 대상). 태블릿에는
     * 그 단축키가 없어 [참여]가 그 자리를 대신한다 — **참여는 곧 말하겠다는 의도**이기 때문이다.
     * 포커스와 발언 대상이 독립이라는 계약은 그대로다(둘을 따로 바꿀 수 있다).
     */
    private var pendingTargetId: String? = null
    /** 대기 중인 참여의 세션이 한 번 목록에 올랐다 — 그 뒤에 사라지면 참여가 실패로 끝난 것이다. */
    private var pendingSeen = false

    /** 지금 누르고 있는 일제 통화(아래 «일제 통화 한 버튼») — 카드 id, 애드혹 일제 통화면 [ADHOC_BROADCAST]. null = 없음.
     *  [cards] 의 onEach 가 읽으므로 그보다 먼저 선언한다(생성 중 Eagerly 수집이 초기화 전 값을 읽지 않게). */
    private val _broadcastHeld = MutableStateFlow<String?>(null)
    val broadcastHeld: StateFlow<String?> = _broadcastHeld.asStateFlow()
    private var bcCallId = -1
    private var bcReleased = false
    /** 개시한 호를 세션 목록에서 한 번이라도 봤나 — 개시 직후 이벤트가 오기 전의 «없음» 을 종료로 읽지 않게. */
    private var bcSeen = false

    /**
     * ① 카드 목록 — **멤버 그룹 전부 + 내가 건 개별 통화·애드혹 그룹 통화**. 항상 전부 보인다(필터는 ② 에만 있다).
     * 순서: 멤버 그룹(이름) → 개별·애드혹(시작 순).
     */
    val cards: StateFlow<List<ChannelCard>> =
        combine(s.groups, s.sessions, s.messages) { groups, sessions, messages ->
            // 서버(GMS) 목록 순 그대로 — 이름순으로 세우면 같은 계정의 핀 번호가 데스크톱과 다르고, 그룹이 늘 때마다 번호가 밀린다
            val memberCards = groups.filter { it.isMember }.map { g ->
                ChannelCard(
                    id = g.id, kind = CardKind.MEMBER, title = g.name, group = g,
                    session = sessions.firstOrNull {
                        it.kind == SessionKind.PTT_CHANNEL && it.info.groupId == g.id
                    },
                    unread = messages[g.id]?.count { !it.read && !it.outgoing } ?: 0)
            }
            val adhocCards = sessions
                .filter { it.kind == SessionKind.PTT_PRIVATE || it.kind == SessionKind.PTT_ADHOC }
                .sortedBy { it.startedAtMs }
                .map { se ->
                    ChannelCard(
                        id = se.channelId,
                        kind = if (se.kind == SessionKind.PTT_PRIVATE) CardKind.PRIVATE else CardKind.ADHOC,
                        title = if (se.kind == SessionKind.PTT_ADHOC)
                                    adhocTitle(se.adhocMembers.map { m -> s.displayName(m) },
                                        se.info.mcptt.callingUserId.takeIf { it.isNotBlank() && se.info.dir == com.cims.ue.sdk.CallDir.INCOMING }
                                            ?.let { s.displayName(it) }.orEmpty())
                                else se.title.ifEmpty { se.info.groupId },
                        session = se)
                }
            // 카드 id 가 목록 키다 — 같은 상대와의 개별 통화가 겹쳐도(내 발신과 상대 발신이 엇갈림) 한 장만 세운다
            (memberCards + adhocCards).distinctBy { it.id }
        }.onEach { list ->
            // 참여가 성립하면(세션이 붙어 canCheck) 대기 중이던 채널을 발언 대상으로 올린다. 판정은 **이번 목록**으로 한다 —
            //   `cards.value` 는 이 블록이 끝난 뒤에야 바뀐다.
            pendingTargetId?.let { id ->
                val c = list.firstOrNull { it.id == id }
                when {
                    c?.canCheck == true -> { applyTargets(setOf(id), list); pendingTargetId = null }
                    c?.session != null -> pendingSeen = true
                    // 참여가 성립 없이 끝났다(4xx·취소) — 남겨 두면 한참 뒤 그 그룹에 선 착신 세션을 «내가 참여한 것» 으로 읽어
                    //   지금 말하던 채널의 발언권을 놓고 대상을 바꾼다.
                    pendingSeen -> pendingTargetId = null
                }
            }
            // «내가 건 호 우선» — 내가 건 애드혹 그룹 통화·반이중 개별 통화는 단일 발언 대상이 된다(발신자가 곧 말하려는 채널).
            list.filter { it.isOwnOutgoing }.forEach { c ->
                val callId = c.session?.callId ?: return@forEach
                if (ownCallsSeen.add(callId) && c.canCheck) applyTargets(setOf(c.id), list)
            }
            ownCallsSeen.retainAll(list.mapNotNull { it.session?.callId }.toSet())
            // 잠금 발언 — 요청해 둔 호가 전부 끝나야 풀린다(한 채널의 회수·시한이 나머지 발언을 풀지 않는다).
            if (_locked.value) {
                val busy = list.any { it.session?.callId in speakingCallIds && it.talkBusy }
                if (busy) lockSeenBusy = true
                else if (TalkRules.lockEnded(lockSeenBusy, busyNow = false)) {
                    // 카드가 사라진(멤버에서 빠진) 호는 «끝남» 으로 읽히지만 세션은 살아 있을 수 있다 — 소유를 버리기 전에 놓는다
                    val carded = list.mapNotNull { it.session?.callId }.toSet()
                    (speakingCallIds - carded).filter(s::isCallAlive).forEach { release(it) }
                    _locked.value = false; speakingCallIds = emptySet()
                }
            }
            // 일제 통화 한 버튼으로 연 호가 끝났다(서버·코어가 먼저 끝냄) — 누름 상태를 푼다. 목록에 오르기 전(개시 직후)은 끝이 아니다
            //   — 그걸 끝으로 읽으면 뗌이 해제를 보내지 않아 마이크가 열린 채 남는다.
            if (bcCallId >= 0) {
                if (s.sessions.value.any { it.callId == bcCallId }) bcSeen = true
                else if (bcSeen) clearBroadcastHold()
            }
            // 세션이 끝났거나 전이중으로 바뀐 대상은 스스로 빠진다 — 없는 세션에 floor 를 걸지 않게.
            val ok = list.filter { it.canCheck }.map { it.id }.toSet()
            if (!ok.containsAll(_targetIds.value)) applyTargets(_targetIds.value intersect ok, list)
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 발언 바의 대상 칩 — 체크된 카드의 투영. */
    val targets: StateFlow<List<TalkTargetChip>> =
        combine(cards, _targetIds) { list, ids ->
            list.filter { it.id in ids }.map { TalkTargetChip(it) }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 포커스된 카드 — ④ 메시지·⑤ 이벤트가 이걸 따라간다. */
    val focused: StateFlow<ChannelCard?> =
        combine(cards, _selectedId) { list, id -> list.firstOrNull { it.id == id } }
            .stateIn(scope, SharingStarted.Eagerly, null)

    init {
        // 측면 하드키(§7) — 발언 바를 누른 것과 같다(잠금 발언 설정도 같이 따른다). 처음 값은 지금 상태라 건너뛴다 —
        //   화면이 다시 설 때 눌림으로 읽으면 누르지 않은 발언이 나간다.
        scope.launch {
            s.hwPtt.pressed.drop(1).collect { down ->
                val lock = s.settingsSnapshot().lockTalk
                if (down) pttDown(lock) else pttUp(lock)
            }
        }
    }

    // ── 포커스 조작 ──
    /** 카드 탭 — 같은 카드를 다시 누르면 접힌다. **발언 대상은 건드리지 않는다.** */
    fun focus(id: String) {
        _selectedId.value = if (_selectedId.value == id) null else id
    }

    /**
     * 채널 패널을 열 때의 포커스 — **토글이 아니다.**
     *
     * [focus] 는 카드를 다시 눌러 접는 조작(데스크톱 §4.1)이라 같은 값을 넣으면 포커스가 풀린다.
     * 패널을 여는 쪽이 그것을 쓰면 «열었는데 아무것도 안 보이는» 상태가 된다.
     */
    fun setFocus(id: String) {
        if (id.isBlank()) return
        // 읽음으로 닫지 않는다 — 채널 상세를 여는 것은 글을 읽은 것이 아니다(메시지는 다른 면에 있다). 읽음은 «메시지» 면에서
        //   그 대화가 보일 때 된다(`PttMessagesViewModel`).
        _selectedId.value = id
    }

    // ── 발언 대상 조작 ──
    /**
     * 카드 체크 — 발언 대상 집합에 넣고 뺀다. **포커스는 건드리지 않는다.** 여럿을 켜면 한 번의 PTT 가 그 전부로 나간다
     * (동시 발언 = 단말 팬아웃 — [TalkRules]). 참여하지 않은 채널은 대상이 될 수 없다 — 왜인지 말해 준다.
     */
    fun toggleTarget(id: String) {
        val card = cards.value.firstOrNull { it.id == id } ?: return
        if (!card.canCheck) {
            if (card.session == null && card.group != null)
                s.notify(com.cims.ue.dispatch.session.NoticeLevel.INFO, "${card.title} — 먼저 [참여]하세요")
            return
        }
        applyTargets(TalkRules.toggle(_targetIds.value, id))
    }

    /**
     * **대상 변경은 전부 여기를 지난다.**
     *
     * 대상을 바꾸면서 **빠진 호의 floor 를 놓지 않으면 그 채널이 계속 송출한다** — 화면은 새 대상을
     * 가리키는데 마이크는 옛 채널로 간다. 한 곳에 모아 두지 않으면 경로가 늘 때마다 같은 결함이 다시
     * 생긴다(`toggleTarget` 은 지켰는데 `setSingleTarget`·`pruneTargets` 가 빠져 있었다).
     *
     * 불변: 대상에서 빠진 호는 **반드시** `floorRelease` 를 받고 `speakingCallIds` 가 그만큼 줄어든다.
     */
    private fun applyTargets(next: Set<String>, list: List<ChannelCard> = cards.value) {
        _targetIds.value = next
        val stillTarget = list.filter { it.id in next }.mapNotNull { it.session?.callId }.toSet()
        (speakingCallIds - stillTarget).filter(s::isCallAlive).forEach { release(it) }    // 끝난 호에는 보낼 것이 없다
        speakingCallIds = speakingCallIds intersect stillTarget
        // 대상이 없거나 발언하던 채널이 전부 빠졌으면 잠금도 의미가 없다 — 화면과 실제를 맞춘다.
        if (next.isEmpty() || speakingCallIds.isEmpty()) _locked.value = false
    }

    /** 이 채널 하나만 발언 대상으로 — 데스크톱 `SetSingleTarget` 과 같다. */
    fun setSingleTarget(id: String) {
        val card = cards.value.firstOrNull { it.id == id } ?: return
        if (!card.canCheck) return
        applyTargets(setOf(id))
    }

    /** 모두 해제 — 선택만 지우는 게 아니라 **요청해 둔 floor 도 푼다**. */
    fun clearTargets() {
        releaseAll()
        _targetIds.value = emptySet()
        _locked.value = false
    }

    /** 세션이 끝났거나 전이중으로 바뀐 대상은 자동으로 뺀다. */
    fun pruneTargets() {
        val ok = cards.value.filter { it.canCheck }.map { it.id }.toSet()
        if (!ok.containsAll(_targetIds.value)) applyTargets(_targetIds.value intersect ok)
    }

    // ── 발언 ──
    /** 누름의 차례 — 늦게 도는 뒤처리(잠금 풀기)가 **그 누름**의 것일 때만 손대게 한다. */
    private var pressSeq = 0

    /** PTT 누름 — 대상 **전부**에 floor 요청. 잠금 발언이면 토글로 동작한다. */
    fun pttDown(lockEnabled: Boolean) {
        if (lockEnabled && _locked.value) { releaseAll(); _locked.value = false; return }
        val ids = _targetIds.value
        val callIds = cards.value.filter { it.id in ids }.mapNotNull { it.session?.callId }.toSet()
        if (callIds.isEmpty()) return
        (speakingCallIds - callIds).forEach { release(it) }     // 이전 요청이 남아 있으면 먼저 푼다
        speakingCallIds = callIds
        lockSeenBusy = false
        val press = ++pressSeq
        // 잠금은 요청을 띄우기 **전에** 적는다 — 요청이 중단 없이 곧바로 실패하면(영상 우선·엔진 없음) 아래 블록이 그 자리에서
        //   끝까지 돌아 잠금을 풀고, 뒤에 적으면 그 위에 다시 «잠금» 이 덮인다.
        if (lockEnabled) _locked.value = true
        scope.launch {
            val requested = callIds.count { s.floorRequest(it).ok }
            // 하나도 요청되지 않았다(영상 우선으로 막힘 · 명령 실패) — 소유와 잠금을 남기면 말하지 않는데 «잠금» 으로 보인다.
            if (requested == 0 && speakingCallIds == callIds) { speakingCallIds = emptySet(); _locked.value = false }
            // 요청은 갔는데 «요청·대기·발언» 인 카드를 한 번도 못 본 채 끝났다(곧바로 거부 — 상태가 한꺼번에 접혀 중간이 보이지
            //   않았다). 잠금이 풀릴 계기가 없어 «잠금» 이 남고 다음 누름이 풀기만 한다 — 잠시 뒤에도 전부 한가하면 푼다.
            else if (lockEnabled) {
                kotlinx.coroutines.delay(LOCK_SETTLE_MS)
                if (press == pressSeq && _locked.value && !lockSeenBusy && speakingCallIds == callIds &&
                    cards.value.none { it.session?.callId in callIds && it.talkBusy }) { releaseAll(); _locked.value = false }
            }
        }
    }

    /** PTT 뗌 — 잠금 중이면 무시(다음 누름이 푼다). */
    fun pttUp(lockEnabled: Boolean) {
        if (lockEnabled && _locked.value) return
        releaseAll()
    }

    /**
     * 요청해 둔 호를 **전부** 해제한다. 화면이 사라지거나 대상이 바뀌거나 ViewModel 이 정리될 때도
     * 부른다 — 이 경로가 빠지면 마이크가 열린 채 남는다.
     */
    fun releaseAll() = releaseAll(viaSession = false)

    /**
     * 요청해 둔 floor 를 전부 놓는다.
     *
     * [viaSession] 은 **이 VM 이 버려질 때** 쓴다. 자기 스코프에 실으면 바로 뒤의 `close()` 가
     * 취소해 해제 명령이 나가지 못하고 **마이크가 열린 채 남는다** — 세션 스코프(호와 같은 수명)로
     * 보내야 끝까지 간다.
     */
    private fun releaseAll(viaSession: Boolean) {
        val ids = speakingCallIds
        speakingCallIds = emptySet()
        if (ids.isEmpty()) return
        if (viaSession) s.scopeLaunch { ids.forEach { s.floorRelease(it) } }
        else scope.launch { ids.forEach { s.floorRelease(it) } }
    }

    private fun release(callId: Int) { scope.launch { s.floorRelease(callId) } }

    /** floor 가 회수·시한 만료되면 잠금을 풀고 소유도 놓는다. */
    fun onFloorLost(callId: Int? = null) {
        _locked.value = false
        speakingCallIds = if (callId == null) emptySet() else speakingCallIds - callId
    }

    /** ViewModel 이 정리될 때도 발언을 놓는다 — 최후의 방어선. */
    override fun close() {
        releaseAll(viaSession = true)     // 내 스코프는 곧 끊긴다 — 해제는 세션이 끝까지 보낸다
        // 일제 통화 한 버튼을 켠 채 닫힌다 — 놓은 것으로 친다. 개시가 아직 돌아오지 않았으면 돌아오는 대로 끝낸다
        //   (개시 블록은 세션 스코프에서 돈다 — `beginBroadcast`).
        if (_broadcastHeld.value != null) { bcReleased = true; if (bcCallId >= 0) finishBroadcast() }
        super.close()
    }

    // ── 일제 통화 한 버튼(dispatch_desktop_ui.md §4.1 · mcptt_broadcast_group_call.md U6) ──
    //   누르는 동안 개시하고 말하며 놓으면 끝(잠금 발언이면 누를 때마다 켜고 끈다). 개시 INVITE 가 암묵적 발언 요청이라
    //   (TS 24.380 §14.2.5) PTT 를 따로 누르지 않는다. 한 번에 하나. 놓으면 호 성립 전 = CANCEL, 뒤 = Floor Release(→ 코어가 호 해제).
    //   개시 전(그룹 종류 조회 중)에 놓으면 개시 직후 끝낸다.


    private val lockTalk: Boolean get() = s.settingsSnapshot().lockTalk

    /** 채널 머리 [일제 통화] 누름. */
    fun broadcastGroupDown(card: ChannelCard) {
        _broadcastHeld.value?.let { held -> if (lockTalk && held == card.id) broadcastEnd(); return }
        val g = card.group ?: return
        if (!card.canBroadcast) return
        beginBroadcast(card.id) { s.startBroadcast(g.id) }
    }

    /** [채널 추가] 패널의 [일제 통화] 누름 — 고른 사람들에게 애드혹 일제 통화(TS 24.379 §17.2.2.1.1 9)). */
    fun broadcastAdhocDown(members: List<String>) {
        _broadcastHeld.value?.let { held -> if (lockTalk && held == ADHOC_BROADCAST) broadcastEnd(); return }
        if (members.isEmpty()) return
        beginBroadcast(ADHOC_BROADCAST) { s.startAdhocBroadcast(members) }
    }

    /** 뗌 — 잠금 발언이면 무시(다음 누름이 끝낸다). */
    fun broadcastUp() { if (!lockTalk) broadcastEnd() }

    private fun beginBroadcast(key: String, start: suspend () -> CimsResult<Int>) {
        _broadcastHeld.value = key
        bcCallId = -1
        bcReleased = false
        bcSeen = false
        // 앞선 개시(개별·애드혹)의 실패 사유가 남아 있으면 지운다 — 남으면 이번 일제 통화가 성공해도 패널이 «실패» 로 읽어
        //   고름·패널을 정리하지 않는다.
        if (key == ADHOC_BROADCAST) _originError.value = null
        // 편성 그룹의 일제 통화는 세션이 서면 **단일 발언 대상**이 된다(데스크톱 `Operation.Broadcast → SetSingleTarget`) — 누르고
        //   말하는 동안 발언 바가 «대상 없음» 이 아니라 그 채널·남은 발언·«놓으면 끝납니다» 를 말한다. 애드혹은 «내가 건 호 우선».
        if (key != ADHOC_BROADCAST) { pendingTargetId = key; pendingSeen = false }
        // 세션 스코프에서 돈다 — 화면이 닫혀 이 VM 의 스코프가 끊겨도 개시한 호의 id 를 받아 끝낼 수 있어야 한다. 받지 못하면
        //   암묵 승인된 발언권을 든 일제 통화가 아무도 놓지 못한 채 남는다.
        s.scopeLaunch {
            val r = start()
            if (_broadcastHeld.value != key) return@scopeLaunch
            if (!r.ok) {
                // [채널 추가] 패널의 [일제 통화] 는 패널이 그 자리에 적는다. 채널 상세의 [일제 통화] 는 적을 자리가 없어 토스트다
                //   (§6.2a-2) — 패널 오류로 두면 보이지 않다가 다음에 패널을 열 때 엉뚱하게 뜬다.
                if (key == ADHOC_BROADCAST) _originError.value = r.reason else s.report(TextArea.PTT_JOIN, r)
                if (pendingTargetId == key) pendingTargetId = null
                clearBroadcastHold(); return@scopeLaunch
            }
            bcCallId = r.value!!
            // 개시가 돌아오기 전에 호가 이미 끝났다(곧바로 거절 — 403·404·480). 호 이벤트는 id 를 모를 때 지나갔으므로 여기서
            //   풀지 않으면 «일제 통화 중» 이 남고, 끝낼 때 낡은 id 로 다른 호를 끊게 된다.
            if (!s.isCallAlive(bcCallId)) {
                // 서지 못한 호를 기다리던 발언 대상도 지운다 — 남으면 뒤에 그 그룹으로 온 착신이 단일 발언 대상이 되며 지금 발언을 놓는다
                if (pendingTargetId == key) pendingTargetId = null
                clearBroadcastHold(); return@scopeLaunch
            }
            // 호 이벤트가 먼저 와 세션이 이미 목록에 올라 있었다 — 그때는 id 를 몰라 «봤다» 를 적지 못했다. 여기서 맞춰 두어야
            //   그 호가 끝날 때(곧바로 거절 포함) 누름 상태가 풀린다.
            if (s.sessions.value.any { it.callId == bcCallId }) bcSeen = true
            if (bcReleased) finishBroadcast()
        }
    }

    private fun broadcastEnd() {
        if (_broadcastHeld.value == null) return
        bcReleased = true
        if (bcCallId >= 0) finishBroadcast()
    }

    private fun finishBroadcast() {
        val id = bcCallId
        clearBroadcastHold()
        speakingCallIds = speakingCallIds - id
        s.scopeLaunch { s.releaseBroadcast(id) }        // 세션 스코프 — 화면이 사라져도 끝까지 간다
    }

    private fun clearBroadcastHold() {
        _broadcastHeld.value = null
        bcCallId = -1
        bcReleased = false
        bcSeen = false
    }

    // ── 세션 조작 ──
    /** 참여 — 포커스를 옮기고, 세션이 서면 발언 대상이 된다(위 [pendingTargetId]). */
    // ── 개별 통화·애드혹 그룹 통화(§4.1) ──────────────────────────────────────

    /** PTT 주소록 — 개별·애드혹 대상 후보. 세션이 로그인 때 받아 둔 것을 본다. */
    val pttBook: StateFlow<DirectoryBook> = s.pttBook

    /** 영상 호(MCVideo) — 카드 1줄 «영상 n» 태그의 원천(§6.14). 영상 호는 카드를 만들지 않는다. */
    val videoCalls: StateFlow<List<com.cims.ue.dispatch.session.VideoCall>> get() = s.videoCalls

    /** 내 PTT 번호 — 로스터 칩의 «나» 표시. */
    val myPttNumber: String get() = userPart(s.myPttId)

    /** 청취가 로스터에 드러나지 않는가(역할 `listen_visibility`) — 채널 상세가 청취자 줄을 세울지 가른다. */
    val listenHidden: Boolean get() = s.listenHidden

    /** PTT 가입자의 지금 상태(«<그룹> 발언»·«<그룹> 참여») — 사용자 패널의 줄. */
    fun pttStatusOf(number: String): String = s.pttStatusOf(number)
    /** 같은 것을 목록용으로 한 번에(번호 정규형 → 상태). */
    fun pttStatusMap(): Map<String, String> = s.pttStatusMap()

    /**
     * 1초 틱 — 세션이 있는 동안만 간다. 카드·채널 상세의 «경과»·«발언 n초» 는 계산 속성이라 스스로 알리지 않는다 — 화면이
     * 이 값을 읽어 1초마다 다시 그린다(통화 쪽 `CallDeskViewModel` 과 같은 틱).
     */
    val tick: StateFlow<Long> get() = s.tick

    /**
     * 번호 → 이름(양 주소록, 없으면 빈 문자열). 로스터 칩·접속자 줄의 라벨 — **이름만**이다(데스크톱 `RosterLine` 과 같다).
     * PTT 번호는 길어서 «번호 이름» 으로 병기하면 카드 한 줄에 한 사람도 못 들어간다. 번호는 채널 상세의 사람 행이 따로 보인다.
     */
    fun nameOf(number: String): String = s.nameOrEmpty(number)

    /**
     * 사람 메뉴가 쓰는 사람 목록 — ③ VM 과 같은 순수 함수(`mergePeople`)를 쓴다.
     *
     * 흐름 배선만 VM 마다 두는 이유는 수명 때문이다(각 VM 이 자기 scope 에서 산다). **판정 규칙은 한 곳**
     * (`ui/PersonMenu.kt`)이므로 두 벌이 되지 않는다.
     */
    val people: StateFlow<List<PersonEntry>> =
        combine(s.phoneBook, s.pttBook) { phone, ptt ->
            mergePeople(phone, ptt, exclude = s.myLineKeys())
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    fun personAt(numberOrUri: String): PersonEntry? =
        resolvePerson(people.value, numberOrUri, s.phoneBook.value,
                      fallbackName = s.displayLabel(numberOrUri))

    private val _originError = MutableStateFlow<String?>(null)
    /** 발신 실패 사유 — [채널 추가] 패널이 보여 준다(§9 사전). 성공하면 패널이 닫히므로 비운다. */
    val originError: StateFlow<String?> = _originError.asStateFlow()

    fun clearOriginError() { _originError.value = null }

    /**
     * 개별 통화 발신. 성공하면 [onDone] — 패널을 닫는다.
     *
     * 반이중이 기본이다. 전이중은 마이크가 늘 열려 있어 발언 대상이 되지 못한다(카드 [음소거]로 다룬다).
     */
    fun startPrivate(peer: String, fullDuplex: Boolean, emergency: Boolean, onDone: () -> Unit) {
        scope.launch {
            val r = s.startPrivateCall(peer, fullDuplex, emergency)
            if (r.ok) { _originError.value = null; onDone() } else _originError.value = r.reason
        }
    }

    /** 애드혹 그룹 통화 개설 — 대상 N명(최소 1). */
    fun startAdhoc(members: List<String>, emergency: Boolean, onDone: () -> Unit) {
        scope.launch {
            val r = s.startAdhoc(members, emergency)
            if (r.ok) { _originError.value = null; onDone() } else _originError.value = r.reason
        }
    }

    fun join(card: ChannelCard) = join(card, emergency = false)

    fun joinEmergency(card: ChannelCard) = join(card, emergency = true)

    private fun join(card: ChannelCard, emergency: Boolean) {
        pendingTargetId = card.id
        pendingSeen = false
        _selectedId.value = card.id
        scope.launch {
            // 명령이 곧바로 실패했다 — 세션이 서지 않으니 대기 대상도 거둔다
            if (!s.joinGroup(card.id, emergency = emergency).ok && pendingTargetId == card.id) pendingTargetId = null
        }
    }

    /** 진행 중 긴급 상향·하향 — 채널 상세의 [긴급]·[긴급 해제](확인은 패널이 받았다). */
    fun setEmergency(card: ChannelCard, on: Boolean) {
        val callId = card.session?.callId ?: return
        scope.launch { s.setEmergency(callId, on) }
    }

    /** 채널 상세 [긴급 해제]·[임박 해제] — 배너와 **같은 경로**(`cancelCondition`: 성립·진행 중 변경·청취 leg 판정 + ⑤ «해제 요청»). */
    fun cancelCondition(card: ChannelCard) {
        val callId = card.session?.callId ?: return
        scope.launch { s.cancelCondition(callId) }
    }

    fun leave(card: ChannelCard) {
        pendingTargetId = null
        card.session?.let { se -> scope.launch { s.leave(se.callId) } }
    }

    /**
     * 전이중 개별 통화 음소거 토글 — 데스크톱 카드 [음소거](`ToggleMute`)와 같다.
     *
     * 카드는 **자격만** 본다(반이중은 floor 가 마이크를 다루므로 무시한다). 뒤집을 값은 카드 사본이 아니라
     * 명령 직전의 코어 스냅샷에서 읽는다 — ③ 통화 카드와 같은 경로(`toggleMuted`)라 연타가 합쳐지지 않는다.
     */
    fun toggleMute(id: String) {
        val card = cards.value.firstOrNull { it.id == id } ?: return
        if (!card.canMute) return
        val callId = card.session?.callId ?: return
        scope.launch { s.toggleMuted(callId) }
    }

    companion object {
        /** [broadcastHeld] 의 애드혹 일제 통화 값 — 카드 id 와 겹치지 않는다(카드 id 는 그룹 id·adhoc-·call-). */
        const val ADHOC_BROADCAST = "#adhoc-broadcast"
        /** 잠금 발언 — 요청 뒤 이만큼 지나도 요청·대기·발언인 카드가 없으면 잠금을 푼다. */
        const val LOCK_SETTLE_MS = 2_000L
    }
}
