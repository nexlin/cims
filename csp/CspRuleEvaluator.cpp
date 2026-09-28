#include "CspRuleEvaluator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>

#include "CspConfigCache.h"
#include "CspDialPlan.h"
#include "Log.h"
#include "SimpleJson.h"

CspRuleEvaluator gclsRuleEvaluator;

// ─────────────────────────────────────────────────────────────
// 내부 유틸

namespace {

    bool _boolish( const std::string &v, bool defTrue = true ) {
        if ( v.empty() ) return defTrue;
        if ( v == "false" || v == "0" ) return false;
        return true;
    }

}  // namespace

// ─────────────────────────────────────────────────────────────
// LoadAll

bool CspRuleEvaluator::LoadAll() {
    std::map<std::string, Rule> newRules;
    std::map<std::string, RuleSet> newRuleSets;

    // rules
    SimpleJson::JsonNode rulesArr = gclsCspConfigCache.GetItems( CACHE_RULE );
    if ( rulesArr.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < rulesArr.Size(); ++i ) {
            SimpleJson::JsonNode row = rulesArr.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            Rule r;
            r.name = row.GetString( "name" );
            r.field = row.GetString( "field" );
            r.op = row.GetString( "op" );
            r.value = row.GetString( "value" );
            r.enabled = _boolish( row.GetString( "enabled" ), true );
            if ( r.name.empty() || r.field.empty() || r.op.empty() ) continue;
            if ( !RuleFieldSyntaxOk( r.field ) )
                CLog::Print( LOG_ERROR,
                             "RuleEvaluator: rule '%s' field '%s' 를 해석할 수 없다 — 이 Rule 은 늘 불일치다",
                             r.name.c_str(), r.field.c_str() );
            newRules[r.name] = r;
        }
    }

    // rule_sets
    SimpleJson::JsonNode setsArr = gclsCspConfigCache.GetItems( CACHE_RULE_SET );
    if ( setsArr.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < setsArr.Size(); ++i ) {
            SimpleJson::JsonNode row = setsArr.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            RuleSet rs;
            rs.name = row.GetString( "name" );
            rs.combinator = row.GetString( "combinator", "AND" );
            rs.enabled = _boolish( row.GetString( "enabled" ), true );
            SimpleJson::JsonNode members = row.Get( "members" );
            if ( members.type == SimpleJson::JSON_ARRAY ) {
                for ( size_t j = 0; j < members.Size(); ++j ) {
                    SimpleJson::JsonNode m = members.At( j );
                    if ( m.type != SimpleJson::JSON_OBJECT ) continue;
                    RuleSetMember mem;
                    mem.rule_ref = m.GetString( "rule_ref" );
                    mem.negate = _boolish( m.GetString( "negate" ), false );
                    if ( mem.rule_ref.empty() ) continue;
                    rs.members.push_back( mem );
                }
            }
            if ( rs.name.empty() ) continue;
            // dangling ref 경고 (skip 하진 않음 — 런타임 평가 시 누락은 false 로 처리)
            for ( const auto &mem : rs.members ) {
                if ( newRules.find( mem.rule_ref ) == newRules.end() ) {
                    CLog::Print( LOG_ERROR, "RuleEvaluator: rule_set '%s' references missing rule '%s'",
                                 rs.name.c_str(), mem.rule_ref.c_str() );
                }
            }
            newRuleSets[rs.name] = rs;
        }
    }

    {
        std::lock_guard<std::mutex> lk( m_mutex );
        m_rules.swap( newRules );
        m_ruleSets.swap( newRuleSets );
    }
    CLog::Print( LOG_INFO, "RuleEvaluator: loaded rules=%zu rule_sets=%zu", RuleCount(), RuleSetCount() );
    return true;
}

// ─────────────────────────────────────────────────────────────
// 조회

size_t CspRuleEvaluator::RuleCount() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_rules.size();
}

size_t CspRuleEvaluator::RuleSetCount() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_ruleSets.size();
}

bool CspRuleEvaluator::HasRule( const std::string &name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_rules.find( name ) != m_rules.end();
}

bool CspRuleEvaluator::HasRuleSet( const std::string &name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_ruleSets.find( name ) != m_ruleSets.end();
}

// ─────────────────────────────────────────────────────────────
// Rule 평가 — field 해석·비교는 CspRuleField (옛 이름은 종전 값 그대로)

namespace {
    const char *const OPS[] = { "eq",      "ne",      "prefix",   "suffix", "contains",  "regex",
                                "in_cidr", "in_list", "in_range", "exists", "not_exists" };
    bool _knownOp( const std::string &op ) {
        for ( const char *o : OPS )
            if ( op == o ) return true;
        return false;
    }
}  // namespace

bool CspRuleEvaluator::_evalRule( const Rule &r, const MessageCtx &ctx ) const {
    if ( !r.enabled ) return false;
    std::vector<std::string> vecValues;
    bool bHost = false;
    if ( !RuleFieldValues( ctx, r.field, vecValues, bHost ) ) {
        CLog::Print( LOG_ERROR, "RuleEvaluator: rule '%s' unknown field '%s'", r.name.c_str(), r.field.c_str() );
        return false;
    }
    if ( !_knownOp( r.op ) ) {
        CLog::Print( LOG_ERROR, "RuleEvaluator: unknown op '%s'", r.op.c_str() );
        return false;
    }
    if ( r.op == "regex" ) {  // 잘못된 정규식은 적재 로그로 드러낸다 — 평가는 불일치
        try {
            std::regex re( r.value, std::regex::ECMAScript );
        } catch ( const std::regex_error &e ) {
            CLog::Print( LOG_ERROR, "RuleEvaluator: bad regex '%s': %s", r.value.c_str(), e.what() );
            return false;
        }
    }
    return RuleApplyOp( vecValues, r.op, r.value, bHost );
}

// ─────────────────────────────────────────────────────────────
// Match

bool CspRuleEvaluator::MatchRule( const std::string &ruleName, const MessageCtx &ctx ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_rules.find( ruleName );
    if ( it == m_rules.end() ) return false;
    return _evalRule( it->second, ctx );
}

bool CspRuleEvaluator::MatchRuleSet( const std::string &ruleSetName, const MessageCtx &ctx ) const {
    if ( ruleSetName.empty() ) return true;  // catch-all 의미
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_ruleSets.find( ruleSetName );
    if ( it == m_ruleSets.end() ) {
        CLog::Print( LOG_DEBUG, "RuleEvaluator: unknown rule_set '%s' — treating as no-match", ruleSetName.c_str() );
        return false;
    }
    const RuleSet &rs = it->second;
    if ( !rs.enabled ) return false;
    if ( rs.members.empty() ) return true;  // 빈 set → true (사용자 원칙)

    bool isAnd = ( rs.combinator != "OR" );  // default AND

    if ( isAnd ) {
        for ( const auto &m : rs.members ) {
            auto rIt = m_rules.find( m.rule_ref );
            bool res = ( rIt != m_rules.end() ) && _evalRule( rIt->second, ctx );
            if ( m.negate ) res = !res;
            if ( !res ) return false;
        }
        return true;
    } else {
        // OR
        for ( const auto &m : rs.members ) {
            auto rIt = m_rules.find( m.rule_ref );
            bool res = ( rIt != m_rules.end() ) && _evalRule( rIt->second, ctx );
            if ( m.negate ) res = !res;
            if ( res ) return true;
        }
        return false;
    }
}
