// ⑤ PTT 이벤트 (docs/design/features/android_dispatch_tablet.md §6.7, dispatch_desktop_ui.md §4.4)
//
// **진행 중 행은 없다** — 전이(발언 시작·종료, 입퇴장, 긴급, 오류)만 링 버퍼에 쌓인다.
// 진행 중 상태는 ①② 카드가 보여주므로 여기서 중복하지 않는다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.DispatchSession
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn

enum class ActivityFilter(val label: String) { ALL("전체"), TALK("발언"), EMERGENCY("긴급") }

class PttActivityViewModel(s: DispatchSession) : ScreenViewModel() {

    private val _filter = MutableStateFlow(ActivityFilter.ALL)
    val filter: StateFlow<ActivityFilter> = _filter.asStateFlow()

    private val _followFocus = MutableStateFlow(false)
    /** 포커스 채널만 보기. 끄면 전 채널. */
    val followFocus: StateFlow<Boolean> = _followFocus.asStateFlow()

    private val _focusGroupId = MutableStateFlow<String?>(null)

    val rows: StateFlow<List<ActivityRow>> =
        combine(s.activity, _filter, _followFocus, _focusGroupId) { all, filter, follow, focus ->
            all.filter { r ->
                (!follow || focus == null || r.groupId == focus) &&
                    when (filter) {
                        ActivityFilter.ALL -> true
                        ActivityFilter.TALK -> r.kind == ActivityKind.TALK
                        ActivityFilter.EMERGENCY -> r.emergency
                    }
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    fun setFilter(f: ActivityFilter) { _filter.value = f }
    fun toggleFollowFocus() { _followFocus.value = !_followFocus.value }
    fun onFocusChanged(groupId: String?) { _focusGroupId.value = groupId }
}
