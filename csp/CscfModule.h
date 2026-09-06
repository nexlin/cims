#ifndef _CSCF_MODULE_H_
#define _CSCF_MODULE_H_

#include "CspUser.h"
#include "IModule.h"
#include "SipCredential.h"

/**
 * @brief SIP 인증 결과
 */
enum ECheckAuthResult {
    E_AUTH_OK = 0,
    E_AUTH_NONCE_NOT_FOUND,
    E_AUTH_ERROR,           // 가입자 존재 — 자격 증명/정책 불일치 (실단말 가능성, 로그 유지)
    E_AUTH_USER_NOT_FOUND,  // 미가입 계정 — 계정 무차별 대입 스캐너 신호 (소스 로그 억제 대상)
    E_AUTH_REALM_MISMATCH,  // Authorization.realm ≠ 서비스 realm — 서버 realm 로 401 재챌린지 (stale 아님)
    E_AUTH_AKA_RESYNC,      // IMS AKA: 단말이 auts 를 보냈다 — CSC 재동기 후 새 챌린지 (RFC 3310 §3.4)
};

// 미가입 계정 무차별 대입 소스의 NETWORK 덤프 억제 시간 (toll-fraud INVITE 603 경로와 동일 값)
#define SIP_SCAN_SUPPRESS_TTL_SEC 300

/** 서버 지원 메서드 목록 — REGISTER 401/200·OPTIONS 200 등의 Allow 헤더 값 (실망 패킷 형태) */
#define SIP_ALLOW_METHODS \
    "REGISTER,INVITE,ACK,BYE,CANCEL,REFER,OPTIONS,NOTIFY,SUBSCRIBE,MESSAGE,INFO,PRACK,UPDATE,PUBLISH"

/**
 * @brief CSCF Module — REGISTER, SUBSCRIBE 처리, 인증
 */
struct SecAgreeIpsecOffer;  // SecAgree.h

class CCscfModule : public IModule {
public:
    const char *GetName() const override {
        return "CSCF";
    }
    bool IsEnabled() const override;

    bool OnSipRequest( int iThreadId, CSipMessage *pclsMessage ) override;

    // 인증 헬퍼 (다른 모듈에서도 사용 가능)
    //   strRealmOverride: 서비스 엔티티에서 계산된 realm 전달. 비면 전역 AuthRealm 사용.
    static bool AddChallenge( CSipMessage *psttResponse, const std::string &strRealmOverride = "",
                              bool bStale = false );
    //   pszSecurityServer: RFC 3329 협상 중이면 401 에 실을 Security-Server 목록 (NULL = 미동봉).
    static bool SendUnAuthorizedResponse( CSipMessage *pclsMessage, const std::string &strRealmOverride = "",
                                          bool bStale = false, const char *pszSecurityServer = NULL );
    static bool CheckAuthorizationResponse( const char *pszHa1, const char *pszNonce, const char *pszUri,
                                            const char *pszResponse, const char *pszMethod, const char *pszQop,
                                            const char *pszNc, const char *pszCnonce );
    //   pstrResyncRand/pstrResyncAuts: E_AUTH_AKA_RESYNC 일 때 직전 챌린지 RAND(hex) 와 단말 AUTS(hex) 를 돌려준다.
    static ECheckAuthResult CheckAuthorization( CSipCredential *pclsCredential, const char *pszFromId,
                                                const char *pszMethod, CspUser &clsXmlUser,
                                                std::string *pstrResyncRand = NULL,
                                                std::string *pstrResyncAuts = NULL );
    static std::string EffectiveHa1( const CspUser &clsUser, const std::string &strImpi, const std::string &strRealm );

    // IMS AKA (sip_access_security.md §8.2, RFC 3310 / TS 33.203 Annex X)
    //   AV 를 CSC 에서 받아 401 (nonce=base64(RAND‖AUTN), algorithm=AKAv1-MD5) 을 보낸다.
    //   strRandPrevHex/strAutsHex 가 있으면 재동기(AUTS) 요청이다. CSC 미도달은 504, AUTS 불일치·미가입은 403.
    //   pclsIpsec: 단말의 ipsec-3gpp 제안(받아들인 것) — AV 의 CK/IK 로 임시 SA 셋을 설치하고 그 파라미터를 실은
    //   Security-Server 를 401 에 동봉한다 (sip_access_security.md §8.3). NULL 이면 pszSecurityServer 그대로.
    static bool SendAkaChallenge( CSipMessage *pclsMessage, const CspUser &clsUser, const std::string &strRealm,
                                  bool bStale = false, const char *pszSecurityServer = NULL,
                                  const std::string &strRandPrevHex = "", const std::string &strAutsHex = "",
                                  const SecAgreeIpsecOffer *pclsIpsec = NULL );
    /** From 가입자의 체계에 맞는 REGISTER 챌린지 — aka 면 SendAkaChallenge, 아니면 Digest 401. */
    static bool SendRegisterChallenge( CSipMessage *pclsMessage, const std::string &strRealm, bool bStale,
                                       const char *pszSecurityServer, const SecAgreeIpsecOffer *pclsIpsec = NULL );

    // REGISTER 인증 체크 (EventIncomingRequestAuth에서 사용)
    static bool CheckAuthrization( CSipMessage *pclsMessage );
    static bool CheckChannelPolicy( CSipMessage *pclsMessage );

private:
    bool RecvRequestRegister( int iThreadId, CSipMessage *pclsMessage );
    bool RecvRequestSubscribe( int iThreadId, CSipMessage *pclsMessage );
    bool RecvRequestPublish( int iThreadId, CSipMessage *pclsMessage );

    bool SendResponse( CSipMessage *pclsMessage, int iStatusCode );
    static bool SendResponseStatic( CSipMessage *pclsMessage, int iStatusCode );
    /** 상태 응답 + Warning 헤더(RFC 3261 §20.43 — TS 24.379 §4.4 warn-code 텍스트) */
    static bool SendResponseWithWarning( CSipMessage *pclsMessage, int iStatusCode, const char *pszWarning );
    /** RFC 3329 협상 거절 — 494(대조 실패/제안 없음) 또는 421(정책상 협상 필수) + 새 Security-Server. */
    bool SendSecAgreeReject( CSipMessage *pclsMessage, int iStatusCode, const std::string &strUser,
                             const char *pszReason );
};

#endif
