#include "RedisStore.h"

#include <vector>

#include "Log.h"

// Phase 1.D-2 — hiredis 통합. CMakeLists 가 libhiredis-dev 감지 시 CIMS_HAS_HIREDIS=1.
// 미감지 환경: 모든 메서드가 false / no-op (cold-mode) — fail-over 시 register 상태는
// 단말의 재 REGISTER 로만 복원.

#ifdef CIMS_HAS_HIREDIS
#include <hiredis/hiredis.h>
namespace {
    // hiredis context (mutex-protected via outer m_mutex).
    struct RedisCtx {
        redisContext* ctx = nullptr;
        ~RedisCtx() {
            if ( ctx ) redisFree( ctx );
        }
    };
    RedisCtx g_redis;

    bool _Auth( redisContext* c, const std::string& strPassword ) {
        if ( strPassword.empty() ) return true;
        redisReply* reply = (redisReply*)redisCommand( c, "AUTH %s", strPassword.c_str() );
        bool ok = reply && reply->type != REDIS_REPLY_ERROR;
        if ( reply ) freeReplyObject( reply );
        return ok;
    }
}  // namespace
#endif

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

#ifdef CIMS_HAS_HIREDIS
    struct timeval tv = { 2, 0 };  // 2s connect timeout
    g_redis.ctx = redisConnectWithTimeout( strHost.c_str(), iPort, tv );
    if ( !g_redis.ctx || g_redis.ctx->err ) {
        const char* errStr = g_redis.ctx ? g_redis.ctx->errstr : "alloc failed";
        CLog::Print( LOG_ERROR, "RedisStore: connect %s:%d 실패 — %s", strHost.c_str(), iPort, errStr );
        if ( g_redis.ctx ) {
            redisFree( g_redis.ctx );
            g_redis.ctx = nullptr;
        }
        m_bConnected = false;
        return false;
    }
    if ( !_Auth( g_redis.ctx, strPassword ) ) {
        CLog::Print( LOG_ERROR, "RedisStore: AUTH 실패 (%s:%d)", strHost.c_str(), iPort );
        redisFree( g_redis.ctx );
        g_redis.ctx = nullptr;
        m_bConnected = false;
        return false;
    }
    // PING 확인
    redisReply* reply = (redisReply*)redisCommand( g_redis.ctx, "PING" );
    bool ok = reply && reply->type == REDIS_REPLY_STATUS && strcasecmp( reply->str, "PONG" ) == 0;
    if ( reply ) freeReplyObject( reply );
    if ( !ok ) {
        CLog::Print( LOG_ERROR, "RedisStore: PING 실패 (%s:%d)", strHost.c_str(), iPort );
        redisFree( g_redis.ctx );
        g_redis.ctx = nullptr;
        m_bConnected = false;
        return false;
    }
    CLog::Print( LOG_INFO, "RedisStore: connected %s:%d (register HA hot-mode 활성)", strHost.c_str(), iPort );
    m_bConnected = true;
    return true;
#else
    CLog::Print( LOG_INFO,
                 "RedisStore: hiredis 미통합 빌드 — host=%s:%d 설정되었으나 cold-mode 유지 "
                 "(libhiredis-dev 설치 후 재빌드 시 활성)",
                 strHost.c_str(), iPort );
    m_bConnected = false;
    return false;
#endif
}

void CRedisStore::Disconnect() {
    std::lock_guard<std::mutex> lock( m_mutex );
#ifdef CIMS_HAS_HIREDIS
    if ( g_redis.ctx ) {
        redisFree( g_redis.ctx );
        g_redis.ctx = nullptr;
    }
#endif
    m_bConnected = false;
}

bool CRedisStore::SetBinding( const std::string& strAor, const std::string& strJson, int iTtlSec ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    if ( !m_bConnected ) return false;
#ifdef CIMS_HAS_HIREDIS
    std::string strKey = "cims:reg:" + strAor;
    redisReply* reply;
    if ( iTtlSec > 0 ) {
        reply = (redisReply*)redisCommand( g_redis.ctx, "SET %s %s EX %d", strKey.c_str(), strJson.c_str(), iTtlSec );
    } else {
        reply = (redisReply*)redisCommand( g_redis.ctx, "SET %s %s", strKey.c_str(), strJson.c_str() );
    }
    bool ok = reply && reply->type == REDIS_REPLY_STATUS && strcasecmp( reply->str, "OK" ) == 0;
    if ( reply ) freeReplyObject( reply );
    if ( !ok ) CLog::Print( LOG_ERROR, "RedisStore::SetBinding %s 실패", strKey.c_str() );
    return ok;
#else
    (void)strAor;
    (void)strJson;
    (void)iTtlSec;
    return false;
#endif
}

bool CRedisStore::GetBinding( const std::string& strAor, std::string& strJsonOut ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    if ( !m_bConnected ) return false;
#ifdef CIMS_HAS_HIREDIS
    std::string strKey = "cims:reg:" + strAor;
    redisReply* reply = (redisReply*)redisCommand( g_redis.ctx, "GET %s", strKey.c_str() );
    bool ok = false;
    if ( reply && reply->type == REDIS_REPLY_STRING ) {
        strJsonOut.assign( reply->str, reply->len );
        ok = true;
    }
    if ( reply ) freeReplyObject( reply );
    return ok;
#else
    (void)strAor;
    (void)strJsonOut;
    return false;
#endif
}

bool CRedisStore::DelBinding( const std::string& strAor ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    if ( !m_bConnected ) return false;
#ifdef CIMS_HAS_HIREDIS
    std::string strKey = "cims:reg:" + strAor;
    redisReply* reply = (redisReply*)redisCommand( g_redis.ctx, "DEL %s", strKey.c_str() );
    bool ok = reply && reply->type == REDIS_REPLY_INTEGER && reply->integer >= 0;
    if ( reply ) freeReplyObject( reply );
    return ok;
#else
    (void)strAor;
    return false;
#endif
}

int CRedisStore::LoadAllBindings( std::vector<std::pair<std::string, std::string>>& vecOut ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    vecOut.clear();
    if ( !m_bConnected ) return 0;
#ifdef CIMS_HAS_HIREDIS
    // SCAN cursor MATCH cims:reg:* COUNT 100 — 누적
    long long cursor = 0;
    int total = 0;
    do {
        redisReply* reply = (redisReply*)redisCommand( g_redis.ctx, "SCAN %lld MATCH cims:reg:* COUNT 100", cursor );
        if ( !reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2 ) {
            if ( reply ) freeReplyObject( reply );
            break;
        }
        cursor = strtoll( reply->element[0]->str, nullptr, 10 );
        redisReply* keys = reply->element[1];
        for ( size_t i = 0; i < keys->elements; ++i ) {
            std::string strKey = keys->element[i]->str;
            // GET each — pipeline 으로 batch 가 더 빠르지만 v1 은 직렬
            redisReply* g = (redisReply*)redisCommand( g_redis.ctx, "GET %s", strKey.c_str() );
            if ( g && g->type == REDIS_REPLY_STRING ) {
                // strip "cims:reg:" prefix
                std::string strAor = strKey.substr( 9 );
                vecOut.emplace_back( strAor, std::string( g->str, g->len ) );
                ++total;
            }
            if ( g ) freeReplyObject( g );
        }
        freeReplyObject( reply );
    } while ( cursor != 0 );
    CLog::Print( LOG_INFO, "RedisStore::LoadAllBindings: %d entries", total );
    return total;
#else
    (void)vecOut;
    return 0;
#endif
}
