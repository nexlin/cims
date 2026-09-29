#include "DeviceHub.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kProto = 1;
constexpr int kHelloTimeoutMs = 10000;
constexpr int kPingIdleMs = 15000;
constexpr int kDeadIdleMs = 45000;
constexpr int kPollMs = 10;
constexpr size_t kMaxLine = 64 * 1024;

long long epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void setTimeouts(int fd, int ms) {
    struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
/** fd 읽을 것 대기(TLS 버퍼에 남은 것 먼저). 1 = 있음, 0 = 시한, <0 = 오류. */
int waitReadable(int fd, SSL* ssl, int ms) {
    if (SSL_pending(ssl) > 0) return 1;
    struct pollfd p{}; p.fd = fd; p.events = POLLIN;
    int r = ::poll(&p, 1, ms);
    if (r > 0 && (p.revents & (POLLERR | POLLNVAL))) return -1;
    return r;
}
bool sslWriteAll(SSL* ssl, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) {
        int n = SSL_write(ssl, d.data() + off, (int)(d.size() - off));
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}
std::string sha256Hex(X509* x) {
    unsigned char md[EVP_MAX_MD_SIZE]; unsigned int n = 0;
    if (X509_digest(x, EVP_sha256(), md, &n) != 1) return "";
    static const char* hex = "0123456789abcdef";
    std::string o;
    for (unsigned i = 0; i < n; ++i) { o += hex[md[i] >> 4]; o += hex[md[i] & 15]; }
    return o;
}
Json fail(const std::string& reason) {
    Json j = Json::Object();
    j["ok"] = Json(false);
    j["call"] = Json(-1);
    j["reason"] = Json(reason);
    return j;
}

}  // namespace

// ── DeviceConn ──

DeviceConn::DeviceConn(int fd, SSL* ssl, const std::string& addr) : m_fd(fd), m_ssl(ssl) { m_info.addr = addr; }
DeviceConn::~DeviceConn() { close(); }

void DeviceConn::start(const Json& hello) {
    m_info.deviceId = hello["device_id"].asString();
    m_info.app = hello["app"].asString();
    m_info.version = hello["version"].asString();
    m_info.platform = hello["platform"].asString();
    m_info.model = hello["model"].asString();
    m_info.engine = hello["engine"].asString();
    m_info.connectedMs = epochMs();
    const Json& accs = hello["accounts"];
    for (size_t i = 0; i < accs.size(); ++i) {
        DeviceAccount a;
        a.service = accs.at(i)["service"].asString();
        a.aor = accs.at(i)["aor"].asString();
        a.msisdn = accs.at(i)["msisdn"].asString();
        a.registered = accs.at(i)["registered"].asBool(false);
        m_info.accounts.push_back(a);
    }
    setTimeouts(m_fd, 1000);            // 입출력 스레드의 SSL_read/SSL_write 가 오래 막히지 않게
    m_io = std::thread([this] { ioLoop(); });
}

bool DeviceConn::writeRaw(const std::string& line) { return sslWriteAll(m_ssl, line + "\n"); }

bool DeviceConn::send(const std::string& cmd) {
    if (!m_alive) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    m_out.push_back(cmd);
    return true;
}

Json DeviceConn::request(const std::string& cmd, int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_mtx);
    if (!m_alive) return fail("device link lost");
    m_results.clear();                  // 앞 명령의 늦은 결과가 이 명령의 결과로 읽히지 않게
    m_out.push_back(cmd);
    bool got = m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !m_results.empty() || !m_alive.load(); });
    if (!got || m_results.empty()) return fail(m_alive ? "timeout" : "device link lost");
    Json r = m_results.front();
    m_results.pop_front();
    return r;
}

void DeviceConn::bind(EventFn fn, const std::string& pool) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_onEvent = std::move(fn);
    m_info.boundPool = pool;
}

void DeviceConn::unbind() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_onEvent = nullptr;
    m_info.boundPool.clear();
}

bool DeviceConn::registered(const std::string& service) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& a : m_info.accounts) if (a.service == service) return a.registered;
    return false;
}

DeviceInfo DeviceConn::info() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    DeviceInfo i = m_info;
    i.alive = m_alive;
    return i;
}

void DeviceConn::deliver(const Json& ev) {
    const std::string kind = ev["event"].asString();
    EventFn fn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (kind == "result") { m_results.push_back(ev); m_cv.notify_all(); return; }
        if (kind == "ping") { m_out.push_back("pong"); return; }
        if (kind == "pong") return;
        if (kind == "reg") {
            const std::string svc = ev["service"].asString(), st = ev["state"].asString();
            for (auto& a : m_info.accounts) if (a.service == svc) a.registered = st == "registered";
        }
        fn = m_onEvent;
    }
    if (fn) fn(ev);
}

