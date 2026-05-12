#include "RedisStore.h"

#include <vector>

#include "Log.h"

// Phase 1.D-1 stub — cold-mode 구현. 모든 메서드가 false / no-op 반환.
// 실제 hiredis 통합은 후속 라운드 — 본 파일의 함수 본체만 교체하면 활성화.

CRedisStore& CRedisStore::GetInstance() {
    static CRedisStore instance;
    return instance;
}

bool CRedisStore::Init( const std::string& strHost, int iPort, const std::string& strPassword ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    m_strHost = strHost;
    m_iPort = iPort;
    m_strPassword = strPassword;

    if ( strHost.empty() || iPort <= 0 ) {
        CLog::Print( LOG_INFO, "RedisStore: 비활성 (host/port 미설정) — register HA cold-mode" );
        m_bConnected = false;
        return false;
    }

    // STUB: hiredis 연결 미통합. 후속 라운드에서 redisConnectWithTimeout 등 호출.
    CLog::Print( LOG_ERROR, "RedisStore: stub mode — host=%s:%d 설정되었으나 hiredis 미통합 → cold-mode 유지",
                 strHost.c_str(), iPort );
    m_bConnected = false;
    return false;
}

void CRedisStore::Disconnect() {
    std::lock_guard<std::mutex> lock( m_mutex );
    m_bConnected = false;
}

bool CRedisStore::SetBinding( const std::string&, const std::string&, int ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    if ( !m_bConnected ) return false;
    // STUB
    return false;
}

bool CRedisStore::GetBinding( const std::string&, std::string& ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    if ( !m_bConnected ) return false;
    // STUB
    return false;
}

bool CRedisStore::DelBinding( const std::string& ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    if ( !m_bConnected ) return false;
    // STUB
    return false;
}

int CRedisStore::LoadAllBindings( std::vector<std::pair<std::string, std::string>>& vecOut ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    vecOut.clear();
    if ( !m_bConnected ) return 0;
    // STUB
    return 0;
}
