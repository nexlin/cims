#include "Worker.h"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "SimSession.h"
#include "SipCodecTable.h"

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

static ESipTransport parseTransport(const std::string& s) {
    if (s == "tls" || s == "TLS") return E_SIP_TLS;
    if (s == "tcp" || s == "TCP") return E_SIP_TCP;
    return E_SIP_UDP;
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
    for (auto& kv : m_pools) {
        for (auto& ep : kv.second->eps)
            if (ep->s && ep->started) { ep->s->Stop(5); ep->started = false; }
        if (kv.second->peer) kv.second->peer->Stop();
    }
}

// ── ICsimObserver / ICsimPeerObserver (스택 스레드) ───────────────────────────
void Worker::OnRegister(SimSession* s, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REGISTER, s, nullptr, st, ms, "", "", false });
}
void Worker::OnIncomingCall(SimSession* s, const std::string& callId, const std::string&) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::INCOMING, s, nullptr, 0, 0, callId, "", true });
}
void Worker::OnCallStart(SimSession* s, const std::string& callId, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::CALLSTART, s, nullptr, 200, ms, callId, "", false });
}
void Worker::OnCallEnd(SimSession* s, const std::string& callId, int st, int q850) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::CALLEND, s, nullptr, st, 0, callId, "", false, false, q850 });
}
void Worker::OnCallRing(SimSession* s, const std::string& callId, int st, bool hasSdp, bool prack) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::RING, s, nullptr, st, 0, callId, "", hasSdp, prack });
}
void Worker::OnReInvite(SimSession* s, const std::string& callId, bool hold) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REINVITE, s, nullptr, 0, 0, callId, "", hold });
}
void Worker::OnReInviteResponse(SimSession* s, const std::string& callId, int st) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REINVITE_RESP, s, nullptr, st, 0, callId, "", false });
}
void Worker::OnReferResponse(SimSession* s, const std::string& callId, int st) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REFER_RESP, s, nullptr, st, 0, callId, "", false });
}
void Worker::OnByeResponse(SimSession* s, const std::string& callId, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::BYERESP, s, nullptr, st, ms, callId, "", false });
}
void Worker::OnPeerIncoming(CsimPeer* p, const std::string& callId, const std::string&, const std::string& to, bool hasPai) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::INCOMING, nullptr, p, 0, 0, callId, to, hasPai });
}
void Worker::OnPeerCallStart(CsimPeer* p, const std::string& callId, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::CALLSTART, nullptr, p, 200, ms, callId, "", false });
}
void Worker::OnPeerCallEnd(CsimPeer* p, const std::string& callId, int st, int q850) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::CALLEND, nullptr, p, st, 0, callId, "", false, false, q850 });
}
void Worker::OnPeerRing(CsimPeer* p, const std::string& callId, int st, bool hasSdp, bool prack) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::RING, nullptr, p, st, 0, callId, "", hasSdp, prack });
}
void Worker::OnPeerPrack(CsimPeer* p, const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::PRACK, nullptr, p, 0, 0, callId, "", false });
}
void Worker::OnPeerReInvite(CsimPeer* p, const std::string& callId, bool hold) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REINVITE, nullptr, p, 0, 0, callId, "", hold });
}
void Worker::OnPeerReInviteResponse(CsimPeer* p, const std::string& callId, int st) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REINVITE_RESP, nullptr, p, st, 0, callId, "", false });
}
void Worker::OnPeerReferResponse(CsimPeer* p, const std::string& callId, int st) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REFER_RESP, nullptr, p, st, 0, callId, "", false });
}
void Worker::OnPeerRegister(CsimPeer* p, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::REGISTER, nullptr, p, st, ms, "", "", false });
}
void Worker::OnPeerByeResponse(CsimPeer* p, const std::string& callId, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::BYERESP, nullptr, p, st, ms, callId, "", false });
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
    j["local_ip"] = Json(m_cfg.localIp);
    long long active = 0;
    Json pools = Json::Array();
    for (auto& kv : m_pools) {
        long long reg = 0, started = 0;
        for (auto& ep : kv.second->eps) { if (ep->started) started++; if (ep->registered) reg++; }
        if (kv.second->peer) active += (long long)kv.second->peer->CallCount();
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
    // 미디어 평면(§4) — RTP 를 쓰는 단말 수·상한·샘플 디렉터리의 파일 목록(컨트롤러가 토폴로지 샘플 라이브러리와 대조)
    Json media = Json::Object();
    media["rtp_streams"] = Json(rtpStreams());
    media["max_rtp_streams"] = Json((long long)m_cfg.maxRtpStreams);
    media["sample_dir"] = Json(m_cfg.sampleDir);
    Json files = Json::Array();
    if (!m_cfg.sampleDir.empty()) {
        if (DIR* dp = opendir(m_cfg.sampleDir.c_str())) {
            std::vector<std::string> names;
            while (struct dirent* de = readdir(dp)) {
                if (de->d_name[0] == '.') continue;
                struct stat sb;
                if (stat((m_cfg.sampleDir + "/" + de->d_name).c_str(), &sb) == 0 && S_ISREG(sb.st_mode)) names.push_back(de->d_name);
            }
            closedir(dp);
            std::sort(names.begin(), names.end());
            for (size_t i = 0; i < names.size() && i < 500; ++i) files.push(Json(names[i]));
        }
    }
    media["files"] = files;
    j["media"] = media;
    return jsonResp(200, j);
}

void Worker::destroyPool(Pool* pool) {
    for (auto& ep : pool->eps) {
        if (ep->s) { m_bySession.erase(ep->s); if (ep->started) ep->s->Stop(5); delete ep->s; ep->s = nullptr; }
    }
    if (pool->peer) { m_byPeer.erase(pool->peer.get()); pool->peer->Stop(); pool->peer.reset(); }
}

