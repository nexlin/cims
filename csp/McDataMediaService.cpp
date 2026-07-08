#include "McDataMediaService.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "CmdpClient.h"
#include "CspServiceMap.h"
#include "GroupMap.h"
#include "Log.h"
#include "McDataCodec.h"
#include "McDataGates.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "SipStatusCode.h"
#include "UserMap.h"

CMcDataMediaService &CMcDataMediaService::GetInstance() {
    static CMcDataMediaService instance;
    return instance;
}

bool CMcDataMediaService::IsEnabled() const {
    return gclsSetup.m_bRoleMcData && gclsSetup.m_bUseMcDataMedia;
}

void CMcDataMediaService::Init() {
    if ( !IsEnabled() ) return;
    if ( !gclsCmdpClient.Init( gclsSetup.m_strCmdpIp, gclsSetup.m_iCmdpPort, gclsSetup.m_iLocalCmdpPort ) ) {
        CLog::Print( LOG_ERROR, "McDataMedia: CmdpClient Init failed" );
        return;
    }
    gclsCmdpClient.SetEventCallback(
        []( const SimpleJson::JsonNode &clsEvent ) { gclsMcDataMediaService.OnCmdpEvent( clsEvent ); } );
    CLog::Print( LOG_SYSTEM, "McDataMedia: enabled (cmdp %s:%d)", gclsSetup.m_strCmdpIp.c_str(),
                 gclsSetup.m_iCmdpPort );
}

bool CMcDataMediaService::IsMsrpInvite( CSipCallRtp *pclsRtp ) {
    if ( pclsRtp == NULL ) return false;
    for ( const auto &clsMedia : pclsRtp->m_clsMediaList ) {
        if ( clsMedia.m_strMedia == "message" && clsMedia.m_strProtocol.find( "MSRP" ) != std::string::npos )
            return true;
    }
    return false;
}

bool CMcDataMediaService::ExtractMsrpOffer( CSipCallRtp *pclsRtp, std::string &strRemotePath,
                                            const CSdpMedia **ppclsAudio ) {
    strRemotePath.clear();
    if ( ppclsAudio ) *ppclsAudio = NULL;
    bool bFound = false;
    for ( const auto &clsMedia : pclsRtp->m_clsMediaList ) {
        if ( clsMedia.m_strMedia == "message" && clsMedia.m_strProtocol.find( "MSRP" ) != std::string::npos ) {
            for ( const auto &clsAttr : clsMedia.m_clsAttributeList ) {
                if ( clsAttr.m_strName == "path" ) {
                    strRemotePath = clsAttr.m_strValue;
                    // 다중 URI(릴레이) 미지원 — 첫 URI 만
                    size_t iSp = strRemotePath.find( ' ' );
                    if ( iSp != std::string::npos ) strRemotePath = strRemotePath.substr( 0, iSp );
                }
            }
            bFound = true;
        } else if ( clsMedia.m_strMedia == "audio" && ppclsAudio && *ppclsAudio == NULL ) {
            *ppclsAudio = &clsMedia;
        }
    }
    return bFound && !strRemotePath.empty();
}

bool CMcDataMediaService::ParseMsrpPathHost( const std::string &strPath, std::string &strIp, int &iPort ) {
    // msrp://ip:port/session;tcp
    static const char kPrefix[] = "msrp://";
    if ( strPath.compare( 0, sizeof( kPrefix ) - 1, kPrefix ) != 0 ) return false;
    size_t iHostBegin = sizeof( kPrefix ) - 1;
    size_t iSlash = strPath.find( '/', iHostBegin );
    if ( iSlash == std::string::npos ) return false;
    std::string strHostPort = strPath.substr( iHostBegin, iSlash - iHostBegin );
    size_t iColon = strHostPort.rfind( ':' );
    if ( iColon == std::string::npos ) {
        strIp = strHostPort;
        iPort = 2855;
    } else {
        strIp = strHostPort.substr( 0, iColon );
        iPort = atoi( strHostPort.substr( iColon + 1 ).c_str() );
    }
    return !strIp.empty() && iPort > 0;
}

