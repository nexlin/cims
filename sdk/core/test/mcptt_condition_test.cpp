// libcimsue 단위시험 — MCPTT 세션 조건(긴급·임박)과 긴급 경보 (S1-UE-UNIT)
//   헤드리스 엔진 + 루프백 가짜 서버(UDP). 그룹콜을 세운 뒤
//   ① 상향 re-INVITE 403 → Denied(이전 값 복원, 호 유지 — TS 24.379 §6.2.8.1.5·§6.3.3.1.14)
//   ② 상향 re-INVITE 200 → Confirmed(+ Resource-Priority·mcptt-info emergency-ind — §10.1.1.2.1.3)
//   ③ 서버 하향 재광고 re-INVITE → Advertised(§10.1.1.2.1.6)
//   ④ 경보 MESSAGE 발신 본문·헤더(§12.1.1.1)와 수신 → onEmergencyAlert(§12.1.1.3)
//   ⑦ 임박 상향의 2xx 에 Warning 149 → 확정을 INFO 로 미룬다(§6.2.8.1.4 2)·§6.2.8.1.13)
//   ⑧ 재광고 re-INVITE 의 alert-ind false + originated-by → onEmergencyAlert(§10.1.1.2.1.6 3)b))
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/mcptt/mcptt_xml.h"
#include "cimsue/cimsue.h"
#include "cimsue/csc.h"
#include "pj_scope.h"

using namespace cimsue;

namespace {

struct CondListener : Listener {
    void onLog(int lv, const std::string& m) override { if (std::getenv("COND_LOG")) std::fprintf(stderr, "[%d] %s\n", lv, m.c_str()); }
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<CallInfo, ConditionCause>> conds;
    std::vector<EmergencyAlert> alerts;
    std::vector<RequestResult> results;
    CallInfo lastState;
    void onCallState(const CallInfo& i) override { { std::lock_guard<std::mutex> lk(m); lastState = i; } cv.notify_all(); }
    void onMcpttCondition(const CallInfo& i, ConditionCause c) override {
        { std::lock_guard<std::mutex> lk(m); conds.emplace_back(i, c); }
        cv.notify_all();
    }
    void onEmergencyAlert(const EmergencyAlert& a) override { { std::lock_guard<std::mutex> lk(m); alerts.push_back(a); } cv.notify_all(); }
    void onRequestResult(const RequestResult& r) override { { std::lock_guard<std::mutex> lk(m); results.push_back(r); } cv.notify_all(); }
    std::vector<std::pair<std::string, size_t>> rosters;   // (groupId, 참가자 수)
    void onRoster(int, const std::string& gid, const std::vector<RosterEntry>& users, bool) override {
        { std::lock_guard<std::mutex> lk(m); rosters.emplace_back(gid, users.size()); }
        cv.notify_all();
    }
    template <class P> bool wait(P pred, int ms = 3000) {
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

std::string uriIn(const std::string& h) {                  // "<sip:…>;tag=x" → sip:…
    size_t a = h.find('<'), b = h.find('>');
    return a == std::string::npos || b == std::string::npos ? h : h.substr(a + 1, b - a - 1);
}

std::string sdp(int version) {
    return "v=0\r\no=- 1 " + std::to_string(version) + " IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
           "m=audio 40000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"
           "m=application 40002 udp MCPTT\r\na=fmtp:MCPTT mc_queueing\r\n";
}

/** 그룹콜 서버 자리 — INVITE 에 답하고, in-dialog 요청을 보낸다. */
struct FakeServer {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    pj_sockaddr_in peer;
    std::string callId, ueFrom, ueContact;                 // 첫 INVITE 에서 배운 dialog
    std::string contact;                                   // 응답 Contact(빈 값 = <sip:srv@…>) — 제어 기능의 세션 식별자 자리
    int cseq = 100;
    FakeServer() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s);
        pj_sockaddr_in a;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&a, &ip, 0);
        pj_sock_bind(s, &a, sizeof(a));
        int l = sizeof(a);
        pj_sock_getsockname(s, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeServer() { if (s != PJ_INVALID_SOCKET) pj_sock_close(s); }

    /** prefix 로 시작하는 메시지 한 건(다른 것은 건너뛴다). */
    std::string recv(const std::string& prefix, int ms = 3000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
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
    void send(const std::string& m) {
        pj_ssize_t len = (pj_ssize_t)m.size();
        pj_sock_sendto(s, m.data(), &len, 0, &peer, sizeof(peer));
    }
    void reply(const std::string& req, int code, const char* reason, const std::string& ct = "", const std::string& body = "",
               const std::string& extra = "") {
        std::string r = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n" + extra;
        size_t p = 0;
        while ((p = req.find("\r\nVia:", p)) != std::string::npos) {
            size_t e = req.find("\r\n", p + 2);
            r += req.substr(p + 2, e - p - 2) + "\r\n";
            p = e;
        }
        std::string to = headerOf(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=srv";
        r += "From: " + headerOf(req, "From") + "\r\nTo: " + to + "\r\nCall-ID: " + headerOf(req, "Call-ID") + "\r\n" +
             "CSeq: " + headerOf(req, "CSeq") + "\r\nContact: " +
             (contact.empty() ? "<sip:srv@127.0.0.1:" + std::to_string(port) + ">" : contact) + "\r\n";
        if (!ct.empty()) r += "Content-Type: " + ct + "\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(r);
    }
    /** 서버발 in-dialog re-INVITE — multipart(SDP + mcptt-info). */
    void reinvite(const std::string& mcpttInfo, int sdpVersion) {
        const std::string b = "b1";
        std::string body = "--" + b + "\r\nContent-Type: application/sdp\r\n\r\n" + sdp(sdpVersion) +
                           "--" + b + "\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n" + mcpttInfo + "\r\n--" + b + "--\r\n";
        std::string r = "INVITE " + ueContact + " SIP/2.0\r\n" +
                        "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(port) + ";branch=z9hG4bKsrv" + std::to_string(cseq) + "\r\n" +
                        "Max-Forwards: 70\r\nFrom: <sip:g001@ptt.test>;tag=srv\r\nTo: " + ueFrom + "\r\nCall-ID: " + callId + "\r\n" +
                        "CSeq: " + std::to_string(++cseq) + " INVITE\r\nContact: <sip:srv@127.0.0.1:" + std::to_string(port) + ">\r\n" +
                        "Content-Type: multipart/mixed;boundary=" + b + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(r);
    }
    /** 서버발 in-dialog INFO — Info Package g.3gpp.mcptt-info(RFC 6086, TS 24.379 §6.2.8.1.13). */
    void info(const std::string& mcpttInfo) {
        std::string r = "INFO " + ueContact + " SIP/2.0\r\n" +
                        "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(port) + ";branch=z9hG4bKinfo" + std::to_string(cseq) + "\r\n" +
                        "Max-Forwards: 70\r\nFrom: <sip:g001@ptt.test>;tag=srv\r\nTo: " + ueFrom + "\r\nCall-ID: " + callId + "\r\n" +
                        "CSeq: " + std::to_string(++cseq) + " INFO\r\nInfo-Package: g.3gpp.mcptt-info\r\n" +
                        "Content-Disposition: Info-Package\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n" +
                        "Content-Length: " + std::to_string(mcpttInfo.size()) + "\r\n\r\n" + mcpttInfo;
        send(r);
    }
    void ackFor(const std::string& resp) {                 // 2xx 에 대한 ACK(새 트랜잭션)
        std::string r = "ACK " + ueContact + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(port) +
                        ";branch=z9hG4bKack" + std::to_string(cseq) + "\r\nMax-Forwards: 70\r\n" +
                        "From: " + headerOf(resp, "From") + "\r\nTo: " + headerOf(resp, "To") + "\r\nCall-ID: " + callId +
                        "\r\nCSeq: " + std::to_string(cseq) + " ACK\r\nContent-Length: 0\r\n\r\n";
        send(r);
    }
};

}  // namespace

/** 제어 기능의 멤버 초대(TS 24.379 §6.3.3.1.2 — mcptt-info + SDP, Session-Expires 는 refresher 생략 6)) — UE 주소로 보낸다. */
static std::string memberInvite(int srvPort, int uePort, const std::string& callId, const std::string& sessionExpires,
                                const std::string& fromUser = "g001", const std::string& group = "g001") {
    const std::string b = "mb1";
    const std::string info = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params>"
                             "<session-type>prearranged</session-type><mcptt-calling-user-id>tel:+82500000002</mcptt-calling-user-id>"
                             + (group.empty() ? std::string() : "<mcptt-calling-group-id>tel:" + group + "</mcptt-calling-group-id>") + "</mcptt-Params></mcpttinfo>";
    const std::string sdpBody = "v=0\r\no=CSS 4 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                                "m=audio 40010 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"
                                "m=application 40012 UDP MCPTT\r\na=floorid:0 mstrm:audio\r\na=fmtp:MCPTT mc_queueing;mc_priority=3\r\n";
    const std::string body = "--" + b + "\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n" + info + "\r\n--" + b +
                             "\r\nContent-Type: application/sdp\r\n\r\n" + sdpBody + "--" + b + "--\r\n";
    const std::string ue = "sip:+82500000001@127.0.0.1:" + std::to_string(uePort);
    return "INVITE " + ue + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srvPort) + ";branch=z9hG4bK" + callId + "\r\n" +
           "Max-Forwards: 70\r\nFrom: <sip:" + fromUser + "@ptt.test>;tag=srv-" + callId + "\r\nTo: <" + ue + ">\r\nCall-ID: " + callId + "\r\n" +
           "CSeq: 1 INVITE\r\nContact: <sip:g001@127.0.0.1:" + std::to_string(srvPort) + ";gr=s1>;+g.3gpp.mcptt;isfocus\r\n" +
           "Supported: timer\r\nSession-Expires: " + sessionExpires + "\r\nMin-SE: 90\r\n" +
           "Accept-Contact: *;+g.3gpp.mcptt;require;explicit\r\nP-Asserted-Service: urn:urn-7:3gpp-service.ims.icsi.mcptt\r\n" +
           "Content-Type: multipart/mixed;boundary=" + b + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

static int countHeader(const std::string& msg, const std::string& name) {
    int n = 0;
    for (size_t p = msg.find("\r\n" + name + ":"); p != std::string::npos; p = msg.find("\r\n" + name + ":", p + 2)) ++n;
    return n;
}

// 멤버 초대 자동 수락의 200 OK — 갱신 주체 = 단말(TS 24.379 §6.2.3.1.1 5)·§6.2.3.1.2: 초대가 refresher 를 정하지 않으면 uas) +
//   Require: timer(2)) 한 줄. 초대가 refresher 를 정했으면(옛 서버의 uac) 그 값을 따른다(RFC 4028 §9 Table 2).
TEST(McpttInvite, MemberInvitationRefresherUas) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    {
        cimsue_test::PjScope pj("inv-port");
        FakeServer probe;                                                     // 빈 포트 하나를 UE 포트로
        cfg.udpPort = probe.port;
    }
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("inv-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        ac.instanceId = "urn:uuid:00000000-0000-4000-8000-000000000001";
        ASSERT_GE(eng.addAccount(ac), 0);
        struct Case { const char* se; const char* want; };
        int n = 0;
        for (const Case& c : {Case{"1800", "1800;refresher=uas"}, Case{"1800;refresher=uac", "1800;refresher=uac"}}) {
            const std::string cid = "mi-" + std::to_string(++n);
            const std::string inv = memberInvite(srv.port, cfg.udpPort, cid, c.se);
            srv.peer = pj_sockaddr_in();
            pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
            pj_sockaddr_in_init(&srv.peer, &ip, (pj_uint16_t)cfg.udpPort);
            srv.send(inv);
            std::string ok = srv.recv("SIP/2.0 200");
            ASSERT_FALSE(ok.empty()) << c.se;
            EXPECT_EQ(headerOf(ok, "Session-Expires"), c.want) << ok;
            EXPECT_NE(headerOf(ok, "Require").find("timer"), std::string::npos) << ok;
            EXPECT_EQ(countHeader(ok, "Require"), 1) << ok;
            srv.callId = cid;
            srv.ueContact = uriIn(headerOf(ok, "Contact"));
            srv.cseq = 1;
            srv.ackFor(ok);
            std::string bye = "BYE " + srv.ueContact + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srv.port) +
                              ";branch=z9hG4bKbye" + cid + "\r\nMax-Forwards: 70\r\nFrom: " + headerOf(ok, "From") + "\r\nTo: " +
                              headerOf(ok, "To") + "\r\nCall-ID: " + cid + "\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n";
            srv.send(bye);
            ASSERT_FALSE(srv.recv("SIP/2.0 200").empty()) << "BYE " << c.se;
        }
        // 착신 그룹 = <mcptt-calling-group-id>(§10.1.1.4.1.1 4)b)) — From 이 제어 기능 PSI 여도 그 그룹의 세션이다. 요소가 없으면 From 의 user
        struct G { const char* from; const char* group; const char* want; };
        for (const G& g : {G{"mcptt_psi", "g009", "g009"}, G{"g007", "", "g007"}}) {
            const std::string cid = std::string("gi-") + g.want;
            srv.send(memberInvite(srv.port, cfg.udpPort, cid, "1800", g.from, g.group));
            std::string ok = srv.recv("SIP/2.0 200");
            ASSERT_FALSE(ok.empty()) << g.want;
            ASSERT_TRUE(l.wait([&] { return l.lastState.groupId == g.want; })) << "group " << l.lastState.groupId;
            // 초대 Contact(isfocus)의 세션 식별자(§6.3.3.1.2 1)) — 나갔다가 재합류할 때 Request-URI(§10.1.1.2.4.1)
            EXPECT_EQ(l.lastState.sessionUri, "sip:g001@127.0.0.1:" + std::to_string(srv.port) + ";gr=s1");
            srv.callId = cid;
            srv.ueContact = uriIn(headerOf(ok, "Contact"));
            srv.cseq = 1;
            srv.ackFor(ok);
            std::string bye = "BYE " + srv.ueContact + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srv.port) +
                              ";branch=z9hG4bKbye" + cid + "\r\nMax-Forwards: 70\r\nFrom: " + headerOf(ok, "From") + "\r\nTo: " +
                              headerOf(ok, "To") + "\r\nCall-ID: " + cid + "\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n";
            srv.send(bye);
            ASSERT_FALSE(srv.recv("SIP/2.0 200").empty()) << "BYE " << g.want;
        }
    }
    eng.stop();
}

