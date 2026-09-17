// 신뢰 앵커 단위시험 — JVM, 기기·네트워크 불필요 (android_dispatch_tablet.md §9)
//
// 앵커가 깨지면 **로그인부터 막힌다**(TLS handshake 실패). PEM 한 글자만 틀어져도 그렇게 되므로
// 파싱·주체·유효기간을 고정해 둔다. `android/core/.../CimsTrustStore.kt` 와 같은 값이어야 한다
// — 둘을 함께 고쳐야 하며, 어긋나면 기존 앱과 태블릿 앱의 신뢰 기준이 갈라진다.
package com.cims.ue.sdk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.Date

class TrustAnchorTest {

    private fun anchors(): List<X509Certificate> =
        CertificateFactory.getInstance("X.509")
            .generateCertificates(ByteArrayInputStream(TrustAnchors.CA_BUNDLE.toByteArray()))
            .filterIsInstance<X509Certificate>()

    @Test fun `앵커가 파싱된다`() {
        val a = anchors()
        assertTrue("앵커가 비었다 — TLS 검증을 켤 수 없다", a.isNotEmpty())
    }

    @Test fun `앵커는 CIMS 루트 CA 다`() {
        val ca = anchors().first()
        assertTrue("주체가 다르다: ${ca.subjectX500Principal.name}",
            ca.subjectX500Principal.name.contains("CIMS Service CA"))
        // 자가서명(루트) — 사이트 CA·leaf 는 서버가 체인으로 보낸다(§387)
        assertEquals(ca.subjectX500Principal, ca.issuerX500Principal)
    }

    @Test fun `앵커가 CA 로 표시돼 있다`() {
        // basicConstraints CA:TRUE — -1 이면 CA 가 아니라 신뢰 저장소에 넣어도 체인이 서지 않는다
        assertTrue("basicConstraints 가 CA 가 아니다", anchors().first().basicConstraints >= 0)
    }

    @Test fun `앵커가 아직 유효하다`() {
        val ca = anchors().first()
        ca.checkValidity(Date())     // 만료면 예외
        // 남은 기간이 1년 미만이면 교체 준비가 필요하다(§8.6 무중단 교체는 신구 병기부터).
        val daysLeft = (ca.notAfter.time - System.currentTimeMillis()) / 86_400_000L
        assertTrue("앵커 잔여 ${daysLeft}일 — 교체(신규 루트 병기)를 시작해야 한다", daysLeft > 365)
    }

    @Test fun `PEM 에 들여쓰기가 남아 있지 않다`() {
        // trimIndent 를 빠뜨리면 OpenSSL PEM_read_bio_X509 가 못 읽는다(코어는 조용히 앵커 0개가 된다).
        TrustAnchors.CA_BUNDLE.lineSequence().forEach {
            assertEquals("PEM 줄 앞에 공백: '$it'", it.trimStart(), it)
        }
        assertTrue(TrustAnchors.CA_BUNDLE.startsWith("-----BEGIN CERTIFICATE-----"))
        assertTrue(TrustAnchors.CA_BUNDLE.trimEnd().endsWith("-----END CERTIFICATE-----"))
    }
}
