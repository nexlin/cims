// libcimsue 단위시험 — affiliation PUBLISH 의 entity-tag 처리(RFC 3903) (S1-UE-UNIT)
//   헤드리스 엔진 + 루프백 가짜 ESC(UDP). ETag 가 낡아 412 를 받으면 그 ETag 를 버리고 SIP-If-Match 없는 초기 PUBLISH 로
//   한 번 다시 알리는지(§5), 앱에는 affiliate() 의 token 으로 최종 결과 하나만 오는지 본다. 예전에는 2xx 에서만 ETag 를
//   기록하고 412 에서 버리지 않아 affiliation 갱신이 같은 412 를 되풀이했다.
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/cimsue.h"
#include "pj_scope.h"

using namespace cimsue;

namespace {

struct ResultListener : Listener {
    void onLog(int lv, const std::string& m) override { if (std::getenv("AFF_LOG")) std::fprintf(stderr, "[%d] %s\n", lv, m.c_str()); }   // AFF_LOG=1 이면 엔진 로그
    std::mutex m;
    std::condition_variable cv;
    std::vector<RequestResult> results;
    void onRequestResult(const RequestResult& r) override {
        { std::lock_guard<std::mutex> lk(m); results.push_back(r); }
        cv.notify_all();
    }
    bool waitCount(size_t n, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return results.size() >= n; });
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

/** 게시 상태 서버(ESC) 자리 — PUBLISH 를 받아 정한 코드로 답한다. */
struct FakeEsc {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    pj_sockaddr_in peer;
    FakeEsc() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s);
        pj_sockaddr_in a;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&a, &ip, 0);
        pj_sock_bind(s, &a, sizeof(a));
        int l = sizeof(a);
        pj_sock_getsockname(s, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeEsc() { if (s != PJ_INVALID_SOCKET) pj_sock_close(s); }

    /** PUBLISH 한 건을 기다린다(keepalive 등 다른 것은 건너뜀). */
    std::string recvPublish(int ms = 3000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char buf[8192];
            pj_ssize_t n = sizeof(buf) - 1;
            int al = sizeof(peer);
            if (pj_sock_recvfrom(s, buf, &n, 0, &peer, &al) != PJ_SUCCESS || n <= 0) continue;
            std::string msg(buf, (size_t)n);
            if (msg.compare(0, 8, "PUBLISH ") == 0) return msg;
        }
        return "";
    }

    void reply(const std::string& req, int code, const char* reason, const std::string& etag) {
        std::string r = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
        size_t p = 0;
        while ((p = req.find("\r\nVia:", p)) != std::string::npos) {           // Via 는 받은 순서 그대로
            size_t e = req.find("\r\n", p + 2);
            r += req.substr(p + 2, e - p - 2) + "\r\n";
            p = e;
        }
        std::string to = headerOf(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=esc";
        r += "From: " + headerOf(req, "From") + "\r\n" + "To: " + to + "\r\n" + "Call-ID: " + headerOf(req, "Call-ID") + "\r\n" +
             "CSeq: " + headerOf(req, "CSeq") + "\r\n";
        if (!etag.empty()) r += "SIP-ETag: " + etag + "\r\nExpires: 3600\r\n";
        r += "Content-Length: 0\r\n\r\n";
        pj_ssize_t len = (pj_ssize_t)r.size();
        pj_sock_sendto(s, r.data(), &len, 0, &peer, sizeof(peer));
    }
};

}  // namespace

// RFC 3903 §5 — 412 를 낸 entity-tag 는 버리고(MUST) 같은 요청을 다시 보내지 않으며(MUST NOT), 초기 PUBLISH 로 상태를 다시 알린다(SHOULD).
TEST(AffiliationPublish, StaleEtag412FallsBackToInitialPublish) {
    Engine eng;
    ResultListener l;
    EngineConfig cfg;
    cfg.logLevel = std::getenv("AFF_LOG") ? 5 : 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    {
        // 엔진이 pjlib 을 먼저 초기화한 뒤(ue-ctl 등록) 이 스레드를 등록한다 — 순서가 바뀌면 ue-ctl 이 미등록이 된다(pj_scope.h).
        cimsue_test::PjScope pj("affil-test");
        FakeEsc esc;
        AccountConfig ac;
        ac.serverHost = "127.0.0.1";
        ac.serverPort = esc.port;
        ac.transport = Transport::UDP;
        ac.domain = "ptt.test";
        ac.msisdn = "+82500000001";
        ac.authId = "450000000000001@ptt.test";                          // IMPI 는 필수(isComplete)
        ac.password = "x";
        int acc = eng.addAccount(ac);
        ASSERT_GE(acc, 0);

        // ① 첫 affiliation — 초기 PUBLISH, 200 + SIP-ETag e1
        int64_t t1 = eng.affiliate(acc, "g001", true);
        ASSERT_GE(t1, 0);
        std::string p1 = esc.recvPublish();
        ASSERT_FALSE(p1.empty());
        EXPECT_EQ(headerOf(p1, "SIP-If-Match"), "");
        esc.reply(p1, 200, "OK", "e1");
        ASSERT_TRUE(l.waitCount(1, 3000));

        // ② 갱신 — SIP-If-Match e1 → ESC 가 모른다(412) → 코어가 e1 을 버리고 초기 PUBLISH 로 다시 → 200 + e2
        int64_t t2 = eng.affiliate(acc, "g001", true);
        ASSERT_GE(t2, 0);
        std::string p2 = esc.recvPublish();
        ASSERT_FALSE(p2.empty());
        EXPECT_EQ(headerOf(p2, "SIP-If-Match"), "e1");
        esc.reply(p2, 412, "Conditional Request Failed", "");
        std::string p3 = esc.recvPublish();
        ASSERT_FALSE(p3.empty()) << "412 뒤 초기 PUBLISH 가 없다";
        EXPECT_EQ(headerOf(p3, "SIP-If-Match"), "");
        EXPECT_NE(headerOf(p3, "Call-ID") + headerOf(p3, "CSeq"), headerOf(p2, "Call-ID") + headerOf(p2, "CSeq"));   // 같은 요청의 재전송이 아니다
        esc.reply(p3, 200, "OK", "e2");
        ASSERT_TRUE(l.waitCount(2, 3000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));     // 412 가 따로 올라오지 않는지
        {
            std::lock_guard<std::mutex> lk(l.m);
            ASSERT_EQ(l.results.size(), 2u);                             // 412 는 앱에 올라가지 않는다
            EXPECT_EQ(l.results[0].token, t1);
            EXPECT_EQ(l.results[0].code, 200);
            EXPECT_EQ(l.results[1].token, t2);                           // 재발행 결과가 affiliate() 의 token 으로
            EXPECT_EQ(l.results[1].code, 200);
            EXPECT_EQ(l.results[1].etag, "e2");
        }

        // ③ 다음 갱신은 새 ETag 로 조건부
        int64_t t3 = eng.affiliate(acc, "g001", false);
        ASSERT_GE(t3, 0);
        std::string p4 = esc.recvPublish();
        ASSERT_FALSE(p4.empty());
        EXPECT_EQ(headerOf(p4, "SIP-If-Match"), "e2");
        EXPECT_EQ(headerOf(p4, "Expires"), "0");
        esc.reply(p4, 200, "OK", "");
        ASSERT_TRUE(l.waitCount(3, 3000));
    }
    eng.stop();
}
