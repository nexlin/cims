#include "Worker.h"
#include "EModel.h"

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
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

static std::string dtmfModeOf(const Json& v);   // 아래 정의 — 풀 dtmf 값(bool|rfc4733|inband|off) → 모드
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

long long Worker::nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
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
    CsimPeer::EnsureCodecTable();   // G.722(PT 9) — psip 기본 테이블에 없다(시나리오 media.audio: g722·피어 codecs G722)
    if (!m_http.start(m_cfg.bindIp, m_cfg.port, [this](const HttpRequest& r) { return handle(r); }, err)) return false;
    m_stop = false;
    m_sipCapture.install(SipCapture::ParseMode(m_cfg.sipCapture));
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
        for (auto& r : kv.second->reals) r->stop();
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
void Worker::OnAffiliate(SimSession* s, const std::string& group, int st, long long ms, bool deaff) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::AFFILIATE, s, nullptr, st, ms, "", group, deaff });
}
void Worker::OnSdsResponse(SimSession* s, const std::string& msgId, int st, long long ms) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::SDS_RESP, s, nullptr, st, ms, "", msgId, false });
}
void Worker::OnSdsRecv(SimSession* s, const std::string& from, const std::string& msgId, const std::string& group, const std::string& /*text*/, int dispReq) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::SDS_RECV, s, nullptr, dispReq, 0, from, msgId, false, false, 0, 0, group });
}
void Worker::OnSdsNotification(SimSession* s, const std::string& msgId, int notifType) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::SDS_NOTIF, s, nullptr, notifType, 0, "", msgId, false });
}
void Worker::OnSubscribeResponse(SimSession* s, const std::string& event, const std::string& resource, int st) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::SUBSCRIBE_RESP, s, nullptr, st, 0, "", resource, false, false, 0, 0, event });
}
void Worker::OnDialogNotify(SimSession* s, const std::string& watched, const std::string& state, const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::DLG_NOTIFY, s, nullptr, 0, 0, callId, watched, false, false, 0, 0, state });
}
void Worker::OnCallAnswered(SimSession* s, const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::ANSWERED, s, nullptr, 200, 0, callId, "", false });
}
void Worker::OnFloor(SimSession* s, int subtype, long long tUs) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::FLOOR, s, nullptr, subtype, 0, "", "", false, false, 0, tUs });
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
void Worker::OnPeerFaultReject(CsimPeer* p, const std::string& callId, const std::string& toUser, int iCode) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::FAULT_REJECT, nullptr, p, iCode, 0, callId, toUser, false });
}
void Worker::OnPeerWireDrop(CsimPeer* p, const std::string& callId, const std::string& method) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::WIRE_DROP, nullptr, p, 0, 0, callId, method, false });
}
void Worker::OnPeerInviteRetrans(CsimPeer* p, const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::INVITE_RETRANS, nullptr, p, 0, 0, callId, "", false });
}
void Worker::OnPeerThig(CsimPeer* p, const std::string& callId, bool bOk) {
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back({ Event::THIG, nullptr, p, bOk ? 1 : 0, 0, callId, "", false });
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
        long long reg = 0, started = 0, aff = 0;
        for (auto& ep : kv.second->eps) { if (ep->started) started++; if (ep->registered) reg++; if (ep->affiliated) aff++; }
        if (kv.second->peer) active += (long long)kv.second->peer->CallCount();
        active += started;
        Json pj = Json::Object();
        pj["pool"] = Json(kv.first);
        pj["kind"] = Json(kv.second->kind);
        pj["endpoints"] = Json((long long)kv.second->eps.size());
        pj["registered"] = Json(reg);
        if (kv.second->service == "ptt") {
            pj["service"] = Json(kv.second->service);
            pj["affiliated"] = Json(aff);
            pj["groups"] = Json((long long)kv.second->groups.size());
        }
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
    // 실단말 풀(§3.3) — 프로세스 수·상한·cimsue-cli 경로(컨트롤러 계획 미리보기가 검산)
    Json real = Json::Object();
    real["processes"] = Json(realProcesses());
    real["max"] = Json((long long)m_cfg.realUeMax);
    real["cli"] = Json(m_cfg.realUeCli);
    j["real_ue"] = real;
    // TLS 파일 보유(§3.1·§3.2) — 컨트롤러 계획 미리보기가 풀의 tls_verify/tls_client_cert/tls_client_auth·TLS 피어 bind 와 대조한다
    Json tls = Json::Object();
    tls["ca"] = Json(!m_cfg.tlsCaFile.empty() && access(m_cfg.tlsCaFile.c_str(), R_OK) == 0);
    tls["client_cert"] = Json(!m_cfg.tlsClientCertFile.empty() && access(m_cfg.tlsClientCertFile.c_str(), R_OK) == 0);
    tls["peer_cert"] = Json(!m_cfg.peerCertFile.empty() && access(m_cfg.peerCertFile.c_str(), R_OK) == 0);
    j["tls"] = tls;
    // NAT 풀(§3.1) — CAP_SYS_ADMIN 보유·이 호스트의 netns 목록(scripts/nat-netns.sh 가 만든 것) — 계획 미리보기가 풀 nat.netns 와 대조
    Json nat = Json::Object();
    nat["capable"] = Json(hasCapSysAdmin());
    Json nss = Json::Array();
    if (DIR* dp = opendir(m_cfg.natNetnsDir.c_str())) {
        std::vector<std::string> names;
        while (struct dirent* de = readdir(dp)) if (de->d_name[0] != '.') names.push_back(de->d_name);
        closedir(dp);
        std::sort(names.begin(), names.end());
        for (auto& n : names) nss.push(Json(n));
    }
    nat["netns"] = nss;
    j["nat"] = nat;
    return jsonResp(200, j);
}

long long Worker::realProcesses() const {
    long long n = 0;
    for (auto& kv : m_pools) for (auto& r : kv.second->reals) if (r->alive()) n++;
    return n;
}

void Worker::destroyPool(Pool* pool) {
    for (auto& ep : pool->eps) {
        if (ep->s) { m_bySession.erase(ep->s); if (ep->started) ep->s->Stop(5); delete ep->s; ep->s = nullptr; }
    }
    if (pool->natFd >= 0) { close(pool->natFd); pool->natFd = -1; }
    if (pool->peer) { m_byPeer.erase(pool->peer.get()); pool->peer->Stop(); pool->peer.reset(); }
    if (!pool->reals.empty()) {
        for (auto& r : pool->reals) r->stop();
        pool->reals.clear();
        // 리더 스레드가 남긴 이 풀 단말의 이벤트(process_exit 등)는 버린다 — Endpoint 가 곧 사라진다
        std::lock_guard<std::mutex> lk(m_evMtx);
        m_events.erase(std::remove_if(m_events.begin(), m_events.end(), [pool](const Event& e) { return e.ep && e.ep->poolRef == pool; }), m_events.end());
        for (auto& ep : pool->eps) ep->real = nullptr;
    }
}

