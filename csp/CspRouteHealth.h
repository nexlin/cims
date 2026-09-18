#ifndef __CSP_ROUTE_HEALTH_H__
#define __CSP_ROUTE_HEALTH_H__

#include <map>
#include <mutex>
#include <string>

class CSipMessage;

/**
 * @brief RouteSet 헬스체크 — OPTIONS 프로브(RFC 3261 §11) 로 Route 의 alive/dead 를 판정한다
 *        (sip_service_model.md §3 RouteSet `health_check_*`).
 *
 *   - 주기: `health_check_mode=options_ping` 인 enabled RouteSet 의 멤버 Route 마다
 *     `health_check_interval_sec` 간격으로 OPTIONS 를 그 Route 의 RemoteNode 로 보낸다
 *     (Via/From 은 Route 의 local_node — 발신 leg 와 같은 자기 주소).
 *   - 판정: 응답(2xx~4xx·6xx) = 살아 있음, 5xx·전송 타임아웃(SendTimeout)·주기 안 무응답 = 실패.
 *     연속 실패 `dead_threshold` 회 → dead, dead 상태에서 연속 성공 `recovery_probes` 회 → alive
 *     (상태는 CCspRouteMap::RouteRuntime — SelectRoute 가 alive 만 고른다).
 *   - 알람: dead 전이에 A-COM-003 connection_lost(mo = <node>/csp/peer/<remote_node>) open, alive 전이에 close.
 *   - 같은 Route 가 여러 RouteSet 에 속하면 임계값은 프로브를 낸 RouteSet 의 것을 쓴다.
 *   - `invite_response` 모드는 미구현(§9) — 그 RouteSet 은 프로브를 내지 않는다.
 */
class CCspRouteHealth {
public:
    /** 1초마다 호출 — 주기가 찬 Route 에 프로브 송신, 기한 지난 미응답 프로브 실패 처리. */
    void Tick( long now );
    /** ISipStackCallBack::RecvResponse 에서 — OPTIONS 프로브 응답이면 소비하고 true. */
    bool OnResponse( CSipMessage *pclsMessage );
    /** ISipStackCallBack::SendTimeout 에서 — OPTIONS 프로브였으면 실패 처리하고 true. */
    bool OnSendTimeout( CSipMessage *pclsMessage );

private:
    struct Probe {
        std::string route;
        std::string remote_node;
        long sent_at_usec = 0;
        int dead_threshold = 3;
        int recovery_probes = 1;
        int interval_sec = 30;
    };

    bool _sendProbe( const std::string &routeName, const std::string &routeSetName, int iInterval, int iDeadThreshold,
                     int iRecoveryProbes, long now );
    void _onResult( const Probe &clsProbe, bool bOk, int iRttMs, const char *pszWhy );
    bool _takeProbe( CSipMessage *pclsMessage, Probe &out );

    std::mutex m_mutex;
    std::map<std::string, Probe> m_mapPending;  // Call-ID → 프로브
    unsigned m_uSeq = 0;
};

extern CCspRouteHealth gclsRouteHealth;

#endif  // __CSP_ROUTE_HEALTH_H__