/** 더미 오디오 응답/오퍼 미디어 — 포트≠0 + a=inactive (서버↔앱 계약: 포트 0 이면 pjsua 콜 사망) */
static CSdpMedia _BuildInactiveAudio( const CSdpMedia *pclsOffered ) {
    CSdpMedia clsAudio;
    if ( pclsOffered ) {
        clsAudio = *pclsOffered;  // 오퍼의 코덱 목록/속성 보존 (pjmedia 협상 정합)
        clsAudio.DeleteAttribute( "sendrecv" );
        clsAudio.DeleteAttribute( "sendonly" );
        clsAudio.DeleteAttribute( "recvonly" );
        clsAudio.DeleteAttribute( "inactive" );
    } else {
        clsAudio.m_strMedia = "audio";
        clsAudio.m_strProtocol = "RTP/AVP";
        // PCMU+PCMA (static PT — rtpmap 불필요). 더미(inactive)지만 수신 단말 pjsua 가
        // 수락 가능한 코덱이 하나는 있어야 488 을 내지 않는다 — 앱은 PCMU 비활성·PCMA 안전망 유지.
        clsAudio.AddFmt( 0 );
        clsAudio.AddFmt( 8 );
    }
    clsAudio.m_iPort = 9;  // discard — RTP 무흐름, 0 금지
    clsAudio.AddAttribute( "inactive", "" );
    return clsAudio;
}

/** tel: URI 표기 — 이미 스킴이 있으면 그대로, 없으면 tel: 부여 (user part 만 보관되는 관례). */
static std::string _TelUriOf( const std::string &strId ) {
    if ( strId.compare( 0, 4, "tel:" ) == 0 || strId.compare( 0, 4, "sip:" ) == 0 ) return strId;
    return "tel:" + strId;
}

/**
 * 배포 레그 INVITE 본문을 multipart/mixed(mcdata-info + SDP)로 재구성 —
 * 수신 단말이 그룹 스레드 귀속(request-uri)과 발신자 표시·disposition 회신 대상
 * (calling-user-id)을 알 수 있게 한다 (GroupCallService::WrapMultipartBody 패턴).
 */
static void _WrapMcDataInfoBody( CSipMessage *pclsInvite, const std::string &strGroup,
                                 const std::string &strCaller ) {
    if ( pclsInvite == NULL || pclsInvite->m_strBody.empty() ) return;

    struct timespec _ts;
    clock_gettime( CLOCK_REALTIME, &_ts );
    char _szBoundary[40];
    snprintf( _szBoundary, sizeof( _szBoundary ), "mcdata_%08x%08x", (unsigned)_ts.tv_sec,
              (unsigned)( _ts.tv_nsec ^ (uintptr_t)pclsInvite ) );
    const std::string strBoundary = _szBoundary;

    std::string strInfo =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\r\n"
        "  <mcdata-Params>\r\n"
        "    <request-type>group-sds</request-type>\r\n"
        "    <mcdata-request-uri type=\"Normal\"><mcdataURI>" + _TelUriOf( strGroup ) +
        "</mcdataURI></mcdata-request-uri>\r\n"
        "    <mcdata-calling-user-id type=\"Normal\"><mcdataURI>" + _TelUriOf( strCaller ) +
        "</mcdataURI></mcdata-calling-user-id>\r\n"
        "  </mcdata-Params>\r\n"
        "</mcdatainfo>";

    std::string strSdp = pclsInvite->m_strBody;
    std::string strBody;
    strBody.reserve( strInfo.size() + strSdp.size() + 300 );
    strBody += "--" + strBoundary + "\r\n";
    strBody += "Content-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n";
    strBody += strInfo;
    strBody += "\r\n--" + strBoundary + "\r\n";
    strBody += "Content-Type: application/sdp\r\n\r\n";
    strBody += strSdp;
    strBody += "\r\n--" + strBoundary + "--\r\n";

    pclsInvite->m_strBody = strBody;
    pclsInvite->m_iContentLength = (int)strBody.size();
    pclsInvite->m_clsContentType.Set( "multipart", "mixed" );
    pclsInvite->m_clsContentType.InsertParam( "boundary", strBoundary.c_str() );
}

static CSdpMedia _BuildMsrpMedia( int iPort, const std::string &strProtocol, const std::string &strPath,
                                  const char *pszSetup, const char *pszDirection ) {
    CSdpMedia clsMsg;
    clsMsg.m_strMedia = "message";
    clsMsg.m_iPort = iPort;
    clsMsg.m_strProtocol = strProtocol;
    clsMsg.m_clsFmtList.push_back( "*" );
    clsMsg.AddAttribute( "path", strPath.c_str() );
    clsMsg.AddAttribute( "accept-types",
                         "multipart/mixed application/vnd.3gpp.mcdata-signalling "
                         "application/vnd.3gpp.mcdata-payload" );
    clsMsg.AddAttribute( "setup", pszSetup );
    clsMsg.AddAttribute( pszDirection, "" );
    return clsMsg;
}

