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
            if (f.id in FloorFieldId.STRING_FIELDS) {
                // 필드(헤더 2 + value) 를 4바이트 경계로 패딩
                val pad = pad4(2 + len)
                repeat(pad) { body.write(0) }
            }
        }
        var bodyBytes = body.toByteArray()
        // 전체 패킷을 32비트 경계로 (header 12 는 이미 4의 배수)
        val tailPad = pad4(bodyBytes.size)
        if (tailPad != 0) bodyBytes += ByteArray(tailPad)

        val total = HEADER_LEN + bodyBytes.size
        val buf = ByteArray(total)
        buf[0] = (0x80 or (msg.type and 0x1f)).toByte()   // V=2,P=0,subtype=type
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
        val type = buf[0].toInt() and 0x1f
        val ssrc = ((buf[4].toLong() and 0xff) shl 24) or ((buf[5].toLong() and 0xff) shl 16) or
            ((buf[6].toLong() and 0xff) shl 8) or (buf[7].toLong() and 0xff)

        val fields = ArrayList<FloorField>()
        var p = HEADER_LEN
        while (p + 2 <= len) {
            val id = buf[p].toInt() and 0xff
            val fl = buf[p + 1].toInt() and 0xff
            val start = p
            p += 2
            if (p + fl > len) break                                   // 손상
            val value = buf.copyOfRange(p, p + fl)
            p += fl
            if (id in FloorFieldId.STRING_FIELDS) p += pad4(p - start) // 필드 4B 정렬
            // id==0(Priority) & fl==0 등 trailing zero 패딩을 필드로 오인하지 않도록:
            if (id == 0 && fl == 0) break
            fields.add(FloorField(id, value))
        }
        return FloorMessage(type, ssrc, fields)
    }

    private fun pad4(n: Int): Int = (4 - (n % 4)) % 4

    // ── UE 송신 메시지 빌더 (TS 24.380) ──

    /** Floor Request: Floor Priority + User ID (+ Floor Indicator 선택). PTT down. */
    fun request(ssrc: Long, userId: String, priority: Int = 0, indicator: Int? = null): ByteArray {
        val fields = ArrayList<FloorField>()
        fields.add(FloorField(FloorFieldId.PRIORITY, byteArrayOf((priority and 0xff).toByte(), 0)))
        fields.add(FloorField(FloorFieldId.USER_ID, userId.toByteArray(Charsets.UTF_8)))
        if (indicator != null) fields.add(u16Field(FloorFieldId.FLOOR_INDICATOR, indicator))
        return encode(FloorMessage(FloorMsgType.REQUEST, ssrc, fields))
    }

    /** Floor Release: User ID. PTT up. */
    fun release(ssrc: Long, userId: String): ByteArray =
        encode(FloorMessage(FloorMsgType.RELEASE, ssrc, listOf(FloorField(FloorFieldId.USER_ID, userId.toByteArray()))))

    /** Floor Queue Position Request: User ID. */
    fun queuePositionRequest(ssrc: Long, userId: String): ByteArray =
        encode(FloorMessage(FloorMsgType.QUEUE_POS_REQUEST, ssrc, listOf(FloorField(FloorFieldId.USER_ID, userId.toByteArray()))))

    /** Floor Ack. */
    fun ack(ssrc: Long): ByteArray = encode(FloorMessage(FloorMsgType.ACK, ssrc, emptyList()))

    private fun u16Field(id: Int, v: Int) =
        FloorField(id, byteArrayOf(((v ushr 8) and 0xff).toByte(), (v and 0xff).toByte()))
}