bool Worker::buildUePool(Pool* pool, const Json& d, std::string& err) {
    const Json& tc = d["target_csp"];
    pool->targetIp = tc["ip"].asString();
    pool->targetPort = (int)(pool->transport == "tls" ? tc["tls"].asInt(5061)
                             : pool->transport == "tcp" ? tc["tcp"].asInt(25061) : tc["udp"].asInt(5060));
    if (pool->targetIp.empty()) { err = "target_csp.ip_required"; return false; }
    const Json& ids = d["identities"];
    for (size_t i = 0; i < ids.size(); ++i) {
        const Json& x = ids.at(i);
        auto ep = std::make_unique<Endpoint>();
        ep->idx = (int)i;
        ep->pool = pool->name;
        ep->poolRef = pool;
        ep->id.user = x["user"].asString();
        ep->id.domain = x["domain"].asString();
        ep->id.ha1 = x["ha1"].asString();
        ep->id.password = x["password"].asString();
        ep->id.display = x["display"].asString();
        ep->id.authScheme = x["auth_scheme"].asString("digest");
        ep->id.akaK = x["aka_k"].asString();
        ep->id.akaOpc = x["aka_opc"].asString();
        std::string authId = x["auth_id"].asString();
        if (ep->id.user.empty() || ep->id.domain.empty()) { err = "identity_user_domain_required"; return false; }
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
        ep->s->SetPrack(d["prack"].asBool(false));
        ep->s->SetDtmf(d["dtmf"].asBool(true));
        ep->s->SetObserver(this);
        if (!m_cfg.mediaFile.empty()) ep->s->m_clsRtpThread.SetMediaFile(m_cfg.mediaFile);
        if (!m_cfg.videoFile.empty()) ep->s->m_clsRtpThread.SetVideoFile(m_cfg.videoFile);
        m_bySession[ep->s] = ep.get();
        pool->eps.push_back(std::move(ep));
    }
    return true;
}

bool Worker::buildPeerPool(Pool* pool, const Json& d, std::string& err) {
    const Json& pd = d["peer"];
    if (!pd.isObject()) { err = "peer_required"; return false; }
    CsimPeerConfig pc;
    pc.name = pool->name;
    pc.profile = pd["profile"].asString("ibcf");
    pc.bindIp = pd["bind"]["ip"].asString();
    pc.port = (int)pd["bind"]["port"].asInt(0);
    pc.transport = parseTransport(pd["bind"]["protocol"].asString("udp"));
    pc.domain = pd["domain"].asString();
    pc.silent = pd["answer"].asString("normal") == "silent";
    pc.certFile = m_cfg.peerCertFile;
    pc.prack = pd["prack"].isBool() ? pd["prack"].asBool() : CsimPeerConfig::DefaultPrack(pc.profile);
    pc.dtmf = pd["dtmf"].asBool(true);
    for (size_t i = 0; i < pd["codecs"].size(); ++i) pc.codecs.push_back(pd["codecs"].at(i).asString());
    if (pc.codecs.empty()) pc.codecs = CsimPeer::DefaultCodecs(pc.profile);
    // 파일 미디어(AMR-WB)는 첫 코덱이 AMR-WB 일 때만 — 나머지는 합성 PCMU
    if (!m_cfg.mediaFile.empty() && strcasecmp(pc.codecs[0].c_str(), "AMR-WB") == 0) pc.mediaFile = m_cfg.mediaFile;
    if (pc.bindIp.empty() || pc.port <= 0 || pc.domain.empty()) { err = "peer.bind.ip/port and peer.domain required"; return false; }
    // 발신 다음 홉 = 대상 CSP 피어링 접속점 (없으면 access UDP 접속점)
    const Json& tc = d["target_csp"];
    const Json& pr = tc["peering"];
    pool->targetIp = pr["ip"].asString(tc["ip"].asString());
    pool->targetPort = (int)(pr.isObject() ? pr["port"].asInt(tc["udp"].asInt(5060)) : tc["udp"].asInt(5060));
    pool->transport = pr.isObject() ? pr["protocol"].asString("udp") : "udp";
    pool->profile = pc.profile;
    if (pool->targetIp.empty()) { err = "target_csp.ip_required"; return false; }
    // pbx 트렁크 REGISTER(SIPconnect 2.0 §8 등록 모드) — 대상의 access 접속점(Digest 챌린지가 있는 쪽)으로, bind 와 같은 transport
    const Json& tr = d["trunk_register"];
    if (tr.isObject()) {
        pc.trunk.user = tr["user"].asString();
        pc.trunk.realm = tr["realm"].asString(tc["domain_volte"].asString(pc.domain));
        pc.trunk.ha1 = tr["ha1"].asString();
        pc.trunk.password = tr["password"].asString();
        pc.trunk.expires = (int)tr["expires"].asInt(3600);
        pc.trunk.ip = tc["ip"].asString();
        pc.trunk.transport = pc.transport;
        pc.trunk.port = (int)(pc.transport == E_SIP_TLS ? tc["tls"].asInt(5061)
                              : pc.transport == E_SIP_TCP ? tc["tcp"].asInt(25061) : tc["udp"].asInt(5060));
        if (pc.trunk.user.empty() || (pc.trunk.ha1.empty() && pc.trunk.password.empty())) { err = "trunk_register.user and ha1|password required"; return false; }
    }
    const Json& ids = d["identities"];
    for (size_t i = 0; i < ids.size(); ++i) {
        const Json& x = ids.at(i);
        auto ep = std::make_unique<Endpoint>();
        ep->idx = (int)i;
        ep->pool = pool->name;
        ep->poolRef = pool;
        ep->id.user = x["user"].asString();
        ep->id.domain = x["domain"].asString(pc.domain);
        if (ep->id.user.empty()) { err = "identity_user_required"; return false; }
        pool->byUser[ep->id.user] = ep.get();
        pool->eps.push_back(std::move(ep));
    }
    pool->peer = std::make_unique<CsimPeer>(pc);
    pool->peer->SetObserver(this);
    if (!pool->peer->Start(err)) { err = "peer_bind_failed: " + err; pool->peer.reset(); return false; }
    m_byPeer[pool->peer.get()] = pool;
    return true;
}

