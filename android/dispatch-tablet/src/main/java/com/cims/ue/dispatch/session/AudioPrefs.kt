// 이어폰 고르기·선호 이어폰 자동 복귀 (android_dispatch_tablet.md §8, dispatch_desktop_ui.md §7)
//
// 데스크톱은 장치를 역할별로 이름으로 고른다(`HeadsetDevice`·`SpeakerDevice`, 다시 붙으면 복귀). Android 통신 경로는 장치
// 하나를 고르면 **출력과 마이크가 함께** 그 장치로 간다 — 그래서 고르는 것은 «어느 이어폰» 하나다(마이크는 따로 없다).
// 판정은 순수 함수라 기기 없이 시험한다.
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.platform.Headset
import com.cims.ue.sdk.platform.Route

/**
 * 경로 의도가 헤드셋(유선)·블루투스(무선)면 그 종류 중 **선호 이어폰**(이름)을, 없으면 그 종류의 첫 것을. 다른 경로(스피커·수화부)
 * 거나 그 종류가 없으면 null — 경로 요청만 한다.
 */
internal fun pickHeadset(route: Route, headsets: List<Headset>, preferred: String): Headset? {
    val wireless = when (route) {
        Route.HEADSET -> false
        Route.BLUETOOTH -> true
        else -> return null
    }
    val same = headsets.filter { it.wireless == wireless }
    return same.firstOrNull { preferred.isNotBlank() && it.name == preferred } ?: same.firstOrNull()
}

/**
 * 선호 이어폰으로 되돌릴 때인가 — 자동 복귀가 켜져 있고, **선호 이어폰이 방금 붙었고**, 경로 의도가 이어폰이다. 스피커를
 * 고른 뒤라면 되돌리지 않는다(사람의 선택이 이긴다).
 */
internal fun returnToPreferred(s: Settings, appeared: Set<String>): Boolean =
    s.autoReturnHeadset && s.preferredHeadset.isNotBlank() && s.preferredHeadset in appeared &&
        (s.audioRoute == Route.HEADSET || s.audioRoute == Route.BLUETOOTH)
