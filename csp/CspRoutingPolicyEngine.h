#ifndef __CSP_ROUTING_POLICY_ENGINE_H__
#define __CSP_ROUTING_POLICY_ENGINE_H__

#include <mutex>
#include <string>
#include <vector>

#include "CspRuleEvaluator.h"  // MessageCtx

/**
 * CspRoutingPolicyEngine — routing_policies.jsonl 평가 (v3, 2026-04-22).
 *
 *   priority 오름차순으로 정책을 평가, 첫 match 의 target 을 반환.
 *   target_type:
 *     - route_set       : RouteSetMap.SelectRoute() 로 실제 Route 선택
 *     - access_service  : AccessServiceMap 으로 이관 (UE 호)
 *     - reject          : 호 거절
 *
 *   fail_action:
 *     - next_policy     : match 실패 또는 RouteSet dead 시 다음 정책 평가
 *     - reject          : 즉시 거절
 */

enum RoutingDecisionType {
    ROUTING_REJECT = 0,
    ROUTING_ROUTE_SET,       // target_name = RouteSet name, picked_route = 선택된 Route name
    ROUTING_ACCESS_SERVICE,  // target_name = AccessService name
    ROUTING_NO_MATCH         // 어떤 정책에도 매칭 안됨 (기본 거절)
};

struct RoutingDecision {
    RoutingDecisionType type = ROUTING_NO_MATCH;
    std::string matched_policy;  // match 된 policy name (디버그)
    std::string target_name;     // route_set 또는 access_service name
    std::string picked_route;    // RouteSet 인 경우 SelectRoute 결과
    std::string reason;          // 거절/실패 이유
};

class CspRoutingPolicyEngine {
public:
    CspRoutingPolicyEngine() = default;

    /** 캐시에서 정책 재로드. priority 오름차순 정렬. */
    bool Sync();

    /** 주어진 컨텍스트에 대해 라우팅 결정.
     *  @param ctx      SIP 메시지에서 추출한 필드
     *  @param hashKey  RouteSet.hash_by_caller 에 사용할 key (발신자 식별 문자열)
     *  @return 결정 (type + target + picked_route) */
    RoutingDecision Decide( const MessageCtx& ctx, const std::string& hashKey );

    /** 정책 수. */
    size_t Size() const;

private:
    struct Policy {
        std::string name;
        int priority = 100;
        std::string match_rule_set_ref;  // 빈 문자열 → catch-all
        std::string target_type;         // route_set | access_service | reject
        std::string target_ref;
        std::vector<std::string> transform_rule_set_refs;  // 예약 필드
        std::string fail_action;                           // reject | next_policy
        bool enabled = true;
    };

    mutable std::mutex m_mutex;
    std::vector<Policy> m_policies;  // priority 오름차순 정렬
};

extern CspRoutingPolicyEngine gclsRoutingPolicyEngine;

#endif  // __CSP_ROUTING_POLICY_ENGINE_H__
