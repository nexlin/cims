#ifndef __CSP_ROUTE_MAP_H__
#define __CSP_ROUTE_MAP_H__

#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * CspRouteMap — routes.jsonl 캐시 (v3, 2026-04-22).
 *
 *   (local_node_ref, remote_node_ref) pair 는 unique.
 *   auth_user/pass/realm 은 Route 가 SOT (RemoteNode 에는 없음).
 *
 *   Route 는 양방향 연결 정보다 — 발신은 RoutingPolicy → RouteSet → Route 로 고르고, **인바운드는
 *   (수신 LocalNode, 소스 주소) 로 Route 를 식별한다**(FindInbound). 피어 신뢰는 그 식별 결과에서 나온다
 *   (inbound_auth) — 접속점 edge 가 아니라 설정된 피어(RemoteNode)가 신뢰의 근거다
 *   (TS 24.229 §5.10 IBCF · TS 29.165 II-NNI, sip_service_model.md §4).
 *
 *   런타임 헬스 상태 (alive/dead, RTT, 연속 실패수) 는 RouteRuntime 에 atomic 으로 보관.
 *   헬스체크 수집은 별도 워커 (RouteSet 레벨 정책에 따라 후속 스테이지에서 통합).
 */

#include <atomic>

struct RouteConfig {
    std::string id;
    std::string name;
    std::string local_node_ref;
    std::string remote_node_ref;
    std::string outbound_proxy_ip;
    int outbound_proxy_port = 0;
    bool register_to_remote = false;
    int register_expires = 3600;
    std::string auth_user;
    std::string auth_password;
    std::string auth_ha1;  // H(A1)=MD5(auth_user:auth_realm:password) — 평문 대신(sip_access_security.md §4.5). 등록형
                           // 트렁크 계정 검증의 우선 재료
    std::string auth_realm;
    int max_concurrent_calls = 0;
    int cps_limit = 0;
    /** 이 Route 로 들어온 요청의 인증 — "none"(기본, 신뢰 피어: Digest 없음) | "digest"(가입자 인증 흐름 — 등록형
     * 트렁크). */
    std::string inbound_auth = "none";
    bool enabled = true;
    std::vector<std::string> tags;
    std::string note;

    bool IsValid() const {
        return !name.empty() && !local_node_ref.empty() && !remote_node_ref.empty();
    }
    bool TrustsInbound() const {
        return inbound_auth != "digest";
    }
    /** 등록형 트렁크 계정(SIPconnect 2.0 §8 등록 모드) — inbound_auth=digest 이고 auth_user 가 있으면 그 계정의
     * REGISTER 를 CCspTrunkRegistrar 가 받고(가입자 아님), 도달 주소는 바인딩(등록 소스)이다. 바인딩이 없으면 Route 는
     * dead. */
    bool IsTrunkAccount() const {
        return inbound_auth == "digest" && !auth_user.empty();
    }
};

struct RouteRuntime {
    std::atomic<bool> alive{ true };
    std::atomic<int> consecutive_failures{ 0 };
    std::atomic<int> consecutive_successes{ 0 };  // dead 상태에서의 회복 프로브 계수 (health_check_recovery_probes)
    std::atomic<int> last_rtt_ms{ -1 };
    std::atomic<long> last_ping_at{ 0 };
    std::atomic<long> last_reply_at{ 0 };

    RouteRuntime() = default;
    RouteRuntime( const RouteRuntime &o )
        : alive( o.alive.load() ),
          consecutive_failures( o.consecutive_failures.load() ),
          consecutive_successes( o.consecutive_successes.load() ),
          last_rtt_ms( o.last_rtt_ms.load() ),
          last_ping_at( o.last_ping_at.load() ),
          last_reply_at( o.last_reply_at.load() ) {
    }
    RouteRuntime &operator=( const RouteRuntime &o ) {
        alive = o.alive.load();
        consecutive_failures = o.consecutive_failures.load();
        consecutive_successes = o.consecutive_successes.load();
        last_rtt_ms = o.last_rtt_ms.load();
        last_ping_at = o.last_ping_at.load();
        last_reply_at = o.last_reply_at.load();
        return *this;
    }
};

struct RouteEntry {
    RouteConfig cfg;
    RouteRuntime rt;
};

class CCspRouteMap {
public:
    CCspRouteMap() = default;

    /** 캐시 재로드. 런타임 상태는 기존 엔트리의 경우 보존. */
    bool Sync();

    /** 참조 무결성 검증 — LocalNodeMap / RemoteNodeMap 에 ref 존재 여부.
     *  누락 ref 가 있는 Route 는 로그 남기고 disabled 취급.
     *  Sync() 이후 Local/Remote Map 이 적재되면 호출. */
    void ValidateRefs();

    /** name 조회. */
    RouteConfig GetByName( const std::string &name ) const;

    /** (local, remote) pair 조회. */
    RouteConfig GetByPair( const std::string &localName, const std::string &remoteName ) const;

    /** 인바운드 Route 식별 — 수신 LocalNode(name, 빈 문자열이면 무관)로 들어온 srcIp[:srcPort]/transport 의 요청이
     *  어느 Route 의 RemoteNode 에서 왔는가. enabled Route 중 RemoteNode.ip == srcIp 이고 protocol 이 맞는 것을 고르되,
     *  RemoteNode.port == srcPort 인 것을 우선한다(UDP 는 피어가 수신 포트로 보내므로 같은 IP 에 피어 여럿도 가른다.
     *  TCP/TLS 소스 포트는 임시 포트라 IP 만 맞으면 된다). 없으면 IsValid()==false.
     *  RemoteNode.ip 가 호스트명이면 여기서는 매칭되지 않는다(IP 리터럴만). */
    RouteConfig FindInbound( const std::string &localName, const std::string &srcIp, int srcPort,
                             const std::string &transport ) const;

    /** 전체 스냅샷 (config 부분만). */
    std::vector<RouteConfig> GetAll() const;

    size_t Size() const;

    // ─ 런타임 상태 조작 (헬스체크 모듈 CCspRouteHealth 가 호출) ─
    /** 프로브 성공. dead 였으면 연속 성공이 iRecoveryProbes 이상일 때 alive 로 전이 — 전이하면 bWentAlive=true. */
    bool MarkAlive( const std::string &routeName, int rtt_ms, int iRecoveryProbes, bool &bWentAlive );
    /** 프로브 실패. 연속 실패가 iDeadThreshold(>0) 이상이면 dead 로 전이 — 전이하면 bWentDead=true. */
    bool MarkFail( const std::string &routeName, int iDeadThreshold, bool &bWentDead );
    bool MarkAlive( const std::string &routeName, int rtt_ms );
    bool MarkFail( const std::string &routeName );
    /** 상태를 직접 놓는다 — 등록형 트렁크(바인딩 유무 = 도달 가능성, CCspTrunkRegistrar). 전이가 있었으면
     * bChanged=true. */
    bool SetAlive( const std::string &routeName, bool bAlive, bool &bChanged );
    bool IsAlive( const std::string &routeName ) const;
    /** 프로브 송신 시각 기록 (헬스체크 주기 판정용). */
    void TouchPing( const std::string &routeName, long now );
    /** 런타임 스냅샷 — 상태 export 용 (없으면 false). */
    bool GetRuntime( const std::string &routeName, RouteRuntime &out ) const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, RouteEntry> m_byName;
    // (local, remote) → name 보조 인덱스
    std::map<std::pair<std::string, std::string>, std::string> m_byPair;
};

extern CCspRouteMap gclsRouteMap;

#endif  // __CSP_ROUTE_MAP_H__
