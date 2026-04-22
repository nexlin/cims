#ifndef __CSP_LOCAL_NODE_MAP_H__
#define __CSP_LOCAL_NODE_MAP_H__

#include <string>
#include <vector>
#include <map>
#include <mutex>

/**
 * CspLocalNodeMap — local_nodes.jsonl 캐시 (v3, 2026-04-22).
 *
 *   RouteMap / AccessServiceMap 이 name ref 를 resolve 하기 위한 조회 계층.
 *   실제 psip UDP 리스너 bind/unbind 는 CCspListenerManager 가 별도 수행.
 *   (두 컴포넌트가 동일 local_nodes.jsonl 을 서로 다른 용도로 소비).
 */

struct LocalNodeInfo {
    std::string id;                 // uuid
    std::string name;
    std::string edge;               // access | peering | mgmt
    std::string bind_ip;
    int         bind_port = 0;
    std::string protocol;           // UDP | TCP | TLS | WS | WSS
    bool        enabled = true;
    std::string tls_cert_path;
    std::string tls_key_path;
    std::string tls_ca_path;
    bool        tls_verify_peer = false;
    int         max_connections = 0;
    std::vector<std::string> tags;
    std::string note;

    bool IsValid() const { return !name.empty(); }
};

class CCspLocalNodeMap {
public:
    CCspLocalNodeMap() = default;

    /** 캐시에서 재로드. SIGUSR1 및 초기 기동 시 호출. */
    bool Sync();

    /** name 으로 조회. 미매칭 시 IsValid()==false 반환. */
    LocalNodeInfo GetByName(const std::string& name) const;

    /** id (uuid string) 로 조회. */
    LocalNodeInfo GetById(const std::string& id) const;

    /** psip 용 int listener id (= CspUuidToIntId(uuid)) 로 역조회.
     *  AclPolicyEngine/AccessServiceMap 에서 수신 메시지의 m_iListenerId → LocalNode 매핑. */
    LocalNodeInfo GetByIntId(int listenerIntId) const;

    /** 전체 스냅샷. */
    std::vector<LocalNodeInfo> GetAll() const;

    size_t Size() const;
    bool   HasName(const std::string& name) const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, LocalNodeInfo> m_byName;
};

extern CCspLocalNodeMap gclsLocalNodeMap;

#endif // __CSP_LOCAL_NODE_MAP_H__
