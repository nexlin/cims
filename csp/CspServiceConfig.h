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
 *  - 개별 호·애드혹 그룹 호의 세션 타이머 — 그룹 문서가 없는 호라 값이 이 문서에 있다(TS 24.380 표 11.1.3-1 T4 출처).
 *    개별 호 = <private-call><hang-time>(T4)·<max-duration-with-floor-control>·<max-duration-without-floor-control>
 *    (TS 24.379 §6.3.8.2), 애드혹 = <anyExt><adhoc-group-call><hang-time>(T4)·<broadcast-hang-time>(일제 애드혹 T4)·
 *    <max-duration-of-call>(TNG3, §17.4.2.2 13)). 요소가 없으면 그 타이머를 돌리지 않는다. 호 종류별 선택 규칙은
 *    CspSessionT4Sec·CspSessionMaxDurationSec(아래)가 한곳에서 정한다.
 *
 * 갱신 시점: 기동 · SIGUSR1 reload · CSC_RESTART · SERVICE_CONFIG_CHANGED. 실패하면 마지막 성공값을 유지한다.
 *
 * 같은 CMS 문서 축의 변경 통지 — UE initial configuration(TS 24.484 §7.2) 변경(UE_INIT_CONFIG_CHANGED)은 cms 구독
 * 단말마다 그 단말의 문서 선택자로 xcap-diff 를 낸다(CspUeInitConfigSelector · CspXcapDiffDocBody, §7.2.2.12 →
 * §6.3.13.3, RFC 5875).
 */
#ifndef _CSP_SERVICE_CONFIG_H_
#define _CSP_SERVICE_CONFIG_H_

#include <ctype.h>
#include <stdlib.h>

#include <mutex>
#include <string>
#include <vector>

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

/** 개별 호·애드혹 그룹 호의 세션 타이머 (초, 1 s 미만은 버린다). -1 = 문서에 없음(그 타이머를 돌리지 않는다). */
// MCVideo service configuration 의 전송 제어 서버 타이머·카운터 — on-network <anyExt><tc-timers-counters-R14>(TS 24.484
// §9.4.2.1
//   on-network 6 d)·TS 24.581 표 11.1.3-1·§11.2.3). 그룹 호의 T1·T5 는 그룹 문서 값이라 여기 없다(§11.1.3). -1 = 문서에
//   없음(CMP K5 기본값).
struct CspMcvTcParams {
    int iT2Ms = -1, iT3Ms = -1, iT4Ms = -1, iT6Ms = -1, iT11Ms = -1;
    int iC2 = -1, iC4 = -1, iC6 = -1, iC7 = -1, iC11 = -1;
};

struct CspCallTimerParams {
    int iPrivateHangSec = -1;         // <private-call><hang-time> — 개별 호 T4 (TS 24.380 표 11.1.3-1)
    int iPrivateMaxFloorSec = -1;     // <private-call><max-duration-with-floor-control> (TS 24.379 §6.3.8.2 2))
    int iPrivateMaxNoFloorSec = -1;   // <private-call><max-duration-without-floor-control>
    int iAdhocHangSec = -1;           // <anyExt><adhoc-group-call><hang-time> — 애드혹 그룹 호 T4
    int iAdhocBroadcastHangSec = -1;  // <adhoc-group-call><broadcast-hang-time> — 일제 애드혹 그룹 호 T4
    int iAdhocMaxDurationSec = -1;    // <adhoc-group-call><max-duration-of-call> — 애드혹 그룹 호 TNG3 (§17.4.2.2 13))
};

/** 호 종류 — T4·최대 시간의 출처와 해제 정책이 호 종류별이다 (TS 24.380 표 11.1.3-1 · TS 24.379 §6.3.8). */
enum class ECspCallKind {
    Prearranged,  // 편성 그룹 호(일제 통화 포함) — 그룹 문서
    Chat,         // chat 그룹 호 — 상시 세션
    Adhoc,        // 애드혹 그룹 호 — service configuration <adhoc-group-call>
    Private       // 개별 호 — service configuration <private-call>
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

