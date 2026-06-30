package com.cims.ue.ptt.floor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** TS 24.380 floor control 코덱 검증 (RTCP APP "MCPT" 인코드/디코드 라운드트립). */
class FloorCodecTest {

    @Test fun requestRoundTrip() {
        val pkt = FloorCodec.request(ssrc = 0x01020304, userId = "tel:+82571900001", priority = 3)
        // RTCP APP 헤더 불변식
        assertEquals(0x80, pkt[0].toInt() and 0xe0 or 0)   // V=2,P=0
        assertEquals(FloorMsgType.REQUEST, pkt[0].toInt() and 0x1f)
        assertEquals(204, pkt[1].toInt() and 0xff)
        assertEquals("MCPT", String(pkt, 8, 4))
        assertEquals(0, pkt.size % 4)                       // 32비트 정렬

        val msg = FloorCodec.decode(pkt)!!
        assertEquals(FloorMsgType.REQUEST, msg.type)
        assertEquals(0x01020304L, msg.ssrc)
        assertEquals("tel:+82571900001", msg.userId)
        assertEquals(3, msg.priority)
    }

    @Test fun releaseRoundTrip() {
        val msg = FloorCodec.decode(FloorCodec.release(7L, "tel:+82571900002"))!!
        assertEquals(FloorMsgType.RELEASE, msg.type)
        assertEquals(7L, msg.ssrc)
        assertEquals("tel:+82571900002", msg.userId)
    }

    @Test fun grantedWithDurationDecodes() {
        // 서버가 보낼 Floor Granted(Duration 2옥텟=30s, Granted Party 문자열) 합성 후 디코드
        val granted = FloorMessage(
            FloorMsgType.GRANTED, ssrc = 1000L,
            fields = listOf(
                FloorField(FloorFieldId.DURATION, byteArrayOf(0, 30)),
                FloorField(FloorFieldId.GRANTED_PARTY, "tel:+82571900001".toByteArray()),
            ),
        )
        val msg = FloorCodec.decode(FloorCodec.encode(granted))!!
        assertEquals(FloorMsgType.GRANTED, msg.type)
        assertEquals(30, msg.durationSec)
        assertEquals("tel:+82571900001", msg.grantedParty)
    }

    @Test fun denyCauseDecodes() {
        val deny = FloorMessage(
            FloorMsgType.DENY, ssrc = 1L,
            fields = listOf(FloorField(FloorFieldId.REJECT_CAUSE, byteArrayOf(0, 1))),
        )
        val msg = FloorCodec.decode(FloorCodec.encode(deny))!!
        assertEquals(1, msg.rejectCause)
        assertEquals("Another MCPTT client has permission", FloorCause.REJECT[msg.rejectCause])
    }

    @Test fun stringFieldPaddedTo4Bytes() {
        // userId 길이 5 → 필드(2+5=7) 가 4의 배수 8 로 패딩되어야 다음 파싱이 안 깨진다
        val pkt = FloorCodec.encode(
            FloorMessage(FloorMsgType.REQUEST, 1L, listOf(
                FloorField(FloorFieldId.USER_ID, "abcde".toByteArray()),
                FloorField(FloorFieldId.PRIORITY, byteArrayOf(2, 0)),
            )),
        )
        assertEquals(0, pkt.size % 4)
        val msg = FloorCodec.decode(pkt)!!
        assertEquals("abcde", msg.userId)
        assertEquals(2, msg.priority)
    }

    @Test fun rejectsNonMcptPacket() {
        val bogus = ByteArray(16).also { it[0] = 0x80.toByte(); it[1] = 200.toByte() } // PT=200, name=0
        assertNull(FloorCodec.decode(bogus))
        assertNull(FloorCodec.decode(ByteArray(4)))   // 너무 짧음
    }

    @Test fun ackHasNoFields() {
        val msg = FloorCodec.decode(FloorCodec.ack(9L))!!
        assertEquals(FloorMsgType.ACK, msg.type)
        assertEquals(9L, msg.ssrc)
        assertTrue(msg.fields.isEmpty())
    }

    @Test fun encodedNameBytesExact() {
        val pkt = FloorCodec.ack(1L)
        assertArrayEquals(byteArrayOf('M'.code.toByte(), 'C'.code.toByte(), 'P'.code.toByte(), 'T'.code.toByte()),
            pkt.copyOfRange(8, 12))
    }
}
