/**
 * CCscfModule — CSCF 모듈: REGISTER, SUBSCRIBE, 인증 처리
 *
 * SipServerRegister.hpp 와 SipServer.cpp 의 SUBSCRIBE 처리 로직을 이 모듈로 이동.
 */

#include "CscfModule.h"
#include "CspServiceMap.h"
#include "SipServerSetup.h"
#include "SipServer.h"
#include "SipMd5.h"
#include "NonceMap.h"
#include "UserMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupCallService.h"
#include "GroupMap.h"
#include "CspPttGroup.h"
#include "SubscriptionManager.h"
#include "SipUtility.h"
#include "Log.h"

extern CSipUserAgent gclsUserAgent;
extern void SendInitialNotify(const SubscriptionInfo& sub);

bool CCscfModule::IsEnabled() const {
    return gclsSetup.m_bRoleCscf;
}

// ──────────────────────────────────────────────────────────────
//  인증 헬퍼 (static)
// ──────────────────────────────────────────────────────────────

bool CCscfModule::AddChallenge(CSipMessage* psttResponse, const std::string& strRealmOverride) {
    CSipChallenge clsChallenge;
    char szNonce[33];

    if (gclsNonceMap.GetNewValue(szNonce, sizeof(szNonce)) == false) {
        CLog::Print(LOG_ERROR, "gclsNonce.GetNewValue() error");
        return false;
    }

    clsChallenge.m_strType = "Digest";
    clsChallenge.m_strAlgorithm = "MD5";
    clsChallenge.m_strNonce = szNonce;
    clsChallenge.m_strRealm = strRealmOverride.empty() ? gclsSetup.m_strAuthRealm : strRealmOverride;
    clsChallenge.m_strQop = "auth";

    psttResponse->m_clsWwwAuthenticateList.push_back(clsChallenge);
    return true;
}

bool CCscfModule::SendUnAuthorizedResponse(CSipMessage* pclsMessage,
                                             const std::string& strRealmOverride) {
    CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(SIP_UNAUTHORIZED);
    if (pclsResponse == NULL) return false;

    AddChallenge(pclsResponse, strRealmOverride);
    gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
    return true;
}

bool CCscfModule::CheckAuthorizationResponse(const char* pszUserName,
    const char* pszRealm, const char* pszNonce, const char* pszUri,
    const char* pszResponse, const char* pszPassWord, const char* pszMethod,
    const char* pszQop, const char* pszNc, const char* pszCnonce)
{
    char szA1[301], szA2[201], szMd5[33], szResponse[1024];

    snprintf(szA1, sizeof(szA1), "%s:%s:%s", pszUserName, pszRealm, pszPassWord);
    SipMd5String(szA1, szMd5);
    snprintf(szA1, sizeof(szA1), "%s", szMd5);

    snprintf(szA2, sizeof(szA2), "%s:%s", pszMethod, pszUri);
    SipMd5String(szA2, szMd5);
    snprintf(szA2, sizeof(szA2), "%s", szMd5);

    if (pszQop && strcasecmp(pszQop, "auth") == 0) {
        snprintf(szResponse, sizeof(szResponse), "%s:%s:%s:%s:%s:%s",
            szA1, pszNonce, pszNc ? pszNc : "", pszCnonce ? pszCnonce : "", pszQop, szA2);
    } else {
        snprintf(szResponse, sizeof(szResponse), "%s:%s:%s", szA1, pszNonce, szA2);
    }

    SipMd5String(szResponse, szMd5);
    snprintf(szResponse, sizeof(szResponse), "%s", szMd5);

    if (strcmp(szResponse, pszResponse)) {
        CLog::Print(LOG_ERROR, "response[%s] is not correct. correct response is [%s]", pszResponse, szResponse);
        return false;
    }
    return true;
}

