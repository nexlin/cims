package com.cims.ue.ptt.mcdata.msrp

import java.security.SecureRandom

/**
 * MSRP(RFC 4975) 프레임 코덱 — MCData SDS over media plane(TS 24.282 §9.2.3)용 부분집합.
 *
 * 순수 JVM(안드로이드 비의존, 유닛테스트 대상). 서버(cmdp)측 PMsrpParser 와 대칭:
 *  - SEND 청크 빌드(Byte-Range, end-line `$`/`+`/`#`), 응답/REPORT 파싱
 *  - 본문 raw 바이너리(TLV) — C-plane 의 base64 CTE 편차 없음
 *  - 릴레이(RFC 4976)·TLS(MSRPS) 없음
 */
object MsrpCodec {

    /** 파싱된 MSRP 프레임. [method]=null 이면 응답([statusCode]). 헤더 키는 소문자 정규화. */
    data class Frame(
        val tid: String,
        val method: String?,
        val statusCode: Int,
        val headers: Map<String, String>,
        val body: ByteArray,
        val contFlag: Char,          // '$' 완결 / '+' 후속 청크 / '#' 발신측 중단
    ) {
        fun header(name: String): String? = headers[name.lowercase()]
        val isResponse: Boolean get() = method == null

        override fun equals(other: Any?): Boolean = this === other
        override fun hashCode(): Int = System.identityHashCode(this)
    }

    private val rnd = SecureRandom()
    private const val TID_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

    /** 트랜잭션 ID 발행. 본문에 end-line 유사열이 있으면 호출자가 재발행(RFC 4975 §7.1). */
    fun newTid(len: Int = 10): String = buildString(len) {
        repeat(len) { append(TID_CHARS[rnd.nextInt(TID_CHARS.length)]) }
    }

    /** [body] 안에 [tid] 의 end-line 유사열(`\r\n-------tid`)이 있는지 — tid 재발행 판단. */
    fun bodyCollides(body: ByteArray, tid: String): Boolean =
        indexOf(body, "\r\n-------$tid".toByteArray(Charsets.US_ASCII), 0, body.size) >= 0

    /**
     * SEND 청크 빌드. [contentType]=null 이면 bodiless(연결 바인딩용). Byte-Range 는 1-기반
     * ([rangeStart]-[rangeEnd]/[total]). 마지막 청크만 flag `$`, 후속 있으면 `+`.
     */
    fun buildSendChunk(
        tid: String,
        toPath: String,
        fromPath: String,
        msgId: String,
        contentType: String?,
        body: ByteArray,
        rangeStart: Long,
        rangeEnd: Long,
        total: Long,
        successReport: Boolean = false,
        flag: Char = '$',
    ): ByteArray {
        val h = buildString {
            append("MSRP $tid SEND\r\n")
            append("To-Path: $toPath\r\n")
            append("From-Path: $fromPath\r\n")
            append("Message-ID: $msgId\r\n")
            append("Byte-Range: $rangeStart-$rangeEnd/$total\r\n")
            if (successReport) append("Success-Report: yes\r\n")
            append("Failure-Report: yes\r\n")
            if (contentType != null) append("Content-Type: $contentType\r\n\r\n")
        }
        val head = h.toByteArray(Charsets.US_ASCII)
        // end-line 프레이밍 CRLF: 본문 있으면 본문 뒤에 별도 부가, bodiless 는 마지막 헤더의
        // CRLF 가 겸한다 (cmdp PMsrpParser·python 오라클과 동일 wire — 빈 줄 오인 방지)
        return if (contentType != null) head + body + "\r\n".toByteArray(Charsets.US_ASCII) + endLine(tid, flag)
        else head + endLine(tid, flag)
    }

    /** 응답 빌드(수신측 200 등) — PR4 수신 경로·테스트 피어용. */
    fun buildResponse(tid: String, code: Int, reason: String, toPath: String, fromPath: String): ByteArray =
        ("MSRP $tid $code $reason\r\nTo-Path: $toPath\r\nFrom-Path: $fromPath\r\n"
            .toByteArray(Charsets.US_ASCII)) + endLine(tid, '$')

