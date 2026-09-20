#include "CspTrunkRegistrar.h"

#include <strings.h>

#include <cstdlib>
#include <ctime>

#include "CscfModule.h"
#include "FmReporter.h"
#include "Log.h"
#include "NonceMap.h"
#include "SimpleJson.h"
#include "SipMd5.h"
#include "SipMessage.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "SipStatusCode.h"
#include "SipTransport.h"

CCspTrunkRegistrar gclsTrunkRegistrar;

namespace {
    bool _send( CSipMessage *pclsMessage, int iStatus ) {
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( iStatus );
        if ( pclsResponse == NULL ) return false;
        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
        return true;
    }
    std::string _firstContact( CSipMessage *pclsMessage ) {
        if ( pclsMessage->m_clsContactList.empty() ) return "";
        char szUri[512];
        CSipFrom &c = pclsMessage->m_clsContactList.front();
        if ( c.m_clsUri.ToString( szUri, sizeof( szUri ) ) <= 0 ) return "";
        return szUri;
    }
}  // namespace

RouteConfig CCspTrunkRegistrar::_account( const std::string &strUser, const std::string &strLocalNodeName ) const {
    if ( strUser.empty() ) return RouteConfig();
    RouteConfig anyNode;
    for ( const RouteConfig &c : gclsRouteMap.GetAll() ) {
        if ( !c.enabled || !c.IsTrunkAccount() || c.auth_user != strUser ) continue;
        if ( !strLocalNodeName.empty() && c.local_node_ref == strLocalNodeName )
            return c;  // 받은 접속점의 Route 가 정답
        if ( !anyNode.IsValid() ) anyNode = c;
    }
    return anyNode;  // 접속점 미상(레거시 단일 리스너)이면 계정이 같은 첫 Route
}

void CCspTrunkRegistrar::_setAlive( const RouteConfig &rc, bool bAlive, const char *pszWhy ) {
    bool bChanged = false;
    gclsRouteMap.SetAlive( rc.name, bAlive, bChanged );
    if ( !bChanged || !gclsFmReporter.IsEnabled() ) return;
    const std::string strMo = gclsFmReporter.Node() + "/csp/peer/" + rc.remote_node_ref;
    if ( bAlive ) {
        gclsFmReporter.AlarmClose( "A-COM-003", strMo );
    } else {
        SimpleJson::JsonNode nodeParams;
        nodeParams.Set( "peer", rc.remote_node_ref.c_str() );
        nodeParams.Set( "route", rc.name.c_str() );
        nodeParams.Set( "fails", 0 );
        nodeParams.Set( "reason", pszWhy );
        gclsFmReporter.AlarmOpen( "A-COM-003", strMo, nodeParams );
    }
}

void CCspTrunkRegistrar::_bind( const RouteConfig &rc, const TrunkBinding &tb ) {
    bool bNew;
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        bNew = m_byRoute.find( rc.name ) == m_byRoute.end();
        m_byRoute[rc.name] = tb;
    }
    CLog::Print( LOG_SYSTEM, "TrunkRegistrar: %s account=%s route=%s peer=%s at %s:%d/%s contact=%s expires=%d",
                 bNew ? "registered" : "refreshed", tb.account.c_str(), rc.name.c_str(), rc.remote_node_ref.c_str(),
                 tb.ip.c_str(), tb.port, tb.transport.c_str(), tb.contact_uri.c_str(), tb.expires );
    _setAlive( rc, true, "trunk registered" );
}

void CCspTrunkRegistrar::_unbind( const RouteConfig &rc, const char *pszWhy ) {
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        if ( m_byRoute.erase( rc.name ) == 0 ) return;
    }
    CLog::Print( LOG_SYSTEM, "TrunkRegistrar: %s account=%s route=%s peer=%s", pszWhy, rc.auth_user.c_str(),
                 rc.name.c_str(), rc.remote_node_ref.c_str() );
    _setAlive( rc, false, pszWhy );
}

