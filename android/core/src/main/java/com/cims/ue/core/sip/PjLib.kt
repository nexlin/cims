package com.cims.ue.core.sip

import android.util.Log
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

    @Synchronized
    fun boot(logLevel: Int = 4) {
        if (booted) return

        // libpjsua2.so 로드. SWIG static initializer 가 이미 로드했으면 중복(무해).
        // c++_shared(libc++_shared.so) 동봉이 선행 조건(설계서 위험 #2).
        System.loadLibrary("pjsua2")

        val endpoint = Endpoint()
        endpoint.libCreate()

        val epc = EpConfig().apply {
            uaConfig.userAgent = "CIMS-UE/M1 (pjsua2)"
            logConfig.level = logLevel.toLong()
        }
        endpoint.libInit(epc)

        // UDP transport (M1 은 UDP only — SRTP/TLS off, 설계서 §2.5). port=0 → 임의 포트 바인드.
        endpoint.transportCreate(
            pjsip_transport_type_e.PJSIP_TRANSPORT_UDP,
            TransportConfig().apply { port = 0 },
        )

        endpoint.libStart()
        ep = endpoint
        booted = true
        Log.i(TAG, "PJSIP started — state=${endpoint.libGetState()}, SIP UDP transport up")

        logRegisteredCodecs()
    }

    /** 현재 스레드를 PJSIP 워커로 등록(미등록 스레드의 ep.* 호출은 native abort). 멱등. */
    fun ensureThread(name: String = Thread.currentThread().name) {
        if (!ep.libIsThreadRegistered()) ep.libRegisterThread(name)
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