TEST(McpttCondition, UpgradeDeniedConfirmedAndAdvertised) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("cond-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        ac.instanceId = "urn:uuid:00000000-0000-4000-8000-000000000001";
        ac.mcdataServerUri = "sip:mcdata_psi@ptt.test";                     // ue-init-config MCData-Service-Details/Server-URI
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        int callId = eng.joinGroupCall(acc, "g001");
        ASSERT_GE(callId, 0);
        std::string inv = srv.recv("INVITE ");
        ASSERT_FALSE(inv.empty());
        EXPECT_EQ(headerOf(inv, "Resource-Priority"), "");                 // 일반 그룹콜은 싣지 않는다
        srv.callId = headerOf(inv, "Call-ID");
        srv.ueFrom = headerOf(inv, "From");
        srv.ueContact = uriIn(headerOf(inv, "Contact"));
        srv.reply(inv, 200, "OK", "application/sdp", sdp(1));
        ASSERT_FALSE(srv.recv("ACK ").empty());
        ASSERT_TRUE(l.wait([&] { return l.lastState.state == CallState::Active; }));
        EXPECT_FALSE(eng.callInfo(callId).condition.emergency);

        // ① 상향 → 403 + mcptt-info(emergency-ind false) — Local 뒤 Denied, 호는 그대로
        ASSERT_TRUE(eng.setCallCondition(callId, true, false).ok);
        EXPECT_FALSE(eng.setCallCondition(callId, false, false).ok);        // 응답 대기 중
        std::string up1 = srv.recv("INVITE ");
        ASSERT_FALSE(up1.empty());
        EXPECT_NE(up1.find("<emergency-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></emergency-ind>"), std::string::npos);   // Annex F.1 contentType
        EXPECT_EQ(headerOf(up1, "Resource-Priority"), "mcpttp.15");
        EXPECT_NE(up1.find("m=application"), std::string::npos);            // floor 섹션 재주입
        srv.reply(up1, 403, "Forbidden", "application/vnd.3gpp.mcptt-info+xml",
                  "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params><emergency-ind>false</emergency-ind>"
                  "</mcptt-Params></mcpttinfo>");
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= 2; }));
        EXPECT_EQ(l.conds[0].second, ConditionCause::Local);
        EXPECT_TRUE(l.conds[0].first.condition.emergency);
        EXPECT_TRUE(l.conds[0].first.condition.pending);
        EXPECT_EQ(l.conds[1].second, ConditionCause::Denied);
        EXPECT_FALSE(l.conds[1].first.condition.emergency);
        EXPECT_FALSE(l.conds[1].first.condition.mine);
        EXPECT_EQ(l.conds[1].first.condition.lastCode, 403);
        EXPECT_EQ(eng.callInfo(callId).state, CallState::Active);

        // ② 다시 상향 → 200 — Confirmed, mine
        ASSERT_TRUE(eng.setCallCondition(callId, true, false).ok);
        std::string up2 = srv.recv("INVITE ");
        ASSERT_FALSE(up2.empty());
        srv.reply(up2, 200, "OK", "application/sdp", sdp(2));
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= 4; }));
        EXPECT_EQ(l.conds[3].second, ConditionCause::Confirmed);
        EXPECT_TRUE(l.conds[3].first.condition.emergency);
        EXPECT_TRUE(l.conds[3].first.condition.mine);
        EXPECT_FALSE(l.conds[3].first.condition.pending);
        EXPECT_TRUE(eng.setCallCondition(callId, true, false).ok);          // 바뀐 것 없음 — 보내지 않는다
        EXPECT_FALSE(eng.setCallCondition(callId, true, true).ok);          // 긴급·임박 동시 불가

        // ③ 서버 하향 재광고(권한자 취소 전파) — Advertised, 조건 해제
        srv.reinvite("<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params><session-type>prearranged</session-type>"
                     "<emergency-ind>false</emergency-ind></mcptt-Params></mcpttinfo>", 3);
        std::string ok = srv.recv("SIP/2.0 200");
        ASSERT_FALSE(ok.empty());
        srv.ackFor(ok);
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= 5; }));
        EXPECT_EQ(l.conds[4].second, ConditionCause::Advertised);
        EXPECT_FALSE(l.conds[4].first.condition.emergency);
        EXPECT_FALSE(l.conds[4].first.condition.mine);
        EXPECT_FALSE(eng.callInfo(callId).condition.emergency);

        // ③b 임박 상향 → 임박 → 긴급 상향 — 지시자 조합(TS 24.379 §6.3.3.1.17): 임박은 긴급·경보 지시자 없이, 긴급은 alert-ind 를
        //   동반하고 imminentperil-ind 는 싣지 않는다(§6.2.8.1.1 4) — 임박은 제어 기능이 내린다, §6.3.3.1.6 3)d))
        std::this_thread::sleep_for(std::chrono::milliseconds(300));       // ③ 의 ACK 처리 전엔 pjsua 가 새 re-INVITE 를 거절한다
        ASSERT_TRUE(eng.setCallCondition(callId, false, true).ok);
        std::string ip = srv.recv("INVITE ");
        ASSERT_FALSE(ip.empty());
        EXPECT_NE(ip.find("<imminentperil-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></imminentperil-ind>"), std::string::npos);
        EXPECT_EQ(ip.find("emergency-ind"), std::string::npos);
        EXPECT_EQ(ip.find("alert-ind"), std::string::npos);
        EXPECT_EQ(headerOf(ip, "Resource-Priority"), "mcpttp.8");
        srv.reply(ip, 200, "OK", "application/sdp", sdp(4));
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= 7; }));
        EXPECT_EQ(l.conds[6].second, ConditionCause::Confirmed);
        EXPECT_TRUE(l.conds[6].first.condition.imminentPeril);
        ASSERT_TRUE(eng.setCallCondition(callId, true, false).ok);
        std::string ie = srv.recv("INVITE ");
        ASSERT_FALSE(ie.empty());
        EXPECT_NE(ie.find("<emergency-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></emergency-ind>"), std::string::npos);
        EXPECT_NE(ie.find("<alert-ind type=\"Normal\"><mcpttBoolean>false</mcpttBoolean></alert-ind>"), std::string::npos);
        EXPECT_EQ(ie.find("imminentperil-ind"), std::string::npos);
        EXPECT_EQ(headerOf(ie, "Resource-Priority"), "mcpttp.15");
        srv.reply(ie, 200, "OK", "application/sdp", sdp(5));
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= 9; }));
        EXPECT_EQ(l.conds[8].second, ConditionCause::Confirmed);
        EXPECT_TRUE(l.conds[8].first.condition.emergency);
        EXPECT_FALSE(l.conds[8].first.condition.imminentPeril);
        EXPECT_FALSE(eng.setCallCondition(callId, false, true).ok);         // 긴급 중 임박 상향 불가(§6.2.8.1.9 1))
        // 긴급 해제 — emergency-ind false 만
        ASSERT_TRUE(eng.setCallCondition(callId, false, false).ok);
        std::string ce = srv.recv("INVITE ");
        ASSERT_FALSE(ce.empty());
        EXPECT_NE(ce.find("<emergency-ind type=\"Normal\"><mcpttBoolean>false</mcpttBoolean></emergency-ind>"), std::string::npos);
        EXPECT_EQ(ce.find("imminentperil-ind"), std::string::npos);
        EXPECT_EQ(headerOf(ce, "Resource-Priority"), "mcpttp.0");
        srv.reply(ce, 200, "OK", "application/sdp", sdp(6));
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= 11; }));
        EXPECT_FALSE(l.conds[10].first.condition.emergency);

        // ④ 경보 발신 — ICSI 헤더·mcptt-client-id(instanceId 가 urn:uuid:)·Request-URI = 그룹
        int64_t tok = eng.sendEmergencyAlert(acc, "g001", true);
        ASSERT_GE(tok, 0);
        std::string msg = srv.recv("MESSAGE ");
        ASSERT_FALSE(msg.empty());
        EXPECT_EQ(msg.rfind("MESSAGE sip:g001@ptt.test ", 0), 0u);
        EXPECT_EQ(headerOf(msg, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcptt");
        EXPECT_NE(headerOf(msg, "Accept-Contact").find("require;explicit"), std::string::npos);
        EXPECT_NE(headerOf(msg, "Content-Type").find("vnd.3gpp.mcptt-info+xml"), std::string::npos);
        EXPECT_NE(msg.find("<alert-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></alert-ind>"), std::string::npos);
        EXPECT_NE(msg.find("<mcptt-client-id type=\"Normal\"><mcpttString>urn:uuid:00000000-0000-4000-8000-000000000001</mcpttString></mcptt-client-id>"), std::string::npos);
        srv.reply(msg, 200, "OK");
        ASSERT_TRUE(l.wait([&] { return !l.results.empty(); }));
        EXPECT_EQ(l.results[0].token, tok);
        EXPECT_EQ(l.results[0].code, 200);

        // ⑤ 경보 수신(서버 팬아웃 = 원본 본문 — calling-group-id 없음) → onEmergencyAlert
        std::string abody = "<?xml version=\"1.0\"?><mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params>"
                            "<mcptt-request-uri>tel:g001</mcptt-request-uri><mcptt-calling-user-id>tel:+82500000014</mcptt-calling-user-id>"
                            "<alert-ind>true</alert-ind></mcptt-Params></mcpttinfo>";
        std::string in = "MESSAGE " + srv.ueContact + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srv.port) +
                         ";branch=z9hG4bKmsg1\r\nMax-Forwards: 70\r\nFrom: <sip:+82500000014@ptt.test>;tag=m1\r\n"
                         "To: <sip:+82500000001@ptt.test>\r\nCall-ID: alert-1\r\nCSeq: 1 MESSAGE\r\n"
                         "Content-Type: application/vnd.3gpp.mcptt-info+xml\r\nContent-Length: " + std::to_string(abody.size()) +
                         "\r\n\r\n" + abody;
        srv.send(in);
        ASSERT_FALSE(srv.recv("SIP/2.0 200").empty());
        ASSERT_TRUE(l.wait([&] { return !l.alerts.empty(); }));
        EXPECT_EQ(l.alerts[0].groupId, "g001");
        EXPECT_EQ(l.alerts[0].userId, "+82500000014");
        EXPECT_EQ(l.alerts[0].alertInd, 1);
        EXPECT_FALSE(l.alerts[0].self);

        // ⑥ SDS disposition 통지 규격형(TS 24.282 §12.2.1.1·§6.2.4.1) — Request-URI = 참여 MCData 기능 PSI
        SdsSend dn = eng.sendSdsNotification(acc, "tel:+82500000014", std::string(32, 'a'), std::string(32, 'b'), 2, "tel:g001");
        ASSERT_TRUE(dn.ok) << dn.reason;
        std::string nm = srv.recv("MESSAGE ");
        ASSERT_FALSE(nm.empty());
        EXPECT_EQ(nm.rfind("MESSAGE sip:mcdata_psi@ptt.test ", 0), 0u);
        const std::string ac2 = headerOf(nm, "Accept-Contact");
        EXPECT_NE(ac2.find("+g.3gpp.mcdata.sds;require;explicit"), std::string::npos);
        EXPECT_NE(ac2.find("3gpp-service.ims.icsi.mcdata.sds\";require;explicit"), std::string::npos);
        EXPECT_EQ(headerOf(nm, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds");
        EXPECT_NE(nm.find("<entry uri=\"tel:+82500000014\"/>"), std::string::npos);
        EXPECT_NE(nm.find("<mcdataURI>tel:g001</mcdataURI></mcdata-calling-group-id>"), std::string::npos);
        srv.reply(nm, 200, "OK");

        // ⑦ 임박 상향 → 200 + Warning 149(«SIP INFO request pending») — 임박으로 확정하지 않는다(TS 24.379 §6.2.8.1.4 2)).
        //   뒤따르는 INFO 가 imminentperil-ind false + emergency-ind true 면 그룹이 긴급 중이다(§6.2.8.1.13 3)a)) — 내 임박은 서지 않았다.
        const size_t n0 = l.conds.size();
        ASSERT_TRUE(eng.setCallCondition(callId, false, true).ok);
        std::string ip2 = srv.recv("INVITE ");
        ASSERT_FALSE(ip2.empty());
        srv.reply(ip2, 200, "OK", "application/sdp", sdp(7), "Warning: 399 srv \"149 SIP INFO request pending\"\r\n");
        ASSERT_FALSE(srv.recv("ACK ").empty());
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= n0 + 2; }));
        EXPECT_EQ(l.conds[n0 + 1].second, ConditionCause::Local) << "149 — Confirmed 가 아니다";
        EXPECT_TRUE(l.conds[n0 + 1].first.condition.pending);
        srv.info("<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params><emergency-ind>true</emergency-ind>"
                 "<imminentperil-ind>false</imminentperil-ind></mcptt-Params></mcpttinfo>");
        ASSERT_FALSE(srv.recv("SIP/2.0 200").empty());                      // INFO 200
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= n0 + 3; }));
        EXPECT_EQ(l.conds[n0 + 2].second, ConditionCause::Advertised);
        EXPECT_TRUE(l.conds[n0 + 2].first.condition.emergency);
        EXPECT_FALSE(l.conds[n0 + 2].first.condition.imminentPeril);
        EXPECT_FALSE(l.conds[n0 + 2].first.condition.mine);
        EXPECT_FALSE(l.conds[n0 + 2].first.condition.pending);

        // ⑧ 재광고 re-INVITE — 긴급 해제 + 경보 취소(alert-ind false) + <originated-by> = 나(TS 24.379 §10.1.1.2.1.6 3)b)):
        //   세션 조건은 Advertised 로 내려가고, 경보 취소는 MESSAGE 수신과 같은 onEmergencyAlert 로 온다(내 경보의 제3자 취소).
        const size_t a0 = l.alerts.size();
        srv.reinvite("<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params><session-type>prearranged</session-type>"
                     "<mcptt-calling-user-id>tel:+82500000099</mcptt-calling-user-id><emergency-ind>false</emergency-ind>"
                     "<alert-ind>false</alert-ind><originated-by>tel:+82500000001</originated-by></mcptt-Params></mcpttinfo>", 8);
        std::string ok8 = srv.recv("SIP/2.0 200");
        ASSERT_FALSE(ok8.empty());
        srv.ackFor(ok8);
        ASSERT_TRUE(l.wait([&] { return l.conds.size() >= n0 + 4 && l.alerts.size() > a0; }));
        EXPECT_EQ(l.conds[n0 + 3].second, ConditionCause::Advertised);
        EXPECT_FALSE(l.conds[n0 + 3].first.condition.emergency);
        EXPECT_EQ(l.alerts[a0].alertInd, -1);
        EXPECT_EQ(l.alerts[a0].emergencyInd, -1);
        EXPECT_EQ(l.alerts[a0].groupId, "g001");                           // 본문에 그룹이 없으면 그 호의 그룹
        EXPECT_EQ(l.alerts[a0].userId, "+82500000099");                    // 취소한 사람
        EXPECT_EQ(l.alerts[a0].originatedBy, "+82500000001");              // 원 경보 발신자 = 나 → 앱이 내 경보를 내린다(MEA 1)
        EXPECT_FALSE(l.alerts[a0].self);

        eng.hangup(callId);
        std::string bye = srv.recv("BYE ");
        if (!bye.empty()) srv.reply(bye, 200, "OK");
    }
    eng.stop();
}

