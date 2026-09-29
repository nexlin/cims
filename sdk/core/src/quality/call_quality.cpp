#include "quality/call_quality.h"

#include "quality/emodel.h"

namespace cimsue {
namespace quality {

namespace {

double pctOf(unsigned part, unsigned whole) { return whole ? 100.0 * (double)part / (double)whole : -1; }
double rate256(unsigned v) { return 100.0 * (double)v / 256.0; }

void applyXr(QualityDirection& d, const XrMetrics& x) {
    if (!x.valid) return;
    d.burstDensityPct = rate256(x.burstDensity);
    d.gapDensityPct = rate256(x.gapDensity);
    d.burstMs = (int)x.burstMs;
    d.gapMs = (int)x.gapMs;
    d.signalDbm = x.signalDbm;
    d.noiseDbm = x.noiseDbm;
}

/** 비율·E-model 을 현재 카운터·지연으로 다시 계산한다(compute·merge 공용). */
void finalize(CallQuality& q) {
    const unsigned rxExpected = q.rx.packets + q.rx.lost;
    q.rx.lossPct = pctOf(q.rx.lost, rxExpected);
    q.rx.discardPct = pctOf(q.rx.discarded, rxExpected);
    q.rx.valid = rxExpected > 0;
    if (q.remote.valid) {
        const unsigned txExpected = q.remote.packets;   // 상대가 기대한 수 ≈ 내가 보낸 수
        q.remote.lossPct = pctOf(q.remote.lost, txExpected);
        if (q.remote.discardPct < 0 && q.remote.discarded) q.remote.discardPct = pctOf(q.remote.discarded, txExpected);
    }

    const emodel::Codec c = emodel::codecOf(q.codec);
    q.wideband = c.wideband;
    q.oneWayMs = -1;
    q.rLq = q.rCq = q.mosLq = q.mosCq = -1;
    if (!q.rx.valid) return;
    // 단방향 입→귀 지연 Ta = 망(RTD/2) + 자기 단말 지연 + 상대 단말 지연 추정(코덱 프레임·lookahead + 장치)
    const double net = q.rtdMs >= 0 ? q.rtdMs / 2.0 : 0.0;
    const double self = q.esdMs >= 0 ? q.esdMs : c.frameMs + kDeviceDelayEstimateMs;
    q.oneWayMs = net + self + c.frameMs + kDeviceDelayEstimateMs;
    const double ppl = q.rx.lossPct + (q.rx.discardPct > 0 ? q.rx.discardPct : 0);
    const emodel::Result lq = emodel::evaluate(c, ppl, 0);
    const emodel::Result cq = emodel::evaluate(c, ppl, q.oneWayMs);
    q.rLq = lq.r; q.mosLq = lq.mos;
    q.rCq = cq.r; q.mosCq = cq.mos;
}

}  // namespace

CallQuality compute(const QualityInput& in) {
    CallQuality q;
    q.valid = true;
    q.codec = in.codec;
    q.clockRate = in.clockRate;

    q.rx.packets = in.rxPackets;
    q.rx.lost = in.rxLost;
    q.rx.discarded = in.rxDiscard;
    q.rx.jitterMs = in.rxJitterMeanUs >= 0 ? in.rxJitterMeanUs / 1000.0 : -1;
    q.rx.jitterMaxMs = in.rxJitterMaxUs >= 0 ? in.rxJitterMaxUs / 1000.0 : -1;
    applyXr(q.rx, in.xrRx);

    if (in.remoteReports > 0) {
        q.remote.valid = true;
        q.remote.packets = in.txPackets;
        q.remote.lost = in.remoteLost;
        q.remote.jitterMs = in.remoteJitterMeanUs >= 0 ? in.remoteJitterMeanUs / 1000.0 : -1;
        q.remote.jitterMaxMs = in.remoteJitterMaxUs >= 0 ? in.remoteJitterMaxUs / 1000.0 : -1;
        if (in.xrRemote.valid) {
            q.remote.discardPct = rate256(in.xrRemote.discardRate);
            q.remote.discarded = (unsigned)((double)in.txPackets * (double)in.xrRemote.discardRate / 256.0 + 0.5);
        }
        applyXr(q.remote, in.xrRemote);
    }

    q.rtdMs = in.rttMeanUs >= 0 ? in.rttMeanUs / 1000.0 : -1;
    const emodel::Codec c = emodel::codecOf(in.codec);
    q.esdMs = (in.jbAvgDelayMs >= 0 ? in.jbAvgDelayMs : 0.0) + c.frameMs + kDeviceDelayEstimateMs;
    q.startEpochMs = in.startEpochMs;
    q.durationMs = in.nowEpochMs > in.startEpochMs && in.startEpochMs > 0 ? in.nowEpochMs - in.startEpochMs : 0;
    finalize(q);
    return q;
}

CallQuality merge(const CallQuality& acc, const CallQuality& next) {
    if (!acc.valid) return next;
    if (!next.valid) return acc;
    CallQuality q = next;
    q.startEpochMs = acc.startEpochMs ? acc.startEpochMs : next.startEpochMs;
    q.durationMs = acc.durationMs + next.durationMs;
    q.rx.packets += acc.rx.packets;
    q.rx.lost += acc.rx.lost;
    q.rx.discarded += acc.rx.discarded;
    if (acc.remote.valid) {
        if (!q.remote.valid) q.remote = acc.remote;
        else {
            q.remote.packets += acc.remote.packets;
            q.remote.lost += acc.remote.lost;
            q.remote.discarded += acc.remote.discarded;
            q.remote.discardPct = -1;              // 합친 폐기 수로 다시 계산
        }
    }
    finalize(q);
    return q;
}

}  // namespace quality
}  // namespace cimsue
