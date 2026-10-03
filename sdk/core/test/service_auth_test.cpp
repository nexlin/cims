// libcimsue 단위시험 — MC 서비스 인가·서비스 설정(TS 24.379 §7.2.2 · TS 24.282 §7.2.2 · TS 24.281 §7.2.2) (S1-UE-UNIT)
//   ① 판정(detail::ServiceAuth) — 언제 인가 PUBLISH 를 (다시) 보내나. ② 본문 — 계약 골든 tests/fixtures/mcptt/sip/14(인가+설정)·
//   16(설정 제거) 과 «같은 요청», MCData·MCVideo 판. ③ 엔진 — 등록 → 인가 PUBLISH, 인가 응답 전 제휴 보류, 403 101 → 인가 안 됨 →
//   새 토큰으로 다시, 제휴 404 141 → 다시 인가, MCVideo 로그오프 = Expires 0 + SIP-If-Match.
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/cimsue.h"
#include "pj_scope.h"
#include "service_auth.h"

using namespace cimsue;
using detail::AuthTrigger;
using detail::ServiceAuth;

namespace {

const char* kClientId = "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f";
const char* kPsi = "sip:mcptt_psi@ptt.cims.example.kr";

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

std::string header(const std::string& msg, const std::string& name) {
    const size_t end = msg.find("\r\n\r\n");
    size_t p = 0;
    while ((p = msg.find("\r\n", p)) != std::string::npos && p < end) {
        p += 2;
        if (msg.compare(p, name.size() + 1, name + ":") == 0) {
            size_t v = p + name.size() + 1, e = msg.find("\r\n", v);
            while (v < e && msg[v] == ' ') ++v;
            return msg.substr(v, e - v);
        }
    }
    return std::string();
}

/** multipart 본문의 파트(Content-Type 앞부분이 type 인 것) — 줄 끝은 의미가 아니다. */
std::string partOf(const std::string& msg, const std::string& type) {
    const std::string ct = header(msg, "Content-Type");
    const size_t bp = ct.find("boundary=");
    if (bp == std::string::npos) return std::string();
    const std::string delim = "--" + ct.substr(bp + 9);
    const std::string body = msg.substr(msg.find("\r\n\r\n") + 4);
    size_t p = body.find(delim);
    while (p != std::string::npos) {
        p += delim.size();
        if (body.compare(p, 2, "--") == 0) break;
        const size_t next = body.find("\r\n" + delim, p);
        const std::string raw = body.substr(p, next == std::string::npos ? std::string::npos : next - p);
        const size_t hb = raw.find("\r\n\r\n");
        if (hb != std::string::npos && raw.find("Content-Type: " + type) != std::string::npos && raw.find("Content-Type: " + type) < hb) {
            std::string b = raw.substr(hb + 4);
            for (size_t q; (q = b.find("\r\n")) != std::string::npos;) b.erase(q, 1);
            return trim(b);
        }
        p = next == std::string::npos ? std::string::npos : next + 2;
    }
    return std::string();
}

struct L : Listener {
    std::mutex m;
    std::condition_variable cv;
    std::vector<RegInfo> regs;
    std::vector<ServiceAuthInfo> auths;
    std::vector<RequestResult> results;
    void onLog(int lv, const std::string& s) override { if (std::getenv("AUTH_LOG")) std::fprintf(stderr, "[%d] %s\n", lv, s.c_str()); }
    void onRegState(const RegInfo& i) override { { std::lock_guard<std::mutex> lk(m); regs.push_back(i); } cv.notify_all(); }
    void onServiceAuth(const ServiceAuthInfo& i) override {
        if (std::getenv("AUTH_LOG")) std::fprintf(stderr, "AUTH %s %s %d\n", toString(i.service), toString(i.state), i.code);
        { std::lock_guard<std::mutex> lk(m); auths.push_back(i); }
        cv.notify_all();
    }
    void onRequestResult(const RequestResult& r) override { { std::lock_guard<std::mutex> lk(m); results.push_back(r); } cv.notify_all(); }
    template <class P> bool wait(P pred, int ms = 5000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pred(); });
    }
    /** 서비스 s 의 마지막 인가 알림이 state 인가. */
    bool lastIs(McService s, ServiceAuthState st) {
        for (auto it = auths.rbegin(); it != auths.rend(); ++it)
            if (it->service == s) return it->state == st;
        return false;
    }
};