    /** 개별 호·애드혹 그룹 호의 세션 타이머 (<private-call> · <anyExt><adhoc-group-call>). */
    CspCallTimerParams GetCallTimerParams();

    /** 사용자의 MCPTT 동시 그룹 호 상한 N6 (TS 24.484 §8.3.2.1 `<MaxSimultaneousCallsN6>` — 결정 D2: 관제 = 역할 배정이
     * 있는 회선, 그 밖 = 일반). 값 = mcptt_service_config(DB, CSC user profile 과 같은 열), 없으면 관제 10 · 그 밖 5.
     * Refresh 가 다시 읽는다(기동·SERVICE_CONFIG_CHANGED). */
    int GetMaxCallsN6( bool bDispatch );

    /** on-network <num-levels-priority-hierarchy> (TS 24.484 §8.4.2 6) — 4~256). 문서에 없으면 스키마의 가장 낮은 값 4
     * 다 (같은 절 «Absence … indicates that the lowest possible value is used»). floor 우선순위 협상의 상한(TS 24.380
     * §14.3.3). */
    int GetNumLevelsPriorityHierarchy();

    /** 문서 → on-network <num-levels-priority-hierarchy>. 없거나 범위(4~256) 밖이면 -1. */
    static int ParseNumLevelsPriorityHierarchy( const std::string &strXml );

    /** 애드혹 그룹 호 초대 인원 상한 — on-network <anyExt><adhoc-group-call><max-no-participants> (TS 24.484 §8.4.2.1
     *  13)d)). 0 이하 = 문서에 없음(상한을 두지 않는다). 개시 INVITE·참가자 변경 re-INVITE 가 넘으면 403 189
     *  (TS 24.379 §17.4.2.2 6) · §17.4.5.1.1 4)a)i)). */
    int GetAdhocMaxParticipants();

    /** 문서 → <adhoc-group-call><max-no-participants>. 없거나 1 미만이면 -1. */
    static int ParseAdhocMaxParticipants( const std::string &strXml );

    /** xs:duration("PT<h>H<m>M<s>S", 초는 소수 허용) → 밀리초. 형식 오류면 -1. */
    static long long DurationMs( const std::string &strDuration );

    /** 문서 → floor 값. 루트가 service-configuration-info 가 아니면 false. */
    static bool Parse( const std::string &strXml, CspFloorParams &clsOut );

    /** 문서 → Resource-Priority 값. namespace·priority 둘 다 있는 항목만 바꾼다. */
    static void ParsePriority( const std::string &strXml, CspPriorityParams &clsOut );

    /** 문서 → on-network <emergency-call><group-time-limit> 초. 없거나 형식 오류면 -1. */
    static int ParseEmergencyGroupTimeLimitSec( const std::string &strXml );

    /** 문서 → on-network <private-call>·<anyExt><adhoc-group-call> 의 세션 타이머. 없는 요소는 -1. */
    static void ParseCallTimers( const std::string &strXml, CspCallTimerParams &clsOut );

    /** MCVideo service configuration(CSC `/internal/mcvideo/service-config`)을 다시 받는다 — 전송 제어 서버
     * 타이머·카운터를 CMP 그룹 ADD 의 tc_timers 로 싣는다(다음 호부터). 실패하면 이전 값 유지. */
    bool RefreshMcVideo();
    CspMcvTcParams GetMcVideoTcParams();

