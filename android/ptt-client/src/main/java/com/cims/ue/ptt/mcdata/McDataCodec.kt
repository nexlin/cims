package com.cims.ue.ptt.mcdata

import java.nio.ByteBuffer
import java.util.UUID

/**
 * MCData SDS 메시지 코덱 — TS 24.282 §15 (메시지 정의·IE 코딩) + Annex D(mcdata-info) + Annex E(MIME).
 *
 * SIP MESSAGE 본문 = multipart/mixed:
 *  - application/vnd.3gpp.mcdata-info+xml   : request-type(group-sds/one-to-one-sds…) + 대상 URI
 *                                             (그룹 URI 또는 1:1 상대 URI — 수신측 스레드 귀속 근거)
 *  - application/vnd.3gpp.mcdata-signalling : SDS SIGNALLING PAYLOAD / SDS NOTIFICATION (TLV)
 *  - application/vnd.3gpp.mcdata-payload    : DATA PAYLOAD (TLV)
 *
 * 바이너리 TLV 파트는 Content-Transfer-Encoding: base64 로 싣는다 — PJSIP Java 바인딩이
 * 본문을 String 으로만 다뤄 raw 바이너리가 UTF-8 재인코딩에 깨지기 때문 (규격 대비 자체 편차,
 * docs/design/features/mcdata_messaging.md 참조).
 */
object McDataCodec {

    // TS 24.282 §15.2.2 message types
    const val MSG_SDS_SIGNALLING = 0x01
    const val MSG_DATA_PAYLOAD = 0x03
    const val MSG_SDS_NOTIFICATION = 0x05

    // §15.2.3 SDS disposition request type
    const val DISP_REQ_DELIVERY = 0x01
    const val DISP_REQ_READ = 0x02
    const val DISP_REQ_DELIVERY_READ = 0x03

    // §15.2.5 SDS disposition notification type
    const val NOTIF_UNDELIVERED = 0x01
    const val NOTIF_DELIVERED = 0x02
    const val NOTIF_READ = 0x03
    const val NOTIF_DELIVERED_READ = 0x04

    const val MSG_FD_SIGNALLING = 0x02

    // §15.2.13 payload content type
    private const val PAYLOAD_TEXT = 0x01
    private const val PAYLOAD_FILEURL = 0x04

    const val CT_INFO = "application/vnd.3gpp.mcdata-info+xml"
    const val CT_SIGNALLING = "application/vnd.3gpp.mcdata-signalling"
    const val CT_PAYLOAD = "application/vnd.3gpp.mcdata-payload"

    // Annex D mcdata-info <request-type> — 그룹/1:1 × SDS/FD
    const val REQ_GROUP_SDS = "group-sds"
    const val REQ_ONE_TO_ONE_SDS = "one-to-one-sds"
    const val REQ_GROUP_FD = "group-fd"
    const val REQ_ONE_TO_ONE_FD = "one-to-one-fd"

    /** 파싱 결과 — SDS 본문 메시지 또는 disposition 통지. */
    sealed interface Parsed
    data class SdsMessage(
        val convId: String,          // UUID hex 32자
        val msgId: String,
        val time: Long,              // epoch seconds
        val dispositionReq: Int,     // 0=없음, DISP_REQ_*
        val text: String,
        val requestUri: String?,     // mcdata-info <mcdata-request-uri> — 그룹 URI 또는 1:1 상대(수신자) URI
        val oneToOne: Boolean = false, // <request-type> 이 one-to-one-* — 스레드 키는 발신자
    ) : Parsed
    data class SdsNotification(val convId: String, val msgId: String, val type: Int) : Parsed
    data class FdMessage(
        val convId: String,
        val msgId: String,
        val time: Long,
        val fileUrl: String,
        val fileName: String,
        val fileSize: Long,
        val fileType: String,
        val requestUri: String?,
        val oneToOne: Boolean = false,
    ) : Parsed

    /** 그룹 스레드 conversation ID — 그룹당 결정적 UUID (기기 간 동일, TS 24.282 의
     *  "기존 대화 지속 시 기존 Conversation ID 재사용" 을 그룹=상시 대화 1개로 프로파일링). */
    fun conversationIdOf(groupId: String): String =
        hex(UUID.nameUUIDFromBytes("cims-mcdata:$groupId".toByteArray(Charsets.UTF_8)))

