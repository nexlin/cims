#include "PttAsModule.h"

#include "CallDir.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupCallService.h"
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
                                         const std::string &strOriginatedBy ) {
        McpttIndicators clsInd;
        clsInd.iAlert = bActivate ? 1 : 0;         // §6.3.3.1.12 1) · §12.1.3.2 2)c)iv)
        clsInd.strOriginatedBy = strOriginatedBy;  // §12.1.3.2 2)c)iii)
        std::string s =
            McpttInfoUri( "mcptt-request-uri", "tel:" + strMemberId );            // §6.3.3.1.11 7) 대상 사용자 MCPTT ID
        s += McpttInfoUri( "mcptt-calling-user-id", "tel:" + strCallingUserId );  // §12.1.3.2 2)c)ii) · §12.1.2.1 9)
        s += McpttInfoUri( "mcptt-calling-group-id", "tel:" + strGroupId );       // §6.3.3.1.11 8)
        s += McpttIndicatorElems( clsInd ) + McpttIndicatorOriginatedBy( clsInd );
        return McpttInfoDocument( s );
    }

    /** 거절 응답 + mcptt-info 본문 (§12.1.3.1 4)a)·§12.1.3.2 1)a)·§12.1.3.3 1)a)). psip 은 응답을 보내지 않게 0 을
     * 돌려준다. */
    int _RejectWithInfo( CSipMessage *pclsMessage, int iStatus, const McpttIndicators &clsInd ) {
        CSipMessage *pclsResp = pclsMessage->CreateResponseWithToTag( iStatus );
        if ( pclsResp ) {
            pclsResp->m_strBody = McpttInfoDocument( McpttIndicatorElems( clsInd ) );
            pclsResp->m_iContentLength = (int)pclsResp->m_strBody.size();
            pclsResp->m_clsContentType.Set( "application", "vnd.3gpp.mcptt-info+xml" );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResp );
        }
        return 0;
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

    const bool bAlertTrue = clsMi.bHasAlertInd && clsMi.bAlert;
    const bool bAlertFalse = clsMi.bHasAlertInd && !clsMi.bAlert;
    const bool bEmgFalse = clsMi.bHasEmergencyInd && !clsMi.bEmergency;

    // ── 경보 발령 (§12.1.3.1 4)) — 인가 = 그룹 allow-MCPTT-emergency-alert(TS 24.481) ∧ 사용자
    // allow-activate-emergency-
    //    alert(TS 24.484, §6.3.3.1.13.1). 미인가는 403 + alert-ind false (4)a)). ──
    if ( bAlertTrue ) {
        if ( !gclsGroupCallService.IsAlertActivateAuthorized( clsGroup, strFrom ) ) {
            CLog::Print( LOG_INFO, "PTT-AS: alert by(%s) group(%s) not authorised → 403", strFrom.c_str(),
                         strGroupId.c_str() );
            McpttIndicators b;
            b.iAlert = 0;
            return _RejectWithInfo( pclsMessage, SIP_FORBIDDEN, b );
        }
        gclsGroupCallService.SetAlertOutstanding( strGroupId, strFrom, true );  // 4)b)iii)A)
        const int iFanout = FanoutAlert( strFrom, strGroupId, clsGroup, pclsMessage, true, "" );
        CLog::Print( LOG_INFO, "PTT-AS: MCPTT emergency alert_sent from(%s) group(%s) R-URI(%s) fanout=%d",
                     strFrom.c_str(), strGroupId.c_str(), pszTo ? pszTo : "", iFanout );
        return SIP_OK;  // 4)b)iv)
    }

    // ── 취소 — 경보 취소(§12.1.3.2)·그룹 긴급 상태 해제(§12.1.3.2 1)b)·2)d)·호 없음 §12.1.3.3) ──
    //    경보 취소 인가 = allow-cancel-emergency-alert(§6.3.3.1.13.3), 긴급 해제 인가 = local policy(§6.3.3.1.13.4 —
    //    개시자 ∨ allow-cancel-group-emergency). 그룹 긴급 상태의 수명은 그룹 세션이라(편차 표) 호가 없으면 해제할
    //    상태도 없다 — 그때 해제 요청은 할 일이 없는 요청으로 200.
    const bool bStateEmg = gclsGroupCallService.GroupConditionOf( strGroupId ) >= 2;
    std::string strReason;
    const bool bEmgCancelOk =
        bEmgFalse && bStateEmg && gclsGroupCallService.IsEmergencyCancelAuthorized( strGroupId, strFrom, strReason );
    const std::string strAlertOwner =
        clsMi.strOriginatedBy.empty() ? strFrom : McpttBareId( clsMi.strOriginatedBy );  // §12.1.3.2 2)a)·b)

    if ( bAlertFalse ) {
        if ( !gclsGroupCallService.IsAlertCancelAuthorized( strFrom ) ) {
            if ( !bEmgCancelOk ) {
                // 1)a) — 403 + alert-ind true, 긴급 해제도 비인가면 emergency-ind true
                CLog::Print( LOG_INFO, "PTT-AS: alert cancel by(%s) group(%s) not authorised%s → 403", strFrom.c_str(),
                             strGroupId.c_str(), ( bEmgFalse && bStateEmg ) ? " (emergency cancel too)" : "" );
                McpttIndicators b;
                if ( bEmgFalse && bStateEmg ) b.iEmergency = 1;
                b.iAlert = 1;
                return _RejectWithInfo( pclsMessage, SIP_FORBIDDEN, b );
            }
            // 1)b) — 긴급 상태만 해제한다(경보는 남는다)
            gclsGroupCallService.CancelGroupEmergency( strGroupId, strFrom, McpttIndicators(), strFrom, "message" );
            return SIP_OK;
        }
        gclsGroupCallService.SetAlertOutstanding( strGroupId, strAlertOwner, false );  // 2)a)·b)
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.PttLogEvent( strGroupId, "alert_cancelled",
                                     std::string( "{\"actor\":\"" ) + CCallDir::JsonEsc( strFrom ) +
                                         "\",\"target\":\"" + CCallDir::JsonEsc( strGroupId ) + "\"}" );
        if ( bEmgCancelOk ) {
            // 2)d) — 긴급 해제와 함께: 참여 멤버 re-INVITE(alert-ind false + originated-by, §6.3.3.1.6 4)b)ii)),
            //   참여하지 않은 제휴 멤버 MESSAGE(alert-ind false + emergency-ind false, 2)d)iv))
            McpttIndicators a;
            a.iAlert = 0;
            a.strOriginatedBy = clsMi.strOriginatedBy;
            gclsGroupCallService.CancelGroupEmergency( strGroupId, strFrom, a, strFrom, "message" );
        } else {
            // 2)c) — 제휴 멤버 전원에 경보 취소 통지(긴급 해제가 없거나 비인가면 emergency-ind 는 싣지 않는다)
            const int iFanout = FanoutAlert( strFrom, strGroupId, clsGroup, pclsMessage, false, clsMi.strOriginatedBy );
            CLog::Print(
                LOG_INFO, "PTT-AS: MCPTT emergency alert_cancelled from(%s) group(%s) owner(%s) fanout=%d%s",
                strFrom.c_str(), strGroupId.c_str(), strAlertOwner.c_str(), iFanout,
                bEmgFalse ? ( bStateEmg ? " (emergency cancel not authorised)" : " (no emergency state)" ) : "" );
        }
        return SIP_OK;  // 2)e)·f)
    }

    // 긴급 상태 해제만 (§12.1.3.3 — alert-ind 없음)
    if ( !bStateEmg ) {
        CLog::Print( LOG_INFO, "PTT-AS: emergency cancel MESSAGE from(%s) group(%s) — 긴급 상태 없음(무동작)",
                     strFrom.c_str(), strGroupId.c_str() );
        return SIP_OK;
    }
    if ( !bEmgCancelOk ) {
        CLog::Print( LOG_INFO, "PTT-AS: emergency cancel MESSAGE from(%s) group(%s) not authorised (%s) → 403",
                     strFrom.c_str(), strGroupId.c_str(), strReason.c_str() );
        McpttIndicators b;  // 1)a)i)
        b.iEmergency = 1;
        return _RejectWithInfo( pclsMessage, SIP_FORBIDDEN, b );
    }
    gclsGroupCallService.CancelGroupEmergency( strGroupId, strFrom, McpttIndicators(), strFrom, "message" );  // 2)
    return SIP_OK;
}

