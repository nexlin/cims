#include "CspTrunkManager.h"
#include "CspConfigCache.h"
#include "SipServerSetup.h"
#include "Log.h"

#include "SipUserAgent.h"
#include "SipStack.h"
#include "SipMessage.h"

#include <thread>
#include <chrono>

extern CSipUserAgent gclsUserAgent;

CCspTrunkManager gclsTrunkManager;

namespace {
    constexpr int kOptionsTimeoutSec = 8;   // 응답 대기 타임아웃
    constexpr int kHealthLoopIntervalMs = 1000;

    std::string _nowCallIdSuffix() {
        char buf[64];
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        snprintf(buf, sizeof(buf), "%lld.%09ld",
                 (long long)ts.tv_sec, ts.tv_nsec);
        return buf;
    }
}

CCspTrunkManager::~CCspTrunkManager() {
    Stop();
}

bool CCspTrunkManager::Start() {
    _loadFromCache();

    m_bStop = false;
    std::thread([this](){ this->_healthLoop(); }).detach();
    CLog::Print(LOG_SYSTEM, "TrunkManager: started (trunks=%zu)", m_mapTrunks.size());
    return true;
}

void CCspTrunkManager::Stop() {
    m_bStop = true;
    // detached thread — 약간 대기 후 메모리 정리
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_mapTrunks) delete kv.second;
    m_mapTrunks.clear();
    m_mapCallIdToTrunk.clear();
}

bool CCspTrunkManager::Sync() {
    _loadFromCache();
    return true;
}

