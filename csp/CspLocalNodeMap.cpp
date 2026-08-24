#include "CspLocalNodeMap.h"

#include "CspConfigCache.h"  // CspUuidToIntId
#include "Log.h"
#include "SimpleJson.h"

CCspLocalNodeMap gclsLocalNodeMap;

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

bool CCspLocalNodeMap::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems( CACHE_LOCAL_NODE );
    std::map<std::string, LocalNodeInfo> newMap;
    if ( items.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < items.Size(); ++i ) {
            SimpleJson::JsonNode row = items.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            LocalNodeInfo n;
            n.id = row.GetString( "id" );
            n.name = row.GetString( "name" );
            n.edge = row.GetString( "edge", "access" );
            n.bind_ip = row.GetString( "bind_ip", "0.0.0.0" );
            n.bind_port = (int)row.GetInt( "bind_port", 0 );
            n.protocol = row.GetString( "protocol", "UDP" );
            n.client_port = (int)row.GetInt( "client_port", 0 );
            n.thread_count = (int)row.GetInt( "thread_count", 0 );
            n.enabled = _boolish( row.GetString( "enabled" ), true );
            n.is_primary = _boolish( row.GetString( "is_primary" ), false );
            n.tls_cert_path = row.GetString( "tls_cert_path" );
            n.tls_key_path = row.GetString( "tls_key_path" );
            n.tls_ca_path = row.GetString( "tls_ca_path" );
            n.tls_verify_peer = _boolish( row.GetString( "tls_verify_peer" ), false );
            n.max_connections = (int)row.GetInt( "max_connections", 0 );
            n.tags = _readStringArray( row.Get( "tags" ) );
            n.note = row.GetString( "note" );
            if ( n.name.empty() ) {
                CLog::Print( LOG_ERROR, "LocalNodeMap: skip record with empty name (id=%s)", n.id.c_str() );
                continue;
            }
            if ( newMap.count( n.name ) ) {
                CLog::Print( LOG_ERROR, "LocalNodeMap: duplicate name '%s' — keeping last", n.name.c_str() );
            }
            newMap[n.name] = n;
        }
    }
    {
        std::lock_guard<std::mutex> lk( m_mutex );
        m_byName.swap( newMap );
    }
    CLog::Print( LOG_INFO, "LocalNodeMap: sync complete, %zu nodes", Size() );
    return true;
}

LocalNodeInfo CCspLocalNodeMap::GetByName( const std::string &name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_byName.find( name );
    if ( it == m_byName.end() ) return LocalNodeInfo();
    return it->second;
}

LocalNodeInfo CCspLocalNodeMap::GetById( const std::string &id ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &kv : m_byName ) {
        if ( kv.second.id == id ) return kv.second;
    }
    return LocalNodeInfo();
}

/** IPsec 접속점 n 의 역할 id 가 listenerIntId 와 같으면 그 역할 */
static EIpsecListenerRole _ipsecRoleOf( const LocalNodeInfo &n, int recordIntId, int listenerIntId ) {
    if ( !n.IsIpsec() ) return IPSEC_LISTENER_NONE;
    static const EIpsecListenerRole arr[3] = { IPSEC_LISTENER_SERVER_UDP, IPSEC_LISTENER_SERVER_TCP,
                                               IPSEC_LISTENER_CLIENT_UDP };
    for ( int i = 0; i < 3; ++i )
        if ( CspIpsecListenerIntId( recordIntId, arr[i] ) == listenerIntId ) return arr[i];
    return IPSEC_LISTENER_NONE;
}

LocalNodeInfo CCspLocalNodeMap::GetByIntId( int listenerIntId ) const {
    if ( listenerIntId <= 0 ) return LocalNodeInfo();
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &kv : m_byName ) {
        const int iRec = CspUuidToIntId( kv.second.id );
        if ( iRec == listenerIntId ) return kv.second;
        if ( _ipsecRoleOf( kv.second, iRec, listenerIntId ) != IPSEC_LISTENER_NONE ) return kv.second;
    }
    return LocalNodeInfo();
}

EIpsecListenerRole CCspLocalNodeMap::GetIpsecRole( int listenerIntId ) const {
    if ( listenerIntId <= 0 ) return IPSEC_LISTENER_NONE;
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &kv : m_byName ) {
        EIpsecListenerRole e = _ipsecRoleOf( kv.second, CspUuidToIntId( kv.second.id ), listenerIntId );
        if ( e != IPSEC_LISTENER_NONE ) return e;
    }
    return IPSEC_LISTENER_NONE;
}

