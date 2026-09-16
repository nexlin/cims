#include "Worker.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "SimSession.h"

static void logf(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf(const char* level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "%s [%s] %s\n", ts, level, buf);
}

long long Worker::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string detectLocalIp() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1";
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    std::string out = "127.0.0.1";
    if (connect(sock, (const sockaddr*)&serv, sizeof(serv)) == 0) {
        sockaddr_in name{};
        socklen_t nl = sizeof(name);
        if (getsockname(sock, (sockaddr*)&name, &nl) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &name.sin_addr, buf, sizeof(buf))) out = buf;
        }
    }
    close(sock);
    return out;
}

static HttpResponse jsonResp(int status, const Json& body) {
    HttpResponse r;
    r.status = status;
    r.body = body.dump();
    return r;
}

static HttpResponse errResp(int status, const std::string& code, const std::string& detail = "") {
    Json j = Json::Object();
    j["error"] = Json(code);
    if (!detail.empty()) j["detail"] = Json(detail);
    return jsonResp(status, j);
}

Worker::Worker(const WorkerConfig& cfg) : m_cfg(cfg) {
    if (m_cfg.localIp.empty()) m_cfg.localIp = detectLocalIp();
    m_runState = "idle";
}

Worker::~Worker() { stop(); }

bool Worker::start(std::string& err) {
    if (!m_http.start(m_cfg.bindIp, m_cfg.port, [this](const HttpRequest& r) { return handle(r); }, err)) return false;
    m_stop = false;
    m_sched = std::thread([this] { schedLoop(); });
    logf("info", "cims-tester-worker %s started — control %s:%d local_ip=%s cores=%ld",
         m_cfg.name.c_str(), m_cfg.bindIp.c_str(), m_cfg.port, m_cfg.localIp.c_str(), sysconf(_SC_NPROCESSORS_ONLN));
    return true;
}

void Worker::stop() {
    if (!m_sched.joinable()) return;
    m_stop = true;
    m_sched.join();
    m_http.stop();
    m_stream.stop();
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& kv : m_pools)
        for (auto& ep : kv.second->eps)
            if (ep->started) { ep->s->Stop(5); ep->started = false; }
}

// ── ICsimObserver (스택 스레드) ─────────────────────────────────────────────
void Worker::OnRegister(SimSession* s, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REGISTER, s, st, ms, "" });
}
void Worker::OnIncomingCall(SimSession* s, const std::string& callId, const std::string&) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::INCOMING, s, 0, 0, callId });
}
void Worker::OnCallStart(SimSession* s, const std::string& callId, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::CALLSTART, s, 200, ms, callId });
}
void Worker::OnCallEnd(SimSession* s, const std::string& callId, int st) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::CALLEND, s, st, 0, callId });
}
void Worker::OnByeResponse(SimSession* s, const std::string& callId, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::BYERESP, s, st, ms, callId });
}

// ── HTTP ───────────────────────────────────────────────────────────────────
HttpResponse Worker::handle(const HttpRequest& req) {
    Json body;
    if (!req.body.empty()) {
        std::string perr;
        if (!Json::parse(req.body, body, perr)) return errResp(400, "bad_json", perr);
    }
    const std::string& p = req.path;
    if (req.method == "GET" && p == "/health") return health();
    if (req.method == "POST" && p == "/pools") return poolCreate(body);
    if (req.method == "DELETE" && p.rfind("/pools/", 0) == 0) return poolDelete(p.substr(7));
    if (req.method == "POST" && p == "/runs") return runStart(body);
    if (p.rfind("/runs/", 0) == 0) {
        std::string rest = p.substr(6);
        size_t sl = rest.find('/');
        std::string id = sl == std::string::npos ? rest : rest.substr(0, sl);
        std::string action = sl == std::string::npos ? "" : rest.substr(sl + 1);
        if (req.method == "POST" && action == "rate") return runRate(id, body);
        if (req.method == "POST" && action == "stop") return runStop(id, body);
        if (req.method == "GET" && action.empty()) return runGet(id);
    }
    return errResp(404, "not_found", req.method + " " + p);
}

HttpResponse Worker::health() {
    std::lock_guard<std::mutex> lk(m_mtx);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    Json j = Json::Object();
    j["worker"] = Json(m_cfg.name);
    j["version"] = Json(m_cfg.version);
    j["max_endpoints"] = Json((long long)(cores * m_cfg.maxEndpointsPerCore));
    j["max_saps"] = Json(cores * m_cfg.maxSapsPerCore);
    j["cpu_pct"] = Json(m_cpuPct.load());
    long long active = 0;
    Json pools = Json::Array();
    for (auto& kv : m_pools) {
        long long reg = 0, started = 0;
        for (auto& ep : kv.second->eps) { if (ep->started) started++; if (ep->registered) reg++; }
        active += started;
        Json pj = Json::Object();
        pj["pool"] = Json(kv.first);
        pj["kind"] = Json(kv.second->kind);
        pj["endpoints"] = Json((long long)kv.second->eps.size());
        pj["registered"] = Json(reg);
        pools.push(pj);
    }
    j["active_endpoints"] = Json(active);
    if (m_run && m_runState != "stopped") j["active_run"] = Json(m_run->runId); else j["active_run"] = Json();
    j["clock_unix_ms"] = Json(nowMs());
    j["pools"] = pools;
    return jsonResp(200, j);
}