void DeviceConn::ioLoop() {
    std::string buf;
    auto lastRx = Clock::now();
    bool pinged = false;
    while (!m_closing) {
        std::deque<std::string> out;
        { std::lock_guard<std::mutex> lk(m_mtx); out.swap(m_out); }
        bool ok = true;
        for (auto& l : out) if (!writeRaw(l)) { ok = false; break; }
        if (!ok) break;
        int w = waitReadable(m_fd, m_ssl, kPollMs);
        if (w < 0) break;
        if (w == 0) {
            long long idle = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastRx).count();
            if (idle > kDeadIdleMs) break;
            if (idle > kPingIdleMs && !pinged) { std::lock_guard<std::mutex> lk(m_mtx); m_out.push_back("ping"); pinged = true; }
            continue;
        }
        char tmp[4096];
        int n = SSL_read(m_ssl, tmp, sizeof tmp);
        if (n <= 0) {
            int e = SSL_get_error(m_ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            break;
        }
        buf.append(tmp, (size_t)n);
        lastRx = Clock::now(); pinged = false;
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            Json ev; std::string perr;
            if (!Json::parse(line, ev, perr) || !ev.isObject()) continue;
            deliver(ev);
        }
        if (buf.size() > kMaxLine) break;
    }
    EventFn fn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_alive = false;
        m_cv.notify_all();
        fn = m_onEvent;
    }
    if (fn && !m_quiet) {
        Json ex = Json::Object();
        ex["event"] = Json("process_exit");
        ex["status"] = Json(0LL);
        ex["error"] = Json("device link lost");
        fn(ex);
    }
}

void DeviceConn::close(bool quiet) {
    if (quiet) m_quiet = true;
    m_closing = true;
    if (m_fd >= 0) ::shutdown(m_fd, SHUT_RDWR);
    if (m_io.joinable()) m_io.join();
    m_alive = false;
    if (m_ssl) { SSL_free(m_ssl); m_ssl = nullptr; }
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
}

// ── DeviceHub ──

DeviceHub::DeviceHub(const DeviceHubConfig& cfg) : m_cfg(cfg) {}
DeviceHub::~DeviceHub() { stop(); }

bool DeviceHub::loadOrCreateCert(std::string& err) {
    if (access(m_cfg.certFile.c_str(), R_OK) != 0 || access(m_cfg.keyFile.c_str(), R_OK) != 0) {
        // 자체 서명 — 운영자 인증서가 없으면 한 번 만들어 둔다(재기동해도 같은 지문 — 단말 TOFU 가 유지된다)
        EVP_PKEY* pkey = EVP_EC_gen("P-256");
        X509* x = X509_new();
        if (!pkey || !x) { err = "keygen"; EVP_PKEY_free(pkey); X509_free(x); return false; }
        X509_set_version(x, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x), (long)(epochMs() / 1000));
        X509_gmtime_adj(X509_getm_notBefore(x), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x), 10L * 365 * 24 * 3600);
        X509_set_pubkey(x, pkey);
        X509_NAME* nm = X509_get_subject_name(x);
        std::string cn = "cims-tester-worker " + m_cfg.workerName;
        X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_UTF8, (const unsigned char*)cn.c_str(), -1, -1, 0);
        X509_set_issuer_name(x, nm);
        bool ok = X509_sign(x, pkey, EVP_sha256()) > 0;
        FILE* fc = ok ? fopen(m_cfg.certFile.c_str(), "w") : nullptr;
        FILE* fk = ok ? fopen(m_cfg.keyFile.c_str(), "w") : nullptr;
        if (fc && fk) {
            chmod(m_cfg.keyFile.c_str(), 0600);
            ok = PEM_write_X509(fc, x) == 1 && PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        } else ok = false;
        if (fc) fclose(fc);
        if (fk) fclose(fk);
        EVP_PKEY_free(pkey);
        X509_free(x);
        if (!ok) { err = "self-signed cert write failed: " + m_cfg.certFile; return false; }
    }
    if (SSL_CTX_use_certificate_chain_file(m_ctx, m_cfg.certFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(m_ctx, m_cfg.keyFile.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(m_ctx) != 1) {
        err = "cert/key load failed: " + m_cfg.certFile;
        return false;
    }
    if (X509* leaf = SSL_CTX_get0_certificate(m_ctx)) m_fingerprint = sha256Hex(leaf);
    return true;
}

bool DeviceHub::start(std::string& err) {
    if (m_cfg.port <= 0) { err = "disabled"; return false; }
    m_ctx = SSL_CTX_new(TLS_server_method());
    if (!m_ctx) { err = "ssl ctx"; return false; }
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
    if (!loadOrCreateCert(err)) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)m_cfg.port);
    if (inet_pton(AF_INET, m_cfg.ip.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 16) != 0) {
        err = std::string("listen ") + m_cfg.ip + ":" + std::to_string(m_cfg.port) + ": " + strerror(errno);
        ::close(fd);
        return false;
    }
    m_listenFd = fd;
    m_stop = false;
    m_accept = std::thread([this] { acceptLoop(); });
    return true;
}