/** 참여 기능 자리 — 요청을 받고 최종 응답을 준다(prefix 로 시작하지 않는 메시지는 버린다). */
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
    void reply(const std::string& req, int code, const char* reason, const std::string& extra = "", const std::string& ct = "",
               const std::string& body = "") {
        std::string r = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n" + extra;
        size_t p = 0;
        while ((p = req.find("\r\nVia:", p)) != std::string::npos) {
            size_t e = req.find("\r\n", p + 2);
            r += req.substr(p + 2, e - p - 2) + "\r\n";
            p = e;
        }
        std::string to = header(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=srv";
        r += "From: " + header(req, "From") + "\r\nTo: " + to + "\r\nCall-ID: " + header(req, "Call-ID") + "\r\nCSeq: " +
             header(req, "CSeq") + "\r\n";
        if (!ct.empty()) r += "Content-Type: " + ct + "\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        pj_ssize_t len = (pj_ssize_t)r.size();
        pj_sock_sendto(s, r.data(), &len, 0, &peer, sizeof(peer));
    }
};

const char* kMultipleDevices =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\">\n  <mcptt-Params>\n"
    "    <multiple-devices-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></multiple-devices-ind>\n  </mcptt-Params>\n</mcpttinfo>\n";

}  // namespace

// ── ① 판정 ──────────────────────────────────────────────────────────────────────

// 첫 등록에 한 번, 갱신 등록에는 다시 보내지 않고, 재성립·망 변경 뒤엔 다시(서버의 묶임은 등록과 함께 산다)
TEST(ServiceAuthRules, RegistrationTriggers) {
    ServiceAuth a;
    const ServiceAuth::Key k{1, McService::Mcptt};
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::Registered, 0));
    a.sent(k, 7, 0);
    EXPECT_TRUE(a.pending(k));
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::Renewed, 0));            // 응답을 기다리는 동안은 겹쳐 보내지 않는다
    EXPECT_FALSE(a.result(k, 8, 200, 0, "", false, "e1", 0, 10));          // 다른 token = 이 인가의 응답이 아니다
    EXPECT_TRUE(a.result(k, 7, 200, 0, "", true, "e1", 0, 10));
    EXPECT_EQ(a.find(k)->info.state, ServiceAuthState::Authorized);
    EXPECT_TRUE(a.find(k)->info.multipleDevices);
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::Registered, 20));        // 등록 갱신
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::TokenChanged, 20));      // 인가된 묶임은 토큰이 바뀌어도 그대로
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::Renewed, 20));
    // 등록이 끊기면 인가 안 됨 — 알릴 것으로 돌려주고, 다음 등록에서 다시
    auto changed = a.unregistered(1);
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(a.find(k)->info.state, ServiceAuthState::Unauthorized);
    EXPECT_EQ(a.find(k)->etag, "");
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::Registered, 30));
    EXPECT_TRUE(a.unregistered(1).empty());                                 // 이미 알린 상태
}

// 거절(403 101 · 486 164)은 새 토큰까지 기다리고, 일시 실패(5xx·408)는 물러나 다시, 토큰이 없으면 새 토큰을 기다린다
TEST(ServiceAuthRules, FailuresAndRetry) {
    ServiceAuth a;
    const ServiceAuth::Key k{1, McService::McVideo};
    a.sent(k, 1, 0);
    EXPECT_TRUE(a.result(k, 1, 403, 101, "service authorisation failed", false, "", 0, 0));
    const auto* e = a.find(k);
    EXPECT_EQ(e->info.state, ServiceAuthState::Unauthorized);
    EXPECT_EQ(e->info.warningCode, 101);
    EXPECT_FALSE(e->transient);
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::Registered, 1000));
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::Retry, 1000000));
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::TokenChanged, 1000));
    a.sent(k, 2, 1000);
    EXPECT_TRUE(a.result(k, 2, 500, 0, "", false, "", 5, 1000));            // IdMS 불가 — 500 + Retry-After 5
    EXPECT_TRUE(a.find(k)->transient);
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::Retry, 5999));
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::Retry, 6000));
    a.sent(k, 3, 6000);
    EXPECT_TRUE(a.result(k, 3, 408, 0, "", false, "", 0, 6000));           // 응답 없음 — 5 s 부터 배로(두 번째 = 10 s)
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::Retry, 15999));
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::Retry, 16000));
    EXPECT_EQ(ServiceAuth::retryDelayMs(1, 0), 5000);
    EXPECT_EQ(ServiceAuth::retryDelayMs(20, 0), 300000);
    // 토큰 없음 — 보내지 않고 인가 안 됨(code 0), 새 토큰에 다시
    const ServiceAuth::Key d{1, McService::McData};
    a.notSent(d, false, 0);
    EXPECT_EQ(a.find(d)->info.state, ServiceAuthState::Unauthorized);
    EXPECT_EQ(a.find(d)->info.code, 0);
    EXPECT_FALSE(a.shouldAuthorize(d, AuthTrigger::Registered, 10));
    EXPECT_TRUE(a.shouldAuthorize(d, AuthTrigger::TokenChanged, 10));
}