bool CCspTrunkRegistrar::HandleRegister( CSipMessage *pclsMessage, const std::string &strLocalNodeName,
                                         const std::string &strRealmFallback ) {
    // AoR = To(RFC 3261 §10.2) — 트렁크 계정. From 도 같은 계정이어야 한다(제3자 등록 없음).
    const std::string &strTo = pclsMessage->m_clsTo.m_clsUri.m_strUser;
    RouteConfig rc = _account( strTo, strLocalNodeName );
    if ( !rc.IsValid() ) return false;
    if ( pclsMessage->m_clsFrom.m_clsUri.m_strUser != strTo ) {
        CLog::Print( LOG_INFO, "TrunkRegistrar: account=%s From(%s) != To → 403 (third-party registration)",
                     strTo.c_str(), pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str() );
        return _send( pclsMessage, SIP_FORBIDDEN );
    }
    const std::string strRealm = rc.auth_realm.empty() ? strRealmFallback : rc.auth_realm;

    SIP_CREDENTIAL_LIST::iterator itCL = pclsMessage->m_clsAuthorizationList.begin();
    const bool bEmptyPreAuth = ( itCL != pclsMessage->m_clsAuthorizationList.end() && itCL->m_strNonce.empty() &&
                                 itCL->m_strResponse.empty() );
    if ( itCL == pclsMessage->m_clsAuthorizationList.end() || bEmptyPreAuth ) {
        CLog::Print( LOG_INFO, "TrunkRegistrar: challenge account=%s route=%s realm=%s src=%s:%d", strTo.c_str(),
                     rc.name.c_str(), strRealm.c_str(), pclsMessage->m_strClientIp.c_str(),
                     pclsMessage->m_iClientPort );
        return CCscfModule::SendUnAuthorizedResponse( pclsMessage, strRealm, false );
    }
    CSipCredential &cred = *itCL;
    if ( cred.m_strUserName != rc.auth_user ) {
        CLog::Print( LOG_INFO, "TrunkRegistrar: account=%s username(%s) mismatch → 403", strTo.c_str(),
                     cred.m_strUserName.c_str() );
        return _send( pclsMessage, SIP_FORBIDDEN );
    }
    const bool bQop = !cred.m_strQop.empty();
    CNonceInfo clsNonce;
    if ( !gclsNonceMap.SelectInfo( cred.m_strNonce.c_str(), clsNonce, !bQop ) ) {
        // 모르는/만료 nonce — stale 재챌린지 (F-07)
        return CCscfModule::SendUnAuthorizedResponse( pclsMessage, strRealm, true );
    }
    if ( clsNonce.m_bAka ) {
        CLog::Print( LOG_ERROR, "TrunkRegistrar: account=%s answered an AKA nonce → 403", strTo.c_str() );
        return _send( pclsMessage, SIP_FORBIDDEN );
    }
    // H(A1) — 저장 H(A1) 우선(auth_realm 과 같은 realm 으로 만든 값이어야 한다), 없으면 평문으로 계산(realm = 단말이
    // 답한 realm)
    std::string strHa1 = rc.auth_ha1;
    if ( strHa1.empty() && !rc.auth_password.empty() ) {
        char szMd5[33];
        const std::string strA1 =
            rc.auth_user + ":" + ( cred.m_strRealm.empty() ? strRealm : cred.m_strRealm ) + ":" + rc.auth_password;
        SipMd5String( strA1.c_str(), szMd5 );
        strHa1 = szMd5;
    }
    if ( strHa1.empty() ) {
        CLog::Print( LOG_ERROR, "TrunkRegistrar: account=%s route=%s has no auth_ha1/auth_password → 403",
                     strTo.c_str(), rc.name.c_str() );
        return _send( pclsMessage, SIP_FORBIDDEN );
    }
    if ( !CCscfModule::CheckAuthorizationResponse( strHa1.c_str(), cred.m_strNonce.c_str(), cred.m_strUri.c_str(),
                                                   cred.m_strResponse.c_str(), pclsMessage->m_strSipMethod.c_str(),
                                                   bQop ? cred.m_strQop.c_str() : NULL, cred.m_strNonceCount.c_str(),
                                                   cred.m_strCnonce.c_str() ) ) {
        CLog::Print( LOG_INFO, "TrunkRegistrar: account=%s digest mismatch → 403 src=%s:%d", strTo.c_str(),
                     pclsMessage->m_strClientIp.c_str(), pclsMessage->m_iClientPort );
        return _send( pclsMessage, SIP_FORBIDDEN );
    }
    if ( bQop && !gclsNonceMap.CheckAndUpdateNc( cred.m_strNonce.c_str(),
                                                 (unsigned int)strtoul( cred.m_strNonceCount.c_str(), NULL, 16 ) ) ) {
        return CCscfModule::SendUnAuthorizedResponse( pclsMessage, strRealm,
                                                      true );  // nc 재사용(replay) — stale 재챌린지
    }

    // 수명 (RFC 3261 §10.2.1.1: Contact ;expires > Expires 헤더). 없으면 Route register_expires, 0 = 해제.
    uint32_t uiReq = 0;
    const ESipExpiresResult eReq = pclsMessage->GetRegisterExpires( uiReq );
    if ( eReq == E_SIP_EXPIRES_INVALID ) return _send( pclsMessage, SIP_BAD_REQUEST );
    if ( eReq == E_SIP_EXPIRES_VALID && uiReq == 0 ) {
        _unbind( rc, "trunk unregistered" );
        return _send( pclsMessage, SIP_OK );
    }
    int iExpires = eReq == E_SIP_EXPIRES_VALID ? (int)( uiReq > 0x7FFFFFFF ? 0x7FFFFFFF : uiReq )
                                               : ( rc.register_expires > 0 ? rc.register_expires : 3600 );
    if ( gclsSetup.m_iMinRegisterTimeout > 0 && iExpires < gclsSetup.m_iMinRegisterTimeout ) {
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_INTERVAL_TOO_BRIEF );
        if ( pclsResponse == NULL ) return false;
        pclsResponse->AddHeader( "Min-Expires", gclsSetup.m_iMinRegisterTimeout );
        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
        return true;
    }

    TrunkBinding tb;
    tb.route_name = rc.name;
    tb.remote_node_ref = rc.remote_node_ref;
    tb.account = rc.auth_user;
    tb.contact_uri = _firstContact( pclsMessage );
    tb.ip = pclsMessage->m_strClientIp;
    tb.port = pclsMessage->m_iClientPort;
    tb.transport = SipGetTransport( pclsMessage->m_eTransport );
    tb.listener_id = pclsMessage->m_iListenerId;
    tb.registered_at = (long)time( NULL );
    tb.expires = iExpires;
    tb.expires_at = tb.registered_at + iExpires;
    _bind( rc, tb );

    CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_OK );
    if ( pclsResponse == NULL ) return false;
    char szExpires[16];
    snprintf( szExpires, sizeof( szExpires ), "%d", iExpires );
    for ( SIP_FROM_LIST::iterator it = pclsMessage->m_clsContactList.begin(); it != pclsMessage->m_clsContactList.end();
          ++it ) {
        CSipFrom clsContact = *it;
        if ( clsContact.UpdateParam( "expires", szExpires ) == false ) clsContact.InsertParam( "expires", szExpires );
        pclsResponse->m_clsContactList.push_back( clsContact );
    }
    pclsResponse->AddHeader( "Expires", iExpires );
    gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    return true;
}

