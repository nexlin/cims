#ifndef __CSP_LISTENER_MANAGER_H__
#define __CSP_LISTENER_MANAGER_H__

#include <mutex>
#include <set>
#include <string>
#include <vector>

/**
 * CspListenerManager — CspConfigCache(local_nodes) ↔ psip CSipStack 의 UDP/TCP/TLS 리스너 동기화 계층.
 *
 *   UDP + TCP + TLS 모두 처리. WS/WSS 는 psip 미지원으로 skip.
 *   psip 의 AddUdpListener/AddTcpListener/AddTlsListener API 를 protocol 에 따라 분기 호출.
 *
 *   ID 타입: 캐시 레코드의 id 는 string (UUID hex). psip SipStack 은 int 외부 ID 를 요구하므로
 *   std::hash<string> 으로 안정적인 int 매핑을 사용. 같은 uuid → 같은 int 보장.
 *
 *   TLS 인증서: local_node 의 tls_cert_path/tls_key_path/tls_ca_path 를 AddTlsListener 로 넘겨
 *   리스너별 ctx 를 만든다. 비어 있으면 stack-global cert (Setup.Sip.CertFile) 로 폴백.
 */

class CCspListenerManager {
public:
    CCspListenerManager() = default;

    /** 캐시에서 최신 listener 리스트를 읽어 psip 와 동기화.
     *  초기 기동 및 SIGUSR1 수신 시 호출. */
    bool Sync();

    /** 디버그용: 현재 관리 중인 리스너 ID 목록 (hashed int). */
    void GetManagedIds( std::vector<int> &out );

    /** TLS 인증서 만료 임박 점검 → A-PRC-009 cert_expiring (접속점 단위).
     *
     *  인증서 만료는 검증을 켠 단말 **전부가 동시에** 등록 불가가 되는 단일 장애점이고,
     *  파일은 로드 가능하므로 A-PRC-012(개설 실패)로는 잡히지 않는다. 그래서 남은 기간을
     *  주기적으로 본다. 파일 안의 인증서를 전부 읽어 **가장 이른 만료**를 기준으로 삼는다
     *  (체인 PEM 이면 CA 만료도 함께 걸린다). 임계 이상이면 close — 교체하면 자연 회수된다. */
    void CheckCertExpiry();

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
    /** Sync 에서 관측한 TLS 인증서 (경로, "proto:port") — 만료 점검 대상.
     *  bootstrap 이 이미 열어 ListenerManager 소유가 아닌 접속점도 포함한다(그 인증서도 만료된다). */
    std::vector<std::pair<std::string, std::string>> m_vecTlsCert;

    /** protocol 을 대문자로 정규화. 미지원 프로토콜(WS/WSS 등) 이면 빈 문자열. */
    std::string _normalizeProtocol( const std::string &protocol ) const;
    bool _shouldManage( const std::string &protocol ) const;
    /** protocol-별 "이미 바인딩된 포트" 체크. bootstrap 리스너와의 중복 스킵. */
    bool _isAlreadyBound( const std::string &protocol, const std::string &ip, int port ) const;
    /** protocol 에 맞는 psip AddXxxListener 호출. */
    bool _addListenerToStack( const ManagedInfo &m, int &outId );
    /** protocol 에 맞는 psip RemoveXxxListener 호출. */
    bool _removeListenerFromStack( const ManagedInfo &m );
    /** bootstrap 이 연 TLS 접속점의 인증서가 바뀌었으면 **무중단 교체**한다(재기동 불필요).
     *  ListenerManager 소유가 아니어서 remove+add 가 불가한 접속점을 위한 경로. */
    void _reloadBootstrapTlsCertIfChanged( const ManagedInfo &d );
};

extern CCspListenerManager gclsListenerManager;

#endif  // __CSP_LISTENER_MANAGER_H__
