package com.cims.ue.core.sip

import android.hardware.camera2.CameraManager
import android.util.Log
import org.pjsip.PjCameraInfo2
import org.pjsip.pjsua2.Endpoint
import org.pjsip.pjsua2.EpConfig
import org.pjsip.pjsua2.TransportConfig
import org.pjsip.pjsua2.pjsip_transport_type_e

/**
 * PJSIP(pjsua2) Endpoint 단일 부팅·수명 관리 (설계서 §3.5, M1.0).
 *
 * 한 프로세스에 Endpoint 는 하나뿐이다. [boot] 는 멱등(이미 부팅이면 no-op)이며
 * **반드시 pj-ctl 전용 스레드에서** 호출된다([SipController] 가 보장). 부팅 직후 그 스레드를
 * [ensureThread] 로 1회 등록한다.
 *
 * ⚠️ 네이티브 시그니처는 core 투입된 실제 SWIG `Endpoint.java` 기준 확정값이다(verify-on-machine 완료):
 *  - enum 은 Java enum 이 아니라 `public final static int` 상수 → `transportCreate(int, …)` 에 상수 직접 전달.
 *  - `libIsThreadRegistered():boolean`, `libRegisterThread(String)`, `libGetState():int`.
 */
object PjLib {

    private const val TAG = "PjLib"

    @Volatile
    var booted = false
        private set

    lateinit var ep: Endpoint
        private set

    /**
     * Android 카메라 매니저 — [boot] 전(영상 캡처 디바이스 열거 전)에 [SipService] 가 주입한다.
     * PJSIP Android 영상 캡처 디바이스(PjCamera2)는 `PjCameraInfo2.SetCameraManager` 로 CameraManager
     * 를 받아야만 카메라를 열거한다. 미주입 시 카메라 0개(Colorbar 합성 디바이스만) → 발신 영상/셀프뷰 불가.
     */
    @Volatile
    var cameraManager: CameraManager? = null

    // 스레드별 PJSIP 등록 여부. libIsThreadRegistered() 는 미등록 스레드에서 호출 시 native abort 하므로
    // 쓰지 않고, 이 ThreadLocal 로 스레드당 정확히 1회 libRegisterThread 를 보장한다(설계서 §3.5 대안).
    private val threadRegistered = ThreadLocal.withInitial { false }

    @Synchronized
    fun boot(logLevel: Int = 4) {
        if (booted) return

        // libpjsua2.so 로드. SWIG static initializer 가 이미 로드했으면 중복(무해).
        // c++_shared(libc++_shared.so) 동봉이 선행 조건(설계서 위험 #2).
        System.loadLibrary("pjsua2")

        // 영상 캡처 디바이스 열거는 libInit(영상 서브시스템 init) 시점에 일어나므로 그 전에 주입해야 한다.
        cameraManager?.let {
            PjCameraInfo2.SetCameraManager(it)
            Log.i(TAG, "CameraManager injected for video capture enumeration")
        } ?: Log.w(TAG, "CameraManager not set — 카메라 캡처 디바이스 열거 불가(발신 영상/셀프뷰 안 됨)")

        val endpoint = Endpoint()
        endpoint.libCreate()

        val epc = EpConfig().apply {
            uaConfig.userAgent = "CIMS-UE/M1 (pjsua2)"
            logConfig.level = logLevel.toLong()
        }
        endpoint.libInit(epc)

        // UDP + TCP transport (port=0 → 임의 포트 바인드). 계정의 transport 설정에 따라 선택 사용.
        // TCP 는 UDP 가 NAT 헤어핀/방화벽에 막힐 때 등록 경로 확보용(공인 IP 뒤 단말).
        endpoint.transportCreate(
            pjsip_transport_type_e.PJSIP_TRANSPORT_UDP,
            TransportConfig().apply { port = 0 },
        )
        runCatching {
            endpoint.transportCreate(
                pjsip_transport_type_e.PJSIP_TRANSPORT_TCP,
                TransportConfig().apply { port = 0 },
            )
        }.onFailure { Log.w(TAG, "TCP transport create failed (UDP-only fallback)", it) }

        endpoint.libStart()
        ep = endpoint
        booted = true
        threadRegistered.set(true)            // libInit 가 boot 스레드를 등록함
        Log.i(TAG, "PJSIP started — state=${endpoint.libGetState()}, SIP UDP+TCP transport up")

        logRegisteredCodecs()
    }

    /**
     * 현재 스레드를 PJSIP 워커로 등록(미등록 스레드의 ep.* 호출은 native abort). 스레드당 1회.
     * ⚠️ `libIsThreadRegistered()` 는 미등록 스레드에서 호출 자체가 abort 하므로 쓰지 않고 ThreadLocal 로 가드.
     */
    fun ensureThread(name: String = Thread.currentThread().name) {
        if (!booted || threadRegistered.get()) return
        runCatching { ep.libRegisterThread(name) }.onSuccess { threadRegistered.set(true) }
    }

    /**
     * M1.0 경로 C 게이트: codecEnum2() 에 AMR-WB 가 **정확히 1개**(opencore 중복 없음)인지 확인용 로그.
     * 실패가 아니라 진단 — logcat 에서 확인한다.
     */
    private fun logRegisteredCodecs() {
        runCatching {
            val codecs = ep.codecEnum2()
            val ids = (0 until codecs.size).map { codecs[it].codecId }
            val amrwb = ids.count { it.contains("AMR-WB", ignoreCase = true) }
            Log.i(TAG, "audio codecs=$ids  (AMR-WB count=$amrwb ${if (amrwb == 1) "OK" else "⚠ expect 1"})")
        }.onFailure { Log.w(TAG, "codecEnum2 failed: ${it.message}") }
    }

    @Synchronized
    fun shutdown() {
        if (!booted) return
        runCatching { ep.libDestroy() }.onFailure { Log.w(TAG, "libDestroy: ${it.message}") }
        booted = false
    }
}
