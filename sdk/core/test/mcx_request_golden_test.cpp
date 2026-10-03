// libcimsue 단위시험 — MCPTT·MCData 요청 형식 계약의 생성 쪽 (S1-UE-UNIT, docs/dev/conformance_gap_plan.md §7)
//   계약 골든 tests/fixtures/mcptt/sip/ (CSP 해석 시험 tests/csp_mcptt_request_test.cpp 와 같은 파일)을 읽어, 엔진이 같은 시나리오
//   (UE A +82510002001 · B +82510002002 · 그룹 g101 · MCPTT/MCData PSI · client ID)에서 만든 요청이 «같은 요청» 인지 본다.
//   의미 비교(README) — 헤더 순서·o=·태그·branch·boundary·포트는 보지 않는다. Accept-Contact 는 줄을 나눠도 쉼표 목록이어도 같다(RFC 3261 §7.3.1).
//   응답 골든(03 · 06)은 SDK 해석 쪽 — Warning 문구 번호가 CallInfo·RequestResult 의 warningCode 로 올라오는지 본다.
//   SDK 쪽 편차: 미디어 평면 SDS INVITE 의 더미 m=audio(a=inactive — mcdata_messaging.md §7 «media plane SDS 의 SDP»)는 골든에 없다.
#include <gtest/gtest.h>

#include <pjlib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cimsue/cimsue.h"
#include "pj_scope.h"

using namespace cimsue;