bool Worker::buildUePool(Pool* pool, const Json& d, std::string& err) {
    const Json& tc = d["target_csp"];
    pool->targetIp = tc["ip"].asString();
    pool->targetPort = (int)(pool->transport == "tls" ? tc["tls"].asInt(5061)
                             : pool->transport == "tcp" ? tc["tcp"].asInt(25061) : tc["udp"].asInt(5060));
    if (pool->targetIp.empty()) { err = "target_csp.ip_required"; return false; }
    pool->service = d["service"].asString("volte");
    const bool ptt = pool->service == "ptt";
    // TLS 서버 검증·클라이언트 인증서(§3.1) — 풀이 켜면 워커의 Tls.* 파일이 있어야 한다
    const bool tlsVerify = d["tls_verify"].asBool(false), tlsClientCert = d["tls_client_cert"].asBool(false);
    if (!tlsRequirements(pool->transport == "tls" && tlsVerify, pool->transport == "tls" && tlsClientCert, err)) return false;
    // NAT 풀(§3.1 nat) — netns 파일을 열어 두고 단말 로컬 IP 를 netns 안 주소로. 파일이 없거나 권한이 없으면 400
    std::string localIp = m_cfg.localIp;
    if (d["nat"].isObject()) {
        pool->natNs = d["nat"]["netns"].asString();
        pool->natLocalIp = d["nat"]["local_ip"].asString();
        if (pool->natNs.empty() || pool->natLocalIp.empty()) { err = "nat.netns/local_ip required"; return false; }
        if (!hasCapSysAdmin()) { err = "nat_cap_missing: 워커에 CAP_SYS_ADMIN 이 없다(cims-priv setcap-sys-admin <bin>) — NAT(netns) 풀 불가"; return false; }
        std::string path = m_cfg.natNetnsDir + "/" + pool->natNs;
        pool->natFd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (pool->natFd < 0) { err = "nat_netns_missing: " + path + " — " + strerror(errno) + " (scripts/nat-netns.sh create " + pool->natNs + ")"; return false; }
        localIp = pool->natLocalIp;
    }
    const Json& ids = d["identities"];
    for (size_t i = 0; i < ids.size(); ++i) {
        const Json& x = ids.at(i);
        auto ep = std::make_unique<Endpoint>();
        ep->idx = (int)i;
        ep->pool = pool->name;
        ep->poolRef = pool;
        ep->id.user = x["user"].asString();
        ep->id.pttGroup = x["ptt_group"].asString();
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
                               pool->targetIp, pool->targetPort, localIp, localPort, ptt, ep->id.pttGroup);
        // MCPTT 단말 — xcap-diff NOTIFY 는 받되 XCAP 문서 GET(IdMS 토큰 필요)은 하지 않는다. 착신은 libcsim 이 자동응답(automatic commencement)
        if (ptt) { ep->s->SetNoXcap(true); if (!ep->id.pttGroup.empty()) pool->groups[ep->id.pttGroup].push_back(ep.get()); }
        if (pool->transport == "tls") ep->s->SetTransport(E_SIP_TLS);
        else if (pool->transport == "tcp") ep->s->SetTransport(E_SIP_TCP);
        ep->s->SetSrtpMode(pool->srtp == "required" ? 2 : pool->srtp == "optional" ? 1 : 0);
        if (!ep->id.akaK.empty()) ep->s->SetAka(ep->id.akaK, ep->id.akaOpc, 0);
        ep->s->SetAnswerMode(SimSession::E_ANSWER_DEFERRED);
        ep->s->SetPrack(d["prack"].asBool(false));
        { std::string dm = dtmfModeOf(d["dtmf"]); ep->s->SetDtmf(dm != "off"); ep->s->SetDtmfInband(dm == "inband"); }
        if (pool->transport == "tls" && (tlsVerify || tlsClientCert))
            ep->s->SetTls(tlsVerify, m_cfg.tlsCaFile, tlsClientCert ? m_cfg.tlsClientCertFile : "", tlsClientCert ? m_cfg.tlsClientKeyFile : "");
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
    {
        // 착신 정책(오류 주입) — silent: 무응답 · reject: 엔진이 fault.code(+Reason Q.850)로 즉시 거절 · delay: fault.delay_ms 보류 뒤 시나리오가 응답
        std::string ans = pd["answer"].asString("normal");
        pc.silent = ans == "silent";
        if (ans == "reject") { pc.rejectCode = (int)pd["fault"]["code"].asInt(503); pc.rejectQ850 = (int)pd["fault"]["q850"].asInt(0); }
        if (ans == "delay") pool->answerDelayMs = (int)pd["fault"]["delay_ms"].asInt(0);
        // 와이어 유실(answer 정책과 독립) — 새 착신 INVITE 첫 N 벌 / 임의 메시지 p %
        pc.dropInvite = (int)pd["fault"]["drop_invite"].asInt(0);
        pc.dropPct = (int)pd["fault"]["drop_pct"].asInt(0);
    }
    pc.certFile = m_cfg.peerCertFile;
    pc.keyFile = m_cfg.peerKeyFile;
    // TLS 상호인증(§3.2) — 수신점의 클라이언트 인증서 요구 / 발신 연결의 서버 검증·클라이언트 인증서 제시. 파일은 워커 Tls.*
    pc.tlsClientAuth = pd["tls_client_auth"].asBool(false);
    pc.tlsVerifyServer = pd["tls_verify"].asBool(false);
    const bool peerClientCert = pd["tls_client_cert"].asBool(false);
    if (pc.transport == E_SIP_TLS && pc.certFile.empty()) { err = "tls_peer_cert_missing: 피어 TLS 수신점에 Tls.PeerCertFile 이 필요하다"; return false; }
    if (!tlsRequirements(pc.tlsVerifyServer || pc.tlsClientAuth, peerClientCert, err)) return false;
    pc.caCertFile = m_cfg.tlsCaFile;
    if (peerClientCert) { pc.clientCertFile = m_cfg.tlsClientCertFile; pc.clientKeyFile = m_cfg.tlsClientKeyFile; }
    pc.thig = pd["thig"].asBool(false) && pc.profile == "ibcf";
    pc.prack = pd["prack"].isBool() ? pd["prack"].asBool() : CsimPeerConfig::DefaultPrack(pc.profile);
    { std::string dm = dtmfModeOf(pd["dtmf"]); pc.dtmf = dm != "off"; pc.dtmfInband = dm == "inband"; }
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
    pool->targetDomain = tc["domain_volte"].asString(pc.domain);
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
        ep->kind = Endpoint::K_PEER;
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

/** real-ue 풀(§3.3) — 신원마다 `cimsue-cli … drive` 프로세스를 띄우고 ready 를 기다린다. 계정 인자는 cspsim creds 규약과 같다
 *  (IMPI = auth_id(없으면 user)@domain, H(A1) 또는 비밀번호). AKA 신원은 cli 가 받지 않아 거절. TLS 는 풀 tls_verify 가 켜져 있고 워커에
 *  RealUe.TlsCaFile 이 있으면 그 앵커로 검증, 아니면 검증 없이 접속(--no-tls-verify). */
bool Worker::buildRealUePool(Pool* pool, const Json& d, std::string& err) {
    if (m_cfg.realUeCli.empty() || access(m_cfg.realUeCli.c_str(), X_OK) != 0) { err = "real_ue_cli_missing: " + (m_cfg.realUeCli.empty() ? std::string("RealUe.CliPath 비어 있음") : m_cfg.realUeCli); return false; }
    const Json& tc = d["target_csp"];
    pool->targetIp = tc["ip"].asString();
    pool->targetPort = (int)(pool->transport == "tls" ? tc["tls"].asInt(5061)
                             : pool->transport == "tcp" ? tc["tcp"].asInt(25061) : tc["udp"].asInt(5060));
    if (pool->targetIp.empty()) { err = "target_csp.ip_required"; return false; }
    pool->service = d["service"].asString("volte");
    const bool tlsVerify = d["tls_verify"].asBool(false);
    const Json& ids = d["identities"];
    long long others = realProcesses();
    if (others + (long long)ids.size() > m_cfg.realUeMax) { err = "real_ue_limit: " + std::to_string(others + (long long)ids.size()) + " > RealUe.MaxProcesses " + std::to_string(m_cfg.realUeMax); return false; }
    if (!m_cfg.realUeLogDir.empty()) mkdir(m_cfg.realUeLogDir.c_str(), 0755);
    for (size_t i = 0; i < ids.size(); ++i) {
        const Json& x = ids.at(i);
        auto ep = std::make_unique<Endpoint>();
        ep->kind = Endpoint::K_REAL;
        ep->idx = (int)i;
        ep->pool = pool->name;
        ep->poolRef = pool;
        ep->id.user = x["user"].asString();
        ep->id.domain = x["domain"].asString();
        ep->id.ha1 = x["ha1"].asString();
        ep->id.password = x["password"].asString();
        ep->id.display = x["display"].asString();
        ep->id.pttGroup = x["ptt_group"].asString();
        ep->id.authScheme = x["auth_scheme"].asString("digest");
        if (ep->id.user.empty() || ep->id.domain.empty()) { err = "identity_user_domain_required"; return false; }
        if (ep->id.authScheme == "aka") { err = "real_ue_aka_unsupported: " + ep->id.user + " — cimsue-cli 는 Digest 계정만"; return false; }
        if (ep->id.ha1.empty() && ep->id.password.empty()) { err = "real_ue_secret_required: " + ep->id.user; return false; }
        std::string authId = x["auth_id"].asString();
        if (authId.empty()) authId = ep->id.user;
        if (authId.find('@') == std::string::npos) authId += "@" + ep->id.domain;
        std::vector<std::string> argv = { m_cfg.realUeCli, "--server", pool->targetIp, "--port", std::to_string(pool->targetPort),
                                          "--transport", pool->transport, "--domain", ep->id.domain, "--msisdn", ep->id.user, "--auth-id", authId,
                                          "--srtp", pool->srtp, "--log-level", std::to_string(m_cfg.realUeLogLevel) };
        if (!ep->id.ha1.empty()) { argv.push_back("--ha1"); argv.push_back(ep->id.ha1); } else { argv.push_back("--password"); argv.push_back(ep->id.password); }
        if (!ep->id.display.empty()) { argv.push_back("--display-name"); argv.push_back(ep->id.display); }
        if (pool->service == "ptt") { argv.push_back("--mcptt-id"); argv.push_back("tel:" + ep->id.user); }
        if (tlsVerify && !m_cfg.realUeTlsCaFile.empty()) { argv.push_back("--tls-ca"); argv.push_back(m_cfg.realUeTlsCaFile); }
        else argv.push_back("--no-tls-verify");
        argv.push_back("drive");
        Endpoint* raw = ep.get();
        auto proc = std::make_unique<RealUeProcess>(pool->name + "/" + ep->id.user, [this, raw](const Json& ev) { onRealEvent(raw, ev); });
        std::string logFile = m_cfg.realUeLogDir.empty() ? "" : m_cfg.realUeLogDir + "/" + pool->name + "-" + ep->id.user + ".log";
        std::string perr;
        if (!proc->start(argv, logFile, perr)) { err = "real_ue_spawn_failed: " + ep->id.user + " — " + perr; return false; }
        ep->real = proc.get();
        if (pool->service == "ptt" && !ep->id.pttGroup.empty()) pool->groups[ep->id.pttGroup].push_back(ep.get());
        pool->reals.push_back(std::move(proc));
        pool->eps.push_back(std::move(ep));
    }
    // 전부 ready(엔진 기동) 까지 — 하나라도 못 뜨면 풀 생성 실패(프로세스는 destroyPool 이 내린다)
    for (auto& ep : pool->eps) {
        if (!ep->real->waitReady(m_cfg.realUeStartTimeoutS * 1000)) {
            err = "real_ue_start_timeout: " + ep->id.user + (ep->real->alive() ? " (ready 없음)" : " (프로세스 종료 — log/real-ue 의 로그 확인)");
            return false;
        }
    }
    return true;
}

/** 실단말 프로세스 이벤트(리더 스레드) → Event. 큐에만 넣는다 — onEvent 가 스케줄러 스레드에서 가상 단말과 같은 종류로 다시 푼다. */
void Worker::onRealEvent(Endpoint* ep, const Json& ev) {
    const std::string kind = ev["event"].asString();
    Event e{};
    e.ep = ep;
    e.s = nullptr;
    e.peer = nullptr;
    auto stats = [&](Event& x) {
        if (!ev["stats_valid"].asBool(false)) return;
        x.statsValid = true;
        x.rx = (unsigned long long)ev["rx_pkts"].asInt(0);
        x.tx = (unsigned long long)ev["tx_pkts"].asInt(0);
        x.lost = (unsigned long long)ev["rx_loss"].asInt(0);
        x.jit = ev["jitter_us"].asInt(0);
    };
    if (kind == "reg") {
        std::string st = ev["state"].asString();
        if (st != "registered" && st != "failed") return;
        e.kind = Event::REGISTER;
        e.status = st == "registered" ? 200 : (int)(ev["code"].asInt(0) > 0 ? ev["code"].asInt(0) : 500);
        e.ms = ev["rrd_ms"].asInt(0);
    } else if (kind == "incoming") {
        if (ev["mcptt"].asBool(false)) return;   // 그룹 fan-out INVITE — 실스택이 자동응답(automatic commencement), call active 가 ANSWERED
        e.kind = Event::INCOMING;
        e.rcall = (int)ev["call"].asInt(-1);
        e.hasPai = true;
    } else if (kind == "call") {
        e.kind = Event::REAL_CALL;
        e.event = ev["state"].asString();
        e.user = ev["dir"].asString("out");
        e.status = (int)ev["code"].asInt(0);
        e.hasPai = ev["by_us"].asBool(false);
        e.prack = ev["mcptt"].asBool(false);
        e.rcall = (int)ev["call"].asInt(-1);
        e.ms = ev.has("srd_ms") ? ev["srd_ms"].asInt(0) : ev.has("sdd_ms") ? ev["sdd_ms"].asInt(0) : 0;
        stats(e);
    } else if (kind == "floor") {
        int sub = (int)ev["subtype"].asInt(-1);
        if (sub < 0) return;
        e.kind = Event::FLOOR;
        e.status = sub;
        e.us = ev["t_us"].asInt(0);
    } else if (kind == "request") {
        if (ev["op"].asString() != "affiliate") return;
        e.kind = Event::AFFILIATE;
        e.status = (int)ev["code"].asInt(0);
        e.ms = ev["ms"].asInt(0);
        e.hasPai = !ev["on"].asBool(true);   // 해제 명령의 응답
    } else if (kind == "stats") {
        e.kind = Event::REAL_STATS;
        e.rcall = (int)ev["call"].asInt(-1);
        stats(e);
        if (!e.statsValid) return;
    } else if (kind == "exit" || kind == "process_exit" || kind == "engine_stopped") {
        e.kind = Event::REAL_EXIT;
        e.status = (int)ev["status"].asInt(0);
        e.event = ev["error"].asString();
    } else return;
    std::lock_guard<std::mutex> lk(m_evMtx);
    m_events.push_back(e);
}

Json Worker::realRequest(Endpoint* ep, const std::string& cmd) {
    if (!ep->real) { Json j = Json::Object(); j["ok"] = Json(false); j["reason"] = Json("no process"); return j; }
    return ep->real->request(cmd, m_cfg.realUeCmdTimeoutMs);
}

HttpResponse Worker::poolCreate(const Json& d) {
    std::string name = d["pool"].asString();
    std::string kind = d["kind"].asString("ue");
    if (name.empty()) return errResp(400, "pool_required");
    if (kind != "ue" && kind != "peer" && kind != "real-ue") return errResp(400, "unsupported_kind", kind);
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
    bool ok = kind == "ue" ? buildUePool(pool.get(), d, err) : kind == "peer" ? buildPeerPool(pool.get(), d, err) : buildRealUePool(pool.get(), d, err);
    if (!ok) { destroyPool(pool.get()); return errResp(400, err); }
    logf("info", "pool %s created — kind=%s endpoints=%zu transport=%s srtp=%s target=%s:%d%s",
         name.c_str(), kind.c_str(), pool->eps.size(), pool->transport.c_str(), pool->srtp.c_str(),
         pool->targetIp.c_str(), pool->targetPort, pool->peer ? (" profile=" + pool->profile).c_str() : "");
    Json j = Json::Object();
    j["pool"] = Json(name);
    j["endpoints"] = Json((long long)pool->eps.size());
    if (pool->peer) j["bind"] = Json(pool->peer->Config().bindIp + ":" + std::to_string(pool->peer->Config().port));
    if (!pool->reals.empty()) j["processes"] = Json((long long)pool->reals.size());
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
                                    "media_send", "media_stop", "group_call", "floor_request", "floor_release",
                                    "pickup", "subscribe", "replaces", "join", "publish", "sds_send", "sds_recv" };

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
        if (in->phase != Instance::DONE && in->rtpMode != CRtpThread::E_MEDIA_NONE) {
            n += (long long)in->actors.size();
            for (auto& m : in->multi) n += (long long)m.second.size();
        }
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
    for (size_t i = 0; i < d["multi_roles"].size(); ++i) spec->multiRoles.push_back(d["multi_roles"].at(i).asString());
    for (size_t i = 0; i < d["guest_roles"].size(); ++i) spec->guestRoles.push_back(d["guest_roles"].at(i).asString());
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
        cs.disposition = s["disposition"].asBool(false);
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
        return errResp(400, "unsupported_step", list + " — 워커는 register/invite/answer/reject/bye/media_hold/wait/expect/progress/hold/resume/dtmf/refer/media_send/media_stop/group_call/floor_request/floor_release/pickup/subscribe/replaces/join/publish/sds_send/sds_recv 만");
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
    for (auto& st : m_body) if (st.step == "invite" || st.step == "group_call") { m_bodyRtpMode = rtpModeOf(st.media); break; }
    // 그룹 단위 인스턴스(body 에 group_call) — 역할은 모두 같은 PTT 풀이어야 하고, 멤버 배정 순서 = 발신자 → body 등장 순 단일 역할 → multi 역할(나머지)
    m_groupBound = false;
    m_groupPool.clear(); m_singleRoles.clear(); m_usedMulti.clear(); m_groupNames.clear(); m_guestRoles.clear();
    m_groupCursor = 0;
    // 그룹 SDS(sds_send 에 to 없음 = 그룹 대상)도 그룹 단위 인스턴스 — 수신자는 multi 역할(그룹의 나머지 멤버)
    for (auto& st : m_body) if (st.step == "group_call" || (st.step == "sds_send" && st.to.empty())) { m_groupBound = true; break; }
    if (m_groupBound) {
        m_guestRoles = spec->guestRoles;
        auto isMulti = [&](const std::string& r) { return std::find(spec->multiRoles.begin(), spec->multiRoles.end(), r) != spec->multiRoles.end(); };
        auto note = [&](const std::string& r) {
            if (r.empty() || isGuestRole(r)) return;   // 그룹 밖 역할은 그룹 멤버 배정에서 뺀다
            auto& v = isMulti(r) ? m_usedMulti : m_singleRoles;
            if (std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
        };
        for (auto& st : m_body) if (st.step == "group_call") {
            if (st.from.empty() || isMulti(st.from)) return errResp(400, "group_call_from", "group_call.from 은 단일 역할이어야 한다");
            if (isGuestRole(st.from)) return errResp(400, "group_call_from", "첫 group_call.from 은 그룹 멤버 역할이어야 한다(그룹 밖 역할은 그 뒤 listen/거절)");
            note(st.from); break;
        }
        for (auto& st : m_body) { note(st.from); note(st.to); for (auto& w : st.who) note(w); }
        if (m_usedMulti.size() > 1) return errResp(400, "multi_roles", "multi 역할은 시나리오에 하나만(그룹의 나머지 멤버)");
        for (auto& r : m_singleRoles) { if (!spec->roles.count(r)) return errResp(400, "unknown_role", r); }
        for (auto& r : m_usedMulti) { if (!spec->roles.count(r)) return errResp(400, "unknown_role", r); }
        m_groupPool = spec->roles[m_singleRoles[0]];
        for (auto& r : m_singleRoles) if (spec->roles[r] != m_groupPool) return errResp(400, "group_pool", "그룹콜 시나리오의 멤버 역할은 모두 같은 풀이어야 한다");
        for (auto& r : m_usedMulti) if (spec->roles[r] != m_groupPool) return errResp(400, "group_pool", "그룹콜 시나리오의 멤버 역할은 모두 같은 풀이어야 한다");
        Pool* gp = m_pools[m_groupPool].get();
        if (gp->service != "ptt") return errResp(400, "group_pool_service", m_groupPool + " — group_call 은 service=ptt 풀에서만");
        if (gp->groups.empty()) return errResp(400, "group_pool_groups", m_groupPool + " — 신원에 ptt_group 이 없다");
        for (auto& g : gp->groups) m_groupNames.push_back(g.first);
        for (auto& r : m_guestRoles) {
            if (!spec->roles.count(r)) return errResp(400, "unknown_role", r);
            Pool* qp = m_pools[spec->roles[r]].get();
            if (qp->service != "ptt") return errResp(400, "guest_pool_service", r + " — 그룹 밖 역할도 service=ptt 풀(비멤버 PTT 단말)이어야 한다");
        }
    } else {
        for (auto& st : m_body)
            if (st.step == "floor_request" || st.step == "floor_release")
                return errResp(400, "floor_without_group_call", "floor 단계는 group_call 뒤에만 둔다");
    }

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
    m_sipShipped = 0;
    m_sipPending.clear();
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
            flushSipPending(now, false);
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
    if (in) {
        for (auto& a : in->actors) if (a.second == ep) return a.first;
        for (auto& m : in->multi) for (auto* x : m.second) if (x == ep) return m.first;
    }
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

void Worker::noteCallId(Instance* in, const std::string& callId) {
    if (!in || callId.empty()) return;
    for (auto& c : in->callIds) if (c == callId) return;
    in->callIds.push_back(callId);
}

/** 끝난 인스턴스의 SIP 덤프 — due 가 된 것(all 이면 전부)을 스트림의 `sip` 레코드로 올리거나 버린다 */
void Worker::flushSipPending(long long now, bool all) {
    while (!m_sipPending.empty() && (all || m_sipPending.front().dueMs <= now)) {
        SipPending p = std::move(m_sipPending.front());
        m_sipPending.pop_front();
        for (auto& callId : p.callIds) {
            if (!p.ship || !m_run || m_sipShipped >= m_cfg.sipDumpMax) { m_sipCapture.drop(callId); continue; }
            std::vector<SipCapturedMessage> msgs = m_sipCapture.take(callId);
            if (msgs.empty()) continue;
            Json j = Json::Object();
            j["kind"] = Json("sip");
            j["t"] = Json(nowMs() / 1000.0);
            j["run_id"] = Json(m_run->runId);
            j["worker"] = Json(m_cfg.name);
            j["call_id"] = Json(callId);
            j["instance"] = Json(p.instance);
            Json arr = Json::Array();
            for (auto& m : msgs) {
                Json mj = Json::Object();
                mj["t"] = Json(m.t);
                mj["dir"] = Json(m.tx ? "tx" : "rx");
                mj["transport"] = Json(m.transport);
                mj["peer"] = Json(m.peer);
                mj["text"] = Json(m.text);
                arr.push(mj);
            }
            j["messages"] = arr;
            m_stream.send(j.dump());
            m_sipShipped++;
            m_metrics.counter("sip_dumps");
        }
    }
}

void Worker::onEvent(const Event& e) {
    Endpoint* ep = nullptr;
    Pool* peerPool = nullptr;
    if (e.ep) {
        ep = e.ep;
        if (e.statsValid) ep->realStats = { e.rx, e.tx, e.lost, e.jit, true };
        if (e.kind == Event::REAL_STATS) return;
        if (e.kind == Event::REAL_EXIT) {
            // 프로세스가 죽었다 — 단말은 쓸 수 없다(등록 상태 내림). 인스턴스 중이면 실패
            bool wasUp = ep->started || ep->registered;
            ep->started = false; ep->registered = false; ep->realRegFailed = true; ep->realCall = -1;
            ep->affStarted = ep->affiliated = false;
            if (wasUp) { m_metrics.counter("real_ue_exit"); emitEvent("real-ue process exited" + (e.event.empty() ? "" : " — " + e.event), ep, "", e.status); }
            if (ep->inst && ep->inst->phase != Instance::DONE) finishInstance(*ep->inst, true, "real-ue process exited", nowMs());
            return;
        }
        if (e.kind == Event::INCOMING) ep->realCall = e.rcall;
        if (e.kind == Event::REAL_CALL) {
            // 실단말 호 상태 → 가상 단말과 같은 이벤트로. active(out) = 확립(SRD) · active(in, mcptt) = fan-out 자동응답 합류 ·
            //   held / held→active = 우리 hold/resume 의 re-INVITE 200 · disconnected = by_us 면 BYE 응답(SDD), 아니면 상대 종료/최종 응답
            Event x = e;
            x.ep = ep;
            const std::string& st = e.event;
            if (st == "active") {
                if (ep->realHeld) { ep->realHeld = false; if (ep->realReinviteWait) { ep->realReinviteWait = false; x.kind = Event::REINVITE_RESP; x.status = 200; onEvent(x); } return; }
                if (e.user == "out") { x.kind = Event::CALLSTART; x.status = 200; onEvent(x); }
                else if (e.prack) { x.kind = Event::ANSWERED; x.status = 200; onEvent(x); }
                return;   // 착신 1:1 확립 — epAnswer 가 이미 inCall 로 두었다
            }
            if (st == "held") {
                ep->realHeld = true;
                if (ep->realReinviteWait) { ep->realReinviteWait = false; x.kind = Event::REINVITE_RESP; x.status = 200; onEvent(x); }
                return;
            }
            if (st == "disconnected") {
                if (ep->realCall == e.rcall) ep->realCall = -1;
                ep->realHeld = false;
                ep->realReinviteWait = false;
                if (e.hasPai) { x.kind = Event::BYERESP; x.status = 200; onEvent(x); }
                else { x.kind = Event::CALLEND; x.status = e.status > 0 ? e.status : 200; onEvent(x); }
                return;
            }
            return;   // outgoing/incoming/null — 관측만
        }
    } else if (e.s) {
        ep = endpointOf(e.s);
    } else if (e.peer) {
        auto pit = m_byPeer.find(e.peer);
        if (pit == m_byPeer.end()) return;
        peerPool = pit->second;
        if (e.kind == Event::FAULT_REJECT) {
            // 오류 주입(answer=reject) — 엔진이 착신을 즉시 거절했다. 인스턴스 밖의 일(대상이 이 피어를 골랐다) — 세고 event 로 남긴다
            m_metrics.counter("peer_fault_reject");
            m_metrics.counter("peer_fault_codes." + std::to_string(e.status));
            auto uit = peerPool->byUser.find(e.user);
            emitEvent("peer fault: INVITE rejected " + std::to_string(e.status) + " (answer=reject)", uit == peerPool->byUser.end() ? nullptr : uit->second, "invite", e.status, e.callId);
            return;
        }
        if (e.kind == Event::WIRE_DROP) {
            // 오류 주입(fault.drop_*) — 와이어 유실처럼 버린 벌. 재전송이 닿으면 INVITE_RETRANS 가 뒤따른다(retrans_rx_pct 의 분자/분모)
            m_metrics.counter("peer_fault_drop");
            m_metrics.counter("peer_fault_drop_methods." + e.user);
            return;
        }
        if (e.kind == Event::INVITE_RETRANS) { m_metrics.counter("invite_retrans_rx"); return; }
        if (e.kind == Event::THIG) {
            // THIG 흔적 — 발신 INVITE 에 얹은 토큰화 Via 가 응답에 보존됐는가(thig_pct = ok / thig_tx)
            m_metrics.counter(e.status ? "thig_via_ok" : "thig_via_lost");
            if (!e.status) { auto cit = peerPool->byCall.find(e.callId); emitEvent("THIG tokenized Via lost in response", cit == peerPool->byCall.end() ? nullptr : cit->second, "invite", 0, e.callId); }
            return;
        }
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
    noteCallId(in, e.callId);
    switch (e.kind) {
    case Event::REGISTER:
        ep->registered = (e.status == 200);
        if (e.status == 200) { m_metrics.counter("registered_ok"); m_metrics.timer("rrd_ms", (double)e.ms); startPtt(ep); }
        else { m_metrics.counter("registered_fail"); m_metrics.counter("codes." + std::to_string(e.status)); emitEvent("REGISTER failed", ep, "register", e.status); }
        break;
    case Event::INCOMING:
        if (ep->isPtt()) break;   // 그룹 fan-out INVITE — libcsim 이 자동응답(automatic commencement)하고 ANSWERED 로 알린다
        ep->pendingInvite = true;
        ep->holdUntilMs = 0;
        if (ep->isPeer() && ep->poolRef->answerDelayMs > 0) {
            // 오류 주입 answer=delay — 보류 시한. answer/progress/reject 단계의 after_ms 와 합쳐 늦은 쪽에 응답한다
            ep->holdUntilMs = now + ep->poolRef->answerDelayMs;
            m_metrics.counter("peer_fault_delay");
        }
        if (in && in->forkDial && ep->tStartCallMs == 0) {
            // 대표번호 포크 leg(TS 24.239) — 그룹원 alert. 승자 외의 leg 는 서버가 CANCEL 한다(487 = 정상, ringing_leg_cancelled).
            //   P-Called-Party-ID 가 다이얼한 대표번호를 실었는가(dispatch_center.md §4 — 착신 표시)
            ep->cancelExpected = true;
            m_metrics.counter("fork_rx");
            if (ep->isSim()) {
                const std::string& pcp = ep->s->m_strLastPCalledParty;
                if (!pcp.empty() && pcp.find(in->dialTarget) != std::string::npos) m_metrics.counter("pcpid_ok");
                else if (!pcp.empty()) emitEvent("P-Called-Party-ID " + pcp + " != dialed " + in->dialTarget, ep, "invite", 0, e.callId);
            }
        }
        // 피어 착신 호는 엔진이 INVITE 수신 때 만든다 — Progress/Answer 전에 인스턴스의 RTP 모드를 입힌다
        if (in && ep->isPeer()) ep->poolRef->peer->SetMediaMode(e.callId, in->rtpMode);
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "incoming:" + roleOf(in, ep)) {
            // answer/reject 단계가 착신을 기다리고 있었다 — after_ms(와 피어 보류 시한 중 늦은 쪽) 뒤 응답
            in->phase = Instance::WAIT_TIME;
            in->waitUntilMs = std::max(now + m_body[in->stepIdx].afterMs, ep->holdUntilMs);
        }
        if (!in) {
            // 인스턴스 밖의 착신(예: 시나리오에 없는 상대) — 486 로 거절해 스택을 비운다
            epReject(ep, 486);
            ep->pendingInvite = false;
            m_metrics.counter("unexpected_invite");
        }
        break;
    case Event::CALLSTART:
        ep->outPending = false;
        ep->pendingInvite = false;
        if (!ep->consultCallId.empty() && e.callId == ep->consultCallId) ep->inConsult = true;   // 상담 통화(두 번째 다이얼로그) 확립
        else ep->inCall = true;
        // 세션(SER 분자)은 1:1 호의 발신 INVITE 확립만 — 상담 호·픽업·Replaces·Join 은 같은 시도 안의 부가 다이얼로그라 <kind>_ok 로 따로 센다
        if (!ep->outKind.empty() && ep->outKind != "invite") { m_metrics.counter(ep->outKind + "_ok"); if (ep->outKind == "join") ep->joined = true; }
        else m_metrics.counter("sessions");
        m_metrics.counter("seer_ok");   // RFC 6076 §4.4 SEER 분자 — 200 (거절 480/486/600/603 은 CALLEND 에서)
        if (ep->isSim() && ep->s->m_clsRtpThread.m_bVideoOffer && ep->s->m_clsRtpThread.m_iDestVideoPort > 0) m_metrics.counter("video_ok");   // answer 의 활성 m=video
        m_metrics.timer("srd_ms", (double)e.ms);
        if (ep->isReal()) { m_metrics.counter("real_legs"); m_metrics.timer("real_srd_ms", (double)e.ms); }   // 실단말 표본은 따로도 남긴다(§3.3)
        if (in && in->progressTx && in->rtpMode != CRtpThread::E_MEDIA_NONE) {
            // early media 의 미디어 평면 — 183+SDP 뒤 200 전까지 발신자가 실제로 RTP 를 받았는가(시그널링 early_media 와 별개).
            //   이벤트 처리 지연(≤ 스케줄러 틱) 동안 200 뒤 패킷이 한둘 섞일 수 있어 5 패킷(100 ms) 이상을 도달로 본다.
            unsigned long long rx = 0, lost = 0;
            long long jit = 0;
            if (ep->isPeer()) ep->poolRef->peer->RtpStats(e.callId, rx, lost, jit);
            else if (ep->isReal()) rx = ep->realStats.rx;
            else rx = ep->s->m_clsRtpThread.m_ullRecvTotal.load();
            m_metrics.counter("early_rtp_rx", (long long)rx);
            if (rx >= 5) m_metrics.counter("early_rtp_ok");
            else emitEvent("early media RTP not received before 200 (rx=" + std::to_string(rx) + ")", ep, "progress", 183, e.callId);
            in->progressTx = false;
        }
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind.rfind("callstart:", 0) == 0) {
            std::string role = in->awaitKind.substr(10);
            if (in->actors[role] == ep) advance(*in, now);
        } else if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "groupup") {
            checkGroupUp(*in, now);
        }
        break;
    case Event::ANSWERED:
        // PTT 멤버가 그룹 fan-out INVITE 에 자동응답했다 — 세션 합류(leg 하나)
        ep->inCall = true;
        if (ep->isReal()) ep->realCall = e.rcall;
        m_metrics.counter("legs");
        m_metrics.counter("group_joined");
        if (ep->isReal()) m_metrics.counter("real_legs");
        if (!in) {
            // 인스턴스 밖의 그룹 세션(이 워커가 연 것이 아니다) — 자리를 비운다
            epBye(ep);
            ep->inCall = false;
            m_metrics.counter("unexpected_invite");
            break;
        }
        in->tLastJoinMs = now;
        if (in->phase == Instance::WAIT_EVENT && in->awaitKind == "groupup") checkGroupUp(*in, now);
        break;
    case Event::AFFILIATE:
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "pubresp" &&
            std::find(in->respWait.begin(), in->respWait.end(), ep) != in->respWait.end()) {
            // publish 단계(affiliation 명령 PUBLISH, TS 24.379 §9)의 최종 응답 — 기대 코드와 다르면 인스턴스 실패
            m_metrics.counter("publish_codes." + std::to_string(e.status));
            if (e.status / 100 == 2) { ep->affiliated = !e.hasPai; if (!e.hasPai) m_metrics.timer("affiliate_ms", (double)e.ms); }
            int want = in->expectCode > 0 ? in->expectCode : 200;
            if (e.status != want) {
                emitEvent(std::string(e.hasPai ? "de-affiliate" : "affiliate") + " PUBLISH expected " + std::to_string(want) + " got " + std::to_string(e.status), ep, "publish", e.status);
                finishInstance(*in, true, "publish " + std::to_string(e.status), now);
                break;
            }
            auto& rw = in->respWait;
            rw.erase(std::remove(rw.begin(), rw.end(), ep), rw.end());
            if (rw.empty()) advance(*in, now);
            break;
        }
        if (e.hasPai) { if (e.status / 100 == 2) ep->affiliated = false; break; }   // 로그아웃의 de-affiliate 응답 — 준비 상태만 내린다
        if (e.status / 100 == 2) {
            ep->affiliated = true;
            m_metrics.counter("affiliated_ok");
            m_metrics.timer("affiliate_ms", (double)e.ms);
        } else {
            ep->affFailed = true;
            m_metrics.counter("affiliated_fail");
            m_metrics.counter("codes." + std::to_string(e.status));
            emitEvent("affiliation PUBLISH refused (group " + ep->id.pttGroup + ")", ep, "register", e.status);
        }
        break;
    case Event::FLOOR:
        onFloor(ep, in, e.status, e.us, now);
        break;
    case Event::CALLEND: {
        if (!ep->consultCallId.empty() && e.callId == ep->consultCallId) {
            // 상담 통화(두 번째 다이얼로그) 종료 — 전달 완결 뒤 서버 BYE 는 정상, 상담 INVITE 의 실패 최종 응답은 인스턴스 실패. 첫 통화 상태는 그대로
            bool pending = ep->outPending && ep->outKind == "consult";
            ep->consultCallId.clear();
            ep->inConsult = false;
            if (pending) ep->outPending = false;
            if (e.status >= 300 && in && in->phase != Instance::DONE) {
                m_metrics.counter("codes." + std::to_string(e.status));
                emitEvent("consultation call failed", ep, "invite", e.status, e.callId);
                finishInstance(*in, true, "consult final " + std::to_string(e.status), now);
            }
            break;
        }
        if (ep->cancelExpected && ep->pendingInvite && ep->tStartCallMs == 0) {
            // 픽업·Replaces 로 다른 단말이 가져간 링잉 착신 leg 를 서버가 CANCEL 했다(487) — 정상 경로, 인스턴스 실패가 아니다
            ep->cancelExpected = false;
            ep->pendingInvite = false;
            ep->inCall = false;
            m_metrics.counter("ringing_leg_cancelled");
            break;
        }
        if (ep->outPending && e.status >= 300) {
            // 자기 INVITE 의 실패 최종 응답 — RFC 6076 §4.4 SEER(사용자 측 거절은 유효 시도) · §4.6 ISA(망 측 실패·Timer B)
            if (e.status == 480 || e.status == 486 || e.status == 600 || e.status == 603) m_metrics.counter("seer_ok");
            if (e.status == 408 || e.status == 500 || e.status == 503 || e.status == 504) m_metrics.counter("isa_fail");
        }
        ep->inCall = false;
        ep->pendingInvite = false;
        ep->outPending = false;
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
        } else if (in->phase == Instance::WAIT_EVENT && in->awaitKind == "byeresp") {
            // bye 단계가 이 단말의 BYE 응답을 기다리는 중에 그 leg 가 끝났다(서버가 먼저 닫음 — 그룹 세션 해제) — 더 기다리지 않는다
            auto& bw = in->byeWait;
            size_t before = bw.size();
            bw.erase(std::remove(bw.begin(), bw.end(), ep), bw.end());
            if (before > 0 && bw.empty()) advance(*in, now);
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
    case Event::SDS_RESP:
        // sds_send 의 MESSAGE 최종 응답 — 기대 코드(기본 200)와 다르면 인스턴스 실패. 응답이 오면 다음 단계(sds_recv 가 수신을 기다린다)
        m_metrics.counter("sds_codes." + std::to_string(e.status));
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "sdsresp" && e.user == in->sdsMsgId) {
            int want = in->expectCode > 0 ? in->expectCode : 200;
            if (e.status != want) {
                emitEvent("SDS MESSAGE expected " + std::to_string(want) + " got " + std::to_string(e.status), ep, "sds_send", e.status);
                finishInstance(*in, true, "sds_send " + std::to_string(e.status), now);
                break;
            }
            advance(*in, now);
        }
        break;
    case Event::SDS_RECV: {
        // SDS 도착 — 단말이 받은 msgId 를 기억한다(sds_recv 단계가 나중에 진입해도 찾는다). 자기 인스턴스가 기다리는 msgId 면 지연 표본·대기 해제
        m_metrics.counter("sds_rx");
        if (ep->sdsRx.size() > 64) ep->sdsRx.erase(ep->sdsRx.begin());
        ep->sdsRx.push_back(e.user);
        if (in && e.user == in->sdsMsgId && in->sdsSendMs > 0) m_metrics.timer("sds_delay_ms", (double)(now - in->sdsSendMs));
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "sdsrecv" && e.user == in->sdsMsgId) {
            auto& w = in->sdsWait;
            w.erase(std::remove(w.begin(), w.end(), ep), w.end());
            if (w.empty()) advance(*in, now);
        }
        break;
    }
    case Event::SDS_NOTIF:
        // 자기 SDS 의 SDS NOTIFICATION(delivered) — disposition 회신율의 분자(sds_disposition_pct)
        m_metrics.counter("sds_notif_rx");
        if (e.status == 2 && in && e.user == in->sdsMsgId && in->sdsDisposition) m_metrics.counter("sds_disposition_rx");
        break;
    case Event::SUBSCRIBE_RESP:
        m_metrics.counter("subscribe_codes." + std::to_string(e.status));
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "subresp" &&
            std::find(in->respWait.begin(), in->respWait.end(), ep) != in->respWait.end()) {
            int want = in->expectCode > 0 ? in->expectCode : 200;
            if (e.status != want) {
                emitEvent("SUBSCRIBE Event:" + e.event + " expected " + std::to_string(want) + " got " + std::to_string(e.status), ep, "subscribe", e.status);
                finishInstance(*in, true, "subscribe " + std::to_string(e.status), now);
                break;
            }
            auto& rw = in->respWait;
            rw.erase(std::remove(rw.begin(), rw.end(), ep), rw.end());
            if (rw.empty()) advance(*in, now);
        }
        break;
    case Event::DLG_NOTIFY:
        // dialog 이벤트 NOTIFY(RFC 4235) — 감시 대상의 dialog 가 early/confirmed 가 되면 replaces/join 단계가 그 Call-ID 로 INVITE 를 낸다
        m_metrics.counter("notify_rx");
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "dialog:" + roleOf(in, ep) &&
            (e.event == "early" || e.event == "confirmed")) {
            in->phase = Instance::RUNNING;
            in->awaitKind.clear();
            execStep(*in, now);   // 같은 단계를 다시 — 이번엔 학습된 다이얼로그로 진행
        }
        break;
    case Event::BYERESP:
        ep->inCall = false;
        m_metrics.timer("sdd_ms", (double)e.ms);
        // 완료(SCR 분자)는 세션 단위 — bye 단계가 낸 BYE 만(정리 BYE 는 세지 않는다), 그룹 세션의 멤버 leg BYE 는 세지 않는다(발신자 leg 만)
        if (e.status / 100 != 2) m_metrics.counter("bye_fail");
        else if (ep->byeByStep && (!ep->isPtt() || ep->tStartCallMs > 0)) m_metrics.counter("completed");
        ep->byeByStep = false;
        if (ep->isPeer() && ep->callId == e.callId) epClearCall(ep);
        if (in && in->phase == Instance::WAIT_EVENT && in->awaitKind == "byeresp") {
            auto& bw = in->byeWait;
            bw.erase(std::remove(bw.begin(), bw.end(), ep), bw.end());
            if (bw.empty()) advance(*in, now);
        }
        break;
    default:
        break;   // FAULT_REJECT·REAL_* 는 위에서 풀어 처리했다
    }
}