    /** 1:1 대화 conversation ID — 사용자 쌍당 결정적 UUID(쌍을 정렬하므로 양쪽 단말이 같은 값). */
    fun conversationIdOf(userA: String, userB: String): String {
        val pair = listOf(userA, userB).sorted().joinToString(":")
        return hex(UUID.nameUUIDFromBytes("cims-mcdata:1to1:$pair".toByteArray(Charsets.UTF_8)))
    }

    fun newMessageId(): String = hex(UUID.randomUUID())

    /** SDS SIGNALLING PAYLOAD TLV (raw) — C-plane MESSAGE(base64)·MSRP 미디어평면(raw) 공용. */
    fun buildSdsSignallingTlv(
        convId: String,
        msgId: String,
        requestDelivery: Boolean = true,
        timeSec: Long = System.currentTimeMillis() / 1000,
    ): ByteArray = ByteBuffer.allocate(39).apply {
        put(MSG_SDS_SIGNALLING.toByte())
        putDateTime(timeSec)
        put(hexToBytes(convId))
        put(hexToBytes(msgId))
        if (requestDelivery) put((0x80 or DISP_REQ_DELIVERY).toByte())  // TV type1, IEI=8-
    }.array().let { if (requestDelivery) it else it.copyOf(38) }

    /** DATA PAYLOAD TLV (raw) — TEXT 단일 payload. */
    fun buildSdsPayloadTlv(text: String): ByteArray {
        val textBytes = text.toByteArray(Charsets.UTF_8)
        return ByteBuffer.allocate(2 + 3 + 1 + textBytes.size).apply {
            put(MSG_DATA_PAYLOAD.toByte())
            put(1)                                   // Number of payloads
            put(0x78)                                // Payload IEI (TLV-E)
            putShort((1 + textBytes.size).toShort()) // content-type + data
            put(PAYLOAD_TEXT.toByte())
            put(textBytes)
        }.array()
    }

    /** C-plane 임계 비교 기준 payload 크기 — 서버 게이트(McDataCodec.cpp payload content 합산)와
     *  동일하게 DATA PAYLOAD 의 내용 바이트(=UTF-8 텍스트 길이, content-type 옥텟 제외). */
    fun sdsPayloadSize(text: String): Int = text.toByteArray(Charsets.UTF_8).size

    /** mcdata-info XML 을 포함한 임의 문자열(예: 서버발 MSRP INVITE 원문)에서
     *  (mcdata-request-uri, mcdata-calling-user-id) URI 추출 — 없으면 각각 null. */
    fun parseInfoUris(s: String): Pair<String?, String?> {
        fun uriOf(elem: String): String? = Regex(
            "<$elem[^>]*>\\s*<mcdataURI>([^<]+)</mcdataURI>", RegexOption.DOT_MATCHES_ALL,
        ).find(s)?.groupValues?.get(1)?.trim()
        return uriOf("mcdata-request-uri") to uriOf("mcdata-calling-user-id")
    }

    /** SDS 발신 본문 생성 — [targetUri]=그룹 URI(group-sds) 또는 상대 URI(one-to-one-sds).
     *  @return (Content-Type, body) */
    fun buildSds(
        targetUri: String,
        text: String,
        convId: String,
        msgId: String,
        oneToOne: Boolean = false,
        requestDelivery: Boolean = true,
        timeSec: Long = System.currentTimeMillis() / 1000,
    ): Pair<String, String> {
        val signalling = buildSdsSignallingTlv(convId, msgId, requestDelivery, timeSec)
        val payload = buildSdsPayloadTlv(text)
        val info = mcDataInfoXml(if (oneToOne) REQ_ONE_TO_ONE_SDS else REQ_GROUP_SDS, targetUri)
        val boundary = "mcdata-${msgId.take(16)}"
        val body = buildString {
            appendPart(boundary, CT_INFO, null, info)
            appendPart(boundary, CT_SIGNALLING, "base64", b64(signalling))
            appendPart(boundary, CT_PAYLOAD, "base64", b64(payload))
            append("--$boundary--\r\n")
        }
        return "multipart/mixed;boundary=$boundary" to body
    }

