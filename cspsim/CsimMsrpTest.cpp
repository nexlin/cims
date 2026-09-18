// libcsim MCData SDS media plane(MSRP, RFC 4975) 단위시험 — 프레이밍 왕복(SEND/응답/REPORT·다중 프레임·부분 프레임·bodiless·end-line 플래그)
//   + 루프백 TCP 로 cmdp 를 흉내내어 발신 절차(signalling·payload SEND → 200/REPORT)와 수신 절차(bodiless 바인딩 → 서버 SEND → 200 →
//   multipart 본문을 McDataSds 파서로 종단)를 검증한다. SIP 스택 없이 127.0.0.1 TCP 만 쓴다. S1-UNIT-TESTER 의 네이티브 항목.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "McDataMsrp.h"
#include "McDataSds.h"

using namespace csim_msrp;

static bool g_all = true;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); g_all = g_all && ok; }

static int listenLoopback(int& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) { close(fd); return -1; }
    socklen_t n = sizeof(a);
    getsockname(fd, (sockaddr*)&a, &n);
    port = ntohs(a.sin_port);
    return fd;
}

/** 서버 쪽 프레임 읽기(버퍼 유지) */
static bool srvRecv(int fd, std::string& buf, Frame& f, int timeoutMs) {
    if (extractFrame(buf, f)) return true;
    char tmp[65536];
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        if (extractFrame(buf, f)) return true;
    }
}
static void srvSend(int fd, const std::string& s) { send(fd, s.data(), s.size(), MSG_NOSIGNAL); }

