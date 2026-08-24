#include "SipStatsMonitor.h"

#include <stdio.h>

#include <map>

#include "FmReporter.h"
#include "MonitorString.h"
#include "SipServer.h"
#include "SipServerSetup.h"

CSipStatsMonitor gclsSipStatsMonitor;

CSipStatsMonitor::CSipStatsMonitor()
    : m_tLastEval( 0 ),
      m_iLastWindowSec( 0 ),
      m_dLastCps( 0.0 ),
      m_ulLastParseError( 0 ),
      m_ulLastChannelPolicy( 0 ),
      m_ulLastSecAgreeReject( 0 ) {
}

void CSipStatsMonitor::SourceCounter::Add( const char *pszIp ) {
    std::lock_guard<std::mutex> lock( clsMutex );
    ++ulTotal;
    ++ulWindow;
    if ( pszIp == NULL || pszIp[0] == '\0' ) return;
    std::map<std::string, uint64_t>::iterator it = mapSrc.find( pszIp );
    if ( it != mapSrc.end() ) {
        ++it->second;
    } else if ( (int)mapSrc.size() < VIOLATION_SRC_MAX ) {
        mapSrc[pszIp] = 1;
    }
}

void CSipStatsMonitor::SourceCounter::Reset() {
    std::lock_guard<std::mutex> lock( clsMutex );
    ulWindow = 0;
    mapSrc.clear();
}

uint64_t CSipStatsMonitor::SourceCounter::Drain( std::string &strTopSrc, uint64_t &ulTopCount ) {
    std::lock_guard<std::mutex> lock( clsMutex );
    const uint64_t ulCount = ulWindow;
    ulWindow = 0;
    strTopSrc.clear();
    ulTopCount = 0;
    for ( std::map<std::string, uint64_t>::iterator it = mapSrc.begin(); it != mapSrc.end(); ++it ) {
        if ( it->second > ulTopCount ) {
            ulTopCount = it->second;
            strTopSrc = it->first;
        }
    }
    mapSrc.clear();
    return ulCount;
}

uint64_t CSipStatsMonitor::SourceCounter::Total() {
    std::lock_guard<std::mutex> lock( clsMutex );
    return ulTotal;
}

void CSipStatsMonitor::AddChannelPolicyViolation( const char *pszIp ) {
    m_clsChannelPolicy.Add( pszIp );
}

void CSipStatsMonitor::AddSecAgreeReject( const char *pszIp ) {
    m_clsSecAgreeReject.Add( pszIp );
}

void CSipStatsMonitor::Poll( time_t tNow ) {
    int iEvalSec = gclsSetup.m_iSipStatsEvalSec;
    if ( iEvalSec <= 0 ) {
        if ( m_tLastEval != 0 ) CloseAll();  // 평가 off 전환 — 잔여 알람 해소
        m_tLastEval = 0;
        return;
    }
    if ( m_tLastEval == 0 ) {
        // 기준 스냅샷 확보 — 첫 윈도우는 다음 EvalSec 경과 시부터
        gclsUserAgent.m_clsSipStack.m_clsCounter.GetSnapshot( m_clsPrev );
        std::map<std::string, uint64_t> mapDiscard;
        gclsUserAgent.m_clsSipStack.m_clsCounter.TakeParseErrorSources( mapDiscard );
        m_clsChannelPolicy.Reset();
        m_clsSecAgreeReject.Reset();
        m_tLastEval = tNow;
        return;
    }
    if ( tNow - m_tLastEval < iEvalSec ) return;
    Evaluate( tNow, (int)( tNow - m_tLastEval ) );
    m_tLastEval = tNow;
}