int CCspLocalNodeMap::GetListenerPort( int listenerIntId ) const {
    if ( listenerIntId <= 0 ) return 0;
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &kv : m_byName ) {
        const int iRec = CspUuidToIntId( kv.second.id );
        if ( iRec == listenerIntId ) return kv.second.bind_port;
        EIpsecListenerRole e = _ipsecRoleOf( kv.second, iRec, listenerIntId );
        if ( e == IPSEC_LISTENER_CLIENT_UDP ) return kv.second.client_port;
        if ( e != IPSEC_LISTENER_NONE ) return kv.second.bind_port;
    }
    return 0;
}

int CCspLocalNodeMap::ToRecordIntId( int listenerIntId ) const {
    if ( listenerIntId <= 0 ) return listenerIntId;
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &kv : m_byName ) {
        const int iRec = CspUuidToIntId( kv.second.id );
        if ( _ipsecRoleOf( kv.second, iRec, listenerIntId ) != IPSEC_LISTENER_NONE ) return iRec;
    }
    return listenerIntId;
}

LocalNodeInfo CCspLocalNodeMap::GetIpsecNode() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    for ( const auto &kv : m_byName ) {  // map 은 name 사전식
        const LocalNodeInfo &n = kv.second;
        if ( n.enabled && n.IsIpsec() && n.edge == "access" ) return n;
    }
    return LocalNodeInfo();
}

std::vector<LocalNodeInfo> CCspLocalNodeMap::GetAll() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    std::vector<LocalNodeInfo> out;
    out.reserve( m_byName.size() );
    for ( const auto &kv : m_byName ) out.push_back( kv.second );
    return out;
}

LocalNodeInfo CCspLocalNodeMap::GetPrimary() const {
    // m_byName 은 std::map 이므로 iteration 이 이미 name 사전식 오름차순.
    // 기존 R1 semantics 유지: is_primary=true 면 protocol 무관 identity 주입.
    std::lock_guard<std::mutex> lk( m_mutex );

    // Rule 1: enabled=true && is_primary=true
    LocalNodeInfo found;
    int primaryCount = 0;
    for ( const auto &kv : m_byName ) {
        const LocalNodeInfo &n = kv.second;
        if ( !n.enabled || !n.is_primary ) continue;
        if ( primaryCount == 0 ) found = n;
        ++primaryCount;
    }
    if ( primaryCount > 1 ) {
        CLog::Print( LOG_ERROR, "LocalNodeMap: multiple is_primary=true nodes (%d) — using '%s'", primaryCount,
                     found.name.c_str() );
    }
    if ( primaryCount >= 1 ) return found;

    // Rule 2: enabled=true && edge=access && protocol=UDP
    for ( const auto &kv : m_byName ) {
        const LocalNodeInfo &n = kv.second;
        if ( !n.enabled ) continue;
        if ( n.edge != "access" ) continue;
        if ( n.protocol != "UDP" ) continue;
        return n;
    }

    // Rule 3: 없음
    return LocalNodeInfo();
}

LocalNodeInfo CCspLocalNodeMap::GetPrimaryByProtocol( const std::string &protocol ) const {
    // G9: protocol 엄격 필터. TCP/TLS primary 주입에 사용.
    std::lock_guard<std::mutex> lk( m_mutex );

    // Rule 1: enabled=true && is_primary=true && protocol match
    LocalNodeInfo found;
    int primaryCount = 0;
    for ( const auto &kv : m_byName ) {
        const LocalNodeInfo &n = kv.second;
        if ( !n.enabled || !n.is_primary ) continue;
        if ( n.protocol != protocol ) continue;
        if ( primaryCount == 0 ) found = n;
        ++primaryCount;
    }
    if ( primaryCount > 1 ) {
        CLog::Print( LOG_ERROR, "LocalNodeMap: multiple is_primary=true for protocol=%s (%d) — using '%s'",
                     protocol.c_str(), primaryCount, found.name.c_str() );
    }
    if ( primaryCount >= 1 ) return found;

    // Rule 2: enabled=true && edge=access && protocol match
    for ( const auto &kv : m_byName ) {
        const LocalNodeInfo &n = kv.second;
        if ( !n.enabled ) continue;
        if ( n.edge != "access" ) continue;
        if ( n.protocol != protocol ) continue;
        return n;
    }

    // Rule 3: 없음
    return LocalNodeInfo();
}

size_t CCspLocalNodeMap::Size() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_byName.size();
}

bool CCspLocalNodeMap::HasName( const std::string &name ) const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_byName.find( name ) != m_byName.end();
}