HttpResponse Worker::poolCreate(const Json& d) {
    std::string name = d["pool"].asString();
    std::string kind = d["kind"].asString("ue");
    if (name.empty()) return errResp(400, "pool_required");
    if (kind != "ue") return errResp(400, "unsupported_kind", kind + " — B 단계는 ue 풀만 (peer/real-ue 는 C·F)");
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_run && m_runState != "stopped" && m_runState != "idle") return errResp(409, "run_active");
    auto it = m_pools.find(name);
    if (it != m_pools.end()) {
        for (auto& ep : it->second->eps) { m_bySession.erase(ep->s); if (ep->started) ep->s->Stop(5); delete ep->s; }
        m_pools.erase(it);
    }
    auto pool = std::make_unique<Pool>();
    pool->name = name;
    pool->kind = kind;
    pool->transport = d["transport"].asString("udp");
    pool->srtp = d["srtp"].asString("off");
    const Json& tc = d["target_csp"];
    pool->targetIp = tc["ip"].asString();
    pool->targetPort = (int)(pool->transport == "tls" ? tc["tls"].asInt(5061)
                             : pool->transport == "tcp" ? tc["tcp"].asInt(25061) : tc["udp"].asInt(5060));
    if (pool->targetIp.empty()) return errResp(400, "target_csp.ip_required");
    const Json& ids = d["identities"];
    for (size_t i = 0; i < ids.size(); ++i) {
        const Json& x = ids.at(i);
        auto ep = std::make_unique<Endpoint>();
        ep->idx = (int)i;
        ep->pool = name;
        ep->id.user = x["user"].asString();
        ep->id.domain = x["domain"].asString();
        ep->id.ha1 = x["ha1"].asString();
        ep->id.password = x["password"].asString();
        ep->id.display = x["display"].asString();
        ep->id.authScheme = x["auth_scheme"].asString("digest");
        ep->id.akaK = x["aka_k"].asString();
        ep->id.akaOpc = x["aka_opc"].asString();
        std::string authId = x["auth_id"].asString();
        if (ep->id.user.empty() || ep->id.domain.empty()) return errResp(400, "identity_user_domain_required");
        // IMPI — '@' 없으면 도메인을 붙인다 (cspsim -creds authId 규약과 같다; CSP 는 authId@domain 을 기대한다)
        if (!authId.empty() && authId.find('@') == std::string::npos) authId += "@" + ep->id.domain;
        int localPort = m_cfg.sipPortBase > 0 ? m_cfg.sipPortBase + 2 * (int)i : 0;
        ep->s = new SimSession((int)i, ep->id.user, authId, ep->id.domain, ep->id.password, ep->id.ha1,
                               pool->targetIp, pool->targetPort, m_cfg.localIp, localPort, false, "");
        if (pool->transport == "tls") ep->s->SetTransport(E_SIP_TLS);
        else if (pool->transport == "tcp") ep->s->SetTransport(E_SIP_TCP);
        ep->s->SetSrtpMode(pool->srtp == "required" ? 2 : pool->srtp == "optional" ? 1 : 0);
        if (!ep->id.akaK.empty()) ep->s->SetAka(ep->id.akaK, ep->id.akaOpc, 0);
        ep->s->SetAnswerMode(SimSession::E_ANSWER_DEFERRED);
        ep->s->SetObserver(this);
        if (!m_cfg.mediaFile.empty()) ep->s->m_clsRtpThread.SetMediaFile(m_cfg.mediaFile);
        if (!m_cfg.videoFile.empty()) ep->s->m_clsRtpThread.SetVideoFile(m_cfg.videoFile);
        m_bySession[ep->s] = ep.get();
        pool->eps.push_back(std::move(ep));
    }
    logf("info", "pool %s created — kind=%s endpoints=%zu transport=%s srtp=%s target=%s:%d",
         name.c_str(), kind.c_str(), pool->eps.size(), pool->transport.c_str(), pool->srtp.c_str(),
         pool->targetIp.c_str(), pool->targetPort);
    Json j = Json::Object();
    j["pool"] = Json(name);
    j["endpoints"] = Json((long long)pool->eps.size());
    m_pools[name] = std::move(pool);
    return jsonResp(201, j);
}

HttpResponse Worker::poolDelete(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_run && m_runState != "stopped" && m_runState != "idle") return errResp(409, "run_active");
    auto it = m_pools.find(name);
    if (it == m_pools.end()) return errResp(404, "pool_not_found");
    for (auto& ep : it->second->eps) { m_bySession.erase(ep->s); if (ep->started) ep->s->Stop(5); delete ep->s; }
    m_pools.erase(it);
    Json j = Json::Object();
    j["deleted"] = Json(true);
    return jsonResp(200, j);
}