void CSipStatsMonitor::Evaluate( time_t tNow, int iWindowSec ) {
    (void)tNow;
    CSipStackCounter &clsCounter = gclsUserAgent.m_clsSipStack.m_clsCounter;
    CSipStackCounter::Snapshot clsCur;
    clsCounter.GetSnapshot( clsCur );

    CSipStackCounter::Snapshot clsDelta;
    for ( int g = 0; g < CSipStackCounter::E_GRP_MAX; ++g ) {
        for ( int i = 0; i < CSipStackCounter::FINAL_SLOTS; ++i ) {
            clsDelta.arrRecvFinal[g][i] = clsCur.arrRecvFinal[g][i] - m_clsPrev.arrRecvFinal[g][i];
            clsDelta.arrSendFinal[g][i] = clsCur.arrSendFinal[g][i] - m_clsPrev.arrSendFinal[g][i];
        }
    }
    clsDelta.ulRecvInviteInitial = clsCur.ulRecvInviteInitial - m_clsPrev.ulRecvInviteInitial;
    clsDelta.ulParseError = clsCur.ulParseError - m_clsPrev.ulParseError;
    m_clsPrev = clsCur;

    std::map<std::string, uint64_t> mapSrc;
    clsCounter.TakeParseErrorSources( mapSrc );
    std::string strTopSrc;
    uint64_t ulTopSrcCount = 0;
    for ( std::map<std::string, uint64_t>::iterator it = mapSrc.begin(); it != mapSrc.end(); ++it ) {
        if ( it->second > ulTopSrcCount ) {
            ulTopSrcCount = it->second;
            strTopSrc = it->first;
        }
    }

    // 보안 위반 윈도우 계수 드레인 — 채널 정책 403 (A-SEC-003) / sec-agree 거절 494/421 (A-SEC-004)
    std::string strViolationTopSrc, strSecAgreeTopSrc;
    uint64_t ulViolationTopCount = 0, ulSecAgreeTopCount = 0;
    const uint64_t ulViolation = m_clsChannelPolicy.Drain( strViolationTopSrc, ulViolationTopCount );
    const uint64_t ulSecAgreeReject = m_clsSecAgreeReject.Drain( strSecAgreeTopSrc, ulSecAgreeTopCount );

    // 호 — 챌린지(401/407) 제외, 유효 = 2xx/3xx + 사용자 행위 결과(486/487/603)
    static const int arrCallExclude[] = { 401, 407 };
    static const int arrCallEffective[] = { 486, 487, 603 };
    RateResult clsCall =
        CalcRate( clsDelta, CSipStackCounter::E_GRP_INVITE, arrCallExclude, 2, arrCallEffective, 3, 400 );
    // 등록 — 챌린지/협상(401/407/423) 제외, 유효 = 2xx
    static const int arrRegExclude[] = { 401, 407, 423 };
    RateResult clsReg = CalcRate( clsDelta, CSipStackCounter::E_GRP_REGISTER, arrRegExclude, 3, NULL, 0, 300 );

    double dCps = iWindowSec > 0 ? (double)clsDelta.ulRecvInviteInitial / iWindowSec : 0.0;

    if ( gclsFmReporter.IsEnabled() ) {
        FireRateAlarm( "A-QOS-006", "calls/success_rate", clsCall, gclsSetup.m_iSipStatsMinFinals,
                       gclsSetup.m_iSipStatsCallRateMinor, gclsSetup.m_iSipStatsCallRateMajor,
                       gclsSetup.m_iSipStatsCallRateCritical, iWindowSec );
        FireRateAlarm( "A-QOS-007", "reg/success_rate", clsReg, gclsSetup.m_iSipStatsMinFinals,
                       gclsSetup.m_iSipStatsRegRateMinor, gclsSetup.m_iSipStatsRegRateMajor,
                       gclsSetup.m_iSipStatsRegRateCritical, iWindowSec );

        // CPS 상한 (0 = 해당 단계 미사용)
        const std::string strCpsMo = gclsFmReporter.Node() + "/csp/cps";
        const char *pszCpsSev = NULL;
        if ( gclsSetup.m_iSipStatsCpsCritical > 0 && dCps >= (double)gclsSetup.m_iSipStatsCpsCritical ) {
            pszCpsSev = "critical";
        } else if ( gclsSetup.m_iSipStatsCpsMajor > 0 && dCps >= (double)gclsSetup.m_iSipStatsCpsMajor ) {
            pszCpsSev = "major";
        } else if ( gclsSetup.m_iSipStatsCpsMinor > 0 && dCps >= (double)gclsSetup.m_iSipStatsCpsMinor ) {
            pszCpsSev = "minor";
        }
        if ( pszCpsSev ) {
            char szObserved[32];
            snprintf( szObserved, sizeof( szObserved ), "%.1f", dCps );
            SimpleJson::JsonNode nodeParams;
            nodeParams.Set( "observed", szObserved );
            nodeParams.Set( "crit", gclsSetup.m_iSipStatsCpsCritical );
            nodeParams.Set( "maj", gclsSetup.m_iSipStatsCpsMajor );
            nodeParams.Set( "min", gclsSetup.m_iSipStatsCpsMinor );
            nodeParams.Set( "window", iWindowSec );
            gclsFmReporter.AlarmOpen( "A-QOS-009", strCpsMo, nodeParams, pszCpsSev );
        } else {
            gclsFmReporter.AlarmClose( "A-QOS-009", strCpsMo );
        }

        // SIP 수신 이상 급증 (파싱 실패 — 단일 단계 minor)
        const std::string strRxMo = gclsFmReporter.Node() + "/csp/sip/rx_error";
        int iRxThreshold = gclsSetup.m_iSipStatsRxErrorMinor;
        if ( iRxThreshold > 0 && clsDelta.ulParseError >= (uint64_t)iRxThreshold ) {
            SimpleJson::JsonNode nodeParams;
            nodeParams.Set( "count", (int)clsDelta.ulParseError );
            nodeParams.Set( "window", iWindowSec );
            nodeParams.Set( "ip", strTopSrc.empty() ? "-" : strTopSrc.c_str() );
            if ( ulTopSrcCount > 0 ) nodeParams.Set( "top_count", (int)ulTopSrcCount );
            gclsFmReporter.AlarmOpen( "A-QOS-011", strRxMo, nodeParams, "minor" );
        } else {
            gclsFmReporter.AlarmClose( "A-QOS-011", strRxMo );
        }

        // 채널 정책 위반 반복 (게이트 403 급증 — 단일 단계 major, sip_access_security.md §3.3)
        const std::string strPolicyMo = gclsFmReporter.Node() + "/csp/channel_policy";
        int iPolicyThreshold = gclsSetup.m_iSipStatsChannelPolicyMajor;
        if ( iPolicyThreshold > 0 && ulViolation >= (uint64_t)iPolicyThreshold ) {
            SimpleJson::JsonNode nodeParams;
            nodeParams.Set( "count", (int)ulViolation );
            nodeParams.Set( "window", iWindowSec );
            nodeParams.Set( "ip", strViolationTopSrc.empty() ? "-" : strViolationTopSrc.c_str() );
            if ( ulViolationTopCount > 0 ) nodeParams.Set( "top_count", (int)ulViolationTopCount );
            gclsFmReporter.AlarmOpen( "A-SEC-003", strPolicyMo, nodeParams, "major" );
        } else {
            gclsFmReporter.AlarmClose( "A-SEC-003", strPolicyMo );
        }

        // sec-agree 협상 거절 반복 (494/421 급증 — 단일 단계 major, sip_access_security.md §8.1)
        const std::string strSecAgreeMo = gclsFmReporter.Node() + "/csp/sec_agree";
        int iSecAgreeThreshold = gclsSetup.m_iSipStatsSecAgreeRejectMajor;
        if ( iSecAgreeThreshold > 0 && ulSecAgreeReject >= (uint64_t)iSecAgreeThreshold ) {
            SimpleJson::JsonNode nodeParams;
            nodeParams.Set( "count", (int)ulSecAgreeReject );
            nodeParams.Set( "window", iWindowSec );
            nodeParams.Set( "ip", strSecAgreeTopSrc.empty() ? "-" : strSecAgreeTopSrc.c_str() );
            if ( ulSecAgreeTopCount > 0 ) nodeParams.Set( "top_count", (int)ulSecAgreeTopCount );
            gclsFmReporter.AlarmOpen( "A-SEC-004", strSecAgreeMo, nodeParams, "major" );
        } else {
            gclsFmReporter.AlarmClose( "A-SEC-004", strSecAgreeMo );
        }
    }

    std::lock_guard<std::mutex> lock( m_clsMutex );
    m_iLastWindowSec = iWindowSec;
    m_clsLastCall = clsCall;
    m_clsLastReg = clsReg;
    m_dLastCps = dCps;
    m_ulLastParseError = clsDelta.ulParseError;
    m_strLastTopSrc = strTopSrc;
    m_ulLastChannelPolicy = ulViolation;
    m_strLastViolationTopSrc = strViolationTopSrc;
    m_ulLastSecAgreeReject = ulSecAgreeReject;
    m_strLastSecAgreeTopSrc = strSecAgreeTopSrc;
}