void CCspTrunkManager::_loadFromCache() {
    SimpleJson::JsonNode items = gclsCspConfigCache.GetItems(CACHE_TRUNK);
    if (items.type != SimpleJson::JSON_ARRAY) {
        CLog::Print(LOG_ERROR, "TrunkManager: cache items not array");
        return;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    std::map<int, TrunkRuntime*> newMap;
    for (size_t i = 0; i < items.Size(); ++i) {
        SimpleJson::JsonNode row = items.At(i);
        if (row.type != SimpleJson::JSON_OBJECT) continue;
        // id 는 jsonl UUID 문자열 → 안정적 int 매핑
        int id = CspUuidToIntId(row.GetString("id"));
        if (id <= 0) continue;

        // 기존 runtime 이 있으면 상태 보존
        TrunkRuntime* t = nullptr;
        auto it = m_mapTrunks.find(id);
        if (it != m_mapTrunks.end()) {
            t = it->second;
            m_mapTrunks.erase(it);
        } else {
            t = new TrunkRuntime();
            t->id = id;
        }
        t->name   = row.GetString("name");
        t->enabled = (row.GetString("enabled") != "false" && row.GetString("enabled") != "0");
        t->serviceId        = (int)row.GetInt("service_id", 0);
        t->failoverPriority = (int)row.GetInt("failover_priority", 100);

        // remote 하위 객체 또는 평면 필드 두 가지 지원 (캐시는 중첩 객체 형태)
        SimpleJson::JsonNode remote = row.Get("remote");
        if (remote.type == SimpleJson::JSON_OBJECT) {
            t->remoteIp     = remote.GetString("ip");
            t->remotePort   = (int)remote.GetInt("port", 5060);
            t->remoteDomain = remote.GetString("domain");
            t->protocol     = remote.GetString("protocol", "UDP");
        } else {
            t->remoteIp     = row.GetString("remote_ip");
            t->remotePort   = (int)row.GetInt("remote_port", 5060);
            t->remoteDomain = row.GetString("remote_domain");
            t->protocol     = row.GetString("protocol", "UDP");
        }

        SimpleJson::JsonNode health = row.Get("health");
        if (health.type == SimpleJson::JSON_OBJECT) {
            t->optionsPingSec = (int)health.GetInt("options_ping_sec", 60);
            t->deadThreshold  = (int)health.GetInt("dead_threshold", 3);
        } else {
            t->optionsPingSec = (int)row.GetInt("options_ping_sec", 60);
            t->deadThreshold  = (int)row.GetInt("options_dead_threshold", 3);
        }
        newMap[id] = t;
    }
    // 남은 기존 트렁크 (DB에서 삭제된 것들) 는 메모리 정리
    for (auto& kv : m_mapTrunks) delete kv.second;
    m_mapTrunks.swap(newMap);

    CLog::Print(LOG_INFO, "TrunkManager: sync complete, %zu trunks loaded", m_mapTrunks.size());
}

void CCspTrunkManager::_sendOptions(TrunkRuntime& t) {
    if (!t.enabled || t.optionsPingSec <= 0) return;
    if (t.protocol != "UDP") return;  // P3: UDP only

    CSipMessage* pclsMessage = new CSipMessage();
    if (!pclsMessage) return;

    pclsMessage->m_strSipMethod = SIP_METHOD_OPTIONS;

    // Request-URI: sip:trunk_name@remote_ip:remote_port
    const char* pszUser = t.name.empty() ? "ping" : t.name.c_str();
    pclsMessage->m_clsReqUri.Set(SIP_PROTOCOL, pszUser, t.remoteIp.c_str(), t.remotePort);

    pclsMessage->m_clsFrom.m_clsUri.Set(SIP_PROTOCOL, "cspserver", gclsSetup.m_strLocalIp.c_str());
    pclsMessage->m_clsFrom.InsertTag();

    pclsMessage->m_clsTo.m_clsUri.Set(SIP_PROTOCOL, pszUser,
                                       t.remoteDomain.empty() ? t.remoteIp.c_str() : t.remoteDomain.c_str(),
                                       t.remotePort);

    // Call-ID: trunk-<id>-<ts>@local
    char callIdBuf[160];
    snprintf(callIdBuf, sizeof(callIdBuf), "trunk-%d-%s@%s",
             t.id, _nowCallIdSuffix().c_str(), gclsSetup.m_strLocalIp.c_str());
    // CSipCallId 는 name@host 형식. callIdBuf 는 이미 foo@host 이므로 파싱 대신 직접 채움.
    {
        const char* at = strchr(callIdBuf, '@');
        if (at) {
            pclsMessage->m_clsCallId.m_strName = std::string(callIdBuf, at - callIdBuf);
            pclsMessage->m_clsCallId.m_strHost = at + 1;
        } else {
            pclsMessage->m_clsCallId.m_strName = callIdBuf;
            pclsMessage->m_clsCallId.m_strHost = gclsSetup.m_strLocalIp;
        }
    }

    int seq = ++t.optionsSeq;
    if (seq > 1000000) { t.optionsSeq = 1; seq = 1; }
    pclsMessage->m_clsCSeq.Set(seq, SIP_METHOD_OPTIONS);
    pclsMessage->AddRoute(t.remoteIp.c_str(), t.remotePort);

    {
        std::lock_guard<std::mutex> lk(t.mtx);
        t.pendingCallId = callIdBuf;
        t.pendingSentAt = time(nullptr);
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_mapCallIdToTrunk[callIdBuf] = t.id;
    }

    t.lastPingAt = time(nullptr);
    if (!gclsUserAgent.m_clsSipStack.SendSipMessage(pclsMessage)) {
        CLog::Print(LOG_ERROR, "TrunkManager: OPTIONS send failed id=%d %s:%d",
                    t.id, t.remoteIp.c_str(), t.remotePort);
        // 실패는 즉시 fail 로 처리 (pending 정리)
        std::lock_guard<std::mutex> lk(t.mtx);
        t.pendingCallId.clear();
        std::lock_guard<std::mutex> lk2(m_mutex);
        m_mapCallIdToTrunk.erase(callIdBuf);
        int fails = ++t.consecutiveFailures;
        if (fails >= t.deadThreshold && t.alive) {
            t.alive = false;
            CLog::Print(LOG_SYSTEM, "TrunkManager: id=%d went DEAD (send-fail)", t.id);
        }
    }
}

bool CCspTrunkManager::OnSipResponse(const std::string& callId, int statusCode) {
    int trunkId = -1;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_mapCallIdToTrunk.find(callId);
        if (it == m_mapCallIdToTrunk.end()) return false;
        trunkId = it->second;
        m_mapCallIdToTrunk.erase(it);
    }
    TrunkRuntime* t = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_mapTrunks.find(trunkId);
        if (it == m_mapTrunks.end()) return true; // 이미 삭제됨 — 응답 무시
        t = it->second;
    }
    time_t now = time(nullptr);
    int rtt = -1;
    {
        std::lock_guard<std::mutex> lk(t->mtx);
        if (!t->pendingCallId.empty() && t->pendingSentAt > 0) {
            rtt = (int)((now - t->pendingSentAt) * 1000);
        }
        t->pendingCallId.clear();
        t->pendingSentAt = 0;
    }
    t->lastReplyAt = now;
    t->lastRttMs   = rtt;
    // OPTIONS 응답 해석 (RFC 3261 §11.2):
    //   2xx      — 살아있음 (정상 응답)
    //   405/501  — 살아있음 (메서드 미지원이지만 노드 응답)
    //   3xx/4xx  — 다른 서버가 응답했으므로 네트워크 도달성 OK → 살아있음 (단 408 제외)
    //   408      — 요청 타임아웃 (stack 이 합성 생성) → 실패 취급
    //   5xx      — 서버 오류 → 실패
    bool isAlive = (statusCode >= 200 && statusCode < 500 && statusCode != 408) ||
                   (statusCode == 405) || (statusCode == 501);
    if (isAlive) {
        t->consecutiveFailures = 0;
        if (!t->alive) {
            t->alive = true;
            CLog::Print(LOG_SYSTEM, "TrunkManager: id=%d went ALIVE (status=%d rtt=%dms)",
                        t->id, statusCode, rtt);
        }
    } else {
        int fails = ++t->consecutiveFailures;
        if (fails >= t->deadThreshold && t->alive) {
            t->alive = false;
            CLog::Print(LOG_SYSTEM, "TrunkManager: id=%d went DEAD (bad status=%d)",
                        t->id, statusCode);
        }
    }
    return true;
}

