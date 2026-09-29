#include "net/tls_stream.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

#include <cstring>
#include <ctime>
#ifdef _WIN32
#  define cimsue_timegm _mkgmtime
#else
#  define cimsue_timegm timegm
#endif

namespace cimsue {
namespace net {

namespace {

// 소켓 층 — BSD 소켓(POSIX) / winsock(Windows) 차이는 여기서만 흡수한다. 그 위 TLS 는 공통.
#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
void closeSock(sock_t s) { closesocket(s); }
void setTimeout(sock_t s, int sec) {
    DWORD ms = (DWORD)sec * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
}
void ensureWinsock() {
    static struct Init { Init() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } } init;
}
int pollIn(sock_t s, int ms) {
    WSAPOLLFD p{}; p.fd = s; p.events = POLLRDNORM;
    int r = WSAPoll(&p, 1, ms);
    return r < 0 ? -1 : r;
}
void shutdownBoth(sock_t s) { shutdown(s, SD_BOTH); }
#else
using sock_t = int;
constexpr sock_t kBadSock = -1;
void closeSock(sock_t s) { ::close(s); }
void setTimeout(sock_t s, int sec) {
    struct timeval tv{sec, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
void ensureWinsock() {}
int pollIn(sock_t s, int ms) {
    struct pollfd p{}; p.fd = s; p.events = POLLIN;
    int r = ::poll(&p, 1, ms);
    return r < 0 ? -1 : r;
}
void shutdownBoth(sock_t s) { ::shutdown(s, SHUT_RDWR); }
#endif

sock_t connectTcp(const std::string& host, int port, int timeoutSec) {
    ensureWinsock();
    struct addrinfo hints; std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return kBadSock;
    sock_t fd = kBadSock;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kBadSock) continue;
        setTimeout(fd, timeoutSec);
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closeSock(fd); fd = kBadSock;
    }
    freeaddrinfo(res);
    return fd;
}

/** 문자열이 IP 리터럴(v4/v6)인가 — SNI 에 넣지 않기 위한 판별(RFC 6066 §3). */
bool isIpAddress(const std::string& h) {
    unsigned char buf[16];
    return inet_pton(AF_INET, h.c_str(), buf) == 1 || inet_pton(AF_INET6, h.c_str(), buf) == 1;
}

bool loadCa(SSL_CTX* ctx, const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) return false;
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    bool any = false;
    for (;;) {
        X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!x) break;
        X509_STORE_add_cert(store, x);
        X509_free(x);
        any = true;
    }
    BIO_free(bio);
    return any;
}

}  // namespace

struct Stream::Impl {
    sock_t fd = kBadSock;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
};

Stream::~Stream() { close(); }

bool Stream::isOpen() const { return impl_ && impl_->fd != kBadSock; }

void Stream::close() {
    if (!impl_) return;
    if (impl_->ssl) { SSL_shutdown(impl_->ssl); SSL_free(impl_->ssl); }
    if (impl_->ctx) SSL_CTX_free(impl_->ctx);
    if (impl_->fd != kBadSock) closeSock(impl_->fd);
    delete impl_;
    impl_ = nullptr;
}