// 애드혹 그룹 호(TS 24.379 §17) — 개시 INVITE 의 session-type = adhoc(§17.2.2.1.1 10)a)), 개시자의 끝내기 = BYE + Reason(§17.2.3.1.1 1)).
//   편성 그룹 호의 BYE 에는 Reason 이 없다(나가기)
TEST(McpttAdhoc, SessionTypeAndReleaseReason) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("adhoc-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        auto establish = [&](const std::string& group, const GroupCallOptions& o) {
            int id = eng.joinGroupCall(acc, group, o);
            EXPECT_GE(id, 0);
            std::string inv = srv.recv("INVITE ");
            EXPECT_FALSE(inv.empty());
            srv.callId = headerOf(inv, "Call-ID");
            srv.ueFrom = headerOf(inv, "From");
            srv.ueContact = uriIn(headerOf(inv, "Contact"));
            srv.reply(inv, 200, "OK", "application/sdp", sdp(1));
            EXPECT_FALSE(srv.recv("ACK ").empty());
            EXPECT_TRUE(l.wait([&] { return l.lastState.callId == id && l.lastState.state == CallState::Active; }));
            return std::make_pair(id, inv);
        };

        GroupCallOptions adhoc;
        adhoc.members = {"tel:+82500000002", "tel:+82500000003"};
        auto a = establish("adhoc-82500000001-1790000000", adhoc);
        EXPECT_NE(a.second.find("<session-type>adhoc</session-type>"), std::string::npos) << a.second;
        // 전송 직전 보정 — speech 미디어의 i=speech(§6.2.1 2)d)), 고친 뒤에도 Content-Length = 본문 바이트
        {
            const size_t ma = a.second.find("\r\nm=audio ");
            ASSERT_NE(ma, std::string::npos);
            const size_t eol = a.second.find("\r\n", ma + 2);
            EXPECT_EQ(a.second.compare(eol + 2, 10, "i=speech\r\n"), 0) << a.second.substr(ma, 120);
            const size_t hb = a.second.find("\r\n\r\n");
            EXPECT_EQ((size_t)std::atoi(headerOf(a.second, "Content-Length").c_str()), a.second.size() - (hb + 4));
        }
        EXPECT_NE(a.second.find("resource-lists"), std::string::npos);
        ASSERT_TRUE(eng.hangup(a.first).ok);
        std::string bye = srv.recv("BYE ");
        ASSERT_FALSE(bye.empty());
        EXPECT_EQ(headerOf(bye, "Reason"), "SIP;cause=200;text=\"User requested release\"");
        srv.reply(bye, 200, "OK");
        ASSERT_TRUE(l.wait([&] { return l.lastState.callId == a.first && l.lastState.state == CallState::Disconnected; }));

        auto g = establish("g001", GroupCallOptions());
        EXPECT_NE(g.second.find("<session-type>prearranged</session-type>"), std::string::npos);
        ASSERT_TRUE(eng.hangup(g.first).ok);
        bye = srv.recv("BYE ");
        ASSERT_FALSE(bye.empty());
        EXPECT_EQ(headerOf(bye, "Reason"), "");                            // 편성 그룹 호 — 나가기
        srv.reply(bye, 200, "OK");
    }
    eng.stop();
}

