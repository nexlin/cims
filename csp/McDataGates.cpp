#include "McDataGates.h"

#include <stdio.h>
#include <time.h>

#include <deque>
#include <mutex>
#include <unordered_map>

#include "CallDir.h"
#include "CscEndpointCache.h"
#include "DbManager.h"
#include "HttpClient.h"
#include "Log.h"
#include "SipServerSetup.h"
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

int McDataGateCheck( const CspPttGroup &clsGroup, const char *pszFrom, bool bFd, int *piWarn ) {
    if ( piWarn ) *piWarn = 0;
    // 규격 순서(TS 24.282 §9.2.2.4.2 6)e)·f) · §9.2.3.4.4 7)c)·d) · §10.2.4.4.2 12)c)·d)) — 멤버십 → 서비스 → 제휴.
    // 게이트 1 — 발신자 그룹 멤버십 (controlling function 검사) — 403 + 116
    bool bMember = false;
    for ( const auto &pUser : clsGroup._pusers )
        if ( pUser && pUser->_id == pszFrom ) bMember = true;
    if ( !bMember ) {
        CLog::Print( LOG_INFO, "McDataGate: from(%s) is not a member of group(%s) — 403 (116)", pszFrom,
                     clsGroup._id.c_str() );
        if ( piWarn ) *piWarn = 116;
        return SIP_FORBIDDEN;
    }

    // 게이트 2 — 그룹문서 mcdata-allow-short-data-service / mcdata-allow-file-distribution (TS 24.481) — 403 + 206 /
    // 213
    if ( bFd ? clsGroup._allowFd == false : clsGroup._allowSds == false ) {
        CLog::Print( LOG_INFO, "McDataGate: group(%s) %s disabled — 403 (%d) from(%s)", clsGroup._id.c_str(),
                     bFd ? "FD" : "SDS", bFd ? 213 : 206, pszFrom );
        if ( piWarn ) *piWarn = bFd ? 213 : 206;
        return SIP_FORBIDDEN;
    }

    // 게이트 3 — 발신자 제휴 (§9.2.2.4.2 6)j) · §9.2.3.4.4 7)g) · §10.2.4.4.2 12)g)). 제휴를 쓰지 않는 그룹은 멤버십이
    //   곧 제휴다. 제휴 저장소에 닿지 못하면 판정할 수 없다 — 500(§9.2.2.4.2 1)), 통과시키지 않는다.
    if ( !clsGroup._requireAffiliation ) return 0;
    if ( !gclsDbManager.IsConnected() ) {
        CLog::Print( LOG_ERROR, "McDataGate: group(%s) from(%s) — 제휴 저장소(DB) 미연결, 제휴 판정 불가 → 500",
                     clsGroup._id.c_str(), pszFrom );
        return SIP_INTERNAL_SERVER_ERROR;
    }
    if ( !gclsDbManager.IsAffiliated( clsGroup._id, pszFrom ) ) {
        CLog::Print( LOG_INFO, "McDataGate: from(%s) is not affiliated to group(%s) — 403 (120)", pszFrom,
                     clsGroup._id.c_str() );
        if ( piWarn ) *piWarn = 120;
        return SIP_FORBIDDEN;
    }
    return 0;
}

int McDataDeliveryTargets( const CspPttGroup &clsGroup, const char *pszFrom, const char *pszGroup,
                           std::vector<std::string> &vecTargets, int *piWarn ) {
    if ( piWarn ) *piWarn = 0;
    vecTargets.clear();
    // 제휴 저장소에 닿지 못하면 대상을 정할 수 없다 — 그룹 전원에게 보내지 않는다(§6.3.4 «affiliated group members
    // only»)
    if ( clsGroup._requireAffiliation && !gclsDbManager.IsConnected() ) {
        CLog::Print( LOG_ERROR, "McDataGate: group(%s) — 제휴 저장소(DB) 미연결, 배포 대상 판정 불가 → 500", pszGroup );
        return SIP_INTERNAL_SERVER_ERROR;
    }
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser || pUser->_id == pszFrom ) continue;
        if ( clsGroup._requireAffiliation && !gclsDbManager.IsAffiliated( pszGroup, pUser->_id ) ) continue;
        vecTargets.push_back( pUser->_id );
    }
    // 발신자 말고 제휴 멤버가 없다 — 보낼 곳이 없는 요청을 200 으로 받지 않는다(발신자 자신은 배포 대상이 아니다)
    if ( vecTargets.empty() ) {
        CLog::Print( LOG_INFO, "McDataGate: group(%s) from(%s) — 제휴 멤버 없음 → 403 (198)", pszGroup, pszFrom );
        if ( piWarn ) *piWarn = 198;
        return SIP_FORBIDDEN;
    }
    return 0;
}