namespace {

const char* kClientId = "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f";

std::string golden(const char* name) {
    std::ifstream f(std::string(CIMS_SOURCE_ROOT) + "/tests/fixtures/mcptt/sip/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == '\r' || s[b] == '\n' || s[b] == ' ')) ++b;
    return s.substr(b);
}

std::vector<std::string> headers(const std::string& msg, const std::string& name) {
    std::vector<std::string> out;
    const size_t end = msg.find("\r\n\r\n");
    size_t p = 0;
    while ((p = msg.find("\r\n", p)) != std::string::npos && p < end) {
        p += 2;
        if (msg.compare(p, name.size() + 1, name + ":") == 0) {
            size_t v = p + name.size() + 1, e = msg.find("\r\n", v);
            while (v < e && msg[v] == ' ') ++v;
            out.push_back(msg.substr(v, e - v));
        }
    }
    return out;
}
std::string header(const std::string& msg, const std::string& name) {
    auto v = headers(msg, name);
    return v.empty() ? std::string() : v[0];
}

/** Accept-Contact 값 집합 — 여러 줄이든 한 줄의 쉼표 목록이든(값 안에 쉼표가 없다). */
std::set<std::string> acceptContacts(const std::string& msg) {
    std::set<std::string> out;
    for (const auto& h : headers(msg, "Accept-Contact")) {
        std::stringstream ss(h);
        std::string one;
        while (std::getline(ss, one, ',')) out.insert(trim(one));
    }
    return out;
}

/** Contact 의 특성 태그 — '>' 뒤 파라미터. */
std::string contactTags(const std::string& msg) {
    const std::string c = header(msg, "Contact");
    const size_t gt = c.find('>');
    return gt == std::string::npos ? std::string() : c.substr(gt + 1);
}

struct Part { std::string type, cte, body; };

std::vector<Part> parts(const std::string& msg) {
    std::vector<Part> out;
    const std::string ct = header(msg, "Content-Type");
    const size_t bp = ct.find("boundary=");
    if (bp == std::string::npos) return out;
    std::string boundary = ct.substr(bp + 9);
    if (!boundary.empty() && boundary[0] == '"') boundary = boundary.substr(1, boundary.find('"', 1) - 1);
    const std::string delim = "--" + boundary;
    const std::string body = msg.substr(msg.find("\r\n\r\n") + 4);
    size_t p = body.find(delim);
    while (p != std::string::npos) {
        p += delim.size();
        if (body.compare(p, 2, "--") == 0) break;
        if (body.compare(p, 2, "\r\n") == 0) p += 2;                  // 구분선 줄 끝
        const size_t next = body.find("\r\n" + delim, p);
        const std::string raw = body.substr(p, next == std::string::npos ? std::string::npos : next - p);
        const size_t hb = raw.find("\r\n\r\n");
        Part pt;
        std::stringstream hs(raw.substr(0, hb == std::string::npos ? raw.size() : hb));
        std::string ln;
        while (std::getline(hs, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            auto val = [&](const char* n) { const size_t k = std::string(n).size(); return trim(ln.substr(k)); };
            if (ln.compare(0, 13, "Content-Type:") == 0) pt.type = val("Content-Type:");
            else if (ln.compare(0, 26, "Content-Transfer-Encoding:") == 0) pt.cte = val("Content-Transfer-Encoding:");
        }
        // 본문 — 줄 끝(CRLF·LF)은 의미가 아니다
        std::string b = hb == std::string::npos ? std::string() : raw.substr(hb + 4);
        for (size_t q; (q = b.find("\r\n")) != std::string::npos;) b.erase(q, 1);
        pt.body = trim(b);
        out.push_back(pt);
        p = next == std::string::npos ? std::string::npos : next + 2;
    }
    return out;
}
const Part* partOf(const std::vector<Part>& v, const std::string& type) {
    for (const auto& p : v)
        if (p.type.compare(0, type.size(), type) == 0) return &p;
    return nullptr;
}

/** SDP 의 m= 줄(포트를 뺀 «media proto fmt…»)과 그 섹션 속성. */
std::vector<std::string> mLines(const std::string& sdp) {
    std::vector<std::string> out;
    std::stringstream ss(sdp);
    std::string ln;
    while (std::getline(ss, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.rfind("m=", 0) != 0) continue;
        std::stringstream ls(ln.substr(2));
        std::string media, port, proto;
        ls >> media >> port >> proto;
        out.push_back(media + " " + proto);
    }
    return out;
}
std::string sectionOf(const std::string& sdp, const std::string& media) {
    const size_t m = sdp.find("m=" + media + " ");
    if (m == std::string::npos) return std::string();
    const size_t next = sdp.find("\nm=", m + 1);
    return sdp.substr(m, next == std::string::npos ? std::string::npos : next - m);
}

/** 골든과 «같은 요청» — 요청 줄·Accept-Contact·P-Preferred-Service·Contact 태그·본문 파트(XML 은 글자 그대로, 이진 파트는 형식·전송 인코딩). */
void expectSameRequest(const std::string& sdk, const std::string& gold, const char* name) {
    SCOPED_TRACE(name);
    ASSERT_FALSE(sdk.empty());
    EXPECT_EQ(sdk.substr(0, sdk.find("\r\n")), gold.substr(0, gold.find("\r\n")));
    EXPECT_EQ(acceptContacts(sdk), acceptContacts(gold));
    EXPECT_EQ(header(sdk, "P-Preferred-Service"), header(gold, "P-Preferred-Service"));
    if (!header(gold, "Contact").empty()) EXPECT_EQ(contactTags(sdk), contactTags(gold));
    if (!header(gold, "Answer-Mode").empty()) EXPECT_EQ(header(sdk, "Answer-Mode"), header(gold, "Answer-Mode"));
    if (!header(gold, "Session-Expires").empty()) {
        EXPECT_FALSE(header(sdk, "Session-Expires").empty());
        EXPECT_NE(header(sdk, "Supported").find("timer"), std::string::npos);
    }
    EXPECT_EQ(header(sdk, "Content-Type").rfind("multipart/mixed", 0), 0u);
    const auto gp = parts(gold), sp = parts(sdk);
    for (const auto& g : gp) {
        const Part* s = partOf(sp, g.type);
        ASSERT_NE(s, nullptr) << "missing part " << g.type;
        if (g.type == "application/sdp") continue;                      // SDP 는 호출자가 본다
        if (!g.cte.empty()) { EXPECT_EQ(s->cte, g.cte) << g.type; continue; }   // 이진(signalling·payload) — 값은 시각·ID 마다 다르다
        EXPECT_EQ(s->body, g.body) << g.type;
    }
    for (const auto& s : sp) EXPECT_NE(partOf(gp, s.type), nullptr) << "extra part " << s.type;
}

struct Listener_ : Listener {
    std::mutex m;
    std::condition_variable cv;
    std::vector<RequestResult> results;
    CallInfo last;
    void onCallState(const CallInfo& i) override { { std::lock_guard<std::mutex> lk(m); last = i; } cv.notify_all(); }
    void onRequestResult(const RequestResult& r) override { { std::lock_guard<std::mutex> lk(m); results.push_back(r); } cv.notify_all(); }
    template <class P> bool wait(P pred, int ms = 5000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pred(); });
    }
};

/** 참여 기능 자리 — 요청을 받고 최종 응답을 준다. */
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
    void reply(const std::string& req, int code, const char* reason, const std::string& extra = "") {
        std::string r = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n" + extra;
        for (const auto& v : headers(req, "Via")) r += "Via: " + v + "\r\n";
        std::string to = header(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=srv";
        r += "From: " + header(req, "From") + "\r\nTo: " + to + "\r\nCall-ID: " + header(req, "Call-ID") + "\r\nCSeq: " +
             header(req, "CSeq") + "\r\nContent-Length: 0\r\n\r\n";
        pj_ssize_t len = (pj_ssize_t)r.size();
        pj_sock_sendto(s, r.data(), &len, 0, &peer, sizeof(peer));
    }
};

}  // namespace

