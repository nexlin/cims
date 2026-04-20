#include "CspRouteEngine.h"
#include "CspConfigCache.h"
#include "Log.h"

#include "SipMessage.h"
#include "SipHeader.h"

#include <algorithm>
#include <regex>
#include <cstring>

// 트렁크 조회용
#include "CspTrunkManager.h"

CCspRouteEngine gclsRouteEngine;

// ─────────────────────────────────────────────────────────────
//  Utility: cidr 매칭 (IPv4 만)
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
        int octets[4] = {0,0,0,0};
        if (sscanf(s.c_str(), "%d.%d.%d.%d", &octets[0], &octets[1], &octets[2], &octets[3]) != 4) return 0;
        for (int i = 0; i < 4; ++i) r = (r << 8) | (uint8_t)octets[i];
        return r;
    };
    uint32_t ipN = toUint(ip);
    uint32_t netN = toUint(net);
    uint32_t mask = (prefix >= 32) ? 0xFFFFFFFFu : (prefix <= 0 ? 0u : (0xFFFFFFFFu << (32 - prefix)));
    return (ipN & mask) == (netN & mask);
}

static std::string _fieldValue(const std::string& field, const CSipMessage* msg, const RouteContext& ctx) {
    if (!msg) return "";
    if (field == "method")          return msg->m_strSipMethod;
    if (field == "req_uri_user")    return msg->m_clsReqUri.m_strUser;
    if (field == "req_uri_host")    return msg->m_clsReqUri.m_strHost;
    if (field == "from_uri") {
        std::string s = "sip:";
        s += msg->m_clsFrom.m_clsUri.m_strUser;
        s += "@";
        s += msg->m_clsFrom.m_clsUri.m_strHost;
        return s;
    }
    if (field == "to_uri") {
        std::string s = "sip:";
        s += msg->m_clsTo.m_clsUri.m_strUser;
        s += "@";
        s += msg->m_clsTo.m_clsUri.m_strHost;
        return s;
    }
    if (field == "source_ip")       return ctx.source_ip;
    if (field == "source_trunk")    return ctx.source_trunk;

    // header:<NAME>
    if (field.size() > 7 && field.substr(0, 7) == "header:") {
        std::string name = field.substr(7);
        CSipHeader* h = const_cast<CSipMessage*>(msg)->GetHeader(name.c_str());
        if (h) return h->m_strValue;
    }
    return "";
}

bool CCspRouteEngine::_matchOne(const RouteMatchCond& c, const CSipMessage* msg, const RouteContext& ctx) {
    std::string v = _fieldValue(c.field, msg, ctx);
    bool ok = false;
    if (c.op == "equals")          ok = (v == c.value);
    else if (c.op == "not_equals") ok = (v != c.value);
    else if (c.op == "prefix")     ok = (v.size() >= c.value.size() && v.compare(0, c.value.size(), c.value) == 0);
    else if (c.op == "suffix")     ok = (v.size() >= c.value.size() && v.compare(v.size() - c.value.size(), c.value.size(), c.value) == 0);
    else if (c.op == "contains")   ok = (v.find(c.value) != std::string::npos);
    else if (c.op == "regex") {
        try { ok = std::regex_search(v, std::regex(c.value)); }
        catch (...) { ok = false; }
    }
    else if (c.op == "cidr")       ok = _cidrMatch(v, c.value);
    else ok = false;

    return c.invert ? !ok : ok;
}

// ─────────────────────────────────────────────────────────────
//  Cache load / sort
// ─────────────────────────────────────────────────────────────

