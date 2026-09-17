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

    val thread: StateFlow<List<Message>> =
        combine(s.messages, groupId) { all, g -> if (g == null) emptyList() else all[g].orEmpty() }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    val title: StateFlow<String> =
        groupId.let { f -> combine(f, s.groups) { g, groups ->
            g?.let { id -> groups.firstOrNull { it.id == id }?.name ?: id } ?: "채널을 고르세요"
        } }.stateIn(scope, SharingStarted.Eagerly, "")

    /** ① 이 포커스를 바꾸면 부른다. */
    fun onFocusChanged(groupId: String?) {
        _focusGroupId.value = groupId
        if (_follow.value && groupId != null) s.markRead(groupId)
    }

    fun toggleFollow() {
        _follow.value = !_follow.value
        if (!_follow.value) _pinned.value = groupId.value
    }

    fun send(text: String) {
        val g = groupId.value ?: return
        if (text.isBlank()) return
        scope.launch { s.sendGroupSds(g, text.trim()) }
    }
}
