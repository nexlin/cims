// cmp_ann_player_test.cpp — 안내 재생기(cmp/PAnnCatalog.cpp · cmp/PAnnPlayer.cpp — announcements.md §4.2) 단위 검증.
//
// 빌드: g++ -std=c++17 -Icmp -Iinclude -Ipkg/opencore-amr/include/opencore-amrwb -Ipkg/vo-amrwbenc-0.1.3/include/vo-amrwbenc \
//         tests/cmp_ann_player_test.cpp cmp/PAnnCatalog.cpp cmp/PAnnPlayer.cpp cmp/PTranscoder.cpp \
//         pkg/opencore-amr/lib/libopencore-amrwb.a pkg/vo-amrwbenc-0.1.3/lib/libvo-amrwbenc.a -lm -o /tmp/ann   (레포 루트)
//   cims-verify S1-UNIT-CMP 가 같은 줄로 빌드·실행한다.
//
// 검증 항목:
//   codecNorm   — 코덱 이름 정규화(amr-wb/AMRWB/"AMR-WB/16000"→AMR-WB, pcmu→PCMU, 모르는 이름 빈 값)·클록 step(160/320)
//   frameG711   — 160 B 프레임화, 배수 아니면 거부
//   frameAmr    — RFC 4867 저장 형식(매직 유무) 프레임화, NO_DATA 는 빈 프레임, 잘린 프레임 거부
//   parseRow    — 카탈로그 jsonl 행 파싱(files 키 정규화·모르는 키 무시·id/files 필수)
//   pacing      — 20 ms 마다 프레임 1개, seq 연속·timestamp step·SSRC 고정·PT·첫 패킷 marker, 늦은 틱은 kMaxBurst 까지 따라잡기
//   sequence    — 항목 repeat(2회)·항목 max_ms 로 다음 항목·전체 repeat 2 + delay_ms 무음(패킷 없음·timestamp 진행·marker 재설정)·completed
//   infinite    — repeat 0 은 끝나지 않고 stop() 으로 stopped, max_ms 상한은 reason=max
//   amrPayload  — AMR-WB 저장 프레임 → RTP 페이로드(octet-aligned CMR+ToC / bandwidth-efficient) 길이·재파싱 일치, NO_DATA 무송신
//   natGate     — natWaitMs 동안 시작 보류, onIngress 로 즉시 시작, 상한 지나면 자동 시작
//   okChecks    — 코덱 파일 없는 음원 거부, repeat 0 항목에 max_ms 없으면 거부(단일 무한 loop 는 허용)
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "PAnnCatalog.h"
#include "PAnnPlayer.h"
#include "PTranscoder.h"

static bool g_all = true;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); g_all = g_all && ok; }

static uint32_t tsOf(const std::string& p) { return ((uint32_t)(unsigned char)p[4] << 24) | ((uint32_t)(unsigned char)p[5] << 16) | ((uint32_t)(unsigned char)p[6] << 8) | (unsigned char)p[7]; }
static uint16_t seqOf(const std::string& p) { return (uint16_t)(((unsigned char)p[2] << 8) | (unsigned char)p[3]); }
static uint32_t ssrcOf(const std::string& p) { return ((uint32_t)(unsigned char)p[8] << 24) | ((uint32_t)(unsigned char)p[9] << 16) | ((uint32_t)(unsigned char)p[10] << 8) | (unsigned char)p[11]; }
static int ptOf(const std::string& p) { return (unsigned char)p[1] & 0x7F; }
static bool markerOf(const std::string& p) { return ((unsigned char)p[1] & 0x80) != 0; }

// 합성 음원 — 코덱 codec 로 nFrames 프레임(각 프레임 첫 바이트 = 프레임 번호)
static std::shared_ptr<const PAnnMedia> makeMedia(const std::string& id, const std::string& codec, int nFrames) {
    auto m = std::make_shared<PAnnMedia>();
    m->id = id;
    m->kind = "tone";
    std::vector<std::string> fr;
    for (int i = 0; i < nFrames; ++i) {
        if (codec == "AMR-WB") {
            std::string f(1, (char)((8 << 3) | 0x04));   // ToC FT 8 (23.85 kbps), Q=1
            f += std::string(60, (char)i);
            fr.push_back(f);
        } else {
            fr.push_back(std::string(160, (char)i));
        }
    }
    m->frames[codec] = fr;
    m->durationMs = nFrames * 20;
    return m;
}

