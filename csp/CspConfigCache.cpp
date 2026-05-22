#include "CspConfigCache.h"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "Log.h"

CCspConfigCache gclsCspConfigCache;

namespace {

    // v3: 9 collection 이름 (로그용)
    const char *kEntityName[CACHE_COUNT] = {
        "local_node", "remote_node",    "route",      "route_set",      "rule",
        "rule_set",   "routing_policy", "acl_policy", "access_service",
    };

    // agent 가 관리하는 jsonl 파일명 (kEntityName 과 순서 동일)
    const char *kJsonlFile[CACHE_COUNT] = {
        "local_nodes.jsonl", "remote_nodes.jsonl",     "routes.jsonl",       "route_sets.jsonl",      "rules.jsonl",
        "rule_sets.jsonl",   "routing_policies.jsonl", "acl_policies.jsonl", "access_services.jsonl",
    };

}  // namespace

// ─────────────────────────────────────────────────────────────

CCspConfigCache::CCspConfigCache() {
}
CCspConfigCache::~CCspConfigCache() {
}

const char *CCspConfigCache::EntityName( CspCacheEntity e ) {
    return ( e >= 0 && e < CACHE_COUNT ) ? kEntityName[e] : "unknown";
}

bool CCspConfigCache::Init( const std::string &jsonlDir ) {
    m_strJsonlDir = jsonlDir;
    if ( m_strJsonlDir.empty() ) {
        CLog::Print( LOG_ERROR, "CspConfigCache: Init called with empty jsonlDir — no dynamic config" );
    } else {
        CLog::Print( LOG_INFO, "CspConfigCache: init jsonlDir=%s", m_strJsonlDir.c_str() );
    }
    return true;
}

bool CCspConfigCache::ReloadFromJsonl() {
    if ( m_strJsonlDir.empty() ) return false;
    int loaded = 0;
    for ( int i = 0; i < CACHE_COUNT; ++i ) {
        if ( _loadFromJsonl( static_cast<CspCacheEntity>( i ) ) ) loaded++;
    }
    CLog::Print( LOG_INFO, "CspConfigCache: reloaded from jsonl (%d entities)", loaded );
    return loaded > 0;
}

SimpleJson::JsonNode CCspConfigCache::GetItems( CspCacheEntity e ) {
    std::lock_guard<std::mutex> lk( m_mutex );
    if ( e < 0 || e >= CACHE_COUNT ) {
        SimpleJson::JsonNode empty;
        empty.type = SimpleJson::JSON_ARRAY;
        return empty;
    }
    return m_entities[e].items;  // copy
}

bool CCspConfigCache::_loadFromJsonl( CspCacheEntity e ) {
    if ( m_strJsonlDir.empty() ) return false;
    std::string path = m_strJsonlDir + "/" + kJsonlFile[e];
    std::ifstream ifs( path );
    SimpleJson::JsonNode arr;
    arr.type = SimpleJson::JSON_ARRAY;
    if ( !ifs ) {
        // 파일 없음 = 빈 배열로 취급 (정상 상태)
        std::lock_guard<std::mutex> lk( m_mutex );
        m_entities[e].items = arr;
        m_entities[e].source = "jsonl-empty";
        m_entities[e].updatedAt = time( nullptr );
        CLog::Print( LOG_INFO, "CspConfigCache: %s jsonl absent — empty", kEntityName[e] );
        return true;
    }
    std::string line;
    int count = 0;
    while ( std::getline( ifs, line ) ) {
        while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) ) line.pop_back();
        if ( line.empty() ) continue;
        SimpleJson::JsonNode rec = SimpleJson::JsonNode::Parse( line );
        if ( rec.type != SimpleJson::JSON_OBJECT ) {
            CLog::Print( LOG_ERROR, "CspConfigCache: %s jsonl line skipped (not object)", kEntityName[e] );
            continue;
        }
        arr.Add( rec );
        ++count;
    }
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        m_entities[e].items = arr;
        m_entities[e].source = "jsonl";
        m_entities[e].updatedAt = time( nullptr );
    }
    CLog::Print( LOG_INFO, "CspConfigCache: loaded %s from %s (%d records)", kEntityName[e], path.c_str(), count );
    return true;
}