HttpResponse Worker::poolCreate(const Json& d) {
    std::string name = d["pool"].asString();
    std::string kind = d["kind"].asString("ue");
    if (name.empty()) return errResp(400, "pool_required");
    if (kind != "ue" && kind != "peer") return errResp(400, "unsupported_kind", kind + " — real-ue 는 F 단계");
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_run && m_runState != "stopped" && m_runState != "idle") return errResp(409, "run_active");
    auto it = m_pools.find(name);
    if (it != m_pools.end()) { destroyPool(it->second.get()); m_pools.erase(it); }
    auto pool = std::make_unique<Pool>();
    pool->name = name;
    pool->kind = kind;
    pool->transport = d["transport"].asString("udp");
    pool->srtp = d["srtp"].asString("off");
    std::string err;
    bool ok = kind == "ue" ? buildUePool(pool.get(), d, err) : buildPeerPool(pool.get(), d, err);
    if (!ok) { destroyPool(pool.get()); return errResp(400, err); }
    logf("info", "pool %s created — kind=%s endpoints=%zu transport=%s srtp=%s target=%s:%d%s",
         name.c_str(), kind.c_str(), pool->eps.size(), pool->transport.c_str(), pool->srtp.c_str(),
         pool->targetIp.c_str(), pool->targetPort, pool->peer ? (" profile=" + pool->profile).c_str() : "");
    Json j = Json::Object();
    j["pool"] = Json(name);
    j["endpoints"] = Json((long long)pool->eps.size());
    if (pool->peer) j["bind"] = Json(pool->peer->Config().bindIp + ":" + std::to_string(pool->peer->Config().port));
    m_pools[name] = std::move(pool);
    return jsonResp(201, j);
}

HttpResponse Worker::poolDelete(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_run && m_runState != "stopped" && m_runState != "idle") return errResp(409, "run_active");
    auto it = m_pools.find(name);
    if (it == m_pools.end()) return errResp(404, "pool_not_found");
    destroyPool(it->second.get());
    m_pools.erase(it);
    Json j = Json::Object();
    j["deleted"] = Json(true);
    return jsonResp(200, j);
}

static const char* kSupported[] = { "register", "deregister", "invite", "answer", "reject", "bye",
                                    "media_hold", "wait", "expect", "progress", "hold", "resume", "dtmf", "refer",
                                    "media_send", "media_stop" };

static int rtpModeOf(const Json& media) {
    std::string m = media["rtp"].asString("auto");
    return m == "none" ? CRtpThread::E_MEDIA_NONE : m == "explicit" ? CRtpThread::E_MEDIA_EXPLICIT : CRtpThread::E_MEDIA_AUTO;
}

/** 샘플 파일 — 샘플 디렉터리 안의 상대 경로만(절대 경로·`..` 거절). "synthetic"/빈 값은 합성(out 빈 문자열). */
bool Worker::resolveSample(const std::string& file, std::string& out, std::string& err) const {
    out.clear();
    if (file.empty() || file == "synthetic") return true;
    if (file[0] == '/' || file.find("..") != std::string::npos) { err = file + " — 샘플 디렉터리 안의 상대 경로만"; return false; }
    if (m_cfg.sampleDir.empty()) { err = file + " — 워커에 Media.SampleDir 가 없다"; return false; }
    out = m_cfg.sampleDir + "/" + file;
    struct stat sb;
    if (stat(out.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size <= 0) { err = out + " — 파일 없음"; return false; }
    return true;
}

long long Worker::rtpStreams() const {
    long long n = 0;
    for (auto& in : m_instances)
        if (in->phase != Instance::DONE && in->rtpMode != CRtpThread::E_MEDIA_NONE) n += (long long)in->actors.size();
    return n;
}

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
        cs.cause = (int)s["cause"].asInt(0);
        cs.group = s["group"].asString();
        cs.payload = s["payload"].asString();
        cs.sample = s["sample"].asString();
        cs.loop = s["loop"].asBool(true);
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
        return errResp(400, "unsupported_step", list + " — 워커는 register/invite/answer/reject/bye/media_hold/wait/expect/progress/hold/resume/dtmf/refer/media_send/media_stop 만");
    }
    if (spec->steps.empty()) return errResp(400, "steps_required");
    // 샘플 라이브러리(§4) — 컨트롤러가 토폴로지 media.samples 에서 이 시나리오가 참조하는 것만 보낸다. 파일은 지금 확인한다
    for (auto& kv : d["samples"].items()) {
        for (auto& ck : kv.second.items()) {
            if (ck.first != "amr-wb" && ck.first != "pcmu" && ck.first != "pcma")
                return errResp(400, "sample_codec_unsupported", kv.first + "." + ck.first + " — amr-wb|pcmu|pcma");
            std::string path, serr;
            if (!resolveSample(ck.second.asString(), path, serr)) return errResp(400, "sample_missing", kv.first + "." + ck.first + ": " + serr);
            spec->samples[kv.first][ck.first] = path;
        }
    }
    for (auto& cs : spec->steps)
        if (cs.step == "media_send" && !cs.sample.empty() && !spec->samples.count(cs.sample))
            return errResp(400, "sample_unknown", cs.sample + " — RunStart.samples 에 없다");

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
    m_bodyRtpMode = CRtpThread::E_MEDIA_AUTO;
    for (auto& st : m_body) if (st.step == "invite") { m_bodyRtpMode = rtpModeOf(st.media); break; }

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
            if (pool->kind == "peer" && (!pool->peer || pool->peer->Config().trunk.user.empty()))
                return errResp(400, "register_on_peer", role + " — 피어 신원은 등록하지 않는다(고정 수신점). 트렁크 REGISTER 는 풀 register 계정으로");
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
                    m_metrics.gauge("rtp_streams", (double)rtpStreams());
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

Endpoint* Worker::endpointOfPeerCall(CsimPeer* p, const std::string& callId) {
    auto pit = m_byPeer.find(p);
    if (pit == m_byPeer.end()) return nullptr;
    auto it = pit->second->byCall.find(callId);
    return it == pit->second->byCall.end() ? nullptr : it->second;
}

std::string Worker::roleOf(Instance* in, Endpoint* ep) {
    if (in) for (auto& a : in->actors) if (a.second == ep) return a.first;
    return "";
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
        std::string role = roleOf(ep->inst, ep);
        if (!role.empty()) j["role"] = Json(role);
    }
    if (!step.empty()) j["step"] = Json(step);
    if (code) j["code"] = Json(code);
    j["detail"] = Json(detail);
    m_stream.send(j.dump());
}

