/*
 * MediaSdes — 미디어 SRTP(SDES, RFC 4568) 키 생성·변환 헬퍼 (media_security.md §3·§5)
 *
 * SDES 인라인 키 = base64(master key 16B || master salt 14B). CSP 가 leg 마다 서버측
 * 키를 생성해 SDP a=crypto 로 광고하고, UE 선언 키와 함께 CMP 제어 명령의 media_crypto
 * {alg, rx{key,salt}, tx{key,salt}} 로 내린다 (cmp_media_api.md).
 */
#ifndef _MEDIA_SDES_H_
#define _MEDIA_SDES_H_

#include <string>

/** CMP media_crypto payload 파라미터 — base64 필드 그대로 UDP JSON 에 실린다.
 *  rx = UE→CMP 상향(UE 의 a=crypto 선언 키), tx = CMP→UE 하향(CSP 생성 키). */
struct CmpMediaCrypto {
    bool bEnabled = false;
    std::string strAlg;               // crypto suite (AES_CM_128_HMAC_SHA1_80|_32)
    std::string strRxKey, strRxSalt;  // base64(16B) / base64(14B)
    std::string strTxKey, strTxSalt;
};

namespace MediaSdes {

    /** 지원 suite 인가 (§2 — _80 기본 제안, _32 수용) */
    bool IsSupportedSuite( const std::string &strSuite );

    /** 서버측 leg 키 생성 — 30B random → base64(key||salt) 인라인 키. 실패 시 빈 문자열. */
    std::string GenerateInlineKeyB64();

    /** 인라인 키(base64, 30B) 검증 — 디코드 길이가 정확히 30B 인가 */
    bool ValidInlineKeyB64( const std::string &strInlineB64 );

    /** 협상 결과 → CMP media_crypto 파라미터. ueInline=UE a=crypto 키(rx), srvInline=서버
     *  생성 키(tx). 어느 한쪽이라도 형식 위반이면 false (조용한 평문 폴백 금지 — 호출자가
     *  협상 실패 처리). */
    bool BuildCmpKeys( const std::string &strSuite, const std::string &strUeInlineB64,
                       const std::string &strSrvInlineB64, CmpMediaCrypto &clsOut );

}  // namespace MediaSdes

#endif
