#include "McDataGates.h"

#include <stdio.h>

#include "CallDir.h"
#include "DbManager.h"
#include "Log.h"
#include "SipStatusCode.h"

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

int McDataGateCheck( const CspPttGroup &clsGroup, const char *pszFrom, bool bFd ) {
    // 게이트 1 — 그룹문서 mcdata-allow-short-data-service / mcdata-allow-file-distribution (TS 24.481)
    if ( bFd ? clsGroup._allowFd == false : clsGroup._allowSds == false ) {
        CLog::Print( LOG_INFO, "McDataGate: group(%s) %s disabled — 403 from(%s)", clsGroup._id.c_str(),
                     bFd ? "FD" : "SDS", pszFrom );
        return SIP_FORBIDDEN;
    }

    // 게이트 2 — 발신자 그룹 멤버십 (controlling function 검사)
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( pUser && pUser->_id == pszFrom ) return 0;
    }
    CLog::Print( LOG_INFO, "McDataGate: from(%s) is not a member of group(%s) — 403", pszFrom, clsGroup._id.c_str() );
    return SIP_FORBIDDEN;
}

void McDataDeliveryTargets( const CspPttGroup &clsGroup, const char *pszFrom, const char *pszGroup,
                            std::vector<std::string> &vecTargets ) {
    vecTargets.clear();
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser || pUser->_id == pszFrom ) continue;
        if ( clsGroup._requireAffiliation && gclsDbManager.IsConnected() &&
             !gclsDbManager.IsAffiliated( pszGroup, pUser->_id ) )
            continue;
        vecTargets.push_back( pUser->_id );
    }
}

void McDataArchiveMessage( const char *pszGroup, const char *pszFrom, const char *pszMsgType,
                           const CMcDataSdsInfo &clsInfo, int iPayloadSize, int iFanout, const char *pszVia,
                           const char *pszFileUrl, bool bMcData ) {
    if ( !gclsCallDir.IsEnabled() ) return;

    char szEvt[512];
    snprintf( szEvt, sizeof( szEvt ),
              "{\"actor\":\"%s\",\"target\":\"%s\",\"conv_id\":\"%s\",\"msg_id\":\"%s\","
              "\"payload_size\":%d,\"disposition_req\":%d,\"fanout\":%d,\"mcdata\":%s%s%s%s}",
              pszFrom, pszGroup, clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str(), iPayloadSize,
              clsInfo.m_iDispositionReq, iFanout, bMcData ? "true" : "false", pszVia && pszVia[0] ? ",\"via\":\"" : "",
              pszVia ? pszVia : "", pszVia && pszVia[0] ? "\"" : "" );
    gclsCallDir.PttLogEvent( pszGroup, "message_sent", szEvt );

    // 메시지 보관 — {ServiceLogDir}/message/{gid}/{시간버킷}/messages.jsonl (콘솔 모니터링 SoT)
    std::string strRec = std::string( "{\"group\":\"" ) + _jesc( pszGroup ) + "\",\"from\":\"" + _jesc( pszFrom ) +
                         "\",\"msg_type\":\"" + pszMsgType + "\",\"conv_id\":\"" + clsInfo.m_strConvId +
                         "\",\"msg_id\":\"" + clsInfo.m_strMsgId + "\",\"text\":\"" + _jesc( clsInfo.m_strText ) +
                         "\",\"size\":" + std::to_string( iPayloadSize ) +
                         ",\"disposition_req\":" + std::to_string( clsInfo.m_iDispositionReq ) +
                         ",\"fanout\":" + std::to_string( iFanout );
    if ( pszVia && pszVia[0] ) strRec += std::string( ",\"via\":\"" ) + pszVia + "\"";
    bool bFileFields = ( clsInfo.m_iMsgType == MCDATA_MSG_FD_SIGNALLING ) || ( pszFileUrl && pszFileUrl[0] );
    if ( bFileFields ) {
        std::string strUrl = ( pszFileUrl && pszFileUrl[0] ) ? pszFileUrl : clsInfo.m_strFileUrl;
        strRec += std::string( ",\"file_name\":\"" ) + _jesc( clsInfo.m_strFileName ) + "\",\"file_url\":\"" +
                  _jesc( strUrl ) + "\",\"file_size\":" + std::to_string( clsInfo.m_llFileSize ) + ",\"file_type\":\"" +
                  _jesc( clsInfo.m_strFileType ) + "\"";
    }
    strRec += "}";
    gclsCallDir.McDataMessageLog( pszGroup, strRec );
}
