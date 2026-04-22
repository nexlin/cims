#include "CspAccessControl.h"
#include "CspConfigCache.h"
#include "Log.h"

#include <algorithm>
#include <regex>
#include <chrono>
#include <cstring>
#include <cstdio>

CCspAccessControl gclsAccessControl;

// ─────────────────────────────────────────────────────────────

static bool _cidrMatch(const std::string& ip, const std::string& cidr) {
    auto slash = cidr.find('/');
    std::string net = cidr;
    int prefix = 32;
    if (slash != std::string::npos) {
        net = cidr.substr(0, slash);
        prefix = atoi(cidr.c_str() + slash + 1);
    }
    auto toUint = [](const std::string& s) -> uint32_t {
        uint32_t r = 0;
        int o[4] = {0,0,0,0};
        if (sscanf(s.c_str(), "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3]) != 4) return 0;
        for (int i = 0; i < 4; ++i) r = (r << 8) | (uint8_t)o[i];
        return r;
    };
    uint32_t ipN = toUint(ip);
    uint32_t netN = toUint(net);
    uint32_t mask = (prefix >= 32) ? 0xFFFFFFFFu : (prefix <= 0 ? 0u : (0xFFFFFFFFu << (32 - prefix)));
    return (ipN & mask) == (netN & mask);
}

// ─────────────────────────────────────────────────────────────

bool CCspAccessControl::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_ACCESS);
    std::vector<AccessEntry> newAcl;
    if (items.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < items.Size(); ++i) {
            SimpleJson::JsonNode row = items.At(i);
            if (row.type != SimpleJson::JSON_OBJECT) continue;
            AccessEntry e;
            e.id           = CspUuidToIntId(row.GetString("id"));  // jsonl UUID → int
            e.scope        = row.GetString("scope", "global");
            e.scope_ref_id = (int)row.GetInt("scope_ref_id", 0);
            e.kind         = row.GetString("kind", "allow");
            e.match_type   = row.GetString("match_type", "ip");
            e.value        = row.GetString("value");
            e.enabled      = (row.GetString("enabled") != "false" && row.GetString("enabled") != "0");
            e.priority     = (int)row.GetInt("priority", 100);
            if (!e.value.empty()) newAcl.push_back(e);
        }
    }
    std::sort(newAcl.begin(), newAcl.end(), [](const AccessEntry& a, const AccessEntry& b){
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.id < b.id;
    });
    std::lock_guard<std::mutex> lk(m_mutex);
    m_acl.swap(newAcl);
    CLog::Print(LOG_INFO, "AccessControl: sync complete, %zu ACL entries", m_acl.size());
    return true;
}

void CCspAccessControl::SetRateLimit(int rps_per_ip, int burst) {
    m_rpsPerIp = rps_per_ip;
    m_burst    = burst > 0 ? burst : rps_per_ip * 2;
    CLog::Print(LOG_SYSTEM, "AccessControl: rate limit set rps/ip=%d burst=%d",
                rps_per_ip, m_burst.load());
}

bool CCspAccessControl::_evaluateAcl(const std::string& ip, int listener_id,
                                     const std::string& ua, std::string& outReason) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& e : m_acl) {
        if (!e.enabled) continue;
        // scope 매칭
        if (e.scope == "listener" && e.scope_ref_id != listener_id) continue;
        if (e.scope == "trunk") continue; // trunk-scope 는 outbound 경로에서 별도

        bool hit = false;
        if (e.match_type == "ip") {
            hit = (e.value == ip);
        } else if (e.match_type == "cidr") {
            hit = _cidrMatch(ip, e.value);
        } else if (e.match_type == "ua_regex") {
            try { hit = std::regex_search(ua, std::regex(e.value)); }
            catch (...) { hit = false; }
        }
        if (!hit) continue;

        if (e.kind == "deny") {
            char buf[160];
            snprintf(buf, sizeof(buf), "ACL deny id=%d match=%s:%s scope=%s",
                     e.id, e.match_type.c_str(), e.value.c_str(), e.scope.c_str());
            outReason = buf;
            return false;
        }
        if (e.kind == "allow") {
            // 명시적 allow 가 있으면 이후 규칙 평가 중단 — 허용
            return true;
        }
    }
    // 매칭 없음 → 기본 허용
    return true;
}

bool CCspAccessControl::_consumeToken(const std::string& ip) {
    int rps = m_rpsPerIp.load();
    if (rps <= 0) return true;   // disabled
    int burst = m_burst.load();
    if (burst <= 0) burst = rps * 2;

    double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lk(m_mutex);
    auto& b = m_buckets[ip];
    if (b.last_ts == 0) {
        b.tokens  = burst;
        b.last_ts = now;
    } else {
        double elapsed = now - b.last_ts;
        b.tokens = std::min((double)burst, b.tokens + elapsed * rps);
        b.last_ts = now;
    }
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

CCspAccessControl::Decision CCspAccessControl::Check(const std::string& source_ip,
                                                      int listener_id,
                                                      const std::string& user_agent) {
    Decision d;
    if (source_ip.empty()) return d;   // 소스 IP 모름 → 허용

    std::string reason;
    if (!_evaluateAcl(source_ip, listener_id, user_agent, reason)) {
        d.allowed   = false;
        d.reason    = reason;
        d.http_code = 403;   // Forbidden
        return d;
    }

    if (!_consumeToken(source_ip)) {
        d.allowed   = false;
        d.reason    = "rate limit exceeded";
        d.http_code = 429;   // Too Many Requests (RFC 6585)
        return d;
    }

    return d;
}