static const char* kSupported[] = { "register", "deregister", "invite", "answer", "reject", "bye",
                                    "media_hold", "wait", "expect" };

HttpResponse Worker::runStart(const Json& d) {
    auto spec = std::make_unique<RunSpec>();
    spec->runId = d["run_id"].asString();
    spec->scenarioId = d["scenario_id"].asString();
    spec->stream = d["stream"].asString();
    spec->rate = d["rate_saps"].asDouble(0);
    spec->maxInstances = d["max_instances"].asInt(0);
    if (spec->runId.empty() || spec->stream.empty()) return errResp(400, "run_id_and_stream_required");
    for (auto& kv : d["roles"].items()) spec->roles[kv.first] = kv.second.asString();
    for (auto& kv : d["role_slices"].items())
        spec->slices[kv.first] = { (int)kv.second.at(0).asInt(), (int)kv.second.at(1).asInt() };
    const Json& steps = d["steps"];
    std::vector<std::string> unsupported;
    for (size_t i = 0; i < steps.size(); ++i) {
        const Json& s = steps.at(i);
        CompiledStep cs;
        cs.idx = (int)s["idx"].asInt((long long)i);
        cs.step = s["step"].asString();
        for (size_t k = 0; k < s["who"].size(); ++k) cs.who.push_back(s["who"].at(k).asString());
        cs.from = s["from"].asString();
        cs.to = s["to"].asString();
        cs.afterMs = (int)s["after_ms"].asInt(0);
        cs.seconds = (int)s["seconds"].asInt(0);
        cs.group = s["group"].asString();
        cs.payload = s["payload"].asString();
        cs.media = s["media"];
        cs.expect = s["expect"];
        bool ok = false;
        for (const char* k : kSupported) if (cs.step == k) ok = true;
        if (!ok) unsupported.push_back(cs.step);
        spec->steps.push_back(cs);
    }
    if (!unsupported.empty()) {
        std::string list;
        for (auto& u : unsupported) list += (list.empty() ? "" : ",") + u;
        return errResp(400, "unsupported_step", list + " — B 단계 워커는 register/invite/answer/reject/bye/media_hold/wait/expect 만");
    }
    if (spec->steps.empty()) return errResp(400, "steps_required");

    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_run && m_runState != "stopped" && m_runState != "idle") return errResp(409, "run_active", m_run->runId);
    for (auto& kv : spec->roles) {
        if (!m_pools.count(kv.second)) return errResp(400, "pool_not_found", kv.first + " → " + kv.second);
        if (!spec->slices.count(kv.first))
            spec->slices[kv.first] = { 0, (int)m_pools[kv.second]->eps.size() };
    }
    // 단계 분할 — prelude(앞쪽 register/wait) · body · epilogue(끝 deregister)
    m_prelude.clear(); m_body.clear(); m_epilogue.clear();
    size_t b = 0, e = spec->steps.size();
    while (b < e && (spec->steps[b].step == "register" || spec->steps[b].step == "wait")) m_prelude.push_back(spec->steps[b++]);
    while (e > b && spec->steps[e - 1].step == "deregister") { m_epilogue.insert(m_epilogue.begin(), spec->steps[e - 1]); --e; }
    for (size_t i = b; i < e; ++i) m_body.push_back(spec->steps[i]);
    for (auto& st : m_body)
        if (st.step == "register" || st.step == "deregister")
            return errResp(400, "register_in_body", "register/deregister 는 흐름의 앞(prelude)/끝(epilogue)에만 둘 수 있다");

    // prelude 대상 단말 목록 + body 역할별 free 목록
    m_preludeList.clear();
    m_free.clear();
    m_instances.clear();
    std::map<Endpoint*, bool> seen;
    for (auto& st : m_prelude) {
        if (st.step != "register") continue;
        for (auto& role : st.who) {
            auto rit = spec->roles.find(role);
            if (rit == spec->roles.end()) return errResp(400, "unknown_role", role);
            Pool* pool = m_pools[rit->second].get();
            auto sl = spec->slices[role];
            for (int i = sl.first; i < sl.second && i < (int)pool->eps.size(); ++i) {
                Endpoint* ep = pool->eps[i].get();
                if (!seen[ep]) { seen[ep] = true; m_preludeList.push_back(ep); }
            }
        }
    }
    for (auto& kv : spec->roles) {
        Pool* pool = m_pools[kv.second].get();
        auto sl = spec->slices[kv.first];
        for (int i = sl.first; i < sl.second && i < (int)pool->eps.size(); ++i) m_free[kv.first].push_back(pool->eps[i].get());
    }

    m_run = std::move(spec);
    m_rate = m_run->rate;
    m_credit = 0;
    m_launched = 0;
    m_stopRequested = false;
    m_stopAtMs = 0;
    m_preludeCursor = 0;
    m_preludeDeadlineMs = 0;
    m_runStartedMs = nowMs();
    m_runState = "prelude";
    Json hello = Json::Object();
    hello["kind"] = Json("hello");
    hello["worker"] = Json(m_cfg.name);
    hello["version"] = Json(m_cfg.version);
    hello["t"] = Json(nowMs() / 1000.0);
    m_stream.stop();
    m_stream.start(m_run->stream, hello.dump());
    logf("info", "run %s started — scenario=%s rate=%.2f prelude=%zu body=%zu epilogue=%zu roles=%zu prelude_endpoints=%zu stream=%s",
         m_run->runId.c_str(), m_run->scenarioId.c_str(), m_run->rate, m_prelude.size(), m_body.size(), m_epilogue.size(),
         m_run->roles.size(), m_preludeList.size(), m_run->stream.c_str());
    Json j = Json::Object();
    j["run_id"] = Json(m_run->runId);
    j["state"] = Json(m_runState);
    j["prelude_endpoints"] = Json((long long)m_preludeList.size());
    return jsonResp(202, j);
}

