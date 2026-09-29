// call_quality — pjmedia 통계(RTCP·RTCP-XR·지터버퍼)를 CallQuality 로 옮기고 E-model 을 적용한다
// (docs/design/features/ue_voice_quality.md §3). pj 타입에 의존하지 않는 평문 입력을 받아 단위시험이 가능하다 —
// engine.cpp 가 pjsua2 StreamStat·pjsua_call_get_stream_stat_xr 결과를 QualityInput 으로 채운다.
#ifndef CIMSUE_QUALITY_CALL_QUALITY_H
#define CIMSUE_QUALITY_CALL_QUALITY_H

#include <cstdint>
#include <string>

#include "cimsue/types.h"

namespace cimsue {
namespace quality {

/** RTCP-XR VoIP Metrics 한 방향(RFC 3611 §4.7 — 비율·밀도는 0~255 = ×1/256). */
struct XrMetrics {
    bool valid = false;
    unsigned lossRate = 0, discardRate = 0;
    unsigned burstDensity = 0, gapDensity = 0;
    unsigned burstMs = 0, gapMs = 0;
    int signalDbm = 127, noiseDbm = 127;
};

struct QualityInput {
    std::string codec;
    unsigned clockRate = 0;
    // 수신(자기 측정 — RTCP rx stat)
    unsigned rxPackets = 0, rxLost = 0, rxDiscard = 0;
    double rxJitterMeanUs = -1, rxJitterMaxUs = -1;
    // 상대가 받은 내 스트림(상대 RR — RTCP tx stat). remoteReports = 받은 RR 보고 수
    unsigned remoteReports = 0;
    unsigned txPackets = 0, remoteLost = 0;
    double remoteJitterMeanUs = -1, remoteJitterMaxUs = -1;
    XrMetrics xrRx;                   // 자기 수신의 XR 계산값
    XrMetrics xrRemote;               // 상대가 보낸 XR
    double rttMeanUs = -1;            // RTCP RTT(LSR/DLSR) 평균, 없으면 -1
    double jbAvgDelayMs = -1;         // 지터버퍼 평균 지연
    int64_t startEpochMs = 0;
    int64_t nowEpochMs = 0;
};

/** 장치(음향 경로·AEC·PLC 등) 지연 추정 — pjmedia rtcp_xr 의 end system delay 추정(30 ms)과 같은 값. */
constexpr double kDeviceDelayEstimateMs = 30.0;

/** 입력 → 품질(한 스트림). */
CallQuality compute(const QualityInput& in);

/** 전달·재협상으로 스트림이 바뀌었을 때: acc(이전 스트림 누적) 에 next(현재 스트림)를 더한다 — 패킷·손실·폐기는 합,
 *  지연·지터·XR 은 next, 비율과 MOS 는 합친 값으로 다시 계산. acc 가 비었으면 next 그대로. */
CallQuality merge(const CallQuality& acc, const CallQuality& next);

}  // namespace quality
}  // namespace cimsue

#endif
