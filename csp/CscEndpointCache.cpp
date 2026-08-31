#include "CscEndpointCache.h"

#include <stdio.h>

#include "CspAddressing.h"
#include "HttpClient.h"
#include "Log.h"
#include "SimpleJson.h"
#include "SipServerSetup.h"

CCscEndpointCache gclsCscEndpointCache;

// 조회 실패 후 재시도 간격 — CSC 가 내려가 있는 동안 NOTIFY 마다 HTTP 를 시도하지 않는다.
static const int kRetryIntervalSec = 30;
// CSC McpttServer 기본 포트 (유도값 전용 — 정상 경로는 CSC 응답의 xcap_root 를 쓴다).
static const int kDefaultMcpttPort = 4430;

namespace CscEndpoint {

    std::string AdminBaseUrl() {
        std::string strHost = gclsSetup.m_strCscHost;
        if ( strHost.empty() ) strHost = gclsSetup.m_strLocalIp;
        const std::string strScheme = gclsSetup.m_strCscScheme.empty() ? "https" : gclsSetup.m_strCscScheme;
        char szPort[16];
        snprintf( szPort, sizeof( szPort ), "%d", gclsSetup.m_iCscPort > 0 ? gclsSetup.m_iCscPort : 4421 );
        return strScheme + "://" + strHost + ":" + szPort;
    }

}  // namespace CscEndpoint

std::string CCscEndpointCache::Derive() {
    std::string strHost = gclsSetup.m_strCscHost;
    if ( strHost.empty() ) strHost = gclsSetup.m_strLocalIp;
    char szPort[16];
    snprintf( szPort, sizeof( szPort ), "%d", kDefaultMcpttPort );
    return std::string( "https://" ) + strHost + ":" + szPort + "/";
}

bool CCscEndpointCache::Fetch( std::string &strOut ) {
    if ( gclsSetup.m_strCscInternalToken.empty() ) {
        CLog::Print( LOG_ERROR, "[topology] Setup.Csc.InternalToken 미설정 — MCPTT endpoint 조회 불가" );
        return false;
    }

    HTTP_HEADER_LIST clsHeaders;
    clsHeaders.push_back( CHttpHeader( "Authorization", ( "Bearer " + gclsSetup.m_strCscInternalToken ).c_str() ) );

    CHttpClient clsClient;
    int iSec = ( gclsSetup.m_iCscTimeoutMs + 999 ) / 1000;
    clsClient.SetRecvTimeout( iSec < 1 ? 1 : iSec );

    const std::string strUrl = CscEndpoint::AdminBaseUrl() + "/internal/mcptt/endpoint";
    std::string strOutType, strBody;
    const bool bSent = clsClient.DoGet( strUrl.c_str(), &clsHeaders, strOutType, strBody );
    const int iStatus = clsClient.GetStatusCode();
    // DoGet 은 비-2xx 에도 false 를 준다 — 상태코드가 있으면 응답은 받은 것이므로 구분해 로그한다.
    if ( iStatus != 200 ) {
        const char *pszHint = "";
        if ( iStatus == 401 )
            pszHint = " — Setup.Csc.InternalToken 이 csc.json InternalApi.Token 과 다름";
        else if ( iStatus == 404 )
            pszHint = " — 구 CSC(내부 API 없음). csc 동반 업그레이드 필요";
        else if ( iStatus == 503 )
            pszHint = " — CSC 의 InternalApi.Token 미설정";
        if ( iStatus > 0 ) {
            CLog::Print( LOG_ERROR, "[topology] MCPTT endpoint 응답 %d url=%s%s", iStatus, strUrl.c_str(), pszHint );
        } else {
            CLog::Print( LOG_ERROR, "[topology] MCPTT endpoint 조회 실패 url=%s (connect/timeout)", strUrl.c_str() );
        }
        return false;
    }
    if ( !bSent ) {
        CLog::Print( LOG_ERROR, "[topology] MCPTT endpoint 응답 처리 실패 url=%s", strUrl.c_str() );
        return false;
    }

    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( strBody );
    std::string strRoot = root.Has( "xcap_root" ) ? root.GetString( "xcap_root" ) : "";
    if ( strRoot.compare( 0, 4, "http" ) != 0 ) {
        CLog::Print( LOG_ERROR, "[topology] MCPTT endpoint 응답에 xcap_root 없음/형식 오류 (url=%s)", strUrl.c_str() );
        return false;
    }
    if ( strRoot[strRoot.size() - 1] != '/' ) strRoot += "/";
    strOut = strRoot;
    return true;
}

bool CCscEndpointCache::Refresh() {
    std::string strRoot;
    const bool bOk = Fetch( strRoot );

    std::lock_guard<std::mutex> lock( m_clsMutex );
    m_tLastAttempt = time( NULL );
    if ( bOk ) {
        if ( m_strXcapRoot != strRoot ) {
            CLog::Print( LOG_SYSTEM, "[topology] MCPTT xcap-root = %s (CSC 정본)", strRoot.c_str() );
        }
        m_strXcapRoot = strRoot;
        return true;
    }
    if ( m_strXcapRoot.empty() ) {
        CLog::Print( LOG_ERROR, "[topology] xcap-root 미취득 — 설정 유도값 %s 사용 (단말 문서 취득이 실패할 수 있음)",
                     Derive().c_str() );
    }
    return false;
}

std::string CCscEndpointCache::GetXcapRoot() {
    {
        std::lock_guard<std::mutex> lock( m_clsMutex );
        if ( !m_strXcapRoot.empty() ) return m_strXcapRoot;
        // 직전 시도가 최근이면 재시도하지 않는다 (SIP 스레드 동기 I/O 스탬피드 방지).
        if ( m_tLastAttempt != 0 && time( NULL ) - m_tLastAttempt < kRetryIntervalSec ) return Derive();
        m_tLastAttempt = time( NULL );  // 시도 선점 — 동시 진입 스레드는 유도값으로 진행
    }

    std::string strRoot;
    if ( Fetch( strRoot ) ) {
        std::lock_guard<std::mutex> lock( m_clsMutex );
        CLog::Print( LOG_SYSTEM, "[topology] MCPTT xcap-root = %s (CSC 정본)", strRoot.c_str() );
        m_strXcapRoot = strRoot;
        return strRoot;
    }

    std::lock_guard<std::mutex> lock( m_clsMutex );
    if ( !m_strXcapRoot.empty() ) return m_strXcapRoot;
    return Derive();
}

std::string CCscEndpointCache::GetServiceUrlBase() {
    std::string strRoot = GetXcapRoot();
    if ( !strRoot.empty() && strRoot[strRoot.size() - 1] == '/' ) strRoot.erase( strRoot.size() - 1 );
    return strRoot;
}