void CSipStatsMonitor::CloseAll() {
    if ( !gclsFmReporter.IsEnabled() ) return;
    const std::string strBase = gclsFmReporter.Node() + "/csp/";
    gclsFmReporter.AlarmClose( "A-QOS-006", strBase + "calls/success_rate" );
    gclsFmReporter.AlarmClose( "A-QOS-007", strBase + "reg/success_rate" );
    gclsFmReporter.AlarmClose( "A-QOS-009", strBase + "cps" );
    gclsFmReporter.AlarmClose( "A-QOS-011", strBase + "sip/rx_error" );
    gclsFmReporter.AlarmClose( "A-SEC-003", strBase + "channel_policy" );
    gclsFmReporter.AlarmClose( "A-SEC-004", strBase + "sec_agree" );
}

CSipStatsMonitor::RateResult CSipStatsMonitor::CalcRate( const CSipStackCounter::Snapshot &clsDelta,
                                                         CSipStackCounter::EGroup eGroup, const int *piExclude,
                                                         int iExcludeCount, const int *piEffective, int iEffectiveCount,
                                                         int iEffectiveBelow ) {
    RateResult clsRes;
    for ( int i = 0; i < CSipStackCounter::FINAL_SLOTS; ++i ) {
        uint64_t ulCount = clsDelta.arrRecvFinal[eGroup][i] + clsDelta.arrSendFinal[eGroup][i];
        if ( ulCount == 0 ) continue;
        int iCode = CSipStackCounter::FINAL_MIN + i;
        bool bSkip = false;
        for ( int e = 0; e < iExcludeCount && !bSkip; ++e ) bSkip = ( piExclude[e] == iCode );
        if ( bSkip ) continue;
        clsRes.ulFinals += ulCount;
        bool bEffective = ( iCode < iEffectiveBelow );
        for ( int e = 0; e < iEffectiveCount && !bEffective; ++e ) bEffective = ( piEffective[e] == iCode );
        if ( bEffective ) {
            clsRes.ulEffective += ulCount;
        } else if ( ulCount > clsRes.ulTopFailCount ) {
            clsRes.ulTopFailCount = ulCount;
            clsRes.iTopFailCode = iCode;
        }
    }
    if ( clsRes.ulFinals > 0 ) clsRes.dRate = 100.0 * (double)clsRes.ulEffective / (double)clsRes.ulFinals;
    return clsRes;
}

