// libcimsue 단위시험 — MCData SDS media plane(MSRP, TS 24.282 §9.2.3 · RFC 4975) (S1-UE-UNIT)
//   ① 프레이밍·SDP 도구 ② 엔진 발신 — 상한 초과 그룹 SDS → MSRP INVITE → 가짜 cmdp 에 SEND 2건 → onRequestResult(MSRP 200)
//   ③ 엔진 수신 — 서버발 배포 INVITE → 200(m=message setup:active recvonly, 오디오 inactive) → 바인딩 SEND → 청크 SEND 조립 → onSds
//   MSRP 호는 앱 호 목록(calls)에 나오지 않는다.
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "../src/account_map.h"
#include "../src/mcdata/msrp.h"
#include "../src/mcdata/sds_codec.h"
#include "cimsue/cimsue.h"
#include "pj_scope.h"

using namespace cimsue;

TEST(Msrp, FramingAndSdpTools) {
    // 청크 SEND — Byte-Range 는 1 기준, 계속 플래그
    std::string s = msrp::buildSend("t1", "msrp://a:2855/x;tcp", "msrp://b:2855/y;tcp", "m2", "application/vnd.3gpp.mcdata-payload",
                                    "HELLO", 10, 20, true, '+');
    EXPECT_NE(s.find("Byte-Range: 11-15/20\r\n"), std::string::npos);
    EXPECT_NE(s.find("Success-Report: yes"), std::string::npos);
    std::string buf = s + msrp::buildResponse("t2", 200, "msrp://b:2855/y;tcp", "msrp://a:2855/x;tcp") + "MSRP t3 SE";
    msrp::Frame f;
    ASSERT_TRUE(msrp::extractFrame(buf, f));
    EXPECT_TRUE(f.request); EXPECT_EQ(f.method, "SEND"); EXPECT_EQ(f.tid, "t1"); EXPECT_EQ(f.flag, '+');
    EXPECT_EQ(f.body, "HELLO");
    EXPECT_EQ(f.header("message-id"), "m2");
    ASSERT_TRUE(msrp::extractFrame(buf, f));
    EXPECT_FALSE(f.request); EXPECT_EQ(f.status, 200); EXPECT_TRUE(f.body.empty());
    EXPECT_FALSE(msrp::extractFrame(buf, f));                                 // 미완성은 남긴다
    EXPECT_EQ(buf, "MSRP t3 SE");

    std::string h; int p = 0;
    ASSERT_TRUE(msrp::parsePath("msrp://10.0.0.5:2856/abc;tcp msrp://relay/x;tcp", h, p));
    EXPECT_EQ(h, "10.0.0.5"); EXPECT_EQ(p, 2856);
    ASSERT_TRUE(msrp::parsePath("msrp://[2001:db8::1]:2900/s;tcp", h, p));
    EXPECT_EQ(h, "2001:db8::1"); EXPECT_EQ(p, 2900);
    EXPECT_FALSE(msrp::parsePath("sip:x@y", h, p));

    std::string sdp = "v=0\r\no=- 1 1 IN IP4 10.1.1.1\r\ns=-\r\nc=IN IP4 10.1.1.1\r\nt=0 0\r\nm=audio 4000 RTP/AVP 0\r\na=sendrecv\r\n"
                      "m=message 2855 TCP/MSRP *\r\na=path:msrp://10.2.2.2:2855/srv;tcp\r\na=setup:passive\r\n";
    EXPECT_EQ(msrp::pathOfSdp(sdp), "msrp://10.2.2.2:2855/srv;tcp");
    EXPECT_EQ(msrp::connAddrOf(sdp), "10.1.1.1");
    std::string ina = msrp::audioInactive(sdp);
    EXPECT_NE(ina.find("m=audio 4000 RTP/AVP 0\r\na=inactive\r\n"), std::string::npos);
    EXPECT_NE(ina.find("m=message"), std::string::npos);
    std::string sec = msrp::sdpSection("msrp://1.2.3.4:2855/s;tcp", "active", "recvonly");
    EXPECT_EQ(sec.rfind("m=message 2855 TCP/MSRP *\r\n", 0), 0u);
    EXPECT_NE(sec.find("a=setup:active\r\na=recvonly\r\n"), std::string::npos);
    EXPECT_NE(sec.find("a=accept-types:multipart/mixed"), std::string::npos);

    EXPECT_EQ(msrp::mcdataInfoUri("<mcdatainfo><mcdata-Params><mcdata-calling-user-id><mcdataURI>tel:+82500000014</mcdataURI>"
                                  "</mcdata-calling-user-id></mcdata-Params></mcdatainfo>", "mcdata-calling-user-id"), "tel:+82500000014");

    // REGISTER Contact ICSI 합치기(RFC 3840 — 한 태그 한 번)
    EXPECT_EQ(detail::withIcsi("", "urn%3Ax"), ";+g.3gpp.icsi-ref=\"urn%3Ax\"");
    EXPECT_EQ(detail::withIcsi(";+g.3gpp.icsi-ref=\"urn%3Aa\";video", "urn%3Ax"), ";+g.3gpp.icsi-ref=\"urn%3Aa,urn%3Ax\";video");
    EXPECT_EQ(detail::withIcsi(";+g.3gpp.icsi-ref=\"urn%3Ax\"", "urn%3Ax"), ";+g.3gpp.icsi-ref=\"urn%3Ax\"");
}