HttpResponse Worker::runRate(const std::string& id, const Json& d) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_run || m_run->runId != id) return errResp(404, "run_not_found");
    m_rate = d["rate_saps"].asDouble(0);
    m_metrics.gauge("rate_saps", m_rate);
    logf("info", "run %s rate → %.2f saps", id.c_str(), m_rate.load());
    Json j = Json::Object();
    j["rate_saps"] = Json(m_rate.load());
    return jsonResp(200, j);
}

HttpResponse Worker::runStop(const std::string& id, const Json& d) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_run || m_run->runId != id) return errResp(404, "run_not_found");
    if (m_runState == "stopped") return errResp(200, "already_stopped");
    int drain = (int)d["drain_s"].asInt(5);
    m_stopRequested = true;
    m_rate = 0;
    m_stopAtMs = nowMs() + (long long)drain * 1000;
    m_runState = "draining";
    logf("info", "run %s stop requested — drain %d s", id.c_str(), drain);
    Json j = Json::Object();
    j["state"] = Json(m_runState);
    return jsonResp(202, j);
}

HttpResponse Worker::runGet(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_run || m_run->runId != id) return errResp(404, "run_not_found");
    Json j = m_metrics.totals();
    j["run_id"] = Json(m_run->runId);
    j["state"] = Json(m_runState);
    j["rate_saps"] = Json(m_rate.load());
    long long active = 0;
    for (auto& in : m_instances) if (in->phase != Instance::DONE) active++;
    j["active_instances"] = Json(active);
    j["stream_connected"] = Json(m_stream.connected());
    j["stream_backlog"] = Json((long long)m_stream.backlog());
    return jsonResp(200, j);
}

