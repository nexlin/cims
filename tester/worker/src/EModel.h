// EModel — RTP 수신 품질(손실·지터·망 지연)에서 MOS 를 추정한다 (test_instrument.md §5 미디어).
//   식과 코덱 상수의 정본은 단말 코어와 공용인 sdk/core/src/quality/emodel.h(ITU-T G.107 / G.107.1) — 여기는 워커의 입력 모양
//   (손실 %·지터·망 지연)을 단방향 입→귀 지연 Ta 로 옮기는 얇은 층이다(ue_voice_quality.md §3.2).
//   Ta = 망 단방향 지연(RTCP RTT/2 — 모르면 0) + 코덱 프레임·lookahead + 지터버퍼(2 × 지터).
#ifndef _CIMS_TESTER_EMODEL_H_
#define _CIMS_TESTER_EMODEL_H_

#include <string>

#include "quality/emodel.h"

using EModelCodec = cimsue::emodel::Codec;

/** 코덱 이름(rtpmap) → 상수. 미지 코덱은 G.711 값. */
inline EModelCodec emodelCodec(const std::string& name) { return cimsue::emodel::codecOf(name); }

/** 손실 %(0~100)·지터 ms·단방향 망 지연 ms → MOS(1.0~4.5, 대화 품질). */
inline double emodelMos(const EModelCodec& c, double lossPct, double jitterMs, double netDelayMs = 0) {
    double ta = (netDelayMs > 0 ? netDelayMs : 0) + c.frameMs + 2.0 * (jitterMs > 0 ? jitterMs : 0);
    return cimsue::emodel::evaluate(c, lossPct, ta).mos;
}

#endif
