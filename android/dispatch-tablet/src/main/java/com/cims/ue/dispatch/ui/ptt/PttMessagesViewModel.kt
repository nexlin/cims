// ④ PTT 메시지 (docs/design/features/android_dispatch_tablet.md §6.7, dispatch_desktop_ui.md §4.4)
//
// MCData SDS 스레드. **포커스 채널을 따라간다** — ① 에서 보는 채널이 바뀌면 이 패널도 바뀐다
// (따라가기 토글로 고정할 수 있다).
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.sendSdsTo
import com.cims.ue.dispatch.session.resendSds
import com.cims.ue.dispatch.ui.RecipientOption
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** ④ 스레드 칩 하나. */
data class ThreadChip(
    val key: String,
    val title: String,
    val unread: Int,
    val lastAtMs: Long,
    /** 마지막 한 통 미리보기 — «나: …»·«이당직: …»(그룹)·«…»(1:1). */
    val last: String = "",
    /** 편성 그룹 스레드(아니면 사람 1:1). */
    val group: Boolean = false,
)

/**
 * 메시지 보관 → 스레드 칩 (순수 함수, 시험 대상).
 *
 * 최근 순으로 세운다 — 방금 온 것이 앞이어야 찾는다. 빈 스레드는 내지 않는다(보관을 지우고 남은 키).
 * 제목은 그룹이면 그룹 이름, 1:1 이면 주소록 이름(없으면 번호) — `nameOf` 가 그 판정을 갖는다.
 */
internal fun threadChips(
    all: Map<String, List<Message>>,
    isGroup: (String) -> Boolean = { false },
    nameOf: (String) -> String,
): List<ThreadChip> =
    all.entries.mapNotNull { (key, list) ->
        if (list.isEmpty()) return@mapNotNull null
        val group = isGroup(key)
        val lastMsg = list.maxBy { it.atMs }
        ThreadChip(
            key = key,
            title = nameOf(key).ifBlank { key },
            unread = list.count { !it.read && !it.outgoing },
            lastAtMs = lastMsg.atMs,
            last = when {
                lastMsg.outgoing -> "나: ${lastMsg.text}"
                group && lastMsg.fromName.isNotBlank() -> "${lastMsg.fromName}: ${lastMsg.text}"
                else -> lastMsg.text
            },
            group = group)
    }.sortedByDescending { it.lastAtMs }

class PttMessagesViewModel(private val s: DispatchSession) : ScreenViewModel() {

    // 저장된 값에서 시작한다(데스크톱 `FollowChannelThread`, 기본 켬). 스레드를 직접 열 때 잠시 끄는 것([openThread])은
    //   저장하지 않는다 — 사람이 [따라가기] 를 누른 것만 남긴다(데스크톱 `ToggleFollow` 와 같다).
    private val _follow = MutableStateFlow(s.settingsSnapshot().followChannelThread)
    /** 포커스 채널 따라가기. 끄면 [pinnedGroupId] 에 고정된다. */
    val follow: StateFlow<Boolean> = _follow.asStateFlow()

    private val _pinned = MutableStateFlow<String?>(null)
    private val _focusGroupId = MutableStateFlow<String?>(null)

    /** 지금 보고 있는 스레드의 그룹. */
    val groupId: StateFlow<String?> =
        combine(_follow, _focusGroupId, _pinned) { follow, focus, pinned ->
            if (follow) focus else pinned ?: focus
        }.stateIn(scope, SharingStarted.Eagerly, null)

