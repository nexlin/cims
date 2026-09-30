#include "PttAsModule.h"

#include "CallDir.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "ModuleDispatcher.h"
#include "SipServerSetup.h"
#include "UserMap.h"

bool CPttAsModule::IsEnabled() const {
    return gclsSetup.m_bRolePttAs;
}

// Phase 3 에서 PTT 그룹콜 로직 이동 예정
// 현재는 ModuleDispatcher가 기존 로직을 직접 호출

EModuleRouteResult CPttAsModule::OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                                 CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    return E_ROUTE_PASS;
}

bool CPttAsModule::OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    return false;
}
bool CPttAsModule::OnCallEnd( const char *pszCallId, int iSipStatus ) {
    return false;
}

namespace {

    /** 제어 기능이 제휴 멤버에게 보내는 경보 통지 mcptt-info (TS 24.379 §6.3.3.1.11·§6.3.3.1.12·§12.1.3.2 2)c)).
     *  요소 순서 = mcptt-ParamsType 시퀀스. */
    std::string _BuildAlertNotification( const std::string &strMemberId, const std::string &strCallingUserId,
                                         const std::string &strGroupId, bool bActivate,
                                         const std::string &strOriginatedBy, bool bEmergencyCancel ) {
        std::string s =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\">\r\n"
            "  <mcptt-Params>\r\n";
        s += McpttInfoUri( "mcptt-request-uri", "tel:" + strMemberId );           // §6.3.3.1.11 7) 대상 사용자 MCPTT ID
        s += McpttInfoUri( "mcptt-calling-user-id", "tel:" + strCallingUserId );  // §12.1.3.2 2)c)ii) · §12.1.2.1 9)
        s += McpttInfoUri( "mcptt-calling-group-id", "tel:" + strGroupId );       // §6.3.3.1.11 8)
        if ( bEmergencyCancel ) s += McpttInfoBool( "emergency-ind", false );     // §12.1.3.2 2)d)iv)E)
        s += McpttInfoBool( "alert-ind", bActivate );                             // §6.3.3.1.12 1) · §12.1.3.2 2)c)iv)
        if ( !strOriginatedBy.empty() ) s += McpttInfoUri( "originated-by", strOriginatedBy );  // §12.1.3.2 2)c)iii)
        s += "  </mcptt-Params>\r\n"
             "</mcpttinfo>\r\n";
        return s;
    }

    /** multipart 본문에서 지정 Content-Type 파트의 원문(헤더 제외)을 꺼낸다. 없으면 빈 문자열. */
    std::string _MimePart( CSipMessage *pclsMessage, const char *pszSubType ) {
        if ( !pclsMessage->m_clsContentType.IsEqual( "multipart", "mixed" ) ) return std::string();
        std::string strBoundary;
        if ( !pclsMessage->m_clsContentType.SelectParam( "boundary", strBoundary ) || strBoundary.empty() )
            return std::string();
        if ( strBoundary.size() >= 2 && strBoundary.front() == '"' )
            strBoundary = strBoundary.substr( 1, strBoundary.size() - 2 );
        const std::string &b = pclsMessage->m_strBody;
        const std::string strDelim = "--" + strBoundary;
        size_t p = 0;
        while ( ( p = b.find( strDelim, p ) ) != std::string::npos ) {
            p += strDelim.size();
            if ( b.compare( p, 2, "--" ) == 0 ) break;  // 종결 구분자
            const size_t hdrEnd = b.find( "\r\n\r\n", p );
            if ( hdrEnd == std::string::npos ) break;
            const std::string strHdr = b.substr( p, hdrEnd - p );
            const size_t next = b.find( "\r\n" + strDelim, hdrEnd + 4 );
            if ( next == std::string::npos ) break;
            std::string strLower = strHdr;
            for ( auto &c : strLower ) c = (char)tolower( (unsigned char)c );
            if ( strLower.find( pszSubType ) != std::string::npos )
                return b.substr( hdrEnd + 4, next - ( hdrEnd + 4 ) );
            p = next + 2;
        }
        return std::string();
    }

}  // namespace

