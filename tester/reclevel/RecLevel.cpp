// cims-rec-level — CMP 녹취 음성 레벨·클리핑 측정기 (docs/design/features/ue_audio_level.md §7).
//
//   CMP 녹취 트랙(.rtp — [u32 len][i64 recv_usec][RTP] 레코드 열, recording.md §3)을 디코드해
//   ITU-T P.56 method B 활성 레벨과 포화 지표를 낸다. 녹취는 단말 **인코더 출력** 그대로라 단말 송신
//   레벨(마이크 → AGC → 배율 → 리미터)의 판정 기준이 된다. 수신 쪽(스피커) 레벨은 단말에서 따로 잰다.
//
//   cims-rec-level <file.rtp> [--pt N]           트랙 하나 → JSON 한 줄
//   cims-rec-level --dir <녹취 루트> [--since YYYY-MM-DD] [--summary]
//                                                seg_*.json 메타를 따라 트랙마다 JSON 한 줄(화자 포함),
//                                                --summary 면 화자별 요약 표만
//
//   코덱: AMR-WB octet-align(opencore-amrwb 디코더) · PCMU(0) · PCMA(8). 그 밖의 PT 는 건너뛴다.
//   척도: 0 dBov = 32768 RMS(P.56). sat = |x| ≥ 32700 샘플, flat_runs = 2샘플 이상 이어진 포화 구간 수,
//   hot_frames = 피크 ≥ -1 dBFS 인 20 ms 프레임 수.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "G711.h"
#include "dec_if.h"

namespace fs = std::filesystem;

namespace {

constexpr int kFs = 16000;
constexpr int kFrame = 320;            // 20 ms @16 kHz

// ── 디코드 ────────────────────────────────────────────────────────────────

const int kAmrWbSize[16] = {17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0};

/** 녹취 트랙 → 16 kHz PCM. RTP timestamp 간격의 유실 구간은 무음으로 메운다(P.56 은 활성 구간 기준). */
bool decodeTrack(const std::string& path, int wantPt, std::vector<int16_t>& out, std::string& codec) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    void* amr = nullptr;
    uint32_t len = 0;
    int64_t usec = 0;
    std::vector<uint8_t> b(4096);
    bool first = true;
    uint32_t lastTs = 0;
    int16_t pcm[kFrame];
    while (fread(&len, 4, 1, f) == 1 && fread(&usec, 8, 1, f) == 1) {
        if (len > b.size() || fread(b.data(), 1, len, f) != len) break;
        if (len < 13 || (b[0] >> 6) != 2) continue;
        int cc = b[0] & 15, x = b[0] & 16, pt = b[1] & 127;
        if (wantPt >= 0 && pt != wantPt) continue;
        size_t off = 12 + cc * 4;
        if (x) {
            if (off + 4 > len) continue;
            off += 4 + 4 * ((b[off + 2] << 8) | b[off + 3]);
        }
        if (off >= len) continue;
        uint32_t ts = (b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];

        bool g711 = pt == 0 || pt == 8;
        if (codec.empty()) codec = pt == 0 ? "PCMU" : pt == 8 ? "PCMA" : "AMR-WB";
        // 유실 메우기 — G.711 은 8 kHz timestamp 를 16 kHz 샘플로 환산
        int tsPerFrame = g711 ? 160 : kFrame;
        if (!first) {
            int32_t gap = (int32_t)(ts - lastTs);
            if (gap > tsPerFrame && gap < kFs * 5)
                out.insert(out.end(), (size_t)(gap - tsPerFrame) * (g711 ? 2 : 1), 0);
        }
        first = false;
        lastTs = ts;

        if (g711) {                                        // 8 kHz → 16 kHz 는 표본 반복(레벨 척도 보존)
            int nIn = (int)(len - off);
            std::vector<int16_t> lin(nIn);
            if (pt == 0) UlawToPcm((const char*)b.data() + off, nIn, (char*)lin.data(), nIn * 2);
            else         AlawToPcm((const char*)b.data() + off, nIn, (char*)lin.data(), nIn * 2);
            for (int16_t s : lin) { out.push_back(s); out.push_back(s); }
            continue;
        }
        if (!amr) amr = D_IF_init();
        size_t q = off + 1;                                // CMR 건너뜀
        uint8_t toc[16];
        int n = 0;
        while (q < len && n < 16) {
            toc[n++] = b[q];
            if (!(b[q++] & 0x80)) break;
        }
        for (int i = 0; i < n; ++i) {
            int ft = (toc[i] >> 3) & 15, sz = kAmrWbSize[ft];
            if (q + sz > len) break;
            uint8_t st[64];
            st[0] = (uint8_t)((ft << 3) | (toc[i] & 4));
            memcpy(st + 1, b.data() + q, sz);
            q += sz;
            D_IF_decode(amr, st, pcm, 0);
            out.insert(out.end(), pcm, pcm + kFrame);
            if (i) lastTs += kFrame;
        }
    }
    if (amr) D_IF_exit(amr);
    fclose(f);
    return true;
}

