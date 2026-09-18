#include "MediaAgent.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

static HttpResponse jres(int status, const Json& body) { HttpResponse r; r.status = status; r.body = body.dump(); return r; }
static HttpResponse jerr(int status, const std::string& code, const std::string& detail = "") {
    Json j = Json::Object(); j["error"] = Json(code); if (!detail.empty()) j["detail"] = Json(detail); return jres(status, j);
}

// ── 서버 ─────────────────────────────────────────────────────────────────────
HttpResponse MediaAgent::handle(const HttpRequest& req, const Json& body) {
    const std::string& p = req.path;
    if (req.method == "POST" && p == "/media/alloc") {
        auto rt = std::make_unique<CRtpThread>();
        if (!m_mediaFile.empty()) rt->SetMediaFile(m_mediaFile);
        if (body["video"].asBool(false) && !m_videoFile.empty()) rt->SetVideoFile(m_videoFile);
        rt->m_bDtmfInband = body["dtmf_inband"].asBool(false);
        if (!rt->Create()) return jerr(500, "alloc_failed", "socket");
        std::lock_guard<std::mutex> lk(m_mtx);
        std::string id = "m" + std::to_string(++m_seq);
        Json j = Json::Object();
        j["id"] = Json(id); j["ip"] = Json(m_localIp); j["port"] = Json((long long)rt->m_iPort); j["video_port"] = Json((long long)rt->m_iVideoPort);
        m_streams[id] = std::move(rt);
        return jres(200, j);
    }
    if (p.rfind("/media/", 0) != 0) return jerr(404, "not_found");
    std::string rest = p.substr(7);
    size_t sl = rest.find('/');
    std::string id = sl == std::string::npos ? rest : rest.substr(0, sl);
    std::string action = sl == std::string::npos ? "" : rest.substr(sl + 1);
    std::unique_lock<std::mutex> lk(m_mtx);
    auto it = m_streams.find(id);
    if (it == m_streams.end()) return jerr(404, "stream_not_found", id);
    CRtpThread* rt = it->second.get();
    if (req.method == "DELETE" && action.empty()) {
        std::unique_ptr<CRtpThread> owned = std::move(it->second);
        m_streams.erase(it);
        lk.unlock();
        owned->Stop();   // 소멸자가 Destroy
        return jres(200, Json::Object());
    }
    lk.unlock();   // 이하는 스트림 하나만 만진다(같은 id 의 동시 요청은 없다 — 시그널링 워커의 한 세션이 순서대로 부른다)
    if (req.method == "POST" && action == "start") {
        rt->m_iAudioPt = (int)body["audio_pt"].asInt(-1);
        rt->m_iDtmfPt = (int)body["dtmf_pt"].asInt(-1);
        rt->m_iDtmfClock = (int)body["dtmf_clock"].asInt(8000);
        rt->m_bDtmfInband = body["dtmf_inband"].asBool(rt->m_bDtmfInband);
        rt->SetMediaMode((int)body["mode"].asInt(0));
        rt->m_bUseMediaFile = body["use_media_file"].asBool(true);
        rt->m_iDestVideoPort = (int)body["dest_video_port"].asInt(0);
        rt->m_bVideoOffer = body["video_offer"].asBool(true);
        std::string suite = body["srtp_suite"].asString();
        if (!suite.empty()) { if (!rt->SetSrtpKeys(suite, body["srtp_local"].asString(), body["srtp_remote"].asString())) return jerr(400, "srtp_failed"); }
        else rt->ClearSrtp();
        std::string vs = body["video_srtp_suite"].asString();
        if (!vs.empty() && rt->m_iVideoPort > 0) { if (!rt->SetVideoSrtpKeys(vs, body["video_srtp_local"].asString(), body["video_srtp_remote"].asString())) return jerr(400, "video_srtp_failed"); }
        if (!rt->Start(body["dest_ip"].asString().c_str(), (int)body["dest_port"].asInt(0))) return jerr(500, "start_failed");
        return jres(200, Json::Object());
    }
    if (req.method == "POST" && action == "stop") { rt->Stop(); return jres(200, Json::Object()); }
    if (req.method == "POST" && action == "ctl") {
        std::string op = body["op"].asString();
        if (op == "send") rt->MediaSend(body["a"].asString(), body["b"].asString(), body["c"].asString(), body["loop"].asBool(true), body["d"].asString());
        else if (op == "send_default") rt->MediaSendDefault();
        else if (op == "stop") rt->MediaStop();
        else if (op == "hold") rt->SetHoldPaused(body["a"].asString() == "on");
        else if (op == "dtmf") { if (!rt->SendDtmf(body["a"].asString())) return jerr(409, "dtmf_refused", "telephone-event not negotiated / in-band needs G.711"); }
        else if (op == "reset") { rt->ResetRecvStats(); rt->ResetDtmf(); }
        else return jerr(400, "bad_op", op);
        return jres(200, Json::Object());
    }
    if (req.method == "GET" && action == "stats") {
        Json j = Json::Object();
        j["tx"] = Json((long long)rt->m_ullSentTotal.load()); j["rx"] = Json((long long)rt->m_ullRecvTotal.load()); j["lost"] = Json((long long)rt->m_ullRecvLost.load());
        j["jitter_us"] = Json((long long)rt->m_llRecvJitterUs.load()); j["recv_pt"] = Json((long long)rt->m_iRecvPt.load());
        j["rtcp_rx"] = Json((long long)rt->m_iRtcpRecv.load()); j["rtcp_rr_blocks"] = Json((long long)rt->m_iRtcpRrBlocks.load()); j["rr_fraction_lost"] = Json((long long)rt->m_iRtcpRrFractionLost.load());
        j["dtmf_sent"] = Json((long long)rt->m_iDtmfSent.load()); j["dtmf_recv"] = Json((long long)rt->m_iDtmfRecv.load()); j["dtmf_digits"] = Json(rt->DtmfRecv());
        j["ssrc_count"] = Json((long long)rt->RecvSsrcCount()); j["send_running"] = Json(rt->MediaRunning()); j["source_ended"] = Json(rt->SourceEnded());
        return jres(200, j);
    }
    return jerr(404, "not_found", req.method + " " + p);
}

