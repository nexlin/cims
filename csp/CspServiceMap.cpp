#include "CspServiceMap.h"
#include "CspConfigCache.h"
#include "SipServerSetup.h"
#include "Log.h"
#include <algorithm>

CCspServiceMap gclsServiceMap;

bool CCspServiceMap::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_SERVICE);
    std::vector<ServiceInfo> newList;
    if (items.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < items.Size(); ++i) {
            SimpleJson::JsonNode row = items.At(i);
            if (row.type != SimpleJson::JSON_OBJECT) continue;
            ServiceInfo s;
            s.id             = (int)row.GetInt("id");
            s.name           = row.GetString("name");
            s.kind           = row.GetString("kind");
            s.domain         = row.GetString("domain");
            s.auth_realm     = row.GetString("auth_realm");
            s.inbound_policy = row.GetString("inbound_policy", "any");
            s.priority       = (int)row.GetInt("priority", 100);
            std::string en   = row.GetString("enabled");
            s.enabled        = (en != "false" && en != "0");
            SimpleJson::JsonNode lst = row.Get("listeners");
            if (lst.type == SimpleJson::JSON_ARRAY) {
                for (size_t j = 0; j < lst.Size(); ++j) {
                    int lid = (int)lst.At(j).AsInt();
                    if (lid > 0) s.listeners.push_back(lid);
                }
            }
            if (s.id > 0 && !s.domain.empty()) newList.push_back(s);
        }
    }
    std::sort(newList.begin(), newList.end(), [](const ServiceInfo& a, const ServiceInfo& b){
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.id < b.id;
    });
    std::lock_guard<std::mutex> lk(m_mutex);
    m_services.swap(newList);
    CLog::Print(LOG_INFO, "ServiceMap: sync complete, %zu services", m_services.size());
    return true;
}

ServiceInfo CCspServiceMap::GetById(int id) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& s : m_services) {
        if (s.id == id) return s;
    }
    // Legacy compat: jsonl 에 service 가 하나도 정의되지 않았고 요청 id > 0 이면
    // Setup 의 AuthRealm 을 domain 으로 가진 default service 를 돌려준다.
    // (ptt_subscriptions/voip_subscriptions 의 service_id 가 있지만 service.jsonl 이 없을 때 사용)
    if (m_services.empty() && id > 0) {
        ServiceInfo fb;
        fb.id             = id;
        fb.name           = "default-compat";
        fb.kind           = "compat";
        fb.domain         = gclsSetup.m_strAuthRealm;
        fb.auth_realm     = gclsSetup.m_strAuthRealm;
        fb.inbound_policy = "any";
        fb.priority       = 100;
        fb.enabled        = true;
        return fb;
    }
    return ServiceInfo();
}

ServiceInfo CCspServiceMap::GetByDomain(const std::string& domain) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& s : m_services) {
        if (s.enabled && s.domain == domain) return s;
    }
    return ServiceInfo();
}

std::vector<ServiceInfo> CCspServiceMap::GetAll() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_services;
}

std::string CCspServiceMap::EffectiveRealm(const ServiceInfo& svc) {
    return svc.auth_realm.empty() ? svc.domain : svc.auth_realm;
}
