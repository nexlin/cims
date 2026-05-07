#include "CspRuleEvaluator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>

#include "CspConfigCache.h"
#include "Log.h"
#include "SimpleJson.h"

CspRuleEvaluator gclsRuleEvaluator;

// ─────────────────────────────────────────────────────────────
// 내부 유틸

namespace {

    // 콤마 구분 문자열 → trimmed items
    std::vector<std::string> _splitList( const std::string& s ) {
        std::vector<std::string> out;
        std::string cur;
        for ( char c : s ) {
            if ( c == ',' ) {
                while ( !cur.empty() && ( cur.front() == ' ' || cur.front() == '\t' ) ) cur.erase( cur.begin() );
                while ( !cur.empty() && ( cur.back() == ' ' || cur.back() == '\t' ) ) cur.pop_back();
                if ( !cur.empty() ) out.push_back( cur );
                cur.clear();
            } else {
                cur.push_back( c );
            }
        }
        while ( !cur.empty() && ( cur.front() == ' ' || cur.front() == '\t' ) ) cur.erase( cur.begin() );
        while ( !cur.empty() && ( cur.back() == ' ' || cur.back() == '\t' ) ) cur.pop_back();
        if ( !cur.empty() ) out.push_back( cur );
        return out;
    }

    // CIDR 매칭 (IPv4).  v6 는 향후.
    bool _cidrMatch( const std::string& ip, const std::string& cidr ) {
        auto slash = cidr.find( '/' );
        std::string net = cidr;
        int prefix = 32;
        if ( slash != std::string::npos ) {
            net = cidr.substr( 0, slash );
            prefix = atoi( cidr.c_str() + slash + 1 );
        }
        auto toUint = []( const std::string& s ) -> uint32_t {
            uint32_t r = 0;
            int o[4] = { 0, 0, 0, 0 };
            if ( sscanf( s.c_str(), "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3] ) != 4 ) return 0;
            for ( int i = 0; i < 4; ++i ) r = ( r << 8 ) | (uint8_t)o[i];
            return r;
        };
        uint32_t ipN = toUint( ip );
        uint32_t netN = toUint( net );
        if ( prefix <= 0 ) return true;
        if ( prefix >= 32 ) return ipN == netN;
        uint32_t mask = 0xFFFFFFFFu << ( 32 - prefix );
        return ( ipN & mask ) == ( netN & mask );
    }

    bool _boolish( const std::string& v, bool defTrue = true ) {
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
            for ( const auto& mem : rs.members ) {
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

bool CspRuleEvaluator::HasRule( const std::string& name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_rules.find( name ) != m_rules.end();
}

bool CspRuleEvaluator::HasRuleSet( const std::string& name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_ruleSets.find( name ) != m_ruleSets.end();
}

// ─────────────────────────────────────────────────────────────
// 필드 매핑

const std::string* CspRuleEvaluator::_getFieldValue( const MessageCtx& ctx, const std::string& field ) const {
    if ( field == "from_uri_host" ) return &ctx.from_uri_host;
    if ( field == "from_uri_user" ) return &ctx.from_uri_user;
    if ( field == "to_uri_host" ) return &ctx.to_uri_host;
    if ( field == "to_uri_user" ) return &ctx.to_uri_user;
    if ( field == "req_uri_host" ) return &ctx.req_uri_host;
    if ( field == "req_uri_user" ) return &ctx.req_uri_user;
    if ( field == "src_ip" ) return &ctx.src_ip;
    if ( field == "dst_ip" ) return &ctx.dst_ip;
    if ( field == "user_agent" ) return &ctx.user_agent;
    if ( field == "method" ) return &ctx.method;
    if ( field == "p_asserted_identity" ) return &ctx.p_asserted_identity;
    if ( field == "via_host" ) return &ctx.via_host;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────
// 연산자

bool CspRuleEvaluator::_applyOp( const std::string& fv, const std::string& op, const std::string& val,
                                 bool fieldExists ) const {
    if ( op == "exists" ) return fieldExists && !fv.empty();
    if ( op == "not_exists" ) return !fieldExists || fv.empty();

    // 다른 op 들은 fv 가 비어있으면 일반적으로 false (in_list 제외는 아래 처리)
    if ( op == "eq" ) return fv == val;
    if ( op == "ne" ) return fv != val;
    if ( op == "prefix" ) return fv.size() >= val.size() && fv.compare( 0, val.size(), val ) == 0;
    if ( op == "suffix" ) return fv.size() >= val.size() && fv.compare( fv.size() - val.size(), val.size(), val ) == 0;
    if ( op == "contains" ) return !val.empty() && fv.find( val ) != std::string::npos;
    if ( op == "regex" ) {
        try {
            std::regex re( val, std::regex::ECMAScript );
            return std::regex_search( fv, re );
        } catch ( const std::regex_error& e ) {
            CLog::Print( LOG_ERROR, "RuleEvaluator: bad regex '%s': %s", val.c_str(), e.what() );
            return false;
        }
    }
    if ( op == "in_cidr" ) return _cidrMatch( fv, val );
    if ( op == "in_list" ) {
        auto items = _splitList( val );
        for ( const auto& it : items )
            if ( it == fv ) return true;
        return false;
    }

    CLog::Print( LOG_ERROR, "RuleEvaluator: unknown op '%s'", op.c_str() );
    return false;
}

bool CspRuleEvaluator::_evalRule( const Rule& r, const MessageCtx& ctx ) const {
    if ( !r.enabled ) return false;
    const std::string* fv = _getFieldValue( ctx, r.field );
    if ( !fv ) {
        CLog::Print( LOG_ERROR, "RuleEvaluator: rule '%s' unknown field '%s'", r.name.c_str(), r.field.c_str() );
        return false;
    }
    // fieldExists 는 "필드가 의미있게 존재" 를 표현. 현재 구조에서는 MessageCtx 의 멤버가
    // 빈 문자열이면 "존재하지 않음" 으로 간주. 필요 시 향후 optional<string> 로 확장.
    bool fieldExists = !fv->empty();
    return _applyOp( *fv, r.op, r.value, fieldExists );
}

// ─────────────────────────────────────────────────────────────
// Match

bool CspRuleEvaluator::MatchRule( const std::string& ruleName, const MessageCtx& ctx ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_rules.find( ruleName );
    if ( it == m_rules.end() ) return false;
    return _evalRule( it->second, ctx );
}

bool CspRuleEvaluator::MatchRuleSet( const std::string& ruleSetName, const MessageCtx& ctx ) const {
    if ( ruleSetName.empty() ) return true;  // catch-all 의미
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_ruleSets.find( ruleSetName );
    if ( it == m_ruleSets.end() ) {
        CLog::Print( LOG_DEBUG, "RuleEvaluator: unknown rule_set '%s' — treating as no-match", ruleSetName.c_str() );
        return false;
    }
    const RuleSet& rs = it->second;
    if ( !rs.enabled ) return false;
    if ( rs.members.empty() ) return true;  // 빈 set → true (사용자 원칙)

    bool isAnd = ( rs.combinator != "OR" );  // default AND

    if ( isAnd ) {
        for ( const auto& m : rs.members ) {
            auto rIt = m_rules.find( m.rule_ref );
            bool res = ( rIt != m_rules.end() ) && _evalRule( rIt->second, ctx );
            if ( m.negate ) res = !res;
            if ( !res ) return false;
        }
        return true;
    } else {
        // OR
        for ( const auto& m : rs.members ) {
            auto rIt = m_rules.find( m.rule_ref );
            bool res = ( rIt != m_rules.end() ) && _evalRule( rIt->second, ctx );
            if ( m.negate ) res = !res;
            if ( res ) return true;
        }
        return false;
    }
}
