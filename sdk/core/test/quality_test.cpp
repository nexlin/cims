// libcimsue 단위시험 — 호 품질 계산·E-model (S1-UE-UNIT, ue_voice_quality.md §3)
#include <gtest/gtest.h>

#include "../src/quality/call_quality.h"
#include "../src/quality/emodel.h"

using namespace cimsue;
using namespace cimsue::quality;
namespace em = cimsue::emodel;

TEST(EModel, NarrowbandDefaults) {
    // G.107 기본 입력(지연·손실 없음, G.711) → R 93.2, MOS 4.41
    em::Result r = em::evaluate(em::codecOf("PCMU"), 0, 0);
    EXPECT_NEAR(r.r, 93.2, 1e-9);
    EXPECT_NEAR(r.mos, 4.41, 0.01);
    EXPECT_DOUBLE_EQ(em::mosOfR(0), 1.0);
    EXPECT_DOUBLE_EQ(em::mosOfR(100), 4.5);
}

TEST(EModel, LossAndDelayMonotonic) {
    em::Codec g711 = em::codecOf("PCMA");
    EXPECT_GT(em::evaluate(g711, 1, 0).mos, em::evaluate(g711, 3, 0).mos);
    EXPECT_LT(em::evaluate(g711, 5, 0).mos, 4.0);                      // 무작위 손실 5 % — Ie,eff ≈ 15.8
    EXPECT_GT(em::evaluate(g711, 0, 100).mos, em::evaluate(g711, 0, 300).mos);
    EXPECT_NEAR(em::delayImpairment(177.3), 0.024 * 177.3, 1e-9);      // 꺾임점 아래는 선형
    EXPECT_GT(em::delayImpairment(300), 0.024 * 300);                  // 꺾임점 위는 기울기 증가
    EXPECT_GT(em::evaluate(g711, 5, 0, 1.0).mos, em::evaluate(g711, 5, 0, 2.0).mos);   // 몰린 손실(BurstR>1)이 더 나쁘다
    EXPECT_LT(em::evaluate(g711, 100, 0).mos, 1.5);
}

TEST(EModel, WidebandScale) {
    em::Codec wb = em::codecOf("AMR-WB");
    EXPECT_TRUE(wb.wideband);
    em::Result r = em::evaluate(wb, 0, 0);
    EXPECT_NEAR(r.r, 129.0 - 13.0, 1e-9);                             // G.107.1 척도의 원 R
    EXPECT_NEAR(r.mos, em::mosOfR((129.0 - 13.0) / 1.29), 1e-9);      // 협대역 척도로 옮긴 MOS
    EXPECT_FALSE(em::codecOf("unknown").wideband);
    EXPECT_TRUE(em::codecOf("amr-wb").wideband);                       // 이름 대소문자 무관
}

static QualityInput cleanInput() {
    QualityInput in;
    in.codec = "AMR-WB"; in.clockRate = 16000;
    in.rxPackets = 1000; in.rxJitterMeanUs = 2000; in.rxJitterMaxUs = 9000;
    in.remoteReports = 3; in.txPackets = 1000; in.remoteLost = 10; in.remoteJitterMeanUs = 3000;
    in.rttMeanUs = 80000; in.jbAvgDelayMs = 40;
    in.startEpochMs = 1000000; in.nowEpochMs = 1020000;
    return in;
}

