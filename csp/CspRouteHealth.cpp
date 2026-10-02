#include "CspRouteHealth.h"

#include <sys/time.h>

#include <set>
#include <vector>

#include "CspAddressing.h"
#include "CspLocalNodeMap.h"
#include "CspRemoteNodeMap.h"
#include "CspRouteMap.h"
#include "CspRouteSetMap.h"
#include "CspTrunkRegistrar.h"
#include "FmReporter.h"
#include "Log.h"
#include "SipMessage.h"
#include "SipServer.h"
#include "SipServerSetup.h"

CCspRouteHealth gclsRouteHealth;

namespace {
    long _nowUsec() {
        struct timeval tv;
        gettimeofday( &tv, NULL );
        return (long)tv.tv_sec * 1000000L + tv.tv_usec;
    }
    ESipTransport _transportOf( const std::string &proto ) {
        if ( proto == "TCP" ) return E_SIP_TCP;
        if ( proto == "TLS" ) return E_SIP_TLS;
        return E_SIP_UDP;
    }
}  // namespace

void CCspRouteHealth::Tick( long now ) {
    // 1) 주기가 찬 Route 에 프로브
    for ( const RouteSetConfig &rs : gclsRouteSetMap.GetAll() ) {
        if ( !rs.enabled || rs.health_check_mode != "options_ping" ) continue;
        const int iInterval = rs.health_check_interval_sec > 0 ? rs.health_check_interval_sec : 30;
        for ( const RouteSetMember &m : rs.members ) {
            RouteRuntime rt;
            if ( !gclsRouteMap.GetRuntime( m.route_ref, rt ) ) continue;
            if ( rt.last_ping_at.load() != 0 && now - rt.last_ping_at.load() < iInterval ) continue;
            _sendProbe( m.route_ref, rs.name, iInterval, rs.health_check_dead_threshold,
                        rs.health_check_recovery_probes, now );
        }
    }
    // 2) 주기 안에 응답도 타임아웃도 없는 프로브 — 실패로 닫는다 (psip Timer F 32 s 보다 짧은 주기 대비)
    std::vector<Probe> vecExpired;
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        for ( auto it = m_mapPending.begin(); it != m_mapPending.end(); ) {
            if ( now * 1000000L - it->second.sent_at_usec >= (long)it->second.interval_sec * 1000000L ) {
                vecExpired.push_back( it->second );
                it = m_mapPending.erase( it );
            } else {
                ++it;
            }
        }
    }
    for ( const Probe &p : vecExpired ) _onResult( p, false, -1, "no reply within interval" );
    // 3) 감시 대상에서 빠진 peer 의 열린 A-COM-003 을 닫는다
    _closeUnmonitoredAlarms();
}

void CCspRouteHealth::_closeUnmonitoredAlarms() {
    if ( !gclsFmReporter.IsEnabled() ) return;
    const std::string strPrefix = gclsFmReporter.Node() + "/csp/peer/";
    const std::vector<std::string> vecOpen = gclsFmReporter.ActiveMos( "A-COM-003", strPrefix );
    if ( vecOpen.empty() ) return;
    // 감시 대상 peer = OPTIONS 프로브를 받는 Remote Node (enabled RouteSet · options_ping · enabled Route · enabled RN)
    //                + 등록형 트렁크 Route 의 Remote Node (바인딩 유무로 CCspTrunkRegistrar 가 여닫는다)
    std::set<std::string> setMonitored;
    for ( const RouteSetConfig &rs : gclsRouteSetMap.GetAll() ) {
        if ( !rs.enabled || rs.health_check_mode != "options_ping" ) continue;
        for ( const RouteSetMember &m : rs.members ) {
            RouteConfig rc = gclsRouteMap.GetByName( m.route_ref );
            if ( !rc.IsValid() || !rc.enabled ) continue;
            if ( !rc.IsTrunkAccount() ) {
                RemoteNodeInfo rn = gclsRemoteNodeMap.GetByName( rc.remote_node_ref );
                if ( !rn.IsValid() || !rn.enabled ) continue;
            }
            setMonitored.insert( rc.remote_node_ref );
        }
    }
    for ( const RouteConfig &rc : gclsRouteMap.GetAll() )
        if ( rc.enabled && rc.IsTrunkAccount() ) setMonitored.insert( rc.remote_node_ref );
    for ( const std::string &strMo : vecOpen ) {
        const std::string strPeer = strMo.substr( strPrefix.size() );
        if ( setMonitored.count( strPeer ) ) continue;
        CLog::Print( LOG_SYSTEM, "RouteHealth: peer='%s' no longer monitored (config change) → A-COM-003 close",
                     strPeer.c_str() );
        gclsFmReporter.AlarmClose( "A-COM-003", strMo );
    }
}

