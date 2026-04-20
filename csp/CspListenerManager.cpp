#include "CspListenerManager.h"
#include "CspConfigCache.h"
#include "SipServerSetup.h"
#include "Log.h"

#include "SipUserAgent.h"
#include "SipStack.h"

extern CSipUserAgent gclsUserAgent;

CCspListenerManager gclsListenerManager;

bool CCspListenerManager::_shouldManage(const std::string& protocol) const {
    // P2: UDP 만. 다른 프로토콜 (TCP/TLS/WS/WSS) 은 후속 phase.
    return protocol == "UDP";
}

bool CCspListenerManager::Sync() {
    // 캐시에서 listener 항목 배열을 읽음
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_LISTENER);
    if (items.type != SimpleJson::JSON_ARRAY) {
        CLog::Print(LOG_ERROR, "ListenerManager: cache items not array");
        return false;
    }

    // 스택이 이미 bind 한 소켓 목록 (기본 bootstrap 및 이전에 Add 된 것)
    std::vector<CSipStackUdpListener*> existing;
    gclsUserAgent.m_clsSipStack.GetUdpListenerInfo(existing);
    auto isAlreadyBound = [&](const std::string& ip, int port) {
        for (auto* e : existing) {
            if (!e) continue;
            // 포트 충돌은 IP 와 무관하게 OS 레벨에서 발생. "0.0.0.0" 또는 빈 IP 는
            // any-interface bind 이므로 어떤 특정 IP 와도 충돌. 따라서 포트만 일치하면
            // 이미 bind 되어 있다고 판단(충돌 회피 기준).
            if (e->m_iPort != port) continue;
            if (ip == "0.0.0.0" || ip.empty()) return true;
            if (e->m_strBindIp == "0.0.0.0" || e->m_strBindIp.empty()) return true;
            if (e->m_strBindIp == ip) return true;
        }
        return false;
    };

    // 1. 원하는 상태 (desired) 추출
    std::vector<ManagedInfo> desired;
    for (size_t i = 0; i < items.Size(); ++i) {
        SimpleJson::JsonNode row = items.At(i);
        if (row.type != SimpleJson::JSON_OBJECT) continue;
        std::string strEnabled = row.GetString("enabled");
        if (strEnabled == "false" || strEnabled == "0") continue;
        std::string proto = row.GetString("protocol", "UDP");
        if (!_shouldManage(proto)) {
            CLog::Print(LOG_DEBUG, "ListenerManager: skip non-UDP id=%lld proto=%s",
                        row.GetInt("id"), proto.c_str());
            continue;
        }
        ManagedInfo m;
        m.id       = (int)row.GetInt("id");
        m.bindIp   = row.GetString("bind_ip", "0.0.0.0");
        m.port     = (int)row.GetInt("bind_port");
        m.protocol = proto;
        if (m.port <= 0 || m.id == 0) continue;

        // bootstrap 이 이미 bind 한 포트와 일치하면 skip(중복 bind 방지). 관리 대상에서 제외.
        if (isAlreadyBound(m.bindIp, m.port)) {
            CLog::Print(LOG_INFO,
                        "ListenerManager: id=%d %s:%d already bound by bootstrap — skip",
                        m.id, m.bindIp.c_str(), m.port);
            continue;
        }
        desired.push_back(m);
    }

    // 2. 현재 상태 (managed) 와 diff
    std::lock_guard<std::mutex> lk(m_mutex);
    std::set<int> desiredIds;
    for (const auto& d : desired) desiredIds.insert(d.id);

    // 제거: managed 에 있지만 desired 에 없는 것
    std::vector<ManagedInfo> stillManaged;
    for (const auto& m : m_vecManaged) {
        if (desiredIds.find(m.id) != desiredIds.end()) {
            stillManaged.push_back(m);
            continue;
        }
        if (gclsUserAgent.m_clsSipStack.RemoveUdpListener(m.id)) {
            CLog::Print(LOG_SYSTEM, "ListenerManager: removed id=%d %s:%d",
                        m.id, m.bindIp.c_str(), m.port);
        } else {
            CLog::Print(LOG_ERROR, "ListenerManager: remove failed id=%d", m.id);
        }
    }

    // 추가: desired 에 있지만 managed 에 없는 것
    std::set<int> managedIds;
    for (const auto& m : stillManaged) managedIds.insert(m.id);

    for (const auto& d : desired) {
        if (managedIds.find(d.id) != managedIds.end()) continue;
        int iOutId = 0;
        const char* pszIp = d.bindIp.empty() ? NULL : d.bindIp.c_str();
        if (gclsUserAgent.m_clsSipStack.AddUdpListener(d.id, pszIp, d.port,
                                                       gclsSetup.m_iUdpThreadCount, iOutId)) {
            stillManaged.push_back(d);
            CLog::Print(LOG_SYSTEM, "ListenerManager: added id=%d %s:%d",
                        d.id, d.bindIp.c_str(), d.port);
        } else {
            CLog::Print(LOG_ERROR, "ListenerManager: add failed id=%d %s:%d",
                        d.id, d.bindIp.c_str(), d.port);
        }
    }

    m_vecManaged.swap(stillManaged);
    return true;
}

void CCspListenerManager::GetManagedIds(std::vector<int>& out) {
    std::lock_guard<std::mutex> lk(m_mutex);
    out.clear();
    for (const auto& m : m_vecManaged) out.push_back(m.id);
}