// 개별 호 발신의 개시 방식 요청(TS 24.379 §11.1.1.2.1.1 14) — RFC 5373): 자동·수동 = Answer-Mode, 강제 자동 = Priv-Answer-Mode, 미지정 = 없음
TEST(McpttPrivate, CommencementModeHeaders) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("priv-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);
        struct Case { CommencementMode mode; const char* answerMode; const char* privAnswerMode; const char* peer; };
        const Case cases[] = {{CommencementMode::Unspecified, "", "", "+82500000002"},
                              {CommencementMode::Auto, "Auto", "", "+82500000003"},
                              {CommencementMode::Manual, "Manual", "", "+82500000004"},
                              {CommencementMode::ForceAuto, "", "Auto", "+82500000005"}};
        for (const Case& c : cases) {
            GroupCallOptions o;
            o.commencement = c.mode;
            int id = eng.startPrivateCall(acc, c.peer, o);
            ASSERT_GE(id, 0);
            std::string inv = srv.recv("INVITE ");
            ASSERT_FALSE(inv.empty());
            EXPECT_EQ(headerOf(inv, "Answer-Mode"), c.answerMode) << c.peer;
            EXPECT_EQ(headerOf(inv, "Priv-Answer-Mode"), c.privAnswerMode) << c.peer;
            EXPECT_NE(inv.find("<session-type>private</session-type>"), std::string::npos);
            srv.reply(inv, 403, "Forbidden");
            ASSERT_FALSE(srv.recv("ACK ").empty());
            ASSERT_TRUE(l.wait([&] { return l.lastState.callId == id && l.lastState.state == CallState::Disconnected; }));
        }
        GroupCallOptions g;                                                // 그룹 호에는 싣지 않는다(멤버 초대의 개시 방식은 제어 기능 몫)
        g.commencement = CommencementMode::Auto;
        int gid = eng.joinGroupCall(acc, "g001", g);
        ASSERT_GE(gid, 0);
        std::string ginv = srv.recv("INVITE ");
        ASSERT_FALSE(ginv.empty());
        EXPECT_EQ(headerOf(ginv, "Answer-Mode"), "");
        srv.reply(ginv, 403, "Forbidden");
        srv.recv("ACK ");
    }
    eng.stop();
}