void CMcDataMediaService::OnIncomingMsrpInvite( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                                CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    (void)pclsMessage;

    if ( !IsEnabled() || !gclsCmdpClient.IsConnected() ) {
        CLog::Print( LOG_ERROR, "McDataMedia: MSRP INVITE from(%s) but cmdp %s — 500", pszFrom,
                     IsEnabled() ? "disconnected" : "disabled" );
        gclsUserAgent.StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
        return;
    }

    // phase 1: 그룹 SDS 만 (1:1 standalone 은 후속)
    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( pszTo, clsGroup ) == false ) {
        CLog::Print( LOG_INFO, "McDataMedia: MSRP INVITE target(%s) is not a group — 403", pszTo );
        gclsUserAgent.StopCall( pszCallId, SIP_FORBIDDEN );
        return;
    }

    int iGate = McDataGateCheck( clsGroup, pszFrom, false );
    if ( iGate != 0 ) {
        gclsUserAgent.StopCall( pszCallId, iGate );
        return;
    }

    std::string strRemotePath;
    const CSdpMedia *pclsAudio = NULL;
    if ( !ExtractMsrpOffer( pclsRtp, strRemotePath, &pclsAudio ) ) {
        CLog::Print( LOG_INFO, "McDataMedia: MSRP INVITE from(%s) without a=path — 488", pszFrom );
        gclsUserAgent.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
        return;
    }

    // cmdp 수신 세션 할당 — 크기 상한 = 그룹 max_sds_size (cmdp 절대상한과 min 은 cmdp 가 적용)
    std::string strSessionId = CCmdpClient::IssueSessionId();
    std::string strMsrpPath;
    if ( !gclsCmdpClient.AddRecvSession( strSessionId, pszFrom, pszTo, strRemotePath,
                                         clsGroup._maxSdsSize > 0 ? clsGroup._maxSdsSize : 0, strMsrpPath ) ) {
        CLog::Print( LOG_ERROR, "McDataMedia: AddRecvSession failed from(%s) group(%s)", pszFrom, pszTo );
        gclsUserAgent.StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
        return;
    }

    std::string strCmdpIp;
    int iCmdpMsrpPort = 0;
    if ( !ParseMsrpPathHost( strMsrpPath, strCmdpIp, iCmdpMsrpPort ) ) {
        gclsCmdpClient.RemoveSession( strSessionId );
        gclsUserAgent.StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
        return;
    }

    // 200 OK answer — 세션 c= 는 cmdp IP. 오디오는 오퍼 에코(포트 9, inactive), message 는 cmdp 종단.
    CSipCallRtp clsAnswer;
    clsAnswer.m_strIp = strCmdpIp;
    clsAnswer.m_iPort = 9;
    clsAnswer.m_clsMediaList.push_back( _BuildInactiveAudio( pclsAudio ) );
    clsAnswer.m_clsMediaList.push_back(
        _BuildMsrpMedia( iCmdpMsrpPort, "TCP/MSRP", strMsrpPath, "passive", "recvonly" ) );

    if ( !gclsUserAgent.AcceptCall( pszCallId, &clsAnswer ) ) {
        CLog::Print( LOG_ERROR, "McDataMedia: AcceptCall(%s) failed", pszCallId );
        gclsCmdpClient.RemoveSession( strSessionId );
        return;
    }

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        McDataMediaCall clsCall;
        clsCall.strSessionId = strSessionId;
        clsCall.strFrom = pszFrom;
        clsCall.strGroup = pszTo;
        clsCall.eDir = DIR_RECV;
        m_mapCalls[pszCallId] = clsCall;
        m_mapSessionToCall[strSessionId] = pszCallId;
    }
    CLog::Print( LOG_INFO, "McDataMedia: recv leg up call(%s) from(%s) group(%s) session(%s) path(%s)",
                 pszCallId, pszFrom, pszTo, strSessionId.c_str(), strMsrpPath.c_str() );
}

