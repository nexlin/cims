// DtmfInband — in-band DTMF(ITU-T Q.23 이중음)를 G.711 오디오 안에 내고(톤 합성) 받은 G.711 을 풀어 검출(Goertzel)한다.
//   RFC 4733 telephone-event 를 협상하지 않는 상대(PSTN 게이트웨이 뒤 in-band 전달 — MGCF 프로파일 옵션, TS 29.163 §7.2.3.2.2 의
//   DTMF in-band 경로)를 모사·검증하기 위한 것. 8 kHz PCM 16-bit, 20 ms(160 샘플) 단위.
//   검출 = 160 샘플 블록마다 8 주파수 Goertzel 에너지 → 행·열 최대가 각 그룹 나머지보다 충분히 크고(트위스트 허용) 두 블록(40 ms) 이상
//   이어지면 톤, 톤이 끊기면 그 숫자를 확정한다(Q.24 최소 40 ms 톤·40 ms 간격).
#ifndef _CSIM_DTMF_INBAND_H_
#define _CSIM_DTMF_INBAND_H_

#include <cmath>
#include <cstring>

namespace csim_dtmf {

static const int kRow[4] = { 697, 770, 852, 941 };
static const int kCol[4] = { 1209, 1336, 1477, 1633 };
static const char kKeypad[4][4] = { { '1', '2', '3', 'A' }, { '4', '5', '6', 'B' }, { '7', '8', '9', 'C' }, { '*', '0', '#', 'D' } };

/** 숫자 문자 → (행, 열) 인덱스. 모르는 문자면 false. */
inline bool KeyOf(char c, int& row, int& col) {
    if (c >= 'a' && c <= 'd') c = (char)(c - 'a' + 'A');
    for (row = 0; row < 4; ++row)
        for (col = 0; col < 4; ++col)
            if (kKeypad[row][col] == c) return true;
    return false;
}

/** 톤 합성기 — 숫자 하나의 이중음을 PCM 으로 이어서 낸다(호출 간 위상 유지). 진폭은 각 −10 dBm0 근사. */
struct ToneGen {
    double ph1 = 0, ph2 = 0;
    int row = -1, col = -1;
    bool Set(char c) { ph1 = ph2 = 0; return KeyOf(c, row, col); }
    void Fill(short* pcm, int n, int rate = 8000) const {
        const double w1 = 2.0 * M_PI * kRow[row] / rate, w2 = 2.0 * M_PI * kCol[col] / rate;
        double p1 = ph1, p2 = ph2;
        for (int i = 0; i < n; ++i) {
            double v = 8000.0 * std::sin(p1) + 8000.0 * std::sin(p2);
            pcm[i] = (short)v;
            p1 += w1; p2 += w2;
        }
        const_cast<ToneGen*>(this)->ph1 = std::fmod(p1, 2.0 * M_PI);
        const_cast<ToneGen*>(this)->ph2 = std::fmod(p2, 2.0 * M_PI);
    }
};

/** Goertzel 검출기 — Feed 가 확정된 숫자를 돌려준다(없으면 0). 톤 시작 뒤 2 블록 이상 같은 숫자가 이어지고 끊기면 확정. */
struct Detector {
    char cur = 0;      // 지금 보고 있는 톤
    int run = 0;       // 연속 블록 수
    char Feed(const short* pcm, int n, int rate = 8000) {
        double er[4], ec[4];
        for (int k = 0; k < 4; ++k) { er[k] = Goertzel(pcm, n, kRow[k], rate); ec[k] = Goertzel(pcm, n, kCol[k], rate); }
        int r = Max(er), c = Max(ec);
        double total = 0;
        for (int i = 0; i < n; ++i) total += (double)pcm[i] * pcm[i];
        total /= n > 0 ? n : 1;
        char tone = 0;
        // 에너지 충분(무음 배제) + 행·열 최대가 각 그룹 2위의 4배 이상 + 두 톤 합이 전체의 절반 이상(음성 배제)
        if (total > 1.0e5 && er[r] > 4.0 * Second(er, r) && ec[c] > 4.0 * Second(ec, c) && (er[r] + ec[c]) > 0.5 * total * n)
            tone = kKeypad[r][c];
        char out = 0;
        if (tone && tone == cur) { ++run; }
        else {
            if (cur && run >= 2) out = cur;   // 톤이 끊겼거나 바뀌었다 — 이전 톤 확정
            cur = tone; run = tone ? 1 : 0;
        }
        return out;
    }
    void Reset() { cur = 0; run = 0; }
private:
    static double Goertzel(const short* x, int n, int f, int rate) {
        const double w = 2.0 * M_PI * f / rate, coeff = 2.0 * std::cos(w);
        double s0 = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < n; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
        return s1 * s1 + s2 * s2 - coeff * s1 * s2;   // 에너지(정규화 없음 — 상대 비교용)
    }
    static int Max(const double* e) { int m = 0; for (int k = 1; k < 4; ++k) if (e[k] > e[m]) m = k; return m; }
    static double Second(const double* e, int skip) { double m = 0; for (int k = 0; k < 4; ++k) if (k != skip && e[k] > m) m = e[k]; return m; }
};

}  // namespace csim_dtmf

#endif