int CPttAsModule::FanoutAlert( const std::string &strFrom, const std::string &strGroupId, const CspPttGroup &clsGroup,
                               CSipMessage *pclsMessage, bool bActivate, const std::string &strOriginatedBy ) {
    if ( bActivate && gclsCallDir.IsEnabled() )
        gclsCallDir.PttLogEvent( strGroupId, "alert_sent",
                                 std::string( "{\"actor\":\"" ) + CCallDir::JsonEsc( strFrom ) + "\",\"target\":\"" +
                                     CCallDir::JsonEsc( strGroupId ) + "\"}" );
    // 발신자 MCPTT ID — 참여 기능이 서빙 사용자로 정한다(§12.1.2.1 9)), 본문 값은 쓰지 않는다.
    const std::string &strCallingUserId = strFrom;
    // 위치 정보 파트는 옮긴다(§6.3.3.1.12 4)).
    const std::string strLocation = _MimePart( pclsMessage, "mcptt-location-info" );
    // §6.3.3.1.11 2)·3)·6) — MCPTT feature tag·ICSI Accept-Contact, P-Asserted-Service(RFC 6050 §4.1 헤더 이름)
    const std::vector<std::pair<std::string, std::string>> vecHeaders = {
        { "Accept-Contact", "*;+g.3gpp.mcptt;require;explicit" },
        { "Accept-Contact", "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit" },
        { "P-Asserted-Service", "urn:urn-7:3gpp-service.ims.icsi.mcptt" } };

    int iFanout = 0;
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

        const std::string strInfo =
            _BuildAlertNotification( pUser->_id, strCallingUserId, strGroupId, bActivate, strOriginatedBy );
        std::string strBody, strContentType;
        if ( strLocation.empty() ) {
            strBody = strInfo;
            strContentType = "application/vnd.3gpp.mcptt-info+xml";
        } else {
            const std::string strBoundary = "mcptt-alert-boundary";
            strBody = "--" + strBoundary + "\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n" + strInfo +
                      "\r\n--" + strBoundary +
                      "\r\nContent-Type: application/vnd.3gpp.mcptt-location-info+xml\r\n\r\n" + strLocation +
                      "\r\n--" + strBoundary + "--\r\n";
            strContentType = "multipart/mixed;boundary=" + strBoundary;
        }
        if ( gclsUserAgent.SendSms( strFrom.c_str(), pUser->_id.c_str(), strBody.c_str(), &clsMemRoute,
                                    strContentType.c_str(), &vecHeaders ) )
            iFanout++;
    }
    return iFanout;
}