    /** MCVideo service configuration 문서 → <tc-timers-counters-R14> 서버 값. 루트가 service-configuration-info 가
     * 아니면 false. C7 은 XSD 철자(`C7-reception-accpeted`)와 본문 철자(`C7-reception-accepted`)를 둘 다 받는다. */
    static bool ParseMcVideoTc( const std::string &strXml, CspMcvTcParams &clsOut );

private:
    std::mutex m_clsMutex;
    CspFloorParams m_clsFloor;
    CspPriorityParams m_clsPriority;
    int m_iTng2Sec = -1;
    CspCallTimerParams m_clsCallTimers;
    int m_iMaxCallsN6 = 5;             ///< 그 밖 단말 N6 (mcptt_service_config.max_calls_n6)
    int m_iMaxCallsN6Dispatch = 10;    ///< 관제 N6 (max_calls_n6_dispatch)
    int m_iNumLevelsPriority = -1;     ///< <num-levels-priority-hierarchy> (-1 = 문서에 없음 → 4)
    int m_iAdhocMaxParticipants = -1;  ///< <adhoc-group-call><max-no-participants> (-1 = 문서에 없음 → 상한 없음)
    CspMcvTcParams m_clsMcvTc;         ///< MCVideo <tc-timers-counters-R14> 서버 값
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

inline int CCspServiceConfig::ParseNumLevelsPriorityHierarchy( const std::string &strXml ) {
    const int n = _CspScCount( _CspScOnNetwork( strXml ), "num-levels-priority-hierarchy" );
    return ( n >= 4 && n <= 256 ) ? n : -1;
}

inline int CCspServiceConfig::ParseAdhocMaxParticipants( const std::string &strXml ) {
    const int n = _CspScCount( _CspScSection( _CspScOnNetwork( strXml ), "adhoc-group-call" ), "max-no-participants" );
    return n >= 1 ? n : -1;
}

inline bool CCspServiceConfig::ParseMcVideoTc( const std::string &strXml, CspMcvTcParams &clsOut ) {
    if ( strXml.find( "service-configuration-info" ) == std::string::npos ) return false;
    const std::string s = _CspScSection( strXml, "tc-timers-counters-R14" );
    if ( s.empty() ) return true;
    auto ms = [&s]( const char *pszTag ) {
        std::string v;
        if ( !McpttElemValue( s, pszTag, v ) ) return -1;
        const long long l = CCspServiceConfig::DurationMs( v );
        return l < 0 || l > 3600000 ? -1 : (int)l;
    };
    clsOut.iT2Ms = ms( "T2-transmission-idle" );
    clsOut.iT3Ms = ms( "T3-transmission-revoke" );
    clsOut.iT4Ms = ms( "T4-transmission-granted" );
    clsOut.iT6Ms = ms( "T6-reception-granted" );
    clsOut.iT11Ms = ms( "T11-stream-reception-idle" );
    clsOut.iC2 = _CspScCount( s, "C2-transmission-idle" );
    clsOut.iC4 = _CspScCount( s, "C4-transmission-granted" );
    clsOut.iC6 = _CspScCount( s, "C6-reception-granted" );
    clsOut.iC7 = _CspScCount( s, "C7-reception-accpeted" );
    if ( clsOut.iC7 < 0 ) clsOut.iC7 = _CspScCount( s, "C7-reception-accepted" );
    clsOut.iC11 = _CspScCount( s, "C11-media-receivers" );
    return true;
}

inline void CCspServiceConfig::ParseCallTimers( const std::string &strXml, CspCallTimerParams &clsOut ) {
    // 요소 이름은 정확히 맞춘다(McpttElemValue) — <hang-time> 은 <broadcast-hang-time>·<hang-time-warning> 이 아니다
    const std::string strOn = _CspScOnNetwork( strXml );
    const std::string strPriv = _CspScSection( strOn, "private-call" );
    const std::string strAdhoc = _CspScSection( strOn, "adhoc-group-call" );  // <anyExt> 아래 (§8.4.2.1 13)d))
    CspCallTimerParams p;
    if ( !strPriv.empty() ) {
        p.iPrivateHangSec = _CspScSec( strPriv, "hang-time" );
        p.iPrivateMaxFloorSec = _CspScSec( strPriv, "max-duration-with-floor-control" );
        p.iPrivateMaxNoFloorSec = _CspScSec( strPriv, "max-duration-without-floor-control" );
    }
    if ( !strAdhoc.empty() ) {
        p.iAdhocHangSec = _CspScSec( strAdhoc, "hang-time" );
        p.iAdhocBroadcastHangSec = _CspScSec( strAdhoc, "broadcast-hang-time" );
        p.iAdhocMaxDurationSec = _CspScSec( strAdhoc, "max-duration-of-call" );
    }
    clsOut = p;
}

/** 세션 T4(Inactivity) 초 — 0 = 걸지 않는다. 출처는 호 종류별 하나다(TS 24.380 표 11.1.3-1): 편성 그룹 호 = 그룹 문서
 *  <on-network-hang-timer>(iGroupHangSec) · 애드혹 그룹 호 = <adhoc-group-call><hang-time>(일제 통화면
 *  <broadcast-hang-time>) · 개별 호 = <private-call><hang-time> — 발언권 제어 없는 개별 호(full-duplex)는 T4 를 돌릴
 *  floor 제어 서버가 없어 0 · chat = 0 (T4 만료 해제 목록에 없다 — TS 24.379 §6.3.8.1 1)). */
inline int CspSessionT4Sec( ECspCallKind eKind, int iGroupHangSec, bool bBroadcast, bool bFloorControl,
                            const CspCallTimerParams &p ) {
    int iSec = 0;
    if ( eKind == ECspCallKind::Prearranged )
        iSec = iGroupHangSec;
    else if ( eKind == ECspCallKind::Adhoc )
        iSec = bBroadcast ? p.iAdhocBroadcastHangSec : p.iAdhocHangSec;
    else if ( eKind == ECspCallKind::Private && bFloorControl )
        iSec = p.iPrivateHangSec;
    return iSec > 0 ? iSec : 0;
}

/** 세션 최대 시간 초 — 0 = 세지 않는다. iStartCond = 세션을 개시한 INVITE 의 조건, iCond = 지금 조건
 *  (2=긴급·1=임박·0=없음).
 *  - 편성 그룹 호 = TNG3, 그룹 문서 <on-network-maximum-duration>(iGroupMaxSec — 0 = 무제한).
 *  - 애드혹 그룹 호 = TNG3, <adhoc-group-call><max-duration-of-call> — 긴급·임박으로 개시한 호(priority adhoc group
 *    call)에는 걸지 않는다(TS 24.379 §17.4.2.2 13)). 두 그룹 호 모두 긴급 상태 동안은 TNG2 가 대신한다(§6.3.3.5.2).
 *  - 개별 호 = 발언권 제어 유무별 «maximum of duration of private call»(§6.3.8.2 2)·§11.1.1.4.1 10)) — 조건과 무관.
 *  - chat = 0 — TNG3 를 돌리지 않는다(그룹 문서에 요소를 싣지 않는다, mcptt_timers.md §7 D7). */
inline int CspSessionMaxDurationSec( ECspCallKind eKind, int iGroupMaxSec, bool bFloorControl, int iStartCond,
                                     int iCond, const CspCallTimerParams &p ) {
    int iSec = 0;
    if ( eKind == ECspCallKind::Prearranged )
        iSec = iCond >= 2 ? 0 : iGroupMaxSec;
    else if ( eKind == ECspCallKind::Adhoc )
        iSec = ( iCond >= 2 || iStartCond >= 1 ) ? 0 : p.iAdhocMaxDurationSec;
    else if ( eKind == ECspCallKind::Private )
        iSec = bFloorControl ? p.iPrivateMaxFloorSec : p.iPrivateMaxNoFloorSec;
    return iSec > 0 ? iSec : 0;
}

// ── UE initial configuration 변경 통지 (TS 24.484 §7.2.2.12 → §6.3.13.3, RFC 5875 xcap-diff) ──

/** 등록 Contact 의 +sip.instance 값(RFC 5626 §4.1 — "<urn:uuid:…>") → MCS UE ID. 따옴표·꺾쇠를 뗀다. */
inline std::string CspMcsUeIdOf( const std::string &strInstance ) {
    std::string o;
    for ( char c : strInstance )
        if ( c != '"' && c != '<' && c != '>' ) o += c;
    return o;
}

/** UE initial configuration 문서 선택자 — XCAP root 뒤 경로 «org.3gpp.mcptt.ue-init-config/users/sip:MCSUEID/MCSUEID»
 *  (TS 24.484 §7.2.1.1). 경로 세그먼트에 쓸 수 없는 문자는 %XX 로 싣는다(RFC 3986 §3.3 pchar — 속성 값에도 안전).
 *  빈 ID 면 빈 문자열. */
inline std::string CspUeInitConfigSelector( const std::string &strMcsUeId ) {
    if ( strMcsUeId.empty() ) return std::string();
    auto enc = []( const std::string &v ) {
        static const char kHex[] = "0123456789ABCDEF";
        std::string o;
        for ( unsigned char c : v ) {
            if ( isalnum( c ) || c == '-' || c == '.' || c == '_' || c == '~' || c == ':' || c == '@' || c == '+' ) {
                o += (char)c;
            } else {
                o += '%';
                o += kHex[c >> 4];
                o += kHex[c & 0x0F];
            }
        }
        return o;
    };
    const std::string strId = enc( strMcsUeId );
    return "org.3gpp.mcptt.ue-init-config/users/sip:" + strId + "/" + strId;
}

/** xcap-diff 본문(RFC 5874) — 바뀐 문서들(바뀐 것만 — §4.2). strEtag 가 비면 ETag 속성을 싣지 않는다. 문서가 지워졌으면
 *  (bRemoved) previous-etag 만 싣는다 — 삭제 통지에 new-etag 를 두지 않는다(§3 «MUST NOT be present»). */
inline std::string CspXcapDiffDocsBody( const std::string &strXcapRoot, const std::vector<std::string> &vecSel,
                                        const std::string &strEtag, bool bRemoved = false ) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    s += "<xcap-diff xmlns=\"urn:ietf:params:xml:ns:xcap-diff\" xcap-root=\"" + strXcapRoot + "\">\r\n";
    const std::string strAttr =
        strEtag.empty() ? std::string() : ( bRemoved ? " previous-etag=\"" : " new-etag=\"" ) + strEtag + "\"";
    for ( const auto &strSel : vecSel ) s += "  <document" + strAttr + " sel=\"" + strSel + "\"/>\r\n";
    s += "</xcap-diff>\r\n";
    return s;
}

