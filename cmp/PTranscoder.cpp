#include "PTranscoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dec_if.h"
#include "enc_if.h"

// ────────────────────────────────────────────────────────────────────────────
//  PCodecDesc
// ────────────────────────────────────────────────────────────────────────────
PCodecDesc PCodecDesc::make(const std::string& name, int rate, int pt, const std::string& fmtp) {
    PCodecDesc d;
    std::string n;
    for (char c : name) n += (char)toupper((unsigned char)c);
    if (n == "AMRWB" || n == "AMR_WB") n = "AMR-WB";
    if (n == "G711U" || n == "G.711U") n = "PCMU";
    if (n == "G711A" || n == "G.711A") n = "PCMA";
    d.name = n;
    d.rate = rate > 0 ? rate : (n == "AMR-WB" ? 16000 : 8000);
    d.pt = pt;
    d.fmtp = fmtp;
    return d;
}

bool PCodecDesc::amrOctetAlign() const {
    return fmtp.find("octet-align=1") != std::string::npos;
}

int PCodecDesc::amrMaxMode() const {
    size_t p = fmtp.find("mode-set=");
    if (p == std::string::npos) return 8;
    int best = -1;
    const char* s = fmtp.c_str() + p + 9;
    while (*s) {
        if (isdigit((unsigned char)*s)) {
            int m = atoi(s);
            if (m >= 0 && m <= 8 && m > best) best = m;
            while (isdigit((unsigned char)*s)) ++s;
        } else if (*s == ',') {
            ++s;
        } else {
            break;
        }
    }
    return best < 0 ? 8 : best;
}

// ────────────────────────────────────────────────────────────────────────────
//  AMR-WB 프레이밍 (RFC 4867 §4.4) — FT 0..8 음성, 9 SID, 14 speech lost, 15 NO_DATA
// ────────────────────────────────────────────────────────────────────────────
static const int kAmrWbBits[16] = {132, 177, 253, 285, 317, 365, 397, 461, 477, 40, 0, 0, 0, 0, 0, 0};
static const int kAmrWbBytes[16] = {17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0};

int PTranscoder::amrWbBytes(int ft) { return (ft >= 0 && ft < 16) ? kAmrWbBytes[ft] : 0; }
int PTranscoder::amrWbBits(int ft) { return (ft >= 0 && ft < 16) ? kAmrWbBits[ft] : 0; }

namespace {
struct BitReader {
    const unsigned char* p; int len; int pos = 0;   // pos = 비트 위치
    BitReader(const unsigned char* d, int l) : p(d), len(l) {}
    bool avail(int n) const { return pos + n <= len * 8; }
    unsigned read(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i) {
            int byte = pos >> 3, bit = 7 - (pos & 7);
            v = (v << 1) | ((p[byte] >> bit) & 1);
            ++pos;
        }
        return v;
    }
};
struct BitWriter {
    std::string out; int pos = 0;
    void write(unsigned v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if ((pos & 7) == 0) out.push_back(0);
            if ((v >> i) & 1) out.back() = (char)((unsigned char)out.back() | (0x80 >> (pos & 7)));
            ++pos;
        }
    }
};
bool _validFt(int ft) { return (ft >= 0 && ft <= 9) || ft == 14 || ft == 15; }
}  // namespace

