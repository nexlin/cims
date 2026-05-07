#ifndef __CSP_LISTENER_MANAGER_H__
#define __CSP_LISTENER_MANAGER_H__

#include <mutex>
#include <set>
#include <string>
#include <vector>

/**
 * CspListenerManager — CspConfigCache(local_nodes) ↔ psip CSipStack 의 UDP/TCP/TLS 리스너 동기화 계층.
 *
 *   R4 범위: UDP + TCP + TLS 모두 처리. WS/WSS 는 psip 미지원으로 skip.
 *   psip 의 AddUdpListener/AddTcpListener/AddTlsListener API 를 protocol 에 따라 분기 호출.
 *
 *   ID 타입: 캐시 레코드의 id 는 string (UUID hex). psip SipStack 은 int 외부 ID 를 요구하므로
 *   std::hash<string> 으로 안정적인 int 매핑을 사용. 같은 uuid → 같은 int 보장.
 *
 *   TLS 인증서: 현재 psip 은 stack-global SSLServerStart(cert, ca) 로 1회 초기화.
 *   local_node.tls_cert_path 는 수집하되 per-listener 적용은 R5+ 에서.
 */

class CCspListenerManager {
public:
    CCspListenerManager() = default;

    /** 캐시에서 최신 listener 리스트를 읽어 psip 와 동기화.
     *  초기 기동 및 SIGUSR1 수신 시 호출. */
    bool Sync();

    /** 디버그용: 현재 관리 중인 리스너 ID 목록 (hashed int). */
    void GetManagedIds( std::vector<int>& out );

private:
    struct ManagedInfo {
        int id;  // record.id 의 UUID 를 hash 한 안정적 int
        std::string bindIp;
        int port;
        std::string protocol;     // "UDP" | "TCP" | "TLS"
        int threadCount;          // R2: UDP 수신 스레드 수. TCP/TLS 는 무시.
        std::string tlsCertPath;  // R5.c: TLS 전용. 비어있으면 stack-global cert 사용.
        std::string tlsKeyPath;
        std::string tlsCaPath;
    };

    std::mutex m_mutex;
    std::vector<ManagedInfo> m_vecManaged;

    /** protocol 을 대문자로 정규화. 미지원 프로토콜(WS/WSS 등) 이면 빈 문자열. */
    std::string _normalizeProtocol( const std::string& protocol ) const;
    bool _shouldManage( const std::string& protocol ) const;
    /** protocol-별 "이미 바인딩된 포트" 체크. bootstrap 리스너와의 중복 스킵. */
    bool _isAlreadyBound( const std::string& protocol, const std::string& ip, int port ) const;
    /** protocol 에 맞는 psip AddXxxListener 호출. */
    bool _addListenerToStack( const ManagedInfo& m, int& outId );
    /** protocol 에 맞는 psip RemoveXxxListener 호출. */
    bool _removeListenerFromStack( const ManagedInfo& m );
};

extern CCspListenerManager gclsListenerManager;

#endif  // __CSP_LISTENER_MANAGER_H__
