#ifndef __CSP_RULE_EVALUATOR_H__
#define __CSP_RULE_EVALUATOR_H__

#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * CspRuleEvaluator — Rule / RuleSet 평가 공통 엔진 (v3 모델, 2026-04-22).
 *
 *   rules.jsonl      → Rule (field + op + value)
 *   rule_sets.jsonl  → RuleSet (combinator AND|OR, members = [{rule_ref, negate}])
 *
 *   이 엔진은 CspRoutingPolicyEngine 및 CspAclPolicyEngine 에서 공용으로 호출.
 *
 *   사용 순서:
 *     1) SIGUSR1 수신 시 LoadAll() — 캐시(CACHE_RULE, CACHE_RULE_SET) 에서 재로드
 *     2) 평가 시 MessageCtx 를 채우고 Match(ruleSetName, ctx)
 */

namespace SimpleJson {
    class JsonNode;
}

/** SIP 메시지에서 추출한 평가용 컨텍스트.
 *  빈 문자열 → exists 체크가 false. */
struct MessageCtx {
    std::string from_uri_host;
    std::string from_uri_user;
    std::string to_uri_host;
    std::string to_uri_user;
    std::string req_uri_host;
    std::string req_uri_user;
    std::string src_ip;
    std::string dst_ip;
    std::string user_agent;
    std::string method;
    std::string p_asserted_identity;
    std::string via_host;
};

class CspRuleEvaluator {
public:
    CspRuleEvaluator() = default;

    /** 캐시에서 Rules + RuleSets 재로드. SIGUSR1 시 호출. */
    bool LoadAll();

    /** Rule Set 평가. ruleSetName 이 비거나 존재하지 않으면 true (catch-all 의미).
     *  존재하면 members 를 combinator 로 집계, 각 member 의 rule 을 MatchRule 로 평가 후 negate 적용. */
    bool MatchRuleSet( const std::string &ruleSetName, const MessageCtx &ctx ) const;

    /** 단일 Rule 평가 (외부에서 직접 쓰는 경우용). */
    bool MatchRule( const std::string &ruleName, const MessageCtx &ctx ) const;

    /** 디버그 — 로드된 개수. */
    size_t RuleCount() const;
    size_t RuleSetCount() const;

    /** Rule 존재 여부. 주로 policy 적재 시 dangling ref 경고용. */
    bool HasRule( const std::string &name ) const;
    bool HasRuleSet( const std::string &name ) const;

private:
    struct Rule {
        std::string name;
        std::string field;  // from_uri_host, to_uri_user, ...
        std::string op;     // eq, ne, prefix, suffix, contains, regex, in_cidr, in_list, exists, not_exists
        std::string value;  // op 에 따른 인자 (in_list 는 콤마 분리)
        bool enabled = true;
    };
    struct RuleSetMember {
        std::string rule_ref;
        bool negate = false;
    };
    struct RuleSet {
        std::string name;
        std::string combinator;  // AND | OR
        std::vector<RuleSetMember> members;
        bool enabled = true;
    };

    const std::string *_getFieldValue( const MessageCtx &ctx, const std::string &field ) const;
    bool _applyOp( const std::string &fieldValue, const std::string &op, const std::string &value,
                   bool fieldExists ) const;
    bool _evalRule( const Rule &r, const MessageCtx &ctx ) const;

    mutable std::mutex m_mutex;
    std::map<std::string, Rule> m_rules;        // name → Rule
    std::map<std::string, RuleSet> m_ruleSets;  // name → RuleSet
};

extern CspRuleEvaluator gclsRuleEvaluator;

#endif  // __CSP_RULE_EVALUATOR_H__
