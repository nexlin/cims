#ifndef __CSP_PENDING_ROUTE_MAP_H__
#define __CSP_PENDING_ROUTE_MAP_H__

#include <chrono>
#include <map>
#include <mutex>
#include <string>

/**
 * CspPendingRouteMap — RecvRequest 에서 내려진 routing decision 을
 *   Call-ID 기반으로 잠시 보관했다가 EventIncomingCall 에서 소비하는 맵.
 *
 * 설계 근거 (2026-04-23, G1 통합):
 *   - Routing 결정은 initial INVITE 수신 시 RecvRequest 에서 1회 내린다.
 *   - 결정 결과는 Call-ID 로 key 하여 본 맵에 저장.
 *   - CSipUserAgent 가 dialog 를 만들어 EventIncomingCall 을 호출하면,
 *     EventIncomingCall 이 Take() 로 결정을 꺼내 B2BUA outbound peer 로 사용.
 *   - 꺼낸 즉시 제거 → 외부 peer forward (B2BUA 가 이후 dialog state 로 전파).
 *   - 미소비 항목 (INVITE 이후 EventIncomingCall 까지 도달 못 한 경우) 은
 *     Cleanup(max_age_ms) 로 주기 제거. 기본 TTL 30s 권장.
 *   - 본 맵은 **initial INVITE 한정**. in-dialog 메시지는 B2BUA dialog state 가 담당.
 */
struct PendingRouteEntry {
    std::string remote_ip;
    int remote_port = 0;
    std::string protocol;  // "UDP" / "TCP" / "TLS"
    std::string route_name;
    std::string route_set;
    std::string policy_name;
    std::string local_node_ref;  // Route 의 local_node_ref — B2BUA outbound leg Via/Contact 자기 주소 결정
    std::chrono::steady_clock::time_point created;
};

class CCspPendingRouteMap {
public:
    CCspPendingRouteMap() = default;

    /** Call-ID 로 routing decision 저장. 동일 Call-ID 가 이미 있으면 덮어씀. */
    void Insert( const std::string& callId, const PendingRouteEntry& entry );

    /** Call-ID 로 조회하며 꺼내기 (있으면 outEntry 에 복사하고 맵에서 제거 후 true). */
    bool Take( const std::string& callId, PendingRouteEntry& outEntry );

    /** Call-ID 존재 여부만 확인 (peek-only, 맵 변경 없음). */
    bool Has( const std::string& callId ) const;

    /** 지정 Call-ID 만 제거 (호 거절 등에서 명시적 정리 필요 시). */
    void Erase( const std::string& callId );

    /** 지정 age 보다 오래된 항목 제거. 반환값 = 제거 건수. */
    size_t CleanupExpired( std::chrono::milliseconds maxAge );

    size_t Size() const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, PendingRouteEntry> m_map;
};

extern CCspPendingRouteMap gclsPendingRouteMap;

#endif  // __CSP_PENDING_ROUTE_MAP_H__