long long MediaAgent::streams() { std::lock_guard<std::mutex> lk(m_mtx); return (long long)m_streams.size(); }
long long MediaAgent::running() { std::lock_guard<std::mutex> lk(m_mtx); long long n = 0; for (auto& kv : m_streams) if (kv.second->MediaRunning()) ++n; return n; }
void MediaAgent::stopAll() { std::lock_guard<std::mutex> lk(m_mtx); for (auto& kv : m_streams) kv.second->Stop(); m_streams.clear(); }

// ── 클라이언트 ────────────────────────────────────────────────────────────────
MediaAgentClient::MediaAgentClient(const std::string& url, int timeoutMs) : m_url(url), m_timeoutMs(timeoutMs) {
    std::string u = url;
    if (u.rfind("http://", 0) == 0) u = u.substr(7);
    size_t sl = u.find('/'); if (sl != std::string::npos) u = u.substr(0, sl);
    size_t c = u.find(':');
    m_host = c == std::string::npos ? u : u.substr(0, c);
    if (c != std::string::npos) m_port = atoi(u.c_str() + c + 1);
}

bool MediaAgentClient::request(const std::string& method, const std::string& path, const Json* body, Json& out, int& status) {
    status = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)m_port);
    if (inet_pton(AF_INET, m_host.c_str(), &sa.sin_addr) != 1) { close(fd); return false; }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, (sockaddr*)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return false; }
    pollfd pw{ fd, POLLOUT, 0 };
    if (poll(&pw, 1, m_timeoutMs) <= 0) { close(fd); return false; }
    int soerr = 0; socklen_t sl = sizeof(soerr); getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr) { close(fd); return false; }
    fcntl(fd, F_SETFL, fl);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::string b = body ? body->dump() : "";
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + m_host + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(b.size()) + "\r\n\r\n" + b;
    size_t off = 0;
    while (off < req.size()) { ssize_t n = send(fd, req.data() + off, req.size() - off, 0); if (n <= 0) { close(fd); return false; } off += (size_t)n; }
    std::string buf; char tmp[4096]; size_t he = std::string::npos, cl = 0;
    for (int iter = 0; iter < 4096; ++iter) {
        if (he != std::string::npos && buf.size() >= he + 4 + cl) break;
        pollfd pr{ fd, POLLIN, 0 };
        if (poll(&pr, 1, m_timeoutMs) <= 0) { close(fd); return false; }
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        if (he == std::string::npos) {
            he = buf.find("\r\n\r\n");
            if (he != std::string::npos) {
                std::string lower = buf.substr(0, he); for (auto& ch : lower) ch = (char)tolower(ch);
                size_t p = lower.find("content-length:"); if (p != std::string::npos) cl = (size_t)strtoul(lower.c_str() + p + 15, nullptr, 10);
            }
        }
    }
    close(fd);
    if (he == std::string::npos) return false;
    status = atoi(buf.c_str() + 9);
    std::string jb = buf.substr(he + 4, cl);
    std::string err;
    if (!jb.empty() && !Json::parse(jb, out, err)) return false;
    return true;
}

