#include "CspPendingRouteMap.h"

CCspPendingRouteMap gclsPendingRouteMap;

void CCspPendingRouteMap::Insert( const std::string& callId, const PendingRouteEntry& entry ) {
    if ( callId.empty() ) return;
    PendingRouteEntry e = entry;
    e.created = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk( m_mutex );
    m_map[callId] = e;
}

bool CCspPendingRouteMap::Take( const std::string& callId, PendingRouteEntry& outEntry ) {
    if ( callId.empty() ) return false;
    std::lock_guard<std::mutex> lk( m_mutex );
    auto it = m_map.find( callId );
    if ( it == m_map.end() ) return false;
    outEntry = it->second;
    m_map.erase( it );
    return true;
}

void CCspPendingRouteMap::Erase( const std::string& callId ) {
    if ( callId.empty() ) return;
    std::lock_guard<std::mutex> lk( m_mutex );
    m_map.erase( callId );
}

size_t CCspPendingRouteMap::CleanupExpired( std::chrono::milliseconds maxAge ) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk( m_mutex );
    size_t removed = 0;
    for ( auto it = m_map.begin(); it != m_map.end(); ) {
        if ( now - it->second.created > maxAge ) {
            it = m_map.erase( it );
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t CCspPendingRouteMap::Size() const {
    std::lock_guard<std::mutex> lk( m_mutex );
    return m_map.size();
}