void CCspTrunkManager::_checkTimeouts(time_t now) {
    std::vector<std::pair<int, std::string>> expired;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& kv : m_mapTrunks) {
            TrunkRuntime* t = kv.second;
            std::lock_guard<std::mutex> lkT(t->mtx);
            if (!t->pendingCallId.empty() && t->pendingSentAt > 0
                && (now - t->pendingSentAt) >= kOptionsTimeoutSec)
            {
                expired.push_back({t->id, t->pendingCallId});
                t->pendingCallId.clear();
                t->pendingSentAt = 0;
            }
        }
    }
    for (auto& e : expired) {
        TrunkRuntime* t = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_mapCallIdToTrunk.erase(e.second);
            auto it = m_mapTrunks.find(e.first);
            if (it == m_mapTrunks.end()) continue;
            t = it->second;
        }
        int fails = ++t->consecutiveFailures;
        if (fails >= t->deadThreshold && t->alive) {
            t->alive = false;
            CLog::Print(LOG_SYSTEM, "TrunkManager: id=%d went DEAD (timeout, fails=%d)",
                        t->id, fails);
        }
    }
}

void CCspTrunkManager::_healthLoop() {
    while (!m_bStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kHealthLoopIntervalMs));
        if (m_bStop) break;
        time_t now = time(nullptr);
        _checkTimeouts(now);

        // 주기 ping 발송 대상 선정
        std::vector<int> toPing;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& kv : m_mapTrunks) {
                TrunkRuntime* t = kv.second;
                if (!t->enabled || t->protocol != "UDP" || t->optionsPingSec <= 0) continue;
                time_t since = now - t->lastPingAt;
                if (since >= t->optionsPingSec) {
                    toPing.push_back(t->id);
                }
            }
        }
        for (int id : toPing) {
            TrunkRuntime* t = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_mapTrunks.find(id);
                if (it == m_mapTrunks.end()) continue;
                t = it->second;
            }
            _sendOptions(*t);
        }
    }
}

void CCspTrunkManager::GetStatus(std::vector<StatusEntry>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_mapTrunks) {
        TrunkRuntime* t = kv.second;
        StatusEntry e;
        e.id           = t->id;
        e.name         = t->name;
        char peer[128];
        snprintf(peer, sizeof(peer), "%s:%d", t->remoteIp.c_str(), t->remotePort);
        e.remote       = peer;
        e.enabled      = t->enabled;
        e.alive        = t->alive.load();
        e.last_rtt_ms  = t->lastRttMs.load();
        e.last_ping    = t->lastPingAt.load();
        e.last_reply   = t->lastReplyAt.load();
        e.fail_count   = t->consecutiveFailures.load();
        e.service_id       = t->serviceId;
        e.failover_priority= t->failoverPriority;
        out.push_back(e);
    }
}

void CCspTrunkManager::GetTrunksByService(int service_id, std::vector<TrunkRef>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_mapTrunks) {
        TrunkRuntime* t = kv.second;
        if (!t->enabled || t->serviceId != service_id) continue;
        TrunkRef r;
        r.id                 = t->id;
        r.remote_ip          = t->remoteIp;
        r.remote_port        = t->remotePort;
        r.protocol           = t->protocol;
        r.alive              = t->alive.load();
        r.failover_priority  = t->failoverPriority;
        out.push_back(r);
    }
    // priority 오름차순 (낮을수록 먼저), alive 먼저
    std::sort(out.begin(), out.end(), [](const TrunkRef& a, const TrunkRef& b){
        if (a.alive != b.alive) return a.alive;   // alive 우선
        return a.failover_priority < b.failover_priority;
    });
}