// ── 스케줄러 ───────────────────────────────────────────────────────────────
void Worker::schedLoop() {
    while (!m_stop) {
        long long now = nowMs();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            drainEvents();
            if (m_run) {
                if (m_runState == "prelude") tickPrelude(now);
                else if (m_runState == "running" || m_runState == "draining") tickBody(now);
            }
            long long nowS = now / 1000;
            if (nowS != m_lastFlushS) {
                m_lastFlushS = nowS;
                sampleCpu();
                if (m_run && m_runState != "stopped" && m_runState != "idle") {
                    long long reg = 0, conc = 0, act = 0;
                    for (auto& kv : m_pools) for (auto& ep : kv.second->eps) if (ep->registered) reg++;
                    for (auto& in : m_instances) {
                        if (in->phase == Instance::DONE) continue;
                        act++;
                        for (auto& a : in->actors) if (a.second->inCall) { conc++; break; }
                    }
                    m_metrics.gauge("registered", (double)reg);
                    m_metrics.gauge("concurrent_sessions", (double)conc);
                    m_metrics.gauge("active_instances", (double)act);
                    m_metrics.gauge("rate_saps", m_rate.load());
                    m_metrics.gauge("cpu_pct", m_cpuPct.load());
                    m_stream.send(m_metrics.flush(m_run->runId, m_cfg.name, (double)nowS).dump());
                    // 끝난 인스턴스 정리
                    m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
                                                     [](const std::unique_ptr<Instance>& i) { return i->phase == Instance::DONE; }),
                                      m_instances.end());
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Worker::sampleCpu() {
    std::ifstream f("/proc/stat");
    std::string cpu;
    long long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0;
    if (f >> cpu >> u >> n >> s >> i >> w >> q >> sq) {
        long long total = u + n + s + i + w + q + sq, idle = i + w;
        if (m_cpuPrevTotal > 0 && total > m_cpuPrevTotal) {
            double pct = 100.0 * (double)((total - m_cpuPrevTotal) - (idle - m_cpuPrevIdle)) / (double)(total - m_cpuPrevTotal);
            m_cpuPct = pct < 0 ? 0 : pct;
        }
        m_cpuPrevTotal = total;
        m_cpuPrevIdle = idle;
    }
}

Endpoint* Worker::endpointOf(SimSession* s) {
    auto it = m_bySession.find(s);
    return it == m_bySession.end() ? nullptr : it->second;
}

void Worker::drainEvents() {
    std::deque<Event> evs;
    { std::lock_guard<std::mutex> lk(m_evMtx); evs.swap(m_events); }
    for (auto& e : evs) onEvent(e);
}

void Worker::emitEvent(const std::string& detail, Endpoint* ep, const std::string& step, int code, const std::string& callId) {
    if (!m_run) return;
    Json j = Json::Object();
    j["kind"] = Json("event");
    j["t"] = Json(nowMs() / 1000.0);
    j["run_id"] = Json(m_run->runId);
    j["worker"] = Json(m_cfg.name);
    if (!callId.empty()) j["call_id"] = Json(callId);
    if (ep) {
        j["identity"] = Json(ep->id.user);
        if (ep->inst) for (auto& a : ep->inst->actors) if (a.second == ep) j["role"] = Json(a.first);
    }
    if (!step.empty()) j["step"] = Json(step);
    if (code) j["code"] = Json(code);
    j["detail"] = Json(detail);
    m_stream.send(j.dump());
}

void Worker::onEvent(const Event& e) {
    Endpoint* ep = endpointOf(e.s);
    if (!ep) return;
    long long now = nowMs();
    Instance* in = ep->inst;
    switch (e.kind) {
    case Event::REGISTER:
        ep->registered = (e.status == 200);
        if (e.status == 200) { m_metrics.counter("registered_ok"); m_metrics.timer("rrd_ms", (double)e.ms); }
        else { m_metrics.counter("registered_fail"); m_metrics.counter("codes." + std::to_string(e.status)); emitEvent("REGISTER failed", ep, "register", e.status); }
        break;
    case Event::INCOMING:
        ep->pendingInvite = true;
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "incoming:" + [&] {
                for (auto& a : in->actors) if (a.second == ep) return a.first; return std::string(); }()) {
            // answer/reject 단계가 착신을 기다리고 있었다 — after_ms 뒤 응답
            in->phase = Instance::WAIT_TIME;
            in->waitUntilMs = now + m_body[in->stepIdx].afterMs;
        }
        if (!in) {
            // 인스턴스 밖의 착신(예: 시나리오에 없는 상대) — 486 로 거절해 스택을 비운다
            ep->s->RejectCall(486);
            ep->pendingInvite = false;
            m_metrics.counter("unexpected_invite");
        }
        break;
    case Event::CALLSTART:
        ep->inCall = true;
        ep->pendingInvite = false;
        m_metrics.counter("sessions");
        m_metrics.timer("srd_ms", (double)e.ms);
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("callstart:", 0) == 0) {
            std::string role = in->awaitKind.substr(10);
            if (in->actors[role] == ep) advance(*in, now);
        }
        break;
    case Event::CALLEND: {
        bool wasInCall = ep->inCall;
        ep->inCall = false;
        ep->pendingInvite = false;
        if (!in) break;
        std::string role;
        for (auto& a : in->actors) if (a.second == ep) role = a.first;
        if (in->phase == Instance::WAIT_EVENT && in->awaitKind == "callend:" + role) {
            // reject 단계: 발신자가 기대한 최종 응답을 받았다
            m_metrics.counter("codes." + std::to_string(e.status));
            advance(*in, now);
            break;
        }
        if (e.status >= 300) {
            m_metrics.counter("codes." + std::to_string(e.status));
            if (in->phase != Instance::DONE) {
                emitEvent("call failed", ep, in->stepIdx < m_body.size() ? m_body[in->stepIdx].step : "", e.status, e.callId);
                finishInstance(*in, true, "final " + std::to_string(e.status), now);
            }
        } else if (wasInCall && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("byeresp:", 0) == 0 &&
                   in->actors[in->awaitKind.substr(8)] != ep) {
            // 상대(착신) 측이 BYE(200) 를 받았다 — 발신 측 BYE 응답을 계속 기다린다
        }
        break;
    }
    case Event::BYERESP:
        ep->inCall = false;
        m_metrics.timer("sdd_ms", (double)e.ms);
        if (e.status / 100 == 2) m_metrics.counter("completed");
        else m_metrics.counter("bye_fail");
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("byeresp:", 0) == 0 &&
            in->actors[in->awaitKind.substr(8)] == ep)
            advance(*in, now);
        break;
    }
}

bool Worker::startEndpoint(Endpoint* ep) {
    if (ep->started) return true;
    ep->s->m_clsRtpThread.ResetRecvStats();
    if (!ep->s->Start()) {
        m_metrics.counter("registered_fail");
        emitEvent("stack start failed", ep, "register", 0);
        return false;
    }
    ep->started = true;
    return true;
}