bool CCspTrunkRegistrar::Get( const std::string &routeName, TrunkBinding &out ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byRoute.find( routeName );
    if ( it == m_byRoute.end() ) return false;
    out = it->second;
    return true;
}

RouteConfig CCspTrunkRegistrar::FindBySource( const std::string &localName, const std::string &srcIp, int srcPort,
                                              const std::string &transport ) const {
    if ( srcIp.empty() ) return RouteConfig();
    std::string strRoute;
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        for ( const auto &kv : m_byRoute ) {
            const TrunkBinding &tb = kv.second;
            if ( tb.ip != srcIp ) continue;
            if ( !transport.empty() && strcasecmp( tb.transport.c_str(), transport.c_str() ) != 0 ) continue;
            if ( srcPort > 0 && tb.port != srcPort ) continue;
            strRoute = kv.first;
            break;
        }
    }
    if ( strRoute.empty() ) return RouteConfig();
    RouteConfig rc = gclsRouteMap.GetByName( strRoute );
    if ( !rc.IsValid() || !rc.enabled ) return RouteConfig();
    if ( !localName.empty() && rc.local_node_ref != localName ) return RouteConfig();
    return rc;
}

bool CCspTrunkRegistrar::IsRegisteredSource( const std::string &routeName, const std::string &srcIp, int srcPort,
                                             const std::string &transport ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byRoute.find( routeName );
    if ( it == m_byRoute.end() ) return false;
    const TrunkBinding &tb = it->second;
    if ( tb.ip != srcIp ) return false;
    if ( !transport.empty() && strcasecmp( tb.transport.c_str(), transport.c_str() ) != 0 ) return false;
    return srcPort <= 0 || tb.port == srcPort;
}

void CCspTrunkRegistrar::Tick( long now ) {
    std::vector<std::string> vecExpired;
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        for ( const auto &kv : m_byRoute )
            if ( kv.second.expires_at <= now ) vecExpired.push_back( kv.first );
    }
    for ( const std::string &r : vecExpired ) {
        RouteConfig rc = gclsRouteMap.GetByName( r );
        if ( rc.IsValid() ) {
            _unbind( rc, "trunk registration expired" );
        } else {
            std::lock_guard<std::mutex> lk( m_mutex );
            m_byRoute.erase( r );
        }
    }
    // 바인딩 없는 등록형 트렁크 Route 는 dead — 기동 직후(RouteMap 기본 alive)·설정 재적재·계정 전환 뒤를 여기서 맞춘다
    for ( const RouteConfig &c : gclsRouteMap.GetAll() ) {
        if ( !c.enabled || !c.IsTrunkAccount() ) continue;
        bool bBound;
        {
            std::lock_guard<std::mutex> lk( m_mutex );
            bBound = m_byRoute.find( c.name ) != m_byRoute.end();
        }
        if ( !bBound && gclsRouteMap.IsAlive( c.name ) ) _setAlive( c, false, "trunk not registered" );
    }
}

std::vector<TrunkBinding> CCspTrunkRegistrar::GetAll() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    std::vector<TrunkBinding> out;
    for ( const auto &kv : m_byRoute ) out.push_back( kv.second );
    return out;
}