static RouteRule _parseRule(const SimpleJson::JsonNode& row) {
    RouteRule r;
    r.id          = (int)row.GetInt("id");
    r.name        = row.GetString("name");
    r.enabled     = (row.GetString("enabled") != "false" && row.GetString("enabled") != "0");
    r.priority    = (int)row.GetInt("priority", 100);
    r.description = row.GetString("description");

    SimpleJson::JsonNode matches = row.Get("match");
    if (matches.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < matches.Size(); ++i) {
            SimpleJson::JsonNode m = matches.At(i);
            RouteMatchCond c;
            c.field  = m.GetString("field");
            c.op     = m.GetString("op", "equals");
            c.value  = m.GetString("value");
            c.invert = (m.GetString("invert") == "true" || m.GetString("invert") == "1");
            if (!c.field.empty()) r.matches.push_back(c);
        }
    }

    SimpleJson::JsonNode trans = row.Get("transform");
    if (trans.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < trans.Size(); ++i) {
            SimpleJson::JsonNode t = trans.At(i);
            RouteTransformAction a;
            a.action = t.GetString("action");
            a.target = t.GetString("target");
            a.value  = t.GetString("value");
            if (!a.action.empty()) r.transforms.push_back(a);
        }
    }

    SimpleJson::JsonNode tgt = row.Get("target");
    if (tgt.type == SimpleJson::JSON_OBJECT) {
        r.target_mode     = tgt.GetString("mode", "trunk");
        r.target_trunk_id = (int)tgt.GetInt("trunk_id", 0);
        SimpleJson::JsonNode tgtJson = tgt.Get("json");
        if (tgtJson.type != SimpleJson::JSON_NULL) r.target_json = tgtJson.ToString();
    }

    SimpleJson::JsonNode fail = row.Get("fail");
    if (fail.type == SimpleJson::JSON_OBJECT) {
        r.fail_action       = fail.GetString("action", "reject");
        r.fail_code         = (int)fail.GetInt("code", 404);
        r.fail_reason       = fail.GetString("reason", "Not Found");
        r.fallback_trunk_id = (int)fail.GetInt("fallback", 0);
        r.timeout_ms        = (int)fail.GetInt("timeout_ms", 4000);
        r.retry_count       = (int)fail.GetInt("retry_count", 0);
    }
    return r;
}

bool CCspRouteEngine::Sync() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_ROUTE);
    std::vector<RouteRule> newRules;
    if (items.type == SimpleJson::JSON_ARRAY) {
        for (size_t i = 0; i < items.Size(); ++i) {
            SimpleJson::JsonNode row = items.At(i);
            if (row.type == SimpleJson::JSON_OBJECT) newRules.push_back(_parseRule(row));
        }
    }
    std::sort(newRules.begin(), newRules.end(), [](const RouteRule& a, const RouteRule& b){
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.id < b.id;
    });

    std::lock_guard<std::mutex> lk(m_mutex);
    // hit counter 보존 (같은 id 에 대해)
    std::map<int, std::pair<uint64_t, time_t>> oldHits;
    for (const auto& r : m_rules) oldHits[r.id] = {r.hit_count, r.last_hit_time};
    for (auto& r : newRules) {
        auto it = oldHits.find(r.id);
        if (it != oldHits.end()) {
            r.hit_count = it->second.first;
            r.last_hit_time = it->second.second;
        }
    }
    m_rules.swap(newRules);
    CLog::Print(LOG_INFO, "RouteEngine: sync complete, %zu rules loaded", m_rules.size());
    return true;
}

// ─────────────────────────────────────────────────────────────
//  Evaluation
// ─────────────────────────────────────────────────────────────

void CCspRouteEngine::_fillTargetFromTrunk(RouteDecision& d, int trunk_id) {
    if (trunk_id <= 0) return;
    std::vector<CCspTrunkManager::StatusEntry> trunks;
    gclsTrunkManager.GetStatus(trunks);
    for (const auto& t : trunks) {
        if (t.id != trunk_id) continue;
        // status.remote = "ip:port"
        auto sep = t.remote.rfind(':');
        if (sep == std::string::npos) continue;
        d.target_ip   = t.remote.substr(0, sep);
        d.target_port = atoi(t.remote.c_str() + sep + 1);
        d.target_protocol = "UDP";   // P4: UDP 한정
        d.target_trunk_id = trunk_id;
        return;
    }
}

