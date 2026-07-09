/**
 * CCscfModule — CSCF 모듈: REGISTER, SUBSCRIBE, 인증 처리
 *
 * SipServerRegister.hpp 와 SipServer.cpp 의 SUBSCRIBE 처리 로직을 이 모듈로 이동.
 */

#include "CscfModule.h"

#include <map>
#include <mutex>
#include <time.h>
#include "CspAddressing.h"
#include "CspPttGroup.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupCallService.h"
#include "GroupMap.h"
#include "Log.h"
#include "McpttInfo.h"  // ParseAffiliationCommand (TS 24.379 §9 affiliation-command)
#include "NonceMap.h"
#include "SipMd5.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "SipStackThread.h"  // GetCurrentInboundListenerId()
#include "SipUtility.h"
#include "SubscriptionManager.h"
#include "UserMap.h"

extern CSipUserAgent gclsUserAgent;
extern void SendInitialNotify( const SubscriptionInfo &sub );
extern void SendTerminatedNotify( const SubscriptionInfo &sub );
extern void SendAffiliationNotify( const std::string &strUserId );  // C2

// F-04/F-13: PUBLISH affiliation ETag 저장소 (key = "userId:groupId")
static std::map<std::string, std::string> s_mapEtag;
static std::mutex s_etagMutex;

bool CCscfModule::IsEnabled() const {
    return gclsSetup.m_bRoleCscf;
}

// ──────────────────────────────────────────────────────────────
//  인증 헬퍼 (static)
// ──────────────────────────────────────────────────────────────

bool CCscfModule::AddChallenge( CSipMessage *psttResponse, const std::string &strRealmOverride, bool bStale ) {
    CSipChallenge clsChallenge;
    char szNonce[33];

    if ( gclsNonceMap.GetNewValue( szNonce, sizeof( szNonce ) ) == false ) {
        CLog::Print( LOG_ERROR, "gclsNonce.GetNewValue() error" );
        return false;
    }

    clsChallenge.m_strType = "Digest";
    clsChallenge.m_strAlgorithm = "MD5";
    clsChallenge.m_strNonce = szNonce;
    // v3 (2026-04-22): fallback realm 은 access_services 의 첫 voip 서비스 auth_realm/domain.
    //   strRealmOverride 가 있으면 그걸 우선 사용 (호출자가 요청의 From host 로 결정한 값).
    std::string strFallbackRealm;
    if ( strRealmOverride.empty() ) {
        ServiceInfo svcFb = gclsServiceMap.GetByKind( "volte" );
        strFallbackRealm = CCspServiceMap::EffectiveRealm( svcFb );
    }
    clsChallenge.m_strRealm = strRealmOverride.empty() ? strFallbackRealm : strRealmOverride;
    clsChallenge.m_strQop = "auth";
    if ( bStale ) clsChallenge.m_strStale = "true";

    psttResponse->m_clsWwwAuthenticateList.push_back( clsChallenge );
    return true;
}

bool CCscfModule::SendUnAuthorizedResponse( CSipMessage *pclsMessage, const std::string &strRealmOverride, bool bStale ) {
    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_UNAUTHORIZED );
    if ( pclsResponse == NULL ) return false;

    AddChallenge( pclsResponse, strRealmOverride, bStale );
    gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    return true;
}

bool CCscfModule::CheckAuthorizationResponse( const char *pszUserName, const char *pszRealm, const char *pszNonce,
                                              const char *pszUri, const char *pszResponse, const char *pszPassWord,
                                              const char *pszMethod, const char *pszQop, const char *pszNc,
                                              const char *pszCnonce ) {
    char szA1[301], szA2[201], szMd5[33], szResponse[1024];

    snprintf( szA1, sizeof( szA1 ), "%s:%s:%s", pszUserName, pszRealm, pszPassWord );
    SipMd5String( szA1, szMd5 );
    snprintf( szA1, sizeof( szA1 ), "%s", szMd5 );

    snprintf( szA2, sizeof( szA2 ), "%s:%s", pszMethod, pszUri );
    SipMd5String( szA2, szMd5 );
    snprintf( szA2, sizeof( szA2 ), "%s", szMd5 );

    if ( pszQop && strcasecmp( pszQop, "auth" ) == 0 ) {
        snprintf( szResponse, sizeof( szResponse ), "%s:%s:%s:%s:%s:%s", szA1, pszNonce, pszNc ? pszNc : "",
                  pszCnonce ? pszCnonce : "", pszQop, szA2 );
    } else {
        snprintf( szResponse, sizeof( szResponse ), "%s:%s:%s", szA1, pszNonce, szA2 );
    }

    SipMd5String( szResponse, szMd5 );
    snprintf( szResponse, sizeof( szResponse ), "%s", szMd5 );

    if ( strcmp( szResponse, pszResponse ) ) {
        CLog::Print( LOG_ERROR, "response[%s] is not correct. correct response is [%s]", pszResponse, szResponse );
        return false;
    }
    return true;
}

