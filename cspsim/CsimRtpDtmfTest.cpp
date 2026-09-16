// libcsim RTP 단위시험 — RFC 4733 telephone-event 송수신(CRtpThread) 루프백. S1-UNIT-TESTER 의 네이티브 항목.
//   A → B 로 합성 PCMU 를 보내며 숫자열 "12#" 을 이벤트로 끼워 넣고, B 가 E 비트 기준으로 3 이벤트·숫자열을 받는지 본다.
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
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
