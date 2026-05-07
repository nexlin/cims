#ifndef __CSP_CONFIG_CACHE_H__
#define __CSP_CONFIG_CACHE_H__

#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "SimpleJson.h"

/** jsonl 레코드의 id (UUID hex 문자열) 를 안정적인 양의 int 로 매핑.
 *  같은 uuid → 항상 같은 int (재부팅 후에도 동일). std::hash 의 deterministic
 *  동작에 의존하므로 같은 컴파일러 환경에서 일관됨.
 *  빈 문자열이면 0 반환.
 */
inline int CspUuidToIntId( const std::string& uuid ) {
    if ( uuid.empty() ) return 0;
    size_t h = std::hash<std::string>{}( uuid );
    return (int)( h & 0x7FFFFFFF );  // 31-bit 양수
}

/**
 * CSP 런타임 설정 캐시 — jsonl 전용 (P10 Phase C 이후, v3 9-collection 2026-04-22)
 *
 *   Agent 가 관리하는 install_path/config 의 jsonl 파일들이 유일한 원천.
 *   SIGUSR1 수신 시 ReloadFromJsonl() 로 메모리 캐시 재구성.
 *
 *   기존의 DB(CSC) HTTP pull 모드 및 로컬 cache 스냅샷은 Phase C 에서 완전 제거.
 */

// v3 (2026-04-22) — 9 collection 재설계
// 구 CACHE_LISTENER/TRUNK/ROUTE/ACCESS/SERVICE 는 제거 (hard cutover).
// 파일명 매핑은 CspConfigCache.cpp 의 kJsonlFile[] 참조.
enum CspCacheEntity {
    CACHE_LOCAL_NODE = 0,  // local_nodes.jsonl
    CACHE_REMOTE_NODE,     // remote_nodes.jsonl
    CACHE_ROUTE,           // routes.jsonl  (의미 변경: (LN,RN) pair)
    CACHE_ROUTE_SET,       // route_sets.jsonl
    CACHE_RULE,            // rules.jsonl
    CACHE_RULE_SET,        // rule_sets.jsonl
    CACHE_ROUTING_POLICY,  // routing_policies.jsonl
    CACHE_ACL_POLICY,      // acl_policies.jsonl
    CACHE_ACCESS_SERVICE,  // access_services.jsonl
    CACHE_COUNT
};

class CCspConfigCache {
public:
    CCspConfigCache();
    ~CCspConfigCache();

    /** 기동 시 1회 호출.
     *  @param jsonlDir   agent 가 관리하는 jsonl 디렉토리 절대 경로.
     *                    listeners.jsonl / trunks.jsonl / routes.jsonl / acl.jsonl / services.jsonl 을 참조.
     */
    bool Init( const std::string& jsonlDir );

    /** jsonl 디렉토리가 설정되어 있으면 true. false 면 CSP 는 동적 설정 없이 동작. */
    bool IsJsonlMode() const {
        return !m_strJsonlDir.empty();
    }

    /** 초기 로드 (Init 직후 1회) 및 SIGUSR1 수신 시 재로드. */
    bool LoadInitial() {
        return ReloadFromJsonl();
    }
    bool ReloadFromJsonl();

    /** 현재 메모리 캐시의 items 배열을 JSON 으로 반환 (읽기 전용 복사). */
    SimpleJson::JsonNode GetItems( CspCacheEntity e );

    static const char* EntityName( CspCacheEntity e );

private:
    struct EntityState {
        SimpleJson::JsonNode items;  // JSON array
        time_t updatedAt = 0;
        std::string source;  // "jsonl" | "jsonl-empty"
    };

    bool _loadFromJsonl( CspCacheEntity e );

    std::string m_strJsonlDir;
    std::mutex m_mutex;
    EntityState m_entities[CACHE_COUNT];
};

extern CCspConfigCache gclsCspConfigCache;

#endif  // __CSP_CONFIG_CACHE_H__
