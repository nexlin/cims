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
    int         id = 0;
    std::string name;
    std::string kind;               // voip | ptt | ibcf | system | console
    std::string domain;
    std::string auth_realm;         // 비어있으면 domain 그대로 사용
    std::string inbound_policy;     // any | restricted
    int         priority = 100;
    bool        enabled = true;
    std::vector<int> listeners;     // restricted 일 때 허용 listener id 목록
};

class CCspServiceMap {
public:
    CCspServiceMap() = default;

    /** 캐시에서 전체 재로드. */
    bool Sync();

    /** ID 로 조회. 없으면 빈 구조체 (id=0). */
    ServiceInfo GetById(int id) const;

    /** domain 으로 조회 (가입자 URI/From host 매칭).
     *  priority 낮은 순으로 첫 매칭 반환. */
    ServiceInfo GetByDomain(const std::string& domain) const;

    /** 전체 서비스 목록 스냅샷. */
    std::vector<ServiceInfo> GetAll() const;

    /** 효과적 realm 계산: service.auth_realm 이 비면 service.domain 을 반환. */
    static std::string EffectiveRealm(const ServiceInfo& svc);

private:
    mutable std::mutex m_mutex;
    std::vector<ServiceInfo> m_services;   // priority 순 정렬
};

extern CCspServiceMap gclsServiceMap;

#endif // __CSP_SERVICE_MAP_H__