/** xcap-diff 본문(RFC 5874) — 바뀐 문서 하나. */
inline std::string CspXcapDiffDocBody( const std::string &strXcapRoot, const std::string &strSel,
                                       const std::string &strEtag ) {
    return CspXcapDiffDocsBody( strXcapRoot, std::vector<std::string>{ strSel }, strEtag );
}

// ── CMS 문서 선택자 (xcap-diff sel — XCAP root 뒤 경로) ──
//   MCPTT 두 문서는 본문 없는 구독(CIMS 단말)이 받아 온 사용자 트리 경로 그대로 — 단말은 sel 의 AUID 로 문서를 가른다.
//   MCVideo 두 문서는 규격 이름(TS 24.484 §9.3.2.8 «mcvideo-user-profile-<index>.xml»(index 1) · §9.4.2.8·§9.4.2.9 전역
//   «mcvideo-service-config.xml»).
inline std::string CspMcpttUserProfileSel( const std::string &strUserId ) {
    return "org.3gpp.mcptt.user-profile/users/tel:" + strUserId + "/user-profile";
}
inline std::string CspMcpttServiceConfigSel( const std::string &strUserId ) {
    return "org.3gpp.mcptt.service-config/users/tel:" + strUserId + "/service-config";
}
inline std::string CspMcVideoUserProfileSel( const std::string &strUserId ) {
    return "org.3gpp.mcvideo.user-profile/users/tel:" + strUserId + "/mcvideo-user-profile-1.xml";
}
inline std::string CspMcVideoServiceConfigSel() {
    return "org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml";
}