void Worker::onEvent(const Event& e) {
    Endpoint* ep = nullptr;
    Pool* peerPool = nullptr;
    if (e.s) {
        ep = endpointOf(e.s);
    } else if (e.peer) {
        auto pit = m_byPeer.find(e.peer);
        if (pit == m_byPeer.end()) return;
        peerPool = pit->second;
        if (e.kind == Event::REGISTER) {
            // 트렁크 REGISTER 결과 — 계정 하나가 풀 신원 전부를 대표한다
            bool ok = e.status == 200;
            peerPool->regFailed = !ok;
            for (auto& pe : peerPool->eps) pe->registered = ok;
            if (ok) { m_metrics.counter("registered_ok"); m_metrics.timer("rrd_ms", (double)e.ms); }
            else { m_metrics.counter("registered_fail"); m_metrics.counter("codes." + std::to_string(e.status));
                   emitEvent("trunk REGISTER failed", peerPool->eps.empty() ? nullptr : peerPool->eps[0].get(), "register", e.status); }
            return;
        }
        if (e.kind == Event::INCOMING) {
            // 착신 귀속 — To user 가 풀 신원 범위 안이어야 한다
            auto uit = peerPool->byUser.find(e.user);
            if (uit == peerPool->byUser.end()) {
                e.peer->Reject(e.callId, 404);
                m_metrics.counter("peer_unknown_callee");
                emitEvent("peer INVITE to unknown identity " + e.user, nullptr, "invite", 404, e.callId);
                return;
            }
            ep = uit->second;
            if (!ep->callId.empty() && ep->callId != e.callId) {
                // 같은 신원에 두 번째 호 — 실 단말처럼 486
                e.peer->Reject(e.callId, 486);
                m_metrics.counter("peer_busy");
                return;
            }
            ep->callId = e.callId;
            peerPool->byCall[e.callId] = ep;
            if (peerPool->profile == "ibcf" && !e.hasPai) {
                // TS 24.229 §5.10 — II-NNI 로 넘어오는 INVITE 의 발신 신원 단언이 없다 (§12 CSP 과제)
                m_metrics.counter("pai_missing");
                emitEvent("inbound INVITE without P-Asserted-Identity", ep, "invite", 0, e.callId);
            }
        } else {
            ep = endpointOfPeerCall(e.peer, e.callId);
        }
    }
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
        // 피어 착신 호는 엔진이 INVITE 수신 때 만든다 — Progress/Answer 전에 인스턴스의 RTP 모드를 입힌다
        if (in && ep->isPeer()) ep->poolRef->peer->SetMediaMode(e.callId, in->rtpMode);
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "incoming:" + roleOf(in, ep)) {
            // answer/reject 단계가 착신을 기다리고 있었다 — after_ms 뒤 응답
            in->phase = Instance::WAIT_TIME;
            in->waitUntilMs = now + m_body[in->stepIdx].afterMs;
        }
        if (!in) {
            // 인스턴스 밖의 착신(예: 시나리오에 없는 상대) — 486 로 거절해 스택을 비운다
            epReject(ep, 486);
            ep->pendingInvite = false;
            m_metrics.counter("unexpected_invite");
        }
        break;
    case Event::CALLSTART:
        ep->inCall = true;
        ep->pendingInvite = false;
        m_metrics.counter("sessions");
        m_metrics.timer("srd_ms", (double)e.ms);
        if (in && in->progressTx && in->rtpMode != CRtpThread::E_MEDIA_NONE) {
            // early media 의 미디어 평면 — 183+SDP 뒤 200 전까지 발신자가 실제로 RTP 를 받았는가(시그널링 early_media 와 별개).
            //   이벤트 처리 지연(≤ 스케줄러 틱) 동안 200 뒤 패킷이 한둘 섞일 수 있어 5 패킷(100 ms) 이상을 도달로 본다.
            unsigned long long rx = 0, lost = 0;
            long long jit = 0;
            if (ep->isPeer()) ep->poolRef->peer->RtpStats(e.callId, rx, lost, jit);
            else rx = ep->s->m_clsRtpThread.m_ullRecvTotal.load();
            m_metrics.counter("early_rtp_rx", (long long)rx);
            if (rx >= 5) m_metrics.counter("early_rtp_ok");
            else emitEvent("early media RTP not received before 200 (rx=" + std::to_string(rx) + ")", ep, "progress", 183, e.callId);
            in->progressTx = false;
        }
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("callstart:", 0) == 0) {
            std::string role = in->awaitKind.substr(10);
            if (in->actors[role] == ep) advance(*in, now);
        }
        break;
    case Event::CALLEND: {
        bool wasInCall = ep->inCall;
        ep->inCall = false;
        ep->pendingInvite = false;
        if (ep->isPeer() && ep->callId == e.callId) epClearCall(ep);
        if (e.q850 > 0) { m_metrics.counter("q850_rx"); m_metrics.counter("q850." + std::to_string(e.q850)); }
        if (!in) break;
        std::string role = roleOf(in, ep);
        if (in->phase == Instance::WAIT_EVENT && in->awaitKind == "callend:" + role) {
            // reject 단계 또는 invite expect.code≥300: 발신자가 최종 응답을 받았다 — 기대 코드와 다르면 실패
            m_metrics.counter("codes." + std::to_string(e.status));
            if (in->expectCode >= 300 && e.status != in->expectCode) {
                emitEvent("expected " + std::to_string(in->expectCode) + " got " + std::to_string(e.status), ep, "invite", e.status, e.callId);
                finishInstance(*in, true, "unexpected final", now);
            } else {
                advance(*in, now);
            }
            break;
        }
        if (e.status >= 300) {
            m_metrics.counter("codes." + std::to_string(e.status));
            if (in->phase != Instance::DONE) {
                if (in->expectCode >= 300 && e.status == in->expectCode && ep->tStartCallMs > 0) {
                    // invite expect.code 가 거절 코드 — 이 최종 응답이 성공 조건 (ACL deny 403, 라우팅 reject 등)
                    finishInstance(*in, false, "", now);
                } else {
                    emitEvent("call failed", ep, in->stepIdx < m_body.size() ? m_body[in->stepIdx].step : "", e.status, e.callId);
                    finishInstance(*in, true, "final " + std::to_string(e.status), now);
                }
            }
        } else if (wasInCall && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("byeresp:", 0) == 0 &&
                   in->actors[in->awaitKind.substr(8)] != ep) {
            // 상대(착신) 측이 BYE(200) 를 받았다 — 발신 측 BYE 응답을 계속 기다린다
        }
        break;
    }
    case Event::RING:
        // 발신자의 1xx — 183 SDP 면 early media, RSeq 가 있었으면 PRACK 을 냈다(RFC 3262)
        m_metrics.counter("ring_rx");
        if (e.hasPai) m_metrics.counter("early_media");
        if (e.prack) m_metrics.counter("prack_tx");
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "ring:" + roleOf(in, ep)) advance(*in, now);
        break;
    case Event::PRACK:
        m_metrics.counter("prack_rx");
        break;
    case Event::REINVITE:
        m_metrics.counter("reinvite_rx");
        m_metrics.counter(e.hasPai ? "remote_hold" : "remote_resume");
        break;
    case Event::REINVITE_RESP:
        m_metrics.counter(e.status / 100 == 2 ? "reinvite_ok" : "reinvite_fail");
        if (e.status / 100 != 2) m_metrics.counter("codes." + std::to_string(e.status));
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "reinviteresp:" + roleOf(in, ep)) {
            if (e.status / 100 == 2) advance(*in, now);
            else { emitEvent("re-INVITE refused", ep, m_body[in->stepIdx].step, e.status, e.callId);
                   finishInstance(*in, true, "reinvite " + std::to_string(e.status), now); }
        }
        break;
    case Event::REFER_RESP:
        m_metrics.counter("refer_codes." + std::to_string(e.status));
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "referresp:" + roleOf(in, ep)) {
            int want = in->expectCode > 0 ? in->expectCode : 202;
            if (e.status == want) advance(*in, now);
            else { emitEvent("REFER expected " + std::to_string(want) + " got " + std::to_string(e.status), ep, "refer", e.status, e.callId);
                   finishInstance(*in, true, "refer " + std::to_string(e.status), now); }
        }
        break;
    case Event::BYERESP:
        ep->inCall = false;
        m_metrics.timer("sdd_ms", (double)e.ms);
        if (e.status / 100 == 2) m_metrics.counter("completed");
        else m_metrics.counter("bye_fail");
        if (ep->isPeer() && ep->callId == e.callId) epClearCall(ep);
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("byeresp:", 0) == 0 &&
            in->actors[in->awaitKind.substr(8)] == ep)
            advance(*in, now);
        break;
    }
}