TEST(CallQuality, ComputeFromStats) {
    QualityInput in = cleanInput();
    in.rxLost = 20; in.rxDiscard = 10;
    CallQuality q = compute(in);
    ASSERT_TRUE(q.valid);
    EXPECT_TRUE(q.wideband);
    EXPECT_NEAR(q.rx.lossPct, 100.0 * 20 / 1020, 1e-9);
    EXPECT_NEAR(q.rx.discardPct, 100.0 * 10 / 1020, 1e-9);
    EXPECT_NEAR(q.rx.jitterMs, 2.0, 1e-9);
    EXPECT_TRUE(q.remote.valid);
    EXPECT_NEAR(q.remote.lossPct, 1.0, 1e-9);
    EXPECT_NEAR(q.rtdMs, 80.0, 1e-9);
    EXPECT_NEAR(q.esdMs, 40 + 25 + kDeviceDelayEstimateMs, 1e-9);
    EXPECT_NEAR(q.oneWayMs, 40.0 + q.esdMs + 25 + kDeviceDelayEstimateMs, 1e-9);
    EXPECT_GT(q.mosLq, q.mosCq);                                       // 대화 품질은 지연 손상만큼 낮다
    EXPECT_EQ(q.durationMs, 20000);
    // 손실 = 망 손실 + 지터버퍼 폐기
    em::Result lq = em::evaluate(em::codecOf("AMR-WB"), q.rx.lossPct + q.rx.discardPct, 0);
    EXPECT_NEAR(q.mosLq, lq.mos, 1e-9);
}

TEST(CallQuality, NoRtcpNoRemoteNoRtd) {
    QualityInput in = cleanInput();
    in.remoteReports = 0; in.rttMeanUs = -1;
    CallQuality q = compute(in);
    EXPECT_FALSE(q.remote.valid);
    EXPECT_DOUBLE_EQ(q.rtdMs, -1);
    EXPECT_NEAR(q.oneWayMs, q.esdMs + 25 + kDeviceDelayEstimateMs, 1e-9);   // 망 지연 0 으로
    EXPECT_GT(q.mosCq, 0);
}

TEST(CallQuality, NothingReceived) {
    QualityInput in = cleanInput();
    in.rxPackets = 0;
    CallQuality q = compute(in);
    EXPECT_TRUE(q.valid);
    EXPECT_FALSE(q.rx.valid);
    EXPECT_DOUBLE_EQ(q.mosCq, -1);                                     // 받은 것이 없으면 추정하지 않는다
}

TEST(CallQuality, XrMetrics) {
    QualityInput in = cleanInput();
    in.xrRx.valid = true; in.xrRx.burstDensity = 128; in.xrRx.gapDensity = 2; in.xrRx.burstMs = 60; in.xrRx.gapMs = 4000;
    in.xrRemote.valid = true; in.xrRemote.discardRate = 64;
    CallQuality q = compute(in);
    EXPECT_NEAR(q.rx.burstDensityPct, 50.0, 1e-9);
    EXPECT_EQ(q.rx.burstMs, 60);
    EXPECT_NEAR(q.remote.discardPct, 25.0, 1e-9);
    EXPECT_EQ(q.remote.discarded, 250u);
    EXPECT_EQ(q.rx.signalDbm, 127);                                    // 레벨 미계산 = 127 그대로
}

TEST(CallQuality, MergeAccumulatesStreams) {
    QualityInput a = cleanInput();
    a.rxLost = 100;                                                    // 첫 스트림은 나빴고
    QualityInput b = cleanInput();
    b.rxLost = 0; b.startEpochMs = 2000000; b.nowEpochMs = 2010000;   // 전달 뒤 새 스트림은 깨끗
    CallQuality qa = compute(a), qb = compute(b);
    CallQuality m = merge(qa, qb);
    EXPECT_EQ(m.rx.packets, 2000u);
    EXPECT_EQ(m.rx.lost, 100u);
    EXPECT_NEAR(m.rx.lossPct, 100.0 * 100 / 2100, 1e-9);
    EXPECT_EQ(m.startEpochMs, 1000000);
    EXPECT_EQ(m.durationMs, 30000);
    EXPECT_GT(m.mosCq, qa.mosCq);
    EXPECT_LT(m.mosCq, qb.mosCq);
    EXPECT_EQ(merge(CallQuality{}, qb).rx.packets, 1000u);             // 빈 누적 + 첫 스트림
}