void CSipStatsMonitor::FireRateAlarm( const char *pszCode, const char *pszComponent, const RateResult &clsRate,
                                      int iMinFinals, int iMinor, int iMajor, int iCritical, int iWindowSec ) {
    const std::string strMo = gclsFmReporter.Node() + "/csp/" + pszComponent;
    const char *pszSev = NULL;
    if ( clsRate.ulFinals >= (uint64_t)( iMinFinals > 0 ? iMinFinals : 1 ) ) {
        if ( iCritical > 0 && clsRate.dRate < (double)iCritical ) {
            pszSev = "critical";
        } else if ( iMajor > 0 && clsRate.dRate < (double)iMajor ) {
            pszSev = "major";
        } else if ( iMinor > 0 && clsRate.dRate < (double)iMinor ) {
            pszSev = "minor";
        }
    }
    if ( pszSev == NULL ) {
        gclsFmReporter.AlarmClose( pszCode, strMo );
        return;
    }
    char szObserved[32];
    snprintf( szObserved, sizeof( szObserved ), "%.1f", clsRate.dRate );
    SimpleJson::JsonNode nodeParams;
    nodeParams.Set( "observed", szObserved );
    nodeParams.Set( "crit", iCritical );
    nodeParams.Set( "maj", iMajor );
    nodeParams.Set( "min", iMinor );
    nodeParams.Set( "window", iWindowSec );
    nodeParams.Set( "finals", (int)clsRate.ulFinals );
    nodeParams.Set( "failed", (int)( clsRate.ulFinals - clsRate.ulEffective ) );
    if ( clsRate.iTopFailCode > 0 ) nodeParams.Set( "top_code", clsRate.iTopFailCode );
    gclsFmReporter.AlarmOpen( pszCode, strMo, nodeParams, pszSev );
}