    /**
     * ④ 스레드 칩 — 메시지가 오간 스레드 전부(데스크톱 §4.4 «스레드 칩»).
     *
     * **이게 없으면 1:1 스레드를 열고 돌아갈 수 없다.** 사람 메뉴의 «문자(SDS)» 가 임의 키를 고정하는데
     * (`openThread`), 목록이 없으면 «따라가기» 를 다시 켜서 포커스 채널로 튕기는 것 말고는 길이 없다.
     */
    val threads: StateFlow<List<ThreadChip>> =
        combine(s.messages, s.groups) { all, groups ->
            threadChips(all, isGroup = { key -> groups.any { it.id == key } }) { key ->
                groups.firstOrNull { it.id == key }?.name ?: s.displayLabel(key)
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 고른 대화가 편성 그룹이면 그 그룹 — 머리의 «그룹 전원 · 편성 12 · 접속 7». */
    val groupInfo: StateFlow<com.cims.ue.dispatch.session.GroupInfo?> =
        combine(groupId, s.groups) { g, groups -> groups.firstOrNull { it.id == g } }
            .stateIn(scope, SharingStarted.Eagerly, null)

    val thread: StateFlow<List<Message>> =
        combine(s.messages, groupId) { all, g -> if (g == null) emptyList() else all[g].orEmpty() }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    // 스레드 키는 그룹 id 이거나 **사람의 PTT 번호**다 — 1:1 SDS 는 서버가 `groupUri` 없이 오므로 세션이
    //   보낸 사람 번호를 키로 삼는다(`applySds`). 그래서 제목도 그룹 → 주소록 이름 → 키 순으로 찾는다.
    val title: StateFlow<String> =
        groupId.let { f -> combine(f, s.groups) { g, groups ->
            g?.let { id ->
                groups.firstOrNull { it.id == id }?.name
                    ?: s.displayLabel(id).ifBlank { id }
            } ?: "채널을 고르세요"
        } }.stateIn(scope, SharingStarted.Eagerly, "")

    /** ① 이 포커스를 바꾸면 부른다. */
    fun onFocusChanged(groupId: String?) {
        _focusGroupId.value = groupId
        if (_follow.value && groupId != null) s.markRead(groupId)
    }

    /**
     * 임의 스레드 열기 — 사람 메뉴의 «문자(SDS)» 가 부른다.
     *
     * 채널 따라가기를 **끄고** 고정한다. 켜 둔 채로 키만 바꾸면 ① 의 포커스가 다음 순간 덮어써서 방금 연
     * 스레드가 사라진다(데스크톱 `SelectKey` 도 스레드를 직접 지정한다).
     */
    fun openThread(key: String) {
        if (key.isBlank()) return
        _follow.value = false
        _pinned.value = key
        s.markRead(key)
    }

    fun toggleFollow() {
        _follow.value = !_follow.value
        if (!_follow.value) _pinned.value = groupId.value
        val v = _follow.value
        s.updateSettings { it.copy(followChannelThread = v) }
    }

    /** 칩을 눌러 그 스레드로 — 따라가기를 끄고 고정한다(`openThread` 와 같다). */
    fun pickThread(key: String) = openThread(key)

    /**
     * «새 대화» 후보 — **편성 그룹 + PTT 주소록 사람**.
     *
     * 무전 메시지는 두 갈래다: 그룹으로 보내면 편성 전원이 받고(단톡방), 사람으로 보내면 그 사람만 받는다
     * (1:1 SDS). 둘을 섞어 두면 잘못 골랐을 때 되돌릴 수 없으므로 후보에 그룹 표를 달아 구분한다.
     *
     * 직접 입력 칸은 두지 않는다 — 무전은 편성·주소록 밖으로 보낼 자리가 없다(있는 번호만 닿는다).
     */
    val candidates: StateFlow<List<RecipientOption>> =
        combine(s.groups, s.pttBook) { groups, book ->
            groups.map { g ->
                RecipientOption(key = g.id, title = g.name.ifBlank { g.id },
                    subtitle = "편성 ${g.memberCount}명", group = true)
            } + book.entries.filter { it.msisdn.isNotBlank() }.map { e ->
                RecipientOption(key = e.msisdn, title = e.name.ifBlank { e.msisdn },
                    subtitle = listOf(e.msisdn, book.orgPath(e.org)).filter { it.isNotBlank() }
                        .joinToString(" · "))
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 고른 대화가 그룹인가 — 말풍선 머리(보낸 사람)를 그룹에서만 보이게 하는 데 쓴다. */
    val isGroup: StateFlow<Boolean> =
        combine(groupId, s.groups) { g, groups -> g != null && groups.any { it.id == g } }
            .stateIn(scope, SharingStarted.Eagerly, false)

    /**
     * 보내기 — 키가 **그룹이면 그룹 SDS, 사람이면 1:1 SDS**.
     *
     * 판정은 세션이 한다(`sendSdsTo`). 전에는 무조건 그룹 경로로 보냈는데, 사람 스레드에서 답장하면
     * request-type 이 `group-sds` 인 채로 나가 서버가 그룹 게이트를 거치고 받는 쪽 스레드 귀속도
     * 틀어졌다(mcdata_messaging.md §4).
     */
    /** 실패한 말풍선 다시 보내기 — 같은 말풍선이 갱신된다(`resendSds`). */
    fun resend(m: Message) { scope.launch { s.resendSds(m) } }

    fun send(text: String) {
        val g = groupId.value ?: return
        if (text.isBlank()) return
        scope.launch { s.sendSdsTo(g, text.trim()) }
    }

    /** «새 대화» 에서 고른 상대로 연다 — 그룹이든 사람이든 스레드 키 하나다. */
    fun openTo(key: String) = openThread(key.trim())
}