void Worker::tickPrelude(long long now) {
    if (m_preludeDeadlineMs == 0) {
        long long extraWait = 0;
        for (auto& st : m_prelude) if (st.step == "wait") extraWait += (long long)st.seconds * 1000;
        m_preludeDeadlineMs = now + (long long)m_cfg.registerTimeoutS * 1000 +
                              (long long)m_preludeList.size() * m_cfg.registerIntervalMs + extraWait;
        if (m_preludeList.empty()) { m_runState = "running"; logf("info", "run %s prelude empty → running", m_run->runId.c_str()); return; }
    }
    // 간격을 두고 Start(REGISTER). 이미 등록된 단말(이전 run)은 건너뛴다.
    static long long lastStart = 0;
    while (m_preludeCursor < m_preludeList.size() && now - lastStart >= m_cfg.registerIntervalMs) {
        Endpoint* ep = m_preludeList[m_preludeCursor++];
        lastStart = now;
        if (ep->started) { if (ep->registered) m_metrics.counter("registered_reused"); continue; }
        startEndpoint(ep);
        if (m_preludeCursor % 100 == 0) logf("info", "prelude: %zu/%zu started", m_preludeCursor, m_preludeList.size());
    }
    if (m_preludeCursor < m_preludeList.size()) return;
    size_t reg = 0, pending = 0;
    for (auto* ep : m_preludeList) { if (ep->registered) reg++; else if (ep->started) pending++; }
    // 전원 응답(성공/실패) 또는 상한 → body 로. 실패한 단말은 free 목록에서 뺀다.
    size_t answered = 0;
    for (auto* ep : m_preludeList) if (ep->registered || !ep->started) answered++;
    bool timeout = now >= m_preludeDeadlineMs;
    if (pending > 0 && !timeout) {
        // 실패 응답(REGISTER 4xx) 은 registered=false 이면서 started=true — 이벤트 카운터로 판정 불가하므로
        // 등록 통계(iRegFail) 를 본다.
        size_t failed = 0;
        for (auto* ep : m_preludeList) if (ep->started && !ep->registered && ep->s->m_stats.iRegFail > 0) failed++;
        if (failed < pending) return;
    }
    (void)answered;
    for (auto& kv : m_free) {
        auto& v = kv.second;
        v.erase(std::remove_if(v.begin(), v.end(), [](Endpoint* ep) { return ep->started && !ep->registered; }), v.end());
    }
    logf("info", "run %s prelude done — registered=%zu/%zu%s → running", m_run->runId.c_str(), reg, m_preludeList.size(),
         timeout ? " (timeout)" : "");
    if (m_stopRequested) { endRun("stopped"); return; }
    m_runState = "running";
    m_credit = 0;
    m_metrics.gauge("rate_saps", m_rate.load());
}

void Worker::tickBody(long long now) {
    // 시간 대기·마감
    for (auto& inp : m_instances) {
        Instance& in = *inp;
        if (in.phase == Instance::WAIT_TIME && now >= in.waitUntilMs) {
            if (in.pending == Instance::ANSWER || in.pending == Instance::REJECT) {
                Endpoint* ep = in.actors[in.pendingRole];
                bool ok = in.pending == Instance::ANSWER ? ep->s->AnswerCall() : ep->s->RejectCall(in.pendingCode);
                if (!ok) { finishInstance(in, true, "no pending invite to answer", now); continue; }
                ep->pendingInvite = false;
                if (in.pending == Instance::ANSWER) {
                    ep->inCall = true;
                    // 발신자 확립(200 OK 수신) 을 기다린다 — 다음 단계는 그 뒤
                    std::string caller;
                    for (auto& a : in.actors) if (a.second->tStartCallMs > 0) caller = a.first;
                    in.pending = Instance::NONE;
                    if (!caller.empty() && !in.actors[caller]->inCall) {
                        in.phase = Instance::WAIT_EVENT;
                        in.awaitKind = "callstart:" + caller;
                        in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                        continue;
                    }
                } else {
                    // 거절: 발신자의 최종 응답 도착을 기다린다
                    std::string caller;
                    for (auto& a : in.actors) if (a.second->tStartCallMs > 0) caller = a.first;
                    in.pending = Instance::NONE;
                    if (!caller.empty()) {
                        in.phase = Instance::WAIT_EVENT;
                        in.awaitKind = "callend:" + caller;
                        in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                        continue;
                    }
                }
                advance(in, now);
                continue;
            }
            advance(in, now);
        } else if (in.phase == Instance::WAIT_EVENT && now >= in.deadlineMs) {
            emitEvent("timeout waiting " + in.awaitKind, nullptr, in.stepIdx < m_body.size() ? m_body[in.stepIdx].step : "", 0);
            finishInstance(in, true, "timeout " + in.awaitKind, now);
        }
    }
    // 발생 — 토큰 버킷 (rate × dt)
    static long long lastTick = 0;
    if (lastTick == 0) lastTick = now;
    double rate = m_rate.load();
    bool quotaDone = m_run->maxInstances > 0 && m_launched >= m_run->maxInstances;
    if (rate > 0 && m_runState == "running" && !quotaDone) {
        m_credit += rate * (double)(now - lastTick) / 1000.0;
        if (m_credit > rate * 2 + 1) m_credit = rate * 2 + 1;   // 긴 정지 뒤 폭주 방지
        while (m_credit >= 1.0 && !(m_run->maxInstances > 0 && m_launched >= m_run->maxInstances)) {
            m_credit -= 1.0;
            launchInstance(now);
        }
        quotaDone = m_run->maxInstances > 0 && m_launched >= m_run->maxInstances;
    }
    lastTick = now;
    // 단발(max_instances) — 할당량을 다 냈고 진행 중 인스턴스가 없으면 스스로 닫는다
    if (quotaDone && m_runState == "running") {
        bool anyActive = false;
        for (auto& in : m_instances) if (in->phase != Instance::DONE) { anyActive = true; break; }
        if (!anyActive) { endRun("stopped"); return; }
    }
    // 종료 판정
    if (m_runState == "draining") {
        bool anyActive = false;
        for (auto& in : m_instances) if (in->phase != Instance::DONE) { anyActive = true; break; }
        if (!anyActive || now >= m_stopAtMs) {
            for (auto& in : m_instances) if (in->phase != Instance::DONE) finishInstance(*in, true, "drain timeout", now);
            endRun("stopped");
        }
    }
}

