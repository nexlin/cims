package com.cims.ue.ptt.mcdata.msrp

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.ServerSocket
import kotlin.concurrent.thread

/** MsrpSession loopback 검증 — cmdp 수신 동작(청크별 200, Success-Report 시 REPORT)을 흉내낸 피어. */
class MsrpSessionTest {

    /** 서버 피어: SEND 청크마다 200 응답, 마지막 청크가 Success-Report 면 REPORT 송신. */
    private fun runPeer(server: ServerSocket, received: MutableList<MsrpCodec.Frame>) {
        server.accept().use { sock ->
            val ins = sock.getInputStream()
            val out = sock.getOutputStream()
            val parser = MsrpCodec.Parser()
            val rx = ByteArray(65536)
            var lastMsgId = ""
            var wantMore = true
            while (wantMore) {
                val n = ins.read(rx)
                if (n <= 0) break
                for (f in parser.feed(rx, 0, n)) {
                    received += f
                    if (f.method != "SEND") continue
                    out.write(MsrpCodec.buildResponse(f.tid, 200, "OK",
                        f.header("from-path").orEmpty(), f.header("to-path").orEmpty()))
                    lastMsgId = f.header("message-id").orEmpty()
                    if (f.contFlag == '$' && f.header("success-report") == "yes") {
                        val tid = MsrpCodec.newTid()
                        // cmdp MsrpBuildReport 와 동일 wire — bodiless 는 마지막 헤더 CRLF 가 프레이밍 겸용
                        out.write(("MSRP $tid REPORT\r\nTo-Path: ${f.header("from-path")}\r\n" +
                            "From-Path: ${f.header("to-path")}\r\nMessage-ID: $lastMsgId\r\n" +
                            "Status: 000 200 OK\r\n-------$tid\$\r\n").toByteArray())
                        wantMore = false
                    }
                    out.flush()
                }
            }
        }
    }

    @Test fun sendSingleChunkMessage() {
        ServerSocket(0).use { server ->
            val received = ArrayList<MsrpCodec.Frame>()
            val peer = thread { runPeer(server, received) }
            val toPath = "msrp://127.0.0.1:${server.localPort}/srv;tcp"
            MsrpSession(toPath, "msrp://127.0.0.1:2855/cli;tcp").use { s ->
                s.connect()
                assertTrue(s.sendMessage("m1", "application/vnd.3gpp.mcdata-signalling",
                    byteArrayOf(0x01, 0x02, 0x03)))
                assertTrue(s.sendMessage("m2", "application/vnd.3gpp.mcdata-payload",
                    "본문 텍스트".toByteArray(), successReport = true))
            }
            peer.join(5000)
            val sends = received.filter { it.method == "SEND" }
            assertEquals(2, sends.size)
            assertArrayEquals(byteArrayOf(0x01, 0x02, 0x03), sends[0].body)
            assertArrayEquals("본문 텍스트".toByteArray(), sends[1].body)
            assertEquals("yes", sends[1].header("Success-Report"))
        }
    }

    @Test fun sendChunkedMessageReassembly() {
        ServerSocket(0).use { server ->
            val received = ArrayList<MsrpCodec.Frame>()
            val peer = thread { runPeer(server, received) }
            val toPath = "msrp://127.0.0.1:${server.localPort}/srv;tcp"
            val body = ByteArray(40_000) { (it % 251).toByte() }   // 16KB 청크 3개
            MsrpSession(toPath, "msrp://127.0.0.1:2855/cli;tcp", chunkSize = 16 * 1024).use { s ->
                s.connect()
                assertTrue(s.sendMessage("big", "application/vnd.3gpp.mcdata-payload", body,
                    successReport = true))
            }
            peer.join(5000)
            val sends = received.filter { it.method == "SEND" }
            assertEquals(3, sends.size)
            assertEquals(listOf('+', '+', '$'), sends.map { it.contFlag })
            // Byte-Range 연속성 + 재조립 = 원본
            assertEquals("1-16384/40000", sends[0].header("Byte-Range"))
            assertEquals("16385-32768/40000", sends[1].header("Byte-Range"))
            assertEquals("32769-40000/40000", sends[2].header("Byte-Range"))
            val reassembled = sends.fold(ByteArray(0)) { acc, f -> acc + f.body }
            assertArrayEquals(body, reassembled)
            // Message-ID 동일(서버 조립 키) + Content-Type 청크마다 반복(cmdp acceptChunk 계약)
            assertTrue(sends.all { it.header("Message-ID") == "big" })
            assertTrue(sends.all { it.header("Content-Type") == "application/vnd.3gpp.mcdata-payload" })
        }
    }

