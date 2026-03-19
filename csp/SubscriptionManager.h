#ifndef _SUBSCRIPTION_MANAGER_H_
#define _SUBSCRIPTION_MANAGER_H_

#include <string>
#include <map>
#include <list>
#include <mutex>
#include <ctime>

/**
 * @ingroup CspServer
 * @brief Subscription Info Structure
 */
struct SubscriptionInfo {
    std::string strUserId;        // User ID extracted from From URI (e.g., "1001")
    std::string strSubscriberUri; // From URI (AoR) - used as NOTIFY To
    std::string strFromTag;       // SUBSCRIBE From-tag  -> NOTIFY To-tag
    std::string strToTag;         // Server-generated tag -> NOTIFY From-tag
    std::string strContact;       // Contact URI from SUBSCRIBE -> NOTIFY Request-URI
    std::string strCallId;        // SIP Dialog Call-ID
    std::string strEventType;     // "gms" or "cms"
    int iExpires;                 // Expires in seconds
    time_t tStartTime;            // Subscription Start Time
    int iNotifySeq;               // CSeq counter for NOTIFY messages
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
     * @brief Add or Update a subscription
     */
    void AddSubscription(const std::string& strResourceUri, const SubscriptionInfo& info);

    /**
     * @brief Remove a subscription (e.g. Expires=0 or Terminated)
     */
    void RemoveSubscription(const std::string& strCallId);

    /**
     * @brief Get subscriptions for a specific user and event type
     * @param strUserId   User ID (e.g. "1001")
     * @param strEventType "gms" or "cms"
     * @param outList     Result list (copies of SubscriptionInfo)
     */
    void GetSubscriptionsByUser(const std::string& strUserId, const std::string& strEventType,
                                std::list<SubscriptionInfo>& outList);

    /**
     * @brief Increment NOTIFY CSeq for a subscription and return the new value
     */
    int IncrementNotifySeq(const std::string& strCallId);

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
