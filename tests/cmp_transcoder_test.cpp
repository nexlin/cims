// cmp_transcoder_test.cpp — 피어 leg 트랜스코더(cmp/PTranscoder.cpp — cmp.md §11) 단위 검증.
//
// 빌드: g++ -std=c++17 -Icmp -Ipkg/opencore-amr/include/opencore-amrwb -Ipkg/vo-amrwbenc-0.1.3/include/vo-amrwbenc \
//         tests/cmp_transcoder_test.cpp cmp/PTranscoder.cpp pkg/opencore-amr/lib/libopencore-amrwb.a \
//         pkg/vo-amrwbenc-0.1.3/lib/libvo-amrwbenc.a -lm -o /tmp/transcoder   (레포 루트에서, AMR ExternalProject 빌드 1회 필요)
//   cims-verify S1-UNIT-CMP 가 같은 줄로 빌드·실행한다.
//
// 검증 항목:
//   framing        — RFC 4867 octet-aligned / bandwidth-efficient 왕복 + 자동 판정 + 잘린 페이로드 거부
//   g711ToAmr      — PCMA 20 ms 패킷 → AMR-WB 프레임 1개(PT·timestamp ×2·SSRC 보존·seq 연속), 10 ms ×2 → 1 프레임, marker 보존
//   amrToG711      — AMR-WB 프레임 → 160 바이트 PCMU 패킷(timestamp ÷2), 다중 프레임 → 패킷 여럿
//   roundTrip      — 1 kHz 사인 PCMA → AMR-WB → PCMA 가 원 신호와 상관 ≥ 0.8 (지연 탐색), 진폭 유지
//   teRescale      — telephone-event timestamp 만 출력 clock 으로
//   pairs          — 지원 쌍 판정(AMR-WB↔PCMU/PCMA 만), 모드셋·octet-align 파싱
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PTranscoder.h"

static bool g_all = true;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); g_all = g_all && ok; }

static std::string rtp(int pt, uint16_t seq, uint32_t ts, uint32_t ssrc, bool marker, const std::string& payload) {
    std::string p(12, '\0');
    p[0] = (char)0x80; p[1] = (char)((marker ? 0x80 : 0) | pt);
    p[2] = (char)(seq >> 8); p[3] = (char)seq;
    p[4] = (char)(ts >> 24); p[5] = (char)(ts >> 16); p[6] = (char)(ts >> 8); p[7] = (char)ts;
    p[8] = (char)(ssrc >> 24); p[9] = (char)(ssrc >> 16); p[10] = (char)(ssrc >> 8); p[11] = (char)ssrc;
    return p + payload;
}
static uint32_t tsOf(const std::string& p) { return ((uint32_t)(unsigned char)p[4] << 24) | ((uint32_t)(unsigned char)p[5] << 16) | ((uint32_t)(unsigned char)p[6] << 8) | (unsigned char)p[7]; }
static uint16_t seqOf(const std::string& p) { return (uint16_t)(((unsigned char)p[2] << 8) | (unsigned char)p[3]); }
static int ptOf(const std::string& p) { return (unsigned char)p[1] & 0x7F; }
static bool markerOf(const std::string& p) { return ((unsigned char)p[1] & 0x80) != 0; }

