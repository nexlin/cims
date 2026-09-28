/*
 * CIMS: conference bridge 레벨 처리 — 송신 AGC + 피크 리미터 (conference.c 전용 private 헤더).
 *
 * 왜 여기인가 — 같은 앱(pjsua2)을 쓰는 단말끼리도 마이크 디지털 레벨이 34 dB 넘게 벌어진다
 * (서버 녹취 P.56 실측: 활성 레벨 -46.6 dBov 단말 ↔ -12 dBov 단말, 후자는 발언 턴 85 % 에 포화).
 * 고정 배율(adjust_rx_level) 하나로는 작은 단말을 키우면 큰 단말이 잘리고, 그 반대도 같다.
 * 그래서 게인을 "측정값의 함수" 로 바꾸는 AGC 와, 어떤 게인 단계든 풀스케일을 넘기지 않는
 * 리미터를 bridge 에 둔다 — Android·Windows·Linux(cimsue-cli) 가 같은 코드를 쓴다.
 *
 * AGC (ITU-T G.169 의 요건 방향):
 *   - 목표 = 활성 음성 레벨(ITU-T P.56 정의, 0 dBov = 32768 RMS). 기본 -26 dBov.
 *   - 레벨 학습은 **음성 프레임만**(잡음 바닥 + 문턱) — 무음·잡음은 키우지 않는다.
 *   - 원단(스피커) 재생이 있는 동안은 학습을 멈춘다 — 잔류 에코를 근단 음성으로 오인하지 않게.
 *   - 게인 변화율 상한(올림 느리게·내림 빠르게) + 게인 범위 상한.
 * 리미터:
 *   - 프레임 피크가 한계(-1 dBFS)를 넘으면 그만큼만 즉시 줄이고, 풀어줄 때는 천천히(release).
 *   - bridge 는 프레임 전체를 먼저 본다 — 줄일 때는 한계를 처음 넘는 샘플 **앞에서** 램프가 끝나게
 *     해(프레임 안 look-ahead) 초과 샘플이 원리적으로 없다. 풀 때는 프레임 앞부분 램프.
 *   - 램프는 게인 불연속(클릭)을 막는다. 최종 하드 클립은 부동소수 반올림용 안전망일 뿐이다.
 *
 * 규약: 모든 함수는 bridge 클록 스레드(get_frame) 안에서만 부른다 — 잠금 없음.
 */
#ifndef __CIMS_LEVEL_H__
#define __CIMS_LEVEL_H__

#include <math.h>

#define CIMS_LVL_FULL           32768.0f
#define CIMS_LVL_RAMP           32          /* 게인 변화 램프 길이(샘플) — 16 kHz 에서 2 ms */

/* ── 피크 리미터 ─────────────────────────────────────────────────────── */

typedef struct cims_limiter
{
    float   g;                              /* 현재 리미터 게인(1 = 개입 없음) */
} cims_limiter;

PJ_INLINE(void) cims_limiter_init(cims_limiter *l)
{
    l->g = 1.0f;
}

/* in[] (게인 적용 뒤 32비트 값) → out[] (16비트). ceil = 선형 한계, rel = 프레임당 release 계수
 * (0..1, 1 에 가까울수록 느림). 반환값 = 한계를 넘어 하드 클립된 샘플 수(안전망 동작 계수). */
