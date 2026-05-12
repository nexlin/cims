#ifndef __CONSISTENT_HASH_RING_H__
#define __CONSISTENT_HASH_RING_H__

// Consistent hash ring for distributing keys (Session-ID) across N endpoints
// with healthcheck-aware skip. Phase 1.E (HA 이중화 — CMP/PMP All Active 분배).
//
// 사용:
//   CConsistentHashRing<int> ring(128);  // vnode = 128
//   ring.AddNode(0);                     // primary CMP idx=0
//   ring.AddNode(1);                     // secondary CMP idx=1
//   int idx = ring.Select("session-id-abc");
//
// healthcheck:
//   ring.MarkUnhealthy(1, 30);           // idx=1 을 30초간 ring 에서 제외
//   ring.MarkHealthy(1);                 // 즉시 복귀
//   ring.Select(key) 는 unhealthy 노드를 건너뛰고 다음 healthy 노드 반환.
//
// thread-safe — 모든 mutating / read 메서드가 내부 mutex 로 보호.

#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

template <typename NodeT>
class CConsistentHashRing {
public:
    explicit CConsistentHashRing( int iVnodesPerNode = 128 ) : m_iVnodes( iVnodesPerNode ) {
    }

    void AddNode( const NodeT& node ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        if ( m_nodes.find( _NodeKey( node ) ) != m_nodes.end() ) return;
        m_nodes[_NodeKey( node )] = node;
        for ( int i = 0; i < m_iVnodes; ++i ) {
            uint32_t h = _Hash( _NodeKey( node ) + "#" + std::to_string( i ) );
            m_ring[h] = _NodeKey( node );
        }
    }

    void RemoveNode( const NodeT& node ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_nodes.erase( _NodeKey( node ) );
        for ( auto it = m_ring.begin(); it != m_ring.end(); ) {
            if ( it->second == _NodeKey( node ) )
                it = m_ring.erase( it );
            else
                ++it;
        }
        m_unhealthyUntil.erase( _NodeKey( node ) );
    }

    bool Select( const std::string& strKey, NodeT& nodeOut ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        if ( m_ring.empty() ) return false;
        uint32_t h = _Hash( strKey );
        auto it = m_ring.lower_bound( h );
        if ( it == m_ring.end() ) it = m_ring.begin();
        auto start = it;
        do {
            if ( _IsHealthyUnlocked( it->second ) ) {
                nodeOut = m_nodes[it->second];
                return true;
            }
            ++it;
            if ( it == m_ring.end() ) it = m_ring.begin();
        } while ( it != start );
        return false;  // 모든 노드 unhealthy
    }

    void MarkUnhealthy( const NodeT& node, int iSeconds = 30 ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_unhealthyUntil[_NodeKey( node )] = _Now() + iSeconds;
    }

    void MarkHealthy( const NodeT& node ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_unhealthyUntil.erase( _NodeKey( node ) );
    }

    bool IsHealthy( const NodeT& node ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        return _IsHealthyUnlocked( _NodeKey( node ) );
    }

    int NodeCount() const {
        std::lock_guard<std::mutex> lock( m_mutex );
        return static_cast<int>( m_nodes.size() );
    }

    std::vector<NodeT> AllNodes() {
        std::lock_guard<std::mutex> lock( m_mutex );
        std::vector<NodeT> out;
        for ( auto& kv : m_nodes ) out.push_back( kv.second );
        return out;
    }

private:
    static std::string _NodeKey( const NodeT& node );  // NodeT 별 specialize

    static uint32_t _Hash( const std::string& strKey ) {
        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA1( reinterpret_cast<const unsigned char*>( strKey.data() ), strKey.size(), digest );
        return ( static_cast<uint32_t>( digest[0] ) << 24 ) | ( static_cast<uint32_t>( digest[1] ) << 16 ) |
               ( static_cast<uint32_t>( digest[2] ) << 8 ) | static_cast<uint32_t>( digest[3] );
    }

    static int64_t _Now() {
        return std::chrono::duration_cast<std::chrono::seconds>( std::chrono::steady_clock::now().time_since_epoch() )
            .count();
    }

    bool _IsHealthyUnlocked( const std::string& key ) {
        auto it = m_unhealthyUntil.find( key );
        if ( it == m_unhealthyUntil.end() ) return true;
        if ( _Now() >= it->second ) {
            m_unhealthyUntil.erase( it );
            return true;
        }
        return false;
    }

    int m_iVnodes;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, NodeT> m_nodes;             // key → node
    std::map<uint32_t, std::string> m_ring;                     // hash → key
    std::unordered_map<std::string, int64_t> m_unhealthyUntil;  // key → expiry_ts
};

// NodeT = std::string specialization: key 가 곧 노드.
template <>
inline std::string CConsistentHashRing<std::string>::_NodeKey( const std::string& node ) {
    return node;
}

#endif