void CSipStatsMonitor::GetString( CMonitorString &strBuf ) {
    CSipStackCounter::Snapshot clsCur;
    gclsUserAgent.m_clsSipStack.m_clsCounter.GetSnapshot( clsCur );

    strBuf.Clear();
    strBuf.AddCol( "recv_invite_initial" );
    strBuf.AddRow( (uint32_t)clsCur.ulRecvInviteInitial );
    strBuf.AddCol( "recv_invite_re" );
    strBuf.AddRow( (uint32_t)clsCur.ulRecvInviteRe );
    strBuf.AddCol( "recv_register" );
    strBuf.AddRow( (uint32_t)clsCur.ulRecvRegister );
    strBuf.AddCol( "recv_other_request" );
    strBuf.AddRow( (uint32_t)clsCur.ulRecvOtherRequest );
    strBuf.AddCol( "parse_error" );
    strBuf.AddRow( (uint32_t)clsCur.ulParseError );
    strBuf.AddCol( "security_drop" );
    strBuf.AddRow( (uint32_t)clsCur.ulSecurityDrop );
    strBuf.AddCol( "channel_policy_violation" );
    strBuf.AddRow( (uint32_t)m_clsChannelPolicy.Total() );
    strBuf.AddCol( "sec_agree_reject" );
    strBuf.AddRow( (uint32_t)m_clsSecAgreeReject.Total() );

    // 그룹×코드 누적 (0 은 생략) — "final <rx|tx> <INVITE|REGISTER|OTHER> <code>"
    static const char *arrGroupName[CSipStackCounter::E_GRP_MAX] = { "INVITE", "REGISTER", "OTHER" };
    for ( int g = 0; g < CSipStackCounter::E_GRP_MAX; ++g ) {
        for ( int i = 0; i < CSipStackCounter::FINAL_SLOTS; ++i ) {
            char szName[48];
            if ( clsCur.arrRecvFinal[g][i] > 0 ) {
                snprintf( szName, sizeof( szName ), "final rx %s %d", arrGroupName[g],
                          CSipStackCounter::FINAL_MIN + i );
                strBuf.AddCol( szName );
                strBuf.AddRow( (uint32_t)clsCur.arrRecvFinal[g][i] );
            }
            if ( clsCur.arrSendFinal[g][i] > 0 ) {
                snprintf( szName, sizeof( szName ), "final tx %s %d", arrGroupName[g],
                          CSipStackCounter::FINAL_MIN + i );
                strBuf.AddCol( szName );
                strBuf.AddRow( (uint32_t)clsCur.arrSendFinal[g][i] );
            }
        }
    }

    std::lock_guard<std::mutex> lock( m_clsMutex );
    if ( m_iLastWindowSec > 0 ) {
        char szVal[128];
        snprintf( szVal, sizeof( szVal ), "%.1f%% (finals=%d, top_fail=%d)", m_clsLastCall.dRate,
                  (int)m_clsLastCall.ulFinals, m_clsLastCall.iTopFailCode );
        strBuf.AddCol( "window call_rate" );
        strBuf.AddRow( szVal );
        snprintf( szVal, sizeof( szVal ), "%.1f%% (finals=%d, top_fail=%d)", m_clsLastReg.dRate,
                  (int)m_clsLastReg.ulFinals, m_clsLastReg.iTopFailCode );
        strBuf.AddCol( "window reg_rate" );
        strBuf.AddRow( szVal );
        snprintf( szVal, sizeof( szVal ), "%.1f (window=%ds)", m_dLastCps, m_iLastWindowSec );
        strBuf.AddCol( "window cps" );
        strBuf.AddRow( szVal );
        snprintf( szVal, sizeof( szVal ), "%u (top_src=%s)", (unsigned)m_ulLastParseError,
                  m_strLastTopSrc.empty() ? "-" : m_strLastTopSrc.c_str() );
        strBuf.AddCol( "window parse_error" );
        strBuf.AddRow( szVal );
        snprintf( szVal, sizeof( szVal ), "%u (top_src=%s)", (unsigned)m_ulLastChannelPolicy,
                  m_strLastViolationTopSrc.empty() ? "-" : m_strLastViolationTopSrc.c_str() );
        strBuf.AddCol( "window channel_policy" );
        strBuf.AddRow( szVal );
        snprintf( szVal, sizeof( szVal ), "%u (top_src=%s)", (unsigned)m_ulLastSecAgreeReject,
                  m_strLastSecAgreeTopSrc.empty() ? "-" : m_strLastSecAgreeTopSrc.c_str() );
        strBuf.AddCol( "window sec_agree_reject" );
        strBuf.AddRow( szVal );
    }
}
