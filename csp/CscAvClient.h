/*
 * CscAvClient — CSC(HSS/AuC 역할) 내부 AV API 클라이언트 (sip_access_security.md §8.2, Cx MAR/MAA 상당)
 *
 *   POST {Setup.Csc.Scheme}://{Host}:{Port}/internal/aka/av   Authorization: Bearer {Setup.Csc.InternalToken}
 *   {"msisdn","service","rand","auts"} → {"av":{"rand","autn","xres","ck","ik"},"resynced"}
 * IMS AKA 가입자의 REGISTER 챌린지마다 동기 호출된다(SIP 수신 스레드, 타임아웃 Setup.Csc.TimeoutMs).
 * K/OPc 는 CSC 밖으로 나오지 않는다 — 이 클라이언트가 받는 것은 AV 뿐이다.
 */
#ifndef _CSC_AV_CLIENT_H_
#define _CSC_AV_CLIENT_H_

#include <string>

struct CscAv {
    std::string strRandHex;  // 16B
    std::string strAutnHex;  // 16B = (SQN⊕AK)‖AMF‖MAC-A
    std::string strXresHex;  // 8B
    std::string strCkHex;    // 16B (Annex X — TLS 위에서는 쓰지 않는다)
    std::string strIkHex;    // 16B
    bool bResynced = false;
};

enum ECscAvResult {
    E_CSC_AV_OK = 0,
    E_CSC_AV_UNKNOWN_SUB,      // 404 — CSC 에 그 가입자가 없다
    E_CSC_AV_SCHEME_MISMATCH,  // 409 — CSC 는 digest 로 알고 있다(캐시 불일치) / 키 미보관
    E_CSC_AV_AUTS_INVALID,     // 422 — 재동기 AUTS MAC-S 불일치
    E_CSC_AV_UNAVAILABLE,      // 5xx / 미설정 / 타임아웃 / 파싱 실패 → REGISTER 504
};

class CCscAvClient {
public:
    /** AV 1개 요청. strRandHex/strAutsHex 는 재동기(AUTS) 때만 (직전 챌린지 RAND + 단말 AUTS). */
    ECscAvResult Request( const std::string &strMsisdn, const std::string &strService, const std::string &strRandHex,
                          const std::string &strAutsHex, CscAv &clsOut );
    /** 요청 URL — CSC admin base URL(Csc.Host → LocalIp fallback) + /internal/aka/av */
    static std::string Url();
};

extern CCscAvClient gclsCscAvClient;

#endif
