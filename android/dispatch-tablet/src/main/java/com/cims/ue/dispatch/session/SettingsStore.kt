// 앱 로컬 설정 (docs/design/features/android_dispatch_tablet.md §6.9)
//
// 데스크톱의 `SettingsStore`(json) 대응. 자격은 여기 두지 않는다 — `SecureStore`(Keystore) 몫이다.
package com.cims.ue.dispatch.session

import android.content.Context
import com.cims.ue.sdk.platform.Route

/** 저장되는 설정 한 벌. 기본값은 "처음 켠 관제석" 기준. */
data class Settings(
    val cscHost: String = "",
    val cscPort: Int = 4430,
    val loginId: String = "",
    val autoLogin: Boolean = true,
    /** 추가 신뢰 앵커(PEM). 비면 SDK 동봉 앵커([TrustAnchors.CA_BUNDLE])만 쓴다 — 보통은 비어 있다. */
    val extraCaPem: String = "",
    val verifyServer: Boolean = true,
    val logLevel: Int = 3,
    val audioRoute: Route = Route.SPEAKER,
    /** 발언 중 손을 떼도 유지(잠금 발언). */
    val lockTalk: Boolean = false,
    /**
     * 활성 통화 중 새 착신에 응답하면 기존 통화를 **자동 보류**한다.
     *
     * 데스크톱과 같은 기본값(`Services/SettingsStore.cs` `AutoHoldOnAnswer = true`). 끄면 두 통화가
     * 동시에 들려 관제사가 어느 쪽에 말하는지 알 수 없다.
     */
    val autoHoldOnAnswer: Boolean = true,
    /** 당겨받기 피처코드 — 접속서비스의 `pickup_feature_code` 와 같아야 한다(기본 `**`). */
    val pickupFeatureCode: String = "**",
)

/** SharedPreferences 한 겹. DataStore 는 의존을 늘려 쓰지 않는다. */
class SettingsStore(context: Context) {

    private val prefs = context.applicationContext.getSharedPreferences("cims-dispatch", Context.MODE_PRIVATE)

    /**
     * **관측 가능해야 한다** — 설정 화면이 바꾼 값을 그 화면이 다시 그려야 하고, 잠금 발언·자동 보류처럼
     * 다른 화면이 읽는 값도 즉시 따라야 한다. `@Volatile` 필드만으로는 Compose 가 재구성을 걸지 못한다.
     */
    private val _flow = kotlinx.coroutines.flow.MutableStateFlow(load())
    val flow: kotlinx.coroutines.flow.StateFlow<Settings> = _flow

    val current: Settings get() = _flow.value

    fun update(f: (Settings) -> Settings) {
        val next = f(current)
        _flow.value = next
        prefs.edit()
            .putString(K_HOST, next.cscHost)
            .putInt(K_PORT, next.cscPort)
            .putString(K_LOGIN, next.loginId)
            .putBoolean(K_AUTO, next.autoLogin)
            .putString(K_CA, next.extraCaPem)
            .putBoolean(K_VERIFY, next.verifyServer)
            .putInt(K_LOG, next.logLevel)
            .putString(K_ROUTE, next.audioRoute.name)
            .putBoolean(K_LOCK, next.lockTalk)
            .putString(K_PICKUP, next.pickupFeatureCode)
            .apply()
    }

    private fun load(): Settings = Settings(
        cscHost = prefs.getString(K_HOST, "") ?: "",
        cscPort = prefs.getInt(K_PORT, 4430),
        loginId = prefs.getString(K_LOGIN, "") ?: "",
        autoLogin = prefs.getBoolean(K_AUTO, true),
        extraCaPem = prefs.getString(K_CA, "") ?: "",
        verifyServer = prefs.getBoolean(K_VERIFY, true),
        logLevel = prefs.getInt(K_LOG, 3),
        audioRoute = runCatching { Route.valueOf(prefs.getString(K_ROUTE, null) ?: "") }
            .getOrDefault(Route.SPEAKER),
        lockTalk = prefs.getBoolean(K_LOCK, false),
        pickupFeatureCode = prefs.getString(K_PICKUP, "**") ?: "**")

    private companion object {
        const val K_HOST = "csc_host"; const val K_PORT = "csc_port"; const val K_LOGIN = "login_id"
        const val K_AUTO = "auto_login"; const val K_CA = "tls_ca"; const val K_VERIFY = "verify"
        const val K_PICKUP = "pickup_code"
        const val K_LOG = "log_level"; const val K_ROUTE = "audio_route"; const val K_LOCK = "lock_talk"
        const val K_AUTOHOLD = "auto_hold_on_answer"
    }
}
