#include "McDataAsModule.h"

#include <stdio.h>
#include <string.h>

#include "CallDir.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "McDataCodec.h"
#include "ModuleDispatcher.h"
#include "SipServerSetup.h"
#include "SipStatusCode.h"
#include "UserMap.h"

bool CMcDataAsModule::IsEnabled() const {
    return gclsSetup.m_bRoleMcData;
}

/** JSON 문자열 이스케이프 (보관 레코드용) */
static std::string _jesc( const std::string &s ) {
    std::string r;
    r.reserve( s.size() + 16 );
    for ( unsigned char c : s ) {
        switch ( c ) {
            case '"':
                r += "\\\"";
                break;
            case '\\':
                r += "\\\\";
                break;
            case '\n':
                r += "\\n";
                break;
            case '\r':
                r += "\\r";
                break;
            case '\t':
                r += "\\t";
                break;
            default:
                if ( c < 0x20 ) {
                    char h[8];
                    snprintf( h, 8, "\\u%04x", c );
                    r += h;
                } else
                    r += (char)c;
        }
    }
    return r;
}

/**
 * 그룹 SDS 처리 (TS 24.282 group standard SDS, controlling function).
 * 처리했으면(성공·거부 모두) true — 거부 응답(403/413)은 여기서 직접 송신하며, 이후
 * RecvMessageRequest 의 자동 200 은 SIP 트랜잭션 상 후행 최종응답이라 클라이언트가 무시한다
 * (긴급경보 경로와 동일한 기존 계약).
 */
bool CMcDataAsModule::OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
    if ( pclsMessage == NULL ) return false;
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
    bool bMcData = McDataIsMultipartMixed( szContentType ) &&
                   McDataParseBody( szContentType, pclsMessage->m_strBody, clsInfo );
    bool bFd = bMcData && clsInfo.m_iMsgType == MCDATA_MSG_FD_SIGNALLING;
    int iPayloadSize = bMcData ? clsInfo.m_iPayloadSize : (int)pclsMessage->m_strBody.size();

    // 게이트 1 — 그룹문서 mcdata-allow-short-data-service / mcdata-allow-file-distribution (TS 24.481)
    if ( bFd ? clsGroup._allowFd == false : clsGroup._allowSds == false ) {
        CLog::Print( LOG_INFO, "McDataAs: group(%s) %s disabled — reject 403 from(%s)", pszTo, bFd ? "FD" : "SDS",
                     pszFrom );
        gclsDispatcher.SendResponse( pclsMessage, SIP_FORBIDDEN );
        return true;
    }

    // 게이트 2 — 발신자 그룹 멤버십 (controlling function 검사)
    bool bMember = false;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( pUser && pUser->_id == pszFrom ) {
            bMember = true;
            break;
        }
    }
    if ( bMember == false ) {
        CLog::Print( LOG_INFO, "McDataAs: from(%s) is not a member of group(%s) — reject 403", pszFrom, pszTo );
        gclsDispatcher.SendResponse( pclsMessage, SIP_FORBIDDEN );
        return true;
    }

    // 게이트 3 — mcdata-on-network-max-data-size-for-SDS (TS 24.481). FD 는 payload=URL 이라 제외
    //   (파일 크기 상한은 CSC 업로드 단에서 강제).
    if ( !bFd && clsGroup._maxSdsSize > 0 && iPayloadSize > clsGroup._maxSdsSize ) {
        CLog::Print( LOG_INFO, "McDataAs: group(%s) payload %d > max %d — reject 413", pszTo, iPayloadSize,
                     clsGroup._maxSdsSize );
        gclsDispatcher.SendResponse( pclsMessage, SIP_REQUEST_ENTITY_TOO_LARGE );
        return true;
    }

    // fan-out — 발신자 제외. affiliation 요구 그룹은 affiliate 멤버만 (긴급경보 경로와 동일 규칙).
    int iFanout = 0;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser || pUser->_id == pszFrom ) continue;
        if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
             !gclsDbManager.IsAffiliated( pszTo, pUser->_id ) )
            continue;
        CUserInfo clsMemInfo;
        if ( gclsUserMap.Select( pUser->_id.c_str(), clsMemInfo ) ) {
            CSipCallRoute clsMemRoute;
            clsMemInfo.GetCallRoute( clsMemRoute );
            if ( gclsUserAgent.SendSms( pszFrom, pUser->_id.c_str(), pclsMessage->m_strBody.c_str(), &clsMemRoute,
                                        szContentType[0] ? szContentType : NULL ) )
                iFanout++;
        }
    }

    if ( gclsCallDir.IsEnabled() ) {
        char szEvt[512];
        snprintf( szEvt, sizeof( szEvt ),
                  "{\"actor\":\"%s\",\"target\":\"%s\",\"conv_id\":\"%s\",\"msg_id\":\"%s\","
                  "\"payload_size\":%d,\"disposition_req\":%d,\"fanout\":%d,\"mcdata\":%s}",
                  pszFrom, pszTo, clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str(), iPayloadSize,
                  clsInfo.m_iDispositionReq, iFanout, bMcData ? "true" : "false" );
        gclsCallDir.PttLogEvent( pszTo, "message_sent", szEvt );

        // 메시지 보관 — {ServiceLogDir}/message/{gid}/{시간버킷}/messages.jsonl (콘솔 모니터링 SoT)
        const char *pszType = bFd ? "fd" : ( bMcData ? "sds" : "text" );
        std::string strText = bMcData ? clsInfo.m_strText : pclsMessage->m_strBody;
        std::string strRec = std::string( "{\"group\":\"" ) + _jesc( pszTo ) + "\",\"from\":\"" + _jesc( pszFrom ) +
                             "\",\"msg_type\":\"" + pszType + "\",\"conv_id\":\"" + clsInfo.m_strConvId +
                             "\",\"msg_id\":\"" + clsInfo.m_strMsgId + "\",\"text\":\"" + _jesc( strText ) +
                             "\",\"size\":" + std::to_string( iPayloadSize ) +
                             ",\"disposition_req\":" + std::to_string( clsInfo.m_iDispositionReq ) +
                             ",\"fanout\":" + std::to_string( iFanout );
        if ( bFd ) {
            strRec += std::string( ",\"file_name\":\"" ) + _jesc( clsInfo.m_strFileName ) + "\",\"file_url\":\"" +
                      _jesc( clsInfo.m_strFileUrl ) + "\",\"file_size\":" + std::to_string( clsInfo.m_llFileSize ) +
                      ",\"file_type\":\"" + _jesc( clsInfo.m_strFileType ) + "\"";
        }
        strRec += "}";
        gclsCallDir.McDataMessageLog( pszTo, strRec );
    }

    CLog::Print( LOG_INFO, "McDataAs: group SDS from(%s) to(%s) mcdata=%d size=%d fanout=%d conv(%s) msg(%s)", pszFrom,
                 pszTo, bMcData, iPayloadSize, iFanout, clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str() );
    return true;  // RecvMessageRequest 가 200 OK 송신
}
