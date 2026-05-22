#ifndef __CSP_ACL_POLICY_ENGINE_H__
#define __CSP_ACL_POLICY_ENGINE_H__

#include <mutex>
#include <string>
#include <vector>

#include "CspRuleEvaluator.h"  // MessageCtx

/**
 * CspAclPolicyEngine — acl_policies.jsonl 평가 (v3, 2026-04-22).
 *
 *   priority 오름차순으로 정책 평가, 첫 match 의 action(allow|deny) 반환.
 *   scope (global/local_node/route/route_set) 로 적용 범위 필터.
 *   Rule/RuleSet 은 RuleEvaluator 공용.
 *
 *   매칭 정책이 없으면 기본 ALLOW (open by default).
 *   DENY 를 앞세우려면 적절한 priority 로 catch-all deny 정책을 둬야 함.
 */

struct AclDecision {
    bool allowed = true;
    std::string matched_policy;  // 매칭된 정책 name (디버그)
    std::string reason;          // deny 인 경우 이유
};

class CspAclPolicyEngine {
public:
    CspAclPolicyEngine() = default;

    /** 캐시에서 정책 재로드. priority 오름차순 정렬. */
    bool Sync();

    /** scope 필터링 컨텍스트.
     *  local_node_name: 수신 Local Node (scope=local_node 에 사용).
     *  route_name     : 해당 호가 탄 Route (outbound 시 scope=route 용, 없으면 빈 문자열).
     *  route_set_name : 해당 호가 속한 RouteSet (scope=route_set 용, 없으면 빈 문자열). */
    AclDecision Check( const MessageCtx &ctx, const std::string &local_node_name, const std::string &route_name,
                       const std::string &route_set_name );

    size_t Size() const;

private:
    struct Policy {
        std::string name;
        int priority = 100;
        std::string match_rule_set_ref;  // required
        std::string scope;               // global | local_node | route | route_set
        std::string scope_ref;           // scope ≠ global 일 때
        std::string action;              // allow | deny
        bool enabled = true;
    };

    mutable std::mutex m_mutex;
    std::vector<Policy> m_policies;
};

extern CspAclPolicyEngine gclsAclPolicyEngine;

#endif  // __CSP_ACL_POLICY_ENGINE_H__
