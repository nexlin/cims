package com.cims.ue.ptt.mcdata

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** TS 24.282 MCData SDS 코덱 검증 (multipart/mixed + TLV 인코드/디코드 라운드트립). */
class McDataCodecTest {

    @Test fun groupSdsRoundTrip() {
        val convId = McDataCodec.conversationIdOf("g001")
        val msgId = McDataCodec.newMessageId()
        val (ct, body) = McDataCodec.buildGroupSds(
            groupUri = "tel:g001", text = "안녕하세요 테스트 메시지", convId = convId, msgId = msgId,
            timeSec = 1_700_000_000L,
        )
        assertTrue(ct.startsWith("multipart/mixed;boundary="))
        assertTrue(body.contains("application/vnd.3gpp.mcdata-signalling"))
        assertTrue(body.contains("application/vnd.3gpp.mcdata-payload"))
        assertTrue(body.contains("urn:3gpp:ns:mcdataInfo:1.0"))

        val p = McDataCodec.parse(ct, body)
        assertTrue(p is McDataCodec.SdsMessage)
        p as McDataCodec.SdsMessage
        assertEquals(convId, p.convId)
        assertEquals(msgId, p.msgId)
        assertEquals(1_700_000_000L, p.time)
        assertEquals(McDataCodec.DISP_REQ_DELIVERY, p.dispositionReq)
        assertEquals("안녕하세요 테스트 메시지", p.text)
        assertEquals("tel:g001", p.groupUri)
    }

    @Test fun conversationIdIsStablePerGroup() {
        assertEquals(McDataCodec.conversationIdOf("g001"), McDataCodec.conversationIdOf("g001"))
        assertTrue(McDataCodec.conversationIdOf("g001") != McDataCodec.conversationIdOf("g002"))
        assertEquals(32, McDataCodec.conversationIdOf("g001").length)
    }

    @Test fun notificationRoundTrip() {
        val convId = McDataCodec.conversationIdOf("g002")
        val msgId = McDataCodec.newMessageId()
        val (ct, body) = McDataCodec.buildNotification(convId, msgId, McDataCodec.NOTIF_DELIVERED)

        val p = McDataCodec.parse(ct, body)
        assertTrue(p is McDataCodec.SdsNotification)
        p as McDataCodec.SdsNotification
        assertEquals(convId, p.convId)
        assertEquals(msgId, p.msgId)
        assertEquals(McDataCodec.NOTIF_DELIVERED, p.type)
    }

    @Test fun noDispositionWhenNotRequested() {
        val (ct, body) = McDataCodec.buildGroupSds(
            groupUri = "tel:g001", text = "x", convId = McDataCodec.conversationIdOf("g001"),
            msgId = McDataCodec.newMessageId(), requestDelivery = false,
        )
        val p = McDataCodec.parse(ct, body) as McDataCodec.SdsMessage
        assertEquals(0, p.dispositionReq)
    }

    @Test fun boundaryFallbackFromBody() {
        // Content-Type 에 boundary 파라미터가 유실돼도 본문 첫 줄에서 유도
        val (_, body) = McDataCodec.buildGroupSds(
            groupUri = "tel:g001", text = "폴백", convId = McDataCodec.conversationIdOf("g001"),
            msgId = McDataCodec.newMessageId(),
        )
        val p = McDataCodec.parse("multipart/mixed", body)
        assertTrue(p is McDataCodec.SdsMessage)
        assertEquals("폴백", (p as McDataCodec.SdsMessage).text)
    }

    @Test fun groupFdRoundTrip() {
        val convId = McDataCodec.conversationIdOf("g001")
        val msgId = McDataCodec.newMessageId()
        val (ct, body) = McDataCodec.buildGroupFd(
            groupUri = "tel:g001", fileUrl = "https://10.0.1.45:4430/mcdata/fd/abc123",
            fileName = "현장 사진.jpg", fileSize = 123456L, mime = "image/jpeg",
            convId = convId, msgId = msgId, timeSec = 1_700_000_000L,
        )
        val p = McDataCodec.parse(ct, body)
        assertTrue(p is McDataCodec.FdMessage)
        p as McDataCodec.FdMessage
        assertEquals(convId, p.convId)
        assertEquals(msgId, p.msgId)
        assertEquals("https://10.0.1.45:4430/mcdata/fd/abc123", p.fileUrl)
        assertEquals("현장 사진.jpg", p.fileName)
        assertEquals(123456L, p.fileSize)
        assertEquals("image/jpeg", p.fileType)
        assertEquals("tel:g001", p.groupUri)
    }

