package com.cims.ue.core.net

import com.cims.ue.core.sip.CimsTrustStore
import okhttp3.OkHttpClient
import java.io.ByteArrayInputStream
import java.security.KeyStore
import java.security.cert.CertificateFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager

/**
 * CIMS 사설 CA 를 신뢰 앵커로 쓰는 HTTPS 설정 — CSC 접속(프로비저닝·IdMS·MCPTT API·FD)용.
 *
 * 단말이 붙는 두 평면(CSP SIP TLS / CSC HTTPS)의 서버 인증서는 **같은 CA** 로 발급된다
 * ([CimsTrustStore]). 그래서 앵커를 따로 배포할 필요가 없고, 단말에 심는 신뢰 기준이 하나로
 * 유지된다(sip_tls_signaling.md §8).
 *
 * OS 신뢰 저장소는 쓰지 않는다 — 사설 CA 는 거기 없다. 호스트명 검사는 OkHttp 기본 검증기가
 * 그대로 수행한다(인증서 SAN 에 접속 주소가 있어야 통과. IP 접속이면 IP SAN 이 필요하다).
 *
 * ⚠️ 이전 구현은 모든 인증서를 통과시키고 호스트명 검사도 끄는 TrustManager 였다
 * (`allowInsecureTls = true`). 이 채널로 **로그인 비밀번호와 SIP 접속 정보**가 오가므로,
 * 중간자가 CSC 서버 행세를 하면 그대로 넘겨주는 상태였다.
 */
object CimsTls {

    /** CIMS CA 앵커로 만든 신뢰 관리자. 앵커를 못 만들면 **예외** — 조용히 검증을 끄지 않는다. */
    private val trustManager: X509TrustManager by lazy { buildTrustManager() }

    /** OkHttp 빌더에 CIMS CA 기반 TLS 검증을 설치한다. */
    fun apply(builder: OkHttpClient.Builder): OkHttpClient.Builder {
        val tm = trustManager
        val ctx = SSLContext.getInstance("TLS").apply { init(null, arrayOf(tm), null) }
        return builder.sslSocketFactory(ctx.socketFactory, tm)
    }

    private fun buildTrustManager(): X509TrustManager {
        val factory = CertificateFactory.getInstance("X.509")
        val store = KeyStore.getInstance(KeyStore.getDefaultType()).apply { load(null, null) }
        var count = 0
        // CA_BUNDLE 은 PEM 여러 장을 이어붙일 수 있다(CA 교체 전환기) — 전부 앵커로 등록한다.
        ByteArrayInputStream(CimsTrustStore.CA_BUNDLE.toByteArray()).use { input ->
            factory.generateCertificates(input).forEach { cert ->
                store.setCertificateEntry("cims-ca-${count++}", cert)
            }
        }
        require(count > 0) { "CIMS CA 앵커가 비어 있다 — TLS 검증을 켤 수 없다" }
        val tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        tmf.init(store)
        return tmf.trustManagers.filterIsInstance<X509TrustManager>().first()
    }
}
