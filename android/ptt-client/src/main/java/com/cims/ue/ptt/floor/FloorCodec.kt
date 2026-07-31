package com.cims.ue.ptt.floor

import java.io.ByteArrayOutputStream

/**
 * TS 24.380 §8 floor control 메시지 ↔ RTCP APP 바이트 인코더/디코더.
 *
 * 패킷 레이아웃:
 * ```
 *  0               1               2               3
 * |V=2|P| subtype |    PT=204     |          length(words-1)      |
 * |                         SSRC (sender)                         |
 * |                       name = "MCPT"                           |
 * | field: ID(8) Length(8) value(Length) [string필드는 4B 정렬패딩] ... |
 * ```
 */
object FloorCodec {

    private const val PT_APP = 204
    private val NAME = byteArrayOf('M'.code.toByte(), 'C'.code.toByte(), 'P'.code.toByte(), 'T'.code.toByte())
    private const val HEADER_LEN = 12   // V/P/subtype + PT + length + SSRC + name

    fun encode(msg: FloorMessage): ByteArray {
        val body = ByteArrayOutputStream()
        for (f in msg.fields) {
            val len = f.value.size
            require(len in 0..255) { "field ${f.id} length $len out of range" }
            body.write(f.id)
            body.write(len)
            body.write(f.value)
            // §8.1.3 — 모든 필드는 (헤더 2 + value) 를 4바이트 경계로 패딩한다.
            //   고정 2옥텟 값 필드는 2+2=4 라 패딩이 0 이고, 가변 길이 필드만 실제로 채워진다.
            repeat(pad4(2 + len)) { body.write(0) }
        }
        var bodyBytes = body.toByteArray()
        // 전체 패킷을 32비트 경계로 (header 12 는 이미 4의 배수)
        val tailPad = pad4(bodyBytes.size)
        if (tailPad != 0) bodyBytes += ByteArray(tailPad)

        val total = HEADER_LEN + bodyBytes.size
        val buf = ByteArray(total)
        // V=2,P=0,subtype=메시지타입(+ack 요구 비트). 단말은 ack 를 요구하지 않으므로 통상 0.
        val subtype = (msg.type and 0x0f) or (if (msg.ackRequired) FloorMsgType.ACK_REQUIRED_BIT else 0)
        buf[0] = (0x80 or subtype).toByte()
        buf[1] = PT_APP.toByte()
        val words = total / 4 - 1
        buf[2] = (words ushr 8).toByte()
        buf[3] = words.toByte()
        buf[4] = (msg.ssrc ushr 24).toByte()
        buf[5] = (msg.ssrc ushr 16).toByte()
        buf[6] = (msg.ssrc ushr 8).toByte()
        buf[7] = msg.ssrc.toByte()
        System.arraycopy(NAME, 0, buf, 8, 4)
        System.arraycopy(bodyBytes, 0, buf, HEADER_LEN, bodyBytes.size)
        return buf
    }

    /** 수신 패킷 파싱. MCPT APP 가 아니거나 손상 시 null. */
    fun decode(buf: ByteArray, len: Int = buf.size): FloorMessage? {
        if (len < HEADER_LEN) return null
        if ((buf[0].toInt() and 0xc0) != 0x80) return null            // V=2
        if ((buf[1].toInt() and 0xff) != PT_APP) return null          // PT=204
        if (!(buf[8] == NAME[0] && buf[9] == NAME[1] && buf[10] == NAME[2] && buf[11] == NAME[3])) return null
        // subtype 첫 비트(0x10) = "Ack 요구" 변종(§8.2.2) — 걷어내 기본 타입으로 처리하고,
        //   요구가 있었다는 사실만 FloorMessage.ackRequired 로 남긴다(회신은 FloorClient).
        val subtype = buf[0].toInt() and 0x1f
        val type = FloorMsgType.op(subtype)
        val ackReq = (subtype and FloorMsgType.ACK_REQUIRED_BIT) != 0
        val ssrc = ((buf[4].toLong() and 0xff) shl 24) or ((buf[5].toLong() and 0xff) shl 16) or
            ((buf[6].toLong() and 0xff) shl 8) or (buf[7].toLong() and 0xff)

        val fields = ArrayList<FloorField>()
        var p = HEADER_LEN
        while (p + 2 <= len) {
            val id = buf[p].toInt() and 0xff
            // §8.1.3 — Length 는 ID<192 면 1옥텟, 그 이상이면 2옥텟.
            val hdr = if (id >= 192) 3 else 2
            if (p + hdr > len) break
            val fl = if (hdr == 3) ((buf[p + 1].toInt() and 0xff) shl 8) or (buf[p + 2].toInt() and 0xff)
                     else buf[p + 1].toInt() and 0xff
            // id==0(Priority) & fl==0 등 trailing zero 패딩을 필드로 오인하지 않도록:
            if (id == 0 && fl == 0) break
            if (p + hdr + fl > len) break                             // 손상
            fields.add(FloorField(id, buf.copyOfRange(p + hdr, p + hdr + fl)))
            p += hdr + fl
            p += pad4(hdr + fl)                                       // 모든 필드 4B 정렬(§8.1.3)
        }
        return FloorMessage(type, ssrc, fields, ackRequired = ackReq)
    }

