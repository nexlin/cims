// Android 접점 — 망 변경 감지 (docs/design/features/ue_sdk.md §5.3 P0a)
//
// **이 층은 프로토콜을 모른다.** 기본 네트워크가 바뀌면(Wi-Fi ↔ 이동망, 끊겼다 다시 붙음) 앱이 준 동작을 부르기만 한다 —
// 보통 등록한 계정마다 `Account.refreshRegistration()`(즉시 재-REGISTER). 기존 VoLTE 앱(`SipService.registerNetworkCallback`)과
// 같은 규칙이다. 재등록 없이 두면 NAT 바인딩·전송 연결이 옛 망에 묶인 채 등록 만료까지 착신이 끊긴다.
package com.cims.ue.sdk.platform

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network

/**
 * 기본 네트워크 변화 감시. [start] 뒤 새 기본 네트워크가 잡힐 때마다 [onChanged] 를 부른다(ConnectivityManager 콜백 스레드 —
 * 블록하지 말고 코루틴으로 넘긴다). 콜백을 등록하면 **지금 망**에 대한 통지가 곧바로 한 번 오는데, 그것은 변화가 아니므로 넘긴다.
 * 걸 때 망이 없었으면(부팅 직후·음영) 처음 잡히는 망이 변화다 — 그동안의 등록은 실패했으니 갱신 주기를 기다리지 않는다.
 */
class NetworkWatcher(context: Context, private val onChanged: (Network) -> Unit) : AutoCloseable {

    private val cm = context.applicationContext.getSystemService(ConnectivityManager::class.java)
    private val filter = NetworkChangeFilter<Network>()
    private var cb: ConnectivityManager.NetworkCallback? = null

    @Synchronized
    fun start() {
        if (cb != null || cm == null) return
        filter.seed(runCatching { cm.activeNetwork }.getOrNull())
        val c = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { if (filter.onAvailable(network)) onChanged(network) }
            override fun onLost(network: Network) { filter.onLost(network) }
        }
        runCatching { cm.registerDefaultNetworkCallback(c); cb = c }
    }

    @Synchronized
    override fun close() {
        cb?.let { c -> runCatching { cm?.unregisterNetworkCallback(c) } }
        cb = null
        filter.reset()
    }
}

/**
 * 판정만 — 첫 통지(등록 직후의 지금 망)는 변화가 아니고, 그 뒤 **다른** 망이 잡히거나 끊겼다 다시 잡히면 변화다.
 * [seed] 로 걸 때의 망을 심으면 «망 없이 걸었다» 를 안다 — 그때는 처음 잡히는 망이 변화다.
 * Android 없이 시험하려고 떼어 둔다(PlatformTest).
 */
class NetworkChangeFilter<N : Any> {
    private var current: N? = null
    private var seen = false
    private var lost = false

    /** 콜백을 걸기 직전의 기본 망. null = 망 없이 건다 — 처음 잡히는 망이 변화다. */
    @Synchronized
    fun seed(initial: N?) { current = initial; seen = true; lost = initial == null }

    /** 새 기본 네트워크 — 변화면 true. */
    @Synchronized
    fun onAvailable(n: N): Boolean {
        val changed = seen && (lost || n != current)
        seen = true; lost = false; current = n
        return changed
    }

    @Synchronized
    fun onLost(n: N) { if (n == current) lost = true }

    @Synchronized
    fun reset() { current = null; seen = false; lost = false }
}