TEST(McxRequestGolden, SdkRequestsMatchContract) {
    Engine eng;
    Listener_ l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("MCX_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("mcx-golden");
        FakeSip sip;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = sip.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.cims.example.kr"; ac.msisdn = "+82510002001"; ac.authId = "450051000200001@ptt.cims.example.kr"; ac.password = "x";
        ac.instanceId = kClientId;
        ac.mcpttEnabled = true;
        ac.mcpttServerUri = "sip:mcptt_psi@ptt.cims.example.kr";
        ac.mcdataServerUri = "sip:mcdata_psi@ptt.cims.example.kr";
        ac.mcdataMsrp = true;
        ac.maxSdsCplaneBytes = 1000;
        const int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        // 01·02 개별 호 — floor 있음/없음. 03 = 착신자 판정 실패 응답(Warning 145)을 SDK 가 읽는다
        const std::string g03 = golden("03_private_reject_403_145.txt");
        for (bool full : {false, true}) {
            GroupCallOptions o;
            o.fullDuplex = full;
            o.commencement = CommencementMode::Manual;
            const int id = eng.startPrivateCall(acc, "+82510002002", o);
            ASSERT_GE(id, 0);
            const std::string inv = sip.recv("INVITE ");
            const std::string gold = golden(full ? "02_private_full_duplex_invite.txt" : "01_private_invite.txt");
            expectSameRequest(inv, gold, full ? "02" : "01");
            const auto sp = parts(inv), gp = parts(gold);                  // partOf 는 벡터 안을 가리킨다 — 임시로 넘기지 않는다
            const Part* sdp = partOf(sp, "application/sdp");
            ASSERT_NE(sdp, nullptr);
            auto want = mLines(partOf(gp, "application/sdp")->body);
            EXPECT_EQ(mLines(sdp->body), want) << sdp->body;                 // m=audio (+ floor m=application udp MCPTT 는 floor 있을 때만)
            EXPECT_NE(sdp->body.find("i=speech"), std::string::npos);
            sip.reply(inv, 403, "Forbidden", "Warning: " + header(g03, "Warning") + "\r\n");
            sip.recv("ACK ");
            ASSERT_TRUE(l.wait([&] { return l.last.callId == id && l.last.state == CallState::Disconnected; }));
            EXPECT_EQ(l.last.lastCode, 403);
            EXPECT_EQ(l.last.warningCode, 145);
        }

        // 04·05 SDS · 07·08 FD(신호 평면). 06 = 1:1 대상 판정 실패 응답(Warning 204)을 SDK 가 읽는다
        const std::string g06 = golden("06_sds_reject_403_204.txt");
        FdFile f;
        f.url = "https://csc.ptt.cims.example.kr:4430/mcdata/fd/0123456789abcdef0123456789abcdef";
        f.name = "site-map.png"; f.size = 48213; f.type = "image/png";
        auto run = [&](const char* gname, const SdsSend& sent, bool reject) {
            ASSERT_TRUE(sent.ok);
            const std::string msg = sip.recv("MESSAGE ");
            expectSameRequest(msg, golden(gname), gname);
            if (reject) sip.reply(msg, 403, "Forbidden", "Warning: " + header(g06, "Warning") + "\r\n");
            else sip.reply(msg, 200, "OK");
            ASSERT_TRUE(l.wait([&] { for (auto& r : l.results) if (r.token == sent.token) return true; return false; }));
            for (auto& r : l.results)
                if (r.token == sent.token && reject) { EXPECT_EQ(r.code, 403); EXPECT_EQ(r.warningCode, 204); }
        };
        run("04_sds_group_message.txt", eng.sendGroupSds(acc, "g101", "hello g101", true), false);
        run("05_sds_one_to_one_message.txt", eng.sendSds(acc, "+82510002002", "hello B", true), true);
        run("07_fd_group_message.txt", eng.sendGroupFd(acc, "g101", f), false);
        run("08_fd_one_to_one_message.txt", eng.sendFd(acc, "+82510002002", f), false);

        // 09 미디어 평면 그룹 SDS INVITE — m=message 섹션은 골든 그대로(포트·path 값 제외), 더미 m=audio 는 SDK 편차
        const SdsSend big = eng.sendGroupSds(acc, "g101", std::string(2000, 'x'));
        ASSERT_TRUE(big.ok);
        const std::string inv = sip.recv("INVITE ");
        const std::string g09 = golden("09_sds_media_group_invite.txt");
        expectSameRequest(inv, g09, "09");
        const auto sp = parts(inv), gp = parts(g09);
        const Part* sdp = partOf(sp, "application/sdp");
        ASSERT_NE(sdp, nullptr);
        const std::string gsec = sectionOf(partOf(gp, "application/sdp")->body, "message");
        const std::string ssec = sectionOf(sdp->body, "message");
        EXPECT_EQ(mLines(ssec), mLines(gsec));
        for (const char* attr : {"a=sendonly", "a=setup:actpass",
                                 "a=accept-types:application/vnd.3gpp.mcdata-signalling application/vnd.3gpp.mcdata-payload"})
            EXPECT_NE((ssec + "\n").find(std::string(attr) + "\n"), std::string::npos) << attr << "\n" << ssec;
        EXPECT_NE(ssec.find("a=path:msrp://"), std::string::npos);
        sip.reply(inv, 488, "Not Acceptable Here");
        sip.recv("ACK ");
        ASSERT_TRUE(l.wait([&] { for (auto& r : l.results) if (r.token == big.token) return true; return false; }));
    }
    eng.stop();
}
