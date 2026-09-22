#include "PAnnMixer.h"

#include <cstring>

#include "PAnnPlayer.h"
#include "PTranscoder.h"
#include "dec_if.h"
#include "enc_if.h"

namespace {
const int kBias = 0x84;
const int kClip = 8159;
}  // namespace

// G.711 (ITU-T G.711 표 변환 — PTranscoder 와 같은 식)
int16_t PAnnMixer::ulaw2lin(unsigned char u) {
    u = ~u;
    int t = ((u & 0x0F) << 3) + kBias;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (kBias - t) : (t - kBias));
}
int16_t PAnnMixer::alaw2lin(unsigned char a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else t = (t + 0x108) << (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}
unsigned char PAnnMixer::lin2ulaw(int16_t s) {
    int pcm = s;
    int mask = 0xFF;
    if (pcm < 0) { pcm = -pcm; mask = 0x7F; }
    pcm >>= 2;
    if (pcm > kClip) pcm = kClip;
    pcm += kBias >> 2;
    int seg = 0;
    for (int v = pcm >> 6; v > 0 && seg < 8; v >>= 1) ++seg;
    if (seg >= 8) return (unsigned char)(0x7F ^ mask);
    unsigned char u = (unsigned char)((seg << 4) | ((pcm >> (seg + 1)) & 0x0F));
    return u ^ mask;
}
unsigned char PAnnMixer::lin2alaw(int16_t s) {
    int pcm = s;
    int mask = 0xD5;
    if (pcm < 0) { pcm = -pcm - 1; mask = 0x55; }
    pcm >>= 3;
    unsigned char a;
    if (pcm >= 0x1000) pcm = 0xFFF;
    if (pcm < 0x20) a = (unsigned char)(pcm >> 1);
    else {
        int seg = 0;
        for (int v = pcm >> 5; v > 1 && seg < 7; v >>= 1) ++seg;
        a = (unsigned char)(((seg + 1) << 4) | ((pcm >> (seg + 1)) & 0x0F));
    }
    return a ^ mask;
}

bool PAnnMixer::supports(const std::string& codec) {
    return codec == "PCMU" || codec == "PCMA" || codec == "AMR-WB";
}

PAnnMixer::PAnnMixer(const std::string& codec, bool amrOctetAlign, int amrEncMode)
    : _codec(codec), _amr(codec == "AMR-WB"), _alaw(codec == "PCMA"), _octetAlign(amrOctetAlign),
      _encMode(amrEncMode < 0 || amrEncMode > 8 ? 8 : amrEncMode) {
    if (!supports(codec)) return;
    if (_amr) {
        _decStream = D_IF_init();
        _decTone = D_IF_init();
        _enc = E_IF_init();
        _ok = _decStream && _decTone && _enc;
    } else {
        _ok = true;
    }
}

PAnnMixer::~PAnnMixer() {
    if (_decStream) D_IF_exit(_decStream);
    if (_decTone) D_IF_exit(_decTone);
    if (_enc) E_IF_exit(_enc);
}

void PAnnMixer::_decodeAmr(void* dec, const unsigned char* storage, int storageLen, int16_t* pcm320) {
    unsigned char buf[64];
    memset(buf, 0, sizeof(buf));
    int n = storageLen > (int)sizeof(buf) ? (int)sizeof(buf) : storageLen;
    memcpy(buf, storage, (size_t)n);
    D_IF_decode(dec, buf, pcm320, 0);
}

bool PAnnMixer::_fillTone(PAnnPlayer& tone, size_t n) {
    while (_toneBuf.size() < n) {
        std::string frame;
        if (!tone.nextPayload(frame)) return false;   // 끝(completed/max)
        if (_amr) {
            int16_t pcm[320];
            if (frame.empty()) memset(pcm, 0, sizeof(pcm));   // NO_DATA/무음 구간
            else _decodeAmr(_decTone, (const unsigned char*)frame.data(), (int)frame.size(), pcm);
            _toneBuf.insert(_toneBuf.end(), pcm, pcm + 320);
        } else {
            if (frame.empty()) { _toneBuf.insert(_toneBuf.end(), 160, 0); continue; }
            for (unsigned char b : frame) _toneBuf.push_back(_alaw ? alaw2lin(b) : ulaw2lin(b));
        }
    }
    return true;
}

bool PAnnMixer::mix(const unsigned char* pkt, int len, PAnnPlayer& tone, std::string& out) {
    if (!_ok || len < 13) return false;
    const int hdr = 12 + 4 * (pkt[0] & 0x0F);   // CSRC
    if (len <= hdr) return false;
    const unsigned char* payload = pkt + hdr;
    const int plen = len - hdr;
    if (!_amr) {
        if (!_fillTone(tone, (size_t)plen)) return false;
        out.assign((const char*)pkt, (size_t)len);
        for (int i = 0; i < plen; ++i) {
            const int16_t s = _alaw ? alaw2lin(payload[i]) : ulaw2lin(payload[i]);
            const int16_t v = sat16((int)s + (int)_toneBuf[(size_t)i]);
            out[(size_t)hdr + (size_t)i] = (char)(_alaw ? lin2alaw(v) : lin2ulaw(v));
        }
        _toneBuf.erase(_toneBuf.begin(), _toneBuf.begin() + plen);
        ++_mixed;
        return true;
    }
    std::vector<PTranscoder::AmrFrame> frames;
    if (!PTranscoder::parseAmrWb(payload, plen, _octetAlign ? 1 : 0, frames) || frames.size() != 1) return false;
    if (!_fillTone(tone, 320)) return false;
    const PTranscoder::AmrFrame& f = frames[0];
    unsigned char storage[64];
    storage[0] = (unsigned char)(((f.ft & 0x0F) << 3) | ((f.q & 1) << 2));
    memcpy(storage + 1, f.data, (size_t)f.bytes);
    int16_t pcm[320];
    _decodeAmr(_decStream, storage, f.bytes + 1, pcm);
    for (int i = 0; i < 320; ++i) pcm[i] = sat16((int)pcm[i] + (int)_toneBuf[(size_t)i]);
    _toneBuf.erase(_toneBuf.begin(), _toneBuf.begin() + 320);
    unsigned char enc[64];
    const int n = E_IF_encode(_enc, _encMode, pcm, enc, 0);
    if (n < 2) return false;
    PTranscoder::AmrFrame o;
    o.ft = (enc[0] >> 3) & 0x0F;
    o.q = (enc[0] >> 2) & 1;
    o.bytes = n - 1 > (int)sizeof(o.data) ? (int)sizeof(o.data) : n - 1;
    memcpy(o.data, enc + 1, (size_t)o.bytes);
    out.assign((const char*)pkt, (size_t)hdr);
    out += PTranscoder::buildAmrWb(o, _octetAlign);
    ++_mixed;
    return true;
}