// 단말이 여는 그룹 호의 INVITE(TS 24.379 §10.1.1.2.1.1) — 4) Contact = MCPTT 특성 태그 · 5)·6) Accept-Contact 둘(require;explicit) ·
//   7) P-Preferred-Service · 10) Request-URI = 참여 MCPTT 기능 PSI · 14) mcptt-info = session-type·그룹 ID·`<mcptt-client-id>`, 발신자 ID 없음
//   (NOTE 2) · chat 합류 session-type chat(§10.1.2.2.1.1 13)a)) · 제어 채널 `m=application <port> udp MCPTT`(TS 24.380 표 4.3.3.1-1).
//   PSI 를 모르는 계정은 Request-URI = 그룹 URI. 멤버 초대에 답하는 180·200 의 Contact 도 특성 태그(§6.2.3.1.1 3)·4)).
TEST(McpttGroupInvite, StandardRequestShape) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    {
        cimsue_test::PjScope pj("ginv-port");
        FakeServer probe;
        cfg.udpPort = probe.port;
    }
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("ginv-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        ac.instanceId = "urn:uuid:00000000-0000-4000-8000-000000000001";
        ac.mcpttServerUri = "sip:mcptt-psi@ptt.test";
        ac.mcpttEnabled = true;
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);
        const std::string tags = ";+g.3gpp.mcptt;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\"";

        auto invite = [&](int account, const std::string& group, const GroupCallOptions& o) {
            int id = eng.joinGroupCall(account, group, o);
            EXPECT_GE(id, 0);
            std::string inv = srv.recv("INVITE ");
            EXPECT_FALSE(inv.empty()) << group;
            srv.reply(inv, 403, "Forbidden");
            srv.recv("ACK ");
            EXPECT_TRUE(l.wait([&] { return l.lastState.callId == id && l.lastState.state == CallState::Disconnected; }));
            return inv;
        };

        std::string inv = invite(acc, "g001", GroupCallOptions());
        EXPECT_EQ(inv.compare(0, 37, "INVITE sip:mcptt-psi@ptt.test SIP/2.0"), 0) << inv.substr(0, 80);
        EXPECT_EQ(countHeader(inv, "Accept-Contact"), 2) << inv;
        EXPECT_NE(inv.find("\r\nAccept-Contact: *;+g.3gpp.mcptt;require;explicit\r\n"), std::string::npos) << inv;
        EXPECT_NE(inv.find("\r\nAccept-Contact: *;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit\r\n"),
                  std::string::npos) << inv;
        EXPECT_EQ(headerOf(inv, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcptt");
        EXPECT_NE(headerOf(inv, "Contact").find(tags), std::string::npos) << headerOf(inv, "Contact");
        EXPECT_NE(inv.find("<session-type>prearranged</session-type>"), std::string::npos);
        EXPECT_NE(inv.find("<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:g001</mcpttURI>"), std::string::npos) << inv;
        EXPECT_NE(inv.find("<mcptt-client-id type=\"Normal\"><mcpttString>urn:uuid:00000000-0000-4000-8000-000000000001</mcpttString>"),
                  std::string::npos) << inv;
        EXPECT_EQ(inv.find("mcptt-calling-user-id"), std::string::npos) << inv;
        EXPECT_NE(inv.find("\r\nm=application "), std::string::npos) << inv;
        EXPECT_NE(inv.find(" udp MCPTT\r\n", inv.find("\r\nm=application ")), std::string::npos) << inv;
        EXPECT_EQ(inv.find("a=floorid"), std::string::npos);

        GroupCallOptions chat;
        chat.chat = true;
        inv = invite(acc, "g002", chat);
        EXPECT_NE(inv.find("<session-type>chat</session-type>"), std::string::npos) << inv;
        EXPECT_EQ(l.lastState.mcptt.sessionType, "chat");                      // CallInfo 는 보낸 session-type 그대로
        EXPECT_NE(inv.find("<mcpttURI>tel:g002</mcpttURI>"), std::string::npos);

        // 개별 호(§11.1.1.2.1.1) — 1) Request-URI = PSI · 9) 착신자 = resource-lists · 14)c) mcptt-info = session-type private(대상·발신자 ID 없음).
        //   floor 없는 개별 호는 offer 에 m=application 을 싣지 않는다(§11.1.2.2)
        for (bool full : {false, true}) {
            GroupCallOptions po;
            po.fullDuplex = full;
            int pid = eng.startPrivateCall(acc, "+82500000002", po);
            ASSERT_GE(pid, 0);
            std::string pinv = srv.recv("INVITE ");
            ASSERT_FALSE(pinv.empty());
            EXPECT_EQ(pinv.compare(0, 37, "INVITE sip:mcptt-psi@ptt.test SIP/2.0"), 0) << pinv.substr(0, 80);
            EXPECT_NE(pinv.find("<session-type>private</session-type>"), std::string::npos);
            EXPECT_EQ(pinv.find("mcptt-request-uri"), std::string::npos) << pinv;
            EXPECT_EQ(pinv.find("mcptt-calling-user-id"), std::string::npos);
            EXPECT_NE(pinv.find("application/resource-lists+xml"), std::string::npos);
            EXPECT_NE(pinv.find("<entry uri=\"tel:+82500000002\""), std::string::npos) << pinv;
            EXPECT_EQ(countHeader(pinv, "Accept-Contact"), 2);
            EXPECT_EQ(pinv.find("m=application") == std::string::npos, full) << pinv;
            EXPECT_EQ(pinv.find("mc_no_floor_ctrl"), std::string::npos);
            srv.reply(pinv, 403, "Forbidden");
            srv.recv("ACK ");
            ASSERT_TRUE(l.wait([&] { return l.lastState.callId == pid && l.lastState.state == CallState::Disconnected; }));
        }

        AccountConfig old = ac;                                             // PSI 를 모르는 계정 — Request-URI = 그룹 URI
        old.mcpttServerUri.clear();
        int acc2 = eng.addAccount(old);
        ASSERT_GE(acc2, 0);
        inv = invite(acc2, "g003", GroupCallOptions());
        EXPECT_EQ(inv.compare(0, 31, "INVITE sip:g003@ptt.test SIP/2."), 0) << inv.substr(0, 80);
        EXPECT_EQ(countHeader(inv, "Accept-Contact"), 2);

        // 멤버 초대 — 180·200 의 Contact = 특성 태그(§6.2.3.1.1 3)·4))
        srv.peer = pj_sockaddr_in();
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&srv.peer, &ip, (pj_uint16_t)cfg.udpPort);
        srv.send(memberInvite(srv.port, cfg.udpPort, "mi-tags", "1800"));
        std::string ringing = srv.recv("SIP/2.0 180");
        ASSERT_FALSE(ringing.empty());
        EXPECT_NE(headerOf(ringing, "Contact").find(tags), std::string::npos) << headerOf(ringing, "Contact");
        std::string ok = srv.recv("SIP/2.0 200");
        ASSERT_FALSE(ok.empty());
        EXPECT_NE(headerOf(ok, "Contact").find(tags), std::string::npos) << headerOf(ok, "Contact");
        srv.callId = "mi-tags";
        srv.ueContact = uriIn(headerOf(ok, "Contact"));
        srv.cseq = 1;
        srv.ackFor(ok);
        std::string bye = "BYE " + srv.ueContact + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srv.port) +
                          ";branch=z9hG4bKbye-tags\r\nMax-Forwards: 70\r\nFrom: " + headerOf(ok, "From") + "\r\nTo: " + headerOf(ok, "To") +
                          "\r\nCall-ID: mi-tags\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n";
        srv.send(bye);
        ASSERT_FALSE(srv.recv("SIP/2.0 200").empty());
    }
    eng.stop();
}

