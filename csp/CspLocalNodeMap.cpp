#include "CspLocalNodeMap.h"
#include "CspConfigCache.h"   // CspUuidToIntId
#include "Log.h"
#include "SimpleJson.h"

CCspLocalNodeMap gclsLocalNodeMap;

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

bool CCspLocalNodeMap::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_LOCAL_NODE);
    std::map<std::string, LocalNodeInfo> newMap;
    if (items.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < items.Size(); ++i) {
            SimpleJson::JsonNode row = items.At(i);
            if (row.type != SimpleJson::JSON_OBJECT) continue;
            LocalNodeInfo n;
            n.id              = row.GetString("id");
            n.name            = row.GetString("name");
            n.edge            = row.GetString("edge", "access");
            n.bind_ip         = row.GetString("bind_ip", "0.0.0.0");
            n.bind_port       = (int)row.GetInt("bind_port", 0);
            n.protocol        = row.GetString("protocol", "UDP");
            n.enabled         = _boolish(row.GetString("enabled"), true);
            n.tls_cert_path   = row.GetString("tls_cert_path");
            n.tls_key_path    = row.GetString("tls_key_path");
            n.tls_ca_path     = row.GetString("tls_ca_path");
            n.tls_verify_peer = _boolish(row.GetString("tls_verify_peer"), false);
            n.max_connections = (int)row.GetInt("max_connections", 0);
            n.tags            = _readStringArray(row.Get("tags"));
            n.note            = row.GetString("note");
            if (n.name.empty()) {
                CLog::Print(LOG_ERROR, "LocalNodeMap: skip record with empty name (id=%s)",
                            n.id.c_str());
                continue;
            }
            if (newMap.count(n.name)) {
                CLog::Print(LOG_ERROR, "LocalNodeMap: duplicate name '%s' — keeping last",
                            n.name.c_str());
            }
            newMap[n.name] = n;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_byName.swap(newMap);
    }
    CLog::Print(LOG_INFO, "LocalNodeMap: sync complete, %zu nodes", Size());
    return true;
}

LocalNodeInfo CCspLocalNodeMap::GetByName(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_byName.find(name);
    if (it == m_byName.end()) return LocalNodeInfo();
    return it->second;
}

LocalNodeInfo CCspLocalNodeMap::GetById(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& kv : m_byName) {
        if (kv.second.id == id) return kv.second;
    }
    return LocalNodeInfo();
}

LocalNodeInfo CCspLocalNodeMap::GetByIntId(int listenerIntId) const {
    if (listenerIntId <= 0) return LocalNodeInfo();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& kv : m_byName) {
        if (CspUuidToIntId(kv.second.id) == listenerIntId) return kv.second;
    }
    return LocalNodeInfo();
}

std::vector<LocalNodeInfo> CCspLocalNodeMap::GetAll() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<LocalNodeInfo> out;
    out.reserve(m_byName.size());
    for (const auto& kv : m_byName) out.push_back(kv.second);
    return out;
}

size_t CCspLocalNodeMap::Size() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byName.size();
}

bool CCspLocalNodeMap::HasName(const std::string& name) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byName.find(name) != m_byName.end();
}
