#ifndef __CSP_LOCAL_NODE_MAP_H__
#define __CSP_LOCAL_NODE_MAP_H__

#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * CspLocalNodeMap — local_nodes.jsonl 캐시 (v3, 2026-04-22).
 *
 *   RouteMap / AccessServiceMap 이 name ref 를 resolve 하기 위한 조회 계층.
 *   실제 psip UDP 리스너 bind/unbind 는 CCspListenerManager 가 별도 수행.
 *   (두 컴포넌트가 동일 local_nodes.jsonl 을 서로 다른 용도로 소비).
 */

struct LocalNodeInfo {
    std::string id;  // uuid
    std::string name;
    std::string edge;  // access | peering | mgmt
    std::string bind_ip;
    int bind_port = 0;
    std::string protocol;  // UDP | TCP | TLS | WS | WSS | IPSEC
    /** protocol=IPSEC 전용 — 보호 클라이언트 포트(port_pc). bind_port 는 보호 서버 포트(port_ps).
     *  IPsec 접속점 하나가 psip 리스너 셋(UDP ps · TCP ps · UDP pc)으로 열린다 (sip_access_security.md §8.3). */
    int client_port = 0;
    int thread_count = 0;  // R2: per-listener UDP 수신 스레드 수. 0=fallback → Setup.Sip.UdpThreadCount.
    bool enabled = true;
    bool is_primary = false;  // CSP 인스턴스 identity (Setup.Sip.LocalIp/UdpPort) 의 근원
    std::string tls_cert_path;
    std::string tls_key_path;
    std::string tls_ca_path;
    bool tls_verify_peer = false;
    int max_connections = 0;
    std::vector<std::string> tags;
    std::string note;

    bool IsValid() const {
        return !name.empty();
    }
    bool IsIpsec() const {
        return protocol == "IPSEC";
    }
};

/** IPsec 접속점이 여는 psip 리스너 셋의 역할 */
enum EIpsecListenerRole {
    IPSEC_LISTENER_NONE = 0,
    IPSEC_LISTENER_SERVER_UDP,  // port_ps UDP — 단말 요청 수신 (SA 1/2)
    IPSEC_LISTENER_SERVER_TCP,  // port_ps TCP
    IPSEC_LISTENER_CLIENT_UDP,  // port_pc UDP — 서버 발신 (SA 3/4)
};

/** IPsec 접속점의 리스너 int id — 레코드 hash 의 하위 28 비트에 역할 태그를 얹는다.
 *  (CspUuidToIntId 는 31 비트 양수. 다른 레코드 id 와의 충돌 확률은 기존 hash 와 같은 급.) */
inline int CspIpsecListenerIntId( int recordIntId, EIpsecListenerRole eRole ) {
    return ( recordIntId & 0x0FFFFFFF ) | ( (int)eRole << 28 );
}

class CCspLocalNodeMap {
public:
    CCspLocalNodeMap() = default;

    /** 캐시에서 재로드. SIGUSR1 및 초기 기동 시 호출. */
    bool Sync();

    /** name 으로 조회. 미매칭 시 IsValid()==false 반환. */
    LocalNodeInfo GetByName( const std::string &name ) const;

    /** id (uuid string) 로 조회. */
    LocalNodeInfo GetById( const std::string &id ) const;

    /** psip 용 int listener id (= CspUuidToIntId(uuid), IPsec 접속점은 그 역할 id 도) 로 역조회.
     *  AclPolicyEngine/AccessServiceMap 에서 수신 메시지의 m_iListenerId → LocalNode 매핑. */
    LocalNodeInfo GetByIntId( int listenerIntId ) const;

    /** listener int id 가 IPsec 접속점의 어느 역할인가 (아니면 IPSEC_LISTENER_NONE). */
    EIpsecListenerRole GetIpsecRole( int listenerIntId ) const;

    /** listener int id 의 bind 포트 — IPsec client 역할이면 client_port, 그 외는 bind_port. 없으면 0. */
    int GetListenerPort( int listenerIntId ) const;

    /** IPsec 역할 id → 레코드 int id(CspUuidToIntId). 일반 id 는 그대로. (inbound_policy 대조용) */
    int ToRecordIntId( int listenerIntId ) const;

    /** 이 노드의 IPsec 접속점 (enabled·edge=access·protocol=IPSEC, name 사전식 첫 번째). 없으면 IsValid()==false. */
    LocalNodeInfo GetIpsecNode() const;

    /** 전체 스냅샷. */
    std::vector<LocalNodeInfo> GetAll() const;

    /** CSP 인스턴스의 primary local_node 조회. 선택 규칙:
     *  1) is_primary=true && enabled=true  (여러 개면 name 사전식 첫 번째 + WARN)
     *  2) enabled=true && edge=access && protocol=UDP  (name 사전식 첫 번째)
     *  3) 없음 → IsValid()==false 반환. 호출자가 _infra fallback 사용.
     *  R1 에서 gclsSetup.m_strLocalIp/m_iUdpPort 초기 주입에 사용. */
    LocalNodeInfo GetPrimary() const;

    /** protocol 별 primary 조회 (G9, 2026-04-23). 선택 규칙:
     *  1) is_primary=true && enabled=true && protocol==<인자> (Rule 1)
     *  2) enabled=true && edge=access && protocol==<인자>     (Rule 2)
     *  3) 없음 → IsValid()==false. 호출자가 _infra fallback 사용.
     *  CspServer 의 TCP/TLS primary 주입에 사용. UDP 는 GetPrimary() 와 일관. */
    LocalNodeInfo GetPrimaryByProtocol( const std::string &protocol ) const;

    size_t Size() const;
    bool HasName( const std::string &name ) const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, LocalNodeInfo> m_byName;
};

extern CCspLocalNodeMap gclsLocalNodeMap;

#endif  // __CSP_LOCAL_NODE_MAP_H__