    private fun endLine(tid: String, flag: Char): ByteArray =
        "-------$tid$flag\r\n".toByteArray(Charsets.US_ASCII)

    /**
     * 증분(binary-safe) 파서 — TCP 스트림 조각을 [feed] 로 밀어 넣으면 완성 프레임을 돌려준다.
     * 한 recv 에 여러 프레임/프레임 경계 분할 도착 모두 대응(잔여분 내부 보존).
     */
    class Parser {
        private var buf = ByteArray(0)

        fun feed(data: ByteArray, off: Int = 0, len: Int = data.size - off): List<Frame> {
            buf += data.copyOfRange(off, off + len)
            val out = ArrayList<Frame>()
            while (true) {
                val f = extract() ?: break
                out.add(f)
            }
            return out
        }

        private fun extract(): Frame? {
            val start = indexOf(buf, MAGIC, 0, buf.size)
            if (start < 0) return null
            val eol = indexOf(buf, CRLF, start, buf.size)
            if (eol < 0) return null
            val firstLine = String(buf, start, eol - start, Charsets.US_ASCII)
            val tokens = firstLine.split(' ')
            if (tokens.size < 3) {           // 훼손 라인 — 폐기 후 다음 프레임 탐색
                buf = buf.copyOfRange(eol + 2, buf.size)
                return extract()
            }
            val tid = tokens[1]
            // end-line: \r\n-------<tid><flag>\r\n
            val marker = "\r\n-------$tid".toByteArray(Charsets.US_ASCII)
            val e = indexOf(buf, marker, eol, buf.size)
            if (e < 0 || e + marker.size + 3 > buf.size) return null    // 미완 — 추가 수신 대기
            val flag = buf[e + marker.size].toInt().toChar()
            val frameEnd = e + marker.size + 3

            val statusCode = tokens[2].toIntOrNull()
            val method = if (statusCode == null) tokens[2] else null

            // 헤더/본문 분리 — 빈 줄이 end-line 앞에 있으면 본문 존재
            val blank = indexOf(buf, BLANK, eol, e)
            val headerEnd = if (blank in 0 until e) blank else e
            val headers = HashMap<String, String>()
            var p = eol + 2
            while (p < headerEnd) {
                val le = indexOf(buf, CRLF, p, headerEnd).let { if (it < 0) headerEnd else it }
                val line = String(buf, p, le - p, Charsets.US_ASCII)
                val c = line.indexOf(':')
                if (c > 0) headers[line.substring(0, c).trim().lowercase()] = line.substring(c + 1).trim()
                p = le + 2
            }
            val body = if (blank in 0 until e) buf.copyOfRange(blank + 4, e) else ByteArray(0)

            buf = buf.copyOfRange(frameEnd, buf.size)
            return Frame(tid, method, statusCode ?: 0, headers, body, flag)
        }

        private companion object {
            val MAGIC = "MSRP ".toByteArray(Charsets.US_ASCII)
            val CRLF = "\r\n".toByteArray(Charsets.US_ASCII)
            val BLANK = "\r\n\r\n".toByteArray(Charsets.US_ASCII)
        }
    }

    /** [pattern] 을 [from, to) 구간에서 탐색 — String 변환 없는 binary-safe indexOf. */
    fun indexOf(data: ByteArray, pattern: ByteArray, from: Int, to: Int): Int {
        if (pattern.isEmpty()) return from
        val limit = minOf(to, data.size) - pattern.size
        outer@ for (i in maxOf(from, 0)..limit) {
            for (j in pattern.indices) if (data[i + j] != pattern[j]) continue@outer
            return i
        }
        return -1
    }

    /** `msrp://host:port/session;tcp` → (host, port). 형식 오류면 null. */
    fun parsePath(path: String): Pair<String, Int>? {
        val m = Regex("msrp://([^:/]+):(\\d+)/").find(path) ?: return null
        return m.groupValues[1] to (m.groupValues[2].toIntOrNull() ?: return null)
    }

    /** 서버·앱 계약 SDP accept-types (cmdp 수용 타입과 일치). */
    const val ACCEPT_TYPES = "multipart/mixed application/vnd.3gpp.mcdata-signalling " +
        "application/vnd.3gpp.mcdata-payload"
}