// ── 측정 ──────────────────────────────────────────────────────────────────

/** ITU-T P.56 method B — 활성 레벨(dBov)·활동률. 시간상수 30 ms · hangover 200 ms · margin 15.9 dB.
 *  tester/worker/samples/gen_samples.py p56_active_level 과 같은 계산(동봉 샘플 메타로 교정). */
bool p56(const std::vector<int16_t>& x, double& levelDb, double& activity) {
    size_t n = x.size();
    if (!n) return false;
    const double g = std::exp(-1.0 / (kFs * 0.03)), M = 15.9;
    const size_t hang = (size_t)(0.2 * kFs);
    std::vector<double> q(n);
    double a1 = 0, a2 = 0, sq = 0;
    for (size_t i = 0; i < n; ++i) {
        double v = x[i];
        sq += v * v;
        a1 = g * a1 + (1 - g) * std::fabs(v);
        a2 = g * a2 + (1 - g) * a1;
        q[i] = a2;
    }
    struct Pt { bool ok; double L, C, a; };
    std::vector<Pt> pts;
    for (int j = 0; j < 16; ++j) {
        double c = 32768.0 * std::pow(2.0, -j);
        size_t act = 0;
        long long last = -1000000000LL;
        for (size_t i = 0; i < n; ++i) {
            if (q[i] > c) last = (long long)i;
            if ((long long)i - last < (long long)hang) ++act;
        }
        if (!act) { pts.push_back({false, 0, 20 * std::log10(c / 32768.0), 0}); continue; }
        pts.push_back({true, 10 * std::log10(sq / act / (32768.0 * 32768.0)), 20 * std::log10(c / 32768.0), (double)act});
    }
    const Pt* prev = nullptr;
    for (int j = 15; j >= 0; --j) {                     // 낮은 문턱부터 — (L−C) 는 단조 감소
        const Pt& p = pts[j];
        if (!p.ok) break;
        double d = p.L - p.C;
        if (d <= M) {
            if (!prev) { levelDb = p.L; activity = p.a / n; return true; }
            double dp = prev->L - prev->C, t = dp != d ? (dp - M) / (dp - d) : 0.0;
            levelDb = prev->L + t * (p.L - prev->L);
            activity = (prev->a + t * (p.a - prev->a)) / n;
            return true;
        }
        prev = &p;
    }
    return false;
}

struct Measure {
    double sec = 0, level = 0, activity = 0, peakDb = -120;
    bool hasLevel = false;
    long sat = 0, flatRuns = 0, hotFrames = 0;
};

Measure measure(const std::vector<int16_t>& x) {
    Measure m;
    m.sec = (double)x.size() / kFs;
    m.hasLevel = p56(x, m.level, m.activity);
    int peak = 0, run = 0;
    for (int16_t s : x) {
        int a = s < 0 ? -(int)s : s;
        peak = std::max(peak, a);
        if (a >= 32700) { ++m.sat; if (++run == 2) ++m.flatRuns; } else run = 0;
    }
    m.peakDb = 20 * std::log10(std::max(peak, 1) / 32768.0);
    const int hot = (int)(32768 * std::pow(10.0, -1 / 20.0));
    for (size_t i = 0; i + kFrame <= x.size(); i += kFrame) {
        int p = 0;
        for (int k = 0; k < kFrame; ++k) p = std::max(p, std::abs((int)x[i + k]));
        if (p >= hot) ++m.hotFrames;
    }
    return m;
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

void printJson(const std::string& file, const std::string& who, const std::string& when,
               const std::string& codec, const Measure& m) {
    printf("{\"file\":\"%s\"", jsonEsc(file).c_str());
    if (!who.empty()) printf(",\"speaker\":\"%s\",\"start\":\"%s\"", jsonEsc(who).c_str(), jsonEsc(when).c_str());
    printf(",\"codec\":\"%s\",\"sec\":%.2f", codec.c_str(), m.sec);
    if (m.hasLevel) printf(",\"active_dbov\":%.1f,\"activity\":%.2f", m.level, m.activity);
    else printf(",\"active_dbov\":null,\"activity\":0");
    printf(",\"peak_dbfs\":%.2f,\"sat_samples\":%ld,\"flat_runs\":%ld,\"hot_frames\":%ld}\n",
           m.peakDb, m.sat, m.flatRuns, m.hotFrames);
}

// ── 녹취 디렉터리 순회 ─────────────────────────────────────────────────────

/** 아주 작은 JSON 문자열 필드 추출기 — 세그먼트 메타(CMP 가 쓰는 고정 형식)만 읽는다. */
std::string field(const std::string& j, const std::string& key, size_t from = 0) {
    size_t k = j.find("\"" + key + "\"", from);
    if (k == std::string::npos) return "";
    size_t c = j.find(':', k), q1 = j.find('"', c + 1);
    if (c == std::string::npos || q1 == std::string::npos) return "";
    size_t q2 = j.find('"', q1 + 1);
    return q2 == std::string::npos ? "" : j.substr(q1 + 1, q2 - q1 - 1);
}
int intField(const std::string& j, const std::string& key, size_t from) {
    size_t k = j.find("\"" + key + "\"", from);
    return k == std::string::npos ? -1 : atoi(j.c_str() + j.find(':', k) + 1);
}

struct Track { std::string file, who, when; int pt; };

/** seg_*.json 하나 → 오디오 트랙 목록. PTT = tracks[].speakers[0].id, VoLTE = side a/b → caller/callee. */
std::vector<Track> tracksOf(const fs::path& meta) {
    std::ifstream in(meta);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string j = ss.str();
    std::vector<Track> out;
    fs::path base = meta.parent_path();
    // PTT 메타의 file 은 세션 루트 기준(seg/000/…), VoLTE 는 같은 디렉터리 기준
    std::string ms = meta.string();
    size_t segPos = ms.rfind("/seg/");
    fs::path pttRoot = segPos != std::string::npos ? fs::path(ms.substr(0, segPos)) : base;
    std::string when = field(j, "start_time").substr(0, 19);
    size_t t = j.find("\"tracks\"");
    for (size_t p = t; t != std::string::npos && (p = j.find("\"kind\"", p)) != std::string::npos; ++p) {
        size_t objStart = j.rfind('{', p);
        if (field(j, "kind", objStart) != "audio") continue;
        std::string file = field(j, "file", objStart);
        int pt = intField(j, "pt", objStart);
        std::string side = field(j, "side", objStart);
        std::string who;
        size_t sp = j.find("\"speakers\"", objStart), next = j.find("\"kind\"", p + 1);
        if (sp != std::string::npos && (next == std::string::npos || sp < next)) who = field(j, "id", sp);
        else if (side == "a") who = field(j, "caller");
        else if (side == "b") who = field(j, "callee");
        fs::path f = file.rfind("seg/", 0) == 0 ? pttRoot / file : base / file;
        out.push_back({f.string(), who, when, pt});
    }
    return out;
}

double pct(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[(size_t)std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5))];
}

