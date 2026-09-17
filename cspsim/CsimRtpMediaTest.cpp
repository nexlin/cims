// libcsim RTP 단위시험 — 계측기 미디어 평면(CRtpThread 모드·송출 제어) 루프백. S1-UNIT-TESTER 의 네이티브 항목.
//   ① NONE = Start 가 스레드를 띄우지 않는다 ② EXPLICIT = MediaSend 전에는 수신만 ③ G.711 샘플 한 번 재생(loop=false) =
//   프레임 수만큼만 나가고 끝에서 멈춘다 ④ 기본 원천 재개 → MediaStop 뒤 정지, 시퀀스 공백 없음(lost 0)
//   ⑤ 상대 hold 정지 ⑥ AMR-WB 합의 + 합성 = NO_DATA 프레임이 협상 PT 로 흐른다 ⑦ 기본 원천(AMR-WB 파일)은 auto 에서 곧바로.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

#include "RtpThread.h"

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    // 25 프레임(0.5 s) PCMU 샘플 파일
    char szPath[] = "/tmp/csim_media_test_XXXXXX";
    int fd = mkstemp(szPath);
    if (fd < 0) { printf("mkstemp failed\n"); return 2; }
    std::string strFrame(160, (char)0x55);
    for (int i = 0; i < 25; ++i) if (write(fd, strFrame.data(), strFrame.size()) != (ssize_t)strFrame.size()) return 2;
    close(fd);

    {   // ① NONE
        CRtpThread n;
        if (!n.Create()) return 2;
        n.SetMediaMode(CRtpThread::E_MEDIA_NONE);
        check(n.Start("127.0.0.1", 9) && !n.MediaRunning(), "none: Start 가 스레드를 띄우지 않는다");
    }
    {
        CRtpThread a, b;
        if (!a.Create() || !b.Create()) { printf("create failed\n"); return 2; }
        a.m_iAudioPt = 0; b.m_iAudioPt = 0;
        a.SetMediaMode(CRtpThread::E_MEDIA_EXPLICIT);
        if (!a.Start("127.0.0.1", b.m_iPort) || !b.Start("127.0.0.1", a.m_iPort)) { printf("start failed\n"); return 2; }
        sleepMs(400);
        // ② explicit — a 는 받기만
        check(b.m_ullRecvTotal.load() == 0 && a.m_ullRecvTotal.load() > 0, "explicit: MediaSend 전에는 수신만");
        // ③ 샘플 한 번 재생
        a.MediaSend("", szPath, "", false);
        sleepMs(1200);
        unsigned long long rx = b.m_ullRecvTotal.load();
        printf("     sample once: rx=%llu sent=%llu ended=%d\n", rx, a.m_ullSentTotal.load(), a.SourceEnded() ? 1 : 0);
        check(rx == 25 && a.SourceEnded(), "sample loop=false: 25 프레임만 나가고 멈춘다");
        // ④ 기본 원천 재개 → 정지
        a.MediaSendDefault();
        sleepMs(400);
        unsigned long long rx2 = b.m_ullRecvTotal.load();
        check(rx2 > rx, "media_send(기본 원천): 송출 재개");
        a.MediaStop();
        sleepMs(100);
        unsigned long long rx3 = b.m_ullRecvTotal.load();
        sleepMs(300);
        check(b.m_ullRecvTotal.load() == rx3, "media_stop: 송출 정지");
        check(b.m_ullRecvLost.load() == 0, "정지·재개·원천 교체에 시퀀스 공백 없음(lost 0)");
        // ⑤ 상대 hold — b 가 보내지 않는다
        b.SetHoldPaused(true);
        sleepMs(100);
        unsigned long long ax = a.m_ullRecvTotal.load();
        sleepMs(300);
        check(a.m_ullRecvTotal.load() == ax, "hold: 상대 sendonly 동안 송출 정지");
        a.Stop(); b.Stop();
    }
    {   // ⑥ AMR-WB 합의 + 합성
        CRtpThread a, b;
        if (!a.Create() || !b.Create()) { printf("create failed\n"); return 2; }
        a.m_iAudioPt = 100; a.m_bUseMediaFile = true;
        b.m_iAudioPt = 100; b.SetMediaMode(CRtpThread::E_MEDIA_EXPLICIT);
        if (!a.Start("127.0.0.1", b.m_iPort) || !b.Start("127.0.0.1", a.m_iPort)) { printf("start failed\n"); return 2; }
        a.MediaSend("", "", "", true);
        sleepMs(500);
        check(b.m_ullRecvTotal.load() > 10 && b.m_ullRecvLost.load() == 0, "amr-wb synthetic: NO_DATA 프레임이 흐른다");
        a.Stop(); b.Stop();
    }
    {   // ⑦ 기본 원천 = AMR-WB 파일(cspsim -media_file 경로) — 레포 시험 미디어가 있을 때만(작업 디렉터리 = 레포 루트)
        const char* pszAmr = "tests/media/8050001000004_audio.amrwb";
        if (access(pszAmr, R_OK) == 0) {
            CRtpThread a, b;
            if (!a.Create() || !b.Create()) { printf("create failed\n"); return 2; }
            a.SetMediaFile(pszAmr); a.m_iAudioPt = 100; a.m_bUseMediaFile = true;
            b.m_iAudioPt = 100; b.SetMediaMode(CRtpThread::E_MEDIA_EXPLICIT);
            if (!a.Start("127.0.0.1", b.m_iPort) || !b.Start("127.0.0.1", a.m_iPort)) { printf("start failed\n"); return 2; }
            sleepMs(600);
            unsigned long long rx = b.m_ullRecvTotal.load(), tx = a.m_ullSentTotal.load();
            printf("     amr-wb file: tx=%llu rx=%llu lost=%llu\n", tx, rx, b.m_ullRecvLost.load());
            check(rx > 15 && tx >= rx && tx - rx <= 2 && b.m_ullRecvLost.load() == 0, "amr-wb file(기본 원천): auto 모드에서 곧바로 송출");
            a.Stop(); b.Stop();
        } else {
            printf("skip amr-wb file case (%s 없음)\n", pszAmr);
        }
    }
    unlink(szPath);
    printf(g_fail == 0 ? "PASS\n" : "FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