    private fun pad4(n: Int): Int = (4 - (n % 4)) % 4

    // ── UE 송신 메시지 빌더 (TS 24.380) ──

    /**
     * Floor Request: User ID (+ Floor Priority·Floor Indicator 선택). PTT down.
     *
     * [priority] 는 **명시 요청이 있을 때만** 싣는다(§6.3.5.4.4-1a) — 유효 우선순위는 SDP 로
     * 협상한 `mc_priority` 와 요청값 중 **낮은 쪽**이라, 협상 단말이 관례적으로 0 을 실으면
     * 자기 우선순위를 0 으로 깎아 선점이 죽는다. 미포함이면 서버 기본값이 적용된다.
     */
    fun request(ssrc: Long, userId: String, priority: Int? = null, indicator: Int? = null): ByteArray {
        val fields = ArrayList<FloorField>()
        if (priority != null) fields.add(FloorField(FloorFieldId.PRIORITY, byteArrayOf((priority and 0xff).toByte(), 0)))
        fields.add(FloorField(FloorFieldId.USER_ID, userId.toByteArray(Charsets.UTF_8)))
        if (indicator != null) fields.add(u16Field(FloorFieldId.FLOOR_INDICATOR, indicator))
        return encode(FloorMessage(FloorMsgType.REQUEST, ssrc, fields))
    }

    /** Floor Release: User ID (+ Floor Indicator — dual floor 의 G-bit 는 Release 에도 실린다). */
    fun release(ssrc: Long, userId: String, indicator: Int? = null): ByteArray {
        val fields = ArrayList<FloorField>()
        fields.add(FloorField(FloorFieldId.USER_ID, userId.toByteArray(Charsets.UTF_8)))
        if (indicator != null) fields.add(u16Field(FloorFieldId.FLOOR_INDICATOR, indicator))
        return encode(FloorMessage(FloorMsgType.RELEASE, ssrc, fields))
    }

    /** Floor Queue Position Request: User ID. */
    fun queuePositionRequest(ssrc: Long, userId: String): ByteArray =
        encode(FloorMessage(FloorMsgType.QUEUE_POS_REQUEST, ssrc, listOf(FloorField(FloorFieldId.USER_ID, userId.toByteArray()))))

    /**
     * Queued Floor Requests(§8.2.15) — Purpose=Cancel Request. **자기 대기 요청 취소**.
     *
     * List of Queued Users 를 싣지 않는다 — 서버는 목록 없는 취소를 요청자 본인의 것으로 해석한다
     * (§6.3.4.4.13: 참가자는 자기 요청만 취소할 수 있다). 발신 주소로 신원이 이미 정해지므로
     * 단말이 자기 ID 표기(URI/번호)를 서버 표기와 맞출 필요가 없다.
     */
    fun cancelQueuedRequest(ssrc: Long): ByteArray = encode(
        FloorMessage(FloorMsgType.QUEUED_CANCEL, ssrc,
            listOf(u16Field(FloorFieldId.QUEUED_PURPOSE, FloorQueuedPurpose.CANCEL_REQUEST))))

    /**
     * Floor Ack (§8.2.13) — ack 요구 변종을 받았음을 확인한다.
     * Source(10)=0(floor participant) + Message Type(12)=확인 대상 subtype(ack 비트 포함, §8.2.3.14).
     */
    fun ackOf(ssrc: Long, ackedSubtype: Int): ByteArray = encode(
        FloorMessage(FloorMsgType.ACK, ssrc, listOf(
            u16Field(FloorFieldId.SOURCE, FloorSource.PARTICIPANT),
            // Message Type 은 2옥텟 — 상위 옥텟에 subtype, 하위는 spare.
            FloorField(FloorFieldId.MSG_TYPE, byteArrayOf((ackedSubtype and 0x1f).toByte(), 0)),
        )))

    /** Floor Ack(NAT keepalive 변형) — [userId] 로 서버가 NAT 뒤 참가자의 floor 주소를 latch 한다.
     *  서버 상태를 바꾸지 않는 무해한 상향이라 경로 개방·유지 전용으로 쓴다(ue_nat_traversal.md §7.1). */
    fun ack(ssrc: Long, userId: String? = null): ByteArray = encode(
        FloorMessage(FloorMsgType.ACK, ssrc,
            if (userId == null) emptyList()
            else listOf(FloorField(FloorFieldId.USER_ID, userId.toByteArray(Charsets.UTF_8)))))

    private fun u16Field(id: Int, v: Int) =
        FloorField(id, byteArrayOf(((v ushr 8) and 0xff).toByte(), (v and 0xff).toByte()))
}