bool Stream::open(const std::string& host, int port, int timeoutSec, const TlsOptions& opt, std::string& err) {
    close();
    peer_ = PeerCert{};
    impl_ = new Impl();
    Impl& c = *impl_;
    c.fd = connectTcp(host, port, timeoutSec);
    if (c.fd == kBadSock) { err = "connect failed"; close(); return false; }
    if (!opt.tls) return true;
    c.ctx = SSL_CTX_new(TLS_client_method());
    if (!c.ctx) { err = "ssl ctx"; close(); return false; }
    SSL_CTX_set_min_proto_version(c.ctx, TLS1_2_VERSION);
    if (opt.verify) {
        if (!opt.caPem.empty()) loadCa(c.ctx, opt.caPem); else SSL_CTX_set_default_verify_paths(c.ctx);
        SSL_CTX_set_verify(c.ctx, SSL_VERIFY_PEER, nullptr);
    }
    c.ssl = SSL_new(c.ctx);
    SSL_set_fd(c.ssl, (int)c.fd);
    // 접속 주소가 IP 리터럴이면 iPAddress SAN 을, 이름이면 DNS SAN 을 검사한다.
    //   X509_check_host(= SSL_set1_host)는 iPAddress SAN 을 보지 않는다 — IP 로 붙으면서 이것만 걸면
    //   인증서에 IP SAN 이 있어도 X509_V_ERR_HOSTNAME_MISMATCH(62) 로 떨어진다. CIMS 는 IP 접속이 정상이다.
    //   SSL_set1_ip_asc 는 문자열이 IP 일 때만 1 을 돌려주므로 IP 리터럴 판별을 겸한다.
    bool isIpLiteral = isIpAddress(host);
    if (opt.verify && isIpLiteral) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c.ssl), host.c_str());
    // SNI 에는 IP 리터럴을 넣지 않는다(RFC 6066 §3) — 이름일 때만 보낸다.
    if (!isIpLiteral) {
        SSL_set_tlsext_host_name(c.ssl, host.c_str());
        if (opt.verify) SSL_set1_host(c.ssl, host.c_str());
    }
    if (SSL_connect(c.ssl) != 1) {
        unsigned long e = ERR_get_error();
        char buf[256]; ERR_error_string_n(e, buf, sizeof buf);
        err = std::string("tls handshake: ") + buf;
        long vr = SSL_get_verify_result(c.ssl);
        if (vr != X509_V_OK) err += std::string(" (") + X509_verify_cert_error_string(vr) + ")";
        close();
        return false;
    }
    // 서버 인증서 — 만료 관측(sip_tls_signaling.md §8.6.2)·지문(계측 링크 TOFU)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (X509* px = SSL_get1_peer_certificate(c.ssl)) {
#else
    if (X509* px = SSL_get_peer_certificate(c.ssl)) {
#endif
        const ASN1_TIME* na = X509_get0_notAfter(px);
        struct tm tmv{};
        if (na && ASN1_TIME_to_tm(na, &tmv) == 1) peer_.notAfterEpoch = (int64_t)cimsue_timegm(&tmv);
        char sbuf[256] = {0};
        if (X509_NAME_oneline(X509_get_subject_name(px), sbuf, sizeof sbuf)) peer_.subject = sbuf;
        unsigned char md[EVP_MAX_MD_SIZE]; unsigned int mdLen = 0;
        if (X509_digest(px, EVP_sha256(), md, &mdLen) == 1) {
            static const char* hex = "0123456789abcdef";
            for (unsigned i = 0; i < mdLen; ++i) { peer_.sha256 += hex[md[i] >> 4]; peer_.sha256 += hex[md[i] & 15]; }
        }
        X509_free(px);
    }
    return true;
}

bool Stream::writeAll(const std::string& d) {
    if (!isOpen()) return false;
    size_t off = 0;
    while (off < d.size()) {
        int n = impl_->ssl ? SSL_write(impl_->ssl, d.data() + off, (int)(d.size() - off))
                           : (int)::send(impl_->fd, d.data() + off, (int)(d.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

int Stream::read(char* b, int n) {
    if (!isOpen()) return -1;
    return impl_->ssl ? SSL_read(impl_->ssl, b, n) : (int)::recv(impl_->fd, b, n, 0);
}

int Stream::waitReadable(int ms) {
    if (!isOpen()) return -1;
    if (impl_->ssl && SSL_pending(impl_->ssl) > 0) return 1;
    return pollIn(impl_->fd, ms);
}

void Stream::shutdownSocket() {
    if (impl_ && impl_->fd != kBadSock) shutdownBoth(impl_->fd);
}

}  // namespace net
}  // namespace cimsue