// ── 단말 동작 (UE 세션 / 피어 신원) ────────────────────────────────────────
bool Worker::startEndpoint(Endpoint* ep) {
    if (ep->isPeer() || ep->started) return true;
    if (ep->isReal()) {
        // 실단말 — 프로세스에 register 명령. 결과(REGISTER 200/실패)는 reg 이벤트로 온다
        ep->realRegFailed = false;
        ep->realStats.valid = false;
        Json r = realRequest(ep, "register");
        if (!r["ok"].asBool(false)) {
            ep->realRegFailed = true;
            m_metrics.counter("registered_fail");
            emitEvent("real-ue register refused: " + r["reason"].asString(), ep, "register", 0);
            return false;
        }
        ep->started = true;
        return true;
    }
    ep->s->m_clsRtpThread.ResetRecvStats();
    bool ok;
    std::string nerr;
    if (ep->poolRef && ep->poolRef->natFd >= 0) ok = startInNetns(ep, nerr);   // NAT 풀 — 소켓을 netns 안에서
    else ok = ep->s->Start();
    if (!ok) {
        m_metrics.counter("registered_fail");
        emitEvent(nerr.empty() ? "stack start failed" : "stack start failed: " + nerr, ep, "register", 0);
        return false;
    }
    ep->started = true;
    return true;
}

