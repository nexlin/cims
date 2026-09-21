/* G.722 인코더 — pjproject pjmedia-codec/g722/g722_enc.{c,h} 를 pj 의존 없이 옮긴 것(GPL v2+, 원 출처 CMU Speech Group).
 * 계측기 샘플 변환기(cims-sample-conv)가 16 kHz PCM → G.722 64 kbit/s(RFC 3551 §4.5.2, 160 B/20 ms)를 만들 때 쓴다. */
#ifndef CIMS_SAMPLECONV_G722_ENC_H
#define CIMS_SAMPLECONV_G722_ENC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct g722_enc_t {
    /* PCM low band */
    int slow;
    int detlow;
    int spl;
    int szl;
    int rlt  [3];
    int al   [3];
    int apl  [3];
    int plt  [3];
    int dlt  [7];
    int bl   [7];
    int bpl  [7];
    int sgl  [7];
    int nbl;

    /* PCM high band*/
    int shigh;
    int dethigh;
    int sph;
    int szh;
    int rh   [3];
    int ah   [3];
    int aph  [3];
    int ph   [3];
    int dh   [7];
    int bh   [7];
    int bph  [7];
    int sgh  [7];
    int nbh;

    /* QMF signal history */
    int x[24];
} g722_enc_t;

void g722_enc_init(g722_enc_t *enc);
/* nsamples 는 짝수. out 은 nsamples/2 바이트. 반환 = 쓴 바이트 수 */
size_t g722_enc_encode(g722_enc_t *enc, const int16_t in[], size_t nsamples, uint8_t *out);
#ifdef __cplusplus
}
#endif
#endif
