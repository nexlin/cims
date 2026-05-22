#include "CspRouteMap.h"

#include <ctime>

#include "CspConfigCache.h"
#include "CspLocalNodeMap.h"
#include "CspRemoteNodeMap.h"
#include "Log.h"
#include "SimpleJson.h"

CCspRouteMap gclsRouteMap;

namespace {
    bool _boolish( const std::string &v, bool defTrue ) {
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

bool CCspRouteMap::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_ROUTE );
    std::map<std::string, RouteEntry> newMap;
    std::map<std::pair<std::string, std::string>, std::string> newPair;

    if ( items.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < items.Size(); ++i ) {
            SimpleJson::JsonNode row = items.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            RouteConfig c;
            c.id = row.GetString( "id" );
            c.name = row.GetString( "name" );
            c.local_node_ref = row.GetString( "local_node_ref" );
            c.remote_node_ref = row.GetString( "remote_node_ref" );
            c.outbound_proxy_ip = row.GetString( "outbound_proxy_ip" );
            c.outbound_proxy_port = (int)row.GetInt( "outbound_proxy_port", 0 );
            c.register_to_remote = _boolish( row.GetString( "register_to_remote" ), false );
            c.register_expires = (int)row.GetInt( "register_expires", 3600 );
            c.auth_user = row.GetString( "auth_user" );
            c.auth_password = row.GetString( "auth_password" );
            c.auth_realm = row.GetString( "auth_realm" );
            c.max_concurrent_calls = (int)row.GetInt( "max_concurrent_calls", 0 );
            c.cps_limit = (int)row.GetInt( "cps_limit", 0 );
            c.enabled = _boolish( row.GetString( "enabled" ), true );
            c.tags = _readStringArray( row.Get( "tags" ) );
            c.note = row.GetString( "note" );

            if ( !c.IsValid() ) {
                CLog::Print( LOG_ERROR, "RouteMap: skip invalid record (name='%s' local='%s' remote='%s')",
                             c.name.c_str(), c.local_node_ref.c_str(), c.remote_node_ref.c_str() );
                continue;
            }
            auto pairKey = std::make_pair( c.local_node_ref, c.remote_node_ref );
            if ( newPair.count( pairKey ) ) {
                CLog::Print(
                    LOG_ERROR, "RouteMap: duplicate (local='%s', remote='%s') pair — keeping first '%s', skipping '%s'",
                    c.local_node_ref.c_str(), c.remote_node_ref.c_str(), newPair[pairKey].c_str(), c.name.c_str() );
                continue;
            }
            if ( newMap.count( c.name ) ) {
                CLog::Print( LOG_ERROR, "RouteMap: duplicate route name '%s' — keeping first", c.name.c_str() );
                continue;
            }

            RouteEntry e;
            e.cfg = c;
            // 기존 런타임 상태 보존
            {
                std::lock_guard<std::mutex> lk( m_mutex );
                auto oldIt = m_byName.find( c.name );
                if ( oldIt != m_byName.end() ) {
                    e.rt = oldIt->second.rt;
                }
            }
            newMap[c.name] = e;
            newPair[pairKey] = c.name;
        }
    }

    {
        std::lock_guard<std::mutex> lk( m_mutex );
        m_byName.swap( newMap );
        m_byPair.swap( newPair );
    }
    CLog::Print( LOG_INFO, "RouteMap: sync complete, %zu routes", Size() );
    return true;
}

void CCspRouteMap::ValidateRefs() {
    std::lock_guard<std::mutex> lk( m_mutex );
    int invalid = 0;
    for ( auto &kv : m_byName ) {
        RouteConfig &c = kv.second.cfg;
        bool lnOk = gclsLocalNodeMap.HasName( c.local_node_ref );
        bool rnOk = gclsRemoteNodeMap.HasName( c.remote_node_ref );
        if ( !lnOk || !rnOk ) {
            CLog::Print( LOG_ERROR,
                         "RouteMap: route '%s' dangling ref (local='%s' found=%s, remote='%s' found=%s) — disabled",
                         c.name.c_str(), c.local_node_ref.c_str(), lnOk ? "yes" : "no", c.remote_node_ref.c_str(),
                         rnOk ? "yes" : "no" );
            c.enabled = false;
            ++invalid;
        }
    }
    if ( invalid > 0 ) {
        CLog::Print( LOG_SYSTEM, "RouteMap: %d route(s) disabled due to dangling refs", invalid );
    }
}

RouteConfig CCspRouteMap::GetByName( const std::string &name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byName.find( name );
    if ( it == m_byName.end() ) return RouteConfig();
    return it->second.cfg;
}

RouteConfig CCspRouteMap::GetByPair( const std::string &localName, const std::string &remoteName ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byPair.find( std::make_pair( localName, remoteName ) );
    if ( it == m_byPair.end() ) return RouteConfig();
    auto rit = m_byName.find( it->second );
    if ( rit == m_byName.end() ) return RouteConfig();
    return rit->second.cfg;
}

std::vector<RouteConfig> CCspRouteMap::GetAll() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    std::vector<RouteConfig> out;
    out.reserve( m_byName.size() );
    for ( const auto &kv : m_byName ) out.push_back( kv.second.cfg );
    return out;
}

size_t CCspRouteMap::Size() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_byName.size();
}

bool CCspRouteMap::MarkAlive( const std::string &routeName, int rtt_ms ) {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byName.find( routeName );
    if ( it == m_byName.end() ) return false;
    long now = (long)time( nullptr );
    bool was = it->second.rt.alive.load();
    it->second.rt.alive.store( true );
    it->second.rt.consecutive_failures.store( 0 );
    it->second.rt.last_rtt_ms.store( rtt_ms );
    it->second.rt.last_reply_at.store( now );
    if ( !was ) {
        CLog::Print( LOG_SYSTEM, "RouteMap: route '%s' went ALIVE (rtt=%dms)", routeName.c_str(), rtt_ms );
    }
    return true;
}

bool CCspRouteMap::MarkFail( const std::string &routeName ) {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byName.find( routeName );
    if ( it == m_byName.end() ) return false;
    int fails = ++it->second.rt.consecutive_failures;
    // dead 임계는 RouteSet 에서 판단 (여기선 카운트만 유지).
    // RouteSetMap 이 dead_threshold 넘으면 MarkDead 별도 호출 가능하지만,
    // 편의상 fails 만으로 판단하고 싶으면 여기서 alive=false 로.
    (void)fails;
    return true;
}

bool CCspRouteMap::IsAlive( const std::string &routeName ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byName.find( routeName );
    if ( it == m_byName.end() ) return false;
    if ( !it->second.cfg.enabled ) return false;
    return it->second.rt.alive.load();
}
