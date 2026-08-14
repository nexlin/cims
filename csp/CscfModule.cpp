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
extern void SendRegEventNotify( const std::string &strUserId, const char *pszEvent,
                                const CUserInfo *pclsInfo );  // RFC 3680 partial

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

    pclsResponse->AddHeader( "Allow", SIP_ALLOW_METHODS );
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
    if ( gclsCspUserMap.Select( pszFromId, clsXmlUser ) == false ) return E_AUTH_USER_NOT_FOUND;

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
        case E_AUTH_USER_NOT_FOUND:
            // 미가입 계정 요청 — REGISTER 경로와 동일하게 소스 NETWORK 덤프 억제 (스캐너).
            //   요약 INFO 는 억제 창 진입 시 1회만.
            if ( !CLog::IsNetworkSourceSuppressed( pclsMessage->m_strClientIp.c_str() ) )
                CLog::Print( LOG_INFO, "CheckAuthrization: unknown user(%s) %s from %s → 403, 소스 로그 %d초 억제",
                             pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), pclsMessage->m_strSipMethod.c_str(),
                             pclsMessage->m_strClientIp.c_str(), SIP_SCAN_SUPPRESS_TTL_SEC );
            CLog::SuppressNetworkSource( pclsMessage->m_strClientIp.c_str(), SIP_SCAN_SUPPRESS_TTL_SEC );
            // fallthrough 없이 동일 403 — 아래 E_AUTH_ERROR 와 같은 응답
            {
                CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
                if ( pclsResponse ) gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
            }
            return false;
        case E_AUTH_ERROR: {
            CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
            if ( pclsResponse ) gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
        }
            return false;
        default:
            break;
    }

    gclsUserMap.Insert( pclsMessage, &clsUser );
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
        case E_AUTH_USER_NOT_FOUND:
            // 미가입 계정 REGISTER = 인터넷 노출 리스너의 계정 무차별 대입 스캐너 신호.
            //   403 후 소스 NETWORK 덤프를 억제해 로그 폭주를 막는다 (toll-fraud INVITE 603
            //   경로와 동일 계약 — 처리·응답은 그대로, 원본 패킷 로그만 생략). 가입자의 자격
            //   증명 실패(E_AUTH_ERROR)는 실단말일 수 있어 억제하지 않는다.
            //   요약 INFO 는 억제 창 진입 시 1회만 — 매 시도마다 찍으면 그 라인이 새 폭주가 된다.
            if ( !CLog::IsNetworkSourceSuppressed( pclsMessage->m_strClientIp.c_str() ) )
                CLog::Print( LOG_INFO, "RecvRequestRegister: unknown user(%s) from %s → 403, 소스 로그 %d초 억제",
                             pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), pclsMessage->m_strClientIp.c_str(),
                             SIP_SCAN_SUPPRESS_TTL_SEC );
            CLog::SuppressNetworkSource( pclsMessage->m_strClientIp.c_str(), SIP_SCAN_SUPPRESS_TTL_SEC );
            SendResponse( pclsMessage, SIP_FORBIDDEN );
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
        // 삭제 직전 바인딩 보관 — reg-event 구독자 통지(partial, event=unregistered)용
        CUserInfo clsRegInfo;
        bool bHadBinding = gclsUserMap.Select( strUserId.c_str(), clsRegInfo );
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
        // reg-event 구독자에게 등록 해제 통지 (partial, 삭제 직전 바인딩)
        if ( bHadBinding ) {
            SendRegEventNotify( strUserId, "unregistered", &clsRegInfo );
        }
        return true;
    }

    // REGISTER
    const bool bRefresh = gclsUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str() );
    if ( gclsUserMap.Insert( pclsMessage, &clsUser ) ) {
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_OK );
        if ( pclsResponse == NULL ) return false;

        // F-12: 요청 Expires 를 그대로 수락 (요청에 없으면 3600 기본값)
        int iReqExpires = pclsMessage->GetExpires();
        int iGrantedExpires = ( iReqExpires > 0 ) ? iReqExpires : 3600;
        char szExpires[16];
        snprintf( szExpires, sizeof( szExpires ), "%d", iGrantedExpires );

        // 200 OK Contact = 요청 Contact 을 그대로 에코 (RFC 3261 §10.3 — 등록된 바인딩 반환).
        //   NAT 뒤 단말의 실제 도달 주소는 UserMap 바인딩(received/rport latch)이 따로 관리한다.
        for ( SIP_FROM_LIST::iterator itContact = pclsMessage->m_clsContactList.begin();
              itContact != pclsMessage->m_clsContactList.end(); ++itContact ) {
            CSipFrom clsContact = *itContact;
            if ( clsContact.UpdateParam( "expires", szExpires ) == false )
                clsContact.InsertParam( "expires", szExpires );
            pclsResponse->m_clsContactList.push_back( clsContact );
        }
        // Contact 없는 REGISTER(바인딩 조회) — 저장된 as-registered Contact 로 응답
        if ( pclsResponse->m_clsContactList.empty() ) {
            CUserInfo clsRegInfo;
            if ( gclsUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsRegInfo ) &&
                 clsRegInfo.m_strContactUri.empty() == false ) {
                CSipFrom clsContact;
                clsContact.m_clsUri.Parse( clsRegInfo.m_strContactUri.c_str(),
                                           (int)clsRegInfo.m_strContactUri.size() );
                clsContact.InsertParam( "expires", szExpires );
                pclsResponse->m_clsContactList.push_back( clsContact );
            }
        }
        // 부여한 만료시간 — Contact 의 expires 파라미터와 Expires 헤더 양쪽에 포함
        pclsResponse->AddHeader( "Expires", iGrantedExpires );
        pclsResponse->AddHeader( "Allow", SIP_ALLOW_METHODS );
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
            // Service-Route = S-CSCF(자기) 주소. host:port 만 사용 — 도메인을 user 파트에
            //   넣으면(sip:도메인@IP) 비표준이라 엄격한 UA 가 이 route 로 요청 생성 시
            //   오라우팅/거부할 수 있다 (RFC 3608 / TS 24.229).
            snprintf( szServiceRoute, sizeof( szServiceRoute ), "<sip:%s:%d;lr>",
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

        // reg-event 구독자에게 등록 갱신 통지 (partial — RFC 3680).
        //   최초 등록은 구독이 있을 수 없어 통상 no-op, 구독 잔존 상태의 재등록이면 created.
        SendRegEventNotify( pclsMessage->m_clsFrom.m_clsUri.m_strUser, bRefresh ? "refreshed" : "created", NULL );

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
    } else if ( strEventHdr == "conference" || bAffiliation ) {
        // conference(RFC 4575) — Event 헤더가 1차 근거, 그룹 URI 매칭은 헤더 없는 구현 호환용.
        strEventType = "conference";
    } else if ( strReqUri.find( "gms" ) != std::string::npos ) {
        strEventType = "gms";
    } else if ( strReqUri.find( "cms" ) != std::string::npos ) {
        strEventType = "cms";
    } else {
        strEventType = "gms";
    }

    // 갱신(in-dialog refresh) SUBSCRIBE 는 Request-URI 가 **자원이 아니라 서버 Contact** 이다
    //   (dialog remote target = 200 OK 의 Contact). 그래서 URI 로 이벤트/자원을 다시 유도하면
    //   conference 구독이 gms 로 재분류돼 엉뚱한 xcap-diff NOTIFY 가 나가고, 구독자 스택은
    //   짝이 맞지 않는 NOTIFY 를 481 로 거절해 구독이 죽는다. RFC 6665 §4.1.2.2 상 갱신은
    //   자원·이벤트를 바꿀 수 없으므로 기존 구독의 값을 그대로 물려받는다(reg/gms/cms 공통).
    SubscriptionInfo clsPrev;
    const bool bRefresh = gclsSubscriptionManager.GetSubscriptionByCallId( strSubCallId, clsPrev );
    if ( bRefresh ) {
        if ( !clsPrev.strEventType.empty() ) strEventType = clsPrev.strEventType;
        if ( strReqUriUser.empty() && !clsPrev.strResourceId.empty() ) strReqUriUser = clsPrev.strResourceId;
        bAffiliation = !strReqUriUser.empty() && gclsGroupMap.Contains( strReqUriUser.c_str() );
    }
    // 재기동 직후의 in-dialog refresh: R-URI 에 자원이 없고(위 주석 — remote target) 이전 기록도
    //   없다(구독 상태는 메모리 — 재기동으로 소실). 이대로 수립하면 자원 없는 구독이 되어
    //   NOTIFY 가 빈 entity/빈 roster 로 나가고 구독자 로스터(채널 접속 인원)가 영구 stale
    //   된다(08-11 실측). 구독 자원 = To URI(초기 SUBSCRIBE 의 R-URI 보존값, RFC 6665)이므로
    //   To 에서 복원한다.
    if ( strReqUriUser.empty() && !pclsMessage->m_clsTo.m_clsUri.m_strUser.empty() ) {
        strReqUriUser = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        bAffiliation = gclsGroupMap.Contains( strReqUriUser.c_str() );
        CLog::Print( LOG_INFO, "SUBSCRIBE refresh without state — resource restored from To URI (%s, user=%s)",
                     strReqUriUser.c_str(), strFromId.c_str() );
    }

    int iExpires = pclsMessage->GetExpires();

    if ( iExpires == 0 ) {
        // RFC 3265 §3.1.4: 200 OK 먼저, 그 다음 final NOTIFY (Subscription-State: terminated)
        //   구독 해지 2xx 에도 Expires: 0 포함 (RFC 6665 §4.2.1.1)
        CSipMessage *pclsUnsubResp = pclsMessage->CreateResponseWithToTag( 200 );
        if ( pclsUnsubResp ) {
            pclsUnsubResp->AddHeader( "Allow", SIP_ALLOW_METHODS );
            pclsUnsubResp->AddHeader( "Expires", 0 );
            pclsUnsubResp->AddHeader( "Supported", "path,100rel,precondition" );
            // 단말이 구독 갱신·해지를 보낼 목적지 — 응답이 나가는 transport(=요청이 온 transport)를
            //   포트와 함께 싣는다. 빠뜨리면 상대가 UDP 로 해석해 TLS 포트에 평문을 보낸다.
            CSipFrom clsSelfContact;
            CspAddressing::FillSelfContact( clsSelfContact, pclsMessage->m_eTransport );
            const int iListenerId = GetCurrentInboundListenerId();
            if ( iListenerId > 0 ) clsSelfContact.m_clsUri.m_strHost = CspAddressing::GetLocalSipAddress( iListenerId );
            pclsUnsubResp->m_clsContactList.push_back( clsSelfContact );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsUnsubResp );
        }

        SubscriptionInfo subInfo;
        if ( gclsSubscriptionManager.GetSubscriptionByCallId( strSubCallId, subInfo ) ) {
            SendTerminatedNotify( subInfo );
        }

        gclsSubscriptionManager.RemoveSubscription( strSubCallId );

        // conference 구독 해지는 제휴와 무관하다 — 아래 참조.
        if ( bAffiliation && strEventType == "affiliation" && gclsDbManager.IsConnected() ) {
            gclsDbManager.RemoveAffiliation( strReqUriUser, strFromId, strContactUri );
            CLog::Print( LOG_INFO, "[Affiliation] de-affiliate user=%s group=%s", strFromId.c_str(),
                         strReqUriUser.c_str() );
        }
        return true;
    }

    // 제휴(affiliation) 상태 변경은 PUBLISH(TS 24.379 §9) 와 presence 구독 경로만 수행한다.
    //   conference 구독(RFC 4575, Event: conference)은 그룹 자원을 Request-URI 로 쓰지만
    //   제휴와 무관한 로스터 열람이다 — 여기서 제휴를 건드리면 그룹콜 이탈 시의 구독 해지가
    //   제휴까지 지워 fan-out 이 조용히 끊긴다.
    if ( bAffiliation && strEventType == "affiliation" && gclsDbManager.IsConnected() ) {
        if ( gclsDbManager.InsertAffiliation( strReqUriUser, strFromId, strContactUri, iExpires ) ) {
            CLog::Print( LOG_INFO, "[Affiliation] affiliate user=%s group=%s expires=%d", strFromId.c_str(),
                         strReqUriUser.c_str(), iExpires );
        } else {
            CLog::Print( LOG_ERROR, "[Affiliation] affiliate 미기록 user=%s group=%s expires=%d — DB 미반영",
                         strFromId.c_str(), strReqUriUser.c_str(), iExpires );
        }
    }

    // 갱신(refresh) SUBSCRIBE 는 같은 dialog 안에서 오므로 To tag 를 새로 만들면 안 된다 —
    //   새 tag 를 200 OK/후속 NOTIFY 에 실으면 구독자 dialog 와 remote tag 가 어긋나
    //   NOTIFY 가 481 로 거절되고 구독이 죽는다(RFC 6665 §4.1.2.2).
    char szToTag[64];
    std::string strReqToTag;
    pclsMessage->m_clsTo.SelectParam( SIP_TAG, strReqToTag );
    if ( bRefresh && !clsPrev.strToTag.empty() ) {
        snprintf( szToTag, sizeof( szToTag ), "%s", clsPrev.strToTag.c_str() );
    } else if ( !strReqToTag.empty() ) {
        // 상태 없는(재기동 후) in-dialog refresh — 구독자 dialog 의 remote tag(요청 To tag)를
        //   그대로 승계해야 후속 NOTIFY 가 구독자 스택에서 481(dialog 불일치)로 거절되지 않는다.
        snprintf( szToTag, sizeof( szToTag ), "%s", strReqToTag.c_str() );
    } else {
        SipMakeTag( szToTag, sizeof( szToTag ) );
    }

    SubscriptionInfo info;
    info.strUserId = strFromId;
    info.strSubscriberUri = szFromBuf;
    info.strFromTag = strFromTag;
    info.strToTag = szToTag;
    info.strContact = strContactUri;
    info.strCallId = strSubCallId;
    info.strEventType = strEventType;
    info.strResourceId = strReqUriUser;  // 자원 기준 조회용 (conference = 그룹 ID)
    info.iExpires = ( iExpires > 0 ) ? iExpires : 3600;
    info.tStartTime = time( NULL );
    info.iNotifySeq = 1;
    // NOTIFY 송신 시 SUBSCRIBE 수신 listener 의 IP/Port 를 Via/Contact 자기 주소로 사용
    info.iInboundListenerId = GetCurrentInboundListenerId();

    gclsSubscriptionManager.AddSubscription( strReqUri, info );

    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( 200 );
    if ( pclsResponse ) {
        // dialog 식별용 To tag 를 구독에 저장한 szToTag 로 교체
        //   (CreateResponseWithToTag 가 생성한 tag 위에 Insert 하면 tag 중복)
        if ( pclsResponse->m_clsTo.UpdateParam( SIP_TAG, szToTag ) == false )
            pclsResponse->m_clsTo.InsertParam( SIP_TAG, szToTag );
        pclsResponse->AddHeader( "Allow", SIP_ALLOW_METHODS );
        // RFC 6665 §4.2.1.1: SUBSCRIBE 2xx 는 부여한 구독시간 Expires 필수
        pclsResponse->AddHeader( "Expires", info.iExpires );
        pclsResponse->AddHeader( "Supported", "path,100rel,precondition" );
        // dialog Contact = 서버 자기 주소 (user 없음 — 실망 형태)
        {
            // 단말이 구독 갱신·해지를 보낼 목적지 — 응답이 나가는 transport(=요청이 온 transport)를
            //   포트와 함께 싣는다. 빠뜨리면 상대가 UDP 로 해석해 TLS 포트에 평문을 보낸다.
            CSipFrom clsSelfContact;
            CspAddressing::FillSelfContact( clsSelfContact, pclsMessage->m_eTransport );
            const int iListenerId = GetCurrentInboundListenerId();
            if ( iListenerId > 0 ) clsSelfContact.m_clsUri.m_strHost = CspAddressing::GetLocalSipAddress( iListenerId );
            pclsResponse->m_clsContactList.push_back( clsSelfContact );
        }
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
            const int iAffExpires = ( iExpires > 0 ) ? iExpires : 3600;
            // 기록 결과를 반드시 판정한다 — 종전엔 반환값을 버리고 성공 로그를 무조건 남겨,
            //   DB 에 아무것도 안 쓰였는데도 로그만 "affiliate" 로 보이는 침묵 실패가 가능했다
            //   (그룹 미발견 시 INSERT..SELECT 는 에러 없이 0행).
            if ( gclsDbManager.InsertAffiliation( strReqUriUser, strFromId, strContactUri, iAffExpires ) ) {
                CLog::Print( LOG_INFO, "[Affiliation/PUBLISH] affiliate user=%s group=%s expires=%d",
                             strFromId.c_str(), strReqUriUser.c_str(), iAffExpires );
            } else {
                CLog::Print( LOG_ERROR,
                             "[Affiliation/PUBLISH] affiliate 미기록 user=%s group=%s expires=%d "
                             "— DB 미반영(그룹 미발견 또는 쿼리 실패). fan-out 대상에서 누락된다.",
                             strFromId.c_str(), strReqUriUser.c_str(), iAffExpires );
            }
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
