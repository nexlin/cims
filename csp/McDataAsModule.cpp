#include "McDataAsModule.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "CallDir.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "McDataCodec.h"
#include "McDataGates.h"
#include "McpttInfo.h"
#include "ModuleDispatcher.h"
#include "SipServerSetup.h"
#include "SipStatusCode.h"
#include "UserMap.h"

bool CMcDataAsModule::IsEnabled() const {
    return gclsSetup.m_bRoleMcData;
}

namespace {
    /** 응답 + Warning (TS 24.282 §4.9 — 399 <agent> "<code> <text>") */
    void _RejectWithWarning( CSipMessage *pclsMessage, int iStatus, int iWarn, const char *pszText ) {
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( iStatus );
        if ( !pclsResponse ) return;
        pclsResponse->AddHeader( "Warning",
                                 McpttWarning( iWarn, pszText, gclsServiceMap.GetDomainByKind( "ptt" ) ).c_str() );
        gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
    }
    std::string _TelOf( const std::string &strId ) {
        return strId.find( ':' ) == std::string::npos ? "tel:" + strId : strId;
    }
}  // namespace

/**
 * 그룹 SDS 처리 (TS 24.282 group standard SDS, controlling function).
 * 처리했으면(성공·거부 모두) true 이고, 보낼 최종 응답 코드를 iStatus 로 돌려준다.
 * Warning 헤더가 필요한 거부만 여기서 직접 응답하고 iStatus=0 으로 표시한다.
 */
