// libcsim TLS 상호인증 단위시험 — psip 클라이언트 인증서 제시·서버 인증서 검증(SSLConnect 연결 단위)과 리스너의 클라이언트 인증서 요구.
//   openssl CLI 로 임시 CA·서버·클라이언트 인증서를 만들고 CsimPeer 둘을 TLS 로 띄운다:
//   A(수신점 TLS, tlsClientAuth — CA 로 클라이언트 인증서 요구) ← B(발신, clientCert 제시 + tlsVerifyServer) → 착신 도달.
//   C(클라이언트 인증서 없음) → A: 핸드셰이크 실패 → 착신 없음·B/C 쪽 호 종료(psip 연결 오류 합성 응답).
//   D(tlsVerifyServer, 앵커 = 다른 CA) → A: 서버 검증 실패 → 착신 없음.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

#include "CsimPeer.h"

struct Obs : ICsimPeerObserver {
    std::atomic<int> incoming{0}, callEnd{0}, callStart{0};
    std::atomic<int> endStatus{0};
    void OnPeerIncoming(CsimPeer*, const std::string&, const std::string&, const std::string&, bool) override { incoming++; }
    void OnPeerCallStart(CsimPeer*, const std::string&, long long) override { callStart++; }
    void OnPeerCallEnd(CsimPeer*, const std::string&, int st, int) override { endStatus = st; callEnd++; }
};

static bool waitFor(std::atomic<int>& v, int want, int ms) {
    for (int i = 0; i < ms / 20; ++i) { if (v.load() >= want) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return v.load() >= want;
}

static bool sh(const std::string& cmd) { int rc = system((cmd + " >/dev/null 2>&1").c_str()); return rc == 0; }

/** CA(자체 서명) + leaf(서버/클라이언트 겸용 — CN 만 다름) — 시험 전용 2048 RSA, 1일 */
static bool makeCa(const std::string& dir, const std::string& name) {
    return sh("openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=" + name + " -keyout " + dir + "/" + name + ".key -out " + dir + "/" + name + ".crt");
}
static bool makeLeaf(const std::string& dir, const std::string& ca, const std::string& name) {
    return sh("openssl req -newkey rsa:2048 -nodes -subj /CN=" + name + " -keyout " + dir + "/" + name + ".key -out " + dir + "/" + name + ".csr") &&
           sh("openssl x509 -req -days 1 -in " + dir + "/" + name + ".csr -CA " + dir + "/" + ca + ".crt -CAkey " + dir + "/" + ca + ".key -CAcreateserial -out " + dir + "/" + name + ".crt");
}

int main() {
    char tmpl[] = "/tmp/csim_tls_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("mkdtemp failed\n"); return 2; }
    std::string d = dir;
    if (!makeCa(d, "ca") || !makeCa(d, "otherca") || !makeLeaf(d, "ca", "server") || !makeLeaf(d, "ca", "client") || !makeLeaf(d, "ca", "peerc") || !makeLeaf(d, "ca", "peerd")) {
        printf("openssl cert generation failed (openssl CLI required)\n"); return 2;
    }
    bool all = true;
    auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); all = all && ok; };

    CsimPeerConfig ca;
    ca.name = "A"; ca.profile = "ibcf"; ca.bindIp = "127.0.0.1"; ca.port = 25091; ca.domain = "a.test"; ca.transport = E_SIP_TLS;
    ca.certFile = d + "/server.crt"; ca.keyFile = d + "/server.key"; ca.caCertFile = d + "/ca.crt"; ca.tlsClientAuth = true; ca.prack = false;
    CsimPeerConfig cb;
    cb.name = "B"; cb.profile = "ibcf"; cb.bindIp = "127.0.0.1"; cb.port = 25092; cb.domain = "b.test"; cb.transport = E_SIP_TLS;
    cb.certFile = d + "/client.crt"; cb.keyFile = d + "/client.key"; cb.caCertFile = d + "/ca.crt"; cb.tlsVerifyServer = true;
    cb.clientCertFile = d + "/client.crt"; cb.clientKeyFile = d + "/client.key"; cb.prack = false;
    CsimPeerConfig cc = cb;   // 클라이언트 인증서 없음
    cc.name = "C"; cc.port = 25093; cc.domain = "c.test"; cc.certFile = d + "/peerc.crt"; cc.keyFile = d + "/peerc.key"; cc.clientCertFile.clear(); cc.clientKeyFile.clear(); cc.tlsVerifyServer = false;
    CsimPeerConfig cd = cb;   // 다른 CA 를 앵커로 서버 검증 → 실패
    cd.name = "D"; cd.port = 25094; cd.domain = "d.test"; cd.certFile = d + "/peerd.crt"; cd.keyFile = d + "/peerd.key"; cd.caCertFile = d + "/otherca.crt";
    CsimPeer a(ca), b(cb), c(cc), dd(cd);
    Obs oa, ob, oc, od;
    a.SetObserver(&oa); b.SetObserver(&ob); c.SetObserver(&oc); dd.SetObserver(&od);
    std::string err;
    if (!a.Start(err)) { printf("A start failed: %s\n", err.c_str()); return 2; }
    if (!b.Start(err)) { printf("B start failed: %s\n", err.c_str()); return 2; }
    if (!c.Start(err)) { printf("C start failed: %s\n", err.c_str()); return 2; }
    if (!dd.Start(err)) { printf("D start failed: %s\n", err.c_str()); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ① 상호인증 성립 — B 가 클라이언트 인증서를 제시하고 A 의 서버 인증서를 CA 로 검증한다
    std::string idB = b.StartCall("+8210001", "user1", "a.test", "127.0.0.1", 25091, E_SIP_TLS);
    check(!idB.empty(), "B StartCall over TLS");
    check(waitFor(oa.incoming, 1, 5000), "A incoming (mutual TLS handshake ok)");
    if (!idB.empty()) b.Bye(idB);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ② 클라이언트 인증서 없는 C — A 가 핸드셰이크를 거절해야 한다
    int incBefore = oa.incoming.load();
    std::string idC = c.StartCall("+8210002", "user1", "a.test", "127.0.0.1", 25091, E_SIP_TLS);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(oa.incoming.load() == incBefore, "A rejects client without certificate (no incoming)");
    check(oc.callStart.load() == 0, "C not established");
    printf("     C end=%d status=%d\n", oc.callEnd.load(), oc.endStatus.load());

    // ③ 서버 검증 실패 — D 는 다른 CA 를 앵커로 A 의 인증서를 거절한다
    incBefore = oa.incoming.load();
    std::string idD = dd.StartCall("+8210003", "user1", "a.test", "127.0.0.1", 25091, E_SIP_TLS);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(oa.incoming.load() == incBefore, "D rejects server certificate from unknown CA (no incoming)");
    check(od.callStart.load() == 0, "D not established");
    printf("     D end=%d status=%d\n", od.callEnd.load(), od.endStatus.load());

    a.Stop(); b.Stop(); c.Stop(); dd.Stop();
    sh("rm -rf " + d);
    printf(all ? "PASS\n" : "FAIL\n");
    return all ? 0 : 1;
}