int usage() {
    fprintf(stderr, "usage: cims-rec-level <file.rtp> [--pt N]\n"
                    "       cims-rec-level --dir <recordings root> [--since YYYY-MM-DD] [--summary]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir, file, since;
    int pt = -1;
    bool summary = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--since" && i + 1 < argc) since = argv[++i];
        else if (a == "--pt" && i + 1 < argc) pt = atoi(argv[++i]);
        else if (a == "--summary") summary = true;
        else if (a[0] != '-') file = a;
        else return usage();
    }
    if (dir.empty() && file.empty()) return usage();

    if (!file.empty()) {
        std::vector<int16_t> x;
        std::string codec;
        if (!decodeTrack(file, pt, x, codec)) { fprintf(stderr, "open failed: %s\n", file.c_str()); return 1; }
        printJson(file, "", "", codec, measure(x));
        return 0;
    }

    struct Agg { std::vector<double> lvl; long turns = 0, satTurns = 0; std::string first, last; };
    std::map<std::string, Agg> agg;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        std::string name = p.filename().string();
        if (name.rfind("seg_", 0) != 0 || p.extension() != ".json") continue;
        for (const Track& t : tracksOf(p)) {
            if (!since.empty() && t.when.substr(0, 10) < since) continue;
            std::vector<int16_t> x;
            std::string codec;
            if (!fs::exists(t.file) || !decodeTrack(t.file, t.pt, x, codec) || x.size() < (size_t)kFs) continue;
            Measure m = measure(x);
            if (!summary) { printJson(t.file, t.who, t.when, codec, m); continue; }
            if (!m.hasLevel || m.activity < 0.1) continue;     // 말하지 않은 leg 제외
            Agg& g = agg[t.who.empty() ? "?" : t.who];
            g.lvl.push_back(m.level);
            ++g.turns;
            if (m.flatRuns > 0) ++g.satTurns;
            if (g.first.empty() || t.when < g.first) g.first = t.when;
            if (t.when > g.last) g.last = t.when;
        }
    }
    if (summary) {
        printf("%-18s %5s %8s %8s %8s %8s  %-16s %-16s\n", "speaker", "turns", "p10", "median", "p90", "sat%",
               "first", "last");
        for (auto& [who, g] : agg)
            printf("%-18s %5ld %8.1f %8.1f %8.1f %7.0f%%  %-16s %-16s\n", who.c_str(), g.turns, pct(g.lvl, 0.1),
                   pct(g.lvl, 0.5), pct(g.lvl, 0.9), 100.0 * g.satTurns / g.turns, g.first.substr(0, 16).c_str(),
                   g.last.substr(0, 16).c_str());
    }
    return 0;
}