bool Worker::startInNetns(Endpoint* ep, std::string& err) {
    // setns 는 부른 스레드만 옮긴다 — 스케줄러 스레드를 옮기지 않고 임시 스레드에서 Start() 한다. psip 스택·RTP 소켓은 Start() 안에서(부른 스레드에서)
    //   만들어지고, Start() 가 띄우는 수신 스레드들은 그 netns 를 물려받는다. 뒤의 Stop()/재Start 도 같은 경로.
    bool ok = false;
    std::string e;
    std::thread th([&] {
        if (setns(ep->poolRef->natFd, CLONE_NEWNET) != 0) { e = std::string("setns(") + ep->poolRef->natNs + "): " + strerror(errno) + " — 워커에 CAP_SYS_ADMIN 이 필요하다(cims-priv setcap-sys-admin)"; return; }
        ok = ep->s->Start();
        if (!ok) e = "stack start failed in netns " + ep->poolRef->natNs;
    });
    th.join();
    err = e;
    return ok;
}

bool Worker::hasCapSysAdmin() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return false;
    char line[256];
    unsigned long long eff = 0;
    while (fgets(line, sizeof(line), f)) if (sscanf(line, "CapEff: %llx", &eff) == 1) break;
    fclose(f);
    return (eff >> 21) & 1ULL;   // CAP_SYS_ADMIN = 21
}

/** 코덱 테이블 PT → rtpmap 이름 (미지 = 빈 문자열) — 수신 wire PT 로 MOS 코덱(E-model Ie/Bpl)을 고른다 */
static std::string codecNameOf(int pt) {
    if (pt < 0) return "";
    for (const auto& e : CSipCodecTable::GetList())
        if (e.m_iPt == pt) return e.m_strName;
    return pt == 0 ? "PCMU" : pt == 8 ? "PCMA" : pt == 18 ? "G729" : "";
}

/** 풀 dtmf 값 → 모드 문자열 — bool(true=rfc4733, false=off) 또는 "rfc4733"|"inband"|"off"(컨트롤러 DtmfMode) */
static std::string dtmfModeOf(const Json& v) {
    if (v.isBool()) return v.asBool() ? "rfc4733" : "off";
    std::string s = v.asString("rfc4733");
    return (s == "inband" || s == "off") ? s : "rfc4733";
}