ECheckAuthResult CCscfModule::CheckAuthorization( CSipCredential *pclsCredential, const char *pszFromId,
                                                  const char *pszMethod, CspUser &clsXmlUser ) {
    if ( pclsCredential->m_strUserName.empty() ) return E_AUTH_ERROR;
    // RFC 7616: qop 사용 시 nonce 는 nc 증가와 함께 재사용 가능 (실제 IMS 망 동일).
    //   여기서는 존재만 확인(삭제 안 함)하고, 해시 검증 통과 후 CheckAndUpdateNc 로 replay 차단.
    //   qop 미사용(레거시) credential 은 기존대로 1회용 삭제.
    const bool bQop = !pclsCredential->m_strQop.empty();
    if ( gclsNonceMap.Select( pclsCredential->m_strNonce.c_str(), !bQop ) == false )
        return E_AUTH_NONCE_NOT_FOUND;
    if ( gclsCspUserMap.Select( pszFromId, clsXmlUser ) == false ) return E_AUTH_ERROR;

    // v3 (2026-04-22): 가입자의 service_ref 가 비어있으면 REGISTER 거부
    if ( clsXmlUser.m_strServiceRef.empty() ) {
        CLog::Print( LOG_ERROR, "Auth reject: user=%s has no service binding", pszFromId );
        return E_AUTH_ERROR;
    }

    // 서비스 정보 로드 → effective username 결정
    ServiceInfo svc = gclsServiceMap.GetByName( clsXmlUser.m_strServiceRef );

    // v3 (2026-04-22): inbound_policy=restricted 검증.
    //   psip 확장으로 메시지에 listener_id 가 담기지만, 이 함수 시그니처에는 메시지가 없어
    //   thread-local (GetCurrentInboundListenerId) 를 계속 사용. 두 경로 모두 결과 동일.
    if ( svc.id > 0 ) {
        int iListenerId = GetCurrentInboundListenerId();
        if ( !CCspServiceMap::IsInboundAllowed( svc, iListenerId ) ) {
            CLog::Print( LOG_ERROR, "Auth reject: service=%s inbound_policy=%s listener_id=%d", svc.name.c_str(),
                         svc.inbound_policy.c_str(), iListenerId );
            return E_AUTH_ERROR;
        }
    }

    std::string strExpectedUser;
    if ( svc.id > 0 && !clsXmlUser.m_strImsi.empty() ) {
        // v3: IMSI + service.domain
        strExpectedUser = clsXmlUser.m_strImsi + "@" + svc.domain;
    } else {
        CLog::Print( LOG_ERROR, "Auth reject: user=%s service_ref='%s' imsi='%s' — 데이터 불완전", pszFromId,
                     clsXmlUser.m_strServiceRef.c_str(), clsXmlUser.m_strImsi.c_str() );
        return E_AUTH_ERROR;
    }

    if ( strExpectedUser != pclsCredential->m_strUserName ) {
        CLog::Print( LOG_ERROR, "Auth reject: username mismatch (got=%s, expected=%s)",
                     pclsCredential->m_strUserName.c_str(), strExpectedUser.c_str() );
        return E_AUTH_ERROR;
    }

    const char *pszQop = pclsCredential->m_strQop.empty() ? NULL : pclsCredential->m_strQop.c_str();
    const char *pszNc = pclsCredential->m_strNonceCount.empty() ? NULL : pclsCredential->m_strNonceCount.c_str();
    const char *pszCnonce = pclsCredential->m_strCnonce.empty() ? NULL : pclsCredential->m_strCnonce.c_str();

    if ( CheckAuthorizationResponse( pclsCredential->m_strUserName.c_str(), pclsCredential->m_strRealm.c_str(),
                                     pclsCredential->m_strNonce.c_str(), pclsCredential->m_strUri.c_str(),
                                     pclsCredential->m_strResponse.c_str(), clsXmlUser.m_strPassWord.c_str(), pszMethod,
                                     pszQop, pszNc, pszCnonce ) == false )
        return E_AUTH_ERROR;

    // qop 재사용 credential: 해시 통과 후 nc 단조증가 검사 (동일 nc 재전송 = replay → stale 재챌린지)
    if ( bQop ) {
        unsigned int uiNc = pszNc ? (unsigned int)strtoul( pszNc, NULL, 16 ) : 0;
        if ( gclsNonceMap.CheckAndUpdateNc( pclsCredential->m_strNonce.c_str(), uiNc ) == false ) {
            CLog::Print( LOG_ERROR, "Auth reject: nc replay (user=%s nc=%s)", pszFromId, pszNc ? pszNc : "" );
            return E_AUTH_NONCE_NOT_FOUND;
        }
    }

    return E_AUTH_OK;
}

