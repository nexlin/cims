// cims-sample-conv — 계측기 미디어 샘플 변환기 (test_instrument.md §4 미디어 평면).
//   입력 WAV(PCM 8/16/24/32-bit·float, mono/stereo, 8~48 kHz) → 16-bit PCM 16 kHz mono 마스터(<id>.wav) + 코덱별 raw 파일:
//     <id>.pcmu / <id>.pcma   G.711(8 kHz 재표본, 160 B = 20 ms)        <id>.g722   G.722 64 kbit/s(160 B = 20 ms)
//     <id>.amrwb              AMR-WB 23.85 kbps raw 61 B 프레임(DTX 없음)  <id>_dtx.amrwb  RFC 4867 §5 저장 형식(VAD/DTX — SPEECH·SID·NO_DATA)
//   + 측정(ITU-T P.56 활성 음성 레벨·활동률, 20 ms 에너지+hangover on/off 통계, DTX 프레임 집계)을 JSON 으로 stdout 에.
//   컨트롤러(oam-cims-tester)가 콘솔 등록 때 부르고, 동봉 샘플 생성기(gen_samples.py)도 같은 변환기를 쓴다 — 변환 경로는 하나.
//
//   cims-sample-conv --in <wav> --out-dir <dir> --id <id> [--codecs pcmu,pcma,g722,amr-wb] [--no-dtx] [--normalize <dBov>] [--master-only]
//   cims-sample-conv --measure <wav>          측정만(JSON)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "G711.h"
#include "enc_if.h"
#include "g722_enc.h"

static const int FS = 16000;
static const int FRAME = 320;   // 20 ms @ 16 kHz

struct Wav { int rate = 0; int channels = 0; int bits = 0; std::vector<double> mono; };   // mono = int16 스케일(-32768..32767) 실수

static bool fail(const std::string& why) { fprintf(stdout, "{\"error\":\"%s\"}\n", why.c_str()); return false; }

// ── WAV ──────────────────────────────────────────────────────────────────────────────────────────────────────────
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool readWav(const std::string& path, Wav& w, std::string& err) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> d; uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(fp);
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) != 0 || memcmp(d.data() + 8, "WAVE", 4) != 0) { err = "not a RIFF/WAVE file"; return false; }
    int fmtTag = 0; size_t dataOff = 0, dataLen = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t len = rd32(d.data() + p + 4);
        const uint8_t* body = d.data() + p + 8;
        size_t avail = d.size() - (p + 8);
        if (memcmp(d.data() + p, "fmt ", 4) == 0 && len >= 16 && avail >= 16) {
            fmtTag = rd16(body); w.channels = rd16(body + 2); w.rate = (int)rd32(body + 4); w.bits = rd16(body + 14);
            if (fmtTag == 0xFFFE && len >= 40 && avail >= 40) fmtTag = rd16(body + 24);   // WAVE_FORMAT_EXTENSIBLE — SubFormat 의 첫 2 바이트
        } else if (memcmp(d.data() + p, "data", 4) == 0) {
            dataOff = p + 8; dataLen = len > avail ? avail : len; break;
        }
        p += 8 + len + (len & 1);
    }
    if (!dataOff || !w.rate || !w.channels) { err = "fmt/data chunk missing"; return false; }
    if (fmtTag != 1 && fmtTag != 3) { err = "unsupported WAVE format tag " + std::to_string(fmtTag) + " (PCM/float only)"; return false; }
    if (w.rate < 8000 || w.rate > 48000) { err = "sample rate " + std::to_string(w.rate) + " out of range 8000..48000"; return false; }
    if (w.channels < 1 || w.channels > 8) { err = "channels " + std::to_string(w.channels); return false; }
    int bps = w.bits / 8;
    if (!((fmtTag == 1 && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32)) || (fmtTag == 3 && w.bits == 32))) { err = "unsupported bit depth " + std::to_string(w.bits); return false; }
    size_t frames = dataLen / (bps * w.channels);
    w.mono.resize(frames);
    const uint8_t* s = d.data() + dataOff;
    for (size_t i = 0; i < frames; ++i) {
        double acc = 0;
        for (int c = 0; c < w.channels; ++c) {
            const uint8_t* q = s + (i * w.channels + c) * bps; double v;
            if (fmtTag == 3) { float f; memcpy(&f, q, 4); v = f * 32768.0; }
            else if (w.bits == 8) v = ((int)q[0] - 128) * 256.0;
            else if (w.bits == 16) v = (int16_t)rd16(q);
            else if (w.bits == 24) v = ((int32_t)((q[0] << 8) | (q[1] << 16) | (q[2] << 24)) >> 8) / 256.0;
            else v = (int32_t)rd32(q) / 65536.0;
            acc += v;
        }
        w.mono[i] = acc / w.channels;
    }
    return true;
}