/** TLS 옵션이 요구하는 워커 파일(Tls.CaFile / Tls.ClientCertFile) 확인 — 없으면 400 사유 */
bool Worker::tlsRequirements(bool needCa, bool needClientCert, std::string& err) const {
    if (needCa && (m_cfg.tlsCaFile.empty() || access(m_cfg.tlsCaFile.c_str(), R_OK) != 0)) { err = "tls_ca_missing: tls_verify/tls_client_auth 에는 워커 Tls.CaFile 이 필요하다"; return false; }
    if (needClientCert && (m_cfg.tlsClientCertFile.empty() || access(m_cfg.tlsClientCertFile.c_str(), R_OK) != 0)) { err = "tls_client_cert_missing: tls_client_cert 에는 워커 Tls.ClientCertFile 이 필요하다"; return false; }
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

bool Worker::epStartCall(Endpoint* from, Endpoint* to, const Json& media, const std::string& dial) {
    if (from->isPeer()) {
        Pool* pool = from->poolRef;
        // to 없으면 번호 리터럴(대표번호·DID) — 피어는 상대(대상) 도메인으로 부른다
        std::string callId = pool->peer->StartCall(from->id.user, to ? to->id.user : dial, to ? to->id.domain : pool->targetDomain,
                                                   pool->targetIp, pool->targetPort, parseTransport(pool->transport), rtpModeOf(media));
        if (callId.empty()) return false;
        from->callId = callId;
        pool->byCall[callId] = from;
        from->outPending = true;
        from->outKind = "invite";
        m_metrics.counter("invite_tx");
        if (pool->peer->Config().thig) m_metrics.counter("thig_tx");
        return true;
    }
    if (!from->started && !startEndpoint(from)) return false;
    if (from->isReal()) {
        // 실단말 — dial <번호|user@도메인>. 두 번째 다이얼로그(상담 통화)는 지원하지 않는다(컴파일 게이트). 오퍼 코덱은 실스택 것
        if (from->realCall >= 0 || from->inCall) return false;
        std::string target = to ? to->id.user : dial;
        if (to && to->isPeer() && to->poolRef->profile == "ibcf") target += "@" + to->id.domain;
        Json r = realRequest(from, "dial " + target + (from->realVideo ? " video" : ""));
        if (!r["ok"].asBool(false)) { emitEvent("real-ue dial refused: " + r["reason"].asString(), from, "invite", 0); return false; }
        from->realCall = (int)r["call"].asInt(-1);
        from->outPending = true;
        from->outKind = "invite";
        m_metrics.counter("invite_tx");
        return true;
    }
    // 시나리오가 오퍼 코덱을 지정하면(pcma/pcmu/amr-wb …) 그 코덱으로 — 트렁크 G.711 경로 시험. 없으면 풀 기본
    from->s->SetOfferCodec(codecPtOf(media["audio"].asString("")));
    // UE 가 피어 신원을 부를 때 — ibcf(타 IMS) 는 user@피어도메인(Request-URI host → req_uri_host 규칙), pbx/mgcf 는 번호 그대로
    //   (DID/E.164 — CSP 가 번호 prefix 규칙으로 트렁크를 고른다, 실 단말이 다이얼하는 꼴)
    std::string target = to ? to->id.user : dial;   // 역할 없는 번호 리터럴(대표번호) 은 실 단말이 다이얼하는 꼴 그대로
    if (to && to->isPeer() && to->poolRef->profile == "ibcf") target += "@" + to->id.domain;
    if (from->inCall && !from->s->m_strInviteId.empty()) {
        // 통화 중인 단말의 두 번째 INVITE = 상담 통화(consultation, 두 번째 다이얼로그) — attended transfer(RFC 3515 + Refer-To Replaces)의 전제.
        //   첫 통화는 유지된다(실 단말은 hold 하지만 계측기는 미디어 방향을 판정하지 않는다 — 전달 뒤 전달자는 빠진다)
        if (!from->consultCallId.empty()) return false;
        from->s->StartConsultCall(target);
        if (from->s->m_strConsultId.empty()) return false;
        from->consultCallId = from->s->m_strConsultId;
        from->outPending = true;
        from->outKind = "consult";
        m_metrics.counter("consult_tx");
        m_metrics.counter("invite_tx");
        return true;
    }
    from->s->StartCall(target);
    if (from->s->m_strInviteId.empty()) return false;
    from->outPending = true;
    from->outKind = "invite";
    m_metrics.counter("invite_tx");
    return true;
}

/** 픽업·Replaces·Join 발신(UE 세션만) — kind 별 INVITE 를 낸다. 실패(스택 거절·대상 다이얼로그 미학습) 는 false. */
static bool startSpecialCall(Endpoint* from, Endpoint* to, const std::string& kind, const std::string& payload, const std::string& dial = "") {
    SimSession* s = from->s;
    if (kind == "pickup") {
        // 피처코드 다이얼(volte_supplementary_services.md §5.2) — <code> 그룹 픽업 · <code><번호> 지정 픽업(번호 = 역할 신원 또는 리터럴 대표번호)
        s->StartCall(payload + (to ? to->id.user : dial));
    } else {
        // RFC 3891/3911 — dialog 이벤트(RFC 4235)로 학습한 대상 다이얼로그. 태그 방향: 우리 to-tag = 상대(remote)·from-tag = 대상(local)
        if (s->m_strWatchedDlgCallId.empty()) return false;
        if (kind == "replaces") s->StartCallWithReplaces(to->id.user, s->m_strWatchedDlgCallId, s->m_strWatchedDlgRemoteTag, s->m_strWatchedDlgLocalTag);
        else s->StartCallWithJoin(to->id.user, s->m_strWatchedDlgCallId, s->m_strWatchedDlgRemoteTag, s->m_strWatchedDlgLocalTag);
    }
    if (s->m_strInviteId.empty()) return false;
    from->outPending = true;
    from->outKind = kind;
    return true;
}

bool Worker::epSubscribe(Endpoint* ep, const std::string& event, const std::string& resource) {
    if (!ep->isSim() || !ep->started) return false;
    if (event == "dialog") { ep->s->SubscribeDialog(resource); ep->dlgWatching = true; }
    else { ep->s->SubscribeEvent(event, resource); ep->evtWatching = true; }
    return true;
}

void Worker::epUnsubscribe(Endpoint* ep) {
    if (!ep->isSim()) return;
    if (ep->dlgWatching) { ep->s->UnsubscribeDialogs(); ep->dlgWatching = false; }
    if (ep->evtWatching) { ep->s->UnsubscribeEvent(); ep->evtWatching = false; }
    ep->s->ClearWatchedDialog();
}

int Worker::epProgress(Endpoint* ep) {
    if (ep->isPeer()) return ep->callId.empty() ? 481 : ep->poolRef->peer->Progress(ep->callId);
    if (ep->isReal()) return 481;    // 실스택은 앱이 183 을 내지 않는다(컴파일 게이트 — REAL_UE_STEPS 밖)
    // UE 측 183 + SDP(deferred 착신 — 실 단말의 착신 안내음·early media 모사). 그 뒤 answer 는 같은 answer 로 200
    return ep->s->ProgressCall();
}

bool Worker::epHold(Endpoint* ep, bool hold) {
    if (ep->isPeer()) {
        if (ep->callId.empty()) return false;
        return hold ? ep->poolRef->peer->Hold(ep->callId) : ep->poolRef->peer->Resume(ep->callId);
    }
    if (ep->isReal()) {
        if (ep->realCall < 0) return false;
        ep->realReinviteWait = true;
        Json r = realRequest(ep, (hold ? "hold " : "resume ") + std::to_string(ep->realCall));
        if (!r["ok"].asBool(false)) { ep->realReinviteWait = false; return false; }
        return true;
    }
    return hold ? ep->s->Hold() : ep->s->Resume();
}

bool Worker::epRefer(Endpoint* from, Endpoint* to, bool attended) {
    // Refer-To 사용자부 = 전달 대상 신원(psip 이 상대 Contact host 로 URI 를 만든다 — B2BUA(CSP)가 종단·재라우팅)
    if (from->isPeer()) return !from->callId.empty() && from->poolRef->peer->Refer(from->callId, to->id.user);
    if (from->isReal()) return false;   // 실스택은 REFER 최종 응답을 이벤트로 내지 않는다 — 컴파일 게이트(REAL_UE_STEPS)가 막는다
    if (from->s->m_strInviteId.empty()) return false;
    if (attended) {
        // 전달자가 to 와 상담 통화 중 → Refer-To 에 상담 다이얼로그의 Replaces(RFC 3515 + RFC 3891 — TS 24.629 consultative ECT)
        if (!from->inConsult) return false;
        from->s->AttendedTransfer();
        m_metrics.counter("refer_attended_tx");
        return true;
    }
    from->s->BlindTransfer(to->id.user);
    return true;
}

/** 영상(invite/group_call 의 media.video) — h264 면 이 인스턴스 단말들의 오퍼에 m=video 를 싣는다(워커 Media.VideoFile 이 있어야 비디오 소켓이 있다).
 *  파일이 없으면 오디오만 나가고 video_unavailable 로 센다(조용히 넘기지 않는다). 발신자 기준 video_offered, answer 의 활성 m=video 는 CALLSTART 에서 video_ok. */
void Worker::epSetVideo(Instance& in, Endpoint* from, bool want) {
    for (auto* ep : endpointsOf(in)) { if (ep->isSim()) ep->s->m_clsRtpThread.m_bVideoOffer = want; else if (ep->isReal()) ep->realVideo = want; }
    if (!want || !from || !from->isSim()) return;   // 실단말의 영상은 실스택 빌드 몫(Linux pjproject 는 --disable-video) — 세지 않는다
    if (from->s->m_clsRtpThread.m_iVideoPort > 0) m_metrics.counter("video_offered");
    else { m_metrics.counter("video_unavailable"); emitEvent("media.video h264 requested but worker has no Media.VideoFile — audio only", from, "invite", 0); }
}

void Worker::epSetMediaMode(Endpoint* ep, int mode) {
    // UE 는 세션의 RTP 스레드에 — 다음 Start(새 호)부터 적용. 피어는 호마다(StartCall 인자 / INCOMING 이벤트)
    //   PTT 단말의 auto = floor 를 가진 동안만 송출(TS 24.380 — 허가 없이 미디어를 내지 않는다): 수신만 시작하고 Granted 에 송출, 해제·Revoke 에 정지
    if (!ep->isSim()) return;   // 실단말의 미디어 평면은 실스택 것(rtp 모드 없음 — 컴파일이 auto 만 허용)
    ep->s->SetMediaMode(ep->isPtt() && mode == CRtpThread::E_MEDIA_AUTO ? CRtpThread::E_MEDIA_EXPLICIT : mode);
}

void Worker::startPtt(Endpoint* ep) {
    // MCPTT 기동 절차(실 단말 순서) — GMS/CMS xcap-diff 구독 → 그룹 affiliation → conference 구독. prelude 는 affiliation 200 까지 기다린다
    if (!ep->isPtt() || ep->affStarted) return;
    ep->affStarted = true;
    if (ep->isReal()) {
        // 실단말 — affiliation PUBLISH 만(GMS/CMS·conference 구독은 실스택 앱 몫). 결과는 request 이벤트 → AFFILIATE
        if (ep->id.pttGroup.empty()) { ep->affiliated = true; return; }
        Json r = realRequest(ep, "affiliate " + ep->id.pttGroup + " on");
        if (!r["ok"].asBool(false)) { ep->affFailed = true; m_metrics.counter("affiliated_fail"); emitEvent("real-ue affiliate refused: " + r["reason"].asString(), ep, "register", 0); }
        return;
    }
    ep->s->SubscribeGms();
    ep->s->SubscribeCms();
    if (ep->id.pttGroup.empty()) { ep->affiliated = true; return; }   // 그룹 없는 PTT 신원 — 등록만(사설콜 등)
    ep->s->AffiliateGroup();
    ep->s->SubscribeConference(ep->id.pttGroup);
}

std::vector<Endpoint*> Worker::endpointsOf(Instance& in) {
    std::vector<Endpoint*> v;
    for (auto& a : in.actors) if (a.second) v.push_back(a.second);
    for (auto& m : in.multi) for (auto* ep : m.second) v.push_back(ep);
    return v;
}

std::vector<Endpoint*> Worker::roleEndpoints(Instance& in, const std::string& role) {
    auto mit = in.multi.find(role);
    if (mit != in.multi.end()) return mit->second;
    auto ait = in.actors.find(role);
    if (ait != in.actors.end() && ait->second) return { ait->second };
    return {};
}

void Worker::checkGroupUp(Instance& in, long long now) {
    // group_call 완료 = 발신자 확립(200) + to 역할 멤버 전원 합류(자동응답 200)
    std::string caller = callerRole(in);
    if (caller.empty() || !in.actors[caller]->inCall) return;
    if (!in.groupTo.empty()) {
        for (auto* ep : roleEndpoints(in, in.groupTo)) if (!ep->inCall) return;
        long long last = in.tLastJoinMs > 0 ? in.tLastJoinMs : now;
        m_metrics.timer("group_fanout_ms", (double)(last - in.tGroupCallMs));
    }
    advance(in, now);
}

void Worker::onFloor(Endpoint* ep, Instance* in, int subtype, long long us, long long now) {
    // TS 24.380 §8.2 subtype — 1 Granted · 2 Taken · 3 Deny · 5 Idle · 6 Revoke · 9 Queue Position Info
    switch (subtype) {
    case 1:
        if (ep->floor != Endpoint::F_REQUESTED && ep->floor != Endpoint::F_QUEUED) break;
        m_metrics.counter("floor_granted");
        m_metrics.timer(ep->wasQueued ? "floor_queue_ms" : "floor_grant_ms", (double)(us - ep->tFloorReqUs) / 1000.0);
        if (ep->wasQueued && in) in->tFloorReqUs = 0;   // 큐를 거친 허가의 Taken 은 요청 시각과 무관 — floor_taken_ms 표본에서 뺀다
        ep->floor = Endpoint::F_GRANTED;
        ep->talked = true;
        if (in && in->rtpMode != CRtpThread::E_MEDIA_NONE && ep->isSim()) ep->s->MediaSend(true, "", "", "", true);   // 실스택은 floor 가 마이크를 게이트한다
        break;
    case 3:
        if (ep->floor != Endpoint::F_REQUESTED) break;
        m_metrics.counter("floor_denied");
        ep->floor = Endpoint::F_DENIED;
        break;
    case 9:
        if (ep->floor != Endpoint::F_REQUESTED) break;
        m_metrics.counter("floor_queued");
        ep->floor = Endpoint::F_QUEUED;
        ep->wasQueued = true;
        break;
    case 6:
        if (ep->floor != Endpoint::F_GRANTED) break;
        m_metrics.counter("floor_revoked");
        if (ep->isSim()) ep->s->MediaStop();
        ep->floor = Endpoint::F_IDLE;
        break;
    case 2:
        m_metrics.counter("floor_taken_rx");
        ep->idleSeen = true;   // 해제 뒤 Idle 없이 다음 발언자가 곧바로 잡았다(큐) — 해제 대기는 이것으로 끝난다
        if (in && in->tFloorReqUs > 0 && !ep->takenSeen && ep->floor != Endpoint::F_GRANTED) {
            ep->takenSeen = true;
            m_metrics.timer("floor_taken_ms", (double)(us - in->tFloorReqUs) / 1000.0);
        }
        break;
    case 5:
        m_metrics.counter("floor_idle_rx");
        if (in && in->tFloorRelUs > 0 && !ep->idleSeen) m_metrics.timer("floor_idle_ms", (double)(us - in->tFloorRelUs) / 1000.0);
        ep->idleSeen = true;
        break;
    default:
        break;
    }
    if (in && in->phase == Instance::WAIT_EVENT && (in->awaitKind == "floor" || in->awaitKind == "floorrel")) checkFloorWait(*in, now);
}

void Worker::checkFloorWait(Instance& in, long long now) {
    if (in.awaitKind == "floorrel") {
        for (auto* ep : in.floorWait) if (!ep->idleSeen) return;
        in.floorWait.clear();
        advance(in, now);
        return;
    }
    // floor_request — 요청자 전원의 결과가 나왔는가. Queued 는 기대가 granted 가 아니거나 다른 요청자가 floor 를 잡았으면 결과로 본다
    //   (동시 요청에서 하나가 잡으면 나머지는 그가 놓을 때까지 큐에 머문다).
    bool anyGranted = false;
    for (auto* ep : in.floorWait) if (ep->floor == Endpoint::F_GRANTED) anyGranted = true;
    int hit = 0;
    for (auto* ep : in.floorWait) {
        if (ep->floor == Endpoint::F_REQUESTED) return;
        if (ep->floor == Endpoint::F_QUEUED && in.floorWant == "granted" && !anyGranted) return;
        if ((in.floorWant == "granted" && ep->floor == Endpoint::F_GRANTED) || (in.floorWant == "denied" && ep->floor == Endpoint::F_DENIED) ||
            (in.floorWant == "queued" && ep->floor == Endpoint::F_QUEUED) || in.floorWant == "any") hit++;
    }
    if (hit == 0) {
        Endpoint* ep = in.floorWait.empty() ? nullptr : in.floorWait[0];
        const char* got = !ep ? "none" : ep->floor == Endpoint::F_GRANTED ? "granted" : ep->floor == Endpoint::F_DENIED ? "denied" : "queued";
        emitEvent("floor request: expected " + in.floorWant + " got " + got, ep, "floor_request", 0);
        in.floorWait.clear();
        finishInstance(in, true, "floor " + std::string(got), now);
        return;
    }
    in.floorWait.clear();
    advance(in, now);
}

bool Worker::epMediaSend(Endpoint* ep, const CompiledStep& st) {
    std::string amrwb, pcmu, pcma, g722;
    bool def = st.sample.empty();
    if (!def) {
        auto& m = m_run->samples[st.sample];
        amrwb = m.count("amr-wb") ? m["amr-wb"] : "";
        pcmu = m.count("pcmu") ? m["pcmu"] : "";
        pcma = m.count("pcma") ? m["pcma"] : "";
        g722 = m.count("g722") ? m["g722"] : "";
    }
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->MediaSend(ep->callId, def, amrwb, pcmu, pcma, st.loop, g722);
    if (ep->isReal()) return false;   // 실단말의 송출은 실스택 것 — 컴파일 게이트
    return ep->s->MediaSend(def, amrwb, pcmu, pcma, st.loop, g722);
}

bool Worker::epMediaStop(Endpoint* ep) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->MediaStop(ep->callId);
    if (ep->isReal()) return false;
    return ep->s->MediaStop();
}