bool CMcDataAsModule::OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage, int &iStatus ) {
    if ( pclsMessage == NULL ) return false;
    if ( OnDispositionNotification( pszFrom, pclsMessage, iStatus ) ) return true;
    if ( gclsGroupMap.Contains( pszTo ) == false ) return false;  // 1:1 → 디스패처 기본 경로

    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( pszTo, clsGroup ) == false ) return false;

    // Content-Type 원문 (boundary 포함) — fan-out 시 그대로 보존
    char szContentType[512];
    szContentType[0] = '\0';
    pclsMessage->m_clsContentType.ToString( szContentType, sizeof( szContentType ) );

    // MCData multipart 면 signalling TLV 파싱 (conv/msg id·disposition·payload 크기),
    // 평문(text/plain 등)이면 본문 전체를 payload 로 간주.
    CMcDataSdsInfo clsInfo;
    bool bMcData =
        McDataIsMultipartMixed( szContentType ) && McDataParseBody( szContentType, pclsMessage->m_strBody, clsInfo );
    bool bFd = bMcData && clsInfo.m_iMsgType == MCDATA_MSG_FD_SIGNALLING;
    int iPayloadSize = bMcData ? clsInfo.m_iPayloadSize : (int)pclsMessage->m_strBody.size();
    int iWarn = 0;

    // 2) mcdata-info·mcdata-signalling·mcdata-payload 가 없는 본문(text/plain·signalling 없는 multipart) — 규격은 403 +
    // 199.
    //   구버전 앱이 평문을 보내는 동안은 엄격 검사 스위치(Setup.Mcptt.StrictCheck, 기본 log) 아래 두고 본문 전체를
    //   payload 로 전달한다(결정 D5 — 앱이 규격형으로 바뀐 뒤 enforce).
    if ( !bMcData &&
         gclsSetup.McpttStrict( "199 expected MIME bodies not in the request",
                                std::string( "MESSAGE from " ) + pszFrom + " → " + pszTo + " ct=" + szContentType ) ) {
        _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, 199, McDataWarnText( 199 ) );
        iStatus = 0;
        return true;
    }

    // FD Payload 검사 (§10.2.4.4.2 6)·7)) — 그룹 판정(12))보다 먼저: Payload 하나 · FILEURL · 이 서버의 파일
    if ( bFd && McDataFdPayloadCheck( clsInfo, &iWarn ) != 0 ) {
        _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, iWarn, McDataWarnText( iWarn ) );
        iStatus = 0;
        return true;
    }

    // 게이트 0 — max-payload-size-sds-cplane-bytes (TS 24.484 서비스 설정, 0/미설정=무제한).
    //   초과 SDS 는 media plane(MSRP) 을 써야 한다 — participating 검사 (TS 24.282 §9.2.2.3.1 8)).
    if ( !bFd && gclsSetup.m_iMaxSdsCplaneBytes > 0 && iPayloadSize > gclsSetup.m_iMaxSdsCplaneBytes ) {
        CLog::Print( LOG_INFO, "McDataAs: payload %d > cplane max %d — reject 403 Warning 203 from(%s)", iPayloadSize,
                     gclsSetup.m_iMaxSdsCplaneBytes, pszFrom );
        CSipMessage *pclsResponse = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
        if ( pclsResponse ) {
            // TS 24.282 §4.9 — 399 <agent> "203 …" (RFC 3261 §20.43 warning-value)
            pclsResponse->AddHeader( "Warning",
                                     McpttWarning( 203, "message too large to send over signalling control plane",
                                                   gclsServiceMap.GetDomainByKind( "ptt" ) )
                                         .c_str() );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
        }
        iStatus = 0;  // Warning 헤더가 붙어야 해서 여기서 직접 보냈다.
        return true;
    }

    // 게이트 1·2·3 — allow_sds/allow_fd + 발신자 멤버십 + 발신자 제휴 (media plane 과 공용, McDataGates)
    int iGate = McDataGateCheck( clsGroup, pszFrom, bFd, &iWarn );
    if ( iGate != 0 ) {
        if ( iWarn > 0 ) {
            _RejectWithWarning( pclsMessage, iGate, iWarn, McDataWarnText( iWarn ) );
            iStatus = 0;
        } else {
            iStatus = iGate;
        }
        return true;
    }

    // 게이트 3 — mcdata-on-network-max-data-size-for-SDS (TS 24.481). FD 는 payload=URL 이라 제외
    //   (파일 크기 상한은 CSC 업로드 단에서 강제).
    if ( !bFd && clsGroup._maxSdsSize > 0 && iPayloadSize > clsGroup._maxSdsSize ) {
        // §9.2.2.4.2 6)i)iii) · §11.1 5) — 403 + 217 (그룹 SDS 크기)
        CLog::Print( LOG_INFO, "McDataAs: group(%s) payload %d > max %d — reject 403 217", pszTo, iPayloadSize,
                     clsGroup._maxSdsSize );
        _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, 217, McDataWarnText( 217 ) );
        iStatus = 0;
        return true;
    }

    // fan-out — 발신자 제외, 제휴 멤버만(§6.3.4). 제휴 멤버가 없으면 403 198(§9.2.2.4.2 6)k)ii) · §10.2.4.4.2 12)i)),
    //   제휴 저장소에 닿지 못하면 500.
    std::vector<std::string> vecTargets;
    const int iTargets = McDataDeliveryTargets( clsGroup, pszFrom, pszTo, vecTargets, &iWarn );
    if ( iTargets != 0 ) {
        if ( iWarn > 0 ) {
            _RejectWithWarning( pclsMessage, iTargets, iWarn, McDataWarnText( iWarn ) );
            iStatus = 0;
        } else {
            iStatus = iTargets;
        }
        return true;
    }
    int iFanout = 0;
    for ( const auto &strMember : vecTargets ) {
        CUserInfo clsMemInfo;
        if ( gclsUserMap.Select( strMember.c_str(), clsMemInfo ) ) {
            CSipCallRoute clsMemRoute;
            clsMemInfo.GetCallRoute( clsMemRoute );
            if ( gclsUserAgent.SendSms( pszFrom, strMember.c_str(), pclsMessage->m_strBody.c_str(), &clsMemRoute,
                                        szContentType[0] ? szContentType : NULL ) )
                iFanout++;
        }
    }

    {
        const char *pszType = bFd ? "fd" : ( bMcData ? "sds" : "text" );
        CMcDataSdsInfo clsArcInfo = clsInfo;
        if ( !bMcData ) clsArcInfo.m_strText = pclsMessage->m_strBody;
        McDataArchiveMessage( pszTo, pszFrom, pszType, clsArcInfo, iPayloadSize, iFanout, "", "", bMcData );
    }

    CLog::Print( LOG_INFO, "McDataAs: group SDS from(%s) to(%s) mcdata=%d size=%d fanout=%d conv(%s) msg(%s)", pszFrom,
                 pszTo, bMcData, iPayloadSize, iFanout, clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str() );
    iStatus = SIP_OK;
    return true;
}