static bool writeWav16k(const std::string& path, const std::vector<double>& x) {
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    uint32_t dataLen = (uint32_t)(x.size() * 2);
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    auto w32 = [&](int off, uint32_t v) { h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF; h[off + 2] = (v >> 16) & 0xFF; h[off + 3] = (v >> 24) & 0xFF; };
    w32(4, 36 + dataLen); w32(24, FS); w32(28, FS * 2); w32(40, dataLen);
    fwrite(h, 1, 44, fp);
    std::vector<int16_t> s(x.size());
    for (size_t i = 0; i < x.size(); ++i) { double v = std::round(x[i]); s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    fwrite(s.data(), 2, s.size(), fp);
    fclose(fp);
    return true;
}

// ── 재표본 — 윈도우 sinc(Blackman) 보간. 하향은 컷오프를 출력 나이퀴스트로 ────────────────────────────────────────
static std::vector<double> resample(const std::vector<double>& x, int fsIn, int fsOut) {
    if (fsIn == fsOut) return x;
    double ratio = (double)fsOut / fsIn;
    double fc = ratio < 1.0 ? ratio : 1.0;      // 입력 나이퀴스트 기준 컷오프
    fc *= 0.92;                                  // 전이 대역 여유
    int half = (int)std::ceil(32.0 / fc);        // 반폭(입력 표본 단위)
    size_t nOut = (size_t)std::floor((double)x.size() * ratio);
    std::vector<double> y(nOut);
    for (size_t n = 0; n < nOut; ++n) {
        double t = n / ratio; long k0 = (long)std::floor(t);
        double acc = 0, wsum = 0;
        for (long k = k0 - half + 1; k <= k0 + half; ++k) {
            if (k < 0 || k >= (long)x.size()) continue;
            double tau = t - k;
            double s = tau == 0 ? fc : std::sin(M_PI * fc * tau) / (M_PI * tau);
            double u = tau / half;  // -1..1
            double wnd = 0.42 + 0.5 * std::cos(M_PI * u) + 0.08 * std::cos(2 * M_PI * u);
            acc += x[k] * s * wnd; wsum += s * wnd;
        }
        y[n] = ratio < 1.0 ? acc : acc / (wsum == 0 ? 1 : wsum) * 1.0;   // 상향은 DC 이득 정규화
    }
    if (ratio < 1.0) { /* 하향: sinc 이득이 fc 로 이미 정규화 */ }
    return y;
}

// ── ITU-T P.56 활성 음성 레벨 (method B) ────────────────────────────────────────────────────────────────────────────
struct P56 { double levelDbov; double activityPct; };
static P56 p56(const std::vector<double>& x, int fs) {
    if (x.empty()) return { -100.0, 0.0 };
    double g = std::exp(-1.0 / (fs * 0.03));
    std::vector<double> q(x.size()); double p = 0, qq = 0; double sq = 0;
    for (size_t i = 0; i < x.size(); ++i) { p = g * p + (1 - g) * std::fabs(x[i]); qq = g * qq + (1 - g) * p; q[i] = qq; sq += x[i] * x[i]; }
    if (sq <= 0) return { -100.0, 0.0 };
    long hang = (long)(0.2 * fs); size_t n = x.size();
    struct Row { double L, C; long a; bool ok; };
    std::vector<Row> rows(16);
    for (int j = 0; j < 16; ++j) {
        double c = 32768.0 * std::pow(2.0, -j); long a = 0; long last = -1000000000;
        for (size_t i = 0; i < n; ++i) { if (q[i] > c) last = (long)i; if ((long)i - last < hang) ++a; }
        rows[j].C = 20 * std::log10(c / 32768.0); rows[j].a = a; rows[j].ok = a > 0;
        rows[j].L = a > 0 ? 10 * std::log10(sq / a / (32768.0 * 32768.0)) : 0;
    }
    const double M = 15.9; const Row* prev = nullptr;
    for (int j = 15; j >= 0; --j) {   // 낮은 문턱부터 올린다 — (L-C) 단조 감소
        const Row& r = rows[j]; if (!r.ok) break;
        double d = r.L - r.C;
        if (d <= M) {
            if (!prev) return { r.L, 100.0 * r.a / n };
            double dp = prev->L - prev->C; double t = dp != d ? (dp - M) / (dp - d) : 0;
            return { prev->L + t * (r.L - prev->L), 100.0 * (prev->a + t * (r.a - prev->a)) / n };
        }
        prev = &r;
    }
    return prev ? P56{ prev->L, 100.0 * prev->a / n } : P56{ -100.0, 0.0 };
}

static double rmsDbov(const std::vector<double>& x) { double s = 0; for (double v : x) s += v * v; double r = x.empty() ? 0 : std::sqrt(s / x.size()); return r > 0 ? 20 * std::log10(r / 32768.0) : -100.0; }

// 20 ms 프레임 에너지 문턱(-45 dBov) + hangover 200 ms — on/off 통계(P.59 표 1 과 같은 '측정' 관점)
struct Pattern { double activityPct, spurtMean, pauseMean; int spurts; };
static Pattern pattern(const std::vector<double>& x) {
    size_t nf = x.size() / FRAME; std::vector<bool> on(nf); long last = -1000000000;
    for (size_t f = 0; f < nf; ++f) {
        double e = 0; for (int i = 0; i < FRAME; ++i) { double v = x[f * FRAME + i]; e += v * v; }
        double db = 10 * std::log10(e / FRAME / (32768.0 * 32768.0) + 1e-12);
        if (db > -45.0) last = (long)f;
        on[f] = ((long)f - last) < 10;
    }
    std::vector<double> sp, pa; if (!nf) return { 0, 0, 0, 0 };
    bool cur = on[0]; int len = 0; long act = 0;
    for (size_t f = 0; f < nf; ++f) { if (on[f]) ++act; if (on[f] == cur) ++len; else { (cur ? sp : pa).push_back(len * 0.02); cur = on[f]; len = 1; } }
    (cur ? sp : pa).push_back(len * 0.02);
    auto mean = [](const std::vector<double>& v) { double s = 0; for (double d : v) s += d; return v.empty() ? 0.0 : s / v.size(); };
    return { 100.0 * act / nf, mean(sp), mean(pa), (int)sp.size() };
}

// ── 인코더 ───────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<int16_t> toS16(const std::vector<double>& x) { std::vector<int16_t> s(x.size()); for (size_t i = 0; i < x.size(); ++i) { double v = std::round(x[i]); s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); } return s; }
static bool writeAll(const std::string& path, const std::vector<uint8_t>& d) { FILE* fp = fopen(path.c_str(), "wb"); if (!fp) return false; fwrite(d.data(), 1, d.size(), fp); fclose(fp); return true; }

static const int kAmrWbData[16] = { 17, 23, 32, 36, 40, 46, 50, 58, 60, 5, -1, -1, -1, -1, 0, 0 };

int main(int argc, char** argv) {
    std::string in, outDir, id, codecs = "pcmu,pcma,g722,amr-wb", measure; bool dtx = true, masterOnly = false, doNorm = false; double normDb = -26.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i]; auto next = [&](std::string& v) { if (i + 1 >= argc) { fail("missing value for " + a); exit(2); } v = argv[++i]; };
        if (a == "--in") next(in); else if (a == "--out-dir") next(outDir); else if (a == "--id") next(id); else if (a == "--codecs") next(codecs);
        else if (a == "--no-dtx") dtx = false; else if (a == "--master-only") masterOnly = true; else if (a == "--measure") next(measure);
        else if (a == "--normalize") { std::string v; next(v); doNorm = true; normDb = atof(v.c_str()); }
        else { fail("unknown option " + a); return 2; }
    }
    if (measure.empty() && (in.empty() || outDir.empty() || id.empty())) { fail("usage: --in <wav> --out-dir <dir> --id <id> | --measure <wav>"); return 2; }
    for (char c : id) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) { fail("id: 영숫자·_-. 만"); return 2; }

    Wav w; std::string err;
    if (!readWav(measure.empty() ? in : measure, w, err)) { fail(err); return 1; }
    std::vector<double> x = resample(w.mono, w.rate, FS);
    size_t pad = (FRAME - x.size() % FRAME) % FRAME; x.resize(x.size() + pad, 0.0);
    if (doNorm) {
        P56 lv = p56(x, FS);
        if (lv.levelDbov > -99) { double gain = std::pow(10.0, (normDb - lv.levelDbov) / 20.0); double peak = 0; for (double v : x) peak = std::max(peak, std::fabs(v) * gain); if (peak > 32000) gain *= 32000.0 / peak; for (double& v : x) v *= gain; }
    }
    P56 lv = p56(x, FS); Pattern pt = pattern(x);
    std::string json = "{";
    auto kv = [&](const std::string& k, const std::string& v) { if (json.size() > 1) json += ","; json += "\"" + k + "\":" + v; };
    auto num = [](double v, int prec = 1) { char b[64]; snprintf(b, sizeof(b), "%.*f", prec, v); return std::string(b); };
    if (!id.empty()) kv("id", "\"" + id + "\"");
    kv("input", "{\"rate\":" + std::to_string(w.rate) + ",\"channels\":" + std::to_string(w.channels) + ",\"bits\":" + std::to_string(w.bits) + "}");
    kv("duration_s", num(x.size() / (double)FS, 2));
    kv("p56_active_level_dbov", num(lv.levelDbov)); kv("p56_activity_pct", num(lv.activityPct)); kv("rms_dbov", num(rmsDbov(x)));
    kv("pattern", "{\"activity_pct\":" + num(pt.activityPct) + ",\"talkspurt_mean_s\":" + num(pt.spurtMean, 3) + ",\"pause_mean_s\":" + num(pt.pauseMean, 3) + ",\"talkspurts\":" + std::to_string(pt.spurts) + "}");
    if (!measure.empty()) { json += "}"; puts(json.c_str()); return 0; }

    std::string master = outDir + "/" + id + ".wav";
    if (!writeWav16k(master, x)) { fail("cannot write " + master); return 1; }
    kv("master", "\"" + id + ".wav\"");
    std::string files = "{"; std::string extra;
    auto want = [&](const char* c) { return !masterOnly && ("," + codecs + ",").find(std::string(",") + c + ",") != std::string::npos; };
    std::vector<int16_t> s16 = toS16(x);
    if (want("pcmu") || want("pcma")) {
        std::vector<int16_t> s8 = toS16(resample(x, FS, 8000)); s8.resize(s8.size() / 160 * 160);
        if (want("pcmu")) { std::vector<uint8_t> o(s8.size()); PcmToUlaw((const char*)s8.data(), (int)s8.size() * 2, (char*)o.data(), (int)o.size()); if (!writeAll(outDir + "/" + id + ".pcmu", o)) { fail("write pcmu"); return 1; } files += "\"pcmu\":\"" + id + ".pcmu\","; }
        if (want("pcma")) { std::vector<uint8_t> o(s8.size()); PcmToAlaw((const char*)s8.data(), (int)s8.size() * 2, (char*)o.data(), (int)o.size()); if (!writeAll(outDir + "/" + id + ".pcma", o)) { fail("write pcma"); return 1; } files += "\"pcma\":\"" + id + ".pcma\","; }
    }
    if (want("g722")) {
        // G.722 입력 스케일 — 이 인코더(pjproject 이식본)는 14-bit 포화 QMF 라 16-bit 를 그대로 넣으면 6 dB 높게 부호화된다.
        //   ffmpeg/spandsp 관례(입력 >>1)에 맞춰 반으로 줄여 넣는다 — 상대 디코더에서 원 레벨로 복원됨(실측: ffmpeg 디코더 -26 dBov 일치)
        std::vector<double> half(x.size()); for (size_t i = 0; i < x.size(); ++i) half[i] = x[i] * 0.5;
        std::vector<int16_t> s16h = toS16(half);
        g722_enc_t enc; g722_enc_init(&enc); std::vector<uint8_t> o(s16h.size() / 2);
        g722_enc_encode(&enc, s16h.data(), s16h.size(), o.data());
        if (!writeAll(outDir + "/" + id + ".g722", o)) { fail("write g722"); return 1; } files += "\"g722\":\"" + id + ".g722\",";
    }
    if (want("amr-wb")) {
        for (int pass = 0; pass < (dtx ? 2 : 1); ++pass) {
            void* st = E_IF_init(); if (!st) { fail("amr-wb encoder init"); return 1; }
            std::vector<uint8_t> o; unsigned char fr[64]; int cnt[16] = { 0 };
            if (pass == 1) { const char* magic = "#!AMR-WB\n"; o.insert(o.end(), magic, magic + 9); }
            for (size_t f = 0; f + FRAME <= s16.size(); f += FRAME) {
                int n = E_IF_encode(st, 8, s16.data() + f, fr, pass == 1 ? 1 : 0);   // mode 8 = 23.85 kbps, RFC 3267 프레임(ToC + 데이터)
                if (n <= 0) { E_IF_exit(st); fail("amr-wb encode"); return 1; }
                int ft = (fr[0] >> 3) & 0x0F; ++cnt[ft];
                if (pass == 0 && (ft != 8 || n != 61)) { E_IF_exit(st); fail("amr-wb non-dtx frame is not FT8/61B"); return 1; }
                if (pass == 1 && (kAmrWbData[ft] < 0 || n != 1 + kAmrWbData[ft])) { E_IF_exit(st); fail("amr-wb dtx frame size mismatch"); return 1; }
                o.insert(o.end(), fr, fr + n);
            }
            E_IF_exit(st);
            std::string name = pass == 0 ? id + ".amrwb" : id + "_dtx.amrwb";
            if (!writeAll(outDir + "/" + name, o)) { fail("write " + name); return 1; }
            if (pass == 0) files += "\"amr-wb\":\"" + name + "\",";
            else {
                int tot = 0; for (int c : cnt) tot += c;
                extra += ",\"amr-wb-dtx\":\"" + name + "\",\"dtx_frames\":{\"speech\":" + std::to_string(cnt[8]) + ",\"sid\":" + std::to_string(cnt[9]) + ",\"no_data\":" + std::to_string(cnt[15]) + ",\"total\":" + std::to_string(tot) + "}"
                       + ",\"dtx_channel_activity_pct\":" + num(tot ? 100.0 * (tot - cnt[15]) / tot : 0.0);
            }
        }
    }
    if (files.size() > 1 && files.back() == ',') files.pop_back();
    files += "}";
    kv("files", files); json += extra + "}";
    puts(json.c_str());
    return 0;
}