bool Worker::epDtmf(Endpoint* ep, const std::string& digits) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->SendDtmf(ep->callId, digits);
    if (ep->isReal()) return ep->realCall >= 0 && realRequest(ep, "dtmf " + std::to_string(ep->realCall) + " " + digits)["ok"].asBool(false);
    return ep->s->SendDtmf(digits);
}

void Worker::sampleDtmf(Endpoint* ep) {
    int sent = 0, recv = 0, pt = -1;
    std::string digits;
    if (ep->isPeer()) {
        if (ep->callId.empty() || !ep->poolRef->peer->DtmfStats(ep->callId, sent, recv, digits, pt)) return;
    } else if (ep->isReal()) {
        return;   // 실스택은 수신 DTMF 를 세지 않는다(pjsua2 onDtmfDigit 미노출)
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
    if (ep->isSim()) ep->s->m_clsRtpThread.ResetDtmf();
}

std::string Worker::callerRole(Instance& in) {
    for (auto& a : in.actors) if (a.second && a.second->tStartCallMs > 0) return a.first;
    return "";
}

bool Worker::epHasCall(Endpoint* ep) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->HasCall(ep->callId);
    if (ep->isReal()) return ep->realCall >= 0;
    return !ep->s->m_strInviteId.empty();
}

int Worker::epAnswer(Endpoint* ep) {
    if (ep->isPeer()) return ep->callId.empty() ? 481 : ep->poolRef->peer->Answer(ep->callId);
    if (ep->isReal()) return ep->realCall >= 0 && realRequest(ep, "answer " + std::to_string(ep->realCall) + (ep->realVideo ? " video" : ""))["ok"].asBool(false) ? 0 : 481;
    return ep->s->AnswerCall() ? 0 : 481;
}

bool Worker::epReject(Endpoint* ep, int code, int cause) {
    if (ep->isPeer()) {
        if (ep->callId.empty()) return false;
        return ep->poolRef->peer->Reject(ep->callId, code, cause);
    }
    // UE 시뮬레이터의 거절은 Reason 없이(실 단말 거절 코드만) — cause 는 피어 프로파일(MGCF Q.850) 몫
    if (ep->isReal()) return ep->realCall >= 0 && realRequest(ep, "reject " + std::to_string(ep->realCall) + " " + std::to_string(code))["ok"].asBool(false);
    return ep->s->RejectCall(code);
}

bool Worker::epBye(Endpoint* ep, int cause) {
    if (ep->isPeer()) return !ep->callId.empty() && ep->poolRef->peer->Bye(ep->callId, cause);
    if (ep->isReal()) return ep->realCall >= 0 && realRequest(ep, "hangup " + std::to_string(ep->realCall))["ok"].asBool(false);
    ep->s->StopCall();
    return true;
}

bool Worker::epGroupCall(Endpoint* from, const std::string& group, bool listen, const Json& media) {
    if (from->isReal()) {
        if (from->realCall >= 0) return false;
        Json r = realRequest(from, "group_call " + group + (listen ? " listen" : ""));
        if (!r["ok"].asBool(false)) return false;
        from->realCall = (int)r["call"].asInt(-1);
        return true;
    }
    from->s->SetOfferCodec(codecPtOf(media["audio"].asString("")));   // 시나리오 오퍼 코덱(PTT 표준 = amr-wb). 없으면 풀 기본
    from->s->SetListenOnly(listen);
    from->s->StartGroupCall(group);
    return !from->s->m_strInviteId.empty();
}

bool Worker::epFloorRequest(Endpoint* ep) {
    if (ep->isReal()) return ep->realCall >= 0 && realRequest(ep, "floor_request " + std::to_string(ep->realCall))["ok"].asBool(false);
    ep->s->SendPttRequest();
    return true;
}

void Worker::epFloorRelease(Endpoint* ep, bool held) {
    if (ep->isReal()) { if (ep->realCall >= 0) realRequest(ep, "floor_release " + std::to_string(ep->realCall)); return; }
    if (held) ep->s->MediaStop();
    ep->s->SendPttRelease();
}

bool Worker::epPickup(Endpoint* from, const std::string& code, const std::string& number) {
    Json r = realRequest(from, "pickup " + code + (number.empty() ? "" : " " + number));
    if (!r["ok"].asBool(false)) return false;
    from->realCall = (int)r["call"].asInt(-1);
    from->outPending = true;
    from->outKind = "pickup";
    return true;
}