ECheckAuthResult CCscfModule::CheckAuthorization(CSipCredential* pclsCredential,
    const char* pszFromId, const char* pszMethod, CspUser& clsXmlUser)
{
    if (pclsCredential->m_strUserName.empty()) return E_AUTH_ERROR;
    if (gclsNonceMap.Select(pclsCredential->m_strNonce.c_str()) == false) return E_AUTH_NONCE_NOT_FOUND;
    if (gclsCspUserMap.Select(pszFromId, clsXmlUser) == false) return E_AUTH_ERROR;

    // P7: 가입자의 service_id 가 0(미지정)이면 REGISTER 거부
    if (clsXmlUser.m_iServiceId <= 0) {
        CLog::Print(LOG_ERROR, "Auth reject: user=%s has no service binding", pszFromId);
        return E_AUTH_ERROR;
    }

    // 서비스 정보 로드 → effective username 결정
    ServiceInfo svc = gclsServiceMap.GetById(clsXmlUser.m_iServiceId);
    std::string strExpectedUser;
    if (svc.id > 0 && !clsXmlUser.m_strImsi.empty()) {
        // 신규 경로: IMSI + service.domain
        strExpectedUser = clsXmlUser.m_strImsi + "@" + svc.domain;
    } else {
        // 레거시 fallback: 저장된 auth_id 그대로
        strExpectedUser = clsXmlUser.m_strAuthId;
    }

    if (strExpectedUser != pclsCredential->m_strUserName) {
        CLog::Print(LOG_ERROR, "Auth reject: username mismatch (got=%s, expected=%s)",
                    pclsCredential->m_strUserName.c_str(), strExpectedUser.c_str());
        return E_AUTH_ERROR;
    }

    const char* pszQop    = pclsCredential->m_strQop.empty()        ? NULL : pclsCredential->m_strQop.c_str();
    const char* pszNc     = pclsCredential->m_strNonceCount.empty() ? NULL : pclsCredential->m_strNonceCount.c_str();
    const char* pszCnonce = pclsCredential->m_strCnonce.empty()     ? NULL : pclsCredential->m_strCnonce.c_str();

    if (CheckAuthorizationResponse(pclsCredential->m_strUserName.c_str(), pclsCredential->m_strRealm.c_str(),
            pclsCredential->m_strNonce.c_str(), pclsCredential->m_strUri.c_str(),
            pclsCredential->m_strResponse.c_str(), clsXmlUser.m_strPassWord.c_str(),
            pszMethod, pszQop, pszNc, pszCnonce) == false) return E_AUTH_ERROR;

    return E_AUTH_OK;
}

bool CCscfModule::CheckAuthrization(CSipMessage* pclsMessage) {
    SIP_CREDENTIAL_LIST::iterator itCL = pclsMessage->m_clsAuthorizationList.begin();

    if (itCL == pclsMessage->m_clsAuthorizationList.end()) {
        SendUnAuthorizedResponse(pclsMessage);
        return false;
    }

    CspUser clsUser;
    ECheckAuthResult eRes = CheckAuthorization(&(*itCL),
        pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(),
        pclsMessage->m_strSipMethod.c_str(), clsUser);

    switch (eRes) {
    case E_AUTH_NONCE_NOT_FOUND:
        SendUnAuthorizedResponse(pclsMessage);
        return false;
    case E_AUTH_ERROR:
        {
            CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(SIP_FORBIDDEN);
            if (pclsResponse) gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
        }
        return false;
    default:
        break;
    }

    gclsUserMap.Insert(pclsMessage, NULL, &clsUser);
    return true;
}

// ──────────────────────────────────────────────────────────────
//  SendResponse 헬퍼
// ──────────────────────────────────────────────────────────────

bool CCscfModule::SendResponse(CSipMessage* pclsMessage, int iStatusCode) {
    CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(iStatusCode);
    if (pclsResponse == NULL) return false;

    gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
    return true;
}

// ──────────────────────────────────────────────────────────────
//  OnSipRequest — REGISTER, SUBSCRIBE 처리
// ──────────────────────────────────────────────────────────────

bool CCscfModule::OnSipRequest(int iThreadId, CSipMessage* pclsMessage) {
    if (pclsMessage->IsMethod(SIP_METHOD_REGISTER)) {
        return RecvRequestRegister(iThreadId, pclsMessage);
    } else if (pclsMessage->IsMethod("SUBSCRIBE")) {
        return RecvRequestSubscribe(iThreadId, pclsMessage);
    }
    return false;
}

