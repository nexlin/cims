#ifndef __PFLOOR_CRYPTO_H__
#define __PFLOOR_CRYPTO_H__

#include <string>
#include <map>
#include <mutex>
#include <cstdint>

/**
 * Floor control(RTCP APP "MCPT") 보호 — SRTCP (RFC 3711) / 3GPP TS 33.180.
 *
 * MCPTT 의 E2E 보안에서 **미디어 RTP 는 CMP 가 복호하지 않는다**(UE↔UE SRTP 를 투명
 * relay). 반면 floor control 은 CMP 가 중재자로 참여하므로 floor RTCP 만 보호 키를
 * 받아 복호·재암호한다. 키는 CSC(KMS)가 GMK/PCK 에서 파생해 CSP 를 거쳐 제어평면
 * (`PTT_GROUP_ADD.floor_crypto`)으로 inline 전달된다 (cmp_media_api.md §7.4).
 *
 * 지원 프로파일:
 *   AES_CM_128_HMAC_SHA1_80 (기본) / AES_CM_128_HMAC_SHA1_32
 *   master key 16B, master salt 14B, KDR=0 (세션 키 1회 파생).
 *
 * SRTCP 패킷 구성 (RFC 3711 §3.4):
 *   [RTCP 헤더 8B][암호화된 본문][E(1)+SRTCP index(31)][MKI(선택)][auth tag]
 *   - 암호화 범위 = 헤더 8B 이후 전체, 인증 범위 = (헤더+본문+E/index), MKI·tag 는 제외.
 *   - 재전송 방지: SSRC 별 index 최고값 + 64 슬롯 비트맵 윈도우.
 */
class PFloorCrypto {
public:
    // 보호 부가 길이 상한 (E+index 4 + MKI + tag) — 송신 버퍼 여유 계산용
    static const int kMaxMki = 16;
    static const int kMaxOverhead = 4 + kMaxMki + 10;

    /** 제어평면 파라미터로 초기화. key/salt 는 **디코드된 바이트열**(16B/14B),
     *  mki 는 디코드된 바이트열(선택). 실패 시 err 에 사유를 담고 false. */
    bool init(const std::string& alg, const std::string& key, const std::string& salt,
              const std::string& mki, std::string& err);
    bool enabled() const { return _enabled; }
    const std::string& alg() const { return _alg; }

    /** 평문 RTCP → SRTCP. out 은 inLen + kMaxOverhead 이상이어야 한다. */
    bool protect(const char* in, int inLen, char* out, int outCap, int& outLen);
    /** SRTCP → 평문 RTCP (인증·복호·재전송 검사). 위조/재전송이면 false. */
    bool unprotect(const char* in, int inLen, char* out, int outCap, int& outLen);

    /** 제어평면 인코딩 디코더 (key/salt=base64, mki=hex). 실패 시 false. */
    static bool DecodeBase64(const std::string& s, std::string& out);
    static bool DecodeHex(const std::string& s, std::string& out);

private:
    // RFC 3711 §4.3.1 KDF — master(key,salt) + label → 세션 키/salt
    bool _deriveSessionKeys(std::string& err);

    bool _enabled = false;
    std::string _alg;
    std::string _masterKey;    // 16B
    std::string _masterSalt;   // 14B
    std::string _mki;          // 디코드된 MKI (빈 값 = 미사용)
    int _tagLen = 10;          // HMAC-SHA1 truncation (80bit=10B / 32bit=4B)

    unsigned char _sessKey[16] = {0};    // SRTCP 암호화 키 (label 0x03)
    unsigned char _sessAuth[20] = {0};   // SRTCP 인증 키   (label 0x04)
    unsigned char _sessSalt[14] = {0};   // SRTCP salt      (label 0x05)

    struct ReplayCtx {
        uint32_t highest = 0;
        uint64_t bitmap = 0;   // highest 기준 과거 64 index 수신 이력
        bool seen = false;
    };
    std::map<uint32_t, uint32_t> _txIndex;     // SSRC → 다음 송신 SRTCP index
    std::map<uint32_t, ReplayCtx> _rxReplay;   // SSRC → 재전송 방지 윈도우
    std::mutex _mtx;                           // 송수신 경로가 서로 다른 스레드다
};

#endif // __PFLOOR_CRYPTO_H__
