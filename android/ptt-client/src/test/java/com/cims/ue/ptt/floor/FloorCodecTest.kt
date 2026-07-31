package com.cims.ue.ptt.floor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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

    // ── Ack 요구 변종 (§8.2.2 / §8.2.13) ──

    @Test fun ackRequiredVariantDecodesAsBaseType() {
        // 서버가 0x12(=Taken+ack요구)로 보낸 것을 Taken 으로 읽고, 요구 사실은 따로 남긴다.
        val taken = FloorCodec.encode(FloorMessage(
            FloorMsgType.TAKEN, ssrc = 5L,
            fields = listOf(FloorField(FloorFieldId.GRANTED_PARTY, "tel:+82571900003".toByteArray())),
            ackRequired = true,
        ))
        assertEquals(FloorMsgType.TAKEN or FloorMsgType.ACK_REQUIRED_BIT, taken[0].toInt() and 0x1f)

        val msg = FloorCodec.decode(taken)!!
        assertEquals(FloorMsgType.TAKEN, msg.type)      // 기본 타입으로 처리
        assertTrue(msg.ackRequired)
        assertEquals("tel:+82571900003", msg.grantedParty)
    }

    @Test fun floorAckCarriesSourceAndMessageType() {
        val acked = FloorMsgType.TAKEN or FloorMsgType.ACK_REQUIRED_BIT
        val msg = FloorCodec.decode(FloorCodec.ackOf(ssrc = 42L, ackedSubtype = acked))!!
        assertEquals(FloorMsgType.ACK, msg.type)
        assertFalse(msg.ackRequired)                     // 단말은 ack 를 요구하지 않는다
        assertEquals(FloorSource.PARTICIPANT, msg.field(FloorFieldId.SOURCE)!!.asU16())
        // Message Type(§8.2.3.14) — 상위 옥텟에 확인 대상 subtype(ack 비트 포함)
        assertEquals(acked, msg.field(FloorFieldId.MSG_TYPE)!!.value[0].toInt() and 0x1f)
    }

    // ── Floor Priority 는 명시 요청이 있을 때만 (§6.3.5.4.4-1a, U15) ──

    @Test fun requestOmitsPriorityFieldByDefault() {
        val msg = FloorCodec.decode(FloorCodec.request(1L, "tel:+82571900001"))!!
        assertNull(msg.field(FloorFieldId.PRIORITY))
        assertNull(msg.priority)
        assertEquals("tel:+82571900001", msg.userId)
    }

    @Test fun releaseCarriesGbitWhenGiven() {
        val msg = FloorCodec.decode(
            FloorCodec.release(1L, "tel:+82571900001", FloorIndicator.DUAL_FLOOR))!!
        assertEquals(FloorMsgType.RELEASE, msg.type)
        assertEquals(FloorIndicator.DUAL_FLOOR, msg.floorIndicator)
    }

    // ── Floor Taken 신규 필드 (U6·U7·U8) ──

    @Test fun takenNewFieldsDecode() {
        val taken = FloorMessage(
            FloorMsgType.TAKEN, ssrc = 0xC0FFEEL,          // 헤더 SSRC = **서버** SSRC
            fields = listOf(
                FloorField(FloorFieldId.GRANTED_PARTY, "tel:+82571900001".toByteArray()),
                u16(FloorFieldId.PERMISSION, FloorPermission.DENIED),
                u16(FloorFieldId.MSG_SEQ, 65535),
                // SSRC 필드(§8.2.3.16) = SSRC(4) + spare(2)
                FloorField(FloorFieldId.SSRC, byteArrayOf(0x11, 0x22, 0x33, 0x44, 0, 0)),
            ),
        )
        val msg = FloorCodec.decode(FloorCodec.encode(taken))!!
        assertEquals(FloorPermission.DENIED, msg.permission)
        assertEquals(65535, msg.msgSeq)
        assertEquals(0x11223344L, msg.speakerSsrc)         // 화자 SSRC 는 필드에서
        assertEquals(0xC0FFEEL, msg.ssrc)                  // 헤더는 서버 SSRC — 화자 식별에 쓰지 않는다
    }

    @Test fun multiTalkerListsDecodeInOrder() {
        // List of Granted Users(§8.2.3.17) = No of users(1) + [len(1)+UTF-8]*
        val users = byteArrayOf(2) + byteArrayOf(3) + "aaa".toByteArray() + byteArrayOf(4) + "bbbb".toByteArray()
        // List of SSRCs(§8.2.3.18) = count(1) + spare(1) + SSRC(4)*
        val ssrcs = byteArrayOf(2, 0, 0, 0, 0, 1, 0, 0, 0, 2)
        val taken = FloorMessage(
            FloorMsgType.TAKEN, ssrc = 1L,
            fields = listOf(
                FloorField(FloorFieldId.GRANTED_USERS, users),
                FloorField(FloorFieldId.SSRC_LIST, ssrcs),
            ),
        )
        val msg = FloorCodec.decode(FloorCodec.encode(taken))!!
        assertEquals(listOf("aaa", "bbbb"), msg.grantedUsers)
        assertEquals(listOf(1L, 2L), msg.ssrcList)
    }

    // ── 화자 집합 파생 (U11·U12) ──

    @Test fun singleTalkerDerivedFromGrantedPartyAndSsrc() {
        val msg = FloorCodec.decode(FloorCodec.encode(FloorMessage(
            FloorMsgType.TAKEN, ssrc = 0xC0FFEEL,
            fields = listOf(
                FloorField(FloorFieldId.GRANTED_PARTY, "tel:+82571900001".toByteArray()),
                FloorField(FloorFieldId.SSRC, byteArrayOf(0x11, 0x22, 0x33, 0x44, 0, 0)),
            ),
        )))!!
        assertEquals(listOf(FloorTalker("tel:+82571900001", 0x11223344L)), msg.talkers)
    }

    @Test fun multiTalkerSetPrefersListsAndKeepsOrder() {
        val users = byteArrayOf(2) + byteArrayOf(3) + "aaa".toByteArray() + byteArrayOf(3) + "bbb".toByteArray()
        val msg = FloorCodec.decode(FloorCodec.encode(FloorMessage(
            FloorMsgType.TAKEN, ssrc = 1L,
            fields = listOf(
                // 리스트가 있으면 Granted Party 단일 화자보다 우선한다.
                FloorField(FloorFieldId.GRANTED_PARTY, "aaa".toByteArray()),
                FloorField(FloorFieldId.GRANTED_USERS, users),
                FloorField(FloorFieldId.SSRC_LIST, byteArrayOf(2, 0, 0, 0, 0, 7, 0, 0, 0, 9)),
            ),
        )))!!
        assertEquals(listOf(FloorTalker("aaa", 7L), FloorTalker("bbb", 9L)), msg.talkers)
    }

    @Test fun releaseMultiTalkerNamesTheLeavingSpeaker() {
        val msg = FloorCodec.decode(FloorCodec.encode(FloorMessage(
            FloorMsgType.RELEASE_MULTI, ssrc = 0xC0FFEEL,
            fields = listOf(
                FloorField(FloorFieldId.SSRC, byteArrayOf(0, 0, 0, 2, 0, 0)),
                FloorField(FloorFieldId.USER_ID, "tel:+82571900002".toByteArray()),
                u16(FloorFieldId.FLOOR_INDICATOR, FloorIndicator.NORMAL or FloorIndicator.MULTI_TALKER),
            ),
        )))!!
        assertEquals(FloorMsgType.RELEASE_MULTI, msg.type)
        assertEquals("tel:+82571900002", msg.userId)
        assertEquals(2L, msg.speakerSsrc)
        assertEquals(FloorIndicator.MULTI_TALKER, msg.floorIndicator!! and FloorIndicator.MULTI_TALKER)
    }

    @Test fun unknownFieldIsSkippedByPadding() {
        // §8.1.4 — 미지 필드도 4옥텟 정렬 덕에 건너뛰고 뒤 필드를 읽을 수 있어야 한다.
        val msg = FloorCodec.decode(FloorCodec.encode(FloorMessage(
            FloorMsgType.TAKEN, ssrc = 1L,
            fields = listOf(
                FloorField(99, byteArrayOf(1, 2, 3, 4, 5)),       // 미정의 필드(2+5 → 8 로 패딩)
                FloorField(FloorFieldId.GRANTED_PARTY, "tel:+82571900009".toByteArray()),
            ),
        )))!!
        assertEquals("tel:+82571900009", msg.grantedParty)
    }

    // ── 대기열 (U16) ──

    @Test fun cancelQueuedRequestHasPurposeAndNoUserList() {
        val msg = FloorCodec.decode(FloorCodec.cancelQueuedRequest(1L))!!
        assertEquals(FloorMsgType.QUEUED_CANCEL, msg.type)
        assertEquals(FloorQueuedPurpose.CANCEL_REQUEST, msg.queuedPurpose)
        // 대상 목록을 싣지 않아야 서버가 "요청자 본인의 요청만" 취소로 해석한다.
        assertNull(msg.field(FloorFieldId.QUEUED_USERS))
    }

    @Test fun queuePositionInfoDecodes() {
        // Queue Info(§8.2.3.5) = position(1옥텟) + priority(1옥텟)
        val msg = FloorCodec.decode(FloorCodec.encode(FloorMessage(
            FloorMsgType.QUEUE_POS_INFO, ssrc = 1L,
            fields = listOf(FloorField(FloorFieldId.QUEUE_INFO, byteArrayOf(3, 5))),
        )))!!
        assertEquals(3, msg.queuePosition)
    }

    @Test fun cancelResultDecodes() {
        val msg = FloorCodec.decode(FloorCodec.encode(FloorMessage(
            FloorMsgType.QUEUED_CANCEL, ssrc = 1L,
            fields = listOf(
                u16(FloorFieldId.QUEUED_PURPOSE, FloorQueuedPurpose.CANCEL_RESULT),
                u16(FloorFieldId.QUEUED_RESULT, 0),
            ),
        )))!!
        assertEquals(FloorQueuedPurpose.CANCEL_RESULT, msg.queuedPurpose)
        assertEquals(0, msg.queuedResult)
    }

    private fun u16(id: Int, v: Int) =
        FloorField(id, byteArrayOf(((v ushr 8) and 0xff).toByte(), (v and 0xff).toByte()))
}