void DeviceHub::acceptLoop() {
    while (!m_stop) {
        struct pollfd p{}; p.fd = m_listenFd; p.events = POLLIN;
        if (::poll(&p, 1, 200) <= 0) continue;
        struct sockaddr_in ra{}; socklen_t rl = sizeof ra;
        int cfd = accept(m_listenFd, (struct sockaddr*)&ra, &rl);
        if (cfd < 0) continue;
        char ip[64] = {0};
        inet_ntop(AF_INET, &ra.sin_addr, ip, sizeof ip);
        std::string addr = std::string(ip) + ":" + std::to_string(ntohs(ra.sin_port));
        // 핸드셰이크·hello 는 연결마다 스레드 — 느린 단말이 다른 단말의 수락을 막지 않게
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_handshakes.begin(); it != m_handshakes.end();) {
            if (it->done->load()) { it->t.join(); it = m_handshakes.erase(it); } else ++it;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        m_handshakes.push_back({ std::thread([this, cfd, addr, done] { handshake(cfd, addr); *done = true; }), done });
    }
}

void DeviceHub::handshake(int fd, std::string addr) {
    setTimeouts(fd, kHelloTimeoutMs);
    SSL* ssl = SSL_new(m_ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) { SSL_free(ssl); ::close(fd); return; }
    auto reply = [&](const std::string& line) { sslWriteAll(ssl, line + "\n"); };
    auto refuse = [&](const std::string& reason) {
        Json b = Json::Object(); b["event"] = Json("bye"); b["reason"] = Json(reason);
        reply(b.dump());
        SSL_shutdown(ssl); SSL_free(ssl); ::close(fd);
    };
    // hello 한 줄
    std::string buf;
    auto t0 = Clock::now();
    size_t nl = std::string::npos;
    while ((nl = buf.find('\n')) == std::string::npos) {
        int left = kHelloTimeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (left <= 0 || m_stop || buf.size() > kMaxLine) { SSL_free(ssl); ::close(fd); return; }
        if (waitReadable(fd, ssl, left < 200 ? left : 200) <= 0) continue;
        char tmp[4096];
        int n = SSL_read(ssl, tmp, sizeof tmp);
        if (n <= 0) { SSL_free(ssl); ::close(fd); return; }
        buf.append(tmp, (size_t)n);
    }
    Json hello; std::string perr;
    if (!Json::parse(buf.substr(0, nl), hello, perr) || hello["event"].asString() != "hello") { refuse("bad_hello"); return; }
    if (hello["proto"].asInt(0) != kProto) { refuse("proto"); return; }
    if (!m_cfg.pairKey.empty() && hello["pair_key"].asString() != m_cfg.pairKey) { refuse("pair_key"); return; }
    const std::string devId = hello["device_id"].asString();
    std::shared_ptr<DeviceConn> old;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_conns.erase(std::remove_if(m_conns.begin(), m_conns.end(), [](const std::shared_ptr<DeviceConn>& c) { return !c->alive() && c->info().boundPool.empty(); }), m_conns.end());
        for (auto& c : m_conns) if (c->info().deviceId == devId) old = c;
        size_t live = 0;
        for (auto& c : m_conns) if (c->alive() && c != old) live++;
        if ((int)live >= m_cfg.maxDevices) { refuse("full"); return; }
    }
    if (old) old->close(false);                               // 같은 단말의 재접속 — 옛 연결은 닫는다(바인딩 풀은 링크 끊김으로 본다)
    Json w = Json::Object();
    w["event"] = Json("welcome");
    w["worker"] = Json(m_cfg.workerName);
    w["accepted"] = Json(true);
    reply(w.dump());
    auto conn = std::make_shared<DeviceConn>(fd, ssl, addr);
    conn->start(hello);
    std::lock_guard<std::mutex> lk(m_mtx);
    if (old) m_conns.erase(std::remove(m_conns.begin(), m_conns.end(), old), m_conns.end());
    m_conns.push_back(conn);
}

void DeviceHub::stop() {
    if (m_stop.exchange(true) && m_listenFd < 0) return;
    if (m_accept.joinable()) m_accept.join();
    std::vector<Handshake> hs;
    std::vector<std::shared_ptr<DeviceConn>> cs;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        hs.swap(m_handshakes);
        cs.swap(m_conns);
    }
    for (auto& h : hs) if (h.t.joinable()) h.t.join();
    for (auto& c : cs) c->close();
    if (m_listenFd >= 0) { ::close(m_listenFd); m_listenFd = -1; }
    if (m_ctx) { SSL_CTX_free(m_ctx); m_ctx = nullptr; }
}

std::vector<DeviceInfo> DeviceHub::list() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<DeviceInfo> v;
    for (auto& c : m_conns) { DeviceInfo i = c->info(); if (i.alive || !i.boundPool.empty()) v.push_back(i); }
    return v;
}

std::shared_ptr<DeviceConn> DeviceHub::find(const std::string& msisdn, const std::string& service) {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& c : m_conns) {
        if (!c->alive()) continue;
        for (auto& a : c->info().accounts)
            if (a.msisdn == msisdn && (service.empty() || a.service == service)) return c;
    }
    return nullptr;
}

int DeviceHub::connected() {
    std::lock_guard<std::mutex> lk(m_mtx);
    int n = 0;
    for (auto& c : m_conns) if (c->alive()) n++;
    return n;
}
