// libcsim RTP 단위시험 — RFC 4733 telephone-event 송수신(CRtpThread) 루프백. S1-UNIT-TESTER 의 네이티브 항목.
//   A → B 로 합성 PCMU 를 보내며 숫자열 "12#" 을 이벤트로 끼워 넣고, B 가 E 비트 기준으로 3 이벤트·숫자열을 받는지 본다.
//   + in-band DTMF(G.711 톤 → Goertzel 검출, 비 G.711 거절) + G.722(PT 9) 합성 원천 루프백.
#include <chrono>
#include <cstdio>
#include <thread>

#include "RtpThread.h"

int main() {
    CRtpThread a, b;
    if (!a.Create() || !b.Create()) { printf("create failed\n"); return 2; }
    a.m_iDtmfPt = 101; a.m_iDtmfClock = 8000; a.m_iAudioPt = 0;
    b.m_iDtmfPt = 101; b.m_iDtmfClock = 8000; b.m_iAudioPt = 0;
    if (!a.Start("127.0.0.1", b.m_iPort) || !b.Start("127.0.0.1", a.m_iPort)) { printf("start failed\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!a.SendDtmf("12#", 160, 100)) { printf("SendDtmf refused\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    int sent = a.m_iDtmfSent.load(), recv = b.m_iDtmfRecv.load();
    std::string digits = b.DtmfRecv();
    unsigned long long rx = b.m_ullRecvTotal.load(), lost = b.m_ullRecvLost.load();
    printf("sent=%d recv=%d digits=%s rx=%llu lost=%llu jitter_us=%lld\n", sent, recv, digits.c_str(), rx, lost,
           (long long)b.m_llRecvJitterUs.load());
    a.Stop(); b.Stop();
    bool ok = sent == 3 && recv == 3 && digits == "12#" && lost == 0;
    printf("%s rfc4733\n", ok ? "ok  " : "FAIL");

    // in-band(DtmfInband.h) — telephone-event 미협상, G.711(PCMU) 톤으로 "4*7" 을 내고 Goertzel 로 받는다. 간격은 무음.
    CRtpThread c, d;
    if (!c.Create() || !d.Create()) { printf("create failed\n"); return 2; }
    c.m_iDtmfPt = -1; c.m_iAudioPt = 0; c.m_bDtmfInband = true;
    d.m_iDtmfPt = -1; d.m_iAudioPt = 0; d.m_bDtmfInband = true;
    if (!c.Start("127.0.0.1", d.m_iPort) || !d.Start("127.0.0.1", c.m_iPort)) { printf("start failed\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!c.SendDtmf("4*7", 160, 100)) { printf("inband SendDtmf refused\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    int sent2 = c.m_iDtmfSent.load(), recv2 = d.m_iDtmfRecv.load();
    std::string digits2 = d.DtmfRecv();
    printf("inband sent=%d recv=%d digits=%s rx=%llu lost=%llu\n", sent2, recv2, digits2.c_str(), d.m_ullRecvTotal.load(), d.m_ullRecvLost.load());
    bool ok2 = sent2 == 3 && recv2 == 3 && digits2 == "4*7" && d.m_ullRecvLost.load() == 0;
    printf("%s inband\n", ok2 ? "ok  " : "FAIL");
    // in-band 는 G.711 에서만 — AMR-WB 합의(PT 96)면 거절
    CRtpThread e;
    e.m_iDtmfPt = -1; e.m_iAudioPt = 96; e.m_bDtmfInband = true;
    bool ok3 = !e.SendDtmf("1");
    printf("%s inband refused on non-G.711\n", ok3 ? "ok  " : "FAIL");
    // G.722(PT 9) 합성 원천 — 160 B 프레임이 8 kHz 클록으로 흐른다(수신 측 PT 9·손실 0)
    CRtpThread f, g;
    if (!f.Create() || !g.Create()) { printf("create failed\n"); return 2; }
    f.m_iAudioPt = 9; g.m_iAudioPt = 9; f.m_bUseMediaFile = false; g.m_bUseMediaFile = false;
    if (!f.Start("127.0.0.1", g.m_iPort) || !g.Start("127.0.0.1", f.m_iPort)) { printf("start failed\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    unsigned long long grx = g.m_ullRecvTotal.load();
    int gpt = g.m_iRecvPt.load();
    printf("g722 rx=%llu pt=%d lost=%llu jitter_us=%lld\n", grx, gpt, g.m_ullRecvLost.load(), (long long)g.m_llRecvJitterUs.load());
    bool ok4 = grx >= 20 && gpt == 9 && g.m_ullRecvLost.load() == 0;
    printf("%s g722 synthetic source\n", ok4 ? "ok  " : "FAIL");
    c.Stop(); d.Stop(); f.Stop(); g.Stop();
    bool all = ok && ok2 && ok3 && ok4;
    printf(all ? "PASS\n" : "FAIL\n");
    return all ? 0 : 1;
}
