#ifndef __CSP_CONFIG_CACHE_H__
#define __CSP_CONFIG_CACHE_H__

#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <mutex>
#include <functional>
#include "SimpleJson.h"

/** jsonl 레코드의 id (UUID hex 문자열) 를 안정적인 양의 int 로 매핑.
 *  같은 uuid → 항상 같은 int (재부팅 후에도 동일). std::hash 의 deterministic
 *  동작에 의존하므로 같은 컴파일러 환경에서 일관됨.
 *  빈 문자열이면 0 반환.
 */
inline int CspUuidToIntId(const std::string& uuid) {
    if (uuid.empty()) return 0;
    size_t h = std::hash<std::string>{}(uuid);
    return (int)(h & 0x7FFFFFFF);   // 31-bit 양수
}

/**
 * CSP 런타임 설정 캐시 — jsonl 전용 (P10 Phase C 이후)
 *
 *   Agent 가 관리하는 install_path/config/*.jsonl 이 유일한 원천.
 *   SIGUSR1 수신 시 ReloadFromJsonl() 로 메모리 캐시 재구성.
 *
 *   기존의 DB(CSC) HTTP pull 모드 및 로컬 cache/*.json 스냅샷은 Phase C 에서 완전 제거.
 */

enum CspCacheEntity {
    CACHE_LISTENER = 0,
    CACHE_TRUNK,
    CACHE_ROUTE,
    CACHE_ACCESS,
    CACHE_SERVICE,
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
    bool Init(const std::string& jsonlDir);

    /** jsonl 디렉토리가 설정되어 있으면 true. false 면 CSP 는 동적 설정 없이 동작. */
    bool IsJsonlMode() const { return !m_strJsonlDir.empty(); }

    /** 초기 로드 (Init 직후 1회) 및 SIGUSR1 수신 시 재로드. */
    bool LoadInitial() { return ReloadFromJsonl(); }
    bool ReloadFromJsonl();

    /** 현재 메모리 캐시의 items 배열을 JSON 으로 반환 (읽기 전용 복사). */
    SimpleJson::JsonNode GetItems(CspCacheEntity e);

    static const char* EntityName(CspCacheEntity e);

private:
    struct EntityState {
        SimpleJson::JsonNode items;  // JSON array
        time_t               updatedAt = 0;
        std::string          source;  // "jsonl" | "jsonl-empty"
    };

    bool _loadFromJsonl(CspCacheEntity e);

    std::string m_strJsonlDir;
    std::mutex  m_mutex;
    EntityState m_entities[CACHE_COUNT];
};

extern CCspConfigCache gclsCspConfigCache;

#endif // __CSP_CONFIG_CACHE_H__