bool MediaAgentClient::probe(std::string& err) {
    Json out; int st = 0;
    if (!request("GET", "/health", nullptr, out, st) || st != 200) { err = "media agent unreachable: " + m_url; return false; }
    if (!out["media"].isObject() || !out["media"].has("agent_streams")) { err = "worker at " + m_url + " has no media agent (old version)"; return false; }
    return true;
}

bool MediaAgentClient::Allocate(bool bVideo, std::string& id, std::string& ip, int& port, int& videoPort) {
    Json b = Json::Object(); b["video"] = Json(bVideo);
    Json out; int st = 0;
    if (!request("POST", "/media/alloc", &b, out, st) || st != 200) return false;
    id = out["id"].asString(); ip = out["ip"].asString(); port = (int)out["port"].asInt(0); videoPort = (int)out["video_port"].asInt(0);
    return !id.empty() && port > 0;
}

bool MediaAgentClient::Release(const std::string& id) { Json out; int st = 0; return request("DELETE", "/media/" + id, nullptr, out, st) && st == 200; }

bool MediaAgentClient::Start(const std::string& id, const RtpRemoteStart& s) {
    Json b = Json::Object();
    b["dest_ip"] = Json(s.destIp); b["dest_port"] = Json((long long)s.destPort); b["audio_pt"] = Json((long long)s.audioPt);
    b["dtmf_pt"] = Json((long long)s.dtmfPt); b["dtmf_clock"] = Json((long long)s.dtmfClock); b["dtmf_inband"] = Json(s.dtmfInband);
    b["mode"] = Json((long long)s.mode); b["use_media_file"] = Json(s.useMediaFile); b["dest_video_port"] = Json((long long)s.destVideoPort); b["video_offer"] = Json(s.videoOffer);
    if (!s.srtpSuite.empty()) { b["srtp_suite"] = Json(s.srtpSuite); b["srtp_local"] = Json(s.srtpLocal); b["srtp_remote"] = Json(s.srtpRemote); }
    if (!s.vSuite.empty()) { b["video_srtp_suite"] = Json(s.vSuite); b["video_srtp_local"] = Json(s.vLocal); b["video_srtp_remote"] = Json(s.vRemote); }
    Json out; int st = 0;
    return request("POST", "/media/" + id + "/start", &b, out, st) && st == 200;
}

bool MediaAgentClient::Stop(const std::string& id) { Json out; int st = 0; return request("POST", "/media/" + id + "/stop", nullptr, out, st) && st == 200; }

bool MediaAgentClient::Control(const std::string& id, const std::string& op, const std::string& a, const std::string& b, const std::string& c,
                               const std::string& d, bool bLoop) {
    Json j = Json::Object(); j["op"] = Json(op); j["a"] = Json(a); j["b"] = Json(b); j["c"] = Json(c); j["d"] = Json(d); j["loop"] = Json(bLoop);
    Json out; int st = 0;
    return request("POST", "/media/" + id + "/ctl", &j, out, st) && st == 200;
}

bool MediaAgentClient::Stats(const std::string& id, RtpRemoteStats& o) {
    Json out; int st = 0;
    if (!request("GET", "/media/" + id + "/stats", nullptr, out, st) || st != 200) return false;
    o.tx = (unsigned long long)out["tx"].asInt(0); o.rx = (unsigned long long)out["rx"].asInt(0); o.lost = (unsigned long long)out["lost"].asInt(0);
    o.jitterUs = out["jitter_us"].asInt(0); o.recvPt = (int)out["recv_pt"].asInt(-1); o.rtcpRx = (int)out["rtcp_rx"].asInt(0);
    o.rtcpRrBlocks = (int)out["rtcp_rr_blocks"].asInt(0); o.rrFractionLost = (int)out["rr_fraction_lost"].asInt(-1);
    o.dtmfSent = (int)out["dtmf_sent"].asInt(0); o.dtmfRecv = (int)out["dtmf_recv"].asInt(0); o.dtmfDigits = out["dtmf_digits"].asString();
    o.ssrcCount = (unsigned long long)out["ssrc_count"].asInt(0); o.sendRunning = out["send_running"].asBool(false); o.sourceEnded = out["source_ended"].asBool(false);
    return true;
}