namespace {

struct MsrpListener : Listener {
    void onLog(int lv, const std::string& m) override { if (std::getenv("MSRP_LOG")) std::fprintf(stderr, "[%d] %s\n", lv, m.c_str()); }
    std::mutex m;
    std::condition_variable cv;
    std::vector<RequestResult> results;
    std::vector<SdsMessage> sds;
    int incoming = 0, callStates = 0;
    void onRequestResult(const RequestResult& r) override { { std::lock_guard<std::mutex> lk(m); results.push_back(r); } cv.notify_all(); }
    void onSds(const SdsMessage& s) override { { std::lock_guard<std::mutex> lk(m); sds.push_back(s); } cv.notify_all(); }
    void onIncomingCall(const CallInfo&) override { std::lock_guard<std::mutex> lk(m); incoming++; }
    void onCallState(const CallInfo&) override { std::lock_guard<std::mutex> lk(m); callStates++; }
    template <class P> bool wait(P pred, int ms = 5000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pred(); });
    }
};

std::string headerOf(const std::string& msg, const std::string& name) {
    size_t p = 0;
    while ((p = msg.find("\r\n", p)) != std::string::npos) {
        p += 2;
        if (msg.compare(p, name.size() + 1, name + ":") == 0) {
            size_t v = p + name.size() + 1, e = msg.find("\r\n", v);
            while (v < e && msg[v] == ' ') ++v;
            return msg.substr(v, e - v);
        }
    }
    return "";
}

std::string sipBodyOf(const std::string& m) {
    size_t p = m.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : m.substr(p + 4);
}

std::string uriIn(const std::string& h) {
    size_t a = h.find('<'), b = h.find('>');
    return a == std::string::npos || b == std::string::npos ? h : h.substr(a + 1, b - a - 1);
}

/** SIP 자리(UDP) — CSP 역할. */
struct FakeSip {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    pj_sockaddr_in peer;
    FakeSip() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s);
        pj_sockaddr_in a;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&a, &ip, 0);
        pj_sock_bind(s, &a, sizeof(a));
        int l = sizeof(a);
        pj_sock_getsockname(s, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeSip() { if (s != PJ_INVALID_SOCKET) pj_sock_close(s); }
    std::string recv(const std::string& prefix, int ms = 3000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds; PJ_FD_ZERO(&fds); PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char buf[16384];
            pj_ssize_t n = sizeof(buf) - 1;
            int al = sizeof(peer);
            if (pj_sock_recvfrom(s, buf, &n, 0, &peer, &al) != PJ_SUCCESS || n <= 0) continue;
            std::string msg(buf, (size_t)n);
            if (msg.compare(0, prefix.size(), prefix) == 0) return msg;
        }
        return "";
    }
    void send(const std::string& m) { pj_ssize_t len = (pj_ssize_t)m.size(); pj_sock_sendto(s, m.data(), &len, 0, &peer, sizeof(peer)); }
    void sendTo(int toPort, const std::string& m) {
        pj_sockaddr_in to;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)toPort);
        peer = to;
        send(m);
    }
    void reply(const std::string& req, int code, const char* reason, const std::string& ct = "", const std::string& body = "") {
        std::string r = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
        size_t p = 0;
        while ((p = req.find("\r\nVia:", p)) != std::string::npos) {
            size_t e = req.find("\r\n", p + 2);
            r += req.substr(p + 2, e - p - 2) + "\r\n";
            p = e;
        }
        std::string to = headerOf(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=srv";
        r += "From: " + headerOf(req, "From") + "\r\nTo: " + to + "\r\nCall-ID: " + headerOf(req, "Call-ID") + "\r\nCSeq: " +
             headerOf(req, "CSeq") + "\r\nContact: <sip:srv@127.0.0.1:" + std::to_string(port) + ">\r\n";
        if (!ct.empty()) r += "Content-Type: " + ct + "\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(r);
    }
};