// 404 141 — 인가된 줄 알았던 서비스만, 인가를 보낸 뒤 잠시(인가 전에 보낸 요청의 거절)는 빼고
TEST(ServiceAuthRules, BindingLost) {
    ServiceAuth a;
    const ServiceAuth::Key k{2, McService::Mcptt};
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::BindingLost, 0));        // 인가한 적이 없다(서비스를 켜지 않았거나 토큰 없음)
    a.sent(k, 1, 100000);
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::BindingLost, 200000));   // 응답 대기
    a.result(k, 1, 200, 0, "", false, "e", 0, 100010);
    EXPECT_FALSE(a.shouldAuthorize(k, AuthTrigger::BindingLost, 100000 + ServiceAuth::kBindingLostMinMs - 1));
    EXPECT_TRUE(a.shouldAuthorize(k, AuthTrigger::BindingLost, 100000 + ServiceAuth::kBindingLostMinMs));
    // 설정 제거(로그오프) — ETag 를 돌려주고 기록을 지운다
    EXPECT_EQ(a.removed(k), "e");
    EXPECT_EQ(a.find(k), nullptr);
    EXPECT_EQ(a.removed(k), "");
}

// ── ② 본문 ──────────────────────────────────────────────────────────────────────

TEST(ServiceAuthBody, McDataAndMcVideoForms) {
    // MCData(TS 24.282 §7.2.2) — mcdata-info 토큰·client ID + poc-settings 선택 user profile 만(am-settings·multiplex 없음)
    auto b = detail::serviceAuthBody(McService::McData, "t<1>", kClientId, kClientId, true, "bd");
    EXPECT_EQ(b.contentType, "multipart/mixed;boundary=bd");
    const std::string msg = "PUBLISH x SIP/2.0\r\nContent-Type: " + b.contentType + "\r\n\r\n" + b.body;
    const std::string info = partOf(msg, "application/vnd.3gpp.mcdata-info+xml");
    EXPECT_NE(info.find("<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">"), std::string::npos) << info;
    EXPECT_NE(info.find("<mcdata-access-token type=\"Normal\"><mcdataString>t&lt;1&gt;</mcdataString></mcdata-access-token>"),
              std::string::npos) << info;
    EXPECT_LT(info.find("mcdata-access-token"), info.find("mcdata-client-id"));
    const std::string poc = partOf(msg, "application/poc-settings+xml");
    EXPECT_EQ(poc.find("am-settings"), std::string::npos) << poc;
    EXPECT_EQ(poc.find("multiplex-support"), std::string::npos) << poc;
    EXPECT_NE(poc.find("<mcs10Set:user-profile-index>1</mcs10Set:user-profile-index>"), std::string::npos);
    // MCVideo(TS 24.281 §7.2.2) — mcvideo-info + Answer-Mode·multiplex
    b = detail::serviceAuthBody(McService::McVideo, "tok", kClientId, kClientId, false, "bv");
    const std::string mv = "PUBLISH x SIP/2.0\r\nContent-Type: " + b.contentType + "\r\n\r\n" + b.body;
    EXPECT_NE(partOf(mv, "application/vnd.3gpp.mcvideo-info+xml").find("<mcvideo-access-token type=\"Normal\"><mcvideoString>tok"),
              std::string::npos);
    EXPECT_NE(partOf(mv, "application/poc-settings+xml").find("<answer-mode>manual</answer-mode>"), std::string::npos);
    EXPECT_STREQ(detail::serviceIcsi(McService::McData), "urn:urn-7:3gpp-service.ims.icsi.mcdata");
    EXPECT_TRUE(detail::multipleDevicesInd(kMultipleDevices));
    EXPECT_FALSE(detail::multipleDevicesInd("<mcpttinfo/>"));
}

// ── ③ 엔진 ──────────────────────────────────────────────────────────────────────

// 골든 14 — 등록이 서면 인가+설정 PUBLISH. 응답 전 앱 제휴는 보류됐다가 200 뒤 앱 token 으로. 200 의 multiple-devices-ind 를 알린다.
// 제휴 404 141 → 다시 인가. 403 101 → 인가 안 됨, 새 토큰(setAccessToken)에 다시.
TEST(ServiceAuthEngine, AuthorisesAfterRegistrationAndHoldsAffiliation) {
    const std::string g14 = golden("14_poc_settings_publish_auth.txt");
    ASSERT_FALSE(g14.empty()) << "tests/fixtures/mcptt/sip/14_poc_settings_publish_auth.txt";
    const std::string gToken = [&] {
        const std::string i = partOf(g14, "application/vnd.3gpp.mcptt-info+xml");
        const size_t a = i.find("<mcpttString>") + 13;
        return i.substr(a, i.find("</mcpttString>", a) - a);
    }();
    Engine eng;
    L l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("AUTH_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("svc-auth");
        FakeSip sip;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = sip.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.cims.example.kr"; ac.msisdn = "+82510002001"; ac.authId = "450051000200001@ptt.cims.example.kr"; ac.password = "x";
        ac.instanceId = kClientId;
        ac.mcpttEnabled = true;
        ac.mcpttServerUri = kPsi;
        ac.autoAnswerMcptt = false;                                 // 골든 = manual
        ac.accessToken = gToken;
        const int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);
        ASSERT_TRUE(eng.registerAccount(acc).ok);
        std::string reg = sip.recv("REGISTER ");
        ASSERT_FALSE(reg.empty());
        sip.reply(reg, 200, "OK", "Contact: " + header(reg, "Contact") + ";expires=3600\r\nExpires: 3600\r\n");

        std::string pub = sip.recv("PUBLISH ");
        ASSERT_FALSE(pub.empty());
        EXPECT_EQ(pub.substr(0, pub.find("\r\n")), g14.substr(0, g14.find("\r\n")));
        for (const char* h : {"P-Preferred-Service", "Event", "Expires"}) EXPECT_EQ(header(pub, h), header(g14, h)) << h;
        EXPECT_EQ(header(pub, "SIP-If-Match"), "");
        EXPECT_EQ(header(pub, "Content-Type").rfind("multipart/mixed", 0), 0u);
        for (const char* t : {"application/vnd.3gpp.mcptt-info+xml", "application/poc-settings+xml"}) {
            EXPECT_FALSE(partOf(g14, t).empty()) << t;
            EXPECT_EQ(partOf(pub, t), partOf(g14, t)) << t;
        }
        ASSERT_TRUE(l.wait([&] { return l.lastIs(McService::Mcptt, ServiceAuthState::Pending); }));
        EXPECT_EQ(eng.serviceAuth(acc, McService::Mcptt).state, ServiceAuthState::Pending);

        // 인가 응답 전 앱 제휴 — 내보내지 않는다
        const int64_t t1 = eng.affiliate(acc, "g101", true);
        ASSERT_GE(t1, 0);
        EXPECT_TRUE(sip.recv("PUBLISH ", 400).empty());
        sip.reply(pub, 200, "OK", "SIP-ETag: poc-etag-1\r\nExpires: 4294967295\r\n", "application/vnd.3gpp.mcptt-info+xml", kMultipleDevices);
        ASSERT_TRUE(l.wait([&] { return l.lastIs(McService::Mcptt, ServiceAuthState::Authorized); }));
        ServiceAuthInfo ok = eng.serviceAuth(acc, McService::Mcptt);
        EXPECT_EQ(ok.code, 200);
        EXPECT_TRUE(ok.multipleDevices);
        for (auto& r : l.results) EXPECT_NE(r.token, t1);              // 인가 응답은 onRequestResult 로 오지 않는다
        std::string aff = sip.recv("PUBLISH ");
        ASSERT_FALSE(aff.empty());
        EXPECT_EQ(header(aff, "Event"), "presence");
        sip.reply(aff, 200, "OK", "SIP-ETag: a1\r\nExpires: 4294967295\r\n");
        ASSERT_TRUE(l.wait([&] { for (auto& r : l.results) if (r.token == t1) return r.code == 200; return false; }));

        // 서버가 묶임을 잃었다 — 제휴 404 141 → 다시 인가(인가 직후 잠시는 빼므로 기다린다)
        std::this_thread::sleep_for(std::chrono::milliseconds(ServiceAuth::kBindingLostMinMs + 100));
        const int64_t t2 = eng.affiliate(acc, "g102", true);
        std::string aff2 = sip.recv("PUBLISH ");
        ASSERT_FALSE(aff2.empty());
        EXPECT_EQ(header(aff2, "Event"), "presence");
        sip.reply(aff2, 404, "Not Found", "Warning: " + header(golden("19_poc_settings_reject_404_141.txt"), "Warning") + "\r\n");
        ASSERT_TRUE(l.wait([&] { for (auto& r : l.results) if (r.token == t2) return r.warningCode == 141; return false; }));
        std::string re = sip.recv("PUBLISH ");
        ASSERT_FALSE(re.empty());
        EXPECT_EQ(header(re, "Event"), "poc-settings");
        // 거절 — 403 101(토큰 만료): 인가 안 됨, 같은 토큰으로 다시 보내지 않는다. 그 사이 다시 실린 제휴는 응답 뒤에 나간다
        sip.reply(re, 403, "Forbidden", "Warning: " + header(golden("18_poc_settings_reject_403_101.txt"), "Warning") + "\r\n");
        ASSERT_TRUE(l.wait([&] { return l.lastIs(McService::Mcptt, ServiceAuthState::Unauthorized); }));
        ServiceAuthInfo no = eng.serviceAuth(acc, McService::Mcptt);
        EXPECT_EQ(no.code, 403);
        EXPECT_EQ(no.warningCode, 101);
        std::string again = sip.recv("PUBLISH ");                     // 보류됐던 제휴 다시 싣기(유지)
        ASSERT_FALSE(again.empty());
        EXPECT_EQ(header(again, "Event"), "presence");
        sip.reply(again, 404, "Not Found", "Warning: " + header(golden("19_poc_settings_reject_404_141.txt"), "Warning") + "\r\n");
        EXPECT_TRUE(sip.recv("PUBLISH ", 500).empty());               // 인가 안 됨 상태의 141 은 인가를 다시 열지 않는다
        // 새 토큰 — 지금 다시 인가
        ASSERT_TRUE(eng.setAccessToken(acc, "fresh-token").ok);
        std::string re2 = sip.recv("PUBLISH ");
        ASSERT_FALSE(re2.empty());
        EXPECT_EQ(header(re2, "Event"), "poc-settings");
        EXPECT_NE(partOf(re2, "application/vnd.3gpp.mcptt-info+xml").find("<mcpttString>fresh-token</mcpttString>"), std::string::npos);
        sip.reply(re2, 200, "OK", "SIP-ETag: poc-etag-2\r\nExpires: 4294967295\r\n");
        ASSERT_TRUE(l.wait([&] { return l.lastIs(McService::Mcptt, ServiceAuthState::Authorized); }));
        EXPECT_FALSE(eng.serviceAuth(acc, McService::Mcptt).multipleDevices);
        ASSERT_TRUE(eng.setAccessToken(acc, "fresher").ok);            // 인가된 서비스는 다시 보내지 않는다
        for (std::string extra; !(extra = sip.recv("PUBLISH ", 700)).empty();) {   // 인가 뒤 다시 실린 제휴만 — 응답해 둔다(멈출 때 남지 않게)
            EXPECT_EQ(header(extra, "Event"), "presence") << extra;
            sip.reply(extra, 200, "OK", "SIP-ETag: a2\r\nExpires: 4294967295\r\n");
        }
    }
    eng.stop();
}

// 골든 16 — MCVideo 로그오프(setMcVideoEnabled false) = 설정 제거 PUBLISH(Expires 0 + SIP-If-Match, 본문 없음). 토큰·PSI 가 없는
// 서비스는 보내지 않고 인가 안 됨(code 0)으로 알린다.
TEST(ServiceAuthEngine, McVideoLogoffRemovesSettings) {
    const std::string g16 = golden("16_poc_settings_publish_remove.txt");
    ASSERT_FALSE(g16.empty());
    Engine eng;
    L l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("AUTH_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("svc-auth-mcv");
        FakeSip sip;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1"; ac.serverPort = sip.port; ac.transport = Transport::UDP;
        ac.domain = "ptt.cims.example.kr"; ac.msisdn = "+82510002001"; ac.authId = "450051000200001@ptt.cims.example.kr"; ac.password = "x";
        ac.instanceId = kClientId;
        ac.mcvideoEnabled = true;
        ac.mcvideoServerUri = "sip:mcvideo_psi@ptt.cims.example.kr";
        ac.mcdataMsrp = true;                                        // MCData 는 켰지만 PSI 가 없다 — 인가 대상 아님
        ac.accessToken = "tok";
        const int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);
        ASSERT_TRUE(eng.registerAccount(acc).ok);
        std::string reg = sip.recv("REGISTER ");
        ASSERT_FALSE(reg.empty());
        sip.reply(reg, 200, "OK", "Contact: " + header(reg, "Contact") + ";expires=3600\r\nExpires: 3600\r\n");
        std::string pub = sip.recv("PUBLISH ");
        ASSERT_FALSE(pub.empty());
        EXPECT_EQ(pub.substr(0, pub.find("\r\n")), "PUBLISH sip:mcvideo_psi@ptt.cims.example.kr SIP/2.0");
        EXPECT_NE(header(pub, "P-Preferred-Service").find("icsi.mcvideo"), std::string::npos);
        sip.reply(pub, 200, "OK", "SIP-ETag: v-etag\r\nExpires: 4294967295\r\n");
        ASSERT_TRUE(l.wait([&] { return l.lastIs(McService::McVideo, ServiceAuthState::Authorized); }));
        EXPECT_TRUE(sip.recv("PUBLISH ", 300).empty());               // MCData·MCPTT 는 켜지 않았거나 PSI 가 없다
        EXPECT_EQ(eng.serviceAuth(acc, McService::McData).code, 0);

        ASSERT_TRUE(eng.setMcVideoEnabled(acc, false).ok);
        std::string rm = sip.recv("PUBLISH ");
        ASSERT_FALSE(rm.empty());
        for (const char* h : {"Event", "Expires"}) EXPECT_EQ(header(rm, h), header(g16, h)) << h;
        EXPECT_NE(header(rm, "P-Preferred-Service").find("icsi.mcvideo"), std::string::npos);
        EXPECT_EQ(header(rm, "SIP-If-Match"), "v-etag");
        EXPECT_EQ(header(rm, "Content-Length"), "0");
        sip.reply(rm, 200, "OK", "SIP-ETag: v-etag-2\r\nExpires: 0\r\n");
        ASSERT_TRUE(l.wait([&] { return l.lastIs(McService::McVideo, ServiceAuthState::Unauthorized); }));
        std::string rereg = sip.recv("REGISTER ");                    // 태그를 뺀 재-REGISTER(§7.2.1AA NOTE)
        ASSERT_FALSE(rereg.empty());
        EXPECT_EQ(header(rereg, "Contact").find("mcvideo"), std::string::npos);
        sip.reply(rereg, 200, "OK", "Contact: " + header(rereg, "Contact") + ";expires=3600\r\nExpires: 3600\r\n");
        EXPECT_TRUE(sip.recv("PUBLISH ", 500).empty());               // 로그오프한 서비스는 다시 인가하지 않는다
        for (auto& r : l.results) EXPECT_NE(r.method, "PUBLISH") << "core requests stay internal";
    }
    eng.stop();
}
