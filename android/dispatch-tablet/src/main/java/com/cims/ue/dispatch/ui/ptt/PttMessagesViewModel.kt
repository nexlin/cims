// ④ PTT 메시지 (docs/design/features/android_dispatch_tablet.md §6.7, dispatch_desktop_ui.md §4.4)
//
// MCData SDS 스레드. **포커스 채널을 따라간다** — ① 에서 보는 채널이 바뀌면 이 패널도 바뀐다
// (따라가기 토글로 고정할 수 있다).
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.sendGroupSds
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** ④ 스레드 칩 하나. */
data class ThreadChip(val key: String, val title: String, val unread: Int, val lastAtMs: Long)

/**
 * 메시지 보관 → 스레드 칩 (순수 함수, 시험 대상).
 *
 * 최근 순으로 세운다 — 방금 온 것이 앞이어야 찾는다. 빈 스레드는 내지 않는다(보관을 지우고 남은 키).
 * 제목은 그룹이면 그룹 이름, 1:1 이면 주소록 이름(없으면 번호) — `nameOf` 가 그 판정을 갖는다.
 */
internal fun threadChips(
    all: Map<String, List<Message>>,
    nameOf: (String) -> String,
): List<ThreadChip> =
    all.entries.mapNotNull { (key, list) ->
        if (list.isEmpty()) return@mapNotNull null
        ThreadChip(
            key = key,
            title = nameOf(key).ifBlank { key },
            unread = list.count { !it.read && !it.outgoing },
            lastAtMs = list.maxOf { it.atMs })
    }.sortedByDescending { it.lastAtMs }

class PttMessagesViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _follow = MutableStateFlow(true)
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
            threadChips(all) { key -> groups.firstOrNull { it.id == key }?.name ?: s.displayLabel(key) }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

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
    }

    /** 칩을 눌러 그 스레드로 — 따라가기를 끄고 고정한다(`openThread` 와 같다). */
    fun pickThread(key: String) = openThread(key)

    fun send(text: String) {
        val g = groupId.value ?: return
        if (text.isBlank()) return
        scope.launch { s.sendGroupSds(g, text.trim()) }
    }
}
