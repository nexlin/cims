#ifndef __CSP_CONFIG_CACHE_H__
#define __CSP_CONFIG_CACHE_H__

#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <mutex>
#include "SimpleJson.h"

/**
 * CSP 런타임 설정 캐시
 *
 *   DB(CSC) → HTTP pull → 메모리 → csp/cache/*.json
 *
 * 기동 순서:
 *   1. 로컬 스냅샷 파일 우선 로드 → 즉시 서비스 가능 상태로 진입
 *   2. 비동기로 CSC 에 HTTP GET 시도 → 성공 시 원자적으로 교체
 *   3. 실패 시 로컬 캐시 유지 + 주기 재시도
 *
 * 변경 수신:
 *   CCscInterface 가 LISTENER_CHANGED 등 이벤트 수신 → 해당 entity 만 HTTP pull → 교체
 *
 * 캐시가 전혀 없는 최초 부팅:
 *   csp.json 의 Bootstrap 블록(최소 1 리스너/realm) 으로 임시 동작
 */

enum CspCacheEntity {
    CACHE_LISTENER = 0,
    CACHE_TRUNK,
    CACHE_ROUTE,
    CACHE_ACCESS,
    CACHE_COUNT
};

class CCspConfigCache {
public:
    CCspConfigCache();
    ~CCspConfigCache();

    /** 기동 시 1회 호출.
     *  @param cacheDir   로컬 스냅샷 디렉토리 경로 (예: "csp/cache")
     *  @param cscHost    CSC 내부 API 호스트 (보통 127.0.0.1)
     *  @param cscPort    CSC 내부 API 포트 (기본 4422)
     *  @param token      shared secret (X-Csp-Internal-Token)
     */
    bool Init(const std::string& cacheDir,
              const std::string& cscHost,
              int cscPort,
              const std::string& token);

    /** 로컬 캐시 로드 → CSC 새로고침 시도 → 성공/실패 여부 반환. 비동기 백그라운드 재시도는 별도. */
    bool LoadInitial();

    /** 개별 entity 를 CSC 에서 pull 하고 캐시/파일 갱신. CCscInterface 이벤트에서 호출. */
    bool RefreshEntity(CspCacheEntity e);

    /** 전체 entity 를 CSC 에서 pull. 예: CSC_RESTART 수신 시. */
    bool RefreshAll();

    /** 현재 메모리 캐시의 items 배열을 JSON 으로 반환 (읽기 전용 복사). */
    SimpleJson::JsonNode GetItems(CspCacheEntity e);

    /** entity 에 대한 현재 etag. */
    std::string GetEtag(CspCacheEntity e);

    /** CSC 연결 성공 여부 최근 상태. false 면 로컬 캐시로만 운영 중. */
    bool IsCscReachable() const { return m_bCscReachable; }

    static const char* EntityName(CspCacheEntity e);
    static const char* EntityFileName(CspCacheEntity e);

private:
    struct EntityState {
        SimpleJson::JsonNode items;  // JSON array
        std::string          etag;
        time_t               updatedAt = 0;
        std::string          source;  // "db" | "file" | "empty"
    };

    bool _loadFromFile(CspCacheEntity e);
    bool _saveToFile(CspCacheEntity e);
    bool _atomicWriteJson(const std::string& path, const std::string& content);
    bool _httpGet(const std::string& path,
                  const std::string& ifNoneMatch,
                  int& outStatus,
                  std::string& outBody,
                  std::string& outEtag);
    bool _applyPullResponse(CspCacheEntity e,
                            const std::string& body,
                            const std::string& etag);

    std::string m_strCacheDir;
    std::string m_strCscHost;
    int         m_iCscPort = 4422;
    std::string m_strToken;

    std::mutex  m_mutex;
    EntityState m_entities[CACHE_COUNT];
    bool        m_bCscReachable = false;
};

extern CCspConfigCache gclsCspConfigCache;

#endif // __CSP_CONFIG_CACHE_H__
