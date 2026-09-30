#include "CspServiceConfig.h"

#include "CscEndpointCache.h"
#include "HttpClient.h"
#include "Log.h"
#include "SipServerSetup.h"

CCspServiceConfig gclsCspServiceConfig;

bool CCspServiceConfig::Refresh() {
    if ( gclsSetup.m_strCscInternalToken.empty() ) {
        CLog::Print( LOG_ERROR,
                     "[service-config] Setup.Csc.InternalToken 미설정 — 문서 취득 불가(CMP floor 값은 CMP 설정)" );
        return false;
    }
    // 조건부 요청(If-None-Match)은 쓰지 않는다 — CHttpClient 가 응답 ETag 를 노출하지 않고, 문서 1건·수 KB 에 계기가
    // 드물다.
    HTTP_HEADER_LIST clsHeaders;
    clsHeaders.push_back( CHttpHeader( "Authorization", ( "Bearer " + gclsSetup.m_strCscInternalToken ).c_str() ) );

    CHttpClient clsClient;
    const int iSec = ( gclsSetup.m_iCscTimeoutMs + 999 ) / 1000;
    clsClient.SetRecvTimeout( iSec < 1 ? 1 : iSec );
    const std::string strUrl = CscEndpoint::AdminBaseUrl() + "/internal/mcptt/service-config";
    std::string strOutType, strBody;
    clsClient.DoGet( strUrl.c_str(), &clsHeaders, strOutType, strBody );
    const int iStatus = clsClient.GetStatusCode();
    if ( iStatus != 200 ) {
        CLog::Print( LOG_ERROR, "[service-config] 응답 %d url=%s%s — 이전 값 유지", iStatus, strUrl.c_str(),
                     iStatus == 404 ? " (구 CSC — csc 0.2.134 이상 필요)" : "" );
        return false;
    }
    CspFloorParams f;
    if ( !Parse( strBody, f ) ) {
        CLog::Print( LOG_ERROR, "[service-config] 문서 해석 실패(루트 service-configuration-info 없음) url=%s",
                     strUrl.c_str() );
        return false;
    }
    {
        std::lock_guard<std::mutex> lock( m_clsMutex );
        m_clsFloor = f;
    }
    CLog::Print( LOG_SYSTEM,
                 "[service-config] floor 값 적재 (CSC 정본) T1=%d T2=%d T3=%d T7=%d T8=%d T20=%d C7=%d C20=%d (s, "
                 "-1=문서에 없음)",
                 f.iT1Sec, f.iT2Sec, f.iT3Sec, f.iT7Sec, f.iT8Sec, f.iT20Sec, f.iC7, f.iC20 );
    return true;
}

CspFloorParams CCspServiceConfig::GetFloorParams() {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return m_clsFloor;
}
