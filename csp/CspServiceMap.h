#ifndef __CSP_SERVICE_MAP_H__
#define __CSP_SERVICE_MAP_H__

#include <string>
#include <vector>
#include <map>
#include <mutex>

/**
 * CspServiceMap — sip_service 설정 캐시를 메모리에 유지.
 *
 *   도메인/ID 로 서비스를 빠르게 조회. CscfModule 이 Digest challenge realm
 *   과 full-IMPI 조립용 domain 을 이 맵에서 가져온다. RouteEngine 과
 *   TrunkManager 도 "서비스 → 트렁크 목록" 조회에 사용.
 */

struct ServiceInfo {
    int         id = 0;                    // UUID → hash int (v3)
    std::string uuid;                      // 원본 UUID 문자열 (v3)
    std::string name;
    std::string kind;                      // voip | ptt  (v3: ibcf/system/console 제거)
    std::string domain;
    std::string auth_realm;                // 비어있으면 domain 상속
    std::string inbound_policy;            // any | restricted
    int         priority = 100;
    bool        enabled = true;
    std::vector<std::string> allowed_local_node_refs;  // v3: LocalNode name 참조 (SOT)
    std::vector<int> listeners;            // 파생: LocalNode name → hash int. 레거시 호환.
};

class CCspServiceMap {
public:
    CCspServiceMap() = default;

    /** 캐시에서 전체 재로드. */
    bool Sync();

    /** ID 로 조회. 없으면 빈 구조체 (id=0). */
    ServiceInfo GetById(int id) const;

    /** name (access_services.name) 으로 조회. v3 신규 — subscriptions.service_ref 해석용. */
    ServiceInfo GetByName(const std::string& name) const;

    /** domain 으로 조회 (가입자 URI/From host 매칭).
     *  priority 낮은 순으로 첫 매칭 반환. */
    ServiceInfo GetByDomain(const std::string& domain) const;

    /** 전체 서비스 목록 스냅샷. */
    std::vector<ServiceInfo> GetAll() const;

    /** kind (voip|ptt) 로 조회. priority 낮은 첫 enabled 서비스 반환. v3 신규. */
    ServiceInfo GetByKind(const std::string& kind) const;

    /** kind 의 대표 domain 반환 (기존 gclsSetup.GetDomainForService 대체). v3 신규. */
    std::string GetDomainByKind(const std::string& kind) const;

    /** domain→kind 매핑 구축 (SipLogger 등에서 로깅용으로 사용).
     *  kind 값: voip | ptt. 하나의 domain 이 중복된 kind 에 걸치면 우선순위 낮은 서비스 승리. */
    std::map<std::string, std::string> BuildDomainToKindMap() const;

    /** inbound_policy=restricted 검사.
     *  @param svc             체크할 서비스
     *  @param listenerIntId   psip 수신 리스너 int id (CSipMessage.m_iListenerId)
     *  @return true  = 허용 (inbound_policy=any 이거나 listenerIntId 가 svc.listeners[] 에 포함)
     *          false = 거절 (restricted 인데 listener 미일치) */
    static bool IsInboundAllowed(const ServiceInfo& svc, int listenerIntId);

    /** 효과적 realm 계산: service.auth_realm 이 비면 service.domain 을 반환. */
    static std::string EffectiveRealm(const ServiceInfo& svc);

private:
    mutable std::mutex m_mutex;
    std::vector<ServiceInfo> m_services;   // priority 순 정렬
};

extern CCspServiceMap gclsServiceMap;

/** v3 호환 래퍼 — CspAccessServiceMap 로 이름 전환 과정의 shim.
 *  S7 에서 gclsServiceMap → gclsAccessServiceMap 개명 후 이 래퍼 제거. */
inline bool gclsAccessServiceMap_Sync_compat() { return gclsServiceMap.Sync(); }

#endif // __CSP_SERVICE_MAP_H__
