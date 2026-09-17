// EModel 단위시험 — G.107 E-model 의 경계값과 단조성. cims-verify S1-UNIT-TESTER 가 실행한다.
#include "EModel.h"
#include <cstdio>
#include <cmath>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    EModelCodec g711 = emodelCodec("PCMU"), amrwb = emodelCodec("AMR-WB");
    double clean = emodelMos(g711, 0, 0);
    CHECK(std::fabs(clean - 4.41) < 0.05);                 // G.711 무손실·무지연 ≈ 4.4 (R ≈ 91.3)
    CHECK(emodelMos(g711, 5, 0) < clean - 0.5);            // 손실 5 % 는 눈에 띄게 낮다
    CHECK(emodelMos(g711, 1, 0) > emodelMos(g711, 3, 0));  // 손실 단조 감소
    CHECK(emodelMos(g711, 0, 0) > emodelMos(g711, 0, 100)); // 지터(지연) 단조 감소
    CHECK(emodelMos(g711, 0, 0, 300) < emodelMos(g711, 0, 0, 100));   // 망 지연 단조 감소
    CHECK(emodelMos(amrwb, 0, 0) < clean);                 // 코덱 Ie 가 있으면 무손실 상한이 낮다
    CHECK(emodelMos(g711, 100, 0) >= 1.0 && emodelMos(g711, 100, 0) < 1.5);   // 전손실 → 바닥
    CHECK(emodelCodec("unknown").bpl == 25.1);
    printf("clean=%.2f loss5=%.2f jitter100=%.2f amrwb=%.2f\n", clean, emodelMos(g711, 5, 0), emodelMos(g711, 0, 100), emodelMos(amrwb, 0, 0));
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}
