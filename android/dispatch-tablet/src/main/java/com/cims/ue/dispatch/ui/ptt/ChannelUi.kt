// 채널 목록의 **표시용 모델** (android_dispatch_tablet.md §6.3)
//
// 화면이 `ChannelCard`·`ScopedCard`(도메인 모델, 안에 SDK `GroupInfo`·`SessionItem` 을 품는다)를 직접 받으면
// **세션 없이는 그려 볼 수 없다.** 그러면 배치·밀도를 확인하는 유일한 길이 «APK 를 만들어 기기에 깔기» 가 되고,
// UI 를 만지는 일이 한 번에 몇 분씩 걸린다.
//
// 그래서 화면은 여기 정의한 값만 받는다. 얻는 것 셋:
//   1. **Compose Preview** 가 선다 — IDE 안에서 즉시 렌더(기기·에뮬레이터·네이티브 .so 불필요)
//   2. 변환이 **순수 함수**라 단위시험 대상이 된다(무엇을 어떻게 보여 주는지를 시험으로 고정)
//   3. 화면이 SDK 타입에서 풀린다 — 코어 모델이 바뀌어도 변환 한 곳만 고친다
package com.cims.ue.dispatch.ui.ptt

/**
 * 채널 목록 한 줄이 그리는 데 필요한 전부.
 *
 * 도메인 모델에서 «이미 판정된» 값만 담는다 — 화면은 계산하지 않고 그리기만 한다.
 */
data class ChannelRowUi(
    val id: String,
    /** 1줄 왼쪽 — 채널 이름. */
    val title: String,
    /** 2줄 — 발언자·사유 등 «지금 무슨 일이 있는가». 비면 2줄을 그리지 않는다. */
    val subtitle: String = "",
    /** 1줄 오른쪽 — 진행 중이면 경과, 아니면 상태. */
    val state: String = "",
    val participants: Int = 0,
    val unread: Int = 0,
    /** 상태 점 — 세션이 있는가(참여하지 않아도 로스터로 안다). */
    val active: Boolean = false,
    val speaking: Boolean = false,
    val emergency: Boolean = false,
    /** 일제 통화 세션(TS 24.379 §4.12) — 1줄 «일제» 태그. */
    val broadcast: Boolean = false,
    /** 발언 대상이 될 수 있는가(참여 중 + 반이중). 범위 채널은 늘 false. */
    val canTarget: Boolean = false,
    val targeted: Boolean = false,
    /** 청취 토글 — 범위 채널만. null = 내 채널(토글 없음). */
    val listening: Boolean? = null,
    /**
     * 음소거 토글 — 참여 중인 전이중 개별 통화만. null = 토글 없음(반이중은 floor 가 마이크를 연다).
     * [canTarget] 과 한 행에 같이 서지 않는다 — 전이중은 발언 대상이 될 수 없어 그 자리를 음소거가 쓴다.
     */
    val muted: Boolean? = null,
    /** 임박 위험 — 긴급이 아닐 때만 선다(서열 긴급 › 임박, 한 행에 둘을 같이 그리지 않는다). */
    val imminentPeril: Boolean = false,
)

/** 내 채널 카드 → 행. */
internal fun ChannelCard.toRowUi(targeted: Boolean): ChannelRowUi = ChannelRowUi(
    id = id,
    title = title,
    subtitle = line2,
    state = stateText,
    participants = participants,
    unread = unread,
    active = hasSession,
    speaking = speaking,
    emergency = emergency,
    broadcast = isBroadcast,
    canTarget = canCheck,
    targeted = targeted,
    listening = null,
    muted = muted.takeIf { canMute },
    imminentPeril = imminentPeril && !emergency)

/** 범위 채널 카드 → 행. 발언 대상이 될 수 없다 — 청취는 관측이지 참여가 아니다(§5.6). */
internal fun ScopedCard.toRowUi(): ChannelRowUi = ChannelRowUi(
    id = id,
    title = title,
    subtitle = line2,
    state = stateText,
    participants = participants,
    unread = 0,
    active = hasSession,
    speaking = speaker.isNotEmpty(),
    emergency = emergency,
    canTarget = false,
    targeted = false,
    listening = listening,
    muted = null,
    imminentPeril = imminentPeril && !emergency)
