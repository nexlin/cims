#ifndef __CSP_LISTENER_MANAGER_H__
#define __CSP_LISTENER_MANAGER_H__

#include <string>
#include <vector>
#include <set>
#include <mutex>

/**
 * CspListenerManager — CspConfigCache(listener) ↔ psip CSipStack 의 UDP 리스너 동기화 계층.
 *
 *   CspConfigCache 의 listener entity(DB → 로컬 JSON) 와 psip stack 이 실제 bind 하고 있는
 *   리스너 집합의 diff 를 계산해 add/remove 를 호출한다.
 *
 *   초기 기동: Sync() — 기존 stack 기본 리스너(Start 에서 생성된 id=0 bootstrap)는 그대로 두고
 *     DB 로부터 받은 추가 리스너들을 AddUdpListener 로 등록한다. DB 리스너와 bootstrap 이
 *     IP/Port 가 겹치면 bootstrap 을 바꾸는 대신 유지 (bootstrap 은 항상 first-match 의 fallback).
 *
 *   런타임 변경: CCscInterface 가 LISTENER_CHANGED 수신 → Sync() 재호출 → 이전 상태와 diff 로
 *     add/remove 수행.
 *
 *   P2 범위: UDP 만. TCP/TLS 리스너 항목은 무시(warn log 1회).
 */

class CCspListenerManager {
public:
    CCspListenerManager() = default;

    /** 캐시에서 최신 listener 리스트를 읽어 psip 와 동기화.
     *  초기 기동 및 LISTENER_CHANGED 이벤트 수신 시 호출.
     *  반환: true (성공) / false (캐시 미준비 또는 stack 미기동 등) */
    bool Sync();

    /** 디버그용: 현재 관리 중인 리스너 목록 스냅샷. */
    void GetManagedIds(std::vector<int>& out);

private:
    struct ManagedInfo {
        int         id;
        std::string bindIp;
        int         port;
        std::string protocol;
    };

    std::mutex m_mutex;
    std::vector<ManagedInfo> m_vecManaged;

    bool _shouldManage(const std::string& protocol) const;
};

extern CCspListenerManager gclsListenerManager;

#endif // __CSP_LISTENER_MANAGER_H__
