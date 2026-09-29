// libcimsue Kotlin 파사드 — 시험 모드 계측 링크 (docs/design/features/ue_voice_quality.md §4·§5)
//
// 코어 `DeviceLink`(cimsue/drive.h)가 계측기 워커(Device.Port)에 TLS 로 먼저 연결해 hello 를 보내고, 수락되면 워커의 drive
// 명령을 이 단말의 엔진으로 실행한다 — 재접속 백오프·ping·지문 고정(TOFU)·구동 호 정리까지 코어가 한다. 파사드는
// **상태 하나와 시작/정지**만 옮긴다. 앱은 DriveSession·줄 프로토콜을 보지 않는다(Windows .NET `DeviceLink` 와 같은 묶음).
//
// 수명: 링크는 엔진을 참조하므로 엔진보다 먼저 멈춘다. [CimsUe.close] 가 열린 링크를 먼저 해제한다 — 앱이 잊어도 해제
// 순서가 지켜진다. 앱은 세션과 같은 수명의 Foreground Service 가 링크를 들고(§4.3), 등록을 끝낸 뒤 [start] 한다 — 등록은
// 앱 소유다(워커의 register/unregister 는 코어가 `app_owned` 로 거절한다).
package com.cims.ue.sdk

import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import com.cims.ue.sdk.jni.DeviceLink as JniDeviceLink
import com.cims.ue.sdk.jni.DeviceLinkListener as JniDeviceLinkListener
import com.cims.ue.sdk.jni.LinkState as JniLinkState

/**
 * 계측기 워커 링크 핸들 — [CimsUe.deviceLink] 로 만든다. 한 엔진에 여러 개를 둘 수 있지만 시험 모드는 하나만 쓴다
 * (같은 `deviceId` 로 두 번 붙으면 워커가 옛 연결을 닫는다).
 */
class DeviceLink internal constructor(private val ue: CimsUe) : AutoCloseable {

    private val link = JniDeviceLink(ue.jni)
    // director 프록시는 Kotlin 강참조로 살린다 — CimsUe 의 Listener 와 같은 규약(소유권을 바꾸지 않는다).
    private val listener = StateListener()
    private val released = AtomicBoolean(false)
    private val lock = Any()

    private val _status = MutableStateFlow(LinkStatus(LinkState.IDLE))
    /** 링크 상태 — 코어 링크 스레드가 바꾼다. 배지("계측기 연결됨 · <워커>")·끊김 사유 표시의 원천. */
    val status: StateFlow<LinkStatus> = _status.asStateFlow()
    val state: LinkState get() = _status.value.state

    /**
     * 링크 시작 — 스레드만 띄우고 곧 돌아온다. [accounts] 는 앱이 가진 회선(첫 항목 = 기본 회선).
     * 이미 돌고 있으면 실패한다. REFUSED(연결 키·지문 불일치) 뒤에 다시 붙으려면 [stop] 뒤 [start] 한다.
     */
    suspend fun start(cfg: DeviceLinkConfig, accounts: List<DriveAccount>): CimsResult<Unit> = ue.command {
        synchronized(lock) {
            if (released.get()) CimsResult.fail(-99, "link closed")
            else CimsResult.of(link.start(cfg.toJni(), cfg.driveOptions(accounts), listener))
        }
    }

    /** 링크를 닫는다 — 이 링크가 구동한 호만 끊고 링크 스레드가 끝날 때까지 기다린다(연결 시도 중이면 연결 시한까지). */
    suspend fun stop() {
        ue.command {
            synchronized(lock) { if (!released.get()) link.stop() }
            CimsResult.ok(Unit)
        }
        _status.value = LinkStatus(LinkState.IDLE)        // 코어 stop 은 Idle 로 돌아가며 콜백을 내지 않는다
    }

    /** 네이티브 해제. 멱등이다. 링크 스레드를 기다리므로 메인 스레드에서는 [stop] 을 먼저 끝낸 뒤 부른다. */
    override fun close() {
        ue.forgetLink(this)
        release()
    }

    /** [CimsUe.close] 도 부른다 — 엔진이 살아 있는 동안 멈추고 해제한다. */
    internal fun release() {
        synchronized(lock) {
            if (!released.compareAndSet(false, true)) return
            runCatching { link.stop() }
            runCatching { link.delete() }
            runCatching { listener.delete() }
        }
        _status.value = LinkStatus(LinkState.IDLE)
    }

    /** 코어 링크 스레드에서 오는 director 콜백 — 기록만 한다. 여기서 블록하면 링크가 멈춘다. */
    private inner class StateListener : JniDeviceLinkListener() {
        override fun onLinkState(state: JniLinkState, detail: String) {
            _status.value = LinkStatus.of(state, detail)
        }
    }
}
