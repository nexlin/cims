#include "https_client.h"

#include "net/tls_stream.h"

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

}  // namespace

Response OpenSslTransport::request(const std::string& method, const std::string& url,
                                   const std::map<std::string, std::string>& hdrs, const std::string& body) {
    Response r;
    Url u;
    if (!parseUrl(url, u)) { r.error = "bad url"; return r; }
    net::Stream c;
    net::TlsOptions to;
    to.tls = u.tls; to.verify = verify_; to.caPem = caPem_;
    std::string err;
    if (!c.open(u.host, u.port, timeoutSec_, to, err)) { r.error = err; return r; }
    // 서버 인증서 만료 관측 — 관제조작반 "서버 인증서 N일 후 만료" 경고의 입력(sip_tls_signaling.md §8.6.2).
    r.peerNotAfterEpoch = c.peer().notAfterEpoch;
    r.peerSubject = c.peer().subject;
    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) + "\r\n"
                      "User-Agent: CIMS-UE/libcimsue\r\nConnection: close\r\nAccept: */*\r\n";
    for (auto& kv : hdrs) req += kv.first + ": " + kv.second + "\r\n";
    if (!body.empty() || method == "POST" || method == "PUT") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    if (!c.writeAll(req)) { r.error = "write failed"; return r; }
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
