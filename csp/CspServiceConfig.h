/*
 * CspServiceConfig — MCPTT service configuration 문서(TS 24.484 §8.4) 의 서버 측 사본
 *
 *   GET {CSC admin}/internal/mcptt/service-config
 *       Authorization: Bearer {Setup.Csc.InternalToken}
 *   → application/vnd.3gpp.mcptt-service-config+xml (단말이 받는 문서와 같은 XML)
 *
 * MCPTT 서버가 CMS 에서 service-config 을 받아 쓰는 경로(TS 24.484 Annex A.2.3)다. CSP 가 쓰는 값:
 *  - floor 제어 서버 파라미터 — on-network <transmit-time><time-limit>(T2) 와 <fc-timers-counters>(T1·T3·T7·T8·T20·
 *    C7·C20, TS 24.380). 그룹 세션의 PTT_GROUP_ADD/MODIFY floor_timers 로 CMP 에 싣는다(cmp_media_api.md §7.7). CMP
 *    설정 Floor*Sec 는 문서를 아직 못 받았을 때의 폴백이다.
 *  - Resource-Priority — <emergency-/imminent-peril-/normal-resource-priority> 의 namespace·priority (TS 24.379
 *    §6.3.3.1.19). 멤버 fan-out·조건 재광고가 싣는다. 문서에 없으면 mcpttp.15/8/0.
 *  - TNG2(진행 중 긴급 그룹콜 타이머) — <emergency-call><group-time-limit> (TS 24.379 §6.3.3.1.16·부속서 F 타이머 표).
 *    없으면 TNG2 를 돌리지 않는다.
 *
 * 갱신 시점: 기동 · SIGUSR1 reload · CSC_RESTART · SERVICE_CONFIG_CHANGED. 실패하면 마지막 성공값을 유지한다.
 */
#ifndef _CSP_SERVICE_CONFIG_H_
#define _CSP_SERVICE_CONFIG_H_

#include <stdlib.h>

#include <mutex>
#include <string>

#include "McpttInfo.h"

/** CMP floor_timers 로 가는 값 (CMP 는 초 단위 — 1 s 미만은 버린다). -1 = 문서에 없음(CMP 설정값을 쓴다). */
struct CspFloorParams {
    bool bValid = false;  // 문서를 한 번이라도 받았다
    int iT1Sec = -1;      // T1-end-of-rtp-media
    int iT2Sec = -1;      // transmit-time/time-limit
    int iT3Sec = -1;      // T3-stop-talking-grace
    int iT7Sec = -1;      // T7-floor-idle
    int iT8Sec = -1;      // T8-floor-revoke
    int iT20Sec = -1;     // T20-floor-granted
    int iC7 = -1;         // C7-floor-idle
    int iC20 = -1;        // C20-floor-granted
};

/** Resource-Priority 헤더 값 "namespace.priority" (RFC 8101, TS 24.379 §6.3.3.1.19). 문서에 없는 항목은 기본값. */
struct CspPriorityParams {
    std::string strEmergency = "mcpttp.15";
    std::string strImminentPeril = "mcpttp.8";
    std::string strNormal = "mcpttp.0";
};

class CCspServiceConfig {
public:
    /** CSC 에서 문서를 다시 받는다. 실패면 기존 값을 유지한다. @return 새 값을 적재했으면 true */
    bool Refresh();

    CspFloorParams GetFloorParams();

    /** 조건(2=긴급·1=임박·0=일반)의 Resource-Priority 값 — 멤버 fan-out·조건 재광고 공용 (TS 24.379 §6.3.3.1.19). */
    std::string ResourcePriorityOf( int iCond );

    /** TNG2 초 (<emergency-call><group-time-limit>) — 0 이하 = 문서에 없음(TNG2 를 돌리지 않는다). */
    int GetEmergencyGroupTimeLimitSec();

    /** xs:duration("PT<h>H<m>M<s>S", 초는 소수 허용) → 밀리초. 형식 오류면 -1. */
    static long long DurationMs( const std::string &strDuration );

    /** 문서 → floor 값. 루트가 service-configuration-info 가 아니면 false. */
    static bool Parse( const std::string &strXml, CspFloorParams &clsOut );

    /** 문서 → Resource-Priority 값. namespace·priority 둘 다 있는 항목만 바꾼다. */
    static void ParsePriority( const std::string &strXml, CspPriorityParams &clsOut );

    /** 문서 → on-network <emergency-call><group-time-limit> 초. 없거나 형식 오류면 -1. */
    static int ParseEmergencyGroupTimeLimitSec( const std::string &strXml );

private:
    std::mutex m_clsMutex;
    CspFloorParams m_clsFloor;
    CspPriorityParams m_clsPriority;
    int m_iTng2Sec = -1;
};

extern CCspServiceConfig gclsCspServiceConfig;

// ── 순수 해석 (헤더 인라인 — S1-UNIT-CSP tests/csp_service_config_test.cpp) ──