// ── 단말 동작 (UE 세션 / 피어 신원) ────────────────────────────────────────
bool Worker::startEndpoint(Endpoint* ep) {
    if (ep->isPeer() || ep->started) return true;
    ep->s->m_clsRtpThread.ResetRecvStats();
    if (!ep->s->Start()) {
        m_metrics.counter("registered_fail");
        emitEvent("stack start failed", ep, "register", 0);
        return false;
    }
    ep->started = true;
    return true;
}

/** 시나리오 media.audio 이름 → 코덱 테이블 PT (-1 = 지정 없음/미지) */
static int codecPtOf(const std::string& name) {
    if (name.empty()) return -1;
    std::string n = name;
    for (auto& c : n) c = (char)toupper((unsigned char)c);
    for (const auto& e : CSipCodecTable::GetList())
        if (strcasecmp(e.m_strName.c_str(), n.c_str()) == 0) return e.m_iPt;
    return -1;
}

bool Worker::epStartCall(Endpoint* from, Endpoint* to, const Json& media) {
    if (from->isPeer()) {
        Pool* pool = from->poolRef;
        std::string callId = pool->peer->StartCall(from->id.user, to->id.user, to->id.domain, pool->targetIp, pool->targetPort,
                                                   parseTransport(pool->transport), rtpModeOf(media));
        if (callId.empty()) return false;
        from->callId = callId;
        pool->byCall[callId] = from;
        return true;
    }
    if (!from->started && !startEndpoint(from)) return false;
    // 시나리오가 오퍼 코덱을 지정하면(pcma/pcmu/amr-wb …) 그 코덱으로 — 트렁크 G.711 경로 시험. 없으면 풀 기본
    from->s->SetOfferCodec(codecPtOf(media["audio"].asString("")));
    // UE 가 피어 신원을 부를 때 — ibcf(타 IMS) 는 user@피어도메인(Request-URI host → req_uri_host 규칙), pbx/mgcf 는 번호 그대로
    //   (DID/E.164 — CSP 가 번호 prefix 규칙으로 트렁크를 고른다, 실 단말이 다이얼하는 꼴)
    std::string target = to->id.user;
    if (to->isPeer() && to->poolRef->profile == "ibcf") target += "@" + to->id.domain;
    from->s->StartCall(target);
    return !from->s->m_strInviteId.empty();
}

int Worker::epProgress(Endpoint* ep) {
    if (!ep->isPeer()) return 481;   // UE 시뮬레이터는 183 early media 를 내지 않는다(실 단말 착신 모사 아님)
    return ep->callId.empty() ? 481 : ep->poolRef->peer->Progress(ep->callId);
}

bool Worker::epHold(Endpoint* ep, bool hold) {
    if (ep->isPeer()) {
        if (ep->callId.empty()) return false;
        return hold ? ep->poolRef->peer->Hold(ep->callId) : ep->poolRef->peer->Resume(ep->callId);
    }
    return hold ? ep->s->Hold() : ep->s->Resume();
}

bool Worker::epRefer(Endpoint* from, Endpoint* to) {
    // Refer-To 사용자부 = 전달 대상 신원(psip 이 상대 Contact host 로 URI 를 만든다 — B2BUA(CSP)가 종단·재라우팅)
    if (from->isPeer()) return !from->callId.empty() && from->poolRef->peer->Refer(from->callId, to->id.user);
    if (from->s->m_strInviteId.empty()) return false;
    from->s->BlindTransfer(to->id.user);
    return true;
}

void Worker::epSetMediaMode(Endpoint* ep, int mode) {
    // UE 는 세션의 RTP 스레드에 — 다음 Start(새 호)부터 적용. 피어는 호마다(StartCall 인자 / INCOMING 이벤트)
    if (!ep->isPeer()) ep->s->SetMediaMode(mode);
}

bool Worker::epMediaSend(Endpoint* ep, const CompiledStep& st) {
    std::string amrwb, pcmu, pcma;
    bool def = st.sample.empty();
    if (!def) {
        auto& m = m_run->samples[st.sample];
        amrwb = m.count("amr-wb") ? m["amr-wb"] : "";
        pcmu = m.count("pcmu") ? m["pcmu"] : "";
        pcma = m.count("pcma") ? m["pcma"] : "";
    }
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->MediaSend(ep->callId, def, amrwb, pcmu, pcma, st.loop);
    return ep->s->MediaSend(def, amrwb, pcmu, pcma, st.loop);
}

bool Worker::epMediaStop(Endpoint* ep) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->MediaStop(ep->callId);
    return ep->s->MediaStop();
}

