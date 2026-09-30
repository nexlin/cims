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
import com.cims.ue.dispatch.session.toggleMuted
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
    val imminentPeril: Boolean get() = session?.isImminentPeril == true
    val speaking: Boolean get() = session?.isSpeaking == true
    val requesting: Boolean get() = session?.isRequesting == true
    val queued: Boolean get() = session?.isQueued == true
    val speaker: String get() = session?.speaker.orEmpty()
    val floorNote: String get() = session?.floorNote.orEmpty()
    val participants: Int get() = group?.connectedCount ?: session?.adhocMembers?.size ?: 0
    val memberCount: Int get() = group?.memberCount ?: 0

    /**
     * 발언 대상이 될 수 있는가 — **참여 중 + 반이중 + 발언 요청 가능**.
     * 전이중 개별 통화는 마이크가 늘 열려 있어 floor 가 없다(음소거로 다룬다 — [canMute]). 남이 연 일제 통화의 수신 멤버는
     * Floor Taken 의 Permission 0 이라 요청할 수 없다(TS 24.380 §6.3.4.4.2 3d).
     */
    val canCheck: Boolean get() = joined && session?.isFullDuplex != true && session?.canRequestFloor != false

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
    val stateText: String get() = when {
        granted -> "승인"
        queued -> card.floorNote.ifEmpty { "대기" }
        requesting -> "요청"
        card.floorNote.isNotEmpty() -> card.floorNote
        else -> ""
    }
}

class PttChannelsViewModel(private val s: DispatchSession) : ScreenViewModel() {

    /**
     * 한 번에 발언할 수 있는 채널 수.
     *
     * 다중 채널 동시 발언은 **단말 팬아웃**으로 푼다 — 3GPP 에 UE 의 다중 그룹 동시 발언 절차가 없다.
     * 코어에 발언 대상 집합 API(`setTalkTargets`)가 들어오기 전까지 1개다. 발언 바·칩·게이지는 이미
     * 집합 기준이라 이 상수만 바꾸면 열린다(dispatch_desktop_ui.md §13).
     */
    val maxTargets: Int get() = if (MULTI_TALK_SUPPORTED) Int.MAX_VALUE else 1

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

