package com.cims.ue.core.device

import android.content.Context
import android.os.Build
import android.provider.Settings
import java.util.UUID

/**
 * 단말 속성 — REGISTER 의 `User-Agent`(모델·OS·앱 버전)와 Contact `+sip.instance`(단말 ID)를 규격 경로로 싣는다
 * (mcptt_management_views.md §4.1 — 서버가 REGISTER 에서 읽는다. 앱 전용 보고 API 없음).
 */
object DeviceIdentity {

    /**
     * `User-Agent`(RFC 3261 §20.41) 규약 — `<제품>/<앱 버전> (Android <판>; <모델>)`.
     * 예: `CIMS-PTT/1.4.2 (Android 15; SM-S921N)`. 형식은 SDK `userAgentOf` 와 같다.
     */
    fun userAgent(context: Context, product: String): String {
        val version = runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull().orEmpty()
        return buildString {
            append(product)
            if (version.isNotEmpty()) append('/').append(version)
            append(" (Android ").append(Build.VERSION.RELEASE)
            if (!Build.MODEL.isNullOrBlank()) append("; ").append(Build.MODEL)
            append(')')
        }
    }

    /**
     * `+sip.instance` 로 쓸 기기 고유 URN(꺾쇠 없이) — TS 24.229 §5.1.1.2 는 IMEI URN(RFC 7254)을 요구하지만
     * 일반 앱은 Android 10 부터 IMEI 를 읽을 수 없다(READ_PRIVILEGED_PHONE_STATE). 그래서 ANDROID_ID 에서
     * 이름 기반 UUID(RFC 4122 v3)를 만든다 — ANDROID_ID 는 서명 키·사용자·기기 단위라 같은 서명의 CIMS 앱
     * (PTT·VoLTE)이 한 기기에서 같은 값을 쓴다. 값을 못 얻으면 null(pjsip 기본값 — 호스트명 해시라 기기마다
     * 같을 수 있다, registration_binding_set.md §8).
     */
    fun instanceUrn(context: Context): String? {
        val id = runCatching {
            Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID)
        }.getOrNull()?.takeIf { it.isNotBlank() } ?: return null
        return "urn:uuid:" + UUID.nameUUIDFromBytes("cims-ue:$id".toByteArray(Charsets.UTF_8))
    }
}
