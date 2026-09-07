#ifndef _SUBSCRIPTION_MANAGER_H_
#define _SUBSCRIPTION_MANAGER_H_

#include <ctime>
#include <list>
#include <map>
#include <mutex>
#include <string>

/**
 * @ingroup CspServer
 * @brief Subscription Info Structure
 */
struct SubscriptionInfo {
    std::string strUserId;         // User ID extracted from From URI (e.g., "1001")
    std::string strSubscriberUri;  // From URI (AoR) - used as NOTIFY To
    std::string strFromTag;        // SUBSCRIBE From-tag  -> NOTIFY To-tag
    std::string strToTag;          // Server-generated tag -> NOTIFY From-tag
    std::string strContact;        // Contact URI from SUBSCRIBE -> NOTIFY Request-URI
    std::string strCallId;         // SIP Dialog Call-ID
    std::string strEventType;      // "reg"|"affiliation"|"conference"|"gms"|"cms"|"dialog"
    std::string strResourceId;     // 구독 대상 자원 (Req-URI user) — conference 는 그룹 ID
    int iExpires;                  // Expires in seconds
    time_t tStartTime;             // Subscription Start Time
    int iNotifySeq;                // CSeq counter for NOTIFY messages
    int iInboundListenerId = 0;    // SUBSCRIBE 수신 listener (NOTIFY Via/Contact 자기 주소 결정)
};

/**
 * @ingroup CspServer
 * @brief Manages SIP Subscriptions (SUBSCRIBE/NOTIFY)
 */
class CSubscriptionManager {
public:
    CSubscriptionManager();
    ~CSubscriptionManager();

    /**
     * @brief 재기동을 넘어 단조 증가하는 NOTIFY CSeq 시드 (RFC 3261 §12.2.2).
     *   상태 없는 in-dialog 갱신(재기동 후 옛 dialog 의 SUBSCRIBE)을 수용할 때 쓴다 — 구독자 dialog 는 이전
     *   인스턴스가 보낸 NOTIFY CSeq 를 기억하므로 1부터 다시 세면 후속 NOTIFY 가 전부 500(Invalid CSeq, 하위
     *   CSeq)으로 거절돼 로스터·xcap-diff 통지가 영구 stale 된다. 2026-01-01 UTC 경과 초 ×8 — 구독 하나에
     *   초당 8건 미만이면 어떤 이전 CSeq 보다 크고, int 범위에서 약 8.5년 유효.
     */
    static int RebootSafeNotifySeq();

    /**
     * @brief Add or Update a subscription
     */
    void AddSubscription( const std::string &strResourceUri, const SubscriptionInfo &info );

    /**
     * @brief Remove a subscription (e.g. Expires=0 or Terminated)
     */
    void RemoveSubscription( const std::string &strCallId );

    /**
     * @brief Get subscriptions for a specific user and event type
     * @param strUserId   User ID (e.g. "1001")
     * @param strEventType "gms" or "cms"
     * @param outList     Result list (copies of SubscriptionInfo)
     */
    void GetSubscriptionsByUser( const std::string &strUserId, const std::string &strEventType,
                                 std::list<SubscriptionInfo> &outList );

    /**
     * @brief 자원(그룹 등) 기준 구독 조회 — conference 이벤트의 그룹별 구독자 목록에 사용
     * @param strResourceId 자원 ID (conference = 그룹 ID)
     * @param strEventType  이벤트 종별 ("conference" 등)
     * @param outList       결과
     */
    void GetSubscriptionsByResource( const std::string &strResourceId, const std::string &strEventType,
                                     std::list<SubscriptionInfo> &outList );

    /**
     * @brief 이벤트 종별 전체 구독 조회 — 시스템 전역 문서 변경(service-config 등)의
     *   전원 통지에 사용. 자원/사용자 키가 없는 유일한 조회라 전체 순회다.
     * @param strEventType 이벤트 종별 ("cms" 등)
     * @param outList      결과
     */
    void GetSubscriptionsByEvent( const std::string &strEventType, std::list<SubscriptionInfo> &outList );

    /**
     * @brief Get a single subscription by Call-ID (returns false if not found)
     */
    bool GetSubscriptionByCallId( const std::string &strCallId, SubscriptionInfo &outInfo );

    /**
     * @brief Increment NOTIFY CSeq for a subscription and return the new value
     */
    int IncrementNotifySeq( const std::string &strCallId );

    /**
     * @brief Check and remove expired subscriptions
     */
    void CheckExpired();

private:
    // Map: CallId -> SubscriptionInfo (one entry per SIP dialog)
    std::map<std::string, SubscriptionInfo> m_mapSubs;
    std::recursive_mutex m_mutex;
};

extern CSubscriptionManager gclsSubscriptionManager;

#endif