PJ_INLINE(unsigned) cims_limiter_run(cims_limiter *l, const float *in, pj_int16_t *out,
                                     unsigned n, float ceil_, float rel)
{
    float peak = 0.0f, need, gn, g, step;
    unsigned i, r0, r1, first = n, clipped = 0;

    for (i = 0; i < n; ++i) {
        float a = in[i] < 0 ? -in[i] : in[i];
        if (a > peak) peak = a;
        if (first == n && a * l->g > ceil_) first = i;     /* 현재 게인으로 처음 넘는 곳 */
    }
    need = peak > ceil_ ? ceil_ / peak : 1.0f;

    if (need < l->g) {
        /* attack: 램프가 first 에서 끝나도록 [first-RAMP, first] 에 둔다. 그 앞 샘플은 현재
         * 게인으로도 한계 안이고 램프 중 게인은 현재 게인 이하라 넘지 않는다. */
        gn = need;
        r1 = first + 1;
        r0 = r1 > CIMS_LVL_RAMP ? r1 - CIMS_LVL_RAMP : 0;
    } else {
        /* release: 1 쪽으로 천천히, 단 이 프레임 피크를 넘기지 않는 선까지. 램프 중 게인은
         * 이전·새 게인 사이라 둘 다 need 이하 → 넘지 않는다. */
        gn = 1.0f - (1.0f - l->g) * rel;
        if (gn > need) gn = need;
        r0 = 0;
        r1 = n < CIMS_LVL_RAMP ? n : CIMS_LVL_RAMP;
    }

    step = (gn - l->g) / (float)(r1 - r0);
    for (i = 0; i < n; ++i) {
        float v;
        if (i < r0)       g = l->g;
        else if (i < r1)  g = l->g + step * (float)(i + 1 - r0);
        else              g = gn;
        v = in[i] * g;
        if (v > 32767.0f)       { v = 32767.0f;  ++clipped; }
        else if (v < -32768.0f) { v = -32768.0f; ++clipped; }
        out[i] = (pj_int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    l->g = gn;
    return clipped;
}

/* ── 송신 AGC ─────────────────────────────────────────────────────────── */

typedef struct cims_agc
{
    pj_bool_t   enabled;
    float       target_db;                  /* 목표 활성 레벨(dBov) */
    float       max_gain_db, min_gain_db;   /* 게인 범위 */
    float       up_db_per_s, down_db_per_s; /* 게인 변화율 상한(수렴 뒤) */
    float       tau_s;                      /* 레벨 학습 시간상수(수렴 뒤) */
    float       warm_s;                     /* 초기 수렴 구간 — 음성 누적 길이(초) */
    float       warm_db_per_s;              /* 초기 수렴 구간의 게인 변화율 상한 */

    float       gain_db;                    /* 현재 게인(dB) */
    float       gain_lin;                   /* 직전 프레임 끝의 선형 게인(램프 시작점) */
    float       level_pow;                  /* 학습된 활성 음성 파워(선형, 풀스케일² 정규화) */
    unsigned    level_frames;               /* 학습에 쓴 음성 프레임 수(초기 수렴용) */
    float       noise_db;                   /* 잡음 바닥 추적값 */
    pj_bool_t   noise_init;
    unsigned    far_hang;                   /* 원단 재생 뒤 학습 정지 잔여 프레임 */
    unsigned    log_tick;                   /* 상태 기록 주기 계수(conference.c) */
} cims_agc;

#define CIMS_AGC_ABS_GATE_DB    -62.0f      /* 이보다 작은 프레임은 음성으로 보지 않는다 */
#define CIMS_AGC_REL_GATE_DB      9.0f      /* 잡음 바닥 + 이 값 이상이어야 음성 */
#ifndef CIMS_AGC_TAU_S
#define CIMS_AGC_TAU_S            1.0f      /* 수렴 뒤 레벨 학습 시간상수 */
#endif
#ifndef CIMS_AGC_UP_DB_PER_S
#define CIMS_AGC_UP_DB_PER_S     10.0f      /* 수렴 뒤 게인 올림 상한 */
#endif
#ifndef CIMS_AGC_DOWN_DB_PER_S
#define CIMS_AGC_DOWN_DB_PER_S   30.0f      /* 수렴 뒤 게인 내림 상한 */
#endif
#ifndef CIMS_AGC_WARM_S
#define CIMS_AGC_WARM_S           1.0f      /* 초기 수렴 구간(음성 누적 초) */
#endif
#ifndef CIMS_AGC_WARM_DB_PER_S
#define CIMS_AGC_WARM_DB_PER_S   60.0f      /* 초기 수렴 구간 게인 변화율 상한 — 첫 발언 안에 수렴 */
#endif
#define CIMS_AGC_FAR_GATE_DB    -50.0f      /* 원단 재생이 이보다 크면 학습 정지 */
#define CIMS_AGC_FAR_HANG_S       0.5f      /* 원단 재생이 멈춘 뒤에도 이만큼 정지 유지 — 재생·녹음 버퍼 지연 + 음향 경로 */

PJ_INLINE(void) cims_agc_init(cims_agc *a, pj_bool_t enabled, float target_db,
                              float max_gain_db, float min_gain_db)
{
    pj_bzero(a, sizeof(*a));
    a->enabled = enabled;
    a->target_db = target_db;
    a->max_gain_db = max_gain_db;
    a->min_gain_db = min_gain_db;
    a->up_db_per_s = CIMS_AGC_UP_DB_PER_S;
    a->down_db_per_s = CIMS_AGC_DOWN_DB_PER_S;
    a->tau_s = CIMS_AGC_TAU_S;
    a->warm_s = CIMS_AGC_WARM_S;
    a->warm_db_per_s = CIMS_AGC_WARM_DB_PER_S;
    a->gain_lin = 1.0f;
}

PJ_INLINE(float) cims_frame_db(const pj_int16_t *x, unsigned n)
{
    double sq = 0;
    unsigned i;
    for (i = 0; i < n; ++i) sq += (double)x[i] * x[i];
    sq /= (double)n * CIMS_LVL_FULL * CIMS_LVL_FULL;
    return sq > 1e-12 ? (float)(10.0 * log10(sq)) : -120.0f;
}

/* 원단(스피커로 나간) 프레임 레벨을 알린다 — bridge 가 slot 0 tx 직후 부른다. */
PJ_INLINE(void) cims_agc_note_far(cims_agc *a, float far_db, float frame_s)
{
    if (far_db > CIMS_AGC_FAR_GATE_DB)
        a->far_hang = (unsigned)(CIMS_AGC_FAR_HANG_S / frame_s + 0.5f);
}

/* x[] (16비트 입력) → y[] (게인 적용, 32비트 float — 뒤따르는 리미터 입력). post = 뒤에 곱할
 * 사용자 배율(adjust_rx_level, 1 = 없음). frame_s = 프레임 길이(초). */
PJ_INLINE(void) cims_agc_run(cims_agc *a, const pj_int16_t *x, float *y, unsigned n,
                             float frame_s, float post)
{
    float fdb = cims_frame_db(x, n);
    float g0, g1, step;
    unsigned i, r;

    /* 잡음 바닥: 내려갈 때는 빠르게, 올라갈 때는 느리게(+1 dB/s) 따라간다. */
    if (!a->noise_init) {
        a->noise_db = fdb;
        a->noise_init = PJ_TRUE;
    } else if (fdb < a->noise_db) {
        a->noise_db += (fdb - a->noise_db) * 0.5f;
    } else {
        a->noise_db += 1.0f * frame_s;
    }

    if (a->far_hang) {
        --a->far_hang;                              /* 원단 재생 중 — 학습·게인 갱신 정지 */
    } else if (fdb > CIMS_AGC_ABS_GATE_DB && fdb > a->noise_db + CIMS_AGC_REL_GATE_DB) {
        /* 음성 프레임 — 활성 레벨 학습(파워 도메인 = P.56 활성 레벨과 같은 척도).
         * 초기 수렴 구간(음성 warm_s 초 분량)은 누적 평균 + 빠른 올림으로 첫 발언 안에 맞추고,
         * 그 뒤는 지수 평활 + 변화율 상한. 수렴 뒤를 더 느리게 하면 화자의 말 크기 변화가 그대로
         * 지나가 발언 간 흩어짐이 오히려 커진다(녹취 세션 비교, ue_audio_level.md §4). */
        float p = (float)pow(10.0, fdb / 10.0);
        float k = 1.0f - (float)exp(-frame_s / a->tau_s);
        float up = a->up_db_per_s * frame_s, down = a->down_db_per_s * frame_s;
        unsigned warm = (unsigned)(a->warm_s / frame_s);
        if (a->level_frames < warm) {
            ++a->level_frames;
            if (1.0f / a->level_frames > k) k = 1.0f / a->level_frames;
            up = a->warm_db_per_s * frame_s;
            if (down < up) down = up;
        }
        a->level_pow += (p - a->level_pow) * k;

        {
            float lvl_db = (float)(10.0 * log10(a->level_pow > 1e-12f ? a->level_pow : 1e-12f));
            float want = a->target_db - lvl_db;
            if (want > a->max_gain_db) want = a->max_gain_db;
            if (want < a->min_gain_db) want = a->min_gain_db;
            if (want > a->gain_db + up)   want = a->gain_db + up;
            if (want < a->gain_db - down) want = a->gain_db - down;
            a->gain_db = want;
        }
    }
    /* 무음·원단 구간은 게인을 유지한다(잡음 펌핑 방지). */

    g0 = a->gain_lin;
    g1 = (float)pow(10.0, a->gain_db / 20.0) * post;
    r = n < CIMS_LVL_RAMP ? n : CIMS_LVL_RAMP;
    step = (g1 - g0) / (float)r;
    for (i = 0; i < n; ++i)
        y[i] = (float)x[i] * (i < r ? g0 + step * (float)(i + 1) : g1);
    a->gain_lin = g1;
}

#endif  /* __CIMS_LEVEL_H__ */
