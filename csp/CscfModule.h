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
};

// 미가입 계정 무차별 대입 소스의 NETWORK 덤프 억제 시간 (toll-fraud INVITE 603 경로와 동일 값)
#define SIP_SCAN_SUPPRESS_TTL_SEC 300

/** 서버 지원 메서드 목록 — REGISTER 401/200·OPTIONS 200 등의 Allow 헤더 값 (실망 패킷 형태) */
#define SIP_ALLOW_METHODS "REGISTER,INVITE,ACK,BYE,CANCEL,REFER,OPTIONS,NOTIFY,SUBSCRIBE,MESSAGE,INFO,PRACK,UPDATE,PUBLISH"

/**
 * @brief CSCF Module — REGISTER, SUBSCRIBE 처리, 인증
 */
class CCscfModule : public IModule {
public:
    const char *GetName() const override {
        return "CSCF";
    }
    bool IsEnabled() const override;

    bool OnSipRequest( int iThreadId, CSipMessage *pclsMessage ) override;

    // 인증 헬퍼 (다른 모듈에서도 사용 가능)
    //   strRealmOverride: 서비스 엔티티에서 계산된 realm 전달. 비면 전역 AuthRealm 사용.
    static bool AddChallenge( CSipMessage *psttResponse, const std::string &strRealmOverride = "", bool bStale = false );
    static bool SendUnAuthorizedResponse( CSipMessage *pclsMessage, const std::string &strRealmOverride = "", bool bStale = false );
    static bool CheckAuthorizationResponse( const char *pszHa1, const char *pszNonce, const char *pszUri,
                                            const char *pszResponse, const char *pszMethod, const char *pszQop,
                                            const char *pszNc, const char *pszCnonce );
    static ECheckAuthResult CheckAuthorization( CSipCredential *pclsCredential, const char *pszFromId,
                                                const char *pszMethod, CspUser &clsXmlUser );
    static std::string EffectiveHa1( const CspUser &clsUser, const std::string &strImpi, const std::string &strRealm );

    // REGISTER 인증 체크 (EventIncomingRequestAuth에서 사용)
    static bool CheckAuthrization( CSipMessage *pclsMessage );
    static bool CheckChannelPolicy( CSipMessage *pclsMessage );

private:
    bool RecvRequestRegister( int iThreadId, CSipMessage *pclsMessage );
    bool RecvRequestSubscribe( int iThreadId, CSipMessage *pclsMessage );
    bool RecvRequestPublish( int iThreadId, CSipMessage *pclsMessage );

    bool SendResponse( CSipMessage *pclsMessage, int iStatusCode );
};

#endif
