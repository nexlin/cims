// DeviceHub 단위시험 — 시험 모드 실기기 계측 링크 수신점(ue_voice_quality.md §5·§6). cims-verify S1-UNIT-TESTER 가 실행한다.
//   OpenSSL 클라이언트가 단말 흉내: hello → welcome/bye, 연결 키 거절, 명령 result, reg 추적, 풀 bind 이벤트, 같은 device_id 재접속,
//   링크 끊김(process_exit), 자체 서명 인증서 생성·재사용(같은 지문).
#include "DeviceHub.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <openssl/ssl.h>

static int g_fail = 0;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail++; }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/** 단말 흉내 — TLS 클라이언트, 줄 입출력. */
struct FakeDevice {
    int fd = -1; SSL_CTX* ctx = nullptr; SSL* ssl = nullptr; std::string buf;
    bool connect(int port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) return false;
        ctx = SSL_CTX_new(TLS_client_method());
        ssl = SSL_new(ctx); SSL_set_fd(ssl, fd);
        return SSL_connect(ssl) == 1;
    }
    void send(const std::string& l) { std::string x = l + "\n"; SSL_write(ssl, x.data(), (int)x.size()); }
    /** 줄 하나(시한 ms). 없으면 빈 문자열. */
    std::string line(int ms = 3000) {
        struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) { std::string l = buf.substr(0, nl); buf.erase(0, nl + 1); return l; }
            char t[2048]; int n = SSL_read(ssl, t, sizeof t);
            if (n <= 0) return "";
            buf.append(t, (size_t)n);
        }
    }
    void close() { if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; } if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; } if (fd >= 0) { ::close(fd); fd = -1; } }
    ~FakeDevice() { close(); }
};

static std::string hello(const std::string& devId, const std::string& key, bool registered) {
    return std::string("{\"event\":\"hello\",\"proto\":1,\"device_id\":\"") + devId + "\",\"app\":\"test\",\"version\":\"1\",\"platform\":\"linux\","
           "\"model\":\"m\",\"pair_key\":\"" + key + "\",\"accounts\":[{\"service\":\"volte\",\"aor\":\"sip:+8210@x\",\"msisdn\":\"+8210\",\"registered\":" +
           (registered ? "true" : "false") + "}],\"test_mode\":true}";
}

int main() {
    signal(SIGPIPE, SIG_IGN);   // 워커(main.cpp)와 같다 — 끊긴 링크에 쓰면 EPIPE 로 받는다
    const int port = 17000 + (getpid() % 2000);
    char dir[] = "/tmp/device_hub_test_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    DeviceHubConfig cfg;
    cfg.ip = "127.0.0.1"; cfg.port = port; cfg.certFile = std::string(dir) + "/d.crt"; cfg.keyFile = std::string(dir) + "/d.key";
    cfg.pairKey = "k1"; cfg.workerName = "w-test";
    std::string fp1;
    {
        DeviceHub hub(cfg);
        std::string err;
        check(hub.start(err), "hub listens (self-signed cert created)");
        fp1 = hub.fingerprint();
        check(fp1.size() == 64 && access(cfg.keyFile.c_str(), R_OK) == 0, "fingerprint sha-256 + key file");

        {   // 연결 키 거절
            FakeDevice d;
            check(d.connect(port), "tls connect");
            d.send(hello("dev-a", "WRONG", true));
            std::string l = d.line();
            check(l.find("\"bye\"") != std::string::npos && l.find("pair_key") != std::string::npos, "wrong pair key → bye{pair_key}");
        }
        FakeDevice d;
        check(d.connect(port), "tls connect #2");
        d.send(hello("dev-a", "k1", false));
        std::string w = d.line();
        check(w.find("\"welcome\"") != std::string::npos && w.find("w-test") != std::string::npos, "welcome{worker}");
        sleepMs(100);
        auto conn = hub.find("+8210", "volte");
        check(conn != nullptr && hub.connected() == 1, "find by msisdn/service");
        check(hub.find("+8210", "ptt") == nullptr, "service mismatch → none");
        check(conn && !conn->registered("volte"), "registered from hello (false)");

        // 명령 → result (단말 스레드가 답한다)
        std::thread responder([&] {
            std::string cmd = d.line();
            d.send("{\"event\":\"result\",\"op\":\"use\",\"ok\":true,\"call\":-1,\"code\":0,\"reason\":\"echo " + cmd + "\"}");
            d.send("{\"event\":\"reg\",\"service\":\"volte\",\"state\":\"registered\",\"code\":200}");
            d.send("{\"event\":\"call\",\"call\":3,\"dir\":\"out\",\"state\":\"active\",\"srd_ms\":90}");
        });
        std::vector<std::string> got;
        std::mutex gm;
        conn->bind([&](const Json& ev) { std::lock_guard<std::mutex> lk(gm); got.push_back(ev["event"].asString()); }, "pool-x");
        Json r = conn->request("use volte", 3000);
        responder.join();
        check(r["ok"].asBool(false) && r["reason"].asString() == "echo use volte", "request → result");
        sleepMs(200);
        check(conn->registered("volte"), "reg event updates registration");
        {
            std::lock_guard<std::mutex> lk(gm);
            check(got.size() == 2 && got[0] == "reg" && got[1] == "call", "bound pool receives events (result excluded)");
        }
        check(conn->info().boundPool == "pool-x", "info.boundPool");
        // 단말 ping → 워커 pong
        d.send("{\"event\":\"ping\"}");
        check(d.line() == "pong", "device ping → pong");

        // 같은 device_id 재접속 — 옛 연결은 닫히고 바인딩 풀은 끊김을 본다
        FakeDevice d2;
        check(d2.connect(port), "reconnect");
        d2.send(hello("dev-a", "k1", true));
        check(d2.line().find("welcome") != std::string::npos, "reconnect welcome");
        sleepMs(200);
        {
            std::lock_guard<std::mutex> lk(gm);
            check(!got.empty() && got.back() == "process_exit", "old link closed → process_exit to bound pool");
        }
        check(!conn->alive() && conn->request("x", 200)["reason"].asString() == "device link lost", "dead link request fails fast");
        auto conn2 = hub.find("+8210", "volte");
        check(conn2 && conn2 != conn && conn2->registered("volte") && conn2->info().boundPool.empty(), "new link is free and registered");
        conn->unbind();
        d2.close();
        sleepMs(300);
        check(hub.connected() == 0 && hub.find("+8210", "") == nullptr, "client close → not connected");
        hub.stop();
    }
    {   // 재기동 — 같은 인증서 파일을 다시 쓴다(단말 TOFU 지문 유지)
        DeviceHub hub(cfg);
        std::string err;
        check(hub.start(err) && hub.fingerprint() == fp1, "restart keeps fingerprint");
        hub.stop();
    }
    unlink(cfg.certFile.c_str()); unlink(cfg.keyFile.c_str()); rmdir(dir);
    printf(g_fail == 0 ? "tester_device_hub_test: PASS\n" : "tester_device_hub_test: FAIL (%d)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
