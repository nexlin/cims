// emodel — ITU-T G.107 / G.107.1 E-model 로 전송 지표(손실·지연)에서 R 과 MOS 를 추정한다 — 단일 정의.
//   단말 코어 quality/(callQuality)와 계측기 워커(tester/worker/src/EModel.h)가 이 헤더 하나를 쓴다
//   (docs/design/features/ue_voice_quality.md §3.2). 헤더 전용·표준 라이브러리 외 의존 없음.
//
//   R = Rmax − Id − Ie,eff  (G.107 §7 의 기본 입력 — Ro − Is 와 A = 0 을 접은 값)
//     협대역(G.107)  Rmax = 93.2
//     광대역(G.107.1) Rmax = 129
//   Id     = 0.024·Ta + 0.11·(Ta − 177.3)·H(Ta − 177.3)   — 단방향 입→귀 지연 Ta(ms)의 한 구간 근사(G.107 Id 의 단순화)
//   Ie,eff = Ie + (95 − Ie)·Ppl / (Ppl/BurstR + Bpl)      — G.107 §7.5. 광대역은 95 대신 Rmax 척도의 같은 식(Ie,wb)
//   MOS    = 1 + 0.035·R + R·(R − 60)·(100 − R)·7·10⁻⁶ (0 < R < 100; R ≤ 0 → 1, R ≥ 100 → 4.5)   — G.107 Annex B
//            광대역 R 은 R/1.29 로 협대역 척도에 옮긴 뒤 같은 식 — 협대역·광대역 호를 한 MOS 척도로 비교한다.
//
//   코덱 상수(Ie, Bpl): 협대역 = G.113 Appendix I. 광대역 Ie,wb 는 G.113 광대역 표의 대표 모드 값(AMR-WB 12.65 kbit/s,
//   G.722 64 kbit/s)이며 대역 차이를 반영하는 상대 비교용이다 — 절대 청취 품질 판정에는 쓰지 않는다.
#ifndef CIMSUE_QUALITY_EMODEL_H
#define CIMSUE_QUALITY_EMODEL_H

#include <cctype>
#include <string>

namespace cimsue {
namespace emodel {

struct Codec {
    bool wideband = false;
    double ie = 0;          // 장비 손상 계수 Ie (광대역이면 Ie,wb)
    double bpl = 25.1;      // 패킷 손실 강건성 Bpl
    double frameMs = 20;    // 프레임 + lookahead 지연(단말 지연 추정용)
};

inline bool nameIs(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return i == a.size() && !b[i];
}

/** 코덱 이름(rtpmap encoding name) → 상수. 미지 코덱은 G.711. */
inline Codec codecOf(const std::string& name) {
    if (nameIs(name, "PCMU") || nameIs(name, "PCMA")) return { false, 0.0, 25.1, 20 };   // G.711 + PLC (G.113 App.I)
    if (nameIs(name, "G729")) return { false, 11.0, 19.0, 25 };                             // 10 ms 프레임 ×2 + 5 ms lookahead
    if (nameIs(name, "AMR")) return { false, 5.0, 10.0, 25 };                               // AMR 12.2
    if (nameIs(name, "AMR-WB")) return { true, 13.0, 13.0, 25 };                            // G.722.2 12.65 kbit/s — 20 ms + 5 ms lookahead
    if (nameIs(name, "G722")) return { true, 13.0, 10.0, 20 };                              // G.722 64 kbit/s
    return { false, 0.0, 25.1, 20 };
}

struct Result {
    double r = -1;          // 코덱 대역의 원 척도 R (광대역이면 0~129)
    double mos = -1;        // 협대역 척도로 옮긴 R 의 MOS (1.0~4.5)
};

inline double mosOfR(double r) {
    if (r <= 0) return 1.0;
    if (r >= 100) return 4.5;
    return 1.0 + 0.035 * r + r * (r - 60.0) * (100.0 - r) * 7e-6;
}

/** 단방향 입→귀 지연 Ta(ms) → Id. */
inline double delayImpairment(double taMs) {
    if (taMs <= 0) return 0.0;
    return 0.024 * taMs + (taMs > 177.3 ? 0.11 * (taMs - 177.3) : 0.0);
}

/** 손실 %(망 손실 + 지터버퍼 폐기, 0~100)·단방향 지연 Ta ms(0 = 지연 손상 제외 = 청취 품질)·BurstR → R, MOS. */
inline Result evaluate(const Codec& c, double pplPct, double taMs, double burstR = 1.0) {
    const double rmax = c.wideband ? 129.0 : 93.2;
    const double ceiling = c.wideband ? 129.0 : 95.0;
    double ppl = pplPct < 0 ? 0 : (pplPct > 100 ? 100 : pplPct);
    if (burstR < 1) burstR = 1;
    const double ieEff = c.ie + (ceiling - c.ie) * ppl / (ppl / burstR + c.bpl);
    Result res;
    res.r = rmax - delayImpairment(taMs) - ieEff;
    if (res.r < 0) res.r = 0;
    res.mos = mosOfR(c.wideband ? res.r / 1.29 : res.r);
    return res;
}

}  // namespace emodel
}  // namespace cimsue

#endif
