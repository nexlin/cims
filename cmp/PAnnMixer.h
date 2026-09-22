#ifndef __PANNMIXER_H__
#define __PANNMIXER_H__

#include <cstdint>
#include <string>
#include <vector>

class PAnnPlayer;

/**
 * 안내 믹서(mode=mix — announcements.md §3.6, cmp_media_api.md §6.7) — 재생기(PAnnPlayer)의 신호음을 **상대 leg 에서 relay 되는
 * 오디오 위에 섞어** 그 leg 로 낸다(통화중대기 in-band 대기음, TS 24.615). replace 모드처럼 relay 를 끊지 않는다.
 *
 *   relay 패킷(dst 코덱, 트랜스코딩 뒤) → 디코드 → 재생기 프레임(같은 코덱 저장 형식)을 pull 해 디코드한 PCM 을 샘플 수만큼 더하고(포화)
 *   → 다시 인코드 → 원 RTP 헤더(seq·ts·SSRC·marker) 그대로 페이로드만 교체. 코덱 = PCMU/PCMA(표 변환) · AMR-WB(opencore 디코더 2개 +
 *   vo 인코더 1개 — 트랜스코딩과 같은 급이라 변환 슬롯을 센다). G.722 는 지원하지 않는다(BAD_REQUEST). 다중 프레임 AMR-WB 패킷은 섞지
 *   않고 그대로 낸다. 상대 leg 가 조용하면(DTX·보류) 대기음도 멈춘다 — relay 위에 얹는 것이 이 모드의 정의다.
 *   스레드 규약: relay 리액터 안(호출자가 relay _mutex 보유).
 */
class PAnnMixer {
public:
    static bool supports(const std::string& codec);

    PAnnMixer(const std::string& codec, bool amrOctetAlign, int amrEncMode);
    ~PAnnMixer();
    PAnnMixer(const PAnnMixer&) = delete;
    PAnnMixer& operator=(const PAnnMixer&) = delete;

    bool ok() const { return _ok; }
    bool isAmrWb() const { return _amr; }

    /** RTP 패킷(평문, 12+ 헤더) 에 tone 을 섞은 패킷을 out 에 만든다. 반환 false = 섞지 않았다(형식 오류·다중 프레임·tone 없음 — 원본 그대로 보낸다) */
    bool mix(const unsigned char* pkt, int len, PAnnPlayer& tone, std::string& out);

    long mixed() const { return _mixed; }

    // ── 순수 함수(단위시험) ──
    static int16_t ulaw2lin(unsigned char u);
    static int16_t alaw2lin(unsigned char a);
    static unsigned char lin2ulaw(int16_t s);
    static unsigned char lin2alaw(int16_t s);
    static inline int16_t sat16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

private:
    /** tone PCM 을 n 샘플 이상 채운다(재생기에서 프레임을 pull 해 디코드). 재생기가 끝났으면 false */
    bool _fillTone(PAnnPlayer& tone, size_t n);
    void _decodeAmr(void* dec, const unsigned char* storage, int storageLen, int16_t* pcm320);

    std::string _codec;
    bool _amr = false;
    bool _alaw = false;
    bool _octetAlign = true;
    int _encMode = 8;
    bool _ok = false;
    void* _decStream = nullptr;   // 상대 leg 오디오 디코더(AMR-WB)
    void* _decTone = nullptr;     // 신호음 디코더(AMR-WB)
    void* _enc = nullptr;
    std::vector<int16_t> _toneBuf;   // leg 클록 PCM 대기열
    long _mixed = 0;
};

#endif  // __PANNMIXER_H__
