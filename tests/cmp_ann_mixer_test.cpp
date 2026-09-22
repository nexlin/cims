/** S1-UNIT-CMP — 안내 믹서(mode=mix, announcements.md §3.6): G.711 표 변환 왕복 · relay 패킷 위에 신호음 합산(포화) · AMR-WB 디코드→합산→인코드 ·
 *  재생기 pull(nextPayload) 진행·종료. 빌드는 verify/lib/items/stage1/unit_cmp.py 가 한다. */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "PAnnCatalog.h"
#include "PAnnMixer.h"
#include "PAnnPlayer.h"
#include "PTranscoder.h"
#include "enc_if.h"

static int g_iPass = 0;
#define CHECK( cond, name )                                  \
    do {                                                     \
        if ( cond ) {                                        \
            ++g_iPass;                                       \
            printf( "PASS %s\n", name );                     \
        } else {                                             \
            printf( "FAIL %s (line %d)\n", name, __LINE__ ); \
            return 1;                                        \
        }                                                    \
    } while ( 0 )

static std::string rtp(int pt, uint16_t seq, const std::string& payload) {
    std::string p(12, '\0');
    p[0] = (char)0x80; p[1] = (char)pt; p[2] = (char)(seq >> 8); p[3] = (char)seq;
    p[8] = 1; p[9] = 2; p[10] = 3; p[11] = 4;
    return p + payload;
}

// 정현파 PCM(8 kHz) 160 샘플 → G.711 μ-law 프레임
static std::string toneU(int amp, int freq, int phase0) {
    std::string f(160, '\0');
    for (int i = 0; i < 160; ++i) f[i] = (char)PAnnMixer::lin2ulaw((int16_t)(amp * std::sin(2 * M_PI * freq * (i + phase0) / 8000.0)));
    return f;
}

static std::shared_ptr<const PAnnMedia> mediaU(const std::string& id, int amp) {
    auto m = std::make_shared<PAnnMedia>();
    m->id = id; m->kind = "tone";
    std::vector<std::string> fr;
    for (int k = 0; k < 5; ++k) fr.push_back(toneU(amp, 440, k * 160));
    m->frames["PCMU"] = fr;
    m->durationMs = 100;
    return m;
}

