#ifndef __CSP_ROUTE_ENGINE_H__
#define __CSP_ROUTE_ENGINE_H__

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

class CSipMessage;

/**
 * CspRouteEngine — SIP 라우팅 규칙 엔진.
 *
 *   CspConfigCache(route) 에서 규칙을 로드해 priority 정렬 + first-match-wins
 *   평가. 매칭된 규칙의 transform 를 적용한 뒤 target 트렁크를 결정한다.
 *
 *   평가 결과는 RouteDecision 으로 반환되며 호출자(ModuleDispatcher)가
 *   - forward(trunk 로 proxy/B2BUA)
 *   - reject(fail_code / fail_reason 응답)
 *   중 하나를 선택한다.
 *
 *   Hit counter 는 메모리에만 누적. CSC 가 주기적(또는 요청 시) DB 동기화.
 *
 *   P4 범위: request-uri / from / to / method / header 매칭 + URI rewrite +
 *   header 추가/삭제 변환 + single-trunk 타겟. priority_list / weighted 는
 *   뼈대만 제공하고 후속 phase 에서 확장.
 */

struct RouteMatchCond {
    std::string field;      // req_uri_user | req_uri_host | from_uri | to_uri | method | source_ip | header:<NAME>
    std::string op;         // equals | not_equals | prefix | suffix | contains | regex | cidr
    std::string value;
    bool        invert = false;
};

struct RouteTransformAction {
    std::string action;     // set_req_uri_user | set_req_uri_host | set_from_host |
                            // add_header | remove_header | replace_header |
                            // strip_prefix | add_prefix
    std::string target;     // header name 등
    std::string value;
};

struct RouteRule {
    int         id = 0;
    std::string name;
    bool        enabled = true;
    int         priority = 100;
    std::string description;

    std::vector<RouteMatchCond>        matches;      // AND 조합
    std::vector<RouteTransformAction>  transforms;   // 순서대로 적용

    std::string target_mode = "trunk";   // trunk | service | priority_list | round_robin | reject
    int         target_trunk_id = 0;
    int         target_service_id = 0;   // target_mode=="service" 일 때
    std::string target_json;              // priority_list / weighted 용 raw JSON

    std::string fail_action = "reject";   // reject | fallback | next_rule
    int         fail_code = 404;
    std::string fail_reason = "Not Found";
    int         fallback_trunk_id = 0;
    int         timeout_ms = 4000;
    int         retry_count = 0;

    // 메모리 히트 카운터
    uint64_t    hit_count = 0;
    time_t      last_hit_time = 0;
};

struct RouteDecision {
    bool matched = false;
    int  rule_id = 0;
    std::string rule_name;

    // 변환 적용된 Request-URI 조각 (요청 메시지는 ModuleDispatcher 가 직접 변조)
    // 이 구조체는 "적용해야 할 변환 목록" 을 그대로 넘김.
    std::vector<RouteTransformAction> apply;

    // 타겟
    std::string target_mode;
    int         target_trunk_id = 0;      // 0 이면 타겟 없음
    std::string target_ip;                // 트렁크 조회로 채움 (편의)
    int         target_port = 0;
    std::string target_protocol;          // "UDP"

    // reject
    bool reject = false;
    int  fail_code = 0;
    std::string fail_reason;
};

/** 컨텍스트 — 매칭 시 필요한 SIP 메시지 외부 정보. */
struct RouteContext {
    std::string source_ip;
    std::string source_trunk;   // 해당 요청이 특정 트렁크에서 들어온 경우 name
};

class CCspRouteEngine {
public:
    CCspRouteEngine() = default;

    /** 캐시에서 규칙을 로드해 메모리 상태 동기화. */
    bool Sync();

    /** SIP 메시지 + 컨텍스트로 규칙 평가. first-match-wins. */
    RouteDecision Evaluate(const CSipMessage* pclsMessage, const RouteContext& ctx);

    /** RouteDecision 의 apply 를 pclsMessage 에 실제로 적용. */
    bool ApplyTransforms(CSipMessage* pclsMessage, const std::vector<RouteTransformAction>& actions);

    /** 히트 카운터 증가 (평가 시 내부에서 호출). */
    void IncrementHit(int rule_id);

    /** 히트 카운터 스냅샷 (CSC 조회용). */
    struct HitEntry { int rule_id; uint64_t hit_count; time_t last_hit; };
    void GetHits(std::vector<HitEntry>& out);

    /** 규칙 목록 스냅샷 (디버그/dry-run 용). */
    std::vector<RouteRule> GetRules();

private:
    std::mutex m_mutex;
    std::vector<RouteRule> m_rules;   // priority 로 정렬

    bool _matchOne(const RouteMatchCond& c, const CSipMessage* msg, const RouteContext& ctx);
    void _fillTargetFromTrunk(RouteDecision& d, int trunk_id);
};

extern CCspRouteEngine gclsRouteEngine;

#endif // __CSP_ROUTE_ENGINE_H__