// ── 규격형 xcap-diff 구독 본문 (TS 24.481 §6.3.13.2.1 a) · TS 24.484 §6.3.13.2 — RFC 5875 §4.1) ──

/** 구독 본문(application/resource-lists+xml — multipart 안이어도)의 <entry uri> 값들 → XCAP root 뒤 경로. 절대 URI 는
 * AUID
 *  (`org.openmobilealliance.` · `org.3gpp.`) 앞을 뗀다 — 상대 경로는 «XCAP root URI 를 base 로» 쓴 것이다(§6.3.13.2.1
 * a)1)A)). &amp; 는 푼다. AUID 를 찾지 못한 entry 는 버린다. */
inline std::vector<std::string> CspXcapDiffEntries( const std::string &strBody ) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ( ( pos = strBody.find( "entry", pos ) ) != std::string::npos ) {
        const bool bTag = pos > 0 && ( strBody[pos - 1] == '<' || strBody[pos - 1] == ':' );
        const size_t end = strBody.find( '>', pos );
        if ( !bTag || end == std::string::npos ) {
            pos += 5;
            continue;
        }
        const std::string strTag = strBody.substr( pos, end - pos );
        pos = end;
        size_t u = strTag.find( "uri=" );
        if ( u == std::string::npos || u + 5 > strTag.size() ) continue;
        const char q = strTag[u + 4];
        if ( q != '"' && q != '\'' ) continue;
        const size_t ue = strTag.find( q, u + 5 );
        if ( ue == std::string::npos ) continue;
        std::string v = strTag.substr( u + 5, ue - u - 5 );
        for ( size_t a; ( a = v.find( "&amp;" ) ) != std::string::npos; ) v.replace( a, 5, "&" );
        size_t k = v.find( "org.openmobilealliance." );
        const size_t k2 = v.find( "org.3gpp." );
        if ( k == std::string::npos || ( k2 != std::string::npos && k2 < k ) ) k = k2;
        if ( k == std::string::npos ) continue;
        out.push_back( v.substr( k ) );
    }
    return out;
}

