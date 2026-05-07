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
    std::string auth_realm;
    int max_concurrent_calls = 0;
    int cps_limit = 0;
    bool enabled = true;
    std::vector<std::string> tags;
    std::string note;

    bool IsValid() const {
        return !name.empty() && !local_node_ref.empty() && !remote_node_ref.empty();
    }
};

struct RouteRuntime {
    std::atomic<bool> alive{ true };
    std::atomic<int> consecutive_failures{ 0 };
    std::atomic<int> last_rtt_ms{ -1 };
    std::atomic<long> last_ping_at{ 0 };
    std::atomic<long> last_reply_at{ 0 };

    RouteRuntime() = default;
    RouteRuntime( const RouteRuntime& o )
        : alive( o.alive.load() ),
          consecutive_failures( o.consecutive_failures.load() ),
          last_rtt_ms( o.last_rtt_ms.load() ),
          last_ping_at( o.last_ping_at.load() ),
          last_reply_at( o.last_reply_at.load() ) {
    }
    RouteRuntime& operator=( const RouteRuntime& o ) {
        alive = o.alive.load();
        consecutive_failures = o.consecutive_failures.load();
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
    RouteConfig GetByName( const std::string& name ) const;

    /** (local, remote) pair 조회. */
    RouteConfig GetByPair( const std::string& localName, const std::string& remoteName ) const;

    /** 전체 스냅샷 (config 부분만). */
    std::vector<RouteConfig> GetAll() const;

    size_t Size() const;

    // ─ 런타임 상태 조작 (헬스체크 모듈이 호출) ─
    bool MarkAlive( const std::string& routeName, int rtt_ms );
    bool MarkFail( const std::string& routeName );
    bool IsAlive( const std::string& routeName ) const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, RouteEntry> m_byName;
    // (local, remote) → name 보조 인덱스
    std::map<std::pair<std::string, std::string>, std::string> m_byPair;
};

extern CCspRouteMap gclsRouteMap;

#endif  // __CSP_ROUTE_MAP_H__