bool CCspRouteHealth::_sendProbe( const std::string &routeName, const std::string &routeSetName, int iInterval,
                                  int iDeadThreshold, int iRecoveryProbes, long now ) {
    RouteConfig rc = gclsRouteMap.GetByName( routeName );
    if ( !rc.IsValid() || !rc.enabled ) return false;
    RemoteNodeInfo rn = gclsRemoteNodeMap.GetByName( rc.remote_node_ref );
    if ( rc.IsTrunkAccount() ) {
        // 등록형 트렁크 — 바인딩이 없으면 프로브할 곳이 없다(CCspTrunkRegistrar 가 dead 로 둔다). 있으면 바인딩 주소로
        TrunkBinding tb;
        if ( !gclsTrunkRegistrar.Get( rc.name, tb ) ) return false;
        rn.name = rc.remote_node_ref;
        rn.enabled = true;
        rn.ip = tb.ip;
        rn.port = tb.port;
        rn.protocol = tb.transport;
    }
    if ( !rn.IsValid() || !rn.enabled || rn.ip.empty() || rn.port <= 0 ) return false;

    // 자기 주소 = Route 의 local_node (발신 leg 와 같은 규칙 — EventIncomingCall 의 T3). 없으면 primary.
    std::string strLocalIp = gclsSetup.m_strLocalIp;
    int iLocalPort = 0;
    const ESipTransport eTransport = _transportOf( rn.protocol );
    if ( !rc.local_node_ref.empty() ) {
        LocalNodeInfo ln = gclsLocalNodeMap.GetByName( rc.local_node_ref );
        if ( ln.IsValid() ) {
            if ( !ln.bind_ip.empty() && ln.bind_ip != "0.0.0.0" ) strLocalIp = ln.bind_ip;
            iLocalPort = ln.bind_port;
        }
    }
    if ( iLocalPort <= 0 ) iLocalPort = CspAddressing::GetLocalSipPortForTransport( 0, eTransport );

    gclsRouteMap.TouchPing( routeName, now );

    CSipMessage *pclsMessage = new CSipMessage();
    if ( pclsMessage == NULL ) return false;
    pclsMessage->m_strSipMethod = SIP_METHOD_OPTIONS;
    pclsMessage->m_eTransport = eTransport;
    pclsMessage->m_clsReqUri.Set( SIP_PROTOCOL, "ping", rn.ip.c_str(), rn.port );
    if ( eTransport != E_SIP_UDP )
        pclsMessage->m_clsReqUri.InsertParam( "transport", eTransport == E_SIP_TLS ? "tls" : "tcp" );
    pclsMessage->AddVia( strLocalIp.c_str(), iLocalPort );
    pclsMessage->m_clsFrom.m_clsUri.Set( SIP_PROTOCOL, "ping", strLocalIp.c_str(), iLocalPort );
    pclsMessage->m_clsFrom.InsertTag();
    pclsMessage->m_clsTo.m_clsUri.Set( SIP_PROTOCOL, "ping", rn.ip.c_str(), rn.port );
    pclsMessage->m_clsCallId.Make( strLocalIp.c_str() );
    if ( ++m_uSeq > 10000000 ) m_uSeq = 1;
    pclsMessage->m_clsCSeq.Set( (int)m_uSeq, SIP_METHOD_OPTIONS );
    pclsMessage->m_iMaxForwards = 70;
    pclsMessage->m_strSendDestIp = rn.ip;
    pclsMessage->m_iSendDestPort = rn.port;

    std::string strCallId;
    pclsMessage->GetCallId( strCallId );
    {
        Probe p;
        p.route = routeName;
        p.remote_node = rn.name;
        p.sent_at_usec = _nowUsec();
        p.dead_threshold = iDeadThreshold;
        p.recovery_probes = iRecoveryProbes;
        p.interval_sec = iInterval;
        std::lock_guard<std::mutex> lk( m_mutex );
        m_mapPending[strCallId] = p;
    }
    CLog::Print( LOG_DEBUG, "RouteHealth: OPTIONS → route='%s' set='%s' %s:%d/%s src=%s:%d callId=%s",
                 routeName.c_str(), routeSetName.c_str(), rn.ip.c_str(), rn.port, rn.protocol.c_str(),
                 strLocalIp.c_str(), iLocalPort, strCallId.c_str() );
    if ( !gclsUserAgent.m_clsSipStack.SendSipMessage( pclsMessage ) ) {
        Probe p;
        {
            std::lock_guard<std::mutex> lk( m_mutex );
            auto it = m_mapPending.find( strCallId );
            if ( it == m_mapPending.end() ) return false;
            p = it->second;
            m_mapPending.erase( it );
        }
        _onResult( p, false, -1, "send failed" );
        return false;
    }
    return true;
}