bool CCscfModule::CheckAuthrization( CSipMessage *pclsMessage ) {
    SIP_CREDENTIAL_LIST::iterator itCL = pclsMessage->m_clsAuthorizationList.begin();

    // 3GPP pre-auth: nonce/response 없는 빈 Authorization은 'Authorization 없음'과 동일 취급.
    //   (RecvRequestRegister 와 동일 가드 — 첫 챌린지에 stale=true 가 붙는 버그 방지)
    const bool bEmptyPreAuth = ( itCL != pclsMessage->m_clsAuthorizationList.end() &&
                                 itCL->m_strNonce.empty() && itCL->m_strResponse.empty() );
    if ( itCL == pclsMessage->m_clsAuthorizationList.end() || bEmptyPreAuth ) {
        SendUnAuthorizedResponse( pclsMessage );
        return false;
    }

    CspUser clsUser;
    ECheckAuthResult eRes = CheckAuthorization( &( *itCL ), pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(),
                                                pclsMessage->m_strSipMethod.c_str(), clsUser );

    switch ( eRes ) {
        case E_AUTH_NONCE_NOT_FOUND:
            SendUnAuthorizedResponse( pclsMessage, "", true );
            return false;
        case E_AUTH_ERROR: {
            CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
            if ( pclsResponse ) gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
        }
            return false;
        default:
            break;
    }

    gclsUserMap.Insert( pclsMessage, NULL, &clsUser );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  SendResponse 헬퍼
// ──────────────────────────────────────────────────────────────

bool CCscfModule::SendResponse( CSipMessage *pclsMessage, int iStatusCode ) {
    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( iStatusCode );
    if ( pclsResponse == NULL ) return false;

    gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  OnSipRequest — REGISTER, SUBSCRIBE 처리
// ──────────────────────────────────────────────────────────────

bool CCscfModule::OnSipRequest( int iThreadId, CSipMessage *pclsMessage ) {
    if ( pclsMessage->IsMethod( SIP_METHOD_REGISTER ) ) {
        return RecvRequestRegister( iThreadId, pclsMessage );
    } else if ( pclsMessage->IsMethod( "SUBSCRIBE" ) ) {
        return RecvRequestSubscribe( iThreadId, pclsMessage );
    } else if ( pclsMessage->IsMethod( "PUBLISH" ) ) {
        return RecvRequestPublish( iThreadId, pclsMessage );
    }
    return false;
}

// ──────────────────────────────────────────────────────────────
//  REGISTER 처리
// ──────────────────────────────────────────────────────────────

bool CCscfModule::RecvRequestRegister( int iThreadId, CSipMessage *pclsMessage ) {
    if ( pclsMessage->m_iExpires > 0 && gclsSetup.m_iMinRegisterTimeout != 0 ) {
        if ( pclsMessage->m_iExpires < gclsSetup.m_iMinRegisterTimeout ) {
            CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_INTERVAL_TOO_BRIEF );
            if ( pclsResponse == NULL ) return false;
            pclsResponse->AddHeader( "Min-Expires", gclsSetup.m_iMinRegisterTimeout );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
            return true;
        }
    }

    SIP_CREDENTIAL_LIST::iterator itCL = pclsMessage->m_clsAuthorizationList.begin();
    // realm: 실제 IMS 망은 등록 도메인(ptt/ims)과 무관하게 항상 IMS core realm 을 사용한다.
    //   access_services 의 auth_realm 이 SOT — Request-URI host 로 서비스 조회 후 EffectiveRealm.
    //   (auth_realm 미지정 시 domain 상속 = 기존 동작.) 서비스 미정의 도메인은 host fallback.
    ServiceInfo svcReg = gclsServiceMap.GetByDomain( pclsMessage->m_clsReqUri.m_strHost );
    const std::string strRegRealm = ( svcReg.id > 0 ) ? CCspServiceMap::EffectiveRealm( svcReg )
                                                      : pclsMessage->m_clsReqUri.m_strHost;
    // 3GPP pre-auth: 첫 REGISTER 의 빈 Authorization(nonce/response 없음)은 IMPI 광고일 뿐
    //   답안 제출이 아니다. 'Authorization 없음'과 동일하게 취급 — nonce 조회로 넘기면
    //   E_AUTH_NONCE_NOT_FOUND(F-07 stale=true) 경로로 빠져 첫 챌린지에 stale 이 붙는 버그가 됨.
    const bool bEmptyPreAuth = ( itCL != pclsMessage->m_clsAuthorizationList.end() &&
                                 itCL->m_strNonce.empty() && itCL->m_strResponse.empty() );
    if ( itCL == pclsMessage->m_clsAuthorizationList.end() || bEmptyPreAuth ) {
        return SendUnAuthorizedResponse( pclsMessage, strRegRealm );
    }

    CspUser clsUser;
    ECheckAuthResult eRes = CheckAuthorization( &( *itCL ), pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(),
                                                pclsMessage->m_strSipMethod.c_str(), clsUser );

    switch ( eRes ) {
        case E_AUTH_NONCE_NOT_FOUND:
            SendUnAuthorizedResponse( pclsMessage, strRegRealm, true );  // F-07: stale=true
            return true;
        case E_AUTH_ERROR:
            SendResponse( pclsMessage, SIP_FORBIDDEN );
            return true;
        default:
            break;
    }

    // UNREGISTER
    if ( pclsMessage->GetExpires() == 0 ) {
        std::string strUserId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
        gclsUserMap.Delete( strUserId.c_str() );
        // DB logout_time 갱신 + CspUserMap 캐시 업데이트
        gclsCspUserMap.unregisterUser( strUserId );
        // PTT 그룹콜 세션 정리 (활성 호 있으면 BYE + DB 갱신)
        gclsGroupCallService.ClearUserCall( strUserId );
        // 등록 해제 시 암묵적 de-affiliation (TS 24.379 — affiliation 은 등록에 묶임)
        if ( gclsDbManager.IsConnected() ) {
            gclsDbManager.RemoveAffiliationsByUser( strUserId );
        }
        SendResponse( pclsMessage, SIP_OK );
        CLog::Print( LOG_INFO, "RecvRequestRegister: user(%s) unregistered (de-affiliated)", strUserId.c_str() );
        return true;
    }

    // REGISTER
    CSipFrom clsContact;
    if ( gclsUserMap.Insert( pclsMessage, &clsContact, &clsUser ) ) {
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_OK );
        if ( pclsResponse == NULL ) return false;

        // F-12: 단말 요청 Expires 협상 (RFC 3261 §10.3). 요청값이 유효하면 그대로, 초과 시 3600으로 조정.
        int iReqExpires = pclsMessage->GetExpires();
        int iGrantedExpires = ( iReqExpires > 0 && iReqExpires <= 3600 ) ? iReqExpires : 3600;

        // F-03: Contact에 expires 파라미터 포함 (RFC 3261 §10.3)
        char szExpires[16];
        snprintf( szExpires, sizeof( szExpires ), "%d", iGrantedExpires );
        clsContact.InsertParam( "expires", szExpires );

        pclsResponse->m_clsContactList.push_back( clsContact );
        pclsResponse->AddHeader( "Expires", iGrantedExpires );
        pclsResponse->AddHeader( "Supported", "path,100rel,precondition" );

        // v3 (2026-04-22): AccessServiceMap 이 SOT. fallback 은 Setup.Realm (레거시).
        const std::string strRegDomain = ( clsUser.m_strServiceType == "ptt" )
                                             ? gclsServiceMap.GetDomainByKind( "ptt" )
                                             : gclsServiceMap.GetDomainByKind( "volte" );

        {
            char szServiceRoute[512];
            // T4: 응답은 수신 listener 의 bind_ip:bind_port 로 Service-Route 생성.
            //     단말이 다른 NIC/리스너로 REGISTER 했어도 그 리스너의 주소가 응답 자기 주소가 됨.
            const int iListenerId = GetCurrentInboundListenerId();
            const std::string strSipAddr = CspAddressing::GetLocalSipAddress( iListenerId );
            const int iSipPort = CspAddressing::GetLocalSipPort( iListenerId, gclsSetup.m_iUdpPort );
            snprintf( szServiceRoute, sizeof( szServiceRoute ), "<sip:%s@%s:%d;lr>", strRegDomain.c_str(),
                      strSipAddr.c_str(), iSipPort );
            pclsResponse->AddHeader( "Service-Route", szServiceRoute );
        }

        {
            char szPAUri[512];
            const std::string &strUser = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
            snprintf( szPAUri, sizeof( szPAUri ), "<sip:%s@%s>", strUser.c_str(), strRegDomain.c_str() );
            pclsResponse->AddHeader( "P-Associated-URI", szPAUri );
        }

        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );

        gclsCspUserMap.registerUser( clsUser.m_strId, "" );

        // [MCPTT 규격] REGISTER 는 호에 부작용 없음 (TS 24.379).
        //   그룹콜 조인 트리거 = (a) 발신 UE 의 그룹 INVITE → ProcessGroupCall fan-out,
        //   또는 (b) affiliation(PUBLISH) → late entry. REGISTER 자동초대/ClearUserCall 안 함.
        //   (예전엔 등록 갱신마다 ClearUserCall+재초대 → 활성 레그 teardown 버그.)
        //   진행 중 세션 멤버의 재등록은 DB 의 affiliation 기반으로 CheckGroupIntegrity 가 복구.
    } else {
        SendResponse( pclsMessage, SIP_BAD_REQUEST );
    }

    return true;
}

