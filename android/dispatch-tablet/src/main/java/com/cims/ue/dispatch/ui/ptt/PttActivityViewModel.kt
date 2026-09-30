// ⑤ PTT 이벤트 (docs/design/features/android_dispatch_tablet.md §6.7, dispatch_desktop_ui.md §4.4)
//
// **진행 중 행은 없다** — 전이(발언 시작·종료, 입퇴장, 긴급, 오류)만 링 버퍼에 쌓인다.
// 진행 중 상태는 ①② 카드가 보여주므로 여기서 중복하지 않는다. 예외 하나 — **진행 중인 긴급·임박은 목록 위에 고정**한다
// (데스크톱 `PttActivityViewModel.Pinned`). 개시 행이 흘러 내려가거나 필터·따라가기에 가려도 아직 풀리지 않은 긴급이 이
// 면에서 사라지지 않게.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.AlertBannerUi
import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.ui.toAlertBannerUi
import com.cims.ue.dispatch.session.ActivityKind
import com.cims.ue.dispatch.session.ActivityRow
import com.cims.ue.dispatch.session.DispatchSession
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn

enum class ActivityFilter(val label: String) { ALL("전체"), TALK("발언"), EMERGENCY("긴급") }

class PttActivityViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _filter = MutableStateFlow(ActivityFilter.ALL)
    val filter: StateFlow<ActivityFilter> = _filter.asStateFlow()

    // 저장된 값에서 시작한다(데스크톱 `FollowChannelEvents`, 기본 켬) — 켜 두었던 것이 다시 켤 때마다 풀리지 않게.
    private val _followFocus = MutableStateFlow(s.settingsSnapshot().followChannelEvents)
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

    /**
     * 고정 행 — 진행 중인 긴급·임박. 전역 배너와 **같은 스택**(`alerts` — 채널마다 하나, 최신 위, 경과는 조건이 선 때부터)이라
     * 배너와 다른 수를 말하지 않는다. 필터·따라가기와 무관하게 늘 선다 — 가리면 고정하는 뜻이 없다.
     */
    val pinned: StateFlow<List<AlertBannerUi>> =
        s.alerts.map { list -> list.mapNotNull { it.toAlertBannerUi(s::displayLabel) } }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    /**
     * 지금 열 수 있는 채널 id — 편성 그룹(멤버·범위)과 살아 있는 세션. 행 탭은 여기 든 것만 연다: 1:1 SDS 행의 키는 사람
     * 번호라 채널이 아니고, 끝난 개별 통화·애드혹은 채널이 사라졌다(데스크톱 `GroupOf` 가 그룹을 못 찾으면 아무것도 안 한다).
     */
    val channelIds: StateFlow<Set<String>> =
        combine(s.groups, s.sessions) { g, se -> g.map { it.id }.toSet() + se.filter { it.isLive }.map { it.channelId } }
            .stateIn(scope, SharingStarted.Eagerly, emptySet())

    /** ⑤ CSV — 필터·따라가기와 무관하게 세션이 든 이벤트 전부(데스크톱 `ExportCsv(ActivityPanel.Ptt)` 와 같다). */
    fun csv(): String = activityCsv(allRows.value)

    /** 세션이 든 이벤트 전부(최신 위) — «이벤트» 면의 종류·채널 거르기와 상세 패널이 이것을 쓴다. */
    val allRows: StateFlow<List<ActivityRow>> = s.activity

    fun setFilter(f: ActivityFilter) { _filter.value = f }
    fun toggleFollowFocus() {
        val v = !_followFocus.value
        _followFocus.value = v
        s.updateSettings { it.copy(followChannelEvents = v) }
    }
    fun onFocusChanged(groupId: String?) { _focusGroupId.value = groupId }
}

/**
 * ⑤ 이벤트 CSV — 시각·채널·종류·내용·긴급, 시간순. 열은 데스크톱 `ActivityLog.ExportCsv` 의 PTT 쪽과 같은 뜻이다
 * (time·title·kind·detail·emergency — 태블릿 행은 제목이 채널 이름, 내용이 한 줄이다). BOM 은 쓰는 쪽이 붙인다.
 */
internal fun activityCsv(rows: List<ActivityRow>, zone: java.util.TimeZone = java.util.TimeZone.getDefault()): String {
    val t = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).apply { timeZone = zone }
    fun q(s: String) = "\"" + s.replace("\"", "\"\"") + "\""
    val sb = StringBuilder("time,channel,kind,detail,emergency\r\n")
    rows.sortedBy { it.atMs }.forEach { r ->
        sb.append(t.format(java.util.Date(r.atMs))).append(',').append(q(r.groupName)).append(',')
            .append(q(activityKindText(r.kind))).append(',').append(q(r.text)).append(',')
            .append(if (r.emergency) 1 else 0).append("\r\n")
    }
    return sb.toString()
}

/** 이벤트 종류 낱말 — CSV 가 쓴다. */
internal fun activityKindText(k: ActivityKind): String = when (k) {
    ActivityKind.TALK -> "발언"
    ActivityKind.JOIN -> "입장"
    ActivityKind.LEAVE -> "퇴장"
    ActivityKind.EMERGENCY -> "긴급"
    ActivityKind.SDS -> "SDS"
    ActivityKind.ERROR -> "오류"
}