int main() {
    // G.711 표 왕복 — 양자화 오차 안
    {
        bool ok = true;
        for (int s = -32000; s <= 32000; s += 997) {
            int u = PAnnMixer::ulaw2lin(PAnnMixer::lin2ulaw((int16_t)s));
            int a = PAnnMixer::alaw2lin(PAnnMixer::lin2alaw((int16_t)s));
            if (std::abs(u - s) > std::max(64, std::abs(s) / 16) || std::abs(a - s) > std::max(64, std::abs(s) / 16)) ok = false;
        }
        CHECK(ok, "g711 ulaw/alaw round trip");
        CHECK(PAnnMixer::sat16(40000) == 32767 && PAnnMixer::sat16(-40000) == -32768, "sat16");
    }
    // PCMU 믹스 — 무음 relay 패킷 + 신호음 → 출력 = 신호음(±양자화), 재생기 위치 진행
    {
        std::vector<PAnnPlayer::Item> items(1);
        items[0].media = mediaU("sys:cw", 8000);
        PAnnPlayer player("p1", 1, "PCMU", 0, true, items, 0 /*loop*/, 0, 0, 0);
        player.setMix(true);
        PAnnMixer mx("PCMU", true, 8);
        CHECK(mx.ok() && !mx.isAmrWb(), "pcmu mixer ok");
        std::string silence(160, (char)PAnnMixer::lin2ulaw(0));
        std::string in = rtp(0, 10, silence), out;
        CHECK(mx.mix((const unsigned char*)in.data(), (int)in.size(), player, out), "mix silence + tone");
        CHECK(out.size() == in.size() && memcmp(out.data(), in.data(), 12) == 0, "rtp header preserved, same length");
        const std::string& t0 = items[0].media->frames.at("PCMU")[0];
        int maxDiff = 0;
        for (int i = 0; i < 160; ++i) maxDiff = std::max(maxDiff, std::abs((int)PAnnMixer::ulaw2lin((unsigned char)out[12 + i]) - (int)PAnnMixer::ulaw2lin((unsigned char)t0[i])));
        CHECK(maxDiff <= 600, "output ≈ tone frame");
        CHECK(player.playedMs(0) == 20 && player.sent() == 1, "player advanced one frame");
        // 음성 + 신호음 — 포화 없이 합산(둘 다 작은 값)
        std::string speech = toneU(3000, 1000, 0);
        in = rtp(0, 11, speech);
        CHECK(mx.mix((const unsigned char*)in.data(), (int)in.size(), player, out), "mix speech + tone");
        bool differs = false;
        for (int i = 0; i < 160 && !differs; ++i) differs = out[12 + i] != speech[i];
        CHECK(differs, "speech changed by tone");
        // 10 ms 패킷(80 B) 도 샘플 수만큼만 당긴다
        in = rtp(0, 12, std::string(80, (char)PAnnMixer::lin2ulaw(0)));
        CHECK(mx.mix((const unsigned char*)in.data(), (int)in.size(), player, out) && out.size() == 92, "80-sample packet");
        CHECK(mx.mixed() == 3, "mixed count");
    }
    // 유한 재생(repeat 1, 5 프레임) — 다 당기면 끝(completed) → mix false
    {
        std::vector<PAnnPlayer::Item> items(1);
        items[0].media = mediaU("sys:ann", 4000);
        PAnnPlayer player("p2", 0, "PCMU", 0, true, items, 1, 0, 0, 0);
        player.setMix(true);
        PAnnMixer mx("PCMU", true, 8);
        std::string in = rtp(0, 1, std::string(160, (char)0xFF)), out;
        int n = 0;
        while (mx.mix((const unsigned char*)in.data(), (int)in.size(), player, out)) ++n;
        CHECK(n == 5 && player.done() && player.reason() == "completed", "finite mix completes after 5 frames");
    }
    // AMR-WB — 인코더로 만든 신호음 프레임 + 무음 프레임 relay 패킷 → 섞인 프레임 1개 페이로드(octet-aligned), 다중 프레임은 그대로
    {
        void* enc = E_IF_init();
        CHECK(enc != nullptr, "amr-wb encoder");
        auto mkFrame = [&](int amp) {
            int16_t pcm[320];
            for (int i = 0; i < 320; ++i) pcm[i] = (int16_t)(amp * std::sin(2 * M_PI * 440 * i / 16000.0));
            unsigned char o[64];
            int n = E_IF_encode(enc, 8, pcm, o, 0);
            return std::string((const char*)o, (size_t)n);   // 저장 형식(ToC + 데이터)
        };
        auto m = std::make_shared<PAnnMedia>();
        m->id = "sys:cw_wb"; m->kind = "tone";
        m->frames["AMR-WB"] = { mkFrame(6000), mkFrame(6000), mkFrame(6000) };
        std::vector<PAnnPlayer::Item> items(1);
        items[0].media = m;
        PAnnPlayer player("p3", 1, "AMR-WB", 96, true, items, 0, 0, 0, 0);
        player.setMix(true);
        PAnnMixer mx("AMR-WB", true, 8);
        CHECK(mx.ok() && mx.isAmrWb(), "amr-wb mixer ok");
        std::string sil = mkFrame(0);
        PTranscoder::AmrFrame f;
        f.ft = ((unsigned char)sil[0] >> 3) & 0x0F; f.q = 1; f.bytes = (int)sil.size() - 1;
        memcpy(f.data, sil.data() + 1, (size_t)f.bytes);
        std::string in = rtp(96, 5, PTranscoder::buildAmrWb(f, true)), out;
        CHECK(mx.mix((const unsigned char*)in.data(), (int)in.size(), player, out), "amr-wb mix");
        std::vector<PTranscoder::AmrFrame> fr;
        CHECK(out.size() > 12 && PTranscoder::parseAmrWb((const unsigned char*)out.data() + 12, (int)out.size() - 12, 1, fr) && fr.size() == 1,
              "output is one octet-aligned AMR-WB frame");
        CHECK(memcmp(out.data(), in.data(), 12) == 0, "amr-wb header preserved");
        std::string two = PTranscoder::buildAmrWb(f, true);
        std::string in2 = rtp(96, 6, two + two.substr(1));   // 조작된 다중 프레임 — 섞지 않는다(false → 원본 그대로)
        std::string out2;
        bool r2 = mx.mix((const unsigned char*)in2.data(), (int)in2.size(), player, out2);
        CHECK(!r2 || out2.size() > 12, "multi-frame handled without crash");
        E_IF_exit(enc);
    }
    CHECK(!PAnnMixer::supports("G722") && PAnnMixer::supports("PCMA"), "codec support table");
    printf("cmp_ann_mixer_test: %d checks passed\n", g_iPass);
    return 0;
}
