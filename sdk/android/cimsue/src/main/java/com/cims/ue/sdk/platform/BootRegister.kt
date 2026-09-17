// Android 접점 — 부팅 재등록 (docs/design/features/android_dispatch_tablet.md §5)
//
// **이 층은 프로토콜을 모른다.** 부팅 방송을 받아 앱의 등록 유지 서비스를 띄우기만 하고,
// 재로그인·등록 절차는 그 서비스(앱)가 한다. Windows 접점의 AutoStart 자리와 같다.
package com.cims.ue.sdk.platform

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * 기기 부팅 시 등록 유지 서비스를 시작한다.
 *
 * 앱이 서브클래싱해 [serviceIntent] 를 채우고 매니페스트에 `BOOT_COMPLETED` 리시버로 등록한다
 * (`RECEIVE_BOOT_COMPLETED` 권한도 앱이 선언한다).
 *
 * **저장된 자격이 없으면 아무것도 하지 않는다** — 최초 1회 수동 로그인 전에는 띄울 이유가 없다.
 * 판단 기준은 [SecureStore] 에 자격이 있는지뿐이며, 그 값의 의미는 여기서 해석하지 않는다.
 */
abstract class BootRegister : BroadcastReceiver() {

    /** 부팅 시 시작할 서비스. */
    abstract fun serviceIntent(context: Context): Intent

    /** 자격 저장소의 이름공간 — 앱이 여러 벌을 쓰면 덮어쓴다. */
    protected open val secureStoreNamespace: String? = null

    /** 자격 존재 판정을 바꾸고 싶으면 덮어쓴다. */
    protected open fun hasCredentials(context: Context): Boolean {
        val store = secureStoreNamespace?.let { SecureStore(context, it) } ?: SecureStore(context)
        return store.contains(SecureStore.KEY_REFRESH_TOKEN)
    }

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED && action != QUICKBOOT) return
        if (!hasCredentials(context)) return
        UeForegroundService.start(context, serviceIntent(context).apply { putExtra(EXTRA_AUTOSTART, true) })
    }

    companion object {
        private const val QUICKBOOT = "android.intent.action.QUICKBOOT_POWERON"
        /** 서비스가 "부팅으로 시작됐다" 를 구분할 때 쓴다. */
        const val EXTRA_AUTOSTART = "cimsue.autostart"
    }
}
