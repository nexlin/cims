#ifndef __CSP_ROUTE_SET_MAP_H__
#define __CSP_ROUTE_SET_MAP_H__

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * CspRouteSetMap — route_sets.jsonl 캐시 + 분배 정책 (v3, 2026-04-22).
 *
 *   Peering cluster 의 핵심. SelectRoute() 가 분배 정책에 따라 live 한 Route name 을 선택.
 *
 *   분배 정책:
 *     failover       : priority 오름차순, alive 중 첫번째
 *     round_robin    : cursor 순환, alive 만 선택
 *     weighted       : weight 비율 (alive 만), cursor + deficit 라운드로빈 근사
 *     hash_by_caller : (후속) 발신자 uri 의 해시로 고정
 *
 *   헬스체크 수집 (OPTIONS ping 발사 + 응답 처리) 은 이 클래스가 후속 스테이지에서 담당.
 *   현재 버전은 RouteMap 의 alive 상태만 소비.
 */

struct RouteSetMember {
    std::string route_ref;
    int priority = 100;
    int weight = 1;
};

struct RouteSetConfig {
    std::string id;
    std::string name;
    std::string distribution_policy;  // failover | round_robin | weighted | hash_by_caller
    std::vector<RouteSetMember> members;
    std::string health_check_mode;  // options_ping | invite_response | none
    int health_check_interval_sec = 30;
    int health_check_dead_threshold = 3;
    int health_check_recovery_probes = 1;
    std::string fallback_policy;  // reject | next_policy
    bool enabled = true;
    std::vector<std::string> tags;
    std::string note;

    bool IsValid() const {
        return !name.empty();
    }
};

struct RouteSetRuntime {
    std::atomic<int> rr_cursor{ 0 };         // round_robin / weighted 커서
    std::atomic<int> weighted_deficit{ 0 };  // weighted 알고리즘의 잔여량

    RouteSetRuntime() = default;
    RouteSetRuntime( const RouteSetRuntime& o )
        : rr_cursor( o.rr_cursor.load() ), weighted_deficit( o.weighted_deficit.load() ) {
    }
    RouteSetRuntime& operator=( const RouteSetRuntime& o ) {
        rr_cursor.store( o.rr_cursor.load() );
        weighted_deficit.store( o.weighted_deficit.load() );
        return *this;
    }
};

struct RouteSetEntry {
    RouteSetConfig cfg;
    RouteSetRuntime rt;
};

class CCspRouteSetMap {
public:
    CCspRouteSetMap() = default;

    bool Sync();

    /** RouteMap 참조 무결성 검증. member 의 route_ref 가 존재하지 않으면 경고. */
    void ValidateRefs();

    RouteSetConfig GetByName( const std::string& name ) const;
    std::vector<RouteSetConfig> GetAll() const;
    size_t Size() const;
    bool HasName( const std::string& name ) const;

    /** 분배 정책에 따라 live route_ref 한 개 반환. non-const: rr_cursor 상태 변경.
     *  @param routeSetName  대상 RouteSet name
     *  @param hashKey       hash_by_caller 일 때 사용할 key (e.g., From URI)
     *  @param outReason     실패 시 이유 (no members / all dead / ...)
     *  @return 선택된 route_ref (빈 문자열이면 선택 불가) */
    std::string SelectRoute( const std::string& routeSetName, const std::string& hashKey, std::string& outReason );

private:
    mutable std::mutex m_mutex;
    std::map<std::string, RouteSetEntry> m_byName;

    // 내부: 정책별 선택 함수. m_mutex 를 잡고 호출.
    std::string _selectFailover( RouteSetEntry& e, std::string& outReason );
    std::string _selectRoundRobin( RouteSetEntry& e, std::string& outReason );
    std::string _selectWeighted( RouteSetEntry& e, std::string& outReason );
    std::string _selectHashByCaller( const RouteSetEntry& e, const std::string& key, std::string& outReason );
};

extern CCspRouteSetMap gclsRouteSetMap;

#endif  // __CSP_ROUTE_SET_MAP_H__