// 재합류(TS 24.379 §10.1.1.2.4.1) — 개시 200 OK Contact(isfocus)의 세션 식별자(§4.5 · §6.3.3.2.3.2 5))를 CallInfo.sessionUri 로 배우고,
//   GroupCallOptions.sessionUri 로 다시 걸면 Request-URI·To = 세션 식별자, 나머지는 §10.1.1.2.1.1 그대로(session-type prearranged · 그룹 ID ·
//   client ID · Accept-Contact 둘 · P-Preferred-Service). 진행 중 세션 합류라 개시 속성(일제 통화·명단·chat)은 싣지 않는다.
//   끝난 세션은 서버가 404(§10.1.1.4.5.1 2)) — 호는 그대로 끝난다(새 세션을 열지 않는다)
TEST(McpttRejoin, SessionIdentityRequestUri) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("rejoin-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        ac.instanceId = "urn:uuid:00000000-0000-4000-8000-000000000001";
        ac.mcpttServerUri = "sip:mcptt-psi@ptt.test";
        ac.mcpttEnabled = true;
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        int id1 = eng.joinGroupCall(acc, "g001");
        ASSERT_GE(id1, 0);
        std::string inv = srv.recv("INVITE ");
        ASSERT_FALSE(inv.empty());
        EXPECT_EQ(inv.compare(0, 37, "INVITE sip:mcptt-psi@ptt.test SIP/2.0"), 0) << inv.substr(0, 80);
        EXPECT_TRUE(eng.callInfo(id1).sessionUri.empty());                      // 세션 식별자는 최종 응답이 준다
        srv.callId = headerOf(inv, "Call-ID");
        srv.ueFrom = headerOf(inv, "From");
        srv.ueContact = uriIn(headerOf(inv, "Contact"));
        const std::string sid = "sip:mcptt-psi@127.0.0.1:" + std::to_string(srv.port) + ";gr=s-0001";
        srv.contact = "<" + sid + ">;+g.3gpp.mcptt;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";isfocus";
        srv.reply(inv, 200, "OK", "application/sdp", sdp(1));
        ASSERT_FALSE(srv.recv("ACK ").empty());
        ASSERT_TRUE(l.wait([&] { return l.lastState.callId == id1 && l.lastState.state == CallState::Active; }));
        EXPECT_EQ(l.lastState.sessionUri, sid);
        EXPECT_EQ(eng.callInfo(id1).sessionUri, sid);

        // 나가기 — BYE 는 세션 식별자로 간다(다이얼로그 원격 대상 = 최종 응답 Contact)
        ASSERT_TRUE(eng.hangup(id1).ok);
        std::string bye = srv.recv("BYE ");
        ASSERT_FALSE(bye.empty());
        EXPECT_EQ(bye.compare(0, 4 + sid.size() + 1, "BYE " + sid + " "), 0) << bye.substr(0, 80);
        srv.reply(bye, 200, "OK");
        ASSERT_TRUE(l.wait([&] { return l.lastState.callId == id1 && l.lastState.state == CallState::Disconnected; }));

        // 재합류 — 개시 속성을 줘도 싣지 않는다
        srv.contact.clear();
        GroupCallOptions ro;
        ro.sessionUri = sid;
        ro.broadcast = true; ro.chat = true; ro.members = {"tel:+82500000002"};
        int id2 = eng.joinGroupCall(acc, "g001", ro);
        ASSERT_GE(id2, 0);
        EXPECT_EQ(eng.callInfo(id2).sessionUri, sid);
        EXPECT_EQ(eng.callInfo(id2).mcptt.sessionType, "prearranged");
        std::string rj = srv.recv("INVITE ");
        ASSERT_FALSE(rj.empty());
        EXPECT_EQ(rj.compare(0, 7 + sid.size() + 10, "INVITE " + sid + " SIP/2.0\r\n"), 0) << rj.substr(0, 100);
        EXPECT_NE(uriIn(headerOf(rj, "To")).find(";gr=s-0001"), std::string::npos) << headerOf(rj, "To");   // gr 는 URI 안(포트는 pjsip 이 뺀다)
        EXPECT_NE(rj.find("<session-type>prearranged</session-type>"), std::string::npos) << rj;
        EXPECT_NE(rj.find("<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:g001</mcpttURI>"), std::string::npos) << rj;
        EXPECT_NE(rj.find("<mcptt-client-id type=\"Normal\"><mcpttString>urn:uuid:00000000-0000-4000-8000-000000000001</mcpttString>"),
                  std::string::npos) << rj;
        EXPECT_EQ(rj.find("broadcast-ind"), std::string::npos) << rj;
        EXPECT_EQ(rj.find("resource-lists"), std::string::npos) << rj;
        EXPECT_EQ(countHeader(rj, "Accept-Contact"), 2) << rj;
        EXPECT_EQ(headerOf(rj, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcptt");
        EXPECT_NE(rj.find(" udp MCPTT\r\n"), std::string::npos) << rj;
        srv.reply(rj, 404, "Not Found");
        srv.recv("ACK ");
        ASSERT_TRUE(l.wait([&] { return l.lastState.callId == id2 && l.lastState.state == CallState::Disconnected; }));
        EXPECT_EQ(l.lastState.lastCode, 404);
        EXPECT_TRUE(srv.recv("INVITE ", 500).empty());                          // 새 세션을 열지 않는다
    }
    eng.stop();
}

// conference 구독(TS 24.379 §10.1.3.2) — 참가한 진행 중 그룹 세션 안에서만: 세션 밖에서는 원하기만 하고 보내지 않는다(§10.1.3.1 · 서버는
//   404 137 — §10.1.3.3 2)). 호가 성립하면 2) Request-URI = 세션 식별자(GRUU — 사용자부는 서버가 정한다) · 3) P-Preferred-Service ·
//   4) Accept-Contact icsi-ref · 5) Expires 4294967295 · 7) Accept · 8) mcptt-info 그룹 ID. 통지의 그룹 = 그 세션 호의 그룹(gr 로 맞춘다).
//   세션을 떠나면 Expires 0 으로 거두고, 서버가 noresource 로 먼저 끝냈으면(RFC 4575 §3.3) 아무것도 보내지 않는다. 일제 통화는 구독하지 않는다
TEST(McpttConference, SubscriptionWithinOngoingSession) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("conf-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        ac.instanceId = "urn:uuid:00000000-0000-4000-8000-000000000001";
        ac.mcpttServerUri = "sip:mcptt-psi@ptt.test";
        ac.mcpttEnabled = true;
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        ASSERT_TRUE(eng.subscribeConference(acc, "g001", true).ok);
        EXPECT_TRUE(srv.recv("SUBSCRIBE ", 300).empty());                     // 세션 밖 — 보내지 않는다

        // 세션 성립 — 세션 식별자의 사용자부는 PSI(서버가 정한다), 그룹은 호의 것
        auto establish = [&](const std::string& gr, const GroupCallOptions& o) {
            int id = eng.joinGroupCall(acc, "g001", o);
            EXPECT_GE(id, 0);
            std::string inv = srv.recv("INVITE ");
            EXPECT_FALSE(inv.empty());
            srv.callId = headerOf(inv, "Call-ID");
            srv.ueFrom = headerOf(inv, "From");
            srv.ueContact = uriIn(headerOf(inv, "Contact"));
            srv.contact = "<sip:mcptt-psi@127.0.0.1:" + std::to_string(srv.port) + ";gr=" + gr + ">;+g.3gpp.mcptt;isfocus";
            srv.reply(inv, 200, "OK", "application/sdp", sdp(1));       // ACK 는 받지 않는다 — 구독이 ACK 보다 먼저 닿을 수 있다(recv 가 버린다)
            EXPECT_TRUE(l.wait([&] { return l.lastState.callId == id && l.lastState.state == CallState::Active; }));
            srv.contact.clear();
            return id;
        };
        const std::string sid = "sip:mcptt-psi@127.0.0.1:" + std::to_string(srv.port) + ";gr=s-0001";
        int id1 = establish("s-0001", GroupCallOptions());
        std::string s1 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s1.empty());
        EXPECT_EQ(s1.substr(0, s1.find("\r\n")), "SUBSCRIBE " + sid + " SIP/2.0");                       // 2)
        EXPECT_NE(headerOf(s1, "To").find(";gr=s-0001>"), std::string::npos) << headerOf(s1, "To");       // gr 는 URI 안
        EXPECT_EQ(headerOf(s1, "Event"), "conference");
        EXPECT_EQ(headerOf(s1, "Expires"), "4294967295");                                                 // 5)
        EXPECT_EQ(countHeader(s1, "Expires"), 1) << s1;
        EXPECT_EQ(headerOf(s1, "Accept"), "application/conference-info+xml");                             // 7)
        EXPECT_EQ(headerOf(s1, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcptt");          // 3)
        EXPECT_EQ(headerOf(s1, "Accept-Contact"), "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit");   // 4)
        EXPECT_EQ(countHeader(s1, "Accept-Contact"), 1) << s1;
        EXPECT_NE(headerOf(s1, "Contact").find(";+g.3gpp.mcptt;"), std::string::npos) << headerOf(s1, "Contact");
        EXPECT_EQ(headerOf(s1, "Content-Type"), "application/vnd.3gpp.mcptt-info+xml");
        EXPECT_NE(s1.find("<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:g001</mcpttURI></mcptt-request-uri>"), std::string::npos) << s1;   // 8)
        srv.reply(s1, 200, "OK", "", "", "Expires: 3600\r\n");
        // 통지 — From = 세션 식별자(사용자부 PSI) → 그룹 g001
        int branch = 0;                                                          // NOTIFY 마다 새 트랜잭션
        auto notify = [&](const std::string& sub, const std::string& state, int seq) {
            const std::string info = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\" "
                                     "entity=\"" + sid + "\" state=\"full\" version=\"" + std::to_string(seq) + "\"><users>"
                                     "<user entity=\"tel:+82500000001\"><endpoint><status>connected</status></endpoint></user>"
                                     "<user entity=\"tel:+82500000002\"><endpoint><status>connected</status></endpoint></user>"
                                     "</users></conference-info>";
            std::string from = headerOf(sub, "To");
            if (from.find(";tag=") == std::string::npos) from += ";tag=srv";
            std::string r = "NOTIFY " + uriIn(headerOf(sub, "Contact")) + " SIP/2.0\r\n" +
                            "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srv.port) + ";branch=z9hG4bKntf" + std::to_string(++branch) + "\r\n" +
                            "Max-Forwards: 70\r\nFrom: " + from + "\r\nTo: " + headerOf(sub, "From") + "\r\nCall-ID: " + headerOf(sub, "Call-ID") +
                            "\r\nCSeq: " + std::to_string(seq) + " NOTIFY\r\nContact: <" + sid + ">\r\nEvent: conference\r\n" +
                            "Subscription-State: " + state + "\r\nContent-Type: application/conference-info+xml\r\n" +
                            "Content-Length: " + std::to_string(info.size()) + "\r\n\r\n" + info;
            srv.send(r);
            EXPECT_FALSE(srv.recv("SIP/2.0 200").empty());
        };
        notify(s1, "active;expires=3600", 1);
        ASSERT_TRUE(l.wait([&] { return !l.rosters.empty(); }));
        {
            std::lock_guard<std::mutex> lk(l.m);
            EXPECT_EQ(l.rosters.back().first, "g001");
            EXPECT_EQ(l.rosters.back().second, 2u);
        }

        // 세션을 떠난다 — 같은 구독 다이얼로그로 Expires 0
        ASSERT_TRUE(eng.hangup(id1).ok);
        std::string bye = srv.recv("BYE ");
        ASSERT_FALSE(bye.empty());
        srv.reply(bye, 200, "OK");
        std::string u1 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(u1.empty());
        EXPECT_EQ(headerOf(u1, "Call-ID"), headerOf(s1, "Call-ID"));
        EXPECT_EQ(headerOf(u1, "Expires"), "0");
        EXPECT_EQ(headerOf(u1, "Content-Type"), "");
        srv.reply(u1, 200, "OK", "", "", "Expires: 0\r\n");
        notify(s1, "terminated;reason=timeout", 2);                              // 해지의 마지막 통지(RFC 6665 §4.2.2)

        // 두 번째 세션 — 서버가 세션 해제로 구독을 먼저 끝낸다(noresource) → 해지를 보내지 않는다
        int id2 = establish("s-0002", GroupCallOptions());
        std::string s2 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s2.empty());
        EXPECT_NE(s2.find(";gr=s-0002 SIP/2.0"), std::string::npos) << s2.substr(0, 100);
        srv.reply(s2, 200, "OK", "", "", "Expires: 3600\r\n");
        notify(s2, "terminated;reason=noresource", 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string sbye = "BYE " + srv.ueContact + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(srv.port) +
                           ";branch=z9hG4bKbye2\r\nMax-Forwards: 70\r\nFrom: <sip:g001@ptt.test>;tag=srv\r\nTo: " + srv.ueFrom +
                           "\r\nCall-ID: " + srv.callId + "\r\nCSeq: 200 BYE\r\nContent-Length: 0\r\n\r\n";
        srv.send(sbye);
        ASSERT_TRUE(l.wait([&] { return l.lastState.callId == id2 && l.lastState.state == CallState::Disconnected; }));
        EXPECT_TRUE(srv.recv("SUBSCRIBE ", 400).empty());

        // 세 번째 세션 — 앱이 그만 원하면 세션 중에도 거둔다
        int id3 = establish("s-0003", GroupCallOptions());
        std::string s3 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s3.empty());
        srv.reply(s3, 200, "OK", "", "", "Expires: 3600\r\n");
        ASSERT_TRUE(eng.subscribeConference(acc, "g001", false).ok);
        std::string u3 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(u3.empty());
        EXPECT_EQ(headerOf(u3, "Expires"), "0");
        srv.reply(u3, 200, "OK", "", "", "Expires: 0\r\n");
        notify(s3, "terminated;reason=timeout", 1);
        ASSERT_TRUE(eng.subscribeConference(acc, "g001", true).ok);                // 다시 원하면 진행 중 세션에 바로
        std::string s3b = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s3b.empty());
        EXPECT_EQ(headerOf(s3b, "Expires"), "4294967295");
        EXPECT_NE(headerOf(s3b, "Call-ID"), headerOf(s3, "Call-ID"));            // 새 다이얼로그
        srv.reply(s3b, 200, "OK", "", "", "Expires: 3600\r\n");
        ASSERT_TRUE(eng.hangup(id3).ok);
        bye = srv.recv("BYE ");
        ASSERT_FALSE(bye.empty());
        srv.reply(bye, 200, "OK");
        std::string u3b = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(u3b.empty());
        srv.reply(u3b, 200, "OK", "", "", "Expires: 0\r\n");

        // 일제 통화(TS 24.379 §10.1.3.2 — broadcast 로 개시한 호는 구독하지 않는다, 서버는 480 105)
        GroupCallOptions bo;
        bo.broadcast = true;
        int id4 = establish("s-0004", bo);
        EXPECT_TRUE(srv.recv("SUBSCRIBE ", 400).empty());
        ASSERT_TRUE(eng.hangup(id4).ok);
        bye = srv.recv("BYE ");
        if (!bye.empty()) srv.reply(bye, 200, "OK");
    }
    eng.stop();
}

