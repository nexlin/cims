// libcimsue 단위시험 — 규격형 제휴 게시(TS 24.379 §9.2.1.2)와 등록에 묶인 것의 유지 (S1-UE-UNIT, ue_sdk.md §4.2)
//   헤드리스 엔진 + 루프백 가짜 서버(UDP — REGISTER·PUBLISH·SUBSCRIBE 에 답한다). 보는 것:
//   ① 제휴 게시 = 참여 기능 PSI 로, Event presence · Expires 2^32-1 · ICSI · mcptt-info + pidf(관심 그룹 전부)
//   ② 망 변경 뒤 첫 등록 성공에 코어가 제휴(조건 없는 초기 게시)와 구독을 다시 싣고, 제휴 결과는 앱에 올리지 않는다
//   ③ 관심 그룹이 비면 Expires 0
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/cimsue.h"
#include "pj_scope.h"

using namespace cimsue;

namespace {

struct UpkeepListener : Listener {
    void onLog(int lv, const std::string& m) override { if (std::getenv("AFF_LOG")) std::fprintf(stderr, "[%d] %s\n", lv, m.c_str()); }
    std::mutex m;
    std::condition_variable cv;
    std::vector<RequestResult> results;
    int registered = 0;
    void onRequestResult(const RequestResult& r) override {
        { std::lock_guard<std::mutex> lk(m); results.push_back(r); }
        cv.notify_all();
    }
    void onRegState(const RegInfo& r) override {
        { std::lock_guard<std::mutex> lk(m); if (r.state == RegState::Registered) ++registered; }
        cv.notify_all();
    }
    bool waitResults(size_t n, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return results.size() >= n; });
    }
    bool waitRegistered(int n, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return registered >= n; });
    }
    size_t resultCount() { std::lock_guard<std::mutex> lk(m); return results.size(); }
};

std::string headerOf(const std::string& msg, const std::string& name) {
    size_t p = 0;
    while ((p = msg.find("\r\n", p)) != std::string::npos) {
        p += 2;
        if (p >= msg.size() || msg.compare(p, 2, "\r\n") == 0) break;
        if (msg.compare(p, name.size() + 1, name + ":") == 0) {
            size_t v = p + name.size() + 1, e = msg.find("\r\n", v);
            while (v < e && msg[v] == ' ') ++v;
            return msg.substr(v, e - v);
        }
    }
    return "";
}

/** 등록·게시·구독 서버 자리 — 요청을 메서드별로 꺼내고, 정한 코드로 답한다. */
struct FakeServer {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    struct Rx { std::string msg; pj_sockaddr_in from; };
    std::deque<Rx> backlog;
    pj_sockaddr_in last;
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

