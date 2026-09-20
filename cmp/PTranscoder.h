#ifndef __PTRANSCODER_H__
#define __PTRANSCODER_H__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * leg 별 코덱 선언 (RELAY_ADD/MODIFY payload `media_codec` — cmp_media_api.md §6.6).
 *   name 은 대문자 정규화(AMR-WB | PCMU | PCMA), rate = clock, pt = wire PT, fmtp = AMR-WB 의 octet-align/mode-set 원문.
 */
struct PCodecDesc {
    std::string name;
    int rate = 0;
    int pt = -1;
    std::string fmtp;

    bool valid() const { return !name.empty() && pt >= 0 && rate > 0; }
    bool isAmrWb() const { return name == "AMR-WB"; }
    bool isG711() const { return name == "PCMU" || name == "PCMA"; }
    bool sameCodec(const PCodecDesc& o) const { return name == o.name && rate == o.rate; }
    std::string label() const { return name + "/" + std::to_string(rate); }
    /** 이름 정규화(대소문자·`AMRWB`→`AMR-WB`) + rate 기본값(AMR-WB 16000 · G.711 8000). */
    static PCodecDesc make(const std::string& name, int rate, int pt, const std::string& fmtp);
    /** fmtp 의 `octet-align=1` (없으면 bandwidth-efficient, RFC 4867 §8.1). */
    bool amrOctetAlign() const;
    /** fmtp `mode-set` 의 최고 모드(없으면 8 = 23.85 kbit/s). */
    int amrMaxMode() const;
};

/**
 * 피어 leg 트랜스코더 — 한 방향(src leg 코덱 → dst leg 코덱)의 오디오 RTP 를 프레임 단위로 변환한다 (cmp.md §11.3).
 *
 *   decode(in) → resample(8k↔16k, 47-tap 하프밴드 FIR) → encode(out). 20 ms 프레임: AMR-WB 320 샘플/16 kHz, G.711 160 바이트/8 kHz.
 *   AMR-WB 페이로드는 RFC 4867 octet-aligned·bandwidth-efficient 둘 다 수신(fmtp 없으면 자동 판정), 송신은 dst fmtp 를 따른다.
 *   출력 RTP 는 새 헤더(V2, dst PT, 자기 seq, 입력 clock 을 출력 clock 으로 비례 사상한 timestamp, 입력 SSRC·marker 보존)를 쓴다 —
 *   입력 1 패킷이 출력 0..N 패킷이 될 수 있다(G.711 10 ms 패킷 2개 → AMR-WB 1 프레임, AMR-WB 다중 프레임 → G.711 여러 패킷).
 *   telephone-event 는 변환하지 않는다 — 호출자가 rescaleTimestamp 로 timestamp 만 재작성하고 PT 를 스탬프한다.
 *   순서 뒤집힘·손실 은닉은 하지 않는다(디코더 PLC 에 맡긴다). 스레드 규약: relay 리액터 안에서만 부른다.
 */
class PTranscoder {
public:
    static bool supportedPair(const PCodecDesc& in, const PCodecDesc& out);

    PTranscoder(const PCodecDesc& in, const PCodecDesc& out);
    ~PTranscoder();
    PTranscoder(const PTranscoder&) = delete;
    PTranscoder& operator=(const PTranscoder&) = delete;

    bool ok() const { return _ok; }
    const PCodecDesc& in() const { return _in; }
    const PCodecDesc& out() const { return _out; }

    /** 오디오 RTP(평문, 12+ 바이트 헤더 포함) → 변환된 RTP 패킷들. 반환 false = RTP/페이로드 형식 오류(호출자가 폐기). */
    bool transcode(const unsigned char* pkt, int len, std::vector<std::string>& out);
    /** 비변환 패킷(telephone-event)의 timestamp 를 출력 clock 으로 재작성(in-place). */
    void rescaleTimestamp(unsigned char* pkt, int len);

    long framesIn() const { return _framesIn; }
    long framesOut() const { return _framesOut; }
    long dropped() const { return _dropped; }

    // ── 순수 함수(단위시험·다른 모듈 공용) ──
    /** RFC 4867 AMR-WB 페이로드 파싱 → 프레임(저장 형식 헤더 1B + 데이터). octetAlign<0 = 자동 판정. */
    struct AmrFrame { int ft = 15; int q = 1; unsigned char data[64]; int bytes = 0; };
    static bool parseAmrWb(const unsigned char* payload, int len, int octetAlign, std::vector<AmrFrame>& frames);
    /** 프레임 1개 → RFC 4867 페이로드(CMR 15 = 요청 없음). */
    static std::string buildAmrWb(const AmrFrame& f, bool octetAlign);
    static int amrWbBytes(int ft);
    static int amrWbBits(int ft);

private:
    struct RtpHdr { bool marker; int pt; uint16_t seq; uint32_t ts; uint32_t ssrc; const unsigned char* payload; int len; };
    static bool _parseRtp(const unsigned char* pkt, int len, RtpHdr& h);
    uint32_t _mapTs(uint32_t inTs);
    std::string _makeRtp(bool marker, uint32_t ts, const unsigned char* payload, int len);
    void _decodeToPcm16k(const AmrFrame& f, int16_t* pcm320);
    void _g711ToPcm(const unsigned char* in, int n, int16_t* out) const;
    void _pcmToG711(const int16_t* in, int n, unsigned char* out) const;
    void _upsample(const int16_t* in8k, int n, std::vector<int16_t>& out16k);
    void _downsample(const int16_t* in16k, int n, std::vector<int16_t>& out8k);

    PCodecDesc _in, _out;
    bool _ok = false;
    void* _dec = nullptr;   // opencore-amrwb (in = AMR-WB)
    void* _enc = nullptr;   // vo-amrwbenc (out = AMR-WB)
    int _inOctetAlign = -1;  // -1 자동
    bool _outOctetAlign = true;
    int _encMode = 8;
    // timestamp 사상 (입력 clock → 출력 clock, 첫 패킷 기준 비례)
    bool _tsInit = false;
    uint32_t _baseIn = 0, _baseOut = 0;
    bool _seqInit = false;
    uint16_t _seq = 0;
    // G.711 → AMR-WB: 16 kHz PCM 누적 버퍼(320 샘플마다 1 프레임)와 그 첫 샘플의 출력 timestamp
    std::vector<int16_t> _pcmBuf;
    uint32_t _bufTs = 0;
    bool _pendingMarker = false;
    // 하프밴드 FIR 상태
    std::vector<float> _fir;
    std::vector<int16_t> _histUp, _histDown;
    long _framesIn = 0, _framesOut = 0, _dropped = 0;
};

#endif  // __PTRANSCODER_H__
