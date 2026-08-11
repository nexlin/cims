
#include "Monitor.h"

#include "CallMap.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "MonitorDefine.h"
#include "ServerService.h"
#include "SipServer.h"
#include "SipServerSetup.h"
#include "SipStatsMonitor.h"
#include "SipTcp.h"
#include "UserMap.h"

CMonitor gclsMonitor;

CMonitor::CMonitor() {
}

CMonitor::~CMonitor() {
}

/**
 * @brief 서버 모니터링 요청 명령 수신 이벤트 핸들러
 * @param pszRequest	요청 명령 문자열
 * @param strResponse 응답 문자열 저장 변수
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CMonitor::RecvRequest( const char *pszRequest, CMonitorString &strResponse ) {
    if ( !strcmp( pszRequest, MC_CALL_MAP_LIST ) ) {
        gclsCallMap.GetString( strResponse );
    } else if ( !strcmp( pszRequest, MC_SIP_SERVER_MAP_LIST ) ) {
        // G10 (2026-04-23): SipServerMap 제거. 빈 응답.
        (void)strResponse;
    } else if ( !strcmp( pszRequest, MC_USER_MAP_LIST ) ) {
        gclsUserMap.GetString( strResponse );
    } else if ( !strcmp( pszRequest, MC_RTP_MAP_LIST ) ) {
        // RTP relay bookkeeping 은 CallMap(relay descriptor)로 통합됨 — CallMap 목록으로 대체.
        gclsCallMap.GetString( strResponse );
    } else if ( !strcmp( pszRequest, MC_DIALOG_MAP_LIST ) ) {
        gclsUserAgent.GetDialogString( strResponse );
    } else if ( !strcmp( pszRequest, MC_SIP_SERVER_LIST ) ) {
        gclsUserAgent.GetServerString( strResponse );
    } else if ( !strcmp( pszRequest, MC_SIP_STACK_COUNT_LIST ) ) {
        gclsUserAgent.m_clsSipStack.GetString( strResponse );
    } else if ( !strcmp( pszRequest, MC_SIP_STATS ) ) {
        gclsSipStatsMonitor.GetString( strResponse );
    }
#ifdef _DEBUG
    else if ( !strcmp( pszRequest, MC_STOP ) ) {
        CLog::Print( LOG_INFO, "stop command recevied" );
        gbStop = true;
    }
#endif
    else {
        return false;
    }

    return true;
}

/**
 * @brief 허용된 클라이언트인가?
 * @param pszIp 클라이언트 IP 주소
 * @returns 허용된 클라이언트이면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CMonitor::IsMonitorIp( const char *pszIp ) {
    return gclsSetup.IsMonitorIp( pszIp );
}