bool CMcDataAsModule::OnDispositionNotification( const char *pszFrom, CSipMessage *pclsMessage, int &iStatus ) {
    char szContentType[512];
    szContentType[0] = '\0';
    pclsMessage->m_clsContentType.ToString( szContentType, sizeof( szContentType ) );
    if ( !McDataIsMultipartMixed( szContentType ) ) return false;
    CMcDataSdsInfo clsInfo;
    if ( !McDataParseBody( szContentType, pclsMessage->m_strBody, clsInfo ) ) return false;
    if ( clsInfo.m_iMsgType != MCDATA_MSG_SDS_NOTIFICATION || !clsInfo.m_bHasResourceLists ) return false;

    const std::string strNotifier = pszFrom ? pszFrom : "";  // 참여 기능이 정한 통지자 MCData ID(§12.2.2.1 2)·10))
    iStatus = 0;                                             // 아래 거절은 Warning 을 실어 여기서 보낸다

    // §12.2.3 2) — ICSI mcdata.sds Accept-Contact (§6.2.4.1 1)b))
    bool bIcsi = false;
    for ( const auto &h : pclsMessage->m_clsHeaderList )
        if ( strcasecmp( h.m_strName.c_str(), "Accept-Contact" ) == 0 &&
             h.m_strValue.find( "3gpp-service.ims.icsi.mcdata.sds" ) != std::string::npos )
            bIcsi = true;
    if ( !bIcsi ) {
        CLog::Print( LOG_INFO, "McDataAs: disposition from(%s) — Accept-Contact ICSI mcdata.sds 없음 → 403",
                     strNotifier.c_str() );
        iStatus = SIP_FORBIDDEN;
        return true;
    }
    // 3) 대상 MCData ID 는 하나 — 없거나 둘 이상이면 145
    if ( clsInfo.m_vecListUris.size() != 1 ) {
        CLog::Print( LOG_INFO, "McDataAs: disposition from(%s) resource-lists entries=%zu → 403 (145)",
                     strNotifier.c_str(), clsInfo.m_vecListUris.size() );
        _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, 145, "unable to determine called party" );
        return true;
    }
    const std::string strTarget = McpttBareId( clsInfo.m_vecListUris[0] );
    // 4)·5) 원 SDS 와 상관 — 대화·메시지 ID 가 이 서버가 전달한 SDS 이고 그 발신자가 통지 대상이어야 한다
    std::string strOrigSender, strOrigGroup;
    if ( !McDataCorrelateSds( clsInfo.m_strConvId, clsInfo.m_strMsgId, strOrigSender, strOrigGroup ) ||
         strOrigSender != strTarget ) {
        CLog::Print( LOG_INFO, "McDataAs: disposition from(%s) to(%s) conv(%s) msg(%s) — 상관 실패 → 403 (216)",
                     strNotifier.c_str(), strTarget.c_str(), clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str() );
        _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, 216, "unable to correlate the disposition notification" );
        return true;
    }
    // 4) 상관 — 통지의 그룹 문맥(<mcdata-calling-group-id>, 1:1 이면 없음)이 원 SDS 의 것(1:1 이면 빈 값)과 같아야
    // 한다.
    //   다르면 다른 대화의 통지다 — 216. 그 뒤 15)b) 그룹 통지면 통지자가 그 그룹 멤버여야 한다
    const std::string strGroup = McpttBareId( clsInfo.m_strCallingGroupId );
    if ( strGroup != strOrigGroup ) {
        CLog::Print( LOG_INFO,
                     "McDataAs: disposition from(%s) conv(%s) msg(%s) group(%s) — 원 SDS 그룹(%s)과 다름 → 403 (216)",
                     strNotifier.c_str(), clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str(),
                     strGroup.empty() ? "-" : strGroup.c_str(), strOrigGroup.empty() ? "-" : strOrigGroup.c_str() );
        _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, 216, "unable to correlate the disposition notification" );
        return true;
    }
    if ( !strGroup.empty() ) {
        CspPttGroup clsGroup;
        bool bMember = false;
        if ( gclsGroupMap.Select( strGroup.c_str(), clsGroup ) )
            for ( const auto &pUser : clsGroup._pusers )
                if ( pUser && pUser->_id == strNotifier ) bMember = true;
        if ( !bMember ) {
            CLog::Print( LOG_INFO, "McDataAs: disposition from(%s) group(%s) — 멤버 아님 → 403 (116)",
                         strNotifier.c_str(), strGroup.c_str() );
            _RejectWithWarning( pclsMessage, SIP_FORBIDDEN, 116, "user is not part of the MCData group" );
            return true;
        }
    }

    CUserInfo clsTargetInfo;
    if ( !gclsUserMap.Select( strTarget.c_str(), clsTargetInfo ) ) {  // 1:1 전달과 같은 구분 (mcdata_messaging.md §7)
        CspUser clsKnown;
        iStatus = gclsCspUserMap.Select( strTarget.c_str(), clsKnown ) ? SIP_TEMPORARILY_UNAVAILABLE : SIP_NOT_FOUND;
        return true;
    }
    CSipCallRoute clsRoute;
    clsTargetInfo.GetCallRoute( clsRoute );

    // 7)~17) 중계 MESSAGE — mcdata-info(14) 대상 = mcdata-request-uri, 통지자·그룹) + 받은 signalling 파트 그대로
    //   (15)d)·16) — 집계(TDC1)는 하지 않는다). 헤더 = Accept-Contact(8)), P-Asserted-Service(11)), P-Asserted-Identity
    //   = 제어 기능 PSI(13) — 그룹 통지는 그룹 URI, 1:1 은 MCData 서버 PSI).
    const std::string strDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    std::string strInfo =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\r\n"
        "  <mcdata-Params>\r\n"
        "    <mcdata-request-uri type=\"Normal\"><mcdataURI>" +
        _TelOf( strTarget ) +
        "</mcdataURI></mcdata-request-uri>\r\n"
        "    <mcdata-calling-user-id type=\"Normal\"><mcdataURI>" +
        _TelOf( strNotifier ) + "</mcdataURI></mcdata-calling-user-id>\r\n";
    if ( !strGroup.empty() )
        strInfo += "    <mcdata-calling-group-id type=\"Normal\"><mcdataURI>" + _TelOf( strGroup ) +
                   "</mcdataURI></mcdata-calling-group-id>\r\n";
    strInfo += "  </mcdata-Params>\r\n</mcdatainfo>";
    const std::string strBoundary = "mcdata-disposition-" + McDataNewMessageId().substr( 0, 12 );
    const std::string strBody = "--" + strBoundary + "\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n" +
                                strInfo + "\r\n--" + strBoundary + "\r\n" + clsInfo.m_strSignallingPart + "\r\n--" +
                                strBoundary + "--\r\n";
    const std::vector<std::pair<std::string, std::string>> vecHeaders = {
        { "Accept-Contact", "*;+g.3gpp.mcdata.sds;require;explicit" },
        { "Accept-Contact", "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\";require;explicit" },
        { "P-Asserted-Service", "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds" },
        { "P-Asserted-Identity",
          "<sip:" + ( strGroup.empty() ? std::string( "mcdata_psi" ) : strGroup ) + "@" + strDomain + ">" } };
    const bool bSent = gclsUserAgent.SendSms( strNotifier.c_str(), strTarget.c_str(), strBody.c_str(), &clsRoute,
                                              ( "multipart/mixed;boundary=" + strBoundary ).c_str(), &vecHeaders );
    CLog::Print( LOG_INFO, "McDataAs: disposition from(%s) → %s notif=%d conv(%s) msg(%s) group(%s) %s",
                 strNotifier.c_str(), strTarget.c_str(), clsInfo.m_iNotifType, clsInfo.m_strConvId.c_str(),
                 clsInfo.m_strMsgId.c_str(), strGroup.empty() ? "-" : strGroup.c_str(), bSent ? "relayed" : "FAILED" );
    iStatus = bSent ? SIP_OK : SIP_INTERNAL_SERVER_ERROR;
    return true;
}