bool CMcDataMediaService::OnCallStarted( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    std::string strSessionId;
    bool bSendLeg = false;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_mapCalls.find( pszCallId );
        if ( it == m_mapCalls.end() ) return false;
        strSessionId = it->second.strSessionId;
        bSendLeg = ( it->second.eDir == DIR_SEND );
    }

    if ( bSendLeg && pclsRtp ) {
        // 수신자 200 OK answer 의 a=path → cmdp (전송 개시 트리거)
        std::string strReceiverPath;
        if ( ExtractMsrpOffer( pclsRtp, strReceiverPath, NULL ) ) {
            gclsCmdpClient.SetRemotePath( strSessionId, strReceiverPath );
        } else {
            CLog::Print( LOG_ERROR, "McDataMedia: send leg(%s) answer without a=path — teardown", pszCallId );
            gclsUserAgent.StopCall( pszCallId );
        }
    }
    return true;
}

bool CMcDataMediaService::OnCallTerminated( const char *pszCallId ) {
    std::string strSessionId;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_mapCalls.find( pszCallId );
        if ( it == m_mapCalls.end() ) return false;
        strSessionId = it->second.strSessionId;
        m_mapSessionToCall.erase( strSessionId );
        m_mapCalls.erase( it );
    }
    gclsCmdpClient.RemoveSession( strSessionId );  // 멱등
    CLog::Print( LOG_INFO, "McDataMedia: call(%s) terminated — session(%s) removed", pszCallId,
                 strSessionId.c_str() );
    return true;
}

void CMcDataMediaService::OnCmdpEvent( const SimpleJson::JsonNode &clsEvent ) {
    std::string strName = clsEvent.GetString( "event" );
    SimpleJson::JsonNode clsPayload = clsEvent.Get( "payload" );
    if ( clsPayload.type != SimpleJson::JSON_OBJECT ) return;
    std::string strSessionId = clsPayload.GetString( "session_id" );

    if ( strName == "MSG_RECEIVED" ) {
        // 이벤트 재전송 중복 방어 — 세션이 이미 정리됐으면 무시
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            if ( m_mapSessionToCall.find( strSessionId ) == m_mapSessionToCall.end() ) {
                CLog::Print( LOG_INFO, "McDataMedia: MSG_RECEIVED for unknown session(%s) — dup/late, ignore",
                             strSessionId.c_str() );
                return;
            }
        }
        HandleMsgReceived( clsPayload );
    } else if ( strName == "SEND_RESULT" ) {
        HandleSessionClosed( strSessionId, clsPayload.GetString( "status" ) == "ok",
                             clsPayload.GetString( "reason" ).c_str() );
    } else if ( strName == "SESSION_ABORTED" ) {
        HandleSessionClosed( strSessionId, false, clsPayload.GetString( "reason" ).c_str() );
    }
}

void CMcDataMediaService::HandleMsgReceived( const SimpleJson::JsonNode &clsPayload ) {
    std::string strSessionId = clsPayload.GetString( "session_id" );
    std::string strFrom = clsPayload.GetString( "caller" );
    std::string strGroup = clsPayload.GetString( "group_id" );

    int iFanout = FanOutMediaSds( strGroup, strFrom, clsPayload );

    // 보관 — C-plane 경로와 동일 SoT (+ via=msrp, file_url)
    CMcDataSdsInfo clsInfo;
    clsInfo.m_iMsgType = (int)clsPayload.GetInt( "msg_type", MCDATA_MSG_SDS_SIGNALLING );
    clsInfo.m_strConvId = clsPayload.GetString( "conv_id" );
    clsInfo.m_strMsgId = clsPayload.GetString( "msg_id" );
    clsInfo.m_iDispositionReq = (int)clsPayload.GetInt( "disposition_req", 0 );
    clsInfo.m_strText = clsPayload.GetString( "text" );
    clsInfo.m_strFileName = clsPayload.GetString( "file_name" );
    clsInfo.m_strFileType = clsPayload.GetString( "file_type" );
    clsInfo.m_llFileSize = clsPayload.GetInt( "size", 0 );
    std::string strUrl = gclsSetup.m_strFdUrlBase.empty()
                             ? ( "https://" +
                                 ( gclsSetup.m_strXcapHost.empty() ? gclsSetup.m_strLocalIp
                                                                   : gclsSetup.m_strXcapHost ) +
                                 ":4430" )
                             : gclsSetup.m_strFdUrlBase;
    strUrl += "/mcdata/fd/" + clsPayload.GetString( "file_id" );
    McDataArchiveMessage( strGroup.c_str(), strFrom.c_str(), "sds", clsInfo,
                          (int)clsPayload.GetInt( "size", 0 ), iFanout, "msrp", strUrl.c_str() );

    // 발신 레그 종료 (전송 완료 — TS 24.282 standalone 은 전달 후 세션 해제)
    std::string strCallId;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_mapSessionToCall.find( strSessionId );
        if ( it != m_mapSessionToCall.end() ) {
            strCallId = it->second;
            m_mapCalls.erase( strCallId );
            m_mapSessionToCall.erase( it );
        }
    }
    if ( !strCallId.empty() ) gclsUserAgent.StopCall( strCallId.c_str() );
    gclsCmdpClient.RemoveSession( strSessionId );

    CLog::Print( LOG_INFO, "McDataMedia: SDS via MSRP from(%s) group(%s) size=%lld fanout=%d msg(%s)",
                 strFrom.c_str(), strGroup.c_str(), (long long)clsPayload.GetInt( "size", 0 ), iFanout,
                 clsInfo.m_strMsgId.c_str() );
}

