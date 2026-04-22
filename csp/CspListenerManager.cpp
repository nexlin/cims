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
    return protocol == "UDP" || protocol == "udp";
}

bool CCspListenerManager::Sync() {
    // v3 (2026-04-22): local_nodes.jsonl 을 소비. 스키마 호환 — 기존 필드 그대로.
    // (id/name/bind_ip/bind_port/protocol/enabled). 새로 추가된 `edge` 필드는
    // 이 매니저에서는 무시 (UDP 수신 관리에만 집중).
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_LOCAL_NODE);
    if (items.type != SimpleJson::JSON_ARRAY) {
        CLog::Print(LOG_ERROR, "ListenerManager: cache items not array");
        return false;
    }

    std::vector<CSipStackUdpListener*> existing;
    gclsUserAgent.m_clsSipStack.GetUdpListenerInfo(existing);
    auto isAlreadyBound = [&](const std::string& ip, int port) {
        for (auto* e : existing) {
            if (!e) continue;
            if (e->m_iPort != port) continue;
            if (ip == "0.0.0.0" || ip.empty()) return true;
            if (e->m_strBindIp == "0.0.0.0" || e->m_strBindIp.empty()) return true;
            if (e->m_strBindIp == ip) return true;
        }
        return false;
    };

    std::vector<ManagedInfo> desired;
    for (size_t i = 0; i < items.Size(); ++i) {
        SimpleJson::JsonNode row = items.At(i);
        if (row.type != SimpleJson::JSON_OBJECT) continue;
        std::string strEnabled = row.GetString("enabled");
        if (strEnabled == "false" || strEnabled == "0") continue;
        std::string proto = row.GetString("protocol", "UDP");
        ManagedInfo m;
        // id 는 UUID 문자열 → 안정적 int 로 매핑
        m.id       = CspUuidToIntId(row.GetString("id"));
        if (!_shouldManage(proto)) {
            CLog::Print(LOG_DEBUG, "ListenerManager: skip non-UDP id=%d proto=%s",
                        m.id, proto.c_str());
            continue;
        }
        m.bindIp   = row.GetString("bind_ip", "0.0.0.0");
        m.port     = (int)row.GetInt("bind_port");
        m.protocol = proto;
        if (m.port <= 0 || m.id == 0) continue;

        if (isAlreadyBound(m.bindIp, m.port)) {
            CLog::Print(LOG_INFO,
                        "ListenerManager: id=%d %s:%d already bound by bootstrap — skip",
                        m.id, m.bindIp.c_str(), m.port);
            continue;
        }
        desired.push_back(m);
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    std::set<int> desiredIds;
    for (const auto& d : desired) desiredIds.insert(d.id);

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