inline long long CCspServiceConfig::DurationMs( const std::string &strDuration ) {
    // TS 24.484 §8.4.2.6 — "PT<h>H<m>M<n>S" (초는 소수 허용). 날짜부(nY/nM/nD)는 이 문서에서 쓰지 않는다.
    size_t p = strDuration.find( "PT" );
    if ( p == std::string::npos ) return -1;
    p += 2;
    double dMs = 0;
    bool bAny = false;
    while ( p < strDuration.size() ) {
        char *pszEnd = NULL;
        const double d = strtod( strDuration.c_str() + p, &pszEnd );
        if ( pszEnd == strDuration.c_str() + p || *pszEnd == '\0' ) return -1;
        const char cUnit = *pszEnd;
        if ( cUnit == 'H' )
            dMs += d * 3600000.0;
        else if ( cUnit == 'M' )
            dMs += d * 60000.0;
        else if ( cUnit == 'S' )
            dMs += d * 1000.0;
        else
            return -1;
        bAny = true;
        p = ( pszEnd - strDuration.c_str() ) + 1;
    }
    if ( !bAny || dMs < 0 ) return -1;
    return (long long)( dMs + 0.5 );
}

inline int _CspScSec( const std::string &strXml, const char *pszTag ) {
    std::string v;
    if ( !McpttElemValue( strXml, pszTag, v ) ) return -1;
    const long long ms = CCspServiceConfig::DurationMs( v );
    return ms < 0 ? -1 : (int)( ms / 1000 );
}

inline int _CspScCount( const std::string &strXml, const char *pszTag ) {
    std::string v;
    if ( !McpttElemValue( strXml, pszTag, v ) || v.empty() ) return -1;
    char *pszEnd = NULL;
    const long l = strtol( v.c_str(), &pszEnd, 10 );
    return ( pszEnd == v.c_str() || l < 0 ) ? -1 : (int)l;
}

/** 요소 구간(시작 태그 ~ 끝 태그) — 접두사 무관, 이름 경계까지 일치하는 첫 요소. 없으면 빈 문자열. */
inline std::string _CspScSection( const std::string &strXml, const char *pszTag ) {
    const std::string n = pszTag;
    size_t p = 0;
    while ( ( p = strXml.find( n, p ) ) != std::string::npos ) {
        const size_t e = p + n.size();
        const bool bNameEnd = e < strXml.size() && ( strXml[e] == '>' || strXml[e] == ' ' || strXml[e] == '\t' ||
                                                     strXml[e] == '\r' || strXml[e] == '\n' || strXml[e] == '/' );
        const bool bOpen = p > 0 && ( strXml[p - 1] == '<' || strXml[p - 1] == ':' );
        if ( !bNameEnd || !bOpen ) {
            p = e;
            continue;
        }
        const size_t q = strXml.rfind( '<', p );
        if ( q == std::string::npos || strXml.compare( q, 2, "</" ) == 0 ) {  // 끝 태그에서 시작하지 않는다
            p = e;
            continue;
        }
        const size_t z = strXml.find( "/" + n, e );
        return strXml.substr( q, z == std::string::npos ? std::string::npos : z + n.size() + 2 - q );
    }
    return std::string();
}

/** on-network 구간 — off-network 에도 transmit-time 등이 있다(§8.4.2.1). */
inline std::string _CspScOnNetwork( const std::string &strXml ) {
    size_t a = strXml.find( "on-network" );
    size_t b = ( a == std::string::npos ) ? std::string::npos : strXml.find( "on-network", a + 10 );
    return ( a == std::string::npos ) ? std::string()
                                      : strXml.substr( a, b == std::string::npos ? std::string::npos : b - a );
}

inline void CCspServiceConfig::ParsePriority( const std::string &strXml, CspPriorityParams &clsOut ) {
    const std::string strOn = _CspScOnNetwork( strXml );
    auto rp = [&]( const char *pszElem, std::string &strOut ) {
        const std::string strSec = _CspScSection( strOn, pszElem );
        std::string strNs, strPrio;
        if ( strSec.empty() || !McpttElemValue( strSec, "resource-priority-namespace", strNs ) ||
             !McpttElemValue( strSec, "resource-priority-priority", strPrio ) || strNs.empty() || strPrio.empty() )
            return;
        strOut = strNs + "." + strPrio;  // RFC 4412 r-value = namespace "." r-priority
    };
    rp( "emergency-resource-priority", clsOut.strEmergency );
    rp( "imminent-peril-resource-priority", clsOut.strImminentPeril );
    rp( "normal-resource-priority", clsOut.strNormal );
}

inline int CCspServiceConfig::ParseEmergencyGroupTimeLimitSec( const std::string &strXml ) {
    const std::string strSec = _CspScSection( _CspScOnNetwork( strXml ), "emergency-call" );
    return strSec.empty() ? -1 : _CspScSec( strSec, "group-time-limit" );
}

inline bool CCspServiceConfig::Parse( const std::string &strXml, CspFloorParams &clsOut ) {
    std::string strTmp;
    if ( !McpttElemValue( strXml, "service-configuration-info", strTmp ) ) return false;
    const std::string strOn = _CspScOnNetwork( strXml );
    CspFloorParams f;
    f.bValid = true;
    f.iT1Sec = _CspScSec( strOn, "T1-end-of-rtp-media" );
    f.iT2Sec = _CspScSec( strOn, "time-limit" );
    f.iT3Sec = _CspScSec( strOn, "T3-stop-talking-grace" );
    f.iT7Sec = _CspScSec( strOn, "T7-floor-idle" );
    f.iT8Sec = _CspScSec( strOn, "T8-floor-revoke" );
    f.iT20Sec = _CspScSec( strOn, "T20-floor-granted" );
    f.iC7 = _CspScCount( strOn, "C7-floor-idle" );
    f.iC20 = _CspScCount( strOn, "C20-floor-granted" );
    clsOut = f;
    return true;
}

#endif
