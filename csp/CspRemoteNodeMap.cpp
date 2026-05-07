#include "CspRemoteNodeMap.h"

#include "CspConfigCache.h"
#include "Log.h"
#include "SimpleJson.h"

CCspRemoteNodeMap gclsRemoteNodeMap;

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

bool CCspRemoteNodeMap::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_REMOTE_NODE );
    std::map<std::string, RemoteNodeInfo> newMap;
    if ( items.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < items.Size(); ++i ) {
            SimpleJson::JsonNode row = items.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            RemoteNodeInfo n;
            n.id = row.GetString( "id" );
            n.name = row.GetString( "name" );
            n.ip = row.GetString( "ip" );
            n.port = (int)row.GetInt( "port", 5060 );
            n.protocol = row.GetString( "protocol", "UDP" );
            n.remote_domain = row.GetString( "remote_domain" );
            n.srv_lookup = _boolish( row.GetString( "srv_lookup" ), false );
            n.dns_fallback = _boolish( row.GetString( "dns_fallback" ), true );
            n.tls_verify = _boolish( row.GetString( "tls_verify" ), false );
            n.enabled = _boolish( row.GetString( "enabled" ), true );
            n.tags = _readStringArray( row.Get( "tags" ) );
            n.note = row.GetString( "note" );
            if ( n.name.empty() ) {
                CLog::Print( LOG_ERROR, "RemoteNodeMap: skip record with empty name (id=%s)", n.id.c_str() );
                continue;
            }
            if ( newMap.count( n.name ) ) {
                CLog::Print( LOG_ERROR, "RemoteNodeMap: duplicate name '%s' — keeping last", n.name.c_str() );
            }
            newMap[n.name] = n;
        }
    }
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        m_byName.swap( newMap );
    }
    CLog::Print( LOG_INFO, "RemoteNodeMap: sync complete, %zu nodes", Size() );
    return true;
}

RemoteNodeInfo CCspRemoteNodeMap::GetByName( const std::string& name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byName.find( name );
    if ( it == m_byName.end() ) return RemoteNodeInfo();
    return it->second;
}

RemoteNodeInfo CCspRemoteNodeMap::GetById( const std::string& id ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto& kv : m_byName ) {
        if ( kv.second.id == id ) return kv.second;
    }
    return RemoteNodeInfo();
}

std::vector<RemoteNodeInfo> CCspRemoteNodeMap::GetAll() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    std::vector<RemoteNodeInfo> out;
    out.reserve( m_byName.size() );
    for ( const auto& kv : m_byName ) out.push_back( kv.second );
    return out;
}

size_t CCspRemoteNodeMap::Size() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_byName.size();
}

bool CCspRemoteNodeMap::HasName( const std::string& name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_byName.find( name ) != m_byName.end();
}
