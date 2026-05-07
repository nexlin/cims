#include "CspAclPolicyEngine.h"

#include <algorithm>

#include "CspConfigCache.h"
#include "Log.h"
#include "SimpleJson.h"

CspAclPolicyEngine gclsAclPolicyEngine;

namespace {
    bool _boolish( const std::string& v, bool defTrue ) {
        if ( v.empty() ) return defTrue;
        if ( v == "false" || v == "0" ) return false;
        return true;
    }
}  // namespace

bool CspAclPolicyEngine::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_ACL_POLICY );
    std::vector<Policy> newList;
    if ( items.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < items.Size(); ++i ) {
            SimpleJson::JsonNode row = items.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            Policy p;
            p.name = row.GetString( "name" );
            p.priority = (int)row.GetInt( "priority", 100 );
            p.match_rule_set_ref = row.GetString( "match_rule_set_ref" );
            p.scope = row.GetString( "scope", "global" );
            p.scope_ref = row.GetString( "scope_ref" );
            p.action = row.GetString( "action", "deny" );
            p.enabled = _boolish( row.GetString( "enabled" ), true );
            if ( p.name.empty() || p.match_rule_set_ref.empty() ) {
                CLog::Print( LOG_ERROR, "AclPolicyEngine: skip invalid policy (name='%s' match_rule_set_ref='%s')",
                             p.name.c_str(), p.match_rule_set_ref.c_str() );
                continue;
            }
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
    CLog::Print( LOG_INFO, "AclPolicyEngine: sync complete, %zu policies", Size() );
    return true;
}

size_t CspAclPolicyEngine::Size() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_policies.size();
}

AclDecision CspAclPolicyEngine::Check( const MessageCtx& ctx, const std::string& local_node_name,
                                       const std::string& route_name, const std::string& route_set_name ) {
    std::vector<Policy> snapshot;
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        snapshot = m_policies;
    }

    for ( const auto& p : snapshot ) {
        if ( !p.enabled ) continue;

        // scope 필터링
        if ( p.scope == "local_node" ) {
            if ( p.scope_ref != local_node_name ) continue;
        } else if ( p.scope == "route" ) {
            if ( p.scope_ref != route_name ) continue;
        } else if ( p.scope == "route_set" ) {
            if ( p.scope_ref != route_set_name ) continue;
        }
        // global 은 항상 통과

        // RuleSet match
        if ( !gclsRuleEvaluator.MatchRuleSet( p.match_rule_set_ref, ctx ) ) continue;

        AclDecision d;
        d.matched_policy = p.name;
        if ( p.action == "deny" ) {
            d.allowed = false;
            d.reason = "acl deny: policy=" + p.name;
        } else {
            d.allowed = true;
        }
        return d;
    }

    // 매칭 없음 → 기본 ALLOW
    AclDecision d;
    d.allowed = true;
    return d;
}