int McDataFdPayloadCheck( const CMcDataSdsInfo &clsInfo, int *piWarn ) {
    *piWarn = 0;
    if ( clsInfo.m_iFdPayloadCount != 1 ) {
        *piWarn = 210;  // 6) Payload IE 는 하나
    } else if ( clsInfo.m_bFdNonFileUrlPayload ) {
        *piWarn = 211;  // 7)a) 내용 형식 = FILEURL
    } else {
        const std::string strBase =
            gclsSetup.m_strFdUrlBase.empty() ? gclsCscEndpointCache.GetServiceUrlBase() : gclsSetup.m_strFdUrlBase;
        // 7)b) 이 서버의 media storage function 파일 — 다른 호스트는 우리 저장소에 없는 파일이다
        if ( !McDataFdUrlIsOurs( clsInfo.m_strFileUrl, strBase ) ) {
            *piWarn = 212;
        } else if ( !gclsSetup.m_strCscInternalToken.empty() ) {
            // 그 파일이 있는가 — §6.7.3.1 HEAD(그 URL 그대로, access token). 제어 기능의 자격 = CSC 내부 API
            // 토큰(콘텐츠 서버가
            //   HEAD 에만 받는다 — mcdata_messaging.md §4.5). 404 = 없음 → 212. 그 밖(401·연결 실패·옛 CSC 405)은
            //   확인하지 못한 것이라 막지 않는다 — 배포는 하고 로그를 남긴다.
            HTTP_HEADER_LIST clsHeaders, clsResp;
            clsHeaders.push_back(
                CHttpHeader( "Authorization", ( "Bearer " + gclsSetup.m_strCscInternalToken ).c_str() ) );
            CHttpClient clsClient;
            const int iSec = ( gclsSetup.m_iCscTimeoutMs + 999 ) / 1000;
            clsClient.SetRecvTimeout( iSec < 1 ? 1 : iSec );
            clsClient.DoHead( clsInfo.m_strFileUrl.c_str(), &clsHeaders, clsResp );
            const int iStatus = clsClient.GetStatusCode();
            if ( iStatus == 404 )
                *piWarn = 212;
            else if ( iStatus != 200 )
                CLog::Print( LOG_ERROR, "McDataGate: FD url(%s) HEAD %d — 파일 존재를 확인하지 못함(배포는 한다)",
                             clsInfo.m_strFileUrl.c_str(), iStatus );
        }
    }
    if ( *piWarn == 0 ) return 0;
    CLog::Print( LOG_INFO, "McDataGate: FD payloads=%d non-fileurl=%d url(%s) — 403 (%d)", clsInfo.m_iFdPayloadCount,
                 clsInfo.m_bFdNonFileUrlPayload ? 1 : 0, clsInfo.m_strFileUrl.c_str(), *piWarn );
    return SIP_FORBIDDEN;
}

const char *McDataWarnText( int iWarn ) {
    switch ( iWarn ) {  // TS 24.282 §4.9 표 4.9-1
        case 113:
            return "group document does not exist";
        case 116:
            return "user is not part of the MCData group";
        case 120:
            return "user is not affiliated to this group";
        case 142:
            return "unable to determine the controlling function";
        case 198:
            return "no users are affiliated to this group";
        case 199:
            return "expected MIME bodies not in the request";
        case 204:
            return "unable to determine targeted user for one-to-one SDS";
        case 205:
            return "unable to determine targeted user for one-to-one FD";
        case 206:
            return "short data service not allowed for this group";
        case 210:
            return "Only one File URL must be present in the FD request";
        case 211:
            return "payload for an FD request is not FILEURL";
        case 212:
            return "file referenced by file URL does not exist";
        case 213:
            return "file distribution not allowed for this group";
        case 217:
            return "user not authorised for SDS communications on this group identity due to message size";
        default:
            return "";
    }
}

