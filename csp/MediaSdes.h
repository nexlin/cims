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

#include "SdpMedia.h"

/** CMP media_crypto payload 파라미터 — base64 필드 그대로 UDP JSON 에 실린다.
 *  rx = UE→CMP 상향(UE 의 a=crypto 선언 키), tx = CMP→UE 하향(CSP 생성 키). */
struct CmpMediaCrypto {
    bool bEnabled = false;
    std::string strAlg;               // crypto suite (AES_CM_128_HMAC_SHA1_80|_32)
    std::string strRxKey, strRxSalt;  // base64(16B) / base64(14B)
    std::string strTxKey, strTxSalt;
};

/** VoLTE relay(B2BUA) leg 한 미디어(m= 라인)의 SDES 협상 상태 — CallMap 에 저장 (§5.2).
 *  relay 는 crypto 를 leg 별로 종단하므로 leg×미디어마다 UE 키/서버 키 쌍을 기억한다. */
struct RelaySdesMedia {
    bool bSrtp = false;
    std::string strTag;     // 그 leg offer 의 crypto tag (answer echo, 비면 "1")
    std::string strSuite;   // 채택 suite
    std::string strProto;   // 그 leg offer 의 m= protocol 원문 (answer protocol echo)
    std::string strUeKey;   // UE 선언 inline b64 (CMP rx)
    std::string strSrvKey;  // 서버 생성 inline b64 (CMP tx — SDP 로 광고)
};

/** relay 한 leg 의 SDES 상태 (오디오 + 비디오 — SDES 는 m= 라인마다 키가 다르다) */
struct RelaySdesLeg {
    RelaySdesMedia clsAudio;
    RelaySdesMedia clsVideo;
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

    // ── VoLTE relay(media-list passthrough) 의 SDP 리스트 조작 (§5.2) ──

    /** pszMedia 의 첫 active(port>0) m= 라인에서 crypto offer 를 읽는다.
     *  반환: 1=유효 crypto(tag/suite/inline 채움) / 0=crypto 없음·미디어 비활성·AVP 에 지원 불가
     *  crypto 병기(무시) / -1=성립 불가(RTP/SAVP 인데 지원 가능한 유효 crypto 없음 — RFC 4568 상
     *  평문 폴백 없음). strProto 는 항상 그 m= 라인의 protocol 원문(미디어 비활성이면 빈 값). */
    int ReadOfferCrypto( const SDP_MEDIA_LIST &clsList, const char *pszMedia, std::string &strTag,
                         std::string &strSuite, std::string &strInline, std::string &strProto );

    /** 모든 m= 라인의 a=crypto 제거 — 구간 종단(수신 leg 키가 반대 leg 로 투과되면 단말끼리
     *  E2E SRTP 를 협상해 CMP 종단·녹취가 깨진다). */
    void StripCrypto( SDP_MEDIA_LIST &clsList );

    /** pszMedia 의 첫 active m= 라인에 protocol 설정 + (strInline 비어있지 않으면) a=crypto 부여.
     *  미디어 부재/비활성이면 무시. strProto 빈 값 = protocol 유지. */
    void ApplyCrypto( SDP_MEDIA_LIST &clsList, const char *pszMedia, const std::string &strProto,
                      const std::string &strTag, const std::string &strSuite, const std::string &strInline );

}  // namespace MediaSdes

#endif