    /** FD(파일전송) 발신 본문 생성 — FD SIGNALLING PAYLOAD(TS 24.282 §15.1.3):
     *  Payload IE=FILEURL, Metadata IE=RFC 5547 file-selector(name/size/type).
     *  [targetUri]=그룹 URI(group-fd) 또는 상대 URI(one-to-one-fd). @return (Content-Type, body) */
    fun buildFd(
        targetUri: String,
        fileUrl: String,
        fileName: String,
        fileSize: Long,
        mime: String,
        convId: String,
        msgId: String,
        oneToOne: Boolean = false,
        timeSec: Long = System.currentTimeMillis() / 1000,
    ): Pair<String, String> {
        val urlBytes = fileUrl.toByteArray(Charsets.UTF_8)
        val metaBytes = "name:\"${fileName.replace("\"", "")}\" size:$fileSize type:$mime"
            .toByteArray(Charsets.UTF_8)
        val tlv = ByteBuffer.allocate(38 + 3 + 1 + urlBytes.size + 3 + metaBytes.size).apply {
            put(MSG_FD_SIGNALLING.toByte())
            putDateTime(timeSec)
            put(hexToBytes(convId))
            put(hexToBytes(msgId))
            put(0x78)                                   // Payload (TLV-E)
            putShort((1 + urlBytes.size).toShort())
            put(PAYLOAD_FILEURL.toByte())
            put(urlBytes)
            put(0x79)                                   // Metadata (TLV-E)
            putShort(metaBytes.size.toShort())
            put(metaBytes)
        }.array()
        val info = mcDataInfoXml(if (oneToOne) REQ_ONE_TO_ONE_FD else REQ_GROUP_FD, targetUri)
        val boundary = "mcdata-fd-${msgId.take(14)}"
        val body = buildString {
            appendPart(boundary, CT_INFO, null, info)
            appendPart(boundary, CT_SIGNALLING, "base64", b64(tlv))
            append("--$boundary--\r\n")
        }
        return "multipart/mixed;boundary=$boundary" to body
    }

    /** SDS NOTIFICATION(전달/읽음 통지) 본문 생성 — 원 발신자 1:1 대상. @return (Content-Type, body) */
    fun buildNotification(
        convId: String,
        msgId: String,
        notifType: Int,
        timeSec: Long = System.currentTimeMillis() / 1000,
    ): Pair<String, String> {
        val tlv = ByteBuffer.allocate(39).apply {
            put(MSG_SDS_NOTIFICATION.toByte())
            put(notifType.toByte())
            putDateTime(timeSec)
            put(hexToBytes(convId))
            put(hexToBytes(msgId))
        }.array()
        val boundary = "mcdata-ntf-${msgId.take(12)}"
        val body = buildString {
            appendPart(boundary, CT_SIGNALLING, "base64", b64(tlv))
            append("--$boundary--\r\n")
        }
        return "multipart/mixed;boundary=$boundary" to body
    }

    /** multipart/mixed MCData 본문 파싱 — mcdata-signalling 파트가 없으면 null. */
    fun parse(contentType: String, body: String): Parsed? {
        val boundary = boundaryOf(contentType) ?: boundaryOf(body.lineSequence().firstOrNull())
            ?: return null
        var convId = ""; var msgId = ""; var time = 0L
        var dispositionReq = 0; var notifType = -1; var msgType = 0
        var text = ""; var requestUri: String? = null; var oneToOne = false
        var fileUrl = ""; var fileName = ""; var fileSize = 0L; var fileType = ""

        for (part in splitParts(body, boundary)) {
            when (part.contentType) {
                CT_SIGNALLING -> {
                    val b = part.bytes()
                    if (b.size < 38) continue
                    when (b[0].toInt() and 0x3F) {
                        MSG_SDS_SIGNALLING -> {
                            msgType = MSG_SDS_SIGNALLING
                            time = readDateTime(b, 1)
                            convId = hex(b, 6, 16)
                            msgId = hex(b, 22, 16)
                            var i = 38
                            while (i < b.size) {
                                val iei = b[i].toInt() and 0xFF
                                when {
                                    iei and 0xF0 == 0x80 -> { dispositionReq = iei and 0x0F; i += 1 }
                                    iei == 0x21 -> i += 17          // InReplyTo message ID
                                    iei == 0x22 -> i += 2           // Application ID
                                    iei == 0x7D -> {                // Extended application ID (TLV-E)
                                        if (i + 3 > b.size) break
                                        i += 3 + ((b[i + 1].toInt() and 0xFF shl 8) or (b[i + 2].toInt() and 0xFF))
                                    }
                                    else -> break
                                }
                            }
                        }
                        MSG_FD_SIGNALLING -> {
                            msgType = MSG_FD_SIGNALLING
                            time = readDateTime(b, 1)
                            convId = hex(b, 6, 16)
                            msgId = hex(b, 22, 16)
                            var i = 38
                            while (i < b.size) {
                                val iei = b[i].toInt() and 0xFF
                                when {
                                    iei and 0xF0 == 0x90 || iei and 0xF0 == 0xA0 -> i += 1  // FD disp req / mandatory dl
                                    iei == 0x21 -> i += 17
                                    iei == 0x22 -> i += 2
                                    iei == 0x78 || iei == 0x79 -> {                          // Payload / Metadata (TLV-E)
                                        if (i + 3 > b.size) break
                                        val len = (b[i + 1].toInt() and 0xFF shl 8) or (b[i + 2].toInt() and 0xFF)
                                        if (i + 3 + len > b.size) break
                                        if (iei == 0x78 && len >= 1 && (b[i + 3].toInt() and 0xFF) == PAYLOAD_FILEURL) {
                                            fileUrl = String(b, i + 4, len - 1, Charsets.UTF_8)
                                        } else if (iei == 0x79) {
                                            val meta = String(b, i + 3, len, Charsets.UTF_8)
                                            fileName = Regex("name:\"([^\"]*)\"").find(meta)?.groupValues?.get(1) ?: ""
                                            fileSize = Regex("size:(\\d+)").find(meta)?.groupValues?.get(1)?.toLongOrNull() ?: 0L
                                            fileType = Regex("type:(\\S+)").find(meta)?.groupValues?.get(1) ?: ""
                                        }
                                        i += 3 + len
                                    }
                                    else -> break
                                }
                            }
                        }
                        MSG_SDS_NOTIFICATION -> {
                            if (b.size < 39) continue
                            msgType = MSG_SDS_NOTIFICATION
                            notifType = b[1].toInt() and 0xFF
                            time = readDateTime(b, 2)
                            convId = hex(b, 7, 16)
                            msgId = hex(b, 23, 16)
                        }
                    }
                }
                CT_PAYLOAD -> {
                    val b = part.bytes()
                    if (b.size < 2 || (b[0].toInt() and 0x3F) != MSG_DATA_PAYLOAD) continue
                    var i = 2
                    while (i + 3 <= b.size) {
                        val iei = b[i].toInt() and 0xFF
                        val len = (b[i + 1].toInt() and 0xFF shl 8) or (b[i + 2].toInt() and 0xFF)
                        if (i + 3 + len > b.size) break
                        if (iei == 0x78 && len >= 1 && (b[i + 3].toInt() and 0xFF) == PAYLOAD_TEXT && text.isEmpty()) {
                            text = String(b, i + 4, len - 1, Charsets.UTF_8)
                        }
                        i += 3 + len
                    }
                }
                CT_INFO -> {
                    requestUri = Regex(
                        "<mcdata-request-uri[^>]*>\\s*<mcdataURI>([^<]+)</mcdataURI>",
                        RegexOption.DOT_MATCHES_ALL,
                    ).find(part.content)?.groupValues?.get(1)?.trim()
                    oneToOne = Regex("<request-type>\\s*([^<]+)</request-type>")
                        .find(part.content)?.groupValues?.get(1)?.trim()
                        ?.startsWith("one-to-one") == true
                }
            }
        }
        return when (msgType) {
            MSG_SDS_SIGNALLING -> SdsMessage(convId, msgId, time, dispositionReq, text, requestUri, oneToOne)
            MSG_FD_SIGNALLING -> FdMessage(convId, msgId, time, fileUrl, fileName, fileSize, fileType, requestUri, oneToOne)
            MSG_SDS_NOTIFICATION -> SdsNotification(convId, msgId, notifType)
            else -> null
        }
    }

    // ── 내부 ──

    private fun mcDataInfoXml(requestType: String, targetUri: String) = """
        <?xml version="1.0" encoding="UTF-8"?>
        <mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0">
          <mcdata-Params>
            <request-type>$requestType</request-type>
            <mcdata-request-uri type="Normal"><mcdataURI>$targetUri</mcdataURI></mcdata-request-uri>
          </mcdata-Params>
        </mcdatainfo>
    """.trimIndent()

    private fun StringBuilder.appendPart(boundary: String, ct: String, cte: String?, content: String) {
        append("--$boundary\r\n")
        append("Content-Type: $ct\r\n")
        if (cte != null) append("Content-Transfer-Encoding: $cte\r\n")
        append("\r\n")
        append(content)
        append("\r\n")
    }

    private class Part(val contentType: String, val base64: Boolean, val content: String) {
        fun bytes(): ByteArray = if (base64) {
            runCatching {
                java.util.Base64.getDecoder().decode(content.filterNot { it.isWhitespace() })
            }.getOrDefault(ByteArray(0))
        } else content.toByteArray(Charsets.ISO_8859_1)
    }

    private fun boundaryOf(contentType: String?): String? {
        contentType ?: return null
        Regex("boundary=\"?([^\";\\r\\n]+)\"?", RegexOption.IGNORE_CASE).find(contentType)
            ?.let { return it.groupValues[1].trim() }
        // 본문 첫 줄 "--X" fallback
        return if (contentType.startsWith("--")) contentType.removePrefix("--").trim().ifEmpty { null } else null
    }

    private fun splitParts(body: String, boundary: String): List<Part> {
        val out = mutableListOf<Part>()
        for (raw in body.split("--$boundary")) {
            val chunk = raw.removePrefix("\r\n")
            if (chunk.startsWith("--")) break
            val sep = chunk.indexOf("\r\n\r\n").let { if (it >= 0) it to 4 else chunk.indexOf("\n\n") to 2 }
            if (sep.first < 0) continue
            val headers = chunk.substring(0, sep.first).lowercase()
            val ct = Regex("content-type:\\s*([^;\\r\\n]+)").find(headers)?.groupValues?.get(1)?.trim() ?: continue
            val b64 = headers.contains("content-transfer-encoding: base64") ||
                headers.contains("content-transfer-encoding:base64")
            out += Part(ct, b64, chunk.substring(sep.first + sep.second).trimEnd('\r', '\n'))
        }
        return out
    }

    private fun ByteBuffer.putDateTime(sec: Long) {
        put(((sec shr 32) and 0xFF).toByte()); put(((sec shr 24) and 0xFF).toByte())
        put(((sec shr 16) and 0xFF).toByte()); put(((sec shr 8) and 0xFF).toByte())
        put((sec and 0xFF).toByte())
    }

    private fun readDateTime(b: ByteArray, off: Int): Long {
        var v = 0L
        for (i in 0 until 5) v = (v shl 8) or (b[off + i].toLong() and 0xFF)
        return v
    }

    private fun b64(b: ByteArray): String = java.util.Base64.getEncoder().encodeToString(b)

    private fun hex(u: UUID): String {
        val bb = ByteBuffer.allocate(16)
        bb.putLong(u.mostSignificantBits); bb.putLong(u.leastSignificantBits)
        return hex(bb.array(), 0, 16)
    }

    private fun hex(b: ByteArray, off: Int, len: Int): String = buildString(len * 2) {
        for (i in off until off + len) append("%02x".format(b[i]))
    }

    private fun hexToBytes(s: String): ByteArray =
        ByteArray(16) { i ->
            if (i * 2 + 1 < s.length) ((Character.digit(s[i * 2], 16) shl 4) or Character.digit(s[i * 2 + 1], 16)).toByte()
            else 0
        }
}
