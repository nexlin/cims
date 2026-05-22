#ifndef __REDIS_STORE_H__
#define __REDIS_STORE_H__

#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Phase 1.D-1 — HA register state replication.
// 본 헤더는 골격 (cold-mode stub). 실제 hiredis 통합은 후속 라운드 (1.D-2)
// 에서 추가. 현재 m_bConnected 는 항상 false → 모든 Set/Get/Del 이 no-op
// 으로 동작 → fail-over 시 register 상태는 단말의 재 REGISTER 로만 복원.
//
// 활성화 흐름 (후속):
//   1. CMakeLists.txt 에 hiredis (또는 cpp_redis) 의존성 추가
//   2. RedisStore.cpp 의 Init/Set/Get/Del 본체에 실제 호출 구현
//   3. csp.json 의 Redis.Host / Redis.Port / Redis.Password 가 설정되면 자동
//      활성, 미설정이면 cold-mode 유지

class CRedisStore {
public:
    static CRedisStore &GetInstance();

    bool Init( const std::string &strHost, int iPort, const std::string &strPassword = "" );
    bool IsConnected() const {
        return m_bConnected;
    }
    void Disconnect();

    // register binding (key: "cims:reg:<aor>", value: JSON)
    bool SetBinding( const std::string &strAor, const std::string &strJson, int iTtlSec );
    bool GetBinding( const std::string &strAor, std::string &strJsonOut );
    bool DelBinding( const std::string &strAor );

    // 일괄 복원 (시작 시 Standby 가 호출) — cold-mode 면 0 반환
    int LoadAllBindings( std::vector<std::pair<std::string, std::string>> &vecOut );

private:
    CRedisStore() : m_bConnected( false ), m_iPort( 0 ) {
    }
    ~CRedisStore() {
        Disconnect();
    }

    mutable std::mutex m_mutex;
    bool m_bConnected;
    std::string m_strHost;
    int m_iPort;
    std::string m_strPassword;
};

#define gclsRedisStore CRedisStore::GetInstance()

#endif