bool PTranscoder::parseAmrWb(const unsigned char* payload, int len, int octetAlign, std::vector<AmrFrame>& frames) {
    frames.clear();
    if (len < 2) return false;
    auto parseOctet = [&](std::vector<AmrFrame>& out) -> bool {
        // [CMR|0000] [ToC...] [data...] — ToC F(1) FT(4) Q(1) P(2)
        int i = 1;
        std::vector<std::pair<int, int>> toc;
        for (;;) {
            if (i >= len) return false;
            unsigned char t = payload[i++];
            int ft = (t >> 3) & 0x0F, q = (t >> 2) & 1;
            if (!_validFt(ft)) return false;
            toc.push_back({ft, q});
            if (!(t & 0x80)) break;
        }
        for (auto& e : toc) {
            AmrFrame f;
            f.ft = e.first; f.q = e.second; f.bytes = kAmrWbBytes[f.ft];
            if (i + f.bytes > len) return false;
            memcpy(f.data, payload + i, f.bytes);
            i += f.bytes;
            out.push_back(f);
        }
        return i == len;   // 정확히 소진 — 남는 바이트는 bandwidth-efficient 오인
    };
    auto parseBe = [&](std::vector<AmrFrame>& out) -> bool {
        BitReader br(payload, len);
        if (!br.avail(4)) return false;
        br.read(4);   // CMR
        std::vector<std::pair<int, int>> toc;
        for (;;) {
            if (!br.avail(6)) return false;
            unsigned f = br.read(1), ft = br.read(4), q = br.read(1);
            if (!_validFt((int)ft)) return false;
            toc.push_back({(int)ft, (int)q});
            if (!f) break;
        }
        for (auto& e : toc) {
            AmrFrame fr;
            fr.ft = e.first; fr.q = e.second;
            int bits = kAmrWbBits[fr.ft];
            if (!br.avail(bits)) return false;
            fr.bytes = (bits + 7) / 8;
            memset(fr.data, 0, sizeof(fr.data));
            for (int b = 0; b < bits; ++b)
                if (br.read(1)) fr.data[b >> 3] |= (unsigned char)(0x80 >> (b & 7));
            out.push_back(fr);
        }
        return true;   // 패딩 허용
    };
    if (octetAlign > 0) return parseOctet(frames);
    if (octetAlign == 0) return parseBe(frames);
    // 자동 — octet-aligned 는 CMR 하위 4비트 0 + 길이 정확 일치. 아니면 bandwidth-efficient
    if ((payload[0] & 0x0F) == 0 && parseOctet(frames)) return true;
    frames.clear();
    return parseBe(frames);
}

std::string PTranscoder::buildAmrWb(const AmrFrame& f, bool octetAlign) {
    std::string out;
    if (octetAlign) {
        out.push_back((char)0xF0);                                    // CMR 15 = 요청 없음
        out.push_back((char)(((f.ft & 0x0F) << 3) | ((f.q & 1) << 2)));   // F=0
        out.append((const char*)f.data, kAmrWbBytes[f.ft]);
        return out;
    }
    BitWriter bw;
    bw.write(15, 4);
    bw.write(0, 1); bw.write((unsigned)f.ft, 4); bw.write((unsigned)f.q, 1);
    int bits = kAmrWbBits[f.ft];
    for (int b = 0; b < bits; ++b) bw.write((f.data[b >> 3] >> (7 - (b & 7))) & 1, 1);
    return bw.out;
}