/** cmdp 자리(TCP, passive) — 연결 하나를 받아 프레임을 주고받는다. */
struct FakeCmdp {
    pj_sock_t ls = PJ_INVALID_SOCKET, c = PJ_INVALID_SOCKET;
    int port = 0;
    std::string buf;
    FakeCmdp() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, &ls);
        pj_sockaddr_in a;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&a, &ip, 0);
        pj_sock_bind(ls, &a, sizeof(a));
        pj_sock_listen(ls, 4);
        int l = sizeof(a);
        pj_sock_getsockname(ls, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeCmdp() { if (c != PJ_INVALID_SOCKET) pj_sock_close(c); if (ls != PJ_INVALID_SOCKET) pj_sock_close(ls); }
    std::string path() const { return "msrp://127.0.0.1:" + std::to_string(port) + "/srv1;tcp"; }
    bool accept(int ms = 5000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds; PJ_FD_ZERO(&fds); PJ_FD_SET(ls, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)ls + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            return pj_sock_accept(ls, &c, nullptr, nullptr) == PJ_SUCCESS;
        }
        return false;
    }
    bool frame(msrp::Frame& f, int ms = 5000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (msrp::extractFrame(buf, f)) return true;
            if (std::chrono::steady_clock::now() >= end) return false;
            pj_fd_set_t fds; PJ_FD_ZERO(&fds); PJ_FD_SET(c, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)c + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char tmp[65536];
            pj_ssize_t n = sizeof(tmp);
            if (pj_sock_recv(c, tmp, &n, 0) != PJ_SUCCESS || n <= 0) return false;
            buf.append(tmp, (size_t)n);
        }
    }
    void send(const std::string& d) { pj_ssize_t n = (pj_ssize_t)d.size(); pj_sock_send(c, d.data(), &n, 0); }
};

std::string answerSdp(const FakeCmdp& cm, const char* setup, const char* dir) {
    return "v=0\r\no=- 7 7 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 9 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=inactive\r\n"
           "m=message " + std::to_string(cm.port) + " TCP/MSRP *\r\na=path:" + cm.path() + "\r\na=accept-types:" + msrp::kAcceptTypes +
           "\r\na=setup:" + setup + "\r\na=" + dir + "\r\n";
}

AccountConfig account(int sipPort) {
    AccountConfig ac;
    ac.serverHost = "127.0.0.1"; ac.serverPort = sipPort; ac.transport = Transport::UDP;
    ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
    return ac;
}

}  // namespace

