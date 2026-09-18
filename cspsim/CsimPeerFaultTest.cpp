// libcsim 피어 엔진 단위시험 — 오류 주입 후속(재전송 유실·THIG 흔적)·G.722 협상. S1-UNIT-TESTER 의 네이티브 항목.
//   같은 프로세스에 CsimPeer 둘을 UDP 루프백으로 띄운다: A(착신, fault drop_invite=1 — 새 INVITE 첫 벌을 와이어 유실처럼 버림, 코덱 G722)
//   ← B(발신 ibcf, thig — 토큰화 Via 를 얹음, 코덱 G722). 기대 = A 가 첫 벌을 버리고(OnPeerWireDrop 1) B 의 Timer A 재전송 벌이 닿아
//   (OnPeerInviteRetrans ≥ 1) 착신이 올라오며 SRD ≥ T1(500 ms), A 의 answer 뒤 B 확립 + B 의 첫 응답에 토큰화 Via 보존(OnPeerThig ok),
//   answer 코덱 = G.722(PT 9) 로 RTP 가 양방향 흐른다.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "CsimPeer.h"

struct Obs : ICsimPeerObserver {
    std::atomic<int> incoming{0}, drops{0}, retrans{0}, callStart{0}, callEnd{0}, thigOk{0}, thigLost{0}, ring{0};
    std::atomic<long long> srd{0};
    std::mutex m; std::string inCallId;
    void OnPeerIncoming(CsimPeer*, const std::string& callId, const std::string&, const std::string&, bool) override {
        std::lock_guard<std::mutex> lk(m); inCallId = callId; incoming++;
    }
    void OnPeerWireDrop(CsimPeer*, const std::string&, const std::string& method) override { if (method == "INVITE") drops++; }
    void OnPeerInviteRetrans(CsimPeer*, const std::string&) override { retrans++; }
    void OnPeerRing(CsimPeer*, const std::string&, int, bool, bool) override { ring++; }
    void OnPeerCallStart(CsimPeer*, const std::string&, long long srdMs) override { srd = srdMs; callStart++; }
    void OnPeerCallEnd(CsimPeer*, const std::string&, int, int) override { callEnd++; }
    void OnPeerThig(CsimPeer*, const std::string&, bool ok) override { if (ok) thigOk++; else thigLost++; }
};

static bool waitFor(std::atomic<int>& v, int want, int ms) {
    for (int i = 0; i < ms / 20; ++i) { if (v.load() >= want) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return v.load() >= want;
}

int main() {
    CsimPeer::EnsureCodecTable();
    bool all = true;
    auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); all = all && ok; };

    CsimPeerConfig ca;
    ca.name = "A"; ca.profile = "pbx"; ca.bindIp = "127.0.0.1"; ca.port = 25081; ca.domain = "pbx.test";
    ca.codecs = { "G722", "PCMU" }; ca.dropInvite = 1; ca.prack = false;
    CsimPeerConfig cb;
    cb.name = "B"; cb.profile = "ibcf"; cb.bindIp = "127.0.0.1"; cb.port = 25082; cb.domain = "ims.other.test";
    cb.codecs = { "G722", "PCMU" }; cb.thig = true; cb.prack = false;
    CsimPeer a(ca), b(cb);
    Obs oa, ob;
    a.SetObserver(&oa); b.SetObserver(&ob);
    std::string err;
    if (!a.Start(err)) { printf("A start failed: %s\n", err.c_str()); return 2; }
    if (!b.Start(err)) { printf("B start failed: %s\n", err.c_str()); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto t0 = std::chrono::steady_clock::now();
    std::string callId = b.StartCall("+8221001000", "2001", "pbx.test", "127.0.0.1", 25081, E_SIP_UDP);
    check(!callId.empty(), "B StartCall");
    // 첫 벌은 A 가 버린다 → 재전송(Timer A = T1 500 ms)이 닿아야 착신이 올라온다
    check(waitFor(oa.incoming, 1, 4000), "A incoming after retransmission");
    long long tInc = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    printf("     incoming after %lld ms · drops=%d retrans=%d\n", tInc, oa.drops.load(), oa.retrans.load());
    check(oa.drops.load() == 1, "A dropped exactly the first INVITE copy");
    check(oa.retrans.load() >= 1, "A observed INVITE retransmission");
    check(tInc >= 400, "incoming delayed by at least ~T1");
    std::string inId; { std::lock_guard<std::mutex> lk(oa.m); inId = oa.inCallId; }
    a.Ring(inId);
    check(waitFor(ob.ring, 1, 2000), "B got 180");
    check(a.Answer(inId) == 0, "A answer (G.722 common codec)");
    check(waitFor(ob.callStart, 1, 3000), "B established");
    printf("     SRD=%lld ms\n", (long long)ob.srd.load());
    check(ob.srd.load() >= 400, "SRD includes the retransmission delay");
    check(ob.thigOk.load() == 1 && ob.thigLost.load() == 0, "THIG tokenized Via preserved in response");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    unsigned long long rxA = 0, lostA = 0, rxB = 0, lostB = 0; long long jA = 0, jB = 0;
    a.RtpStats(inId, rxA, lostA, jA); b.RtpStats(callId, rxB, lostB, jB);
    int ptA = -1, ptB = -1, rr = 0, fl = 0;
    a.RtpQuality(inId, ptA, rr, fl); b.RtpQuality(callId, ptB, rr, fl);
    printf("     rtp A rx=%llu lost=%llu pt=%d · B rx=%llu lost=%llu pt=%d\n", rxA, lostA, ptA, rxB, lostB, ptB);
    check(rxA >= 20 && rxB >= 20 && lostA == 0 && lostB == 0, "RTP flows both ways");
    check(ptA == 9 && ptB == 9, "negotiated wire PT = 9 (G.722)");
    b.Bye(callId);
    check(waitFor(oa.callEnd, 1, 3000), "A saw BYE");
    a.Stop(); b.Stop();
    printf(all ? "PASS\n" : "FAIL\n");
    return all ? 0 : 1;
}
