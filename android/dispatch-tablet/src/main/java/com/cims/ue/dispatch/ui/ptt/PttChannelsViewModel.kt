// ① 내 채널 + 발언 바 (docs/design/features/android_dispatch_tablet.md §6.3·§6.7,
//                     dispatch_desktop_ui.md §4.1)
//
// **포커스 ≠ 발언 대상.** 이 파일의 가장 중요한 계약이다.
//   · 포커스(`selectedId`) = 지금 **보는** 채널. ④ 메시지·⑤ 이벤트가 따라간다. 하나뿐이다.
//   · 발언 대상(`targets`) = 지금 **말하는** 채널의 집합. 카드 체크로 고른다.
// 둘은 독립이다 — 순찰1을 보면서 상황실에 말할 수 있다. 섞으면 관제사가 엉뚱한 채널로 송출한다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.startAdhoc
import com.cims.ue.dispatch.session.startPrivateCall
import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.floorRelease
import com.cims.ue.dispatch.session.floorRequest
import com.cims.ue.dispatch.session.joinGroup
import com.cims.ue.dispatch.session.leave
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** ① 카드의 종류 — 멤버 그룹은 항상 서 있고, 사설콜·애드혹은 세션이 있을 때만 선다. */
enum class CardKind { MEMBER, PRIVATE, ADHOC }

/**
 * ① 내 채널 카드 하나.
 *
 * 멤버 그룹 카드는 **세션이 없어도 선다**(참여하지 않은 채널도 보여야 한다). 사설콜·애드혹은
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
    /** 핀 번호(Ctrl+n). 카드가 빠지면 다시 매겨진다. */
    val index: Int = 0,
) {
    val badge: String get() = when (kind) {
        CardKind.MEMBER -> "멤버"; CardKind.PRIVATE -> "사설콜"; CardKind.ADHOC -> "임시"
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
     * 발언 대상이 될 수 있는가 — **참여 중 + 반이중**.
     * 전이중 사설콜은 마이크가 늘 열려 있어 floor 가 없다(음소거로 다룬다).
     */
    val canCheck: Boolean get() = joined && session?.isFullDuplex != true

    /** 1줄 오른쪽 — 진행 중이면 경과, 아니면 상태. */
    val stateText: String get() = when {
        joined -> fmtElapsed(session!!.elapsedMs)
        group?.hasSession == true -> "진행(미참여)"
        else -> "대기"
    }

    /** 2줄 — 발언자·사유. 대기 중이면 멤버 수. */
    val line2: String get() = when {
        speaker.isNotEmpty() -> "발언 $speaker" + fmtSpeaker()
        joined && floorNote.isNotEmpty() -> floorNote
        joined -> "발언 없음"
        kind == CardKind.MEMBER -> "멤버 $memberCount"
        else -> ""
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
     * 해제를 «현재 대상» 으로 하면, 누른 채 대상이 바뀌었을 때(Ctrl+n·자동 승격·세션 종료) 원래 요청한
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

    /**
     * ① 카드 목록 — **멤버 그룹 전부 + 내가 건 사설콜·애드혹**. 항상 전부 보인다(필터는 ② 에만 있다).
     * 순서: 멤버 그룹(이름) → 사설콜·애드혹(시작 순).
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
                        id = se.info.groupId.ifEmpty { "call-${se.callId}" },
                        kind = if (se.kind == SessionKind.PTT_PRIVATE) CardKind.PRIVATE else CardKind.ADHOC,
                        title = se.title.ifEmpty { se.info.groupId },
                        session = se)
                }
            (memberCards + adhocCards).mapIndexed { i, c -> c.copy(index = i + 1) }
        }.onEach { list ->
            // 참여가 성립하면(세션이 붙어 canCheck) 대기 중이던 채널을 발언 대상으로 올린다.
            pendingTargetId?.let { id ->
                if (list.firstOrNull { it.id == id }?.canCheck == true) {
                    setSingleTarget(id)
                    pendingTargetId = null
                }
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

    // ── 포커스 조작 ──
    /** 카드 탭 — 같은 카드를 다시 누르면 접힌다. **발언 대상은 건드리지 않는다.** */
    fun focus(id: String) {
        _selectedId.value = if (_selectedId.value == id) null else id
        _selectedId.value?.let { s.markRead(it) }
    }

    /** Ctrl+n — n 번째 카드로 포커스 + 단일 발언 대상(데스크톱과 같다). */
    fun focusIndex(n: Int) {
        val c = cards.value.firstOrNull { it.index == n } ?: return
        _selectedId.value = c.id
        s.markRead(c.id)
        if (c.canCheck) setSingleTarget(c.id)
    }

    // ── 발언 대상 조작 ──
    /**
     * 카드 체크 — 발언 대상 집합에 넣고 뺀다. **포커스는 건드리지 않는다.**
     * 상한([maxTargets])을 넘으면 가장 오래된 것을 밀어낸다(팬아웃 전에는 1개라 교체가 된다).
     */
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

    // ── 세션 조작 ──
    /** 참여 — 포커스를 옮기고, 세션이 서면 발언 대상이 된다(위 [pendingTargetId]). */
    // ── 사설콜·애드혹(§4.1) ──────────────────────────────────────────────────

    /** PTT 주소록 — 사설콜·애드혹 대상 후보. 세션이 로그인 때 받아 둔 것을 본다. */
    val pttBook: StateFlow<DirectoryBook> = s.pttBook

    private val _originError = MutableStateFlow<String?>(null)
    /** 발신 실패 사유 — 시트가 보여 준다(§9 사전). 성공하면 시트가 닫히므로 비운다. */
    val originError: StateFlow<String?> = _originError.asStateFlow()

    fun clearOriginError() { _originError.value = null }

    /**
     * 사설콜 발신. 성공하면 [onDone] — 시트를 닫는다.
     *
     * 반이중이 기본이다. 전이중은 마이크가 늘 열려 있어 발언 대상이 되지 못한다(카드 [음소거]로 다룬다).
     */
    fun startPrivate(peer: String, fullDuplex: Boolean, emergency: Boolean, onDone: () -> Unit) {
        scope.launch {
            val r = s.startPrivateCall(peer, fullDuplex, emergency)
            if (r.ok) { _originError.value = null; onDone() } else _originError.value = r.reason
        }
    }

    /** 애드혹 개설 — 대상 N명(최소 1). */
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

    companion object {
        /** 코어에 발언 대상 집합 API 가 들어오면 true 로 바꾼다 — 그것 하나로 다중 발언이 열린다. */
        const val MULTI_TALK_SUPPORTED = false
    }
}