// 제어 기능 Contact 의 세션 식별자(TS 24.379 §4.5) — isfocus 가 붙은 name-addr 의 URI. isfocus 는 헤더 파라미터만 본다(대소문자 무관)
TEST(McpttXml, SessionIdentityFromContact) {
    using mcptt::sessionIdentity;
    using mcptt::sessionGr;
    EXPECT_EQ(sessionIdentity("<sip:mcptt_psi@ptt.test;gr=a1>;+g.3gpp.mcptt;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";isfocus"),
              "sip:mcptt_psi@ptt.test;gr=a1");
    EXPECT_EQ(sessionIdentity("Contact: <sip:g001@10.0.0.1:5060;gr=s1>;IsFocus"), "sip:g001@10.0.0.1:5060;gr=s1");
    EXPECT_EQ(sessionIdentity("<sip:g001@10.0.0.1;gr=s1>;isfocus;+g.3gpp.mcptt"), "sip:g001@10.0.0.1;gr=s1");
    EXPECT_EQ(sessionIdentity("<sip:g001@10.0.0.1;gr=s1>;+g.3gpp.mcptt"), "");      // 세션 초점이 아니다(단말 Contact)
    EXPECT_EQ(sessionIdentity("<sip:isfocus@10.0.0.1;isfocus>"), "");               // URI 안의 글자는 보지 않는다
    EXPECT_EQ(sessionIdentity("<sip:g001@10.0.0.1;gr=s1>;isfocused"), "");
    EXPECT_EQ(sessionIdentity("sip:g001@10.0.0.1;isfocus"), "");                    // addr-spec — 파라미터가 URI 의 것인지 가를 수 없다
    // 세션을 가르는 값 = GRUU 의 gr(conference 통지 From → 그 세션 호의 그룹)
    EXPECT_EQ(sessionGr("sip:mcptt_psi@ptt.test:5061;transport=tls;gr=1790-7"), "1790-7");
    EXPECT_EQ(sessionGr("<sip:g001@10.0.0.1;GR=s1>;tag=a"), "s1");
    EXPECT_EQ(sessionGr("<sip:g001@10.0.0.1>;gr=s1"), "");                         // 헤더 파라미터는 아니다
    EXPECT_EQ(sessionGr("sip:g001@10.0.0.1;grx=1"), "");
    EXPECT_EQ(sessionIdentity(""), "");
}

// floor 없는 개별 호의 판정(TS 24.379 §11.1.2.2) — 개별 호 offer 에 floor 제어 채널(m=application … MCPTT)이 없으면 floor 없음.
//   mc_no_floor_ctrl 은 사전 설정 세션 용(TS 24.380 §14.2.6)이라 보지 않는다. 그룹 호·SDP 없는 초대는 floor 없음으로 보지 않는다
TEST(McpttXml, PrivateCallWithoutFloorControlBySdp) {
    auto info = [](const char* type) {
        return std::string("Content-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params>"
                           "<session-type>") + type + "</session-type></mcptt-Params></mcpttinfo>\r\n";
    };
    const std::string audio = "Content-Type: application/sdp\r\n\r\nv=0\r\nm=audio 4000 RTP/AVP 96\r\na=rtpmap:96 AMR-WB/16000\r\n";
    const std::string floor = "m=application 4002 udp MCPTT\r\na=fmtp:MCPTT mc_queueing\r\n";
    EXPECT_TRUE(mcptt::parseMcpttInfo(info("private") + audio).noFloorCtrl);
    EXPECT_FALSE(mcptt::parseMcpttInfo(info("private") + audio + floor).noFloorCtrl);
    EXPECT_FALSE(mcptt::parseMcpttInfo(info("private") + audio + "m=application 4002 UDP MCPTT\r\na=fmtp:MCPTT mc_no_floor_ctrl\r\n").noFloorCtrl);
    EXPECT_FALSE(mcptt::parseMcpttInfo(info("prearranged") + audio).noFloorCtrl);
    EXPECT_FALSE(mcptt::parseMcpttInfo(info("private")).noFloorCtrl);
}

// MCPTT speech 미디어의 i=speech(TS 24.379 §6.2.1 2)d) · §6.2.2 3)e)) — m=audio 바로 뒤, MCPTT SDP 에만, 이미 있으면 그대로
TEST(McpttXml, SpeechInfoLine) {
    const std::string floor = "m=application 4002 udp MCPTT\r\na=fmtp:MCPTT mc_queueing\r\n";
    const std::string sdp = "v=0\r\ns=-\r\nc=IN IP4 1.2.3.4\r\nt=0 0\r\nm=audio 4000 RTP/AVP 96\r\na=rtpmap:96 AMR-WB/16000\r\n" + floor;
    const std::string fixed = mcptt::withSpeechInfo(sdp);
    EXPECT_NE(fixed.find("m=audio 4000 RTP/AVP 96\r\ni=speech\r\na=rtpmap:96"), std::string::npos) << fixed;
    EXPECT_EQ(mcptt::withSpeechInfo(fixed), fixed);                                   // 두 번 넣지 않는다
    EXPECT_EQ(fixed.find("i=speech", fixed.find("m=application")), std::string::npos);   // 제어 채널에는 넣지 않는다
    const std::string volte = "v=0\r\nm=audio 4000 RTP/AVP 96\r\na=rtpmap:96 AMR-WB/16000\r\n";
    EXPECT_EQ(mcptt::withSpeechInfo(volte), volte);                                   // MCPTT 호가 아니면 그대로(일반 전화)
}

// 규격형 문서 변경 구독(TS 24.481 §6.3.13.2.1 · TS 24.484 §6.3.13.2.2) — Request-URI = PSI, 본문 = mcptt-info 액세스 토큰 + resource-lists,
//   P-Preferred-Service, 다이얼로그 Contact 의 MCPTT icsi-ref, Event·Expires 는 한 번씩. 다시 부르면 같은 다이얼로그의 re-SUBSCRIBE 가 새 목록을 싣는다
static int countHeaderLines(const std::string& msg, const std::string& name) {
    int n = 0;
    for (size_t p = msg.find("\r\n" + name + ":"); p != std::string::npos; p = msg.find("\r\n" + name + ":", p + 2)) ++n;
    return n;
}
TEST(McpttXcapDiff, StandardSubscriptionBodyAndResubscribe) {
    Engine eng;
    CondListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("COND_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("xcap-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = srv.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.test"; ac.msisdn = "+82500000001"; ac.authId = "450000000000001@ptt.test"; ac.password = "x";
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        XcapDiffSubscription sub;
        sub.documents = CscClient::gmsSubscriptionDocuments({"tel:g001", "tel:g-0a1b2c3d"});
        sub.accessToken = "eyJ.tok.1";
        ASSERT_TRUE(eng.subscribeXcapDiff(acc, "sip:gms_psi@ptt.test", sub, true).ok);
        std::string s1 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s1.empty());
        EXPECT_EQ(s1.substr(0, s1.find("\r\n")), "SUBSCRIBE sip:gms_psi@ptt.test SIP/2.0");         // b) 구독 프록시 PSI
        EXPECT_EQ(headerOf(s1, "Event"), "xcap-diff");
        EXPECT_EQ(countHeaderLines(s1, "Event"), 1) << s1;                                            // evsub 가 넣은 것 하나
        EXPECT_EQ(countHeaderLines(s1, "Expires"), 1) << s1;
        EXPECT_EQ(countHeaderLines(s1, "Contact"), 1) << s1;
        EXPECT_EQ(headerOf(s1, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcptt");      // e)
        EXPECT_NE(headerOf(s1, "Contact").find("+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\""), std::string::npos)
            << headerOf(s1, "Contact");                                                               // f)
        EXPECT_EQ(headerOf(s1, "Content-Type").rfind("multipart/mixed;boundary=", 0), 0u) << headerOf(s1, "Content-Type");
        EXPECT_NE(s1.find("<mcptt-access-token type=\"Normal\"><mcpttString>eyJ.tok.1</mcpttString></mcptt-access-token>"),
                  std::string::npos);                                                                 // c)
        EXPECT_NE(s1.find("<entry uri=\"org.openmobilealliance.groups/global/byGroupID/tel%3Ag001\"/>"), std::string::npos) << s1;   // a)
        EXPECT_NE(s1.find("byGroupID/tel%3Ag-0a1b2c3d\"/>"), std::string::npos);
        const size_t hb = s1.find("\r\n\r\n");
        EXPECT_EQ((size_t)std::atoi(headerOf(s1, "Content-Length").c_str()), s1.size() - (hb + 4));
        srv.reply(s1, 200, "OK", "", "", "Expires: 3600\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));                       // 스택이 200 으로 다이얼로그를 세운다

        // 문서 목록이 바뀌었다 — 같은 다이얼로그(Call-ID)의 re-SUBSCRIBE 가 새 목록·새 토큰을 싣는다(§6.3.13.2.1 «re-subscribe … modified list»)
        sub.documents = CscClient::gmsSubscriptionDocuments({"tel:g001"});
        sub.accessToken = "eyJ.tok.2";
        ASSERT_TRUE(eng.subscribeXcapDiff(acc, "sip:gms_psi@ptt.test", sub, true).ok);
        std::string s2 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s2.empty());
        EXPECT_EQ(headerOf(s2, "Call-ID"), headerOf(s1, "Call-ID"));
        EXPECT_NE(headerOf(s2, "To").find("tag=srv"), std::string::npos);
        EXPECT_EQ(countHeaderLines(s2, "Event"), 1) << s2;
        EXPECT_NE(s2.find("eyJ.tok.2"), std::string::npos);
        EXPECT_EQ(s2.find("g-0a1b2c3d"), std::string::npos);
        srv.reply(s2, 200, "OK", "", "", "Expires: 3600\r\n");

        // 해지 — Expires 0, 본문 없음
        ASSERT_TRUE(eng.subscribeXcapDiff(acc, "sip:gms_psi@ptt.test", sub, false).ok);
        std::string s3 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s3.empty());
        EXPECT_EQ(headerOf(s3, "Expires"), "0");
        EXPECT_EQ(headerOf(s3, "Content-Type"), "");
        srv.reply(s3, 200, "OK", "", "", "Expires: 0\r\n");

        // 본문 없는 구독(옛 형식) — 그대로
        ASSERT_TRUE(eng.subscribeXcapDiff(acc, "sip:cms_psi@ptt.test", true).ok);
        std::string s4 = srv.recv("SUBSCRIBE ");
        ASSERT_FALSE(s4.empty());
        EXPECT_EQ(headerOf(s4, "Content-Type"), "");
        EXPECT_EQ(headerOf(s4, "P-Preferred-Service"), "");
        srv.reply(s4, 200, "OK", "", "", "Expires: 3600\r\n");
        EXPECT_FALSE(eng.subscribeXcapDiff(acc, "sip:gms_psi@ptt.test", XcapDiffSubscription(), true).ok);   // 문서 없음
    }
    eng.stop();
}

TEST(McpttXml, AlertBuildAndParse) {
    std::string b = mcptt::alertInfo("tel:g002", "tel:+82500000002", "urn:uuid:abc", false, "tel:+82500000013", -1);
    // 요소 순서 = TS 24.379 §F.1 mcptt-ParamsType(request-uri → calling-user-id → emergency-ind → alert-ind → originated-by → client-id)
    size_t ru = b.find("mcptt-request-uri"), cu = b.find("mcptt-calling-user-id"),
           em = b.find("<emergency-ind type=\"Normal\"><mcpttBoolean>false</mcpttBoolean></emergency-ind>"), al = b.find("<alert-ind type=\"Normal\"><mcpttBoolean>false</mcpttBoolean></alert-ind>"),
           ob = b.find("<originated-by type=\"Normal\"><mcpttURI>tel:+82500000013</mcpttURI></originated-by>"), ci = b.find("<mcptt-client-id type=\"Normal\"><mcpttString>urn:uuid:abc</mcpttString></mcptt-client-id>");
    ASSERT_NE(ci, std::string::npos);
    EXPECT_TRUE(ru < cu && cu < em && em < al && al < ob && ob < ci);
    EXPECT_EQ(mcptt::alertInfo("tel:g002", "tel:+1", "", true).find("mcptt-client-id"), std::string::npos);

    EmergencyAlert a;
    ASSERT_TRUE(mcptt::parseEmergencyAlert(b, a));
    EXPECT_EQ(a.groupId, "g002");                                             // calling-group-id 없으면 request-uri
    EXPECT_EQ(a.userId, "+82500000002");
    EXPECT_EQ(a.originatedBy, "+82500000013");
    EXPECT_EQ(a.alertInd, -1);
    EXPECT_EQ(a.emergencyInd, -1);
    EXPECT_EQ(a.imminentPerilInd, 0);

    // 규격 서버 형식 — 접두사·calling-group-id 우선, 경보 없는 그룹 긴급 통지(§12.1.1.3 3))
    EmergencyAlert n;
    ASSERT_TRUE(mcptt::parseEmergencyAlert("<mi:mcpttinfo xmlns:mi=\"urn:3gpp:ns:mcpttInfo:1.0\"><mi:mcptt-Params>"
                                           "<mi:mcptt-request-uri>sip:psi@d</mi:mcptt-request-uri>"
                                           "<mi:mcptt-calling-group-id>tel:g003</mi:mcptt-calling-group-id>"
                                           "<mi:emergency-ind>true</mi:emergency-ind><mi:mc-org>소방</mi:mc-org>"
                                           "</mi:mcptt-Params></mi:mcpttinfo>", n));
    EXPECT_EQ(n.groupId, "g003");
    EXPECT_EQ(n.alertInd, 0);
    EXPECT_EQ(n.emergencyInd, 1);
    EXPECT_EQ(n.mcOrg, "소방");
    EXPECT_FALSE(mcptt::parseEmergencyAlert("<mcpttinfo><mcptt-Params><session-type>prearranged</session-type>"
                                            "</mcptt-Params></mcpttinfo>", n));      // 지시자 없음 = 경보·통지 아님

    EXPECT_EQ(mcptt::indicator("<a><emergency-ind> TRUE </emergency-ind></a>", "emergency-ind"), 1);   // 값 직접 기재(옛 서버)
    EXPECT_EQ(mcptt::indicator("<a><emergency-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></emergency-ind></a>",
                               "emergency-ind"), 1);                           // Annex F.1 contentType
    EXPECT_EQ(mcptt::indicator("<a><alert-ind-rcvd>true</alert-ind-rcvd></a>", "alert-ind"), 0);   // 이름 경계
    // 규격 서버 통지(§6.3.3.1.11) — contentType + &amp; 해제
    EmergencyAlert w;
    ASSERT_TRUE(mcptt::parseEmergencyAlert("<mcpttinfo><mcptt-Params>"
                                           "<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:+82500000014</mcpttURI></mcptt-request-uri>"
                                           "<mcptt-calling-user-id type=\"Normal\"><mcpttURI>tel:+82500000013</mcpttURI></mcptt-calling-user-id>"
                                           "<mcptt-calling-group-id type=\"Normal\"><mcpttURI>tel:g&amp;5</mcpttURI></mcptt-calling-group-id>"
                                           "<alert-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></alert-ind>"
                                           "</mcptt-Params></mcpttinfo>", w));
    EXPECT_EQ(w.groupId, "g&5");
    EXPECT_EQ(w.userId, "+82500000013");
    EXPECT_EQ(w.alertInd, 1);
    EXPECT_EQ(mcptt::indicator("<a><x:imminentperil-ind>false</x:imminentperil-ind></a>", "imminentperil-ind"), -1);
    EXPECT_EQ(mcptt::indicator("<a/>", "emergency-ind"), 0);
}

// TS 24.379 §6.3.3.3 — INFO g.3gpp.mcptt-info 의 미응답 멤버(<anyExt> 안, F.1 contentType · 값 직접 기재 · 접두사 · 자기 닫힘)
TEST(McpttXml, NonAcknowledgedUsers) {
    const std::string body =
        "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\"><mcptt-Params>"
        "<mcptt-calling-group-id type=\"Normal\"><mcpttURI>tel:g005</mcpttURI></mcptt-calling-group-id><anyExt>"
        "<non-acknowledged-user type=\"Normal\"><mcpttURI>tel:+82500000014</mcpttURI></non-acknowledged-user>"
        "<non-acknowledged-user type=\"Normal\"><mcpttURI>sip:+82500000015@ptt.example</mcpttURI></non-acknowledged-user>"
        "<non-acknowledged-user/>"
        "<non-acknowledged-user>tel:+82500000016</non-acknowledged-user>"
        "</anyExt></mcptt-Params></mcpttinfo>";
    std::vector<std::string> u = mcptt::nonAcknowledgedUsers(body);
    ASSERT_EQ(u.size(), 3u);
    EXPECT_EQ(u[0], "+82500000014");
    EXPECT_EQ(u[1], "+82500000015");
    EXPECT_EQ(u[2], "+82500000016");
    EXPECT_TRUE(mcptt::nonAcknowledgedUsers("<mcpttinfo><mcptt-Params/></mcpttinfo>").empty());
    EXPECT_EQ(mcptt::nonAcknowledgedUsers("<m:non-acknowledged-user type=\"Normal\"><m:mcpttURI>tel:9</m:mcpttURI></m:non-acknowledged-user>").size(), 1u);
}