    /** method 요청 한 건 — 다른 메서드는 뒤로 미뤄 둔다. 재전송(같은 Call-ID·CSeq)은 건너뛴다. */
    std::string recv(const std::string& method, int ms = 3000) {
        for (auto it = backlog.begin(); it != backlog.end(); ++it) {
            if (it->msg.compare(0, method.size() + 1, method + " ") == 0) {
                std::string m = it->msg;
                last = it->from;
                backlog.erase(it);
                return m;
            }
        }
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char buf[16384];
            pj_ssize_t n = sizeof(buf) - 1;
            pj_sockaddr_in from;
            int al = sizeof(from);
            if (pj_sock_recvfrom(s, buf, &n, 0, &from, &al) != PJ_SUCCESS || n <= 0) continue;
            std::string msg(buf, (size_t)n);
            if (msg.compare(0, 4, "SIP/") == 0 || msg.size() < 8) continue;          // 응답·keepalive
            if (seen(msg)) continue;
            if (msg.compare(0, method.size() + 1, method + " ") == 0) { last = from; return msg; }
            backlog.push_back({msg, from});
        }
        return "";
    }
    /** ms 동안 method 요청이 오지 않는다. */
    bool none(const std::string& method, int ms) { return recv(method, ms).empty(); }

    std::vector<std::string> ids;
    bool seen(const std::string& msg) {
        const std::string id = headerOf(msg, "Call-ID") + "/" + headerOf(msg, "CSeq");
        for (const auto& x : ids) if (x == id) return true;
        ids.push_back(id);
        return false;
    }

    void reply(const std::string& req, int code, const std::string& extra) {
        std::string r = "SIP/2.0 " + std::to_string(code) + (code / 100 == 2 ? " OK" : " Forbidden") + "\r\n";
        size_t p = 0;
        while ((p = req.find("\r\nVia:", p)) != std::string::npos) {
            size_t e = req.find("\r\n", p + 2);
            r += req.substr(p + 2, e - p - 2) + "\r\n";
            p = e;
        }
        std::string to = headerOf(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=srv";
        r += "From: " + headerOf(req, "From") + "\r\n" + "To: " + to + "\r\n" + "Call-ID: " + headerOf(req, "Call-ID") + "\r\n" +
             "CSeq: " + headerOf(req, "CSeq") + "\r\n" + extra + "Content-Length: 0\r\n\r\n";
        pj_ssize_t len = (pj_ssize_t)r.size();
        pj_sock_sendto(s, r.data(), &len, 0, &last, sizeof(last));
    }
    void replyRegister(const std::string& req) {
        reply(req, 200, "Contact: " + headerOf(req, "Contact") + ";expires=300\r\nExpires: 300\r\n");
    }
};

}  // namespace

