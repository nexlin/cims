package com.cims.ue.ptt.mcdata.msrp

import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * MSRP 송신 세션 — 서버(cmdp) a=path 로 TCP out-connect 후 SDS TLV 메시지를 SEND 한다.
 *
 * 방향성 계약: 서버는 항상 `a=setup:passive`(리슨), 단말이 항상 접속(NAT). 단말 a=path 는
 * 광고용(리슨 안 함)이므로 [fromPath] 는 문자열 정합만 맞으면 된다.
 *
 * **블로킹 소켓** — Dispatchers.IO 에서 호출할 것. 청크는 stop-and-wait(청크별 200 확인).
 * 순수 JVM(유닛테스트: loopback ServerSocket 피어).
 */
class MsrpSession(
    /** 서버 MSRP URI (200 OK answer 의 a=path) — To-Path·접속 대상. */
    private val toPath: String,
    /** 우리가 INVITE offer a=path 로 광고한 URI — From-Path. */
    private val fromPath: String,
    private val chunkSize: Int = 16 * 1024,
    private val ioTimeoutMs: Int = 10_000,
) : AutoCloseable {

    private var socket: Socket? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null
    private val parser = MsrpCodec.Parser()
    private val pending = ArrayDeque<MsrpCodec.Frame>()

    fun connect(connectTimeoutMs: Int = 5_000) {
        val (host, port) = MsrpCodec.parsePath(toPath) ?: error("MSRP path 형식 오류: $toPath")
        val s = Socket()
        s.tcpNoDelay = true
        s.soTimeout = ioTimeoutMs
        s.connect(InetSocketAddress(host, port), connectTimeoutMs)
        socket = s
        input = s.getInputStream()
        output = s.getOutputStream()
    }

    /**
     * 메시지 1건 송신 — [chunkSize] 청크 stop-and-wait, 청크마다 200 확인.
     * [successReport]=true 면 최종 청크 후 서버 REPORT 도 대기(미도착은 경고성 — 200 이 성공 기준).
     * [onProgress] 는 청크 200 수리 시마다 누적 송신 바이트로 호출(진행률 표시용).
     * [chunkDelayMs] > 0 이면 청크 사이 지연 — 진행률 UI 육안 확인용(디버그 속성으로만 활성).
     * @return 모든 청크가 200 으로 수리되면 true
     */
    fun sendMessage(
        msgId: String,
        contentType: String,
        body: ByteArray,
        successReport: Boolean = false,
        onProgress: ((sentBytes: Int) -> Unit)? = null,
        chunkDelayMs: Long = 0,
    ): Boolean {
        val out = output ?: error("connect() 선행 필요")
        var offset = 0
        while (offset < body.size || (body.isEmpty() && offset == 0)) {
            val n = minOf(chunkSize, body.size - offset)
            val last = offset + n >= body.size
            val chunk = body.copyOfRange(offset, offset + n)
            var tid = MsrpCodec.newTid()
            while (MsrpCodec.bodyCollides(chunk, tid)) tid = MsrpCodec.newTid()
            out.write(
                MsrpCodec.buildSendChunk(
                    tid, toPath, fromPath, msgId, contentType, chunk,
                    rangeStart = offset + 1L, rangeEnd = (offset + n).toLong(), total = body.size.toLong(),
                    successReport = successReport && last, flag = if (last) '$' else '+',
                ),
            )
            out.flush()
            val resp = awaitFrame { it.isResponse && it.tid == tid } ?: return false
            if (resp.statusCode != 200) return false
            offset += n
            onProgress?.invoke(offset)
            if (chunkDelayMs > 0 && !last) Thread.sleep(chunkDelayMs)
            if (body.isEmpty()) break
        }
        if (successReport) {
            // REPORT 는 요청 프레임(응답 금지, RFC 4975 §7.1.2) — 수신만 하고 소비
            awaitFrame { it.method == "REPORT" && it.header("message-id") == msgId }
        }
        return true
    }

    /**
     * 수신 세션(서버발 배포 레그) — bodiless SEND 로 연결을 세션에 바인딩한 뒤 서버(cmdp)의
     * SEND 청크를 수신·청크별 200 응답·Message-ID 단위 조립. 완결(`$`)까지 조립되면 반환.
     * @return (Content-Type, body) — 실패/타임아웃 시 null
     */
    fun receiveMessage(bindMsgId: String = "b0"): Pair<String, ByteArray>? {
        val out = output ?: error("connect() 선행 필요")
        out.write(MsrpCodec.buildSendChunk(MsrpCodec.newTid(), toPath, fromPath, bindMsgId,
            contentType = null, body = ByteArray(0), rangeStart = 1, rangeEnd = 0, total = 0))
        out.flush()
        var contentType = ""
        var curMsgId: String? = null
        var assembled = ByteArray(0)
        while (true) {
            val f = awaitFrame { it.method == "SEND" } ?: return null   // 바인딩 200 응답은 큐에 남고 무시
            val msgId = f.header("message-id")
            if (msgId != curMsgId) {
                curMsgId = msgId
                contentType = f.header("content-type").orEmpty()
                assembled = ByteArray(0)
            }
            assembled += f.body
            out.write(MsrpCodec.buildResponse(f.tid, 200, "OK",
                f.header("from-path") ?: toPath, fromPath))
            out.flush()
            when (f.contFlag) {
                '$' -> return contentType to assembled
                '#' -> { curMsgId = null; assembled = ByteArray(0) }    // 발신측 중단 — 폐기
            }
        }
    }

    /** 다음 수신 프레임 중 [pred] 일치 프레임 대기(불일치 프레임은 큐 보존). 타임아웃 시 null. */
    fun awaitFrame(pred: (MsrpCodec.Frame) -> Boolean): MsrpCodec.Frame? {
        pending.firstOrNull(pred)?.let { pending.remove(it); return it }
        val ins = input ?: return null
        val rx = ByteArray(65536)
        while (true) {
            val n = try {
                ins.read(rx)
            } catch (e: SocketTimeoutException) {
                return null
            }
            if (n <= 0) return null
            for (f in parser.feed(rx, 0, n)) pending.addLast(f)
            pending.firstOrNull(pred)?.let { pending.remove(it); return it }
        }
    }

    override fun close() {
        runCatching { socket?.close() }
        socket = null; input = null; output = null
    }
}
