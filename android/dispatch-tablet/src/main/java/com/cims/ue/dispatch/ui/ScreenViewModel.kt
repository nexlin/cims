// 화면·패널 VM 의 바탕 (docs/design/features/android_dispatch_tablet.md §6.1)
//
// **androidx `ViewModel` 을 쓰지 않는다.** 이들의 소유자는 Activity 의 `ViewModelStore` 가 아니라
// [com.cims.ue.dispatch.ui.MainViewModel] 이다. `ViewModel` 을 상속해 놓고 생성자로 만들어 필드에 들면
// `onCleared()` 가 **영영 불리지 않는다** — 정리(플레이어 해제·발언 해제)가 죽고, `viewModelScope` 도
// 취소되지 않아 소유자가 버린 VM 의 코루틴(`stateIn(Eagerly)`·`collect`)이 계속 돈다. 로그아웃→재로그인을
// 되풀이하면 세션 Flow 구독이 쌓인다.
//
// 그래서 수명을 **명시적으로** 든다 — 소유자가 [close] 를 부르고, 그때 스코프가 취소된다.
// 소유자(MainViewModel)만이 진짜 `ViewModel` 이고, 그것의 `onCleared` 가 이 사슬의 뿌리다.
package com.cims.ue.dispatch.ui

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel

abstract class ScreenViewModel : AutoCloseable {

    /**
     * 이 VM 의 수명 스코프. `viewModelScope` 를 대신한다.
     *
     * `Main.immediate` 인 것은 화면 상태를 바꾸는 일이 대부분이고, 이벤트 처리 중 상태 갱신이
     * 한 프레임 밀리지 않게 하기 위해서다(기존 `viewModelScope` 와 같은 디스패처).
     */
    protected val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** 소유자가 버릴 때 부른다. 하위 클래스는 정리한 뒤 `super.close()` 를 부른다. 멱등이다. */
    override fun close() {
        scope.cancel()
    }
}