TEST(AffiliationUpkeep, SpecFormPublishAndRenewAfterNetworkChange) {
    Engine eng;
    UpkeepListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("AFF_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        cimsue_test::PjScope pj("upkeep-test");
        FakeServer srv;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1";
        ac.serverPort = srv.port;
        ac.transport = Transport::UDP;
        ac.domain = "ptt.test";
        ac.msisdn = "+82500000001";
        ac.authId = "450000000000001@ptt.test";
        ac.password = "x";
        ac.mcpttServerUri = "sip:mcptt_psi@ptt.test";
        ac.mcpttClientId = "urn:uuid:00000000-0000-0000-0000-000000000001";
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);
        ASSERT_TRUE(eng.registerAccount(acc).ok);
        std::string reg = srv.recv("REGISTER");
        ASSERT_FALSE(reg.empty());
        srv.replyRegister(reg);
        ASSERT_TRUE(l.waitRegistered(1, 3000));

        // ① 규격형 제휴 게시 — 그룹을 더할 때마다 관심 그룹 전부
        int64_t t1 = eng.affiliate(acc, "g001", true);
        ASSERT_GE(t1, 0);
        std::string p1 = srv.recv("PUBLISH");
        ASSERT_FALSE(p1.empty());
        EXPECT_EQ(p1.compare(0, 31, "PUBLISH sip:mcptt_psi@ptt.test "), 0) << p1.substr(0, 60);
        EXPECT_EQ(headerOf(p1, "Event"), "presence");
        EXPECT_EQ(headerOf(p1, "Expires"), "4294967295");
        EXPECT_EQ(headerOf(p1, "P-Preferred-Service"), "urn:urn-7:3gpp-service.ims.icsi.mcptt");
        EXPECT_NE(p1.find("<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:+82500000001</mcpttURI>"), std::string::npos);
        EXPECT_NE(p1.find("entity=\"tel:+82500000001\""), std::string::npos);
        EXPECT_NE(p1.find("<tuple id=\"urn:uuid:00000000-0000-0000-0000-000000000001\">"), std::string::npos);
        EXPECT_NE(p1.find("group=\"tel:g001\""), std::string::npos);
        EXPECT_EQ(p1.find("status="), std::string::npos);                        // §9.2.1.2 5)b)iii)
        srv.reply(p1, 200, "SIP-ETag: e1\r\nExpires: 4294967295\r\n");
        ASSERT_TRUE(l.waitResults(1, 3000));                                       // ETag 를 적은 뒤 다음 게시
        int64_t t2 = eng.affiliate(acc, "g002", true);
        std::string p2 = srv.recv("PUBLISH");
        ASSERT_FALSE(p2.empty());
        EXPECT_NE(p2.find("group=\"tel:g001\""), std::string::npos);
        EXPECT_NE(p2.find("group=\"tel:g002\""), std::string::npos);
        EXPECT_EQ(headerOf(p2, "SIP-If-Match"), "e1");
        srv.reply(p2, 200, "SIP-ETag: e2\r\nExpires: 4294967295\r\n");
        ASSERT_TRUE(l.waitResults(2, 3000));
        ASSERT_TRUE(eng.subscribeConference(acc, "g001", true).ok);
        std::string s1 = srv.recv("SUBSCRIBE");
        ASSERT_FALSE(s1.empty());
        EXPECT_EQ(headerOf(s1, "Event"), "conference");
        srv.reply(s1, 200, "Contact: <sip:srv@127.0.0.1:" + std::to_string(srv.port) + ">\r\nExpires: 3600\r\n");                                 // 구독은 스택(evsub)이 다루고 결과는 올라오지 않는다
        {
            std::lock_guard<std::mutex> lk(l.m);
            EXPECT_EQ(l.results[0].token, t1);
            EXPECT_EQ(l.results[1].token, t2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const size_t before = l.resultCount();

        // ② 망 변경 → 재등록 성공 → 코어가 제휴(초기 게시)·구독을 다시 싣는다, 결과는 앱에 올리지 않는다
        ASSERT_TRUE(eng.handleNetworkChange().ok);
        std::string reg2 = srv.recv("REGISTER");
        ASSERT_FALSE(reg2.empty());
        srv.replyRegister(reg2);
        ASSERT_TRUE(l.waitRegistered(2, 3000));
        std::string p3 = srv.recv("PUBLISH");
        ASSERT_FALSE(p3.empty()) << "재등록 뒤 제휴 재게시가 없다";
        EXPECT_EQ(headerOf(p3, "SIP-If-Match"), "");                             // 서버가 잃었을 수 있다 — 초기 게시
        EXPECT_NE(p3.find("group=\"tel:g001\""), std::string::npos);
        EXPECT_NE(p3.find("group=\"tel:g002\""), std::string::npos);
        srv.reply(p3, 200, "SIP-ETag: e3\r\nExpires: 4294967295\r\n");
        std::string s2 = srv.recv("SUBSCRIBE");
        ASSERT_FALSE(s2.empty()) << "재등록 뒤 conference 재구독이 없다";
        EXPECT_EQ(headerOf(s2, "Event"), "conference");
        srv.reply(s2, 200, "Contact: <sip:srv@127.0.0.1:" + std::to_string(srv.port) + ">\r\nExpires: 3600\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(l.resultCount(), before);                                        // 내부 요청 결과는 올라가지 않는다

        // 같은 등록의 갱신은 다시 싣지 않는다
        ASSERT_TRUE(eng.refreshRegistration(acc).ok);
        std::string reg3 = srv.recv("REGISTER");
        ASSERT_FALSE(reg3.empty());
        srv.replyRegister(reg3);
        ASSERT_TRUE(l.waitRegistered(3, 3000));
        EXPECT_TRUE(srv.none("PUBLISH", 500));

        // ③ 관심 그룹이 비면 Expires 0
        eng.affiliate(acc, "g001", false);
        std::string p4 = srv.recv("PUBLISH");
        ASSERT_FALSE(p4.empty());
        EXPECT_EQ(headerOf(p4, "Expires"), "4294967295");
        EXPECT_EQ(p4.find("group=\"tel:g001\""), std::string::npos);
        srv.reply(p4, 200, "SIP-ETag: e4\r\nExpires: 4294967295\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        eng.affiliate(acc, "g002", false);
        std::string p5 = srv.recv("PUBLISH");
        ASSERT_FALSE(p5.empty());
        EXPECT_EQ(headerOf(p5, "Expires"), "0");
        srv.reply(p5, 200, "");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    eng.stop();
}
