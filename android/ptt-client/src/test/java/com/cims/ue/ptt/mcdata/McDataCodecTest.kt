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

    @Test fun nonMcDataBodyReturnsNull() {
        assertNull(McDataCodec.parse("text/plain", "hello"))
        assertNull(McDataCodec.parse("multipart/mixed;boundary=x", "--x\r\nContent-Type: text/plain\r\n\r\nhi\r\n--x--\r\n"))
    }
}