    /**
     * 참여를 누른 채널 — 세션이 서면 **자동으로 단일 발언 대상**이 된다.
     *
     * 데스크톱은 발신(`Dir==Outgoing`)과 `Ctrl+n` 에서 같은 일을 한다(포커스 + 단일 대상). 태블릿에는
     * 그 단축키가 없어 [참여]가 그 자리를 대신한다 — **참여는 곧 말하겠다는 의도**이기 때문이다.
     * 포커스와 발언 대상이 독립이라는 계약은 그대로다(둘을 따로 바꿀 수 있다).
     */
    private var pendingTargetId: String? = null

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
            val memberCards = groups.filter { it.isMember }.sortedBy { it.name }.map { g ->
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
                        title = se.title.ifEmpty { se.info.groupId },
                        session = se)
                }
            memberCards + adhocCards
        }.onEach { list ->
            // 참여가 성립하면(세션이 붙어 canCheck) 대기 중이던 채널을 발언 대상으로 올린다.
            pendingTargetId?.let { id ->
                if (list.firstOrNull { it.id == id }?.canCheck == true) {
                    setSingleTarget(id)
                    pendingTargetId = null
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
            if (!ok.containsAll(_targetIds.value)) {
                _targetIds.value = _targetIds.value intersect ok
                if (_targetIds.value.isEmpty()) _locked.value = false
            }
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
        _selectedId.value?.let { s.markRead(it) }
    }

    /**
     * 채널 화면을 열 때의 포커스 — **토글이 아니다.**
     *
     * [focus] 는 카드를 다시 눌러 접는 조작(데스크톱 §4.1)이라 같은 값을 넣으면 포커스가 풀린다.
     * 화면을 여는 쪽이 그것을 쓰면 «열었는데 아무것도 안 보이는» 상태가 된다.
     */
    fun setFocus(id: String) {
        if (id.isBlank()) return
        _selectedId.value = id
        s.markRead(id)
    }

    // ── 발언 대상 조작 ──
    /**
     * 카드 체크 — 발언 대상 집합에 넣고 뺀다. **포커스는 건드리지 않는다.**
     * 상한([maxTargets])을 넘으면 가장 오래된 것을 밀어낸다(팬아웃 전에는 1개라 교체가 된다).
     */
    /**
     * 사람 메뉴의 «애드혹에 추가» 가 심어 두는 상대 — 발신 시트가 열릴 때 미리 골라 둔다.
     *
     * 시트를 직접 열지 않고 씨앗만 두는 이유: 시트는 ① 패널이 소유하는 화면 상태라 다른 탭(③ 일반통화)에서
     * 직접 띄울 수 없다. 데스크톱은 `PttOriginate.AddAdhoc(n)` + `PttOriginateOpen = true` 로 같은 일을 한다.
     */
    private val _adhocSeed = MutableStateFlow("")
    val adhocSeed: StateFlow<String> = _adhocSeed.asStateFlow()

    fun seedAdhoc(number: String) { if (number.isNotBlank()) _adhocSeed.value = number }

    /** 시트가 씨앗을 받아 갔다 — 한 번만 쓴다(닫았다 다시 열 때 또 끼어들면 안 된다). */
    fun consumeAdhocSeed(): String = _adhocSeed.value.also { _adhocSeed.value = "" }

    fun toggleTarget(id: String) {
        val card = cards.value.firstOrNull { it.id == id } ?: return
        if (!card.canCheck) return
        val cur = _targetIds.value
        applyTargets(when {
            id in cur -> cur - id
            cur.size < maxTargets -> cur + id
            maxTargets == 1 -> setOf(id)                 // 교체
            else -> cur.drop(1).toSet() + id
        })
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
    private fun applyTargets(next: Set<String>) {
        _targetIds.value = next
        val stillTarget = cards.value.filter { it.id in next }.mapNotNull { it.session?.callId }.toSet()
        (speakingCallIds - stillTarget).forEach { release(it) }
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
    /** PTT 누름 — 대상 **전부**에 floor 요청. 잠금 발언이면 토글로 동작한다. */
    fun pttDown(lockEnabled: Boolean) {
        if (lockEnabled && _locked.value) { releaseAll(); _locked.value = false; return }
        val ids = _targetIds.value
        val callIds = cards.value.filter { it.id in ids }.mapNotNull { it.session?.callId }.toSet()
        if (callIds.isEmpty()) return
        (speakingCallIds - callIds).forEach { release(it) }     // 이전 요청이 남아 있으면 먼저 푼다
        speakingCallIds = callIds
        scope.launch { callIds.forEach { s.floorRequest(it) } }
        if (lockEnabled) _locked.value = true
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

    /** 발신 시트 [애드혹] 의 [일제 통화] 누름 — 고른 사람들에게(TS 24.379 §17.2.2.1.1 9)). */
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
        scope.launch {
            val r = start()
            if (_broadcastHeld.value != key) return@launch
            if (!r.ok) { _originError.value = r.reason; clearBroadcastHold(); return@launch }
            bcCallId = r.value!!
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

    /** 내 PTT 번호 — 로스터 칩의 «나» 표시. */
    val myPttNumber: String get() = userPart(s.myPttId)

    /** 번호 → 이름(양 주소록). 로스터 칩 라벨. */
    fun nameOf(number: String): String = s.displayLabel(number).takeIf { it != number }.orEmpty()

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
    /** 발신 실패 사유 — 시트가 보여 준다(§9 사전). 성공하면 시트가 닫히므로 비운다. */
    val originError: StateFlow<String?> = _originError.asStateFlow()

    fun clearOriginError() { _originError.value = null }

    /**
     * 개별 통화 발신. 성공하면 [onDone] — 시트를 닫는다.
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

    fun join(card: ChannelCard) {
        pendingTargetId = card.id
        _selectedId.value = card.id
        scope.launch { s.joinGroup(card.id) }
    }

    fun joinEmergency(card: ChannelCard) {
        pendingTargetId = card.id
        _selectedId.value = card.id
        scope.launch { s.joinGroup(card.id, emergency = true) }
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
        /** 코어에 발언 대상 집합 API 가 들어오면 true 로 바꾼다 — 그것 하나로 다중 발언이 열린다. */
        const val MULTI_TALK_SUPPORTED = false
        /** [broadcastHeld] 의 애드혹 일제 통화 값 — 카드 id 와 겹치지 않는다(카드 id 는 그룹 id·adhoc-·call-). */
        const val ADHOC_BROADCAST = "#adhoc-broadcast"
    }
}
