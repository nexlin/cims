#include "https_client.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cstring>

namespace cimsue {
namespace http {

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string header(const Response& r, const std::string& name) {
    std::string k = name;
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    auto it = r.headers.find(k);
    return it == r.headers.end() ? std::string() : it->second;
}

namespace {

struct Url { bool tls = false; std::string host; int port = 0; std::string path; };

bool parseUrl(const std::string& url, Url& u) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    std::string scheme = url.substr(0, p);
    u.tls = scheme == "https";
    if (!u.tls && scheme != "http") return false;
    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hp = rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos && hp.find(']') == std::string::npos) { u.host = hp.substr(0, colon); u.port = std::atoi(hp.c_str() + colon + 1); }
    else { u.host = hp; u.port = u.tls ? 443 : 80; }
    return !u.host.empty() && u.port > 0;
}

int connectTcp(const std::string& host, int port, int timeoutSec) {
    struct addrinfo hints; std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv{timeoutSec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

struct Conn {
    int fd = -1; SSL_CTX* ctx = nullptr; SSL* ssl = nullptr;
    ~Conn() { if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); } if (ctx) SSL_CTX_free(ctx); if (fd >= 0) close(fd); }
    int write(const std::string& d) { return ssl ? SSL_write(ssl, d.data(), (int)d.size()) : (int)::send(fd, d.data(), d.size(), 0); }
    int read(char* b, int n) { return ssl ? SSL_read(ssl, b, n) : (int)::recv(fd, b, n, 0); }
};

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

Response OpenSslTransport::request(const std::string& method, const std::string& url,
                                   const std::map<std::string, std::string>& hdrs, const std::string& body) {
    Response r;
    Url u;
    if (!parseUrl(url, u)) { r.error = "bad url"; return r; }
    Conn c;
    c.fd = connectTcp(u.host, u.port, timeoutSec_);
    if (c.fd < 0) { r.error = "connect failed"; return r; }
    if (u.tls) {
        c.ctx = SSL_CTX_new(TLS_client_method());
        if (!c.ctx) { r.error = "ssl ctx"; return r; }
        if (verify_) {
            if (!caPem_.empty()) loadCa(c.ctx, caPem_); else SSL_CTX_set_default_verify_paths(c.ctx);
            SSL_CTX_set_verify(c.ctx, SSL_VERIFY_PEER, nullptr);
        }
        c.ssl = SSL_new(c.ctx);
        SSL_set_fd(c.ssl, c.fd);
        SSL_set_tlsext_host_name(c.ssl, u.host.c_str());
        if (verify_) SSL_set1_host(c.ssl, u.host.c_str());
        if (SSL_connect(c.ssl) != 1) {
            unsigned long e = ERR_get_error();
            char buf[256]; ERR_error_string_n(e, buf, sizeof buf);
            r.error = std::string("tls handshake: ") + buf;
            long vr = SSL_get_verify_result(c.ssl);
            if (vr != X509_V_OK) r.error += std::string(" (") + X509_verify_cert_error_string(vr) + ")";
            return r;
        }
    }
    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) + "\r\n"
                      "User-Agent: CIMS-UE/libcimsue\r\nConnection: close\r\nAccept: */*\r\n";
    for (auto& kv : hdrs) req += kv.first + ": " + kv.second + "\r\n";
    if (!body.empty() || method == "POST" || method == "PUT") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    if (c.write(req) <= 0) { r.error = "write failed"; return r; }
    std::string resp;
    char buf[8192];
    for (;;) {
        int n = c.read(buf, sizeof buf);
        if (n <= 0) break;
        resp.append(buf, n);
        if (resp.size() > (64u << 20)) break;
    }
    size_t hend = resp.find("\r\n\r\n");
    if (hend == std::string::npos) { r.error = "bad response"; return r; }
    std::string head = resp.substr(0, hend);
    std::string rest = resp.substr(hend + 4);
    size_t sp = head.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(head.c_str() + sp + 1);
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        size_t eol = head.find("\r\n", pos + 2);
        std::string line = head.substr(pos + 2, eol == std::string::npos ? std::string::npos : eol - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon), v = line.substr(colon + 1);
            for (auto& ch : k) ch = (char)std::tolower((unsigned char)ch);
            size_t b = v.find_first_not_of(" \t"); v = b == std::string::npos ? "" : v.substr(b);
            r.headers[k] = v;
        }
        pos = eol;
    }
    if (header(r, "transfer-encoding").find("chunked") != std::string::npos) {
        size_t p = 0;
        while (p < rest.size()) {
            size_t eol = rest.find("\r\n", p);
            if (eol == std::string::npos) break;
            size_t len = std::strtoul(rest.substr(p, eol - p).c_str(), nullptr, 16);
            if (len == 0) break;
            r.body += rest.substr(eol + 2, len);
            p = eol + 2 + len + 2;
        }
    } else {
        r.body = rest;
    }
    return r;
}

}  // namespace http
}  // namespace cimsue