// ──────────────────────────────────────────────────────────────
//  REGISTER 처리
// ──────────────────────────────────────────────────────────────

bool CCscfModule::RecvRequestRegister(int iThreadId, CSipMessage* pclsMessage) {
    if (pclsMessage->m_iExpires > 0 && gclsSetup.m_iMinRegisterTimeout != 0) {
        if (pclsMessage->m_iExpires < gclsSetup.m_iMinRegisterTimeout) {
            CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(SIP_INTERVAL_TOO_BRIEF);
            if (pclsResponse == NULL) return false;
            pclsResponse->AddHeader("Min-Expires", gclsSetup.m_iMinRegisterTimeout);
            gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
            return true;
        }
    }

    SIP_CREDENTIAL_LIST::iterator itCL = pclsMessage->m_clsAuthorizationList.begin();
    if (itCL == pclsMessage->m_clsAuthorizationList.end()) {
        return SendUnAuthorizedResponse(pclsMessage);
    }

    CspUser clsUser;
    ECheckAuthResult eRes = CheckAuthorization(&(*itCL),
        pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(),
        pclsMessage->m_strSipMethod.c_str(), clsUser);

    switch (eRes) {
    case E_AUTH_NONCE_NOT_FOUND:
        SendUnAuthorizedResponse(pclsMessage);
        return true;
    case E_AUTH_ERROR:
        SendResponse(pclsMessage, SIP_FORBIDDEN);
        return true;
    default:
        break;
    }

    // UNREGISTER
    if (pclsMessage->GetExpires() == 0) {
        std::string strUserId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
        gclsUserMap.Delete(strUserId.c_str());
        // DB logout_time 갱신 + CspUserMap 캐시 업데이트
        gclsCspUserMap.unregisterUser(strUserId);
        // PTT 그룹콜 세션 정리 (활성 호 있으면 BYE + DB 갱신)
        gclsGroupCallService.ClearUserCall(strUserId);
        SendResponse(pclsMessage, SIP_OK);
        CLog::Print(LOG_INFO, "RecvRequestRegister: user(%s) unregistered",
                    strUserId.c_str());
        return true;
    }

    // REGISTER
    CSipFrom clsContact;
    if (gclsUserMap.Insert(pclsMessage, &clsContact, &clsUser)) {
        CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(SIP_OK);
        if (pclsResponse == NULL) return false;

        pclsResponse->m_clsContactList.push_back(clsContact);
        pclsResponse->AddHeader("Expires", 3600);
        pclsResponse->AddHeader("Supported", "path,100rel,precondition");

        const std::string strRegDomain = (clsUser.m_strServiceType == "ptt")
            ? gclsSetup.GetDomainForService("mcptt")
            : gclsSetup.GetDomainForService("volte");

        {
            char szServiceRoute[512];
            snprintf(szServiceRoute, sizeof(szServiceRoute),
                "<sip:%s@%s:%d;lr>",
                strRegDomain.c_str(),
                gclsSetup.m_strLocalIp.c_str(),
                gclsSetup.m_iUdpPort);
            pclsResponse->AddHeader("Service-Route", szServiceRoute);
        }

        {
            char szPAUri[512];
            const std::string& strUser = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
            snprintf(szPAUri, sizeof(szPAUri), "<sip:%s@%s>", strUser.c_str(), strRegDomain.c_str());
            pclsResponse->AddHeader("P-Associated-URI", szPAUri);
            // P-Asserted-Identity (3GPP TS 24.229 §5.4.3.2) — 인증된 사용자 신원
            pclsResponse->AddHeader("P-Asserted-Identity", szPAUri);
        }

        gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);

        gclsCspUserMap.registerUser(clsUser.m_strId, "");

        // PTT auto-invite
        if (clsUser.m_strServiceType == "ptt" || clsUser.m_strServiceType == "both") {
            std::string strUserId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
            CLog::Print(LOG_INFO, "RecvRequestRegister: PTT user(%s) registered, auto-joining groups", strUserId.c_str());

            gclsGroupCallService.ClearUserCall(strUserId);

            if (gclsDbManager.IsConnected()) {
                std::vector<std::string> vecGroupIds;
                gclsDbManager.SelectGroupsByUser(strUserId, vecGroupIds);
                for (const auto& gid : vecGroupIds) {
                    CLog::Print(LOG_INFO, "RecvRequestRegister: Inviting user(%s) to group(%s) [DB]",
                                strUserId.c_str(), gid.c_str());
                    gclsGroupCallService.InviteMember(strUserId.c_str(), gid.c_str());
                }
            } else {
                gclsGroupMap.IterateInternal([&strUserId](const CspPttGroup& group) {
                    for (const auto& pUser : group._pusers) {
                        if (pUser && pUser->_id == strUserId) {
                            CLog::Print(LOG_INFO, "RecvRequestRegister: Inviting user(%s) to group(%s) [map]",
                                        strUserId.c_str(), group._id.c_str());
                            gclsGroupCallService.InviteMember(strUserId.c_str(), group._id.c_str());
                            break;
                        }
                    }
                });
            }
        }
    } else {
        SendResponse(pclsMessage, SIP_BAD_REQUEST);
    }

    return true;
}