bool CCspRouteHealth::_takeProbe( CSipMessage *pclsMessage, Probe &out ) {
    if ( pclsMessage == NULL || !pclsMessage->IsMethod( SIP_METHOD_OPTIONS ) ) return false;
    std::string strCallId;
    if ( !pclsMessage->GetCallId( strCallId ) ) return false;
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_mapPending.find( strCallId );
    if ( it == m_mapPending.end() ) return false;
    out = it->second;
    m_mapPending.erase( it );
    return true;
}

bool CCspRouteHealth::OnResponse( CSipMessage *pclsMessage ) {
    Probe p;
    if ( !_takeProbe( pclsMessage, p ) ) return false;
    const int iStatus = pclsMessage->m_iStatusCode;
    const int iRtt = (int)( ( _nowUsec() - p.sent_at_usec ) / 1000 );
    // 5xx 는 서버가 있어도 서비스 불가(503 = 과부하/점검, RFC 3261 §21.5.4) — 실패로 센다. 그 밖의 최종 응답은 도달
    // 증거.
    const bool bOk = iStatus < 500 || iStatus >= 600;
    char szWhy[64];
    snprintf( szWhy, sizeof( szWhy ), "%d", iStatus );
    _onResult( p, bOk, iRtt, szWhy );
    return true;
}

bool CCspRouteHealth::OnSendTimeout( CSipMessage *pclsMessage ) {
    Probe p;
    if ( !_takeProbe( pclsMessage, p ) ) return false;
    _onResult( p, false, -1, "send timeout" );
    return true;
}

// A-COM-003 의 원인 구절(한국어) — 알람 문구 `연동 상대 {peer} 응답 없음 — {reason} (경로 {route})` 에 들어간다.
//   pszWhy 는 마지막 실패의 원인(무응답·전송 실패/시간 초과·5xx 상태 코드). 원문은 reason_raw 로 함께 보낸다.
static std::string _alarmReason( const char *pszWhy, int iFails ) {
    const std::string strWhy = pszWhy ? pszWhy : "";
    char szBuf[128];
    if ( strWhy == "no reply within interval" ) {
        snprintf( szBuf, sizeof( szBuf ), "OPTIONS %d회 연속 무응답", iFails );
    } else {
        const char *pszLast = strWhy == "send timeout"  ? "전송 시간 초과"
                              : strWhy == "send failed" ? "전송 실패"
                                                        : nullptr;
        if ( pszLast )
            snprintf( szBuf, sizeof( szBuf ), "OPTIONS %d회 연속 실패, 마지막 %s", iFails, pszLast );
        else
            snprintf( szBuf, sizeof( szBuf ), "OPTIONS %d회 연속 실패, 마지막 %s 응답", iFails, strWhy.c_str() );
    }
    return szBuf;
}

void CCspRouteHealth::_onResult( const Probe &p, bool bOk, int iRttMs, const char *pszWhy ) {
    bool bChanged = false;
    if ( bOk ) {
        gclsRouteMap.MarkAlive( p.route, iRttMs, p.recovery_probes, bChanged );
        if ( bChanged && gclsFmReporter.IsEnabled() )
            gclsFmReporter.AlarmClose( "A-COM-003", gclsFmReporter.Node() + "/csp/peer/" + p.remote_node );
    } else {
        gclsRouteMap.MarkFail( p.route, p.dead_threshold, bChanged );
        RouteRuntime rt;
        gclsRouteMap.GetRuntime( p.route, rt );
        CLog::Print( bChanged ? LOG_SYSTEM : LOG_INFO, "RouteHealth: probe failed route='%s' peer='%s' (%s) fails=%d%s",
                     p.route.c_str(), p.remote_node.c_str(), pszWhy, rt.consecutive_failures.load(),
                     bChanged ? " → DEAD" : "" );
        if ( bChanged && gclsFmReporter.IsEnabled() ) {
            SimpleJson::JsonNode nodeParams;
            nodeParams.Set( "peer", p.remote_node.c_str() );
            nodeParams.Set( "route", p.route.c_str() );
            nodeParams.Set( "fails", rt.consecutive_failures.load() );
            nodeParams.Set( "reason", _alarmReason( pszWhy, rt.consecutive_failures.load() ) );
            nodeParams.Set( "reason_raw", pszWhy );
            gclsFmReporter.AlarmOpen( "A-COM-003", gclsFmReporter.Node() + "/csp/peer/" + p.remote_node, nodeParams );
        }
    }
}
