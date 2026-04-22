#include "CspRouteSetMap.h"
#include "CspConfigCache.h"
#include "CspRouteMap.h"
#include "Log.h"
#include "SimpleJson.h"

#include <algorithm>
#include <functional>

CCspRouteSetMap gclsRouteSetMap;

namespace {
bool _boolish(const std::string& v, bool defTrue) {
    if (v.empty()) return defTrue;
    if (v == "false" || v == "0") return false;
    return true;
}
std::vector<std::string> _readStringArray(SimpleJson::JsonNode arr) {
    std::vector<std::string> out;
    if (arr.type != SimpleJson::JSON_ARRAY) return out;
    for (size_t i = 0; i < arr.Size(); ++i) {
        std::string s = arr.At(i).AsString();
        if (!s.empty()) out.push_back(s);
    }
    return out;
}
} // namespace

bool CCspRouteSetMap::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_ROUTE_SET);
    std::map<std::string, RouteSetEntry> newMap;
    if (items.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < items.Size(); ++i) {
            SimpleJson::JsonNode row = items.At(i);
            if (row.type != SimpleJson::JSON_OBJECT) continue;
            RouteSetConfig c;
            c.id                           = row.GetString("id");
            c.name                         = row.GetString("name");
            c.distribution_policy          = row.GetString("distribution_policy", "failover");
            c.health_check_mode            = row.GetString("health_check_mode", "options_ping");
            c.health_check_interval_sec    = (int)row.GetInt("health_check_interval_sec", 30);
            c.health_check_dead_threshold  = (int)row.GetInt("health_check_dead_threshold", 3);
            c.health_check_recovery_probes = (int)row.GetInt("health_check_recovery_probes", 1);
            c.fallback_policy              = row.GetString("fallback_policy", "reject");
            c.enabled                      = _boolish(row.GetString("enabled"), true);
            c.tags                         = _readStringArray(row.Get("tags"));
            c.note                         = row.GetString("note");

            SimpleJson::JsonNode members = row.Get("members");
            if (members.type == SimpleJson::JSON_ARRAY) {
                for (size_t j = 0; j < members.Size(); ++j) {
                    SimpleJson::JsonNode m = members.At(j);
                    if (m.type != SimpleJson::JSON_OBJECT) continue;
                    RouteSetMember mem;
                    mem.route_ref = m.GetString("route_ref");
                    mem.priority  = (int)m.GetInt("priority", 100);
                    mem.weight    = (int)m.GetInt("weight", 1);
                    if (mem.route_ref.empty()) continue;
                    c.members.push_back(mem);
                }
            }

            if (!c.IsValid()) continue;
            if (newMap.count(c.name)) {
                CLog::Print(LOG_ERROR, "RouteSetMap: duplicate name '%s' — keeping first", c.name.c_str());
                continue;
            }

            RouteSetEntry e;
            e.cfg = c;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto oldIt = m_byName.find(c.name);
                if (oldIt != m_byName.end()) e.rt = oldIt->second.rt;
            }
            newMap[c.name] = e;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_byName.swap(newMap);
    }
    CLog::Print(LOG_INFO, "RouteSetMap: sync complete, %zu route sets", Size());
    return true;
}

void CCspRouteSetMap::ValidateRefs() {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_byName) {
        RouteSetConfig& c = kv.second.cfg;
        int missing = 0;
        for (const auto& m : c.members) {
            if (gclsRouteMap.GetByName(m.route_ref).name.empty()) {
                CLog::Print(LOG_ERROR,
                    "RouteSetMap: '%s' references missing route '%s'",
                    c.name.c_str(), m.route_ref.c_str());
                ++missing;
            }
        }
        if (missing > 0) {
            CLog::Print(LOG_SYSTEM,
                "RouteSetMap: '%s' has %d missing route ref(s) — still kept, selection will skip missing",
                c.name.c_str(), missing);
        }
    }
}

RouteSetConfig CCspRouteSetMap::GetByName(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_byName.find(name);
    if (it == m_byName.end()) return RouteSetConfig();
    return it->second.cfg;
}

std::vector<RouteSetConfig> CCspRouteSetMap::GetAll() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<RouteSetConfig> out;
    out.reserve(m_byName.size());
    for (const auto& kv : m_byName) out.push_back(kv.second.cfg);
    return out;
}

size_t CCspRouteSetMap::Size() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byName.size();
}

bool CCspRouteSetMap::HasName(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byName.find(name) != m_byName.end();
}

