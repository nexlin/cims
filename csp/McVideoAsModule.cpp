#include "McVideoAsModule.h"

#include <strings.h>

#include "Log.h"
#include "McVideoInfo.h"
#include "ModuleDispatcher.h"
#include "SipServerSetup.h"

bool CMcVideoAsModule::IsEnabled() const {
    return gclsSetup.m_bRoleMcVideo;
}

bool CMcVideoAsModule::IsMcVideoRequest( CSipMessage *pclsMessage ) {
    if ( !pclsMessage ) return false;
    std::string strServices;
    for ( const char *pszName : { "P-Asserted-Service", "P-Preferred-Service" } ) {
        CSipHeader *pH = pclsMessage->GetHeader( pszName );
        if ( pH ) strServices += pH->m_strValue + ",";
    }
    // Accept-Contact 은 여러 줄이다 — 퍼센트 표기 icsi-ref 도 McVideoIcsiIn 이 본다(TS 24.281 §9.2.2.4.1.1 2)).
    for ( const auto &clsHdr : pclsMessage->m_clsHeaderList )
        if ( strcasecmp( clsHdr.m_strName.c_str(), "Accept-Contact" ) == 0 ) strServices += clsHdr.m_strValue + ",";
    CSipHeader *pAccept = pclsMessage->GetHeader( "Accept" );
    const std::string strCtype =
        pclsMessage->m_clsContentType.m_strType + "/" + pclsMessage->m_clsContentType.m_strSubType;
    return McVideoRequestIndicated( strServices, pAccept ? pAccept->m_strValue : std::string(), pclsMessage->m_strBody,
                                    strCtype );
}

EModuleRouteResult CMcVideoAsModule::OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                                     CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    (void)pclsRtp;
    (void)pclsMessage;
    if ( !IsEnabled() ) {
        // MCVideo 를 내지 않는 사이트 — 참여 MCVideo 기능 PSI 미할당(TS 24.281 §6.3.7.1)
        CLog::Print( LOG_INFO, "MCVIDEO-AS: INVITE from(%s) to(%s) — MCVideo 미제공(Roles.MCVIDEO off) → 404",
                     pszFrom ? pszFrom : "", pszTo ? pszTo : "" );
        gclsDispatcher.StopCall( pszCallId, SIP_NOT_FOUND );
        return E_ROUTE_HANDLED;
    }
    // 그룹 호 처리(McVideoCallService — chat·prearranged 개시·합류·해제, mcvideo_dev_plan.md A10)가 아직 없다. MCPTT
    // 호로 흘리지 않고
    //   일시 불가로 끝낸다(RFC 3261 §21.4.18).
    CLog::Print( LOG_INFO, "MCVIDEO-AS: INVITE from(%s) to(%s) — MCVideo 그룹 호 미구현 → 480", pszFrom ? pszFrom : "",
                 pszTo ? pszTo : "" );
    gclsDispatcher.StopCall( pszCallId, SIP_TEMPORARILY_UNAVAILABLE );
    return E_ROUTE_HANDLED;
}