// G.711 A-law 부호화(시험 신호 생성용 — Sun 동형, PTranscoder 내부와 독립)
static unsigned char lin2alaw(int pcm) {
    static const short seg_aend[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int mask, seg; unsigned char aval;
    pcm = pcm >> 3;
    if (pcm >= 0) mask = 0xD5; else { mask = 0x55; pcm = -pcm - 1; }
    for (seg = 0; seg < 8; seg++) if (pcm <= seg_aend[seg]) break;
    if (seg >= 8) return (unsigned char)(0x7F ^ mask);
    aval = (unsigned char)(seg << 4);
    aval |= (seg < 2) ? (pcm >> 1) & 0x0F : (pcm >> seg) & 0x0F;
    return aval ^ mask;
}
static int alaw2lin(unsigned char a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    switch (seg) { case 0: t += 8; break; case 1: t += 0x108; break; default: t += 0x108; t <<= seg - 1; }
    return (a & 0x80) ? t : -t;
}

int main() {
    const PCodecDesc amrOa = PCodecDesc::make("AMR-WB", 16000, 96, "octet-align=1;mode-set=0,2,4,7");
    const PCodecDesc amrBe = PCodecDesc::make("amr-wb", 0, 97, "");
    const PCodecDesc pcma = PCodecDesc::make("PCMA", 8000, 8, "");
    const PCodecDesc pcmu = PCodecDesc::make("pcmu", 0, 0, "");

    // ── pairs / desc ──
    check(amrOa.isAmrWb() && amrBe.isAmrWb() && amrBe.rate == 16000 && pcmu.rate == 8000 && pcmu.name == "PCMU", "codec desc normalization (name·rate defaults)");
    check(amrOa.amrOctetAlign() && !amrBe.amrOctetAlign() && amrOa.amrMaxMode() == 7 && amrBe.amrMaxMode() == 8, "fmtp octet-align / mode-set parsing");
    check(PTranscoder::supportedPair(amrOa, pcma) && PTranscoder::supportedPair(pcmu, amrBe) && !PTranscoder::supportedPair(pcma, pcmu) &&
              !PTranscoder::supportedPair(amrOa, amrBe), "supported pairs = AMR-WB <-> PCMU/PCMA only");

    // ── framing ──
    {
        PTranscoder::AmrFrame f;
        f.ft = 8; f.q = 1; f.bytes = 60;
        for (int i = 0; i < 60; ++i) f.data[i] = (unsigned char)(i * 37 + 11);
        std::string oa = PTranscoder::buildAmrWb(f, true), be = PTranscoder::buildAmrWb(f, false);
        check(oa.size() == 62 && (unsigned char)oa[0] == 0xF0 && (unsigned char)oa[1] == 0x44, "octet-aligned payload = CMR + ToC(FT8,Q1) + 60B");
        check(be.size() == 61, "bandwidth-efficient payload = ceil((4+6+477)/8) = 61B");
        std::vector<PTranscoder::AmrFrame> out;
        check(PTranscoder::parseAmrWb((const unsigned char*)oa.data(), (int)oa.size(), 1, out) && out.size() == 1 && out[0].ft == 8 &&
                  memcmp(out[0].data, f.data, 60) == 0, "octet-aligned parse round-trip");
        check(PTranscoder::parseAmrWb((const unsigned char*)be.data(), (int)be.size(), 0, out) && out.size() == 1 && out[0].ft == 8 &&
                  memcmp(out[0].data, f.data, 59) == 0 && ((out[0].data[59] ^ f.data[59]) & 0xF8) == 0, "bandwidth-efficient parse round-trip (477 bits)");
        check(PTranscoder::parseAmrWb((const unsigned char*)oa.data(), (int)oa.size(), -1, out) && out[0].ft == 8 && memcmp(out[0].data, f.data, 60) == 0,
              "auto-detect picks octet-aligned when CMR low nibble 0 and length matches");
        check(PTranscoder::parseAmrWb((const unsigned char*)be.data(), (int)be.size(), -1, out) && out[0].ft == 8, "auto-detect falls back to bandwidth-efficient");
        check(!PTranscoder::parseAmrWb((const unsigned char*)oa.data(), 30, 1, out), "truncated octet-aligned payload rejected");
        // NO_DATA(FT 15) 프레임 — 데이터 0B
        PTranscoder::AmrFrame nd; nd.ft = 15; nd.q = 1; nd.bytes = 0;
        std::string ndp = PTranscoder::buildAmrWb(nd, true);
        check(ndp.size() == 2 && PTranscoder::parseAmrWb((const unsigned char*)ndp.data(), 2, -1, out) && out.size() == 1 && out[0].ft == 15, "NO_DATA frame framing");
    }

    // 시험 신호 — 1 kHz 사인, 8 kHz A-law, 20 ms 패킷 N 개
    const int N = 25;
    std::vector<std::string> alawPkts;
    std::vector<int16_t> srcPcm;
    for (int p = 0; p < N; ++p) {
        std::string pay(160, '\0');
        for (int i = 0; i < 160; ++i) {
            int s = (int)(8000.0 * std::sin(2 * M_PI * 1000.0 * (p * 160 + i) / 8000.0));
            srcPcm.push_back((int16_t)s);
            pay[i] = (char)lin2alaw(s);
        }
        alawPkts.push_back(rtp(8, (uint16_t)(1000 + p), 40000 + 160 * p, 0xABCD1234, p == 0, pay));
    }

    // ── g711ToAmr ──
    std::vector<std::string> amrPkts;
    {
        PTranscoder x(pcma, amrOa);
        check(x.ok(), "PCMA -> AMR-WB transcoder init");
        std::vector<std::string> out;
        bool ok = true;
        for (int p = 0; p < N; ++p) {
            ok = x.transcode((const unsigned char*)alawPkts[p].data(), (int)alawPkts[p].size(), out) && ok;
            for (auto& o : out) amrPkts.push_back(o);
        }
        check(ok && amrPkts.size() == (size_t)N, "20 ms PCMA packets -> one AMR-WB frame each");
        check(ptOf(amrPkts[0]) == 96 && markerOf(amrPkts[0]) && !markerOf(amrPkts[1]), "output PT = dst codec PT, marker preserved on first");
        check(tsOf(amrPkts[0]) == 80000 && tsOf(amrPkts[1]) == 80320 && seqOf(amrPkts[0]) == 1000 && seqOf(amrPkts[1]) == 1001,
              "timestamp mapped x2 (8k->16k), seq continues from input");
        check(memcmp(amrPkts[0].data() + 8, alawPkts[0].data() + 8, 4) == 0, "SSRC preserved");
        std::vector<PTranscoder::AmrFrame> fr;
        check(PTranscoder::parseAmrWb((const unsigned char*)amrPkts[3].data() + 12, (int)amrPkts[3].size() - 12, 1, fr) && fr.size() == 1 && fr[0].ft == 7,
              "encoder honours mode-set max (FT 7 = 19.85 kbit/s)");
        // 10 ms 패킷 ×2 → 프레임 1
        PTranscoder y(pcmu, amrBe);
        std::string half(80, '\0');
        out.clear();
        y.transcode((const unsigned char*)rtp(0, 1, 100, 7, true, half).data(), 92, out);
        size_t after1 = out.size();
        y.transcode((const unsigned char*)rtp(0, 2, 180, 7, false, half).data(), 92, out);
        check(after1 == 0 && out.size() == 1 && ptOf(out[0]) == 97 && tsOf(out[0]) == 200 && markerOf(out[0]), "two 10 ms G.711 packets -> one bandwidth-efficient frame (ts of first, marker kept)");
    }

    // ── amrToG711 + roundTrip ──
    {
        PTranscoder x(amrOa, pcma);
        check(x.ok(), "AMR-WB -> PCMA transcoder init");
        std::vector<std::string> out, all;
        for (auto& a : amrPkts) { x.transcode((const unsigned char*)a.data(), (int)a.size(), out); for (auto& o : out) all.push_back(o); }
        check(all.size() == (size_t)N && all[0].size() == 172 && ptOf(all[0]) == 8, "each AMR-WB frame -> one 160B PCMA packet");
        check(tsOf(all[0]) == 40000 && tsOf(all[1]) == 40160, "timestamp mapped /2 (16k->8k)");
        // 다중 프레임 패킷: 프레임 2개를 한 페이로드에 → 출력 2
        std::vector<PTranscoder::AmrFrame> f2;
        PTranscoder::parseAmrWb((const unsigned char*)amrPkts[5].data() + 12, (int)amrPkts[5].size() - 12, 1, f2);
        std::string two; two.push_back((char)0xF0);
        two.push_back((char)(0x80 | ((f2[0].ft & 0x0F) << 3) | ((f2[0].q & 1) << 2)));   // F=1 더 있음
        two.push_back((char)(((f2[0].ft & 0x0F) << 3) | ((f2[0].q & 1) << 2)));
        two.append((const char*)f2[0].data, PTranscoder::amrWbBytes(f2[0].ft));
        two.append((const char*)f2[0].data, PTranscoder::amrWbBytes(f2[0].ft));
        out.clear();
        PTranscoder z(amrOa, pcmu);
        check(z.transcode((const unsigned char*)rtp(96, 9, 5000, 1, false, two).data(), 12 + (int)two.size(), out) && out.size() == 2 && tsOf(out[1]) == tsOf(out[0]) + 160,
              "two frames in one payload -> two G.711 packets");
        // 왕복 상관 — 출력 PCM 을 모아 원 신호와 지연 탐색 상관
        std::vector<int16_t> outPcm;
        for (auto& o : all) for (size_t i = 12; i < o.size(); ++i) outPcm.push_back((int16_t)alaw2lin((unsigned char)o[i]));
        double best = 0; int bestLag = 0;
        const int win = 160 * 10, start = 160 * 8;   // 앞 8 패킷은 코덱·FIR 워밍업
        for (int lag = 0; lag < 400; ++lag) {
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = start; i < start + win && i + lag < (int)outPcm.size(); ++i) {
                double a = srcPcm[i], b = outPcm[i + lag];
                sxy += a * b; sxx += a * a; syy += b * b;
            }
            double r = (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0;
            if (r > best) { best = r; bestLag = lag; }
        }
        double rmsIn = 0, rmsOut = 0;
        for (int i = start; i < start + win; ++i) { rmsIn += (double)srcPcm[i] * srcPcm[i]; rmsOut += (double)outPcm[i + bestLag] * outPcm[i + bestLag]; }
        rmsIn = std::sqrt(rmsIn / win); rmsOut = std::sqrt(rmsOut / win);
        printf("     round-trip corr=%.3f lag=%d samples rmsIn=%.0f rmsOut=%.0f\n", best, bestLag, rmsIn, rmsOut);
        check(best >= 0.8, "PCMA -> AMR-WB -> PCMA correlation with source >= 0.8");
        check(rmsOut > rmsIn * 0.5 && rmsOut < rmsIn * 1.5, "round-trip amplitude within ±50 %");
    }

    // ── teRescale ──
    {
        PTranscoder x(pcma, amrOa);
        std::vector<std::string> out;
        x.transcode((const unsigned char*)alawPkts[0].data(), (int)alawPkts[0].size(), out);
        std::string te = rtp(101, 5, 40000 + 160 * 3, 0xABCD1234, true, std::string("\x01\x8a\x00\xa0", 4));
        x.rescaleTimestamp((unsigned char*)&te[0], (int)te.size());
        check(tsOf(te) == 80000 + 320 * 3 && ptOf(te) == 101, "telephone-event timestamp rescaled to 16k clock, PT untouched");
    }

    printf(g_all ? "PASS\n" : "FAIL\n");
    return g_all ? 0 : 1;
}
