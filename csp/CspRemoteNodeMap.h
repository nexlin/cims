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
    /** 가입자→이 피어 오퍼에 CSP 가 끼워 넣는 코덱(예 ["PCMA","PCMU"] — IP-PBX G.711). 피어가 그 코덱으로 답하면 CMP 가
     * 가입자 leg 와 변환한다(cmp.md §11.2, TS 29.162 코덱 삽입). 비면 삽입 없음(종전 동작). */
    std::vector<std::string> transcode_codecs;
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

    /** TLS 피어의 발신 연결 정책을 psip 에 옮긴다 — `tls_verify` 인 노드는 서버 인증서를 검증(앵커 = TLS primary
     *  local_node 의 tls_ca_path, 비면 시스템 저장소). 클라이언트 인증서는 자기 노드 인증서(CspServer 전역).
     *  Sync 끝에 호출되며 Setup 이 확정된 뒤 다시 불러도 된다(전부 비우고 재등록). */
    void ApplyTlsPolicies() const;

    RemoteNodeInfo GetByName( const std::string &name ) const;
    RemoteNodeInfo GetById( const std::string &id ) const;
    std::vector<RemoteNodeInfo> GetAll() const;
    size_t Size() const;
    bool HasName( const std::string &name ) const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, RemoteNodeInfo> m_byName;
};

extern CCspRemoteNodeMap gclsRemoteNodeMap;

#endif  // __CSP_REMOTE_NODE_MAP_H__