bool Worker::epDtmf(Endpoint* ep, const std::string& digits) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->SendDtmf(ep->callId, digits);
    return ep->s->SendDtmf(digits);
}

void Worker::sampleDtmf(Endpoint* ep) {
    int sent = 0, recv = 0, pt = -1;
    std::string digits;
    if (ep->isPeer()) {
        if (ep->callId.empty() || !ep->poolRef->peer->DtmfStats(ep->callId, sent, recv, digits, pt)) return;
    } else {
        pt = ep->s->m_clsRtpThread.m_iDtmfPt;
        sent = ep->s->m_clsRtpThread.m_iDtmfSent.load();
        recv = ep->s->m_clsRtpThread.m_iDtmfRecv.load();
        digits = ep->s->m_clsRtpThread.DtmfRecv();
    }
    if (sent > 0 || recv > 0 || pt >= 0)
        logf("debug", "dtmf sample %s(%s): te_pt=%d sent=%d recv=%d digits=%s", roleOf(ep->inst, ep).c_str(), ep->id.user.c_str(), pt, sent, recv, digits.c_str());
    if (sent > 0) m_metrics.counter("dtmf_sent", sent);   // RTP 로 실제 나간 이벤트 수(단계 카운터 dtmf_tx 와 대조)
    if (recv > 0) m_metrics.counter("dtmf_rx", recv);
    if (!ep->isPeer()) ep->s->m_clsRtpThread.ResetDtmf();
}

std::string Worker::callerRole(Instance& in) {
    for (auto& a : in.actors) if (a.second->tStartCallMs > 0) return a.first;
    return "";
}

bool Worker::epHasCall(Endpoint* ep) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->HasCall(ep->callId);
    return !ep->s->m_strInviteId.empty();
}

int Worker::epAnswer(Endpoint* ep) {
    if (ep->isPeer()) return ep->callId.empty() ? 481 : ep->poolRef->peer->Answer(ep->callId);
    return ep->s->AnswerCall() ? 0 : 481;
}

bool Worker::epReject(Endpoint* ep, int code, int cause) {
    if (ep->isPeer()) {
        if (ep->callId.empty()) return false;
        return ep->poolRef->peer->Reject(ep->callId, code, cause);
    }
    // UE 시뮬레이터의 거절은 Reason 없이(실 단말 거절 코드만) — cause 는 피어 프로파일(MGCF Q.850) 몫
    return ep->s->RejectCall(code);
}

bool Worker::epBye(Endpoint* ep, int cause) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->Bye(ep->callId, cause);
    ep->s->StopCall();
    return true;
}