static const int64_t US = 1000;   // 1 ms

int main() {
    // ── codecNorm
    check(PAnnCatalog::NormalizeCodec("amr-wb") == "AMR-WB" && PAnnCatalog::NormalizeCodec("AMRWB") == "AMR-WB" &&
          PAnnCatalog::NormalizeCodec("AMR-WB/16000") == "AMR-WB" && PAnnCatalog::NormalizeCodec("pcmu") == "PCMU" &&
          PAnnCatalog::NormalizeCodec("G722") == "G722" && PAnnCatalog::NormalizeCodec("opus").empty(), "codecNorm names");
    check(PAnnCatalog::TsStep("PCMU") == 160 && PAnnCatalog::TsStep("G722") == 160 && PAnnCatalog::TsStep("AMR-WB") == 320, "codecNorm ts step");

    // ── frameG711
    {
        std::vector<std::string> fr; std::string err;
        check(PAnnCatalog::Frame("PCMA", std::string(160 * 3, 'x'), fr, err) && fr.size() == 3 && fr[0].size() == 160, "frameG711 3 frames");
        check(!PAnnCatalog::Frame("PCMU", std::string(161, 'x'), fr, err), "frameG711 rejects non-multiple");
    }
    // ── frameAmr
    {
        std::string f8(1, (char)(8 << 3)); f8 += std::string(60, 'a');            // SPEECH 23.85
        std::string f9(1, (char)(9 << 3)); f9 += std::string(5, 's');             // SID
        std::string f15(1, (char)(15 << 3));                                        // NO_DATA
        std::string body = f8 + f9 + f15 + f8;
        std::vector<std::string> fr; std::string err;
        check(PAnnCatalog::Frame("AMR-WB", std::string("#!AMR-WB\n") + body, fr, err) && fr.size() == 4 && fr[0].size() == 61 &&
              fr[1].size() == 6 && fr[2].empty() && fr[3].size() == 61, "frameAmr magic + 4 frames (NO_DATA empty)");
        check(PAnnCatalog::Frame("AMR-WB", body, fr, err) && fr.size() == 4, "frameAmr without magic");
        check(!PAnnCatalog::Frame("AMR-WB", body.substr(0, 30), fr, err), "frameAmr truncated rejected");
    }
    // ── parseRow
    {
        PAnnMedia m; std::string err;
        bool ok = PAnnCatalog::ParseRow("{\"id\":\"sys:busy_kr\",\"kind\":\"tone\",\"duration_ms\":2000,\"loop\":true,"
                                        "\"files\":{\"amr-wb\":\"sys/busy_kr.amrwb\",\"pcmu\":\"sys/busy_kr.pcmu\",\"h264\":\"x\"}}", m, err);
        check(ok && m.id == "sys:busy_kr" && m.kind == "tone" && m.durationMs == 2000 && m.loop && m.files.size() == 2 &&
              m.files.count("AMR-WB") && m.files.count("PCMU"), "parseRow ok + codec key normalization");
        check(!PAnnCatalog::ParseRow("{\"kind\":\"tone\",\"files\":{\"pcmu\":\"a\"}}", m, err), "parseRow rejects missing id");
        check(!PAnnCatalog::ParseRow("{\"id\":\"x\"}", m, err), "parseRow rejects missing files");
    }

    // ── pacing
    {
        std::vector<PAnnPlayer::Item> items{ { makeMedia("t", "PCMU", 10), 1, 0 } };
        PAnnPlayer p("p1", 0, "PCMU", 0, true, items, 1, 0, 0, 0);
        std::string err;
        check(p.ok(err), "pacing ok()");
        check(p.durationMs() == 200, "pacing durationMs 200");
        std::vector<std::string> pk;
        int64_t t0 = 1000 * 1000 * US;
        p.tick(t0, pk);
        check(pk.size() == 1 && markerOf(pk[0]) && ptOf(pk[0]) == 0 && pk[0].size() == 172 && (unsigned char)pk[0][12] == 0, "pacing first tick 1 frame + marker");
        uint32_t ssrc = ssrcOf(pk[0]); uint16_t seq0 = seqOf(pk[0]); uint32_t ts0 = tsOf(pk[0]);
        pk.clear(); p.tick(t0 + 10 * US, pk);
        check(pk.empty(), "pacing no frame at +10 ms");
        pk.clear(); p.tick(t0 + 20 * US, pk);
        check(pk.size() == 1 && !markerOf(pk[0]) && seqOf(pk[0]) == (uint16_t)(seq0 + 1) && tsOf(pk[0]) == ts0 + 160 && ssrcOf(pk[0]) == ssrc, "pacing +20 ms seq/ts/ssrc");
        pk.clear(); p.tick(t0 + 100 * US, pk);   // 4 프레임 밀림 → 따라잡기
        check(pk.size() == 4 && (unsigned char)pk[3][12] == 5, "pacing late tick catches up 4 frames");
        pk.clear(); p.tick(t0 + 1000 * US, pk);   // 남은 4 프레임 (버스트 상한 5)
        check(pk.size() == 4 && p.done() && p.reason() == "completed" && p.playedMs(0) == 200, "pacing finishes → completed, played 200 ms");
    }

    // ── sequence: item0 3 frames ×2, item1 5 frames max_ms 60(→3 frames), 전체 repeat 2, delay 40 ms
    {
        std::vector<PAnnPlayer::Item> items{ { makeMedia("a", "PCMA", 3), 2, 0 }, { makeMedia("b", "PCMA", 5), 0, 60 } };
        PAnnPlayer p("p2", 1, "PCMA", 8, true, items, 2, 40, 0, 0);
        std::string err;
        check(p.ok(err), "sequence ok()");
        check(p.durationMs() == (60 * 2 + 60) * 2 + 40, "sequence durationMs 400");
        std::vector<std::string> all;
        int64_t t0 = 0;
        for (int i = 0; i <= 40; ++i) { std::vector<std::string> pk; p.tick(t0 + i * 20 * US, pk); all.insert(all.end(), pk.begin(), pk.end()); if (p.done()) break; }
        // 1회 = a×2(6) + b 3 = 9 프레임, 2회 → 18 패킷, 사이에 무음 2 프레임(패킷 없음)
        check(all.size() == 18 && p.done() && p.reason() == "completed", "sequence 18 packets, completed");
        bool ptOk = true; for (auto& s : all) ptOk = ptOk && ptOf(s) == 8;
        check(ptOk, "sequence PT 8");
        // marker: 첫 패킷, item1 첫 패킷(index 6), 2회차 첫 패킷(index 9), 2회차 item1(index 15)
        check(markerOf(all[0]) && markerOf(all[6]) && markerOf(all[9]) && markerOf(all[15]) && !markerOf(all[1]) && !markerOf(all[7]), "sequence marker at item/repeat boundaries");
        // timestamp: 9번째→10번째 사이 무음 2 프레임 → ts 차이 = 3×160, seq 는 연속(무음은 패킷이 아니다)
        check(tsOf(all[9]) - tsOf(all[8]) == 3 * 160 && seqOf(all[9]) == (uint16_t)(seqOf(all[8]) + 1), "sequence delay advances ts only");
        check((unsigned char)all[6][12] == 0 && (unsigned char)all[8][12] == 2 && (unsigned char)all[5][12] == 2, "sequence item content order");
        check(p.playedMs(0) == 400, "sequence playedMs 400 (incl. delay)");
    }

    // ── infinite / max
    {
        std::vector<PAnnPlayer::Item> items{ { makeMedia("m", "PCMU", 4), 1, 0 } };
        PAnnPlayer p("p3", 0, "PCMU", 0, true, items, 0, 0, 0, 0);
        std::string err;
        check(p.ok(err) && p.durationMs() == 0, "infinite ok, durationMs 0");
        int n = 0;
        for (int i = 0; i < 100; ++i) { std::vector<std::string> pk; p.tick(i * 20 * US, pk); n += (int)pk.size(); }
        check(n == 100 && !p.done(), "infinite keeps looping (100 frames)");
        p.stop("stopped");
        check(p.done() && p.reason() == "stopped", "infinite stop() → stopped");

        PAnnPlayer q("p4", 0, "PCMU", 0, true, items, 0, 0, 100, 0);   // max 100 ms
        int m = 0;
        for (int i = 0; i < 20 && !q.done(); ++i) { std::vector<std::string> pk; q.tick(i * 20 * US, pk); m += (int)pk.size(); }
        check(q.done() && q.reason() == "max" && m == 5, "max_ms 100 → 5 frames, reason max");
    }

    // ── amrPayload
    {
        std::vector<PAnnPlayer::Item> items{ { makeMedia("w", "AMR-WB", 2), 1, 0 } };
        PAnnPlayer p("p5", 0, "AMR-WB", 96, true, items, 1, 0, 0, 0);
        std::vector<std::string> pk; p.tick(0, pk);
        check(pk.size() == 1 && pk[0].size() == 12 + 2 + 60 && ((unsigned char)pk[0][12] == 0xF0), "amrPayload octet-aligned CMR+ToC+60");
        std::vector<PTranscoder::AmrFrame> fr;
        check(PTranscoder::parseAmrWb((const unsigned char*)pk[0].data() + 12, (int)pk[0].size() - 12, 1, fr) && fr.size() == 1 && fr[0].ft == 8 && fr[0].bytes == 60, "amrPayload re-parses (FT 8, 60 B)");
        pk.clear(); p.tick(20 * US, pk);
        check(pk.size() == 1 && tsOf(pk[0]) - 0 != 0 && ptOf(pk[0]) == 96, "amrPayload second frame PT 96");
        PAnnPlayer b("p6", 0, "AMR-WB", 97, false, items, 1, 0, 0, 0);
        pk.clear(); b.tick(0, pk);
        check(pk.size() == 1 && pk[0].size() == 12 + 61, "amrPayload bandwidth-efficient 61 B payload");
        // NO_DATA 프레임은 패킷을 내지 않고 timestamp 만 진행
        auto m = std::make_shared<PAnnMedia>(); m->id = "nd"; m->frames["AMR-WB"] = { items[0].media->frames.at("AMR-WB")[0], std::string(), items[0].media->frames.at("AMR-WB")[1] };
        std::vector<PAnnPlayer::Item> it2{ { m, 1, 0 } };
        PAnnPlayer c("p7", 0, "AMR-WB", 96, true, it2, 1, 0, 0, 0);
        std::vector<std::string> a1, a2, a3;
        c.tick(0, a1); c.tick(20 * US, a2); c.tick(40 * US, a3);
        check(a1.size() == 1 && a2.empty() && a3.size() == 1 && tsOf(a3[0]) - tsOf(a1[0]) == 640 && seqOf(a3[0]) == (uint16_t)(seqOf(a1[0]) + 1) && markerOf(a3[0]), "amrPayload NO_DATA skipped (ts +640, marker after gap)");
    }

    // ── natGate
    {
        std::vector<PAnnPlayer::Item> items{ { makeMedia("n", "PCMU", 10), 1, 0 } };
        PAnnPlayer p("p8", 0, "PCMU", 0, true, items, 1, 0, 0, 500);
        std::vector<std::string> pk;
        p.tick(0, pk); p.tick(100 * US, pk);
        check(pk.empty(), "natGate holds start");
        p.onIngress(); p.tick(120 * US, pk);
        check(pk.size() == 1 && markerOf(pk[0]), "natGate opens on ingress");
        PAnnPlayer q("p9", 0, "PCMU", 0, true, items, 1, 0, 0, 500);
        pk.clear(); q.tick(0, pk); q.tick(600 * US, pk);
        check(pk.size() == 1, "natGate auto-starts after natWaitMs");
    }

    // ── okChecks
    {
        std::vector<PAnnPlayer::Item> items{ { makeMedia("x", "PCMU", 2), 1, 0 } };
        std::string err;
        PAnnPlayer p("p10", 0, "PCMA", 8, true, items, 1, 0, 0, 0);
        check(!p.ok(err) && err.find("no PCMA") != std::string::npos, "okChecks codec file missing rejected");
        std::vector<PAnnPlayer::Item> two{ { makeMedia("x", "PCMU", 2), 0, 0 }, { makeMedia("y", "PCMU", 2), 1, 0 } };
        PAnnPlayer q("p11", 0, "PCMU", 0, true, two, 1, 0, 0, 0);
        check(!q.ok(err), "okChecks repeat 0 item without max_ms rejected");
        std::vector<PAnnPlayer::Item> one{ { makeMedia("x", "PCMU", 2), 0, 0 } };
        PAnnPlayer r("p12", 0, "PCMU", 0, true, one, 0, 0, 0, 0);
        check(r.ok(err), "okChecks single infinite loop item allowed (hold)");
    }

    printf("%s\n", g_all ? "ALL OK" : "SOME FAILED");
    return g_all ? 0 : 1;
}