int CMcDataMediaService::FanOutMediaSds( const std::string &strGroup, const std::string &strFrom,
                                         const SimpleJson::JsonNode &clsPayload ) {
    CspPttGroup clsGroup;
    if ( gclsGroupMap.Select( strGroup.c_str(), clsGroup ) == false ) {
        CLog::Print( LOG_ERROR, "McDataMedia: fan-out group(%s) not found", strGroup.c_str() );
        return 0;
    }

    std::vector<std::string> vecTargets;
    McDataDeliveryTargets( clsGroup, strFrom.c_str(), strGroup.c_str(), vecTargets );

    std::string strFileId = clsPayload.GetString( "file_id" );
    std::string strContentType = clsPayload.GetString( "content_type" );

    // FILEURL 폴백 본문 (비 MSRP 단말) — 대상이 있을 때 1회만 생성
    std::string strFallbackBody, strFallbackCt;

    int iFanout = 0;
    for ( const auto &strMember : vecTargets ) {
        CUserInfo clsMemInfo;
        if ( gclsUserMap.Select( strMember.c_str(), clsMemInfo ) == false ) continue;

        if ( clsMemInfo.m_bMcDataMsrp ) {
            if ( InviteMsrpReceiver( strGroup, strFrom, strMember, strFileId, strContentType ) ) iFanout++;
            continue;
        }

        // 폴백 — FD FILEURL MESSAGE (기존 C-plane FD 수신 경로 재사용)
        if ( strFallbackBody.empty() ) {
            std::string strDomain = gclsServiceMap.GetDomainByKind( "ptt" );
            std::string strGroupUri =
                "sip:" + strGroup + ( strDomain.empty() ? "" : "@" + strDomain );
            std::string strUrlBase = gclsSetup.m_strFdUrlBase;
            if ( strUrlBase.empty() )
                strUrlBase = "https://" +
                             ( gclsSetup.m_strXcapHost.empty() ? gclsSetup.m_strLocalIp
                                                               : gclsSetup.m_strXcapHost ) +
                             ":4430";
            strFallbackBody = McDataBuildFdSignallingBody(
                strFallbackCt, strGroupUri, strUrlBase + "/mcdata/fd/" + strFileId,
                clsPayload.GetString( "file_name" ), clsPayload.GetInt( "size", 0 ),
                clsPayload.GetString( "file_type" ), clsPayload.GetString( "conv_id" ),
                clsPayload.GetString( "msg_id" ) );
        }
        CSipCallRoute clsRoute;
        clsMemInfo.GetCallRoute( clsRoute );
        if ( gclsUserAgent.SendSms( strFrom.c_str(), strMember.c_str(), strFallbackBody.c_str(), &clsRoute,
                                    strFallbackCt.c_str() ) )
            iFanout++;
    }
    return iFanout;
}

