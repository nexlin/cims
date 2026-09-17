// 단말 신뢰 앵커 (sip_tls_signaling.md §8, android_dispatch_tablet.md §6.9)
//
// 단말이 붙는 두 평면(CSP SIP TLS / CSC HTTPS)의 서버 인증서는 **같은 앵커**로 검증한다.
// 그래서 `EngineConfig.tlsCaPem` 과 `CscEndpoint.caPem` 에 같은 값을 넣는다(ue_sdk.md §4.4).
//
// **앵커는 APK 에 동봉한다.** 프로비저닝으로 내려받지 않는다 — 프로비저닝 채널(CSC 4430) 자체가
// 이 앵커로 검증되므로, 신뢰의 최초 씨앗을 그 채널로 받으면 중간자가 자기 CA 를 심을 수 있다
// (sip_tls_signaling.md §8.5·§579).
//
// **앵커는 루트 한 장이다.** 사이트 CA 와 leaf 는 서버가 체인으로 보낸다 — 그래서 APK 는 사이트 수·
// 서버 주소·사이트 CA 교체와 무관하다(§387). 실제 배포 체인의 예:
//     leaf(CN=ctrl01) ← 사이트 CA(CN=CIMS-OAM-CA) ← 루트(CN=CIMS Service CA, 여기 있는 것)
//
// **교체(무중단)**: [CA_BUNDLE] 에 신규 루트를 **추가**한 앱을 먼저 배포해 둘을 동시에 신뢰시키고 →
// 서버 인증서를 신규 체인으로 바꾸고 → 다음 배포에서 구 루트를 뺀다. 코어는 PEM 을 전부 적재한다
// (`https_client.cpp` 의 `loadCa`, pjsip 의 `caBuf` 도 같다).
//
// ⚠ 같은 앵커가 `android/core/.../sip/CimsTrustStore.kt` 에도 있다 — 기존 PTT/VoLTE 앱이 아직 SDK
// 파사드로 넘어오지 않았기 때문이다(ue_sdk.md §5.3). 전환이 끝나면 그쪽이 걷힌다. **둘을 함께 고친다.**
package com.cims.ue.sdk

object TrustAnchors {

    /** CIMS Service CA — 자가서명 RSA4096, 만료 2036-08-15. */
    private val SERVICE_CA = """
        -----BEGIN CERTIFICATE-----
        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
        -----END CERTIFICATE-----
    """.trimIndent()

    /** `EngineConfig.tlsCaPem`·`CscEndpoint.caPem` 에 넣을 앵커 묶음. 여러 장을 이어붙여도 전부 적재된다. */
    val CA_BUNDLE: String = listOf(SERVICE_CA).joinToString("\n")
}
