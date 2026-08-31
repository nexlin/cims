#ifndef __PMEDIA_CRYPTO_H__
#define __PMEDIA_CRYPTO_H__

#include <string>

// libsrtp 불투명 핸들 전방선언 (srtp2/srtp.h 는 .cpp 에서만 포함)
struct srtp_ctx_t_;

/**
 * 미디어 SRTP leg 컨텍스트 — RFC 3711, TS 33.328 e2ae (media_security.md §6).
 *
 * CMP 는 leg 별로 SRTP 를 종단한다: ingress unprotect → 평문(믹스·디먹스·녹취·DTMF)
 * → egress protect. 엔진은 ext/libsrtp(2.5.x, OpenSSL EVP 백엔드) — floor SRTCP 의
 * 자체 구현(PFloorCrypto, TS 33.180)과는 별개 축으로 공존한다.
 *
 * 컨텍스트 = srtp_t 2개:
 *   _rx (ssrc_any_inbound)  — UE→CMP 상향. 키 = UE 가 SDP a=crypto 로 선언한 값.
 *   _tx (ssrc_any_outbound) — CMP→UE 하향. 키 = CSP 가 leg 마다 생성한 값.
 * 하향 슬롯 SSRC(0x10000000+/0x40000000+…) 는 any_outbound 템플릿이 SSRC 별
 * 스트림(ROC·재전송 창)을 자동 생성한다 — RFC 3711 세션 키는 SSRC 무관.
 *
 * ⚠️ 스레드 규약: srtp_t 는 thread-safe 가 아니다. CMP 의 현 구조가 직렬화를
 * 보장한다 — relay 는 핸들러의 전 fd 가 같은 리액터에 배정되고(epollAddHandler
 * 핸들러 단위 widx), 그룹 경로는 분배까지 그룹 _mutex 아래다. 이 컨텍스트는
 * 그 직렬화 범위 안에서만 접근한다 — 향후 락 분리/리액터 재배정 시 이 전제를
 * 깨면 안 된다.
 *
 * ⚠️ 재협상 키 교체 = 세션 재생성(ROC·재전송 창 리셋). srtp_update() 는 ROC 를
 * 유지하는 반면 단말(pjmedia)은 키 변경 시 세션을 재생성(ROC 0)하므로, update
 * 를 쓰면 재협상 직후 전 패킷이 인증 실패한다 — 금지. 동일 구성 재선언
 * (refresh/재전송)은 세션을 유지한다(키 유지 규칙, media_security.md §5.2).
 */
class PMediaCrypto {
public:
    // protect 부가 길이 상한 (auth tag ≤16, MKI 미사용) — 송신 버퍼 여유 계산용
    static const int kMaxOverhead = 16;

    PMediaCrypto() = default;
    ~PMediaCrypto();
    PMediaCrypto(const PMediaCrypto&) = delete;
    PMediaCrypto& operator=(const PMediaCrypto&) = delete;

    /** 초기화/재키잉. key/salt 는 **디코드된 바이트열**(16B/14B).
     *  동일 구성(alg·키 전부 불변) 재선언이면 세션 유지(no-op true). 구성이 바뀌면
     *  세션 재생성. 실패 시 err 에 사유를 담고 false(기존 세션은 clear 됨 — 키
     *  오류 leg 를 평문으로 조용히 폴백하지 않도록 호출자가 명령을 거부한다). */
    bool init(const std::string& alg,
              const std::string& rxKey, const std::string& rxSalt,
              const std::string& txKey, const std::string& txSalt,
              std::string& err);
    void clear();
    bool enabled() const { return _rx != nullptr; }
    const std::string& alg() const { return _alg; }

    // 전부 in-place 변환 — 성공 시 len 갱신. protect 는 cap ≥ len + kMaxOverhead 필요.
    // unprotect 실패(인증 태그 불일치·재전송 창 밖)는 false — 호출자가 드롭+카운터.
    bool protectRtp(char* buf, int& len, int cap);
    bool unprotectRtp(char* buf, int& len);
    bool protectRtcp(char* buf, int& len, int cap);
    bool unprotectRtcp(char* buf, int& len);

    /** 지원 suite 검증 ("AES_CM_128_HMAC_SHA1_80"|"AES_CM_128_HMAC_SHA1_32") */
    static bool IsSupportedAlg(const std::string& alg);

private:
    bool _alloc(const std::string& err_who, bool inbound, const std::string& key,
                const std::string& salt, srtp_ctx_t_** out, std::string& err);

    std::string _alg;
    std::string _rxKey, _rxSalt, _txKey, _txSalt;   // 동일 구성 재선언 판정용
    srtp_ctx_t_* _rx = nullptr;   // ssrc_any_inbound  (상향 unprotect)
    srtp_ctx_t_* _tx = nullptr;   // ssrc_any_outbound (하향 protect)
};

#endif // __PMEDIA_CRYPTO_H__