    @Test fun rawTlvBuildersMatchGroupSdsParts() {
        // buildGroupSds 가 raw 빌더 산출물의 base64 인 것 확인 — MSRP(raw)와 C-plane(base64) 동일 TLV
        val convId = McDataCodec.conversationIdOf("g001")
        val msgId = McDataCodec.newMessageId()
        val sig = McDataCodec.buildSdsSignallingTlv(convId, msgId, timeSec = 1_700_000_000L)
        val pay = McDataCodec.buildSdsPayloadTlv("가나다 abc")
        val (_, body) = McDataCodec.buildGroupSds(
            groupUri = "tel:g001", text = "가나다 abc", convId = convId, msgId = msgId,
            timeSec = 1_700_000_000L,
        )
        val b64 = java.util.Base64.getEncoder()
        assertTrue(body.contains(b64.encodeToString(sig)))
        assertTrue(body.contains(b64.encodeToString(pay)))
    }

    @Test fun rawSignallingTlvLayout() {
        val convId = McDataCodec.conversationIdOf("g001")
        val msgId = McDataCodec.newMessageId()
        val withDisp = McDataCodec.buildSdsSignallingTlv(convId, msgId, timeSec = 1L)
        assertEquals(39, withDisp.size)
        assertEquals(McDataCodec.MSG_SDS_SIGNALLING, withDisp[0].toInt())
        assertEquals((0x80 or McDataCodec.DISP_REQ_DELIVERY).toByte(), withDisp[38])
        val noDisp = McDataCodec.buildSdsSignallingTlv(convId, msgId, requestDelivery = false, timeSec = 1L)
        assertEquals(38, noDisp.size)
    }

    @Test fun sdsPayloadSizeIsUtf8ContentBytes() {
        // 서버 게이트(McDataCodec.cpp payload content 합산)와 동일 기준 — content-type 옥텟 제외
        assertEquals(3, McDataCodec.sdsPayloadSize("abc"))
        assertEquals(3, McDataCodec.sdsPayloadSize("가"))          // UTF-8 3바이트
        val pay = McDataCodec.buildSdsPayloadTlv("abc")
        val len = (pay[3].toInt() and 0xFF shl 8) or (pay[4].toInt() and 0xFF)
        assertEquals(1 + 3, len)                                    // TLV 길이 = content-type(1)+data
    }

    @Test fun parseInfoUrisFromInviteBody() {
        // 서버(InviteMsrpReceiver) mcdata-info multipart 형식 — 그룹·원발신자 추출
        val invite = """
            INVITE sip:+82500000001@ptt.x SIP/2.0
            Content-Type: multipart/mixed;boundary=mcdata_ab12

            --mcdata_ab12
            Content-Type: application/vnd.3gpp.mcdata-info+xml

            <?xml version="1.0" encoding="UTF-8"?>
            <mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0">
              <mcdata-Params>
                <request-type>group-sds</request-type>
                <mcdata-request-uri type="Normal"><mcdataURI>tel:g001</mcdataURI></mcdata-request-uri>
                <mcdata-calling-user-id type="Normal"><mcdataURI>tel:+82500000002</mcdataURI></mcdata-calling-user-id>
              </mcdata-Params>
            </mcdatainfo>
            --mcdata_ab12--
        """.trimIndent()
        val (group, caller) = McDataCodec.parseInfoUris(invite)
        assertEquals("tel:g001", group)
        assertEquals("tel:+82500000002", caller)
        assertEquals(null to null, McDataCodec.parseInfoUris("no info here"))
    }

    @Test fun nonMcDataBodyReturnsNull() {
        assertNull(McDataCodec.parse("text/plain", "hello"))
        assertNull(McDataCodec.parse("multipart/mixed;boundary=x", "--x\r\nContent-Type: text/plain\r\n\r\nhi\r\n--x--\r\n"))
    }
}