int main() {
    // ── 프레이밍 ──
    {
        std::string s = buildSend("tid1", "msrp://1.2.3.4:2855/abc;tcp", "msrp://5.6.7.8:2855/def;tcp", "m1", "application/vnd.3gpp.mcdata-payload", "hello", true);
        Frame f;
        std::string buf = s;
        check(extractFrame(buf, f) && f.request && f.method == "SEND" && f.tid == "tid1", "SEND frame parses (request/method/tid)");
        check(f.header("To-Path") == "msrp://1.2.3.4:2855/abc;tcp" && f.header("message-id") == "m1" && f.header("Byte-Range") == "1-5/5", "headers (case-insensitive)");
        check(f.body == "hello" && f.flag == '$' && f.header("Success-Report") == "yes", "body·end-line flag·Success-Report");
        check(buf.empty(), "buffer consumed");
        // 두 프레임이 한 번에 + 뒤에 부분 프레임
        std::string two = buildResponse("tid1", 200, "msrp://a/1;tcp", "msrp://b/2;tcp") +
                          "MSRP rr1 REPORT\r\nTo-Path: msrp://a/1;tcp\r\nFrom-Path: msrp://b/2;tcp\r\nMessage-ID: m1\r\nByte-Range: 1-5/5\r\nStatus: 000 200 OK\r\n-------rr1$\r\n" +
                          "MSRP part SEND\r\nTo-Path: x\r\n";
        buf = two;
        Frame r1, r2, r3;
        check(extractFrame(buf, r1) && !r1.request && r1.status == 200 && r1.tid == "tid1", "response frame parses (status)");
        check(extractFrame(buf, r2) && r2.request && r2.method == "REPORT" && r2.header("Status") == "000 200 OK", "REPORT frame parses");
        check(!extractFrame(buf, r3) && buf.rfind("MSRP part SEND", 0) == 0, "partial frame stays in buffer");
        std::string bl = buildSend("b0", "msrp://a/1;tcp", "msrp://b/2;tcp", "b0", "", "", false);
        buf = bl;
        Frame fb;
        check(extractFrame(buf, fb) && fb.request && fb.header("Content-Type").empty() && fb.body.empty(), "bodiless SEND (binding) parses");
        std::string cont = buildSend("c1", "msrp://a/1;tcp", "msrp://b/2;tcp", "m2", "text/plain", "abc", false, '+');
        buf = cont;
        Frame fc;
        check(extractFrame(buf, fc) && fc.flag == '+' && fc.body == "abc", "continuation flag '+'");
        // 본문 안에 "MSRP " 가 있어도 end-line 기준으로 자른다
        std::string tricky = buildSend("t9", "msrp://a/1;tcp", "msrp://b/2;tcp", "m3", "text/plain", "x MSRP y\r\n-------zzz$\r\n", false);
        buf = tricky;
        Frame ft;
        check(extractFrame(buf, ft) && ft.body == "x MSRP y\r\n-------zzz$\r\n" && buf.empty(), "body containing MSRP/end-line-like text");
        std::string host; int port = 0;
        check(parsePath("msrp://10.0.2.48:2855/ab12;tcp msrp://relay/x;tcp", host, port) && host == "10.0.2.48" && port == 2855, "parsePath host:port (first URI)");
        check(parsePath("msrp://cmdp.local/ab12;tcp", host, port) && host == "cmdp.local" && port == 2855, "parsePath default port");
        check(localPath("10.0.0.5", "s1") == "msrp://10.0.0.5:2855/s1;tcp", "localPath");
    }

    // ── 루프백: cmdp 흉내 서버 ──
    int lport = 0;
    int lfd = listenLoopback(lport);
    check(lfd >= 0, "loopback listen");
    if (lfd < 0) return 1;
    const std::string serverPath = localPath("127.0.0.1", "srv1");
    const std::string convId = csim_mcdata::conversationIdOf("g001");
    const std::string msgId = csim_mcdata::newMessageId();
    std::string gotSig, gotPay;
    bool srvOk = false;
    // 발신 절차 상대: signalling SEND → 200, payload SEND(Success-Report) → 200 + REPORT
    std::thread srv([&] {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) return;
        std::string buf;
        Frame f;
        for (int i = 0; i < 2; ++i) {
            if (!srvRecv(c, buf, f, 3000) || !f.request || f.method != "SEND") { close(c); return; }
            if (f.header("Content-Type") == csim_mcdata::kCtSignalling) gotSig = f.body;
            if (f.header("Content-Type") == csim_mcdata::kCtPayload) gotPay = f.body;
            srvSend(c, buildResponse(f.tid, 200, f.header("From-Path"), serverPath));
            if (f.header("Success-Report") == "yes")
                srvSend(c, "MSRP rp1 REPORT\r\nTo-Path: " + f.header("From-Path") + "\r\nFrom-Path: " + serverPath + "\r\nMessage-ID: " + f.header("Message-ID") +
                            "\r\nByte-Range: 1-" + std::to_string(f.body.size()) + "/" + std::to_string(f.body.size()) + "\r\nStatus: 000 200 OK\r\n-------rp1$\r\n");
        }
        srvOk = true;
        usleep(200 * 1000);
        close(c);
    });
    {
        Client cli;
        check(cli.Connect("127.0.0.1", lport, 2000), "client connect");
        const std::string lp = localPath("127.0.0.1", "cli1");
        std::string t1 = newTransId(), t2 = newTransId();
        check(t1.size() == 10 && t1 != t2, "transaction ids");
        check(cli.Send(buildSend(t1, serverPath, lp, "m1", csim_mcdata::kCtSignalling, csim_mcdata::signallingTlv(convId, msgId, true, 1700000000), false)), "send signalling");
        Frame f;
        check(cli.RecvFrame(f, 3000) && !f.request && f.tid == t1 && f.status == 200, "signalling → 200");
        check(cli.Send(buildSend(t2, serverPath, lp, "m2", csim_mcdata::kCtPayload, csim_mcdata::payloadTlv("hello media plane"), true)), "send payload");
        bool resp = false, report = false;
        for (int i = 0; i < 2 && cli.RecvFrame(f, 3000); ++i) {
            if (!f.request && f.tid == t2 && f.status == 200) resp = true;
            if (f.request && f.method == "REPORT") report = true;
        }
        check(resp && report, "payload → 200 + REPORT");
        cli.Close();
    }
    srv.join();
    check(srvOk && gotSig.size() == 39 && (unsigned char)gotSig[0] == 0x01 && (unsigned char)gotSig[38] == 0x81, "server got raw SDS SIGNALLING TLV (+delivery)");
    check(gotPay.size() == 6 + strlen("hello media plane") && (unsigned char)gotPay[0] == 0x03, "server got raw DATA PAYLOAD TLV");
    // 서버가 조립한 multipart 를 McDataSds 파서가 읽는다(cmdp buildCombinedBody 와 같은 꼴)
    {
        std::string b = "--cmdp-x\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\n\r\n" + gotSig +
                        "\r\n--cmdp-x\r\nContent-Type: application/vnd.3gpp.mcdata-payload\r\n\r\n" + gotPay + "\r\n--cmdp-x--\r\n";
        csim_mcdata::SdsMsg m;
        check(csim_mcdata::parse("multipart/mixed;boundary=cmdp-x", b, m) && m.msgId == msgId && m.convId == convId && m.text == "hello media plane" && m.dispositionReq == 1,
              "combined multipart (raw TLV parts) parses to the same msg/conv/text/disposition");
    }

    // 수신 절차 상대: 클라이언트 bodiless SEND → 서버가 multipart SEND(2 청크 '+' '$') → 200 두 번
    std::string recvBody, recvCt;
    bool srv2Ok = false;
    std::thread srv2([&] {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) return;
        std::string buf;
        Frame f;
        if (!srvRecv(c, buf, f, 3000) || !f.request || !f.header("Content-Type").empty()) { close(c); return; }
        std::string body = "--cmdp-y\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\n\r\n" + gotSig +
                           "\r\n--cmdp-y\r\nContent-Type: application/vnd.3gpp.mcdata-payload\r\n\r\n" + gotPay + "\r\n--cmdp-y--\r\n";
        size_t half = body.size() / 2;
        std::string tA = newTransId(), tB = newTransId();
        // 청크 1('+') — Byte-Range 는 참고용, 수신자는 Message-ID 로 이어 붙인다
        srvSend(c, "MSRP " + tA + " SEND\r\nTo-Path: " + f.header("From-Path") + "\r\nFrom-Path: " + serverPath + "\r\nMessage-ID: mm1\r\nByte-Range: 1-" +
                       std::to_string(half) + "/" + std::to_string(body.size()) + "\r\nContent-Type: multipart/mixed;boundary=cmdp-y\r\n\r\n" + body.substr(0, half) +
                       "\r\n-------" + tA + "+\r\n");
        srvSend(c, "MSRP " + tB + " SEND\r\nTo-Path: " + f.header("From-Path") + "\r\nFrom-Path: " + serverPath + "\r\nMessage-ID: mm1\r\nByte-Range: " +
                       std::to_string(half + 1) + "-" + std::to_string(body.size()) + "/" + std::to_string(body.size()) + "\r\nContent-Type: multipart/mixed;boundary=cmdp-y\r\n\r\n" +
                       body.substr(half) + "\r\n-------" + tB + "$\r\n");
        int ok200 = 0;
        for (int i = 0; i < 2 && srvRecv(c, buf, f, 3000); ++i) if (!f.request && f.status == 200) ++ok200;
        srv2Ok = ok200 == 2;
        close(c);
    });
    {
        Client cli;
        check(cli.Connect("127.0.0.1", lport, 2000), "receiver connect");
        const std::string lp = localPath("127.0.0.1", "cli2");
        cli.Send(buildSend(newTransId(), serverPath, lp, "b0", "", "", false));
        Frame f;
        for (int i = 0; i < 4; ++i) {
            if (!cli.RecvFrame(f, 3000)) break;
            if (!f.request || f.method != "SEND") continue;
            cli.Send(buildResponse(f.tid, 200, f.header("From-Path"), lp));
            recvCt = f.header("Content-Type");
            recvBody += f.body;
            if (f.flag == '$') break;
        }
        cli.Close();
        csim_mcdata::SdsMsg m;
        check(csim_mcdata::parse(recvCt, recvBody, m) && m.msgId == msgId && m.text == "hello media plane", "receiver reassembles 2 chunks and parses SDS");
    }
    srv2.join();
    check(srv2Ok, "receiver answered 200 to each chunk");
    close(lfd);
    printf("%s\n", g_all ? "PASS" : "FAIL");
    return g_all ? 0 : 1;
}
