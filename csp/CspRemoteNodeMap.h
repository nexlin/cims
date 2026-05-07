#ifndef __CSP_REMOTE_NODE_MAP_H__
#define __CSP_REMOTE_NODE_MAP_H__

#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * CspRemoteNodeMap — remote_nodes.jsonl 캐시 (v3, 2026-04-22).
 *
 *   외부 피어(IMS/PBX) 의 transport 정보 보관. auth 정보는 Route 가 관리.
 *   Route 가 remote_node_ref (name) 로 해당 RemoteNode 를 resolve 한다.
 */

struct RemoteNodeInfo {
    std::string id;  // uuid
    std::string name;
    std::string ip;  // IP 또는 hostname
    int port = 0;
    std::string protocol;       // UDP | TCP | TLS
    std::string remote_domain;  // 피어의 SIP URI host
    bool srv_lookup = false;
    bool dns_fallback = true;
    bool tls_verify = false;
    bool enabled = true;
    std::vector<std::string> tags;
    std::string note;

    bool IsValid() const {
        return !name.empty();
    }
};

class CCspRemoteNodeMap {
public:
    CCspRemoteNodeMap() = default;

    bool Sync();

    RemoteNodeInfo GetByName( const std::string& name ) const;
    RemoteNodeInfo GetById( const std::string& id ) const;
    std::vector<RemoteNodeInfo> GetAll() const;
    size_t Size() const;
    bool HasName( const std::string& name ) const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, RemoteNodeInfo> m_byName;
};

extern CCspRemoteNodeMap gclsRemoteNodeMap;

#endif  // __CSP_REMOTE_NODE_MAP_H__