    @Test fun receiveMessageAssemblesServerChunks() {
        ServerSocket(0).use { server ->
            // cmdp 송신 레그 흉내: bodiless 바인딩 SEND 수신→200, 이후 청크 2개('+','$') 송신, 200 수신
            val body = "수신 본문 ".repeat(2000).toByteArray()   // multi-chunk 크기
            val ct = "multipart/mixed;boundary=cmdp-test1"
            val acks = ArrayList<Int>()
            val peer = thread {
                server.accept().use { sock ->
                    val ins = sock.getInputStream()
                    val out = sock.getOutputStream()
                    val parser = MsrpCodec.Parser()
                    val rx = ByteArray(65536)
                    // 1) 바인딩 SEND 대기 → 200
                    var bind: MsrpCodec.Frame? = null
                    while (bind == null) {
                        val n = ins.read(rx); if (n <= 0) return@thread
                        bind = parser.feed(rx, 0, n).firstOrNull { it.method == "SEND" }
                    }
                    out.write(MsrpCodec.buildResponse(bind.tid, 200, "OK",
                        bind.header("from-path").orEmpty(), bind.header("to-path").orEmpty()))
                    out.flush()
                    // 2) 16KB 청크 스트리밍 (cmdp nextSendChunk 동일 규칙) — 청크별 200 대기
                    var off = 0
                    while (off < body.size) {
                        val n2 = minOf(16 * 1024, body.size - off)
                        val last = off + n2 >= body.size
                        val tid = MsrpCodec.newTid()
                        out.write(MsrpCodec.buildSendChunk(tid,
                            bind.header("from-path").orEmpty(), bind.header("to-path").orEmpty(),
                            "srvmsg", ct, body.copyOfRange(off, off + n2),
                            off + 1L, (off + n2).toLong(), body.size.toLong(),
                            flag = if (last) '$' else '+'))
                        out.flush()
                        var resp: MsrpCodec.Frame? = null
                        while (resp == null) {
                            val n3 = ins.read(rx); if (n3 <= 0) return@thread
                            resp = parser.feed(rx, 0, n3).firstOrNull { it.isResponse && it.tid == tid }
                        }
                        acks.add(resp.statusCode)
                        off += n2
                    }
                }
            }
            val toPath = "msrp://127.0.0.1:${server.localPort}/srv;tcp"
            val received = MsrpSession(toPath, "msrp://127.0.0.1:2855/cli;tcp").use { s ->
                s.connect()
                s.receiveMessage()
            }
            peer.join(5000)
            assertTrue(received != null)
            assertEquals(ct, received!!.first)
            assertArrayEquals(body, received.second)
            assertTrue(acks.all { it == 200 })
        }
    }

    @Test fun non200ResponseFails() {
        ServerSocket(0).use { server ->
            val peer = thread {
                server.accept().use { sock ->
                    val ins = sock.getInputStream()
                    val out = sock.getOutputStream()
                    val parser = MsrpCodec.Parser()
                    val rx = ByteArray(8192)
                    val n = ins.read(rx)
                    for (f in parser.feed(rx, 0, n)) {
                        out.write(MsrpCodec.buildResponse(f.tid, 413, "Too Large",
                            f.header("from-path").orEmpty(), f.header("to-path").orEmpty()))
                        out.flush()
                    }
                }
            }
            val toPath = "msrp://127.0.0.1:${server.localPort}/srv;tcp"
            MsrpSession(toPath, "msrp://127.0.0.1:2855/cli;tcp").use { s ->
                s.connect()
                assertFalse(s.sendMessage("m1", "multipart/mixed", "x".toByteArray()))
            }
            peer.join(5000)
        }
    }
}