/** 선택자가 AUID strAuid 의 문서인가 («AUID/…»). */
inline bool CspXcapSelIsAuid( const std::string &strSel, const std::string &strAuid ) {
    return strSel.size() > strAuid.size() && strSel.compare( 0, strAuid.size(), strAuid ) == 0 &&
           strSel[strAuid.size()] == '/';
}

/** 그룹 문서 선택자(org.openmobilealliance.groups/…/<그룹 ID>)가 그룹 strGroupId 의 문서인가 — 마지막 경로 세그먼트를
 *  %3A·%40 를 풀고 tel:/sip: 접두·@도메인을 떼어 비교한다(TS 24.481 §7.2.10.2 — 그룹 ID 로 가리키는 문서). */
inline bool CspXcapSelIsGroupDoc( const std::string &strSel, const std::string &strGroupId ) {
    if ( !CspXcapSelIsAuid( strSel, "org.openmobilealliance.groups" ) ) return false;
    auto bare = []( std::string v ) {
        for ( size_t a; ( a = v.find( "%3A" ) ) != std::string::npos || ( a = v.find( "%3a" ) ) != std::string::npos; )
            v.replace( a, 3, ":" );
        for ( size_t a; ( a = v.find( "%40" ) ) != std::string::npos; ) v.replace( a, 3, "@" );
        if ( v.compare( 0, 4, "tel:" ) == 0 || v.compare( 0, 4, "sip:" ) == 0 ) v = v.substr( 4 );
        const size_t at = v.find( '@' );
        if ( at != std::string::npos ) v = v.substr( 0, at );
        return v;
    };
    const size_t slash = strSel.rfind( '/' );
    return !strGroupId.empty() && bare( strSel.substr( slash + 1 ) ) == bare( strGroupId );
}

#endif