RouteDecision CCspRouteEngine::Evaluate(const CSipMessage* pclsMessage, const RouteContext& ctx) {
    RouteDecision d;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& r : m_rules) {
        if (!r.enabled) continue;
        bool allMatch = true;
        for (const auto& c : r.matches) {
            if (!_matchOne(c, pclsMessage, ctx)) { allMatch = false; break; }
        }
        if (!allMatch) continue;

        // 매칭됨
        d.matched    = true;
        d.rule_id    = r.id;
        d.rule_name  = r.name;
        d.apply      = r.transforms;
        d.target_mode = r.target_mode;

        if (r.target_mode == "reject") {
            d.reject     = true;
            d.fail_code  = r.fail_code;
            d.fail_reason = r.fail_reason;
        } else if (r.target_mode == "trunk") {
            _fillTargetFromTrunk(d, r.target_trunk_id);
            if (d.target_trunk_id == 0) {
                d.reject = (r.fail_action == "reject");
                d.fail_code = r.fail_code;
                d.fail_reason = r.fail_reason;
            }
        } else {
            // priority_list / round_robin / weighted 는 target_json 기반 — 후속 phase
            d.reject = (r.fail_action == "reject");
            d.fail_code = r.fail_code;
            d.fail_reason = r.fail_reason;
        }

        // 히트 카운터
        r.hit_count++;
        r.last_hit_time = time(nullptr);
        return d;
    }
    return d;   // matched=false
}

bool CCspRouteEngine::ApplyTransforms(CSipMessage* pclsMessage,
                                       const std::vector<RouteTransformAction>& actions) {
    if (!pclsMessage) return false;
    for (const auto& a : actions) {
        if (a.action == "set_req_uri_user") {
            pclsMessage->m_clsReqUri.m_strUser = a.value;
        } else if (a.action == "set_req_uri_host") {
            pclsMessage->m_clsReqUri.m_strHost = a.value;
        } else if (a.action == "set_from_host") {
            pclsMessage->m_clsFrom.m_clsUri.m_strHost = a.value;
        } else if (a.action == "add_header") {
            pclsMessage->AddHeader(a.target.c_str(), a.value.c_str());
        } else if (a.action == "remove_header") {
            SIP_HEADER_LIST& hl = pclsMessage->m_clsHeaderList;
            for (auto it = hl.begin(); it != hl.end(); ) {
                if (strcasecmp(it->m_strName.c_str(), a.target.c_str()) == 0) it = hl.erase(it);
                else ++it;
            }
        } else if (a.action == "replace_header") {
            SIP_HEADER_LIST& hl = pclsMessage->m_clsHeaderList;
            for (auto it = hl.begin(); it != hl.end(); ) {
                if (strcasecmp(it->m_strName.c_str(), a.target.c_str()) == 0) it = hl.erase(it);
                else ++it;
            }
            pclsMessage->AddHeader(a.target.c_str(), a.value.c_str());
        } else if (a.action == "strip_prefix") {
            std::string& u = pclsMessage->m_clsReqUri.m_strUser;
            if (u.size() >= a.value.size() && u.compare(0, a.value.size(), a.value) == 0) {
                u.erase(0, a.value.size());
            }
        } else if (a.action == "add_prefix") {
            pclsMessage->m_clsReqUri.m_strUser.insert(0, a.value);
        }
    }
    return true;
}

void CCspRouteEngine::IncrementHit(int rule_id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& r : m_rules) {
        if (r.id == rule_id) {
            r.hit_count++;
            r.last_hit_time = time(nullptr);
            return;
        }
    }
}

void CCspRouteEngine::GetHits(std::vector<HitEntry>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& r : m_rules) {
        HitEntry e; e.rule_id = r.id; e.hit_count = r.hit_count; e.last_hit = r.last_hit_time;
        out.push_back(e);
    }
}

std::vector<RouteRule> CCspRouteEngine::GetRules() {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_rules;
}