void Worker::epClearCall(Endpoint* ep) {
    if (!ep->isPeer() || ep->callId.empty()) return;
    ep->poolRef->byCall.erase(ep->callId);
    ep->callId.clear();
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
        if (ep->isPeer()) {
            // 트렁크 REGISTER — 풀 단위로 한 번(계정 하나가 신원 범위를 대표). 결과는 OnPeerRegister → 풀 신원 전부 registered
            Pool* pool = ep->poolRef;
            ep->started = true;
            if (!pool->regStarted) {
                pool->regStarted = true;
                pool->regFailed = false;
                if (!pool->peer->Register()) { pool->regFailed = true; m_metrics.counter("registered_fail"); emitEvent("trunk REGISTER not sent", ep, "register", 0); }
            }
            continue;
        }
        startEndpoint(ep);
        if (m_preludeCursor % 100 == 0) logf("info", "prelude: %zu/%zu started", m_preludeCursor, m_preludeList.size());
    }
    if (m_preludeCursor < m_preludeList.size()) return;
    size_t reg = 0, pending = 0;
    for (auto* ep : m_preludeList) { if (ep->registered) reg++; else if (ep->started) pending++; }
    bool timeout = now >= m_preludeDeadlineMs;
    if (pending > 0 && !timeout) {
        // 실패 응답(REGISTER 4xx) 은 registered=false 이면서 started=true — 이벤트 카운터로 판정 불가하므로
        // 등록 통계(iRegFail) 를 본다.
        size_t failed = 0;
        for (auto* ep : m_preludeList)
            if (ep->started && !ep->registered && (ep->isPeer() ? ep->poolRef->regFailed : ep->s->m_stats.iRegFail > 0)) failed++;
        if (failed < pending) return;
    }
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
            if (in.pending == Instance::MEDIA) {
                // media_send/media_stop 의 after_ms 가 지났다 — 같은 단계를 다시 실행(이번엔 동작)
                in.phase = Instance::RUNNING;
                execStep(in, now);
                continue;
            }
            if (in.pending == Instance::PROGRESS) {
                // 183 early media(피어 UAS) — 발신자의 1xx 도달(RING 이벤트)까지 기다린다: CSP 가 183/SDP 를 전달하는지가 시험 대상
                Endpoint* ep = in.actors[in.pendingRole];
                int rc = epProgress(ep);
                in.pending = Instance::NONE;
                if (rc != 0) {
                    m_metrics.counter("codes." + std::to_string(rc));
                    emitEvent("183 refused", ep, "progress", rc, ep->callId);
                    finishInstance(in, true, "progress " + std::to_string(rc), now);
                    continue;
                }
                m_metrics.counter("progress_tx");
                in.progressTx = true;
                std::string caller = callerRole(in);
                if (!caller.empty()) {
                    in.phase = Instance::WAIT_EVENT;
                    in.awaitKind = "ring:" + caller;
                    in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                    continue;
                }
                advance(in, now);
                continue;
            }
            if (in.pending == Instance::ANSWER || in.pending == Instance::REJECT) {
                Endpoint* ep = in.actors[in.pendingRole];
                bool ok;
                if (in.pending == Instance::ANSWER) {
                    int rc = epAnswer(ep);
                    ok = rc == 0;
                    if (!ok && rc != 481) {
                        // 착신 측이 응답을 못 냈다 (488 = 공통 코덱 없음 — IP-PBX G.711 ↔ AMR-WB, cmp.md §11)
                        m_metrics.counter("codes." + std::to_string(rc));
                        emitEvent("answer refused", ep, "answer", rc, ep->callId);
                        finishInstance(in, true, "answer " + std::to_string(rc), now);
                        continue;
                    }
                } else {
                    ok = epReject(ep, in.pendingCode, in.pendingCause);
                    if (ok && in.pendingCause > 0) m_metrics.counter("q850_tx");
                }
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
            // 역할에 쓸 수 있는 단말이 하나도 없으면(등록 전부 실패 등) 발생을 계속해도 skipped 만 쌓인다 — run 을 닫는다
            bool anyActive = false;
            for (auto& i : m_instances) if (i->phase != Instance::DONE) { anyActive = true; break; }
            if (v.empty() && !anyActive) {
                emitEvent("no usable endpoint for role " + kv.first + " — run closed", nullptr, "", 0);
                logf("warn", "run %s: role %s has no usable endpoint — closing", m_run->runId.c_str(), kv.first.c_str());
                endRun("stopped");
            }
            return;
        }
        actors[kv.first] = pick;
    }
    // RTP 동시 상한(Media.MaxRtpStreams) — 넘으면 이 슬롯은 건너뛴다(단말 부족과 같은 skipped, 사유 카운터 따로)
    if (m_cfg.maxRtpStreams > 0 && m_bodyRtpMode != CRtpThread::E_MEDIA_NONE &&
        rtpStreams() + (long long)actors.size() > m_cfg.maxRtpStreams) {
        for (auto& a : actors) m_free[a.first].push_back(a.second);
        m_metrics.counter("skipped");
        m_metrics.counter("skipped_rtp_cap");
        return;
    }
    auto in = std::make_unique<Instance>();
    in->id = m_nextInstanceId++;
    in->actors = actors;
    in->tStartMs = now;
    in->rtpMode = m_bodyRtpMode;
    for (auto& a : actors) {
        a.second->inst = in.get();
        a.second->tStartCallMs = 0;
        epSetMediaMode(a.second, in->rtpMode);
        if (a.second->s) a.second->s->m_clsRtpThread.ResetRecvStats();
    }
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
            from->tStartCallMs = now;
            in.expectCode = (int)st.expect["code"].asInt(0);
            in.rtpMode = rtpModeOf(st.media);   // 이 호의 미디어 평면 — 인스턴스의 모든 단말(전달 대상 포함)에 같은 모드
            for (auto& a : in.actors) epSetMediaMode(a.second, in.rtpMode);
            if (!epStartCall(from, to, st.media)) { finishInstance(in, true, "invite: StartCall refused (busy/stack?)", now); return; }
            m_metrics.counter("legs", 2);
            bool calleeActs = in.stepIdx + 1 < m_body.size() &&
                              (m_body[in.stepIdx + 1].step == "reject" || m_body[in.stepIdx + 1].step == "answer" ||
                               m_body[in.stepIdx + 1].step == "progress");
            if (in.expectCode >= 300 && !calleeActs) {
                // 거절이 기대값(ACL 403·라우팅 reject 등 대상이 스스로 거절) — 발신자의 최종 응답을 여기서 기다린다.
                //   다음 단계가 착신 측 reject(피어 MGCF 503 등)면 그 단계가 거절을 내고 최종 응답을 기다린다.
                in.stepIdx++;
                in.phase = Instance::WAIT_EVENT;
                in.awaitKind = "callend:" + st.from;
                in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                return;
            }
            in.stepIdx++;   // 비동기 — 확립은 answer/media_hold 가 기다린다
            continue;
        }
        if (st.step == "answer" || st.step == "reject" || st.step == "progress") {
            std::string role = st.who.empty() ? st.to : st.who[0];
            Endpoint* ep = in.actors[role];
            if (!ep) { finishInstance(in, true, st.step + ": role missing", now); return; }
            in.pending = st.step == "answer" ? Instance::ANSWER : st.step == "reject" ? Instance::REJECT : Instance::PROGRESS;
            in.pendingRole = role;
            in.pendingCode = st.step == "reject" ? (int)st.expect["code"].asInt(486) : 0;
            in.pendingCause = st.cause;
            if (st.step == "reject" && st.payload.size()) in.pendingCode = atoi(st.payload.c_str());
            if (st.step == "progress" && ep->inCall) { in.stepIdx++; continue; }   // 이미 확립 — 183 은 의미 없음
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
            in.mediaHeld = true;
            // 대기가 끝나면 advance → RTP 표본은 bye 단계 진입 시 뜬다
            return;
        }
        if (st.step == "media_send" || st.step == "media_stop") {
            // 송출 제어 — 그 역할이 SDP 를 주고받은 뒤(183+SDP 또는 200)에만. after_ms 는 실행 전 지연(WAIT_TIME 뒤 다시 이 단계로)
            if (in.rtpMode == CRtpThread::E_MEDIA_NONE) { finishInstance(in, true, st.step + ": media.rtp=none 인 호", now); return; }
            if (st.afterMs > 0 && in.pending != Instance::MEDIA) {
                in.pending = Instance::MEDIA;
                in.phase = Instance::WAIT_TIME;
                in.waitUntilMs = now + st.afterMs;
                return;
            }
            in.pending = Instance::NONE;
            std::vector<std::string> roles = st.who;
            if (roles.empty() && !st.from.empty()) roles.push_back(st.from);
            for (auto& role : roles) {
                Endpoint* ep = in.actors[role];
                if (!ep) { finishInstance(in, true, st.step + ": role missing", now); return; }
                bool ok = st.step == "media_send" ? epMediaSend(ep, st) : epMediaStop(ep);
                if (!ok) {
                    emitEvent(st.step + ": no media session (SDP 미교환)", ep, st.step, 0, ep->callId);
                    finishInstance(in, true, st.step + ": " + role + " has no media session", now);
                    return;
                }
                m_metrics.counter(st.step == "media_send" ? "media_send" : "media_stop");
            }
            in.stepIdx++;
            continue;
        }
        if (st.step == "wait") {
            in.phase = Instance::WAIT_TIME;
            in.waitUntilMs = now + (long long)st.seconds * 1000;
            in.pending = Instance::NONE;
            return;
        }
        if (st.step == "hold" || st.step == "resume") {
            std::string role = st.who.empty() ? st.from : st.who[0];
            Endpoint* ep = in.actors[role];
            if (!ep) { finishInstance(in, true, st.step + ": role missing", now); return; }
            if (!ep->inCall) {
                // 확립 전이면 발신자 확립을 기다렸다가 다시 이 단계로
                std::string caller = callerRole(in);
                if (!caller.empty() && !in.actors[caller]->inCall) {
                    in.phase = Instance::WAIT_EVENT; in.awaitKind = "callstart:" + caller; in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                    in.stepIdx--;
                    return;
                }
                finishInstance(in, true, st.step + ": not in call", now); return;
            }
            if (!epHold(ep, st.step == "hold")) { finishInstance(in, true, st.step + ": re-INVITE not sent", now); return; }
            m_metrics.counter(st.step == "hold" ? "hold_tx" : "resume_tx");
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "reinviteresp:" + role;
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "dtmf") {
            std::string role = st.from.empty() ? (st.who.empty() ? "" : st.who[0]) : st.from;
            Endpoint* ep = in.actors[role];
            if (!ep) { finishInstance(in, true, "dtmf: role missing", now); return; }
            if (!ep->inCall) {
                std::string caller = callerRole(in);
                if (!caller.empty() && !in.actors[caller]->inCall) {
                    in.phase = Instance::WAIT_EVENT; in.awaitKind = "callstart:" + caller; in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                    in.stepIdx--;
                    return;
                }
                finishInstance(in, true, "dtmf: not in call", now); return;
            }
            if (st.payload.empty()) { finishInstance(in, true, "dtmf: payload(digits) required", now); return; }
            if (!epDtmf(ep, st.payload)) {
                m_metrics.counter("dtmf_unsupported");
                emitEvent("DTMF not negotiated (telephone-event)", ep, "dtmf", 0, ep->callId);
                finishInstance(in, true, "dtmf: telephone-event not negotiated", now); return;
            }
            m_metrics.counter("dtmf_tx", (long long)st.payload.size());
            // 숫자열이 다 나갈 때까지(이벤트 길이+간격) 기다린 뒤 다음 단계 — 수신 수는 bye 에서 표본
            in.phase = Instance::WAIT_TIME;
            in.waitUntilMs = now + (long long)st.payload.size() * (m_cfg.dtmfDigitMs + m_cfg.dtmfGapMs + 60) + 300;
            in.pending = Instance::NONE;
            return;
        }
        if (st.step == "refer") {
            Endpoint* from = in.actors[st.from];
            Endpoint* to = st.to.empty() ? nullptr : in.actors[st.to];
            if (!from || !to) { finishInstance(in, true, "refer: from/to role required", now); return; }
            if (!from->inCall) {
                std::string caller = callerRole(in);
                if (!caller.empty() && !in.actors[caller]->inCall) {
                    in.phase = Instance::WAIT_EVENT; in.awaitKind = "callstart:" + caller; in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                    in.stepIdx--;
                    return;
                }
                finishInstance(in, true, "refer: not in call", now); return;
            }
            in.expectCode = (int)st.expect["code"].asInt(202);
            if (!epRefer(from, to)) { finishInstance(in, true, "refer: REFER not sent", now); return; }
            m_metrics.counter("refer_tx");
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "referresp:" + st.from;
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "bye") {
            // media_hold 직후라면 RTP 품질 표본. DTMF 수신 수는 항상 표본(단계 dtmf 가 있었을 때만 값이 있다)
            for (auto& a : in.actors) sampleDtmf(a.second);   // RTP 표본이 카운터를 리셋하므로 먼저
            if (in.mediaHeld && in.rtpMode != CRtpThread::E_MEDIA_NONE) {
                for (auto& a : in.actors) sampleRtp(a.second);
                in.mediaHeld = false;
            }
            Endpoint* from = in.actors[st.from];
            if (!from) { finishInstance(in, true, "bye: role missing", now); return; }
            if (!from->inCall || !epHasCall(from)) {
                // 이미 끝난 호(상대 종료·실패) — BYE 없이 통과
                in.stepIdx++;
                continue;
            }
            epBye(from, st.cause);
            if (st.cause > 0) m_metrics.counter("q850_tx");
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
    unsigned long long rx = 0, lost = 0;
    long long jitterUs = 0;
    if (ep->isPeer()) {
        if (ep->callId.empty() || !ep->poolRef->peer->RtpStats(ep->callId, rx, lost, jitterUs)) return;
    } else {
        CRtpThread& rt = ep->s->m_clsRtpThread;
        rx = rt.m_ullRecvTotal.load(); lost = rt.m_ullRecvLost.load(); jitterUs = rt.m_llRecvJitterUs.load();
    }
    unsigned long long tx = ep->isPeer() ? ep->poolRef->peer->RtpSent(ep->callId) : ep->s->m_clsRtpThread.m_ullSentTotal.load();
    logf("debug", "rtp sample %s(%s): tx=%llu rx=%llu lost=%llu jitter_us=%lld", roleOf(ep->inst, ep).c_str(), ep->id.user.c_str(), tx, rx, lost, jitterUs);
    m_metrics.counter("rtp_tx", (long long)tx);
    m_metrics.counter("rtp_rx", (long long)rx);
    m_metrics.counter("rtp_lost", (long long)lost);
    if (rx + lost > 0) {
        m_metrics.timer("rtp_loss_pct", 100.0 * (double)lost / (double)(rx + lost));
        m_metrics.timer("jitter_ms", (double)jitterUs / 1000.0);
    } else {
        m_metrics.counter("rtp_silent_legs");
    }
    if (ep->isPeer()) ep->poolRef->peer->ResetRtpStats(ep->callId);
    else ep->s->m_clsRtpThread.ResetRecvStats();
}

void Worker::releaseEndpoint(Endpoint* ep) {
    if (ep->pendingInvite) { epReject(ep, 480); ep->pendingInvite = false; }
    else if (ep->inCall || epHasCall(ep)) epBye(ep);
    ep->inCall = false;
    ep->tStartCallMs = 0;
    if (ep->isPeer()) epClearCall(ep);
    else ep->s->SetMediaMode(CRtpThread::E_MEDIA_AUTO);
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
    // epilogue: deregister 단계가 있으면 그 역할의 단말을 내린다 (피어 신원은 해당 없음)
    for (auto& st : m_epilogue) {
        if (st.step != "deregister") continue;
        for (auto& role : st.who) {
            auto rit = m_run->roles.find(role);
            if (rit == m_run->roles.end()) continue;
            Pool* pool = m_pools[rit->second].get();
            auto sl = m_run->slices[role];
            for (int i = sl.first; i < sl.second && i < (int)pool->eps.size(); ++i) {
                Endpoint* ep = pool->eps[i].get();
                if (!ep->s || !ep->started) continue;
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