void Worker::launchInstance(long long now) {
    // body 의 모든 역할에 free 단말이 있어야 한다
    std::map<std::string, Endpoint*> actors;
    for (auto& kv : m_run->roles) {
        bool used = false;
        for (auto& st : m_body) {
            if (st.from == kv.first || st.to == kv.first) used = true;
            for (auto& w : st.who) if (w == kv.first) used = true;
        }
        if (!used) continue;
        auto& v = m_free[kv.first];
        // 등록된(또는 등록 없이 쓰는) 단말만
        Endpoint* pick = nullptr;
        while (!v.empty()) {
            Endpoint* c = v.back(); v.pop_back();
            if (c->inst == nullptr && (c->registered || !c->started)) { pick = c; break; }
        }
        if (!pick) {
            for (auto& a : actors) releaseEndpoint(a.second);
            m_metrics.counter("skipped");
            return;
        }
        actors[kv.first] = pick;
    }
    auto in = std::make_unique<Instance>();
    in->id = m_nextInstanceId++;
    in->actors = actors;
    in->tStartMs = now;
    for (auto& a : actors) { a.second->inst = in.get(); a.second->tStartCallMs = 0; a.second->s->m_clsRtpThread.ResetRecvStats(); }
    Instance* raw = in.get();
    m_instances.push_back(std::move(in));
    m_launched++;
    m_metrics.counter("attempts");
    execStep(*raw, now);
}

void Worker::execStep(Instance& in, long long now) {
    while (in.phase == Instance::RUNNING) {
        if (in.stepIdx >= m_body.size()) { finishInstance(in, in.failed, "", now); return; }
        const CompiledStep& st = m_body[in.stepIdx];
        if (st.step == "invite") {
            Endpoint* from = in.actors[st.from];
            Endpoint* to = st.to.empty() ? nullptr : in.actors[st.to];
            if (!from || !to) { finishInstance(in, true, "invite: role missing", now); return; }
            if (!from->started && !startEndpoint(from)) { finishInstance(in, true, "invite: stack start failed", now); return; }
            from->tStartCallMs = now;
            from->s->StartCall(to->id.user);
            if (from->s->m_strInviteId.empty()) { finishInstance(in, true, "invite: StartCall refused (busy?)", now); return; }
            m_metrics.counter("legs", 2);
            in.stepIdx++;   // 비동기 — 확립은 answer/media_hold 가 기다린다
            continue;
        }
        if (st.step == "answer" || st.step == "reject") {
            std::string role = st.who.empty() ? st.to : st.who[0];
            Endpoint* ep = in.actors[role];
            if (!ep) { finishInstance(in, true, st.step + ": role missing", now); return; }
            in.pending = st.step == "answer" ? Instance::ANSWER : Instance::REJECT;
            in.pendingRole = role;
            in.pendingCode = st.step == "reject" ? (int)st.expect["code"].asInt(486) : 0;
            if (st.step == "reject" && st.payload.size()) in.pendingCode = atoi(st.payload.c_str());
            if (ep->pendingInvite) { in.phase = Instance::WAIT_TIME; in.waitUntilMs = now + st.afterMs; }
            else { in.phase = Instance::WAIT_EVENT; in.awaitKind = "incoming:" + role; in.deadlineMs = now + m_cfg.inviteTimeoutMs; }
            return;
        }
        if (st.step == "media_hold") {
            std::string caller;
            for (auto& a : in.actors) if (a.second->tStartCallMs > 0) caller = a.first;
            if (!caller.empty() && !in.actors[caller]->inCall) {
                in.phase = Instance::WAIT_EVENT; in.awaitKind = "callstart:" + caller; in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                // 확립되면 다시 이 단계로 온다(advance 가 stepIdx++ 하므로 하나 되돌린다)
                in.stepIdx--;
                return;
            }
            in.phase = Instance::WAIT_TIME;
            in.waitUntilMs = now + (long long)st.seconds * 1000;
            in.pending = Instance::NONE;
            // 대기가 끝나면 advance → 다음 단계 전에 RTP 표본을 뜬다 (bye 단계 진입 시)
            return;
        }
        if (st.step == "wait") {
            in.phase = Instance::WAIT_TIME;
            in.waitUntilMs = now + (long long)st.seconds * 1000;
            in.pending = Instance::NONE;
            return;
        }
        if (st.step == "bye") {
            // media_hold 직후라면 RTP 품질 표본
            if (in.stepIdx > 0 && m_body[in.stepIdx - 1].step == "media_hold")
                for (auto& a : in.actors) sampleRtp(a.second);
            Endpoint* from = in.actors[st.from];
            if (!from) { finishInstance(in, true, "bye: role missing", now); return; }
            if (!from->inCall || from->s->m_strInviteId.empty()) {
                // 이미 끝난 호(상대 종료·실패) — BYE 없이 통과
                in.stepIdx++;
                continue;
            }
            from->s->StopCall();
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "byeresp:" + st.from;
            in.deadlineMs = now + m_cfg.byeTimeoutMs;
            return;
        }
        if (st.step == "expect") { in.stepIdx++; continue; }
        finishInstance(in, true, "unsupported step " + st.step, now);
        return;
    }
}