// ─────────────────────────────────────────────────────────────
// SelectRoute — 정책별 분기 (m_mutex 는 비재귀; RouteMap.IsAlive 는 RouteMap 의 자체 mutex 사용)

std::string CCspRouteSetMap::SelectRoute(const std::string& routeSetName,
                                         const std::string& hashKey,
                                         std::string& outReason) {
    // RouteMap.IsAlive 는 자체 mutex 를 쓰므로 m_mutex 를 잡은 채 호출해도 교차 lock 없음.
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_byName.find(routeSetName);
    if (it == m_byName.end()) {
        outReason = "unknown route_set";
        return "";
    }
    RouteSetEntry& e = it->second;
    if (!e.cfg.enabled) { outReason = "disabled"; return ""; }
    if (e.cfg.members.empty()) { outReason = "no members"; return ""; }

    const std::string& pol = e.cfg.distribution_policy;
    if (pol == "round_robin")    return _selectRoundRobin(e, outReason);
    if (pol == "weighted")       return _selectWeighted(e, outReason);
    if (pol == "hash_by_caller") return _selectHashByCaller(e, hashKey, outReason);
    return _selectFailover(e, outReason);  // default
}

std::string CCspRouteSetMap::_selectFailover(RouteSetEntry& e, std::string& outReason) {
    // priority 오름차순 정렬 후 alive 첫번째
    std::vector<RouteSetMember> sorted = e.cfg.members;
    std::sort(sorted.begin(), sorted.end(),
              [](const RouteSetMember& a, const RouteSetMember& b){
                  return a.priority < b.priority;
              });
    for (const auto& m : sorted) {
        if (m.weight == 0) continue;  // weight=0 은 분배 제외
        if (gclsRouteMap.IsAlive(m.route_ref)) return m.route_ref;
    }
    outReason = "all routes dead (failover)";
    return "";
}

std::string CCspRouteSetMap::_selectRoundRobin(RouteSetEntry& e, std::string& outReason) {
    int n = (int)e.cfg.members.size();
    if (n <= 0) { outReason = "no members"; return ""; }
    int start = e.rt.rr_cursor.load();
    for (int tries = 0; tries < n; ++tries) {
        int idx = (start + tries) % n;
        const RouteSetMember& m = e.cfg.members[idx];
        if (m.weight == 0) continue;
        if (gclsRouteMap.IsAlive(m.route_ref)) {
            e.rt.rr_cursor.store((idx + 1) % n);
            return m.route_ref;
        }
    }
    outReason = "all routes dead (round_robin)";
    return "";
}

std::string CCspRouteSetMap::_selectWeighted(RouteSetEntry& e, std::string& outReason) {
    // Deficit-round-robin 근사: 가중치 합 단위로 순회, 각 membership 의 weight 만큼 선택권.
    // 단순 구현: 누적 weight 를 모아 전체 합에서 커서를 쪼개 선택.
    int n = (int)e.cfg.members.size();
    if (n <= 0) { outReason = "no members"; return ""; }
    int totalW = 0;
    for (const auto& m : e.cfg.members) if (m.weight > 0) totalW += m.weight;
    if (totalW <= 0) { outReason = "total weight 0"; return ""; }

    int target = e.rt.rr_cursor.load() % totalW;
    for (int tries = 0; tries < n; ++tries) {
        int t = target;
        for (const auto& m : e.cfg.members) {
            if (m.weight <= 0) continue;
            if (t < m.weight) {
                if (gclsRouteMap.IsAlive(m.route_ref)) {
                    e.rt.rr_cursor.store((e.rt.rr_cursor.load() + 1) % totalW);
                    return m.route_ref;
                }
                break;  // 이 슬롯 dead — target 증가해서 재시도
            }
            t -= m.weight;
        }
        target = (target + 1) % totalW;
    }
    outReason = "all routes dead (weighted)";
    return "";
}

std::string CCspRouteSetMap::_selectHashByCaller(const RouteSetEntry& e,
                                                 const std::string& key,
                                                 std::string& outReason) {
    int n = (int)e.cfg.members.size();
    if (n <= 0) { outReason = "no members"; return ""; }
    size_t h = std::hash<std::string>{}(key);
    // 먼저 해시 위치의 member 시도 → dead 이면 다음 member 순회
    for (int tries = 0; tries < n; ++tries) {
        int idx = (int)((h + tries) % n);
        const RouteSetMember& m = e.cfg.members[idx];
        if (m.weight == 0) continue;
        if (gclsRouteMap.IsAlive(m.route_ref)) return m.route_ref;
    }
    outReason = "all routes dead (hash)";
    return "";
}