int CPttAsModule::OnEmergencyAlert( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage,
                                    const CMcpttInfo &clsMi ) {
    const bool bActivate = clsMi.bAlert;  // true=경보, false=경보 취소
    const std::string strFrom = pszFrom ? pszFrom : "";

    // 대상 그룹 — 본문 <mcptt-request-uri>(§12.1.1.1 4)a)), Request-URI 가 그룹이면 그 그룹(전환기).
    std::string strGroupId;
    if ( pszTo && gclsGroupMap.Contains( pszTo ) ) {
        strGroupId = pszTo;
    } else {
        const std::string strBody = McpttBareId( clsMi.strRequestUri );
        if ( !strBody.empty() && gclsGroupMap.Contains( strBody.c_str() ) ) strGroupId = strBody;
    }
    CspPttGroup clsGroup;
    const bool bHaveGroup = !strGroupId.empty() && gclsGroupMap.Select( strGroupId.c_str(), clsGroup );
    if ( !bHaveGroup ) {
        // 경보 대상은 그룹이다(§12.1.1.1) — 알 수 없는 그룹이면 404 (TS 24.229 §5.7.1.4 미지 PSI 대상과 같은 구분)
        CLog::Print( LOG_INFO, "PTT-AS: emergency alert from(%s) R-URI(%s) request-uri(%s) — unknown group → 404",
                     strFrom.c_str(), pszTo ? pszTo : "", clsMi.strRequestUri.c_str() );
        return SIP_NOT_FOUND;
    }

    // 인가 — 그룹 allow-MCPTT-emergency-alert(TS 24.481) AND 사용자 allow-activate-emergency-alert(TS 24.484,
    //   §6.3.3.1.13.1). 미인가 경보는 전파하지 않는다. 취소는 사용자 게이트 비대상(잔존 경보 정리 경로 보존).
    bool bAllowed = clsGroup._emergencyAlert;
    if ( bAllowed && bActivate ) {
        CspUserProfile clsProf;
        if ( gclsDbManager.SelectUserProfile( strFrom, clsProf ) >= 0 && !clsProf.m_bAllowEmergencyAlert ) {
            bAllowed = false;
            CLog::Print( LOG_INFO, "PTT-AS: alert by(%s) not authorised (user profile) → drop", strFrom.c_str() );
        }
    }

    const char *pszEvt = bActivate ? "alert_sent" : "alert_cancelled";
    int iFanout = 0;
    if ( bAllowed ) {
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, pszEvt,
                                     std::string( "{\"actor\":\"" ) + CCallDir::JsonEsc( strFrom ) +
                                         "\",\"target\":\"" + CCallDir::JsonEsc( strGroupId ) + "\"}" );

        // 제3자 취소 — <originated-by> 는 그대로 옮긴다(§12.1.3.2 2)c)iii)). 경보 취소에 동봉된 그룹 긴급 해제
        //   (<emergency-ind>false) 는 제휴 멤버 통지에 emergency-ind=false 로 싣는다(§12.1.3.2 2)d)iv)).
        const bool bEmergencyCancel = !bActivate && clsMi.bHasEmergencyInd && !clsMi.bEmergency;
        const std::string strOriginatedBy = bActivate ? std::string() : clsMi.strOriginatedBy;
        // 발신자 MCPTT ID — 참여 기능이 서빙 사용자로 정한다(§12.1.2.1 9)), 본문 값은 쓰지 않는다.
        const std::string &strCallingUserId = strFrom;

        // 위치 정보 파트는 옮긴다(§6.3.3.1.12 4)).
        const std::string strLocation = _MimePart( pclsMessage, "mcptt-location-info" );

        // §6.3.3.1.11 2)·3)·6) — MCPTT feature tag·ICSI Accept-Contact, P-Asserted-Service(RFC 6050 §4.1 헤더 이름)
        const std::vector<std::pair<std::string, std::string>> vecHeaders = {
            { "Accept-Contact", "*;+g.3gpp.mcptt;require;explicit" },
            { "Accept-Contact", "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit" },
            { "P-Asserted-Service", "urn:urn-7:3gpp-service.ims.icsi.mcptt" } };

        for ( const auto &pUser : clsGroup._pusers ) {
            if ( !pUser || pUser->_id == strFrom ) continue;
            // 제휴 멤버만(§12.1.3.1·§12.1.3.2 2)c) "affiliated members")
            if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
                 !gclsDbManager.IsAffiliated( strGroupId, pUser->_id ) )
                continue;
            CUserInfo clsMemInfo;
            if ( !gclsUserMap.Select( pUser->_id.c_str(), clsMemInfo ) ) continue;
            CSipCallRoute clsMemRoute;
            clsMemInfo.GetCallRoute( clsMemRoute );

            const std::string strInfo = _BuildAlertNotification( pUser->_id, strCallingUserId, strGroupId, bActivate,
                                                                 strOriginatedBy, bEmergencyCancel );
            std::string strBody, strContentType;
            if ( strLocation.empty() ) {
                strBody = strInfo;
                strContentType = "application/vnd.3gpp.mcptt-info+xml";
            } else {
                const std::string strBoundary = "mcptt-alert-boundary";
                strBody = "--" + strBoundary + "\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n" +
                          strInfo + "\r\n--" + strBoundary +
                          "\r\nContent-Type: application/vnd.3gpp.mcptt-location-info+xml\r\n\r\n" + strLocation +
                          "\r\n--" + strBoundary + "--\r\n";
                strContentType = "multipart/mixed;boundary=" + strBoundary;
            }
            if ( gclsUserAgent.SendSms( strFrom.c_str(), pUser->_id.c_str(), strBody.c_str(), &clsMemRoute,
                                        strContentType.c_str(), &vecHeaders ) )
                iFanout++;
        }
    }
    CLog::Print( LOG_INFO, "PTT-AS: MCPTT emergency %s from(%s) group(%s) R-URI(%s) fanout=%d", pszEvt, strFrom.c_str(),
                 strGroupId.c_str(), pszTo ? pszTo : "", iFanout );
    return SIP_OK;  // §12.1.3.1 iii)·§12.1.3.2 2)e) 200 OK
}