// ────────────────────────────────────────────────────────────────────────────
//  G.711 (ITU-T G.711 — Sun g711.c 동형)
// ────────────────────────────────────────────────────────────────────────────
namespace {
const int kBias = 0x84, kClip = 8159;
const int16_t kSegAend[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
const int16_t kSegUend[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
int _search(int val, const int16_t* table, int size) {
    for (int i = 0; i < size; i++) if (val <= table[i]) return i;
    return size;
}
unsigned char _lin2alaw(int pcm) {
    int mask, seg; unsigned char aval;
    pcm = pcm >> 3;
    if (pcm >= 0) mask = 0xD5; else { mask = 0x55; pcm = -pcm - 1; }
    seg = _search(pcm, kSegAend, 8);
    if (seg >= 8) return (unsigned char)(0x7F ^ mask);
    aval = (unsigned char)(seg << 4);
    aval |= (seg < 2) ? (pcm >> 1) & 0x0F : (pcm >> seg) & 0x0F;
    return aval ^ mask;
}
int _alaw2lin(unsigned char a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    switch (seg) { case 0: t += 8; break; case 1: t += 0x108; break; default: t += 0x108; t <<= seg - 1; }
    return (a & 0x80) ? t : -t;
}
unsigned char _lin2ulaw(int pcm) {
    int mask, seg; unsigned char uval;
    pcm = pcm >> 2;
    if (pcm < 0) { pcm = -pcm; mask = 0x7F; } else mask = 0xFF;
    if (pcm > kClip) pcm = kClip;
    pcm += (kBias >> 2);
    seg = _search(pcm, kSegUend, 8);
    if (seg >= 8) return (unsigned char)(0x7F ^ mask);
    uval = (unsigned char)((seg << 4) | ((pcm >> (seg + 1)) & 0x0F));
    return uval ^ mask;
}
int _ulaw2lin(unsigned char u) {
    u = ~u;
    int t = ((u & 0x0F) << 3) + kBias;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (kBias - t) : (t - kBias);
}
}  // namespace

void PTranscoder::_g711ToPcm(const unsigned char* in, int n, int16_t* out) const {
    const bool alaw = _in.name == "PCMA";
    for (int i = 0; i < n; ++i) out[i] = (int16_t)(alaw ? _alaw2lin(in[i]) : _ulaw2lin(in[i]));
}
void PTranscoder::_pcmToG711(const int16_t* in, int n, unsigned char* out) const {
    const bool alaw = _out.name == "PCMA";
    for (int i = 0; i < n; ++i) out[i] = alaw ? _lin2alaw(in[i]) : _lin2ulaw(in[i]);
}

// ────────────────────────────────────────────────────────────────────────────
//  하프밴드 FIR 리샘플러 (47탭 해밍 창 sinc, 차단 = 16 kHz 의 4 kHz)
// ────────────────────────────────────────────────────────────────────────────
static const int kTaps = 47;

static void _initFir(std::vector<float>& h) {
    h.assign(kTaps, 0.0f);
    const int M = (kTaps - 1) / 2;
    double sum = 0;
    for (int n = 0; n < kTaps; ++n) {
        double x = n - M;
        double sinc = x == 0 ? 0.5 : std::sin(M_PI * 0.5 * x) / (M_PI * x);
        double w = 0.54 - 0.46 * std::cos(2 * M_PI * n / (kTaps - 1));
        h[n] = (float)(sinc * w);
        sum += h[n];
    }
    for (auto& v : h) v = (float)(v / sum);   // DC 이득 1
}

static inline int16_t _clip16(float v) {
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (int16_t)std::lrint(v);
}

void PTranscoder::_upsample(const int16_t* in8k, int n, std::vector<int16_t>& out16k) {
    // 0 삽입(×2) 뒤 저역 통과 — 이득 2 로 보상. 이력 = 직전 kTaps-1 개의 16 kHz 삽입 샘플
    std::vector<int16_t> x;
    x.reserve(_histUp.size() + 2 * n);
    x.insert(x.end(), _histUp.begin(), _histUp.end());
    for (int i = 0; i < n; ++i) { x.push_back(in8k[i]); x.push_back(0); }
    out16k.resize(2 * n);
    const int H = (int)_histUp.size();
    for (int i = 0; i < 2 * n; ++i) {
        float acc = 0;
        const int base = H + i;   // x[base] 가 현재 샘플, 과거 방향으로 kTaps
        for (int k = 0; k < kTaps; ++k) acc += _fir[k] * x[base - k];
        out16k[i] = _clip16(acc * 2.0f);
    }
    _histUp.assign(x.end() - (kTaps - 1), x.end());
}

void PTranscoder::_downsample(const int16_t* in16k, int n, std::vector<int16_t>& out8k) {
    std::vector<int16_t> x;
    x.reserve(_histDown.size() + n);
    x.insert(x.end(), _histDown.begin(), _histDown.end());
    x.insert(x.end(), in16k, in16k + n);
    out8k.resize(n / 2);
    const int H = (int)_histDown.size();
    for (int i = 0; i < n / 2; ++i) {
        const int base = H + 2 * i + 1;
        float acc = 0;
        for (int k = 0; k < kTaps; ++k) acc += _fir[k] * x[base - k];
        out8k[i] = _clip16(acc);
    }
    _histDown.assign(x.end() - (kTaps - 1), x.end());
}

// ────────────────────────────────────────────────────────────────────────────
//  PTranscoder
// ────────────────────────────────────────────────────────────────────────────
bool PTranscoder::supportedPair(const PCodecDesc& in, const PCodecDesc& out) {
    if (!in.valid() || !out.valid() || in.sameCodec(out)) return false;
    return (in.isAmrWb() && out.isG711()) || (in.isG711() && out.isAmrWb());
}

PTranscoder::PTranscoder(const PCodecDesc& in, const PCodecDesc& out) : _in(in), _out(out) {
    if (!supportedPair(in, out)) return;
    _initFir(_fir);
    _histUp.assign(kTaps - 1, 0);
    _histDown.assign(kTaps - 1, 0);
    if (_in.isAmrWb()) {
        _dec = D_IF_init();
        _inOctetAlign = _in.fmtp.empty() ? -1 : (_in.amrOctetAlign() ? 1 : 0);
    }
    if (_out.isAmrWb()) {
        _enc = E_IF_init();
        _outOctetAlign = _out.amrOctetAlign();
        _encMode = _out.amrMaxMode();
    }
    _ok = (!_in.isAmrWb() || _dec) && (!_out.isAmrWb() || _enc);
}

PTranscoder::~PTranscoder() {
    if (_dec) D_IF_exit(_dec);
    if (_enc) E_IF_exit(_enc);
}

bool PTranscoder::_parseRtp(const unsigned char* pkt, int len, RtpHdr& h) {
    if (len < 12 || (pkt[0] >> 6) != 2) return false;
    const int cc = pkt[0] & 0x0F;
    const bool x = (pkt[0] & 0x10) != 0, p = (pkt[0] & 0x20) != 0;
    int off = 12 + 4 * cc;
    if (off > len) return false;
    if (x) {
        if (off + 4 > len) return false;
        int words = (pkt[off + 2] << 8) | pkt[off + 3];
        off += 4 + 4 * words;
        if (off > len) return false;
    }
    int end = len;
    if (p) { int pad = pkt[len - 1]; if (pad <= 0 || off + pad > len) return false; end = len - pad; }
    h.marker = (pkt[1] & 0x80) != 0;
    h.pt = pkt[1] & 0x7F;
    h.seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    h.ts = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
    h.ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) | ((uint32_t)pkt[10] << 8) | pkt[11];
    h.payload = pkt + off;
    h.len = end - off;
    return h.len >= 0;
}

uint32_t PTranscoder::_mapTs(uint32_t inTs) {
    if (!_tsInit) {
        _tsInit = true;
        _baseIn = inTs;
        _baseOut = (uint32_t)(((uint64_t)inTs * (uint64_t)_out.rate) / (uint64_t)_in.rate);
    }
    int64_t d = (int32_t)(inTs - _baseIn);
    d = d * _out.rate / _in.rate;
    return (uint32_t)((int64_t)_baseOut + d);
}

void PTranscoder::rescaleTimestamp(unsigned char* pkt, int len) {
    if (len < 12) return;
    uint32_t ts = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
    ts = _mapTs(ts);
    pkt[4] = (unsigned char)(ts >> 24); pkt[5] = (unsigned char)(ts >> 16); pkt[6] = (unsigned char)(ts >> 8); pkt[7] = (unsigned char)ts;
}

std::string PTranscoder::_makeRtp(bool marker, uint32_t ts, const unsigned char* payload, int len) {
    std::string o;
    o.resize(12 + len);
    unsigned char* p = (unsigned char*)&o[0];
    p[0] = 0x80;
    p[1] = (unsigned char)((marker ? 0x80 : 0) | (_out.pt & 0x7F));
    p[2] = (unsigned char)(_seq >> 8); p[3] = (unsigned char)_seq; ++_seq;
    p[4] = (unsigned char)(ts >> 24); p[5] = (unsigned char)(ts >> 16); p[6] = (unsigned char)(ts >> 8); p[7] = (unsigned char)ts;
    // SSRC 는 transcode() 가 채운다(호출자 컨텍스트) — 여기서는 자리만
    memcpy(p + 12, payload, len);
    return o;
}

void PTranscoder::_decodeToPcm16k(const AmrFrame& f, int16_t* pcm320) {
    unsigned char buf[64];
    buf[0] = (unsigned char)(((f.ft & 0x0F) << 3) | ((f.q & 1) << 2));   // RFC 4867 저장 형식 헤더
    memcpy(buf + 1, f.data, kAmrWbBytes[f.ft]);
    D_IF_decode(_dec, buf, pcm320, f.ft == 14 ? 1 : _good_frame);
}

bool PTranscoder::transcode(const unsigned char* pkt, int len, std::vector<std::string>& out) {
    out.clear();
    if (!_ok) return false;
    RtpHdr h;
    if (!_parseRtp(pkt, len, h)) { ++_dropped; return false; }
    if (!_seqInit) { _seqInit = true; _seq = h.seq; }
    auto stampSsrc = [&](std::string& o) {
        unsigned char* p = (unsigned char*)&o[0];
        p[8] = (unsigned char)(h.ssrc >> 24); p[9] = (unsigned char)(h.ssrc >> 16); p[10] = (unsigned char)(h.ssrc >> 8); p[11] = (unsigned char)h.ssrc;
    };

    if (_in.isAmrWb()) {
        // AMR-WB → G.711: 프레임마다 320 샘플 → 160 샘플 → 160 바이트 패킷
        std::vector<AmrFrame> frames;
        if (!parseAmrWb(h.payload, h.len, _inOctetAlign, frames) || frames.empty()) { ++_dropped; return false; }
        uint32_t ts = _mapTs(h.ts);
        int16_t pcm[320];
        std::vector<int16_t> pcm8k;
        unsigned char g711[160];
        bool first = true;
        for (const AmrFrame& f : frames) {
            ++_framesIn;
            _decodeToPcm16k(f, pcm);
            _downsample(pcm, 320, pcm8k);
            _pcmToG711(pcm8k.data(), 160, g711);
            std::string o = _makeRtp(first && h.marker, ts, g711, 160);
            stampSsrc(o);
            out.push_back(std::move(o));
            ts += 160;
            first = false;
            ++_framesOut;
        }
        return true;
    }

    // G.711 → AMR-WB: 8 kHz 샘플을 16 kHz 로 올려 320 샘플마다 프레임 하나
    if (h.len <= 0) return true;
    std::vector<int16_t> pcm8k(h.len), pcm16k;
    _g711ToPcm(h.payload, h.len, pcm8k.data());
    _upsample(pcm8k.data(), h.len, pcm16k);
    uint32_t inMapped = _mapTs(h.ts);
    if (_pcmBuf.empty()) {
        _bufTs = inMapped;
        _pendingMarker = h.marker;
    } else {
        // 불연속(> 1 프레임)이면 버퍼를 버리고 새 timestamp 기준으로 — 손실 은닉은 하지 않는다
        uint32_t expect = _bufTs + (uint32_t)_pcmBuf.size();
        if ((int32_t)(inMapped - expect) > 320 || (int32_t)(inMapped - expect) < -320) {
            _pcmBuf.clear();
            _bufTs = inMapped;
            _pendingMarker = true;
        }
    }
    _pcmBuf.insert(_pcmBuf.end(), pcm16k.begin(), pcm16k.end());
    unsigned char enc[64];
    while (_pcmBuf.size() >= 320) {
        ++_framesIn;
        int n = E_IF_encode(_enc, _encMode, _pcmBuf.data(), enc, 0);
        _pcmBuf.erase(_pcmBuf.begin(), _pcmBuf.begin() + 320);
        if (n < 1) { _bufTs += 320; continue; }
        AmrFrame f;
        f.ft = (enc[0] >> 3) & 0x0F;
        f.q = (enc[0] >> 2) & 1;
        f.bytes = std::min(n - 1, (int)sizeof(f.data));
        memcpy(f.data, enc + 1, f.bytes);
        std::string payload = buildAmrWb(f, _outOctetAlign);
        std::string o = _makeRtp(_pendingMarker, _bufTs, (const unsigned char*)payload.data(), (int)payload.size());
        stampSsrc(o);
        out.push_back(std::move(o));
        _pendingMarker = false;
        _bufTs += 320;
        ++_framesOut;
    }
    return true;
}
