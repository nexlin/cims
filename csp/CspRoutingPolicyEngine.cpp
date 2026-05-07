#include "CspRoutingPolicyEngine.h"

#include <algorithm>

#include "CspConfigCache.h"
#include "CspRouteSetMap.h"
#include "Log.h"
#include "SimpleJson.h"

CspRoutingPolicyEngine gclsRoutingPolicyEngine;

namespace {
    bool _boolish( const std::string& v, bool defTrue ) {
        if ( v.empty() ) return defTrue;
        if ( v == "false" || v == "0" ) return false;
        return true;
    }
    std::vector<std::string> _readStringArray( SimpleJson::JsonNode arr ) {
        std::vector<std::string> out;
        if ( arr.type != SimpleJson::JSON_ARRAY ) return out;
        for ( size_t i = 0; i < arr.Size(); ++i ) {
            std::string s = arr.At( i ).AsString();
            if ( !s.empty() ) out.push_back( s );
        }
        return out;
    }
}  // namespace

bool CspRoutingPolicyEngine::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_ROUTING_POLICY );
    std::vector<Policy> newList;
    if ( items.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < items.Size(); ++i ) {
            SimpleJson::JsonNode row = items.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            Policy p;
            p.name = row.GetString( "name" );
            p.priority = (int)row.GetInt( "priority", 100 );
            p.match_rule_set_ref = row.GetString( "match_rule_set_ref" );
            p.target_type = row.GetString( "target_type", "route_set" );
            p.target_ref = row.GetString( "target_ref" );
            p.transform_rule_set_refs = _readStringArray( row.Get( "transform_rule_set_refs" ) );
            p.fail_action = row.GetString( "fail_action", "next_policy" );
            p.enabled = _boolish( row.GetString( "enabled" ), true );
            if ( p.name.empty() ) continue;
            newList.push_back( p );
        }
    }
    std::sort( newList.begin(), newList.end(), []( const Policy& a, const Policy& b ) {
        if ( a.priority != b.priority ) return a.priority < b.priority;
        return a.name < b.name;
    } );
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        m_policies.swap( newList );
    }
    CLog::Print( LOG_INFO, "RoutingPolicyEngine: sync complete, %zu policies", Size() );
    return true;
}

size_t CspRoutingPolicyEngine::Size() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_policies.size();
}

RoutingDecision CspRoutingPolicyEngine::Decide( const MessageCtx& ctx, const std::string& hashKey ) {
    std::vector<Policy> snapshot;
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        snapshot = m_policies;  // priority 순 정렬된 상태
    }

    for ( const auto& p : snapshot ) {
        if ( !p.enabled ) continue;

        // 1) RuleSet match 확인 (빈 ref 면 catch-all = true)
        bool matched = gclsRuleEvaluator.MatchRuleSet( p.match_rule_set_ref, ctx );
        if ( !matched ) continue;

        // 2) target_type 분기
        if ( p.target_type == "reject" ) {
            RoutingDecision d;
            d.type = ROUTING_REJECT;
            d.matched_policy = p.name;
            d.reason = "policy=reject";
            return d;
        }
        if ( p.target_type == "access_service" ) {
            RoutingDecision d;
            d.type = ROUTING_ACCESS_SERVICE;
            d.matched_policy = p.name;
            d.target_name = p.target_ref;
            return d;
        }
        if ( p.target_type == "route_set" ) {
            std::string reason;
            std::string picked = gclsRouteSetMap.SelectRoute( p.target_ref, hashKey, reason );
            if ( !picked.empty() ) {
                RoutingDecision d;
                d.type = ROUTING_ROUTE_SET;
                d.matched_policy = p.name;
                d.target_name = p.target_ref;
                d.picked_route = picked;
                return d;
            }
            // RouteSet 선택 실패 — fail_action 에 따라 분기
            CLog::Print( LOG_INFO, "RoutingPolicyEngine: '%s' matched but route_set '%s' unavailable (%s)",
                         p.name.c_str(), p.target_ref.c_str(), reason.c_str() );
            if ( p.fail_action == "reject" ) {
                RoutingDecision d;
                d.type = ROUTING_REJECT;
                d.matched_policy = p.name;
                d.reason = "route_set unavailable: " + reason;
                return d;
            }
            // next_policy — 계속 평가
            continue;
        }

        CLog::Print( LOG_ERROR, "RoutingPolicyEngine: policy '%s' unknown target_type '%s'", p.name.c_str(),
                     p.target_type.c_str() );
    }

    RoutingDecision d;
    d.type = ROUTING_NO_MATCH;
    d.reason = "no matching policy";
    return d;
}