// ──────────────────────────────────────────────────────────────
//  SUBSCRIBE 처리
// ──────────────────────────────────────────────────────────────

bool CCscfModule::RecvRequestSubscribe( int iThreadId, CSipMessage *pclsMessage ) {
    char szFromBuf[256];
    pclsMessage->m_clsFrom.m_clsUri.ToString( szFromBuf, sizeof( szFromBuf ) );
    CLog::Print( LOG_DEBUG, "RecvRequest: SUBSCRIBE From=%s", szFromBuf );

    std::string strFromId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
    if ( !gclsUserMap.Select( strFromId.c_str() ) ) {
        CLog::Print( LOG_ERROR, "SUBSCRIBE Rejected: User %s not registered", strFromId.c_str() );
        SendUnAuthorizedResponse( pclsMessage );
        return true;
    }

    std::string strSubCallId;
    pclsMessage->GetCallId( strSubCallId );

    std::string strFromTag = pclsMessage->m_clsFrom.SelectParamValue( SIP_TAG );

    char szReqUriBuf[256];
    pclsMessage->m_clsReqUri.ToString( szReqUriBuf, sizeof( szReqUriBuf ) );
    std::string strReqUri = szReqUriBuf;

    // Contact (client_id 로도 사용)
    std::string strContactUri;
    if ( !pclsMessage->m_clsContactList.empty() ) {
        char szContact[256];
        pclsMessage->m_clsContactList.front().m_clsUri.ToString( szContact, sizeof( szContact ) );
        strContactUri = szContact;
    } else {
        strContactUri = szFromBuf;
    }

    // Event 헤더 추출 (";id=..." 등 파라미터는 제거하고 토큰만)
    std::string strEventHdr;
    {
        CSipHeader *pclsEventHdr = pclsMessage->GetHeader( "Event" );
        if ( pclsEventHdr ) {
            strEventHdr = pclsEventHdr->m_strValue;
            size_t nSemi = strEventHdr.find( ';' );
            if ( nSemi != std::string::npos ) strEventHdr = strEventHdr.substr( 0, nSemi );
        }
    }

    // 그룹 affiliation 판별 (TS 24.379 §9): Request-URI user 부분이 알려진 그룹 ID 이면
    //   해당 SUBSCRIBE 를 (user, group, client) affiliation 으로 처리한다.
    //   Event 헤더가 presence/conference 면 더 명확하나, 그룹 매칭만으로 충분.
    std::string strReqUriUser = pclsMessage->m_clsReqUri.m_strUser;
    bool bAffiliation = !strReqUriUser.empty() && gclsGroupMap.Contains( strReqUriUser.c_str() );

    // C2: affiliation-info 구독 판별 (TS 24.379 §9.3) — Event:presence 또는
    //   Accept: application/vnd.3gpp.mcptt-affiliation-info+xml → 제휴상태 NOTIFY 대상.
    CSipHeader *pclsSubEvent  = pclsMessage->GetHeader( "Event" );
    CSipHeader *pclsSubAccept = pclsMessage->GetHeader( "Accept" );
    bool bAffInfo = ( pclsSubEvent && pclsSubEvent->m_strValue == "presence" ) ||
                    ( pclsSubAccept && pclsSubAccept->m_strValue.find( "mcptt-affiliation-info" ) != std::string::npos );

    std::string strEventType;
    if ( strEventHdr == "reg" ) {
        // RFC 3680 reg-event: 실제 UE 는 REGISTER 200 OK 직후 자신의 등록 상태를 구독.
        strEventType = "reg";
    } else if ( bAffInfo ) {
        strEventType = "affiliation";  // 제휴상태(affiliation-info) 구독 → presence NOTIFY
    } else if ( bAffiliation ) {
        strEventType = "conference";  // 그룹 affiliation/conference 상태 구독
    } else if ( strReqUri.find( "gms" ) != std::string::npos ) {
        strEventType = "gms";
    } else if ( strReqUri.find( "cms" ) != std::string::npos ) {
        strEventType = "cms";
    } else {
        strEventType = "gms";
    }

    int iExpires = pclsMessage->GetExpires();

    if ( iExpires == 0 ) {
        // RFC 3265 §3.1.4: 200 OK 먼저, 그 다음 final NOTIFY (Subscription-State: terminated)
        SendResponse( pclsMessage, 200 );

        SubscriptionInfo subInfo;
        if ( gclsSubscriptionManager.GetSubscriptionByCallId( strSubCallId, subInfo ) ) {
            SendTerminatedNotify( subInfo );
        }

        gclsSubscriptionManager.RemoveSubscription( strSubCallId );

        if ( bAffiliation && gclsDbManager.IsConnected() ) {
            gclsDbManager.RemoveAffiliation( strReqUriUser, strFromId, strContactUri );
            CLog::Print( LOG_INFO, "[Affiliation] de-affiliate user=%s group=%s", strFromId.c_str(),
                         strReqUriUser.c_str() );
        }
        return true;
    }

    if ( bAffiliation && gclsDbManager.IsConnected() ) {
        gclsDbManager.InsertAffiliation( strReqUriUser, strFromId, strContactUri, iExpires );
        CLog::Print( LOG_INFO, "[Affiliation] affiliate user=%s group=%s expires=%d", strFromId.c_str(),
                     strReqUriUser.c_str(), iExpires );
    }

    char szToTag[64];
    SipMakeTag( szToTag, sizeof( szToTag ) );

    SubscriptionInfo info;
    info.strUserId = strFromId;
    info.strSubscriberUri = szFromBuf;
    info.strFromTag = strFromTag;
    info.strToTag = szToTag;
    info.strContact = strContactUri;
    info.strCallId = strSubCallId;
    info.strEventType = strEventType;
    info.iExpires = ( iExpires > 0 ) ? iExpires : 3600;
    info.tStartTime = time( NULL );
    info.iNotifySeq = 1;
    // NOTIFY 송신 시 SUBSCRIBE 수신 listener 의 IP/Port 를 Via/Contact 자기 주소로 사용
    info.iInboundListenerId = GetCurrentInboundListenerId();

    gclsSubscriptionManager.AddSubscription( strReqUri, info );

    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( 200 );
    if ( pclsResponse ) {
        pclsResponse->m_clsTo.InsertParam( SIP_TAG, szToTag );
        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    }

    SendInitialNotify( info );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  PUBLISH 처리 — MCPTT affiliation (TS 24.379 §9, RFC 3903)
//   UE 가 그룹 URI 로 PUBLISH(application/vnd.3gpp.mcptt-affiliation-command+xml) →
//   (user, group, client) affiliation 등록/해제. Expires>0=affiliate, Expires:0 또는
//   body 에 "de-affiliate"=해제. dialog/NOTIFY 없음(상태 publish). 200 OK + SIP-ETag.
//   late entry(진행 중 prearranged/상시 chat 세션 합류)는 CheckGroupIntegrity 주기 sweep 이
//   affiliation(DB) 기반으로 수행하므로 여기서 직접 INVITE 하지 않는다.
// ──────────────────────────────────────────────────────────────
bool CCscfModule::RecvRequestPublish( int iThreadId, CSipMessage *pclsMessage ) {
    (void)iThreadId;
    char szFromBuf[256];
    pclsMessage->m_clsFrom.m_clsUri.ToString( szFromBuf, sizeof( szFromBuf ) );
    std::string strFromId = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
    if ( !gclsUserMap.Select( strFromId.c_str() ) ) {
        CLog::Print( LOG_ERROR, "PUBLISH Rejected: User %s not registered", strFromId.c_str() );
        SendResponse( pclsMessage, 403 );
        return true;
    }

    // F-05: Event 헤더 검증 — TS 24.379 §9는 "mcptt" 요구, 불일치 시 489 Bad Event
    CSipHeader *pclsEventHdr = pclsMessage->GetHeader( "Event" );
    if ( pclsEventHdr == NULL || pclsEventHdr->m_strValue != "mcptt" ) {
        CLog::Print( LOG_ERROR, "PUBLISH Rejected: invalid or missing Event header (got '%s')",
                     pclsEventHdr ? pclsEventHdr->m_strValue.c_str() : "" );
        SendResponse( pclsMessage, 489 );
        return true;
    }

    std::string strReqUriUser = pclsMessage->m_clsReqUri.m_strUser;
    bool bAffiliation = !strReqUriUser.empty() && gclsGroupMap.Contains( strReqUriUser.c_str() );
    if ( !bAffiliation ) {
        // affiliation 대상(그룹) 아님 — 상태 없이 200 수용
        SendResponse( pclsMessage, 200 );
        return true;
    }

    std::string strContactUri;
    if ( !pclsMessage->m_clsContactList.empty() ) {
        char szC[256];
        pclsMessage->m_clsContactList.front().m_clsUri.ToString( szC, sizeof( szC ) );
        strContactUri = szC;
    } else {
        strContactUri = szFromBuf;
    }

    // ── C1: Content-Type 검증 + affiliation-command XML 파싱 (TS 24.379 §9) ──
    //   본문이 있으면 Content-Type = application/vnd.3gpp.mcptt-affiliation-command+xml 강제.
    //   (body 없는 순수 Expires:0 refresh 는 관대 처리.)
    const std::string &strBody = pclsMessage->m_strBody;
    const std::string &strCtSub = pclsMessage->m_clsContentType.m_strSubType;
    if ( !strBody.empty() && !strCtSub.empty() &&
         strCtSub.find( "mcptt-affiliation" ) == std::string::npos ) {
        CLog::Print( LOG_ERROR, "[Affiliation/PUBLISH] 415: Content-Type %s/%s (mcptt-affiliation-command 기대)",
                     pclsMessage->m_clsContentType.m_strType.c_str(), strCtSub.c_str() );
        SendResponse( pclsMessage, 415 );  // Unsupported Media Type
        return true;
    }

    int iExpires = pclsMessage->GetExpires();
    // affiliate vs de-affiliate 판정 — affiliation-command 액션 요소 기반 파싱(요소 앵커, substring 아님).
    //   액션이 de-affiliate 이거나 Expires:0 이면 해제, else 등록. group 속성은 Req-URI 와 교차검증.
    CMcpttAffiliation clsCmd = ParseAffiliationCommand( strBody );
    bool bDeaffiliate = ( iExpires == 0 ) || ( clsCmd.bValid && clsCmd.bDeaffiliate );
    if ( clsCmd.bValid && !clsCmd.strGroup.empty() &&
         clsCmd.strGroup.find( strReqUriUser ) == std::string::npos ) {
        CLog::Print( LOG_INFO, "[Affiliation/PUBLISH] command group=%s ≠ Req-URI group=%s (Req-URI 우선)",
                     clsCmd.strGroup.c_str(), strReqUriUser.c_str() );
    }
    CLog::Print( LOG_DEBUG, "[Affiliation/PUBLISH] cmd valid=%d deaffiliate=%d group=%s expires=%d",
                 clsCmd.bValid, bDeaffiliate, clsCmd.strGroup.c_str(), iExpires );

    // ── 규격 정합 (item 1): 멤버십 게이트 — 자신이 멤버인 그룹에만 affiliate 가능 ──
    //   TS 24.481/24.379. (de-affiliate 는 멤버 여부와 무관하게 항상 허용.)
    if ( !bDeaffiliate ) {
        CspPttGroup clsGroup;
        bool bMember = false;
        if ( gclsGroupMap.Select( strReqUriUser.c_str(), clsGroup ) ) {
            for ( const auto &pUser : clsGroup._pusers ) {
                if ( pUser && ( pUser->_id == strFromId || pUser->_mcpttId == strFromId ) ) {
                    bMember = true;
                    break;
                }
            }
        }
        if ( !bMember ) {
            CLog::Print( LOG_ERROR, "[Affiliation/PUBLISH] Rejected: user %s not a member of group %s",
                         strFromId.c_str(), strReqUriUser.c_str() );
            SendResponse( pclsMessage, 403 );
            return true;
        }
    }

    // F-13: SIP-If-Match 검증 (RFC 3903 §4)
    //   헤더 있음 = refresh PUBLISH → 저장된 ETag와 일치해야 함
    //   헤더 없음 = initial PUBLISH → 검증 skip
    std::string strEtagKey = strFromId + ":" + strReqUriUser;
    CSipHeader *pclsIfMatch = pclsMessage->GetHeader( "SIP-If-Match" );
    if ( pclsIfMatch != NULL && !bDeaffiliate ) {
        std::unique_lock<std::mutex> lock( s_etagMutex );
        auto it = s_mapEtag.find( strEtagKey );
        if ( it == s_mapEtag.end() || it->second != pclsIfMatch->m_strValue ) {
            CLog::Print( LOG_ERROR, "PUBLISH 412: ETag mismatch user=%s group=%s If-Match=%s stored=%s",
                         strFromId.c_str(), strReqUriUser.c_str(),
                         pclsIfMatch->m_strValue.c_str(),
                         ( it != s_mapEtag.end() ) ? it->second.c_str() : "(none)" );
            SendResponse( pclsMessage, 412 );
            return true;
        }
    }

    if ( gclsDbManager.IsConnected() ) {
        if ( bDeaffiliate ) {
            gclsDbManager.RemoveAffiliation( strReqUriUser, strFromId, strContactUri );
            CLog::Print( LOG_INFO, "[Affiliation/PUBLISH] de-affiliate user=%s group=%s", strFromId.c_str(),
                         strReqUriUser.c_str() );
        } else {
            gclsDbManager.InsertAffiliation( strReqUriUser, strFromId, strContactUri,
                                             iExpires > 0 ? iExpires : 3600 );
            CLog::Print( LOG_INFO, "[Affiliation/PUBLISH] affiliate user=%s group=%s expires=%d", strFromId.c_str(),
                         strReqUriUser.c_str(), iExpires > 0 ? iExpires : 3600 );
        }
        // C2: 제휴상태 변경 → 해당 가입자의 affiliation-info(presence) 구독자에게 NOTIFY.
        SendAffiliationNotify( strFromId );
    }

    // de-affiliate: ETag 저장소에서 제거 후 200 반환
    if ( bDeaffiliate ) {
        std::unique_lock<std::mutex> lock( s_etagMutex );
        s_mapEtag.erase( strEtagKey );
        SendResponse( pclsMessage, 200 );
        return true;
    }

    // F-04: SIP-ETag 생성 — 밀리초 + 포인터 기반 랜덤 비트 (초 단위 충돌 방지)
    struct timespec ts;
    clock_gettime( CLOCK_REALTIME, &ts );
    unsigned uRnd = (unsigned)( ts.tv_nsec ^ (uintptr_t)pclsMessage );
    char szEtag[64];
    snprintf( szEtag, sizeof( szEtag ), "aff-%llx%08x",
              (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL,
              uRnd );

    // ETag 저장 (initial 또는 refresh 모두 갱신)
    {
        std::unique_lock<std::mutex> lock( s_etagMutex );
        s_mapEtag[strEtagKey] = szEtag;
    }

    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( 200 );
    if ( pclsResponse ) {
        pclsResponse->AddHeader( "SIP-ETag", szEtag );
        pclsResponse->AddHeader( "Expires", iExpires > 0 ? iExpires : 3600 );
        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    }
    return true;
}