void McDataArchiveMessage( const char *pszGroup, const char *pszFrom, const char *pszMsgType,
                           const CMcDataSdsInfo &clsInfo, int iPayloadSize, int iFanout, const char *pszVia,
                           const char *pszFileUrl, bool bMcData ) {
    // disposition 통지 상관 색인 — 보관 여부와 무관하게 기록한다(SDS 만 — FD 통지는 미사용)
    if ( bMcData && clsInfo.m_iMsgType == MCDATA_MSG_SDS_SIGNALLING && !clsInfo.m_strMsgId.empty() )
        McDataRememberSds( clsInfo.m_strConvId, clsInfo.m_strMsgId, pszFrom ? pszFrom : "", pszGroup ? pszGroup : "" );
    if ( !gclsCallDir.IsEnabled() ) return;

    char szEvt[512];
    snprintf( szEvt, sizeof( szEvt ),
              "{\"actor\":\"%s\",\"target\":\"%s\",\"conv_id\":\"%s\",\"msg_id\":\"%s\","
              "\"payload_size\":%d,\"disposition_req\":%d,\"fanout\":%d,\"mcdata\":%s%s%s%s}",
              pszFrom, pszGroup, clsInfo.m_strConvId.c_str(), clsInfo.m_strMsgId.c_str(), iPayloadSize,
              clsInfo.m_iDispositionReq, iFanout, bMcData ? "true" : "false", pszVia && pszVia[0] ? ",\"via\":\"" : "",
              pszVia ? pszVia : "", pszVia && pszVia[0] ? "\"" : "" );
    gclsCallDir.PttLogEvent( pszGroup, "message_sent", szEvt );

    // 메시지 보관 — <recordings>/message/{gid}/{시간버킷}/messages.jsonl (콘솔 모니터링 SoT)
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

// ── disposition 통지 상관 색인 (TS 24.282 §12.2.3 4)) ──────────────────────────────────────────────
namespace {
    struct SdsOrigin {
        std::string strSender, strGroup;
        time_t tAt = 0;
    };
    std::mutex g_mtxSdsIndex;
    std::unordered_map<std::string, SdsOrigin> g_mapSdsIndex;
    std::deque<std::pair<std::string, time_t>> g_dqSdsOrder;  // 삽입 순 — 시한·상한 초과분을 앞에서 버린다
    constexpr size_t kSdsIndexMax = 20000;
    constexpr time_t kSdsIndexTtl = 24 * 3600;
}  // namespace

void McDataRememberSds( const std::string &strConvId, const std::string &strMsgId, const std::string &strSender,
                        const std::string &strGroup ) {
    const std::string strKey = strConvId + ":" + strMsgId;
    const time_t tNow = time( NULL );
    std::lock_guard<std::mutex> lock( g_mtxSdsIndex );
    SdsOrigin &o = g_mapSdsIndex[strKey];
    o.strSender = strSender;
    o.strGroup = strGroup;
    o.tAt = tNow;
    g_dqSdsOrder.push_back( { strKey, tNow } );
    while ( !g_dqSdsOrder.empty() &&
            ( g_dqSdsOrder.size() > kSdsIndexMax || tNow - g_dqSdsOrder.front().second > kSdsIndexTtl ) ) {
        auto it = g_mapSdsIndex.find( g_dqSdsOrder.front().first );
        // 같은 키가 다시 기록됐으면(재전송) 최신 기록은 남긴다
        if ( it != g_mapSdsIndex.end() && it->second.tAt == g_dqSdsOrder.front().second ) g_mapSdsIndex.erase( it );
        g_dqSdsOrder.pop_front();
    }
}

bool McDataCorrelateSds( const std::string &strConvId, const std::string &strMsgId, std::string &strSender,
                         std::string &strGroup ) {
    std::lock_guard<std::mutex> lock( g_mtxSdsIndex );
    auto it = g_mapSdsIndex.find( strConvId + ":" + strMsgId );
    if ( it == g_mapSdsIndex.end() || time( NULL ) - it->second.tAt > kSdsIndexTtl ) return false;
    strSender = it->second.strSender;
    strGroup = it->second.strGroup;
    return true;
}
