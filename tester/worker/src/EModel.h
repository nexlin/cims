// EModel — ITU-T G.107 E-model 로 RTP 수신 품질(손실·지터)에서 MOS 를 추정한다 (test_instrument.md §5 미디어).
//
//  R = R0 − Is − Id − Ie,eff + A  (G.107 §7) 에서 기본값 R0 = 93.2, Is = 1.41(기본 입력의 Is 합), A = 0 을 쓰고
//    Id     = 0.024·d + 0.11·(d − 177.3)·H(d − 177.3)      — G.107 Id 의 단순화(ITU-T G.108 App. II 의 한 구간 근사), d = 단방향 지연 ms
//    Ie,eff = Ie + (95 − Ie)·Ppl / (Ppl/BurstR + Bpl)      — G.107 §7.5(BurstR = 1, 무작위 손실)
//    MOS    = 1 + 0.035·R + R·(R − 60)·(100 − R)·7·10⁻⁶ (0 < R < 100; R ≤ 0 → 1, R ≥ 100 → 4.5)   — G.107 Annex B
//  코덱 Ie/Bpl 은 G.113 Appendix I(G.711 PLC 있음 25.1 · G.729 · AMR 12.2) 값, AMR-WB/G.722 는 광대역 모델(G.107.1) 미구현이라
//  협대역 척도의 근사값이다 — 수치는 상대 비교(회귀·부하 단계 사이)용이고 절대 청취 품질 판정에는 쓰지 않는다.
//  단방향 지연은 계측기가 모른다(RTCP RTT 미측정) — d = 코덱 20 ms + 버퍼(2 × 지터) + 기본 망 지연(인자, 기본 0).
#ifndef _CIMS_TESTER_EMODEL_H_
#define _CIMS_TESTER_EMODEL_H_

#include <string>
#include <strings.h>

struct EModelCodec { double ie; double bpl; };

/** 코덱 이름(rtpmap) → Ie/Bpl. 미지 코덱은 G.711 값. */
inline EModelCodec emodelCodec(const std::string& name) {
    if (strcasecmp(name.c_str(), "PCMU") == 0 || strcasecmp(name.c_str(), "PCMA") == 0) return { 0.0, 25.1 };   // G.113 App.I — G.711 + PLC
    if (strcasecmp(name.c_str(), "G729") == 0) return { 11.0, 19.0 };
    if (strcasecmp(name.c_str(), "AMR") == 0) return { 5.0, 10.0 };        // AMR 12.2
    if (strcasecmp(name.c_str(), "AMR-WB") == 0) return { 6.0, 13.0 };     // 근사(광대역 모델 미구현)
    if (strcasecmp(name.c_str(), "G722") == 0) return { 13.0, 10.0 };      // 근사
    return { 0.0, 25.1 };
}

/** 손실 %(0~100)·지터 ms·기본 단방향 망 지연 ms → MOS(1.0~4.5). */
inline double emodelMos(const EModelCodec& c, double lossPct, double jitterMs, double baseDelayMs = 0) {
    double d = baseDelayMs + 20.0 + 2.0 * (jitterMs > 0 ? jitterMs : 0);
    double id = 0.024 * d + (d > 177.3 ? 0.11 * (d - 177.3) : 0.0);
    double ppl = lossPct < 0 ? 0 : lossPct;
    double ieEff = c.ie + (95.0 - c.ie) * ppl / (ppl + c.bpl);
    double r = 93.2 - 1.41 - id - ieEff;
    if (r <= 0) return 1.0;
    if (r >= 100) return 4.5;
    return 1.0 + 0.035 * r + r * (r - 60.0) * (100.0 - r) * 7e-6;
}

#endif