// ──────────────────────────────────────────────────────────────
//  SUBSCRIBE 처리
// ──────────────────────────────────────────────────────────────

bool CCscfModule::RecvRequestSubscribe(int iThreadId, CSipMessage* pclsMessage) {
    char szFromBuf[256];
    pclsMessage->m_clsFrom.m_clsUri.ToString(szFromBuf, sizeof(szFromBuf));
    CLog::Print(LOG_DEBUG, "RecvRequest: SUBSCRIBE From=%s", szFromBuf);

    std::string strFromId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
    if (!gclsUserMap.Select(strFromId.c_str())) {
        CLog::Print(LOG_ERROR, "SUBSCRIBE Rejected: User %s not registered", strFromId.c_str());
        SendResponse(pclsMessage, 403);
        return true;
    }

    std::string strSubCallId;
    pclsMessage->GetCallId(strSubCallId);

    std::string strFromTag = pclsMessage->m_clsFrom.SelectParamValue(SIP_TAG);

    char szReqUriBuf[256];
    pclsMessage->m_clsReqUri.ToString(szReqUriBuf, sizeof(szReqUriBuf));
    std::string strReqUri = szReqUriBuf;
    std::string strEventType;
    if (strReqUri.find("gms") != std::string::npos) {
        strEventType = "gms";
    } else if (strReqUri.find("cms") != std::string::npos) {
        strEventType = "cms";
    } else {
        strEventType = "gms";
    }

    int iExpires = pclsMessage->GetExpires();

    if (iExpires == 0) {
        gclsSubscriptionManager.RemoveSubscription(strSubCallId);
        SendResponse(pclsMessage, 200);
        return true;
    }

    std::string strContactUri;
    if (!pclsMessage->m_clsContactList.empty()) {
        char szContact[256];
        pclsMessage->m_clsContactList.front().m_clsUri.ToString(szContact, sizeof(szContact));
        strContactUri = szContact;
    } else {
        strContactUri = szFromBuf;
    }

    char szToTag[64];
    SipMakeTag(szToTag, sizeof(szToTag));

    SubscriptionInfo info;
    info.strUserId = strFromId;
    info.strSubscriberUri = szFromBuf;
    info.strFromTag = strFromTag;
    info.strToTag = szToTag;
    info.strContact = strContactUri;
    info.strCallId = strSubCallId;
    info.strEventType = strEventType;
    info.iExpires = (iExpires > 0) ? iExpires : 3600;
    info.tStartTime = time(NULL);
    info.iNotifySeq = 1;

    gclsSubscriptionManager.AddSubscription(strReqUri, info);

    CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(200);
    if (pclsResponse) {
        pclsResponse->m_clsTo.InsertParam(SIP_TAG, szToTag);
        gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
    }

    SendInitialNotify(info);
    return true;
}
