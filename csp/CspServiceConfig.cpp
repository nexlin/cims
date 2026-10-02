#include "CspServiceConfig.h"

#include "CscEndpointCache.h"
#include "DbManager.h"
#include "HttpClient.h"
#include "Log.h"
#include "SipServerSetup.h"

CCspServiceConfig gclsCspServiceConfig;

bool CCspServiceConfig::Refresh() {
    // N6 (user profile 공통 값 — 결정 D2) 는 문서가 아니라 DB 의 같은 열을 읽는다. CSC 문서 취득과 따로 갱신한다.
    {
        int iOther = 5, iDispatch = 10;
        const bool bRow = gclsDbManager.IsConnected() && gclsDbManager.SelectMcpttN6( iOther, iDispatch );
        {
            std::lock_guard<std::mutex> lock( m_clsMutex );
            m_iMaxCallsN6 = iOther;
            m_iMaxCallsN6Dispatch = iDispatch;
        }
        CLog::Print( LOG_SYSTEM, "[service-config] N6 그 밖=%d 관제=%d%s", iOther, iDispatch,
                     bRow ? "" : " (mcptt_service_config 없음 — 기본값)" );
    }
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
    CspPriorityParams rp;
    ParsePriority( strBody, rp );
    const int iTng2 = ParseEmergencyGroupTimeLimitSec( strBody );
    CspCallTimerParams ct;
    ParseCallTimers( strBody, ct );
    const int iLevels = ParseNumLevelsPriorityHierarchy( strBody );
    const int iAdhocMax = ParseAdhocMaxParticipants( strBody );
    {
        std::lock_guard<std::mutex> lock( m_clsMutex );
        m_clsFloor = f;
        m_clsPriority = rp;
        m_iTng2Sec = iTng2;
        m_clsCallTimers = ct;
        m_iNumLevelsPriority = iLevels;
        m_iAdhocMaxParticipants = iAdhocMax;
    }
    CLog::Print( LOG_SYSTEM, "[service-config] num-levels-priority-hierarchy=%d%s", iLevels < 0 ? 4 : iLevels,
                 iLevels < 0 ? " (문서에 없음 — 스키마 최솟값)" : "" );
    CLog::Print( LOG_SYSTEM,
                 "[service-config] floor 값 적재 (CSC 정본) T1=%d T2=%d T3=%d T7=%d T8=%d T20=%d C7=%d C20=%d (s, "
                 "-1=문서에 없음)",
                 f.iT1Sec, f.iT2Sec, f.iT3Sec, f.iT7Sec, f.iT8Sec, f.iT20Sec, f.iC7, f.iC20 );
    CLog::Print( LOG_SYSTEM, "[service-config] Resource-Priority emergency=%s imminent=%s normal=%s · TNG2=%d s%s",
                 rp.strEmergency.c_str(), rp.strImminentPeril.c_str(), rp.strNormal.c_str(), iTng2,
                 iTng2 > 0 ? "" : " (문서에 없음 — TNG2 없음)" );
    CLog::Print( LOG_SYSTEM,
                 "[service-config] 개별 호 T4=%d 최대=%d/%d(발언권 제어 있음/없음) · 애드혹 T4=%d 일제 T4=%d "
                 "TNG3=%d (s, -1=문서에 없음 — 그 타이머 없음) · 애드혹 초대 상한=%d",
                 ct.iPrivateHangSec, ct.iPrivateMaxFloorSec, ct.iPrivateMaxNoFloorSec, ct.iAdhocHangSec,
                 ct.iAdhocBroadcastHangSec, ct.iAdhocMaxDurationSec, iAdhocMax );
    return true;
}

CspFloorParams CCspServiceConfig::GetFloorParams() {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return m_clsFloor;
}

std::string CCspServiceConfig::ResourcePriorityOf( int iCond ) {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return iCond >= 2 ? m_clsPriority.strEmergency
                      : ( iCond == 1 ? m_clsPriority.strImminentPeril : m_clsPriority.strNormal );
}

int CCspServiceConfig::GetEmergencyGroupTimeLimitSec() {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return m_iTng2Sec;
}

CspCallTimerParams CCspServiceConfig::GetCallTimerParams() {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return m_clsCallTimers;
}

int CCspServiceConfig::GetMaxCallsN6( bool bDispatch ) {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return bDispatch ? m_iMaxCallsN6Dispatch : m_iMaxCallsN6;
}

int CCspServiceConfig::GetAdhocMaxParticipants() {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return m_iAdhocMaxParticipants;
}

int CCspServiceConfig::GetNumLevelsPriorityHierarchy() {
    std::lock_guard<std::mutex> lock( m_clsMutex );
    return m_iNumLevelsPriority < 0 ? 4 : m_iNumLevelsPriority;
}
