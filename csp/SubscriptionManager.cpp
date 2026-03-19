#include "SubscriptionManager.h"
#include "Log.h"

CSubscriptionManager gclsSubscriptionManager;

CSubscriptionManager::CSubscriptionManager() {
}

CSubscriptionManager::~CSubscriptionManager() {
}

void CSubscriptionManager::AddSubscription(const std::string& strResourceUri, const SubscriptionInfo& info) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    auto it = m_mapSubs.find(info.strCallId);
    if (it != m_mapSubs.end()) {
        // Update existing dialog (re-SUBSCRIBE or refresh)
        int iOldSeq = it->second.iNotifySeq;
        it->second = info;
        it->second.iNotifySeq = iOldSeq; // Preserve NOTIFY seq
        CLog::Print(LOG_DEBUG, "Subscription Updated: User=%s Type=%s CallId=%s Expires=%d",
            info.strUserId.c_str(), info.strEventType.c_str(), info.strCallId.c_str(), info.iExpires);
    } else {
        m_mapSubs[info.strCallId] = info;
        CLog::Print(LOG_INFO, "Subscription Added: User=%s Type=%s CallId=%s Expires=%d",
            info.strUserId.c_str(), info.strEventType.c_str(), info.strCallId.c_str(), info.iExpires);
    }
}

void CSubscriptionManager::RemoveSubscription(const std::string& strCallId) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    auto it = m_mapSubs.find(strCallId);
    if (it != m_mapSubs.end()) {
        CLog::Print(LOG_INFO, "Subscription Removed: User=%s Type=%s CallId=%s",
            it->second.strUserId.c_str(), it->second.strEventType.c_str(), strCallId.c_str());
        m_mapSubs.erase(it);
    }
}

void CSubscriptionManager::GetSubscriptionsByUser(const std::string& strUserId,
                                                  const std::string& strEventType,
                                                  std::list<SubscriptionInfo>& outList) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    for (auto& pair : m_mapSubs) {
        if (pair.second.strUserId == strUserId && pair.second.strEventType == strEventType) {
            outList.push_back(pair.second);
        }
    }
}

int CSubscriptionManager::IncrementNotifySeq(const std::string& strCallId) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    auto it = m_mapSubs.find(strCallId);
    if (it != m_mapSubs.end()) {
        return ++(it->second.iNotifySeq);
    }
    return 1;
}

void CSubscriptionManager::CheckExpired() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    time_t now = time(NULL);

    for (auto it = m_mapSubs.begin(); it != m_mapSubs.end(); ) {
        time_t tExpireAt = it->second.tStartTime + it->second.iExpires;
        if (now > tExpireAt) {
            CLog::Print(LOG_INFO, "Subscription Expired: User=%s Type=%s CallId=%s",
                it->second.strUserId.c_str(), it->second.strEventType.c_str(), it->first.c_str());
            it = m_mapSubs.erase(it);
        } else {
            ++it;
        }
    }
}
