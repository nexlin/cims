#ifndef _CSCF_MODULE_H_
#define _CSCF_MODULE_H_

#include "IModule.h"
#include "CspUser.h"
#include "SipCredential.h"

/**
 * @brief SIP 인증 결과
 */
enum ECheckAuthResult {
    E_AUTH_OK = 0,
    E_AUTH_NONCE_NOT_FOUND,
    E_AUTH_ERROR
};

/**
 * @brief CSCF Module — REGISTER, SUBSCRIBE 처리, 인증
 */
class CCscfModule : public IModule {
public:
    const char* GetName() const override { return "CSCF"; }
    bool IsEnabled() const override;

    bool OnSipRequest(int iThreadId, CSipMessage* pclsMessage) override;

    // 인증 헬퍼 (다른 모듈에서도 사용 가능)
    //   strRealmOverride: 서비스 엔티티에서 계산된 realm 전달. 비면 전역 AuthRealm 사용.
    static bool AddChallenge(CSipMessage* psttResponse, const std::string& strRealmOverride = "");
    static bool SendUnAuthorizedResponse(CSipMessage* pclsMessage,
                                          const std::string& strRealmOverride = "");
    static bool CheckAuthorizationResponse(const char* pszUserName, const char* pszRealm,
        const char* pszNonce, const char* pszUri, const char* pszResponse,
        const char* pszPassWord, const char* pszMethod,
        const char* pszQop, const char* pszNc, const char* pszCnonce);
    static ECheckAuthResult CheckAuthorization(CSipCredential* pclsCredential,
        const char* pszFromId, const char* pszMethod, CspUser& clsXmlUser);

    // REGISTER 인증 체크 (EventIncomingRequestAuth에서 사용)
    static bool CheckAuthrization(CSipMessage* pclsMessage);

private:
    bool RecvRequestRegister(int iThreadId, CSipMessage* pclsMessage);
    bool RecvRequestSubscribe(int iThreadId, CSipMessage* pclsMessage);

    bool SendResponse(CSipMessage* pclsMessage, int iStatusCode);
};

#endif
