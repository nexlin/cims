// Android 접점 — 기기 식별자 (docs/design/features/ue_sdk.md §5.3, mcptt_management_views.md §4.1)
//
// **이 층은 프로토콜을 모른다.** 기기 값(ANDROID_ID)만 읽어 URN 을 만든다 — 그 값을 REGISTER Contact `+sip.instance`
// (`AccountConfig.instanceId`)와 UE initial configuration 조회의 MCS UE ID(`CscClient.fetchUeInitConfig`, TS 24.484 §7.2.1.1)에
// 넣는 것은 앱이다. .NET 파사드 `CimsUe.Platform.DeviceIdentity.InstanceUrn` 과 같은 규칙이다.
package com.cims.ue.sdk.platform

import android.content.Context
import android.provider.Settings
import java.util.UUID

object DeviceIdentity {
    /**
     * `+sip.instance` 로 쓸 기기 고유 URN(꺾쇠 없이). TS 24.229 §5.1.1.2 는 IMEI URN(RFC 7254)을 요구하지만 일반 앱은 Android 10 부터
     * IMEI 를 읽을 수 없다(READ_PRIVILEGED_PHONE_STATE) — ANDROID_ID 에서 이름 기반 UUID(RFC 4122 v3, `cims-ue:` 접두 MD5)를 만든다.
     * ANDROID_ID 는 서명 키·사용자·기기 단위라 같은 서명의 CIMS 앱이 한 기기에서 같은 값을 쓰고, 재설치해도 바뀌지 않는다
     * (Windows 는 MachineGuid 로 같은 규칙). 값을 못 얻으면 null — pjsip 기본값(호스트명 해시)이 쓰인다(registration_binding_set.md §8).
     */
    fun instanceUrn(context: Context): String? {
        val id = runCatching {
            Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID)
        }.getOrNull()?.takeIf { it.isNotBlank() } ?: return null
        return "urn:uuid:" + UUID.nameUUIDFromBytes("cims-ue:$id".toByteArray(Charsets.UTF_8))
    }
}