bool CMcDataMediaService::InviteMsrpReceiver( const std::string &strGroup, const std::string &strFrom,
                                              const std::string &strMember, const std::string &strFileId,
                                              const std::string &strContentType ) {
    std::string strSessionId = CCmdpClient::IssueSessionId();
    std::string strMsrpPath;
    if ( !gclsCmdpClient.AddSendSession( strSessionId, strFileId, strGroup, strMember, strContentType,
                                         strMsrpPath ) ) {
        CLog::Print( LOG_ERROR, "McDataMedia: AddSendSession failed member(%s) file(%s)", strMember.c_str(),
                     strFileId.c_str() );
        return false;
    }

    std::string strCmdpIp;
    int iCmdpMsrpPort = 0;
    if ( !ParseMsrpPathHost( strMsrpPath, strCmdpIp, iCmdpMsrpPort ) ) {
        gclsCmdpClient.RemoveSession( strSessionId );
        return false;
    }

    // 오퍼 — 더미 오디오(inactive) + m=message sendonly (서버 항상 passive: 수신자가 out-connect)
    CSipCallRtp clsOffer;
    clsOffer.m_strIp = strCmdpIp;
    clsOffer.m_iPort = 9;
    clsOffer.m_clsMediaList.push_back( _BuildInactiveAudio( NULL ) );
    clsOffer.m_clsMediaList.push_back(
        _BuildMsrpMedia( iCmdpMsrpPort, "TCP/MSRP", strMsrpPath, "passive", "sendonly" ) );

    CUserInfo clsMemInfo;
    if ( gclsUserMap.Select( strMember.c_str(), clsMemInfo ) == false ) {
        gclsCmdpClient.RemoveSession( strSessionId );
        return false;
    }
    CSipCallRoute clsRoute;
    clsMemInfo.GetCallRoute( clsRoute );

    std::string strCallId;
    CSipMessage *pclsInvite = NULL;
    std::string strDomain = gclsServiceMap.GetDomainByKind( "ptt" );
    if ( !gclsUserAgent.CreateCall( strGroup.c_str(), strMember.c_str(), &clsOffer, &clsRoute, strCallId,
                                    &pclsInvite, strDomain.empty() ? NULL : strDomain.c_str() ) ) {
        gclsCmdpClient.RemoveSession( strSessionId );
        return false;
    }
    if ( pclsInvite != NULL ) {
        // MCData SDS ICSI (TS 24.282 §6.3) — MSRP 대응 단말만 이 INVITE 를 받는다
        pclsInvite->AddHeader(
            "Accept-Contact", "*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\";require;explicit" );
        pclsInvite->AddHeader( "P-Preferred-Service", "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds" );
        pclsInvite->AddHeader( "Answer-Mode", "Auto" );
        // mcdata-info multipart — 수신 단말의 그룹 스레드 귀속·발신자 표시·disposition 회신 대상
        // (TS 24.282: controlling function 발신 INVITE 는 mcdata-info 포함)
        _WrapMcDataInfoBody( pclsInvite, strGroup, strFrom );
    }

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        McDataMediaCall clsCall;
        clsCall.strSessionId = strSessionId;
        clsCall.strFrom = strGroup;
        clsCall.strGroup = strGroup;
        clsCall.strCallee = strMember;
        clsCall.eDir = DIR_SEND;
        m_mapCalls[strCallId] = clsCall;
        m_mapSessionToCall[strSessionId] = strCallId;
    }

    if ( !gclsUserAgent.StartCall( strCallId.c_str(), pclsInvite ) ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_mapCalls.erase( strCallId );
        m_mapSessionToCall.erase( strSessionId );
        gclsCmdpClient.RemoveSession( strSessionId );
        return false;
    }
    CLog::Print( LOG_INFO, "McDataMedia: send leg call(%s) member(%s) session(%s) file(%s)",
                 strCallId.c_str(), strMember.c_str(), strSessionId.c_str(), strFileId.c_str() );
    return true;
}

void CMcDataMediaService::HandleSessionClosed( const std::string &strSessionId, bool bOk,
                                               const char *pszReason ) {
    std::string strCallId;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_mapSessionToCall.find( strSessionId );
        if ( it == m_mapSessionToCall.end() ) return;  // 이미 정리됨 (중복 이벤트)
        strCallId = it->second;
        m_mapCalls.erase( strCallId );
        m_mapSessionToCall.erase( it );
    }
    CLog::Print( bOk ? LOG_INFO : LOG_ERROR, "McDataMedia: session(%s) closed ok=%d reason(%s) — BYE call(%s)",
                 strSessionId.c_str(), bOk ? 1 : 0, pszReason ? pszReason : "", strCallId.c_str() );
    gclsUserAgent.StopCall( strCallId.c_str() );
    gclsCmdpClient.RemoveSession( strSessionId );
}