int CMcDataAsModule::OnEmergencyAlert( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage,
                                       const std::string &strInfo ) {
    std::string strAlert, strTarget;
    McpttElemValue( strInfo, "alert-ind", strAlert );
    std::transform( strAlert.begin(), strAlert.end(), strAlert.begin(), ::tolower );
    const bool bActivate = ( strAlert == "true" || strAlert == "1" );
    McpttElemValue( strInfo, "mcdata-request-uri", strTarget );
    const std::string strGroup = strTarget.empty() ? std::string( pszTo ? pszTo : "" ) : McpttBareId( strTarget );

    if ( bActivate ) {
        // §16.2.3.1 4)a) — 인가(§6.3.7.2.1) = MCData user profile allow-activate-emergency-alert ∧ 그룹 문서
        //   <mcdata-allow-emergency-alert> true. 그룹 문서(CSC)는 그 요소를 싣지 않는다(MCData 긴급 경보 미지원 —
        //   mcdata_messaging.md §8) → 미인가 = 403 + mcdata-info <alert-ind>false. MCPTT 경보로 바꿔 배포하지 않는다.
        CLog::Print( LOG_INFO,
                     "McDataAs: emergency alert from(%s) group(%s) — not authorised (mcdata-allow-emergency-alert) "
                     "→ 403",
                     pszFrom ? pszFrom : "", strGroup.c_str() );
        CSipMessage *pclsResp = pclsMessage->CreateResponseWithToTag( SIP_FORBIDDEN );
        if ( pclsResp ) {
            pclsResp->m_strBody =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\r\n"
                "  <mcdata-Params>\r\n"
                "    <alert-ind type=\"Normal\"><mcdataBoolean>false</mcdataBoolean></alert-ind>\r\n"
                "  </mcdata-Params>\r\n</mcdatainfo>";
            pclsResp->m_iContentLength = (int)pclsResp->m_strBody.size();
            pclsResp->m_clsContentType.Set( "application", "vnd.3gpp.mcdata-info+xml" );
            gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResp );
        }
        return 0;
    }
    // §16.2.3.2 — 발령이 늘 미인가라 남은 MCData 경보가 없다: 지울 캐시(2)a)·b))도, 보낼 취소 통지(2)c))도 없다 → 200.
    CLog::Print( LOG_INFO, "McDataAs: emergency alert cancel from(%s) group(%s) — no outstanding MCData alert → 200",
                 pszFrom ? pszFrom : "", strGroup.c_str() );
    return SIP_OK;
}