TEST(Msrp, EngineSendsLargeGroupSdsOverMediaPlane) {
    Engine eng;
    MsrpListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("MSRP_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("msrp-test");
        FakeSip sip;
        FakeCmdp cmdp;
        AccountConfig ac = account(sip.port);
        ac.maxSdsCplaneBytes = 10;
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        const std::string text(40000, 'x');                                // 청크 3개(16 KB)
        SdsSend sent = eng.sendGroupSds(acc, "g005", text);
        ASSERT_TRUE(sent.ok) << sent.reason;
        std::string inv = sip.recv("INVITE ");
        ASSERT_FALSE(inv.empty());
        EXPECT_EQ(inv.rfind("INVITE sip:g005@ptt.test", 0), 0u);
        EXPECT_NE(headerOf(inv, "Accept-Contact").find("icsi.mcdata.sds\";require;explicit"), std::string::npos);
        EXPECT_EQ(headerOf(inv, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds");
        EXPECT_NE(inv.find("m=message 2855 TCP/MSRP *"), std::string::npos);
        EXPECT_NE(inv.find("a=setup:actpass\r\na=sendonly"), std::string::npos);
        const std::string uePath = msrp::pathOfSdp(inv);
        ASSERT_FALSE(uePath.empty());

        sip.reply(inv, 200, "OK", "application/sdp", answerSdp(cmdp, "passive", "recvonly"));
        ASSERT_FALSE(sip.recv("ACK ").empty());
        ASSERT_TRUE(cmdp.accept());
        msrp::Frame f;
        ASSERT_TRUE(cmdp.frame(f));                                          // ① signalling
        EXPECT_EQ(f.method, "SEND");
        EXPECT_EQ(f.header("Content-Type"), "application/vnd.3gpp.mcdata-signalling");
        EXPECT_EQ(f.header("From-Path"), uePath);
        EXPECT_EQ(f.header("To-Path"), cmdp.path());
        cmdp.send(msrp::buildResponse(f.tid, 200, uePath, cmdp.path()));
        std::string payload;
        for (int i = 0; i < 8; ++i) {                                        // ② payload 청크 — stop-and-wait
            ASSERT_TRUE(cmdp.frame(f));
            EXPECT_EQ(f.header("Content-Type"), "application/vnd.3gpp.mcdata-payload");
            EXPECT_EQ(f.header("Success-Report"), "yes");
            payload += f.body;
            cmdp.send(msrp::buildResponse(f.tid, 200, uePath, cmdp.path()));
            if (f.flag == '$') break;
            EXPECT_EQ(f.flag, '+');
        }
        EXPECT_EQ(payload, mcdata::sdsPayloadTlv(text));
        cmdp.send("MSRP r1 REPORT\r\nTo-Path: " + uePath + "\r\nFrom-Path: " + cmdp.path() + "\r\nMessage-ID: m2\r\nStatus: 000 200 OK\r\n-------r1$\r\n");
        ASSERT_TRUE(l.wait([&] { return !l.results.empty(); }));
        EXPECT_EQ(l.results[0].method, "MSRP");
        EXPECT_EQ(l.results[0].token, sent.token);
        EXPECT_EQ(l.results[0].code, 200);
        EXPECT_TRUE(eng.calls().empty());                                    // 앱 호 목록 밖
        EXPECT_EQ(l.callStates, 0);
        // 서버가 BYE 하지 않으면 코어가 끊는다(5 s)
        std::string bye = sip.recv("BYE ", 8000);
        ASSERT_FALSE(bye.empty());
        sip.reply(bye, 200, "OK");

        // 상한 아래는 그대로 시그널링 평면(MESSAGE) — 호출자가 준 message ID(재전송)가 본문에 실린다
        const std::string given = "0123456789abcdef0123456789ABCDEF";
        SdsSend small = eng.sendGroupSds(acc, "g005", "hi", true, given);
        ASSERT_TRUE(small.ok);
        EXPECT_EQ(small.msgId, "0123456789abcdef0123456789abcdef");
        std::string msg = sip.recv("MESSAGE ");
        ASSERT_FALSE(msg.empty());
        SdsMessage parsed;
        ASSERT_TRUE(mcdata::parse(headerOf(msg, "Content-Type"), sipBodyOf(msg), parsed));
        EXPECT_EQ(parsed.msgId, small.msgId);
        sip.reply(msg, 200, "OK");
        EXPECT_FALSE(eng.sendGroupSds(acc, "g005", "hi", true, "not-hex").ok);
    }
    eng.stop();
}

TEST(Msrp, EngineReceivesServerDeliveryOverMediaPlane) {
    Engine eng;
    MsrpListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("MSRP_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("msrp-test");
        FakeSip sip;
        FakeCmdp cmdp;
        AccountConfig ac = account(sip.port);
        ac.mcdataMsrp = true;
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);
        // 엔진 주소를 배운다 — 등록을 보내게 해서 REGISTER 의 Contact 로
        ASSERT_TRUE(eng.registerAccount(acc).ok);
        std::string reg = sip.recv("REGISTER ");
        ASSERT_FALSE(reg.empty());
        EXPECT_NE(headerOf(reg, "Contact").find("+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\""), std::string::npos);
        sip.reply(reg, 200, "OK");
        const std::string ue = uriIn(headerOf(reg, "Contact"));

        // 서버발 배포 INVITE — multipart(mcdata-info + SDP: 더미 오디오 + m=message sendonly passive)
        std::string info = "<?xml version=\"1.0\"?><mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\"><mcdata-Params>"
                           "<request-type>group-sds</request-type><mcdata-request-uri><mcdataURI>tel:g005</mcdataURI></mcdata-request-uri>"
                           "<mcdata-calling-user-id><mcdataURI>tel:+82500000014</mcdataURI></mcdata-calling-user-id></mcdata-Params></mcdatainfo>";
        std::string offer = "v=0\r\no=- 3 3 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 9 RTP/AVP 0 8\r\na=rtpmap:0 PCMU/8000\r\n"
                            "a=rtpmap:8 PCMA/8000\r\na=sendonly\r\nm=message " + std::to_string(cmdp.port) + " TCP/MSRP *\r\na=path:" + cmdp.path() +
                            "\r\na=accept-types:" + msrp::kAcceptTypes + "\r\na=setup:passive\r\na=sendonly\r\n";
        std::string body = "--b1\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n" + info + "\r\n--b1\r\nContent-Type: application/sdp\r\n\r\n" +
                           offer + "\r\n--b1--\r\n";
        std::string inv = "INVITE " + ue + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(sip.port) + ";branch=z9hG4bKmsrp1\r\n"
                          "Max-Forwards: 70\r\nFrom: <sip:g005@ptt.test>;tag=d1\r\nTo: <sip:+82500000001@ptt.test>\r\nCall-ID: msrp-dl-1\r\n"
                          "CSeq: 1 INVITE\r\nContact: <sip:srv@127.0.0.1:" + std::to_string(sip.port) + ">\r\n"
                          "Accept-Contact: *;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\";require;explicit\r\n"
                          "Content-Type: multipart/mixed;boundary=b1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        sip.send(inv);
        std::string ok = sip.recv("SIP/2.0 200");
        ASSERT_FALSE(ok.empty());
        EXPECT_NE(ok.find("a=setup:active\r\na=recvonly"), std::string::npos);
        EXPECT_EQ(ok.find("m=message 0 "), std::string::npos);                 // pjsua 의 포트 0 섹션을 교체했다
        EXPECT_NE(ok.find("a=inactive"), std::string::npos);
        const std::string uePath = msrp::pathOfSdp(sipBodyOf(ok));
        ASSERT_FALSE(uePath.empty());
        std::string ack = "ACK " + uriIn(headerOf(ok, "Contact")) + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(sip.port) +
                          ";branch=z9hG4bKmsrpack\r\nMax-Forwards: 70\r\nFrom: " + headerOf(ok, "From") + "\r\nTo: " + headerOf(ok, "To") +
                          "\r\nCall-ID: msrp-dl-1\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n";
        sip.send(ack);

        ASSERT_TRUE(cmdp.accept());
        msrp::Frame f;
        ASSERT_TRUE(cmdp.frame(f));                                          // 바인딩(bodiless)
        EXPECT_EQ(f.method, "SEND");
        EXPECT_TRUE(f.header("Content-Type").empty());
        EXPECT_EQ(f.header("From-Path"), uePath);
        // 재전달 = multipart(signalling + payload, 날 TLV) 두 청크
        const std::string text = "대용량 SDS 본문";
        const std::string msg = "--c1\r\nContent-Type: " + std::string(mcdata::kCtSignalling) + "\r\n\r\n" +
                                mcdata::sdsSignallingTlv(mcdata::conversationIdOf("g005"), mcdata::newMessageId(), true, 1790000000) +
                                "\r\n--c1\r\nContent-Type: " + mcdata::kCtPayload + "\r\n\r\n" + mcdata::sdsPayloadTlv(text) + "\r\n--c1--\r\n";
        const size_t half = msg.size() / 2;
        cmdp.send(msrp::buildSend("s1", uePath, cmdp.path(), "d1", "multipart/mixed;boundary=c1", msg.substr(0, half), 0, msg.size(), false, '+'));
        ASSERT_TRUE(cmdp.frame(f));
        EXPECT_EQ(f.status, 200);
        cmdp.send(msrp::buildSend("s2", uePath, cmdp.path(), "d1", "multipart/mixed;boundary=c1", msg.substr(half), half, msg.size(), false, '$'));
        ASSERT_TRUE(cmdp.frame(f));
        EXPECT_EQ(f.status, 200);
        ASSERT_TRUE(l.wait([&] { return !l.sds.empty(); }));
        EXPECT_EQ(l.sds[0].text, text);
        EXPECT_TRUE(l.sds[0].mediaPlane);
        EXPECT_EQ(l.sds[0].fromUri, "tel:+82500000014");
        EXPECT_EQ(l.sds[0].groupUri, "tel:g005");
        EXPECT_EQ(l.sds[0].dispositionReq & 1, 1);
        EXPECT_EQ(l.incoming, 0);                                            // 통화 착신이 아니다
        EXPECT_TRUE(eng.calls().empty());
        // 서버 BYE
        std::string bye = "BYE " + uriIn(headerOf(ok, "Contact")) + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(sip.port) +
                          ";branch=z9hG4bKmsrpbye\r\nMax-Forwards: 70\r\nFrom: " + headerOf(ok, "From") + "\r\nTo: " + headerOf(ok, "To") +
                          "\r\nCall-ID: msrp-dl-1\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n";
        sip.send(bye);
        EXPECT_FALSE(sip.recv("SIP/2.0 200").empty());
    }
    eng.stop();
}
