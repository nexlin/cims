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

private:
    std::mutex m_clsMutex;
    CspFloorParams m_clsFloor;
    CspPriorityParams m_clsPriority;
    int m_iTng2Sec = -1;
    CspCallTimerParams m_clsCallTimers;
    int m_iMaxCallsN6 = 5;           ///< 그 밖 단말 N6 (mcptt_service_config.max_calls_n6)
    int m_iMaxCallsN6Dispatch = 10;  ///< 관제 N6 (max_calls_n6_dispatch)
    int m_iNumLevelsPriority = -1;   ///< <num-levels-priority-hierarchy> (-1 = 문서에 없음 → 4)
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

/** xcap-diff 본문(RFC 5874) — 바뀐 문서 하나. strEtag 가 비면 new-etag 를 싣지 않는다. */
inline std::string CspXcapDiffDocBody( const std::string &strXcapRoot, const std::string &strSel,
                                       const std::string &strEtag ) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    s += "<xcap-diff xmlns=\"urn:ietf:params:xml:ns:xcap-diff\" xcap-root=\"" + strXcapRoot + "\">\r\n";
    s += "  <document" + ( strEtag.empty() ? std::string() : " new-etag=\"" + strEtag + "\"" ) + " sel=\"" + strSel +
         "\"/>\r\n";
    s += "</xcap-diff>\r\n";
    return s;
}

#endif
