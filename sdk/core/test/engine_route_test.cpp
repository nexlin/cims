// libcimsue 단위시험 — 재생 라우트(ExtraAudioDevice 재생 전용 패치) 수명 (S1-UE-UNIT)
// 헤드리스(null 장치) 엔진을 실제로 기동해 라우트 추가/호 결선/제거/종료 순서가 assert·크래시 없이 도는지 본다.
// 실장치 2개 출력의 지연·에코는 Windows 실기 검증 항목(ue_sdk.md §11).
#include <gtest/gtest.h>

#include "cimsue/cimsue.h"

using namespace cimsue;

namespace {
struct QuietListener : Listener {};
}

TEST(EngineRoute, AddRemoveOnNullDevice) {
    Engine eng;
    QuietListener l;
    EngineConfig cfg;
    cfg.logLevel = 0;
    cfg.nullAudioDevice = true;
    Result r = eng.start(cfg, &l);
    ASSERT_TRUE(r.ok) << r.reason;

    // 미기동 상태 오류 경로가 아닌, 기동 상태에서의 인자 검증
    EXPECT_FALSE(eng.setCallRoute(0, 99).ok);          // 없는 라우트
    EXPECT_FALSE(eng.removePlaybackRoute(1).ok);       // 없는 라우트

    // 열거된 장치마다 재생 전용 라우트를 열어 본다 — 헤드리스 구성이면 null 장치 하나(또는 0개)
    std::vector<int> opened;
    for (const auto& d : eng.audioDevices()) {
        if (d.outputCount == 0) continue;
        int id = eng.addPlaybackRoute(d.id);
        if (id > 0) opened.push_back(id);
    }
    for (int id : opened) EXPECT_TRUE(eng.removePlaybackRoute(id).ok);
    EXPECT_TRUE(eng.refreshAudioDevices().ok);
    eng.stop();                                        // routes → libDestroy 순서
    EXPECT_FALSE(eng.running());
}

// 캡처 게이트(ue_sdk.md §4.5) — 상태·미기동 오류·기동마다 전이중. 실장치 SPEAKER_ONLY 재오픈은 실기 항목(헤드리스는 상태만).
TEST(EngineCapture, GateStateOnNullDevice) {
    Engine eng;
    QuietListener l;
    EXPECT_FALSE(eng.setCaptureEnabled(false).ok);     // 미기동
    EngineConfig cfg;
    cfg.logLevel = 0;
    cfg.nullAudioDevice = true;
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    EXPECT_TRUE(eng.captureEnabled());                 // 기본 전이중
    EXPECT_TRUE(eng.setCaptureEnabled(false).ok);
    EXPECT_FALSE(eng.captureEnabled());
    EXPECT_TRUE(eng.setCaptureEnabled(false).ok);      // 같은 값 — 멱등
    eng.stop();
    ASSERT_TRUE(eng.start(cfg, &l).ok);
    EXPECT_TRUE(eng.captureEnabled());                 // 재기동 = 전이중
    eng.stop();
}
