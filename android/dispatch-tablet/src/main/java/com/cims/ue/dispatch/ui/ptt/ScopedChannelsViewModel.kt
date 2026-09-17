// ② 범위 채널 (docs/design/features/android_dispatch_tablet.md §6.3, dispatch_desktop_ui.md §4.2)
//
// 내가 멤버가 **아닌** 그룹 — 청취 범위(`pttTargets`)와 관리 범위. 참여하지 않고 로스터만 받아
// 진행 여부·참가자 수를 안다. **필터·검색은 이 패널에만 있다**(① 은 항상 전부 보인다).
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.dispatch.session.SessionKind
import com.cims.ue.dispatch.session.leave
import com.cims.ue.dispatch.session.listenGroup
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** ② 필터 — 데스크톱과 같은 축(전체·활성·긴급·청취 중). */
enum class ScopeFilter(val label: String) {
    ALL("전체"), ACTIVE("활성"), EMERGENCY("긴급"), LISTENING("청취 중")
}

/** ② 카드 — 청취 범위 그룹 하나. */
data class ScopedCard(
    val id: String,
    val title: String,
    val group: GroupInfo,
    /** 내가 청취 중인 세션(recvonly). 없으면 미청취. */
    val listenSession: SessionItem? = null,
) {
    val listening: Boolean get() = listenSession?.isLive == true
    val hasSession: Boolean get() = group.hasSession
    val participants: Int get() = group.connectedCount
    val emergency: Boolean get() = listenSession?.isEmergency == true
    val speaker: String get() = listenSession?.speaker.orEmpty()

    val stateText: String get() = when {
        listening -> fmtElapsed(listenSession!!.elapsedMs)
        hasSession -> "진행 중"
        else -> "대기"
    }

    val line2: String get() = when {
        listening && speaker.isNotEmpty() -> "발언 $speaker"
        listening -> "청취 중 · 발언 없음"
        hasSession -> "세션 진행 중 · 참가 $participants"
        group.sessionSinceMs != null -> "마지막 세션"
        else -> "대기"
    }
}

class ScopedChannelsViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _filter = MutableStateFlow(ScopeFilter.ALL)
    val filter: StateFlow<ScopeFilter> = _filter.asStateFlow()

    private val _query = MutableStateFlow("")
    val query: StateFlow<String> = _query.asStateFlow()

    /** 청취 범위 카드 — 긴급 › 진행 › 대기 순. 데스크톱과 같은 정렬이다. */
    val cards: StateFlow<List<ScopedCard>> =
        combine(s.groups, s.sessions, _filter, _query) { groups, sessions, filter, q ->
            groups.filter { !it.isMember }
                .map { g ->
                    ScopedCard(
                        id = g.id, title = g.name, group = g,
                        listenSession = sessions.firstOrNull {
                            it.kind == SessionKind.PTT_LISTEN && it.info.groupId == g.id
                        })
                }
                .filter { c ->
                    (q.isBlank() || c.title.contains(q, ignoreCase = true) || c.id.contains(q, ignoreCase = true)) &&
                        when (filter) {
                            ScopeFilter.ALL -> true
                            ScopeFilter.ACTIVE -> c.hasSession
                            ScopeFilter.EMERGENCY -> c.emergency
                            ScopeFilter.LISTENING -> c.listening
                        }
                }
                .sortedWith(compareByDescending<ScopedCard> { it.emergency }
                    .thenByDescending { it.hasSession }
                    .thenBy { it.title })
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    /** 동시 청취 수 — 화면 머리의 "청취 n". */
    val listeningCount: StateFlow<Int> =
        cards.let { f -> combine(f, f) { a, _ -> a.count { it.listening } } }
            .stateIn(scope, SharingStarted.Eagerly, 0)

    fun setFilter(f: ScopeFilter) { _filter.value = f }
    fun setQuery(q: String) { _query.value = q }

    /**
     * 청취 토글 — 켜면 `listenOnly` 합류(a=recvonly), 끄면 이탈.
     * 범위 밖·자격 없음은 서버가 403 + `Warning: 138` 로 거절한다 — 재시도하지 않는다.
     */
    fun toggleListen(card: ScopedCard) {
        scope.launch {
            val se = card.listenSession
            if (se != null && se.isLive) s.leave(se.callId) else s.listenGroup(card.id)
        }
    }
}