bool Worker::epUnregister(Endpoint* ep) {
    if (ep->isReal()) { if (ep->started) realRequest(ep, "unregister"); }
    else if (ep->s && ep->started) ep->s->Stop(5);
    else return false;
    ep->started = false;
    ep->registered = false;
    ep->realCall = -1;
    ep->affStarted = ep->affiliated = ep->affFailed = false;
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
    for (auto* ep : m_preludeList) { if (ep->ready()) reg++; else if (ep->started) pending++; }
    bool timeout = now >= m_preludeDeadlineMs;
    if (pending > 0 && !timeout) {
        // 실패 응답(REGISTER 4xx) 은 registered=false 이면서 started=true — 이벤트 카운터로 판정 불가하므로
        // 등록 통계(iRegFail) 를 본다.
        size_t failed = 0;
        for (auto* ep : m_preludeList)
            if (ep->started && !ep->ready() && (ep->isPeer() ? ep->poolRef->regFailed : ep->isReal() ? (ep->realRegFailed || ep->affFailed) : (ep->s->m_stats.iRegFail > 0 || ep->affFailed))) failed++;
        if (failed < pending) return;
    }
    for (auto& kv : m_free) {
        auto& v = kv.second;
        v.erase(std::remove_if(v.begin(), v.end(), [](Endpoint* ep) { return ep->started && !ep->ready(); }), v.end());
    }
    logf("info", "run %s prelude done — ready=%zu/%zu%s → running", m_run->runId.c_str(), reg, m_preludeList.size(),
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
                std::string caller = pendingCallerRole(in);
                if (!caller.empty() && in.actors[caller]->isReal()) caller.clear();   // 실스택은 1xx 를 이벤트로 내지 않는다 — 183 도달 대기 없이 진행
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
                    // 발신자 확립(200 OK 수신) 을 기다린다 — 다음 단계는 그 뒤. 상담 호(두 번째 다이얼로그)도 같은 대기
                    std::string caller = pendingCallerRole(in);
                    in.pending = Instance::NONE;
                    if (!caller.empty()) {
                        in.phase = Instance::WAIT_EVENT;
                        in.awaitKind = "callstart:" + caller;
                        in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                        continue;
                    }
                } else {
                    // 거절: 발신자의 최종 응답 도착을 기다린다
                    std::string caller = pendingCallerRole(in);
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
            std::string what = in.awaitKind;
            if (in.awaitKind == "groupup" && !in.groupTo.empty()) {
                size_t joined = 0, total = 0;
                for (auto* ep : roleEndpoints(in, in.groupTo)) { total++; if (ep->inCall) joined++; }
                what += " — joined " + std::to_string(joined) + "/" + std::to_string(total);
            }
            emitEvent("timeout waiting " + what, nullptr, in.stepIdx < m_body.size() ? m_body[in.stepIdx].step : "", 0);
            if (!pendingCallerRole(in).empty()) m_metrics.counter("isa_fail");   // 최종 응답 없이 시한 — Timer B 만료 상당(RFC 6076 §4.6)
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

/** 그룹 단위 인스턴스 — 준비된 멤버 전원이 free 인 그룹 하나를 순환 선택해 역할에 배정한다(단일 역할 하나씩, multi 역할 = 나머지).
 *  준비 안 된 멤버(등록·affiliation 실패)는 fan-out 대상이 아니므로 뺀다. anyEligible = 멤버 수가 되는 그룹이 하나라도 있는가. */
bool Worker::pickGroup(std::map<std::string, Endpoint*>& actors, std::map<std::string, std::vector<Endpoint*>>& multi, std::string& group,
                       bool& anyEligible) {
    anyEligible = false;
    Pool* pool = m_pools[m_groupPool].get();
    auto sl = m_run->slices[m_singleRoles[0]];
    size_t need = m_singleRoles.size() + (m_usedMulti.empty() ? 0 : 1);
    long long now = nowMs();
    for (size_t k = 0; k < m_groupNames.size(); ++k) {
        size_t gi = (m_groupCursor + k) % m_groupNames.size();
        std::vector<Endpoint*> members;
        for (auto* ep : pool->groups[m_groupNames[gi]])
            if (ep->idx >= sl.first && ep->idx < sl.second && ep->ready()) members.push_back(ep);
        if (members.size() < need) continue;
        anyEligible = true;
        bool free = true;
        for (auto* ep : members)
            if (ep->inst != nullptr || epHasCall(ep) || now - ep->tReleasedMs < m_cfg.groupReuseGapMs) { free = false; break; }
        if (!free) continue;
        size_t i = 0;
        for (auto& r : m_singleRoles) actors[r] = members[i++];
        if (!m_usedMulti.empty()) multi[m_usedMulti[0]].assign(members.begin() + (long)i, members.end());
        group = m_groupNames[gi];
        m_groupCursor = gi + 1;
        return true;
    }
    return false;
}

void Worker::launchInstance(long long now) {
    // body 의 모든 역할에 free 단말이 있어야 한다
    std::map<std::string, Endpoint*> actors;
    std::map<std::string, std::vector<Endpoint*>> multi;
    std::string group;
    if (m_groupBound) {
        bool anyEligible = false;
        if (!pickGroup(actors, multi, group, anyEligible)) {
            m_metrics.counter("skipped");
            bool anyActive = false;
            for (auto& i : m_instances) if (i->phase != Instance::DONE) { anyActive = true; break; }
            if (!anyEligible && !anyActive) {
                emitEvent("no usable group in pool " + m_groupPool + " — run closed", nullptr, "", 0);
                logf("warn", "run %s: pool %s has no group with enough ready members — closing", m_run->runId.c_str(), m_groupPool.c_str());
                endRun("stopped");
            }
            return;
        }
        // 그룹 밖 역할(member: false) — 잡은 그룹의 멤버가 아닌 준비된 PTT 단말을 free 목록에서(청취 관제사·비멤버 거절 시험)
        for (auto& g : m_guestRoles) {
            auto& v = m_free[g];
            Endpoint* pick = nullptr;
            for (size_t i = 0; i < v.size(); ++i) {
                Endpoint* c = v[i];
                if (c->inst == nullptr && c->ready() && c->id.pttGroup != group && !epHasCall(c)) { pick = c; v.erase(v.begin() + (long)i); break; }
            }
            if (!pick) {
                m_metrics.counter("skipped");
                bool anyActive = false;
                for (auto& i : m_instances) if (i->phase != Instance::DONE) { anyActive = true; break; }
                if (!anyActive) {
                    emitEvent("no non-member PTT endpoint for guest role " + g + " (group " + group + ") — run closed", nullptr, "", 0);
                    logf("warn", "run %s: guest role %s has no usable non-member endpoint — closing", m_run->runId.c_str(), g.c_str());
                    endRun("stopped");
                }
                return;
            }
            actors[g] = pick;
        }
    }
    for (auto& kv : m_run->roles) {
        if (m_groupBound) break;
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
    size_t nEps = actors.size();
    for (auto& m : multi) nEps += m.second.size();
    if (m_cfg.maxRtpStreams > 0 && m_bodyRtpMode != CRtpThread::E_MEDIA_NONE &&
        rtpStreams() + (long long)nEps > m_cfg.maxRtpStreams) {
        if (!m_groupBound) for (auto& a : actors) m_free[a.first].push_back(a.second);
        m_metrics.counter("skipped");
        m_metrics.counter("skipped_rtp_cap");
        return;
    }
    auto in = std::make_unique<Instance>();
    in->id = m_nextInstanceId++;
    in->actors = actors;
    in->multi = multi;
    in->group = group;
    in->tStartMs = now;
    in->rtpMode = m_bodyRtpMode;
    for (auto* ep : endpointsOf(*in)) {
        ep->inst = in.get();
        ep->tStartCallMs = 0;
        ep->floor = Endpoint::F_IDLE;
        ep->talked = false;
        epSetMediaMode(ep, in->rtpMode);
        if (ep->s) ep->s->m_clsRtpThread.ResetRecvStats();
        ep->realStats.valid = false;
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
            // to = 역할 또는 다이얼 번호 리터럴(대표번호 — TS 24.239 Flexible Alerting). 번호면 인스턴스의 다른 UE 역할이 포크 착신을 받는다
            bool dial = !st.to.empty() && !m_run->roles.count(st.to);
            Endpoint* to = (st.to.empty() || dial) ? nullptr : in.actors[st.to];
            if (!from || (!to && !dial)) { finishInstance(in, true, "invite: role missing", now); return; }
            from->tStartCallMs = now;
            in.expectCode = (int)st.expect["code"].asInt(0);
            in.rtpMode = rtpModeOf(st.media);   // 이 호의 미디어 평면 — 인스턴스의 모든 단말(전달 대상 포함)에 같은 모드
            for (auto& a : in.actors) epSetMediaMode(a.second, in.rtpMode);
            epSetVideo(in, from, st.media["video"].asString("") == "h264");
            if (dial) {
                // 포크 관측 기준 — 발신자를 뺀 UE 역할 전부가 alert 를 받아야 한다(그룹원으로 시드된 신원). 포크 leg 의 CANCEL(487) 은 정상
                in.forkDial = true;
                in.dialTarget = st.to;
                long long expected = 0;
                for (auto& a : in.actors) if (a.second && a.second != from && !a.second->isPeer()) ++expected;
                m_metrics.counter("fork_expected", expected);
                m_metrics.counter("fork_dial_tx");
            }
            if (!epStartCall(from, to, st.media, dial ? st.to : std::string())) { finishInstance(in, true, "invite: StartCall refused (busy/stack/consult?)", now); return; }
            if (from->outKind == "consult") from->consultTo = st.to;
            noteCallId(&in, from->isPeer() ? from->callId : from->isReal() ? std::string() : from->outKind == "consult" ? from->consultCallId : from->s->m_strInviteId);
            m_metrics.counter("legs", 2);
            bool calleeActs = in.stepIdx + 1 < m_body.size() &&
                              (m_body[in.stepIdx + 1].step == "reject" || m_body[in.stepIdx + 1].step == "answer" ||
                               m_body[in.stepIdx + 1].step == "progress");
            if (in.expectCode >= 300 && !calleeActs) {
                // 거절이 기대값(ACL 403·라우팅 reject 등 대상이 스스로 거절) — 발신자의 최종 응답을 여기서 기다린다(advance 가 다음 단계로).
                //   다음 단계가 착신 측 reject(피어 MGCF 503 등)면 그 단계가 거절을 내고 최종 응답을 기다린다.
                in.phase = Instance::WAIT_EVENT;
                in.awaitKind = "callend:" + st.from;
                in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                return;
            }
            in.stepIdx++;   // 비동기 — 확립은 answer/media_hold 가 기다린다
            continue;
        }
        if (st.step == "group_call") {
            // PTT 그룹콜 — 발신자가 그룹 URI 로 INVITE, 대상이 affiliation 멤버에게 fan-out 하고 멤버는 자동응답한다.
            //   완료 = 발신자 확립 + to(multi 역할) 멤버 전원 합류. expect.code≥300 이면 그 최종 응답이 성공 조건(비멤버 403 등).
            Endpoint* from = in.actors[st.from];
            if (!from || !from->isPtt()) { finishInstance(in, true, "group_call: from 은 PTT 단말이어야 한다", now); return; }
            std::string group = st.group.empty() ? in.group : st.group;
            if (group.empty()) { finishInstance(in, true, "group_call: group 없음", now); return; }
            // 세션이 이미 서 있으면(앞선 group_call) 이 발신은 합류 — 청취(payload listen = a=recvonly, dispatch_center.md §5.6 비멤버 관제사) 또는
            //   비멤버 일반 INVITE(거절 403 기대). 완료 = 이 발신자 자기 200(또는 expect.code) — fan-out 을 다시 기다리지 않는다
            bool listen = st.payload == "listen";
            bool sessionUp = false;
            for (auto* ep : endpointsOf(in)) if (ep != from && ep->inCall) { sessionUp = true; break; }
            if (listen && !sessionUp) { finishInstance(in, true, "group_call listen: 진행 중인 그룹 세션이 없다", now); return; }
            from->tStartCallMs = now;
            if (!sessionUp) { in.tGroupCallMs = now; in.tLastJoinMs = 0; in.groupTo = st.to; }
            in.expectCode = (int)st.expect["code"].asInt(0);
            if (!sessionUp) { in.rtpMode = rtpModeOf(st.media); for (auto* ep : endpointsOf(in)) epSetMediaMode(ep, in.rtpMode); }
            else epSetMediaMode(from, in.rtpMode);
            epSetVideo(in, from, st.media["video"].asString("") == "h264");
            if (!epGroupCall(from, group, listen, st.media)) { finishInstance(in, true, "group_call: StartGroupCall refused (busy/stack?)", now); return; }
            if (from->isSim()) noteCallId(&in, from->s->m_strInviteId);
            m_metrics.counter("legs");
            if (!sessionUp) m_metrics.counter("group_calls");
            m_metrics.counter("invite_tx");
            if (listen) m_metrics.counter("listen_tx");
            from->outPending = true;
            from->outKind = listen ? "listen" : sessionUp ? "group_join" : "invite";   // 확립 때 <kind>_ok — 세션(SER)은 개시 INVITE 만
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = in.expectCode >= 300 ? "callend:" + st.from : sessionUp ? "callstart:" + st.from : "groupup";   // 셋 다 advance 가 stepIdx++ 한다
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "floor_request" || st.step == "floor_release") {
            std::vector<Endpoint*> eps;
            for (auto& role : st.who) for (auto* ep : roleEndpoints(in, role)) eps.push_back(ep);
            if (eps.empty()) { finishInstance(in, true, st.step + ": role missing", now); return; }
            for (auto* ep : eps)
                if (!ep->isPtt() || !ep->inCall) { finishInstance(in, true, st.step + ": " + ep->id.user + " 그룹 세션 밖", now); return; }
            std::vector<Endpoint*> all = endpointsOf(in);
            if (st.step == "floor_request") {
                std::string want = st.payload.empty() ? "granted" : st.payload;
                in.floorWant = want;
                in.floorWait = eps;
                in.tFloorReqUs = nowUs();
                for (auto* ep : all) ep->takenSeen = false;
                for (auto* ep : eps) {
                    ep->floor = Endpoint::F_REQUESTED;
                    ep->wasQueued = false;
                    ep->tFloorReqUs = nowUs();
                    if (!epFloorRequest(ep)) { finishInstance(in, true, "floor_request: " + ep->id.user + " 요청 거절", now); return; }
                    m_metrics.counter("floor_request_tx");
                }
                in.phase = Instance::WAIT_EVENT;
                in.awaitKind = "floor";
                in.deadlineMs = now + m_cfg.floorTimeoutMs;
                return;
            }
            // floor_release — floor 를 가진(또는 큐에 있는) 단말만. 완료 = 해제한 발언자가 Idle(또는 다음 발언자의 Taken)을 받음
            in.floorWait.clear();
            for (auto* ep : all) ep->idleSeen = false;
            in.tFloorRelUs = nowUs();
            for (auto* ep : eps) {
                if (ep->floor != Endpoint::F_GRANTED && ep->floor != Endpoint::F_QUEUED && ep->floor != Endpoint::F_REQUESTED) continue;
                bool held = ep->floor == Endpoint::F_GRANTED;
                epFloorRelease(ep, held);
                ep->floor = Endpoint::F_IDLE;
                m_metrics.counter("floor_release_tx");
                if (held) in.floorWait.push_back(ep);
            }
            if (in.floorWait.empty()) { in.stepIdx++; continue; }
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "floorrel";
            in.deadlineMs = now + m_cfg.floorTimeoutMs;
            return;
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
            if (ep->pendingInvite) { in.phase = Instance::WAIT_TIME; in.waitUntilMs = std::max(now + st.afterMs, ep->holdUntilMs); }
            else { in.phase = Instance::WAIT_EVENT; in.awaitKind = "incoming:" + role; in.deadlineMs = now + m_cfg.inviteTimeoutMs; }
            return;
        }
        if (st.step == "media_hold") {
            std::string caller = pendingCallerRole(in);
            if (!caller.empty()) {
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
                std::vector<Endpoint*> eps = roleEndpoints(in, role);
                if (eps.empty()) { finishInstance(in, true, st.step + ": role missing", now); return; }
                for (auto* ep : eps) {
                    bool ok = st.step == "media_send" ? epMediaSend(ep, st) : epMediaStop(ep);
                    if (!ok) {
                        emitEvent(st.step + ": no media session (SDP 미교환)", ep, st.step, 0, ep->callId);
                        finishInstance(in, true, st.step + ": " + role + " has no media session", now);
                        return;
                    }
                    m_metrics.counter(st.step == "media_send" ? "media_send" : "media_stop");
                }
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
                std::string caller = pendingCallerRole(in);
                if (!caller.empty()) {
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
                std::string caller = pendingCallerRole(in);
                if (!caller.empty()) {
                    in.phase = Instance::WAIT_EVENT; in.awaitKind = "callstart:" + caller; in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                    in.stepIdx--;
                    return;
                }
                finishInstance(in, true, "dtmf: not in call", now); return;
            }
            if (st.payload.empty()) { finishInstance(in, true, "dtmf: payload(digits) required", now); return; }
            if (!epDtmf(ep, st.payload)) {
                m_metrics.counter("dtmf_unsupported");
                emitEvent("DTMF unavailable (telephone-event not negotiated, in-band needs G.711)", ep, "dtmf", 0, ep->callId);
                finishInstance(in, true, "dtmf: telephone-event not negotiated / in-band not G.711", now); return;
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
            {
                // 첫 통화·상담 통화(있으면) 확립을 기다렸다가 다시 이 단계로
                std::string caller = pendingCallerRole(in);
                if (!caller.empty()) {
                    in.phase = Instance::WAIT_EVENT; in.awaitKind = "callstart:" + caller; in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                    in.stepIdx--;
                    return;
                }
            }
            if (!from->inCall) { finishInstance(in, true, "refer: not in call", now); return; }
            // attended = 전달자가 to 와 상담 통화 중(같은 인스턴스에서 from 이 to 를 두 번째 다이얼로그로 불렀다) — Refer-To 에 Replaces. 아니면 blind
            bool attended = !from->isPeer() && from->inConsult && from->consultTo == st.to;
            in.expectCode = (int)st.expect["code"].asInt(202);
            if (!epRefer(from, to, attended)) { finishInstance(in, true, "refer: REFER not sent", now); return; }
            m_metrics.counter("refer_tx");
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "referresp:" + st.from;
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "bye") {
            // media_hold 직후라면 RTP 품질 표본. DTMF 수신 수는 항상 표본(단계 dtmf 가 있었을 때만 값이 있다)
            for (auto* ep : endpointsOf(in)) sampleDtmf(ep);   // RTP 표본이 카운터를 리셋하므로 먼저
            if (in.mediaHeld && in.rtpMode != CRtpThread::E_MEDIA_NONE) {
                for (auto* ep : endpointsOf(in)) sampleRtp(ep);
                in.mediaHeld = false;
            }
            // 행위자 = from 또는 who(역할 여럿·multi 역할 — 그룹 세션에서 멤버들이 각자 나간다). 응답을 전부 기다린다
            std::vector<std::string> roles = st.who;
            if (!st.from.empty()) roles.assign(1, st.from);
            std::vector<Endpoint*> eps;
            for (auto& role : roles) for (auto* ep : roleEndpoints(in, role)) eps.push_back(ep);
            if (eps.empty()) { finishInstance(in, true, "bye: role missing", now); return; }
            in.byeWait.clear();
            for (auto* ep : eps) {
                if (!ep->inCall || !epHasCall(ep)) continue;   // 이미 끝난 호(상대 종료·실패) — BYE 없이 통과
                ep->byeByStep = true;
                epBye(ep, st.cause);
                if (st.cause > 0) m_metrics.counter("q850_tx");
                in.byeWait.push_back(ep);
            }
            if (in.byeWait.empty()) { in.stepIdx++; continue; }
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "byeresp";
            in.deadlineMs = now + m_cfg.byeTimeoutMs;
            return;
        }
        if (st.step == "pickup" || st.step == "replaces" || st.step == "join") {
            // 당겨받기(피처코드, volte_supplementary_services.md §5) · INVITE-Replaces(RFC 3891, BLF 클릭 픽업 §6.2) · INVITE-Join(RFC 3911, 합법감청 청취 dispatch_center.md §5.3)
            //   from 이 새 다이얼로그를 열고 서버가 대상 호를 재고정(픽업·Replaces)하거나 청취 leg 를 붙인다(Join). 완료 = from 의 200(또는 expect.code 의 거절)
            Endpoint* from = in.actors[st.from];
            // pickup 의 to 는 역할 또는 번호 리터럴(링잉 대표번호 지정 픽업 — dispatch_center.md §4.4 PickUpFork)
            bool dialTo = st.step == "pickup" && !st.to.empty() && !m_run->roles.count(st.to);
            Endpoint* to = (st.to.empty() || dialTo) ? nullptr : in.actors[st.to];
            if (!from || from->isPeer()) { finishInstance(in, true, st.step + ": from 은 UE 역할이어야 한다", now); return; }
            if (st.step != "pickup" && !to) { finishInstance(in, true, st.step + ": to(대상 다이얼로그의 당사자 역할) 필요", now); return; }
            if (st.step == "pickup" && !st.to.empty() && !to && !dialTo) { finishInstance(in, true, "pickup: to role missing", now); return; }
            if (st.step == "pickup" && st.payload.empty()) { finishInstance(in, true, "pickup: payload(피처코드) 필요", now); return; }
            if (from->inCall || from->outPending) { finishInstance(in, true, st.step + ": from 이 이미 통화 중", now); return; }
            if (from->isReal()) {
                // 실단말 — 픽업만(피처코드 다이얼). Replaces/Join 은 dialog 학습이 실스택 앱 몫이라 컴파일 게이트가 막는다
                if (st.step != "pickup") { finishInstance(in, true, st.step + ": real-ue 는 pickup 만", now); return; }
                markCancelExpected(in);
                from->tStartCallMs = now;
                in.expectCode = (int)st.expect["code"].asInt(0);
                if (!epPickup(from, st.payload, to ? to->id.user : dialTo ? st.to : std::string())) { finishInstance(in, true, "pickup: INVITE not sent", now); return; }
                m_metrics.counter("legs");
                m_metrics.counter("pickup_tx");
                m_metrics.counter("invite_tx");
                in.phase = Instance::WAIT_EVENT;
                in.awaitKind = (in.expectCode >= 300 ? "callend:" : "callstart:") + st.from;
                in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                return;
            }
            if (st.step != "pickup" && from->s->m_strWatchedDlgCallId.empty()) {
                // dialog 이벤트 NOTIFY 로 대상 다이얼로그를 아직 못 배웠다 — NOTIFY(early|confirmed) 가 오면 이 단계를 다시 실행한다
                if (!from->dlgWatching) { finishInstance(in, true, st.step + ": from 이 to 를 dialog 구독하지 않았다(subscribe 단계 선행)", now); return; }
                in.phase = Instance::WAIT_EVENT;
                in.awaitKind = "dialog:" + st.from;
                in.deadlineMs = now + m_cfg.inviteTimeoutMs;
                return;
            }
            markCancelExpected(in);
            if (!from->started && !startEndpoint(from)) { finishInstance(in, true, st.step + ": stack", now); return; }
            from->s->SetOfferCodec(codecPtOf(st.media["audio"].asString("")));
            from->tStartCallMs = now;
            in.expectCode = (int)st.expect["code"].asInt(0);
            if (!startSpecialCall(from, to, st.step, st.payload, dialTo ? st.to : std::string())) { finishInstance(in, true, st.step + ": INVITE not sent", now); return; }
            noteCallId(&in, from->s->m_strInviteId);
            m_metrics.counter("legs");
            m_metrics.counter(st.step + "_tx");
            m_metrics.counter("invite_tx");
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = (in.expectCode >= 300 ? "callend:" : "callstart:") + st.from;
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "subscribe") {
            // out-of-dialog SUBSCRIBE(RFC 6665) — payload = 이벤트 패키지(기본 dialog, RFC 4235), to = 감시 대상 역할(생략 = 자기 AoR).
            //   완료 = who 전원의 최종 응답(expect.code, 기본 200 — 403 그룹 밖 감시·489 미지 패키지도 기대값으로 둘 수 있다)
            std::string event = st.payload.empty() ? "dialog" : st.payload;
            Endpoint* to = st.to.empty() ? nullptr : in.actors[st.to];
            if (!st.to.empty() && !to) { finishInstance(in, true, "subscribe: to role missing", now); return; }
            in.expectCode = (int)st.expect["code"].asInt(0);
            in.respWait.clear();
            for (auto& role : st.who) {
                Endpoint* ep = in.actors[role];
                if (!ep) { finishInstance(in, true, "subscribe: role missing", now); return; }
                if (!epSubscribe(ep, event, to ? to->id.user : ep->id.user)) { finishInstance(in, true, "subscribe: " + role + " 은 등록된 UE 여야 한다", now); return; }
                m_metrics.counter("subscribe_tx");
                in.respWait.push_back(ep);
            }
            if (in.respWait.empty()) { finishInstance(in, true, "subscribe: who required", now); return; }
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "subresp";
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "publish") {
            // MCPTT affiliation 명령 PUBLISH(TS 24.379 §9, RFC 3903) — payload = affiliate(기본)|deaffiliate, group = 대상 그룹(생략 = 신원의 그룹).
            //   완료 = who 전원의 최종 응답(expect.code, 기본 200 — 비멤버 403 도 기대값으로). de-affiliate 는 단말의 준비 상태를 내린다
            bool deaff = st.payload == "deaffiliate";
            in.expectCode = (int)st.expect["code"].asInt(0);
            in.respWait.clear();
            for (auto& role : st.who) {
                for (auto* ep : roleEndpoints(in, role)) {
                    if (!ep->isPtt() || !ep->registered || !ep->isSim()) { finishInstance(in, true, "publish: " + role + " 은 등록된 PTT 가상 단말이어야 한다", now); return; }
                    ep->s->AffiliateGroup(deaff, st.group);
                    m_metrics.counter("publish_tx");
                    in.respWait.push_back(ep);
                }
            }
            if (in.respWait.empty()) { finishInstance(in, true, "publish: who required", now); return; }
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "pubresp";
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "sds_send") {
            // MCData SDS(TS 24.282) — from 이 to 역할에 1:1 SDS 또는(to 없음) 인스턴스의 그룹(또는 step.group)에 그룹 SDS 를 낸다. payload = 본문.
            //   완료 = MESSAGE 최종 응답(expect.code, 기본 200). disposition: true 면 delivery 요청(수신 단말이 NOTIFICATION 회신 → sds_disposition_pct)
            std::string role = st.from.empty() ? (st.who.empty() ? "" : st.who[0]) : st.from;
            Endpoint* from = in.actors[role];
            if (!from) { finishInstance(in, true, "sds_send: from role missing", now); return; }
            if (!from->isSim() || !from->registered) { finishInstance(in, true, "sds_send: " + role + " 은 등록된 가상 UE 여야 한다", now); return; }
            Endpoint* to = st.to.empty() ? nullptr : in.actors[st.to];
            if (!st.to.empty() && !to) { finishInstance(in, true, "sds_send: to role missing", now); return; }
            std::string group = to ? "" : (st.group.empty() ? in.group : st.group);
            if (!to && group.empty()) { finishInstance(in, true, "sds_send: to 역할 또는 그룹(그룹 세션·group) 이 필요하다", now); return; }
            if (st.payload.empty()) { finishInstance(in, true, "sds_send: payload(본문) required", now); return; }
            std::string msgId = from->s->SendSds(to ? to->id.user : "", group, st.payload, st.disposition);
            if (msgId.empty()) { finishInstance(in, true, "sds_send: 스택 거절", now); return; }
            in.sdsMsgId = msgId;
            in.sdsSendMs = now;
            in.sdsDisposition = st.disposition;
            in.expectCode = (int)st.expect["code"].asInt(0);
            m_metrics.counter("sds_tx");
            if (!group.empty()) m_metrics.counter("sds_group_tx");
            if (st.disposition) m_metrics.counter("sds_disposition_req");
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "sdsresp";
            in.deadlineMs = now + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "sds_recv") {
            // who 전원이 인스턴스의 마지막 SDS(msgId)를 받을 때까지 — 이미 받은 단말은 바로 지운다(단계 진입 전 도착)
            if (in.sdsMsgId.empty()) { finishInstance(in, true, "sds_recv: 앞선 sds_send 가 없다", now); return; }
            in.sdsWait.clear();
            for (auto& role : st.who)
                for (auto* ep : roleEndpoints(in, role)) {
                    if (!ep->isSim()) { finishInstance(in, true, "sds_recv: " + role + " 은 가상 UE 여야 한다", now); return; }
                    if (std::find(ep->sdsRx.begin(), ep->sdsRx.end(), in.sdsMsgId) == ep->sdsRx.end()) in.sdsWait.push_back(ep);
                }
            if (in.sdsWait.empty()) { in.stepIdx++; continue; }
            in.phase = Instance::WAIT_EVENT;
            in.awaitKind = "sdsrecv";
            in.deadlineMs = now + std::max(st.afterMs, 0) + m_cfg.inviteTimeoutMs;
            return;
        }
        if (st.step == "expect") { in.stepIdx++; continue; }
        finishInstance(in, true, "unsupported step " + st.step, now);
        return;
    }
}

std::string Worker::pendingCallerRole(Instance& in) {
    for (auto& a : in.actors) if (a.second && a.second->outPending) return a.first;
    return "";
}

void Worker::markCancelExpected(Instance& in) {
    // 링잉 중인(응답 보류) 착신 leg — 픽업·Replaces 가 그 호를 가져가면 서버가 CANCEL 한다(487). 인스턴스 실패로 세지 않는다
    for (auto* ep : endpointsOf(in))
        if (ep->pendingInvite && ep->tStartCallMs == 0) ep->cancelExpected = true;
}

void Worker::sampleRtp(Endpoint* ep) {
    unsigned long long rx = 0, lost = 0;
    long long jitterUs = 0;
    if (ep->isReal()) {
        // 실단말 표본 — 프로세스가 1 초마다 올린 RTP/RTCP 통계(pjmedia). leg 하나이므로 전체 지표(rtp_*·jitter_ms·mos)에 다른 단말과 같이 들어가고,
        //   부하 아래 실단말 품질만 따로 보려는 real_* 시리즈에도 같은 표본을 남긴다(§3.3). 코덱 = 접속환경 표준(수신 PT 는 실스택이 내지 않는다)
        if (!ep->realStats.valid) { m_metrics.counter("real_rtp_nosample"); return; }
        rx = ep->realStats.rx; lost = ep->realStats.lost; jitterUs = ep->realStats.jitterUs;
        logf("debug", "real-ue rtp sample %s(%s): tx=%llu rx=%llu lost=%llu jitter_us=%lld", roleOf(ep->inst, ep).c_str(), ep->id.user.c_str(), ep->realStats.tx, rx, lost, jitterUs);
        m_metrics.counter("rtp_tx", (long long)ep->realStats.tx);
        m_metrics.counter("rtp_rx", (long long)rx);
        m_metrics.counter("rtp_lost", (long long)lost);
        m_metrics.counter("real_rtp_tx", (long long)ep->realStats.tx);
        m_metrics.counter("real_rtp_rx", (long long)rx);
        m_metrics.counter("real_rtp_lost", (long long)lost);
        if (rx + lost > 0) {
            double lossPct = 100.0 * (double)lost / (double)(rx + lost), jitterMs = (double)jitterUs / 1000.0;
            double mos = emodelMos(emodelCodec(ep->isPtt() || ep->poolRef->service == "volte" ? "AMR-WB" : "PCMU"), lossPct, jitterMs);
            m_metrics.timer("rtp_loss_pct", lossPct);
            m_metrics.timer("jitter_ms", jitterMs);
            m_metrics.timer("mos", mos);
            m_metrics.timer("real_rtp_loss_pct", lossPct);
            m_metrics.timer("real_jitter_ms", jitterMs);
            m_metrics.timer("real_mos", mos);
        } else if (!(ep->isPtt() && ep->talked)) {
            m_metrics.counter("rtp_silent_legs");
            m_metrics.counter("real_rtp_silent_legs");
        }
        ep->talked = ep->floor == Endpoint::F_GRANTED;
        ep->realStats.valid = false;
        return;
    }
    if (ep->isPeer()) {
        if (ep->callId.empty() || !ep->poolRef->peer->RtpStats(ep->callId, rx, lost, jitterUs)) return;
    } else {
        CRtpThread& rt = ep->s->m_clsRtpThread;
        rx = rt.m_ullRecvTotal.load(); lost = rt.m_ullRecvLost.load(); jitterUs = rt.m_llRecvJitterUs.load();
    }
    unsigned long long tx = ep->isPeer() ? ep->poolRef->peer->RtpSent(ep->callId) : ep->s->m_clsRtpThread.m_ullSentTotal.load();
    // 수신 품질 부가 — wire PT(MOS 코덱)·RTCP SR/RR 수신 통계(상대가 본 우리 스트림의 fraction lost)
    int pt = -1, rtcpRx = 0, rrFrac = -1;
    if (ep->isPeer()) ep->poolRef->peer->RtpQuality(ep->callId, pt, rtcpRx, rrFrac);
    else { CRtpThread& rt = ep->s->m_clsRtpThread; pt = rt.m_iRecvPt.load(); rtcpRx = rt.m_iRtcpRecv.load(); rrFrac = rt.m_iRtcpRrFractionLost.load(); }
    logf("debug", "rtp sample %s(%s): tx=%llu rx=%llu lost=%llu jitter_us=%lld pt=%d rtcp_rx=%d rr_frac=%d", roleOf(ep->inst, ep).c_str(), ep->id.user.c_str(), tx, rx, lost, jitterUs, pt, rtcpRx, rrFrac);
    m_metrics.counter("rtp_tx", (long long)tx);
    m_metrics.counter("rtp_rx", (long long)rx);
    m_metrics.counter("rtp_lost", (long long)lost);
    if (rtcpRx > 0) { m_metrics.counter("rtcp_rx", rtcpRx); if (rrFrac >= 0) { m_metrics.counter("rtcp_rr_rx"); m_metrics.timer("rtcp_remote_loss_pct", rrFrac * 100.0 / 256.0); } }
    if (ep->joined && !ep->isPeer()) {
        // 청취 leg(RFC 3911 Join) — 서버가 양 화자를 SSRC 2개로 분리 인도했는가(dispatch_center.md §5.3, RFC 5576 라벨링)
        size_t ssrc = ep->s->RecvSsrcCount();
        if (ssrc >= 2) m_metrics.counter("join_ssrc2");
        else emitEvent("join tap received " + std::to_string(ssrc) + " SSRC (expected 2)", ep, "join", 0, ep->s->m_strInviteId);
    }
    if (rx + lost > 0) {
        double lossPct = 100.0 * (double)lost / (double)(rx + lost), jitterMs = (double)jitterUs / 1000.0;
        m_metrics.timer("rtp_loss_pct", lossPct);
        m_metrics.timer("jitter_ms", jitterMs);
        // MOS 추정(G.107 E-model, 코덱 = 수신 wire PT) — 손실·지터에서, 단방향 망 지연은 0 으로 둔다(RTCP RTT 미측정)
        m_metrics.timer("mos", emodelMos(emodelCodec(codecNameOf(pt)), lossPct, jitterMs));
    } else if (!(ep->isPtt() && ep->talked)) {
        m_metrics.counter("rtp_silent_legs");   // PTT 발언자는 자기 발언 동안 수신이 없는 것이 정상
    }
    ep->talked = ep->floor == Endpoint::F_GRANTED;
    if (ep->isPeer()) ep->poolRef->peer->ResetRtpStats(ep->callId);
    else ep->s->m_clsRtpThread.ResetRecvStats();
}

void Worker::releaseEndpoint(Endpoint* ep) {
    if (ep->pendingInvite) { epReject(ep, 480); ep->pendingInvite = false; }
    else if (ep->inCall || epHasCall(ep)) epBye(ep);   // UE 는 상담 통화도 함께 내린다(StopCall)
    ep->inCall = false;
    ep->tStartCallMs = 0;
    ep->outPending = false;
    ep->outKind.clear();
    ep->byeByStep = false;
    ep->consultCallId.clear();
    ep->consultTo.clear();
    ep->inConsult = false;
    ep->cancelExpected = false;
    ep->joined = false;
    ep->floor = Endpoint::F_IDLE;
    ep->tReleasedMs = nowMs();
    if (ep->isPeer()) epClearCall(ep);
    else if (ep->isReal()) { ep->realReinviteWait = false; ep->realHeld = false; ep->realVideo = false; }
    else { epUnsubscribe(ep); ep->s->SetMediaMode(CRtpThread::E_MEDIA_AUTO); ep->s->SetListenOnly(false); }
    Instance* in = ep->inst;
    ep->inst = nullptr;
    // 그룹 단위 인스턴스는 멤버를 free 목록에서 고르지 않는다(그룹 목록에서). 그룹 밖 역할(member: false)만 free 목록으로 돌아간다
    if (in) for (auto& a : in->actors) if (a.second == ep) { if (!m_groupBound || isGuestRole(a.first)) m_free[a.first].insert(m_free[a.first].begin(), ep); break; }
}

void Worker::finishInstance(Instance& in, bool failed, const std::string& why, long long now) {
    if (in.phase == Instance::DONE) return;
    in.phase = Instance::DONE;
    in.failed = failed;
    if (failed) { m_metrics.counter("failed"); if (!why.empty()) logf("debug", "instance %lld failed: %s", in.id, why.c_str()); }
    else m_metrics.counter("instances_ok");
    m_metrics.timer("sdt_s", (double)(now - in.tStartMs) / 1000.0);
    // SIP 덤프 — 정리 BYE/CANCEL·487 까지 담기게 조금 뒤에 올린다(성공한 인스턴스의 것은 그때 버린다)
    if (m_sipCapture.mode() != SipCapture::OFF && !in.callIds.empty())
        m_sipPending.push_back({ now + 1500, failed || m_sipCapture.mode() == SipCapture::ALL, in.id, in.callIds });
    // 단말 반환 (남은 호 정리 포함)
    for (auto* ep : endpointsOf(in)) releaseEndpoint(ep);
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
            for (int i = sl.first; i < sl.second && i < (int)pool->eps.size(); ++i) epUnregister(pool->eps[i].get());
        }
    }
    // 남은 SIP 덤프(마지막 인스턴스들) → 마지막 집계 + 종료 로그
    flushSipPending(nowMs(), true);
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