void Worker::sampleRtp(Endpoint* ep) {
    CRtpThread& rt = ep->s->m_clsRtpThread;
    unsigned long long rx = rt.m_ullRecvTotal.load(), lost = rt.m_ullRecvLost.load();
    long long jitterUs = rt.m_llRecvJitterUs.load();
    m_metrics.counter("rtp_rx", (long long)rx);
    m_metrics.counter("rtp_lost", (long long)lost);
    if (rx + lost > 0) {
        m_metrics.timer("rtp_loss_pct", 100.0 * (double)lost / (double)(rx + lost));
        m_metrics.timer("jitter_ms", (double)jitterUs / 1000.0);
    } else {
        m_metrics.counter("rtp_silent_legs");
    }
    rt.ResetRecvStats();
}

void Worker::releaseEndpoint(Endpoint* ep) {
    if (ep->inCall || !ep->s->m_strInviteId.empty()) ep->s->StopCall();
    if (ep->pendingInvite) { ep->s->RejectCall(480); ep->pendingInvite = false; }
    ep->inCall = false;
    ep->tStartCallMs = 0;
    Instance* in = ep->inst;
    ep->inst = nullptr;
    if (in) for (auto& a : in->actors) if (a.second == ep) { m_free[a.first].insert(m_free[a.first].begin(), ep); break; }
}

void Worker::finishInstance(Instance& in, bool failed, const std::string& why, long long now) {
    if (in.phase == Instance::DONE) return;
    in.phase = Instance::DONE;
    in.failed = failed;
    if (failed) { m_metrics.counter("failed"); if (!why.empty()) logf("debug", "instance %lld failed: %s", in.id, why.c_str()); }
    else m_metrics.counter("instances_ok");
    m_metrics.timer("sdt_s", (double)(now - in.tStartMs) / 1000.0);
    // 단말 반환 (남은 호 정리 포함)
    std::vector<Endpoint*> eps;
    for (auto& a : in.actors) eps.push_back(a.second);
    for (auto* ep : eps) releaseEndpoint(ep);
}

void Worker::endRun(const std::string& state) {
    logf("info", "run %s ending → %s", m_run->runId.c_str(), state.c_str());
    // epilogue: deregister 단계가 있으면 그 역할의 단말을 내린다
    for (auto& st : m_epilogue) {
        if (st.step != "deregister") continue;
        for (auto& role : st.who) {
            auto rit = m_run->roles.find(role);
            if (rit == m_run->roles.end()) continue;
            Pool* pool = m_pools[rit->second].get();
            auto sl = m_run->slices[role];
            for (int i = sl.first; i < sl.second && i < (int)pool->eps.size(); ++i) {
                Endpoint* ep = pool->eps[i].get();
                if (!ep->started) continue;
                ep->s->Stop(5);
                ep->started = false;
                ep->registered = false;
            }
        }
    }
    // 마지막 집계 + 종료 로그
    long long nowS = nowMs() / 1000;
    m_stream.send(m_metrics.flush(m_run->runId, m_cfg.name, (double)nowS).dump());
    Json l = Json::Object();
    l["kind"] = Json("log");
    l["t"] = Json(nowMs() / 1000.0);
    l["worker"] = Json(m_cfg.name);
    l["level"] = Json("info");
    l["msg"] = Json("run " + m_run->runId + " " + state);
    m_stream.send(l.dump());
    m_runState = state;
    m_instances.clear();
    m_free.clear();
    // 스트림은 열어 둔다 — 남은 레코드는 송신 스레드가 계속 보내고, 다음 run 시작(runStart) 이나 워커 정지(stop)
    // 가 닫는다. 여기서 별도 스레드로 닫으면 stop() 의 join 과 겹쳐 같은 스레드를 두 번 join 하게 된다.
}
