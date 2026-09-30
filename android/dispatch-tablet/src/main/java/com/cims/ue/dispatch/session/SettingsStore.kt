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
    /**
     * **동시 청취 상한** — 감청(통화 Join)과 PTT 청취를 합쳐 한 번에 몇 개까지 열 수 있나.
     *
     * 데스크톱과 같은 기본값 4(`Services/SettingsStore.cs` `MaxMonitorWindows`). 상한이 필요한 이유는
     * 화면이 아니라 **자원**이다 — 청취 leg 하나마다 서버에 CMP tap/멤버가 생기고, 서버에도 세션당 상한이
     * 있다(dispatch_center.md §5.5 `MaxTapsPerSession`). 앱이 무제한으로 열면 서버가 486 으로 거절하기
     * 시작하고, 관제사는 «왜 안 되는지» 를 알 수 없다. 앱에서 먼저 막고 이유를 말한다.
     */
    val maxListen: Int = 4,
    /**
     * 메시지 보관 일수(1~365, 기본 30 — 데스크톱 `MessageRetentionDays` 와 같은 값·범위). 기동 때 이보다 오래된 SDS·문자를
     * 지운다. 서버에 SDS·SMS 이력이 없으므로 **지운 것은 되살릴 수 없다**(§6.2e 메시지 보관).
     */
    val messageRetentionDays: Int = 30,
    /** ④ «메시지» 가 포커스 채널의 스레드를 따라가는가(데스크톱 `FollowChannelThread`, 기본 켬). */
    val followChannelThread: Boolean = true,
    /** ⑤ «이벤트» 가 포커스 채널만 보이는가(데스크톱 `FollowChannelEvents`, 기본 켬). */
    val followChannelEvents: Boolean = true,
    /**
     * 화면 테마 — `dark` | `light`(데스크톱 `Theme` 과 같은 값). 기본은 어둡게다 — 관제실·차량의 어두운 자리에서 쓰는 단말이라
     * 태블릿은 처음부터 어둡게 섰다(데스크톱 기본은 밝게).
     */
    val theme: String = THEME_DARK,
    /**
     * 선호 이어폰(장치 이름) — 경로가 헤드셋·블루투스일 때 여럿 중 이것을 고른다(데스크톱 `HeadsetDevice`, 이름 기준).
     * `AudioDeviceInfo.id` 는 재연결·재부팅 때 바뀌어 이름으로 든다.
     */
    val preferredHeadset: String = "",
    /** 선호 이어폰이 다시 연결되면 그리로 되돌린다(데스크톱 `AutoReturnToPreferredDevice`, 기본 켬). */
    val autoReturnHeadset: Boolean = true,
) {
    val dark: Boolean get() = theme != THEME_LIGHT

    companion object {
        const val THEME_DARK = "dark"
        const val THEME_LIGHT = "light"
    }
}

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
            .putBoolean(K_AUTOHOLD, next.autoHoldOnAnswer)
            .putInt(K_MAXLISTEN, next.maxListen)
            .putInt(K_RETAIN, next.messageRetentionDays)
            .putBoolean(K_FOLLOW_THREAD, next.followChannelThread)
            .putBoolean(K_FOLLOW_EVENTS, next.followChannelEvents)
            .putString(K_THEME, next.theme)
            .putString(K_HEADSET, next.preferredHeadset)
            .putBoolean(K_AUTORETURN, next.autoReturnHeadset)
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
        pickupFeatureCode = prefs.getString(K_PICKUP, "**") ?: "**",
        autoHoldOnAnswer = prefs.getBoolean(K_AUTOHOLD, true),
        maxListen = prefs.getInt(K_MAXLISTEN, 4).coerceIn(1, 16),
        messageRetentionDays = prefs.getInt(K_RETAIN, 30).coerceIn(1, 365),
        followChannelThread = prefs.getBoolean(K_FOLLOW_THREAD, true),
        followChannelEvents = prefs.getBoolean(K_FOLLOW_EVENTS, true),
        theme = prefs.getString(K_THEME, Settings.THEME_DARK)
            ?.takeIf { it == Settings.THEME_LIGHT || it == Settings.THEME_DARK } ?: Settings.THEME_DARK,
        preferredHeadset = prefs.getString(K_HEADSET, "") ?: "",
        autoReturnHeadset = prefs.getBoolean(K_AUTORETURN, true))

    private companion object {
        const val K_HOST = "csc_host"; const val K_PORT = "csc_port"; const val K_LOGIN = "login_id"
        const val K_AUTO = "auto_login"; const val K_CA = "tls_ca"; const val K_VERIFY = "verify"
        const val K_PICKUP = "pickup_code"
        const val K_LOG = "log_level"; const val K_ROUTE = "audio_route"; const val K_LOCK = "lock_talk"
        const val K_AUTOHOLD = "auto_hold_on_answer"; const val K_MAXLISTEN = "max_listen"
        const val K_RETAIN = "message_retention_days"
        const val K_FOLLOW_THREAD = "follow_channel_thread"; const val K_FOLLOW_EVENTS = "follow_channel_events"
        const val K_THEME = "theme"
        const val K_HEADSET = "preferred_headset"; const val K_AUTORETURN = "auto_return_headset"
    }
}
