#ifndef __CSP_LISTENER_MANAGER_H__
#define __CSP_LISTENER_MANAGER_H__

#include <string>
#include <vector>
#include <set>
#include <mutex>

/**
 * CspListenerManager — CspConfigCache(listener) ↔ psip CSipStack 의 UDP 리스너 동기화 계층.
 *
 *   P2 범위: UDP 만. TCP/TLS 리스너 항목은 무시(debug log).
 *
 *   ID 타입: 캐시 레코드의 id 는 string (UUID hex). psip SipStack 은 int 외부 ID 를 요구하므로
 *   std::hash<string> 으로 안정적인 int 매핑을 사용. 같은 uuid → 같은 int 보장.
 */

class CCspListenerManager {
public:
    CCspListenerManager() = default;

    /** 캐시에서 최신 listener 리스트를 읽어 psip 와 동기화.
     *  초기 기동 및 SIGUSR1 수신 시 호출. */
    bool Sync();

    /** 디버그용: 현재 관리 중인 리스너 ID 목록 (hashed int). */
    void GetManagedIds(std::vector<int>& out);

private:
    struct ManagedInfo {
        int         id;           // record.id 의 UUID 를 hash 한 안정적 int
        std::string bindIp;
        int         port;
        std::string protocol;
        int         threadCount;  // R2: per-listener UDP 수신 스레드 수 (실효 적용값)
    };

    std::mutex m_mutex;
    std::vector<ManagedInfo> m_vecManaged;

    bool _shouldManage(const std::string& protocol) const;
};

extern CCspListenerManager gclsListenerManager;

#endif // __CSP_LISTENER_MANAGER_H__
