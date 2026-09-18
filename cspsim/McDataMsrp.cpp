#include "McDataMsrp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>

namespace csim_msrp {

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string randHex(size_t n) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* h = "0123456789abcdef";
    std::string s;
    while (s.size() < n) { uint64_t v = rng(); for (int i = 0; i < 16 && s.size() < n; ++i) { s += h[v & 15]; v >>= 4; } }
    return s;
}

std::string Frame::header(const char* name) const {
    auto it = headers.find(lower(name));
    return it == headers.end() ? "" : it->second;
}

std::string newTransId() { return "t" + randHex(9); }
std::string newSessionId() { return randHex(12); }

bool extractFrame(std::string& buf, Frame& out) {
    size_t m = buf.find("MSRP ");
    if (m == std::string::npos) return false;
    size_t eol = buf.find("\r\n", m);
    if (eol == std::string::npos) return false;
    std::string start = buf.substr(m, eol - m);
    // "MSRP <tid> <method|code [phrase]>"
    size_t s1 = start.find(' ', 5);
    if (s1 == std::string::npos) return false;
    std::string tid = start.substr(5, s1 - 5);
    std::string endLine = "-------" + tid;
    size_t e = buf.find(endLine, eol);
    if (e == std::string::npos || e + endLine.size() + 3 > buf.size()) return false;   // 플래그 + CRLF
    out = Frame();
    out.tid = tid;
    std::string rest = start.substr(s1 + 1);
    if (!rest.empty() && std::isdigit((unsigned char)rest[0])) { out.request = false; out.status = atoi(rest.c_str()); }
    else { out.request = true; size_t sp = rest.find(' '); out.method = sp == std::string::npos ? rest : rest.substr(0, sp); }
    out.flag = buf[e + endLine.size()];
    // 헤더: start-line 다음부터 빈 줄까지(본문 없는 프레임은 end-line 직전까지)
    size_t p = eol + 2;
    size_t blank = buf.find("\r\n\r\n", p);
    size_t hdrEnd = (blank != std::string::npos && blank < e) ? blank : e;
    while (p < hdrEnd) {
        size_t le = buf.find("\r\n", p);
        if (le == std::string::npos || le > hdrEnd) le = hdrEnd;
        std::string line = buf.substr(p, le - p);
        size_t c = line.find(':');
        if (c != std::string::npos) out.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
        p = le + 2;
    }
    if (blank != std::string::npos && blank < e) {
        size_t bs = blank + 4;
        size_t be = e >= 2 && buf.compare(e - 2, 2, "\r\n") == 0 ? e - 2 : e;   // 본문 뒤 CRLF 는 end-line 의 것
        if (be > bs) out.body = buf.substr(bs, be - bs);
    }
    buf.erase(0, e + endLine.size() + 3);
    return true;
}

std::string buildSend(const std::string& tid, const std::string& toPath, const std::string& fromPath, const std::string& msgId,
                      const std::string& contentType, const std::string& body, bool successReport, char flag) {
    std::string h = "MSRP " + tid + " SEND\r\nTo-Path: " + toPath + "\r\nFrom-Path: " + fromPath + "\r\nMessage-ID: " + msgId + "\r\n";
    if (!contentType.empty()) h += "Byte-Range: 1-" + std::to_string(body.size()) + "/" + std::to_string(body.size()) + "\r\n";
    if (successReport) h += "Success-Report: yes\r\n";
    h += "Failure-Report: yes\r\n";
    if (contentType.empty()) return h + "-------" + tid + flag + "\r\n";
    return h + "Content-Type: " + contentType + "\r\n\r\n" + body + "\r\n-------" + tid + flag + "\r\n";
}

std::string buildResponse(const std::string& tid, int code, const std::string& toPath, const std::string& fromPath) {
    const char* phrase = code == 200 ? "OK" : code == 413 ? "Stop Sending Message" : code == 415 ? "Unsupported Media Type" : "Error";
    return "MSRP " + tid + " " + std::to_string(code) + " " + phrase + "\r\nTo-Path: " + toPath + "\r\nFrom-Path: " + fromPath +
           "\r\n-------" + tid + "$\r\n";
}

bool parsePath(const std::string& pathIn, std::string& host, int& port) {
    std::string path = pathIn;
    size_t sp = path.find(' ');
    if (sp != std::string::npos) path = path.substr(0, sp);
    size_t s = path.find("://");
    if (s == std::string::npos) return false;
    size_t hs = s + 3, he = path.find('/', hs);
    std::string hp = path.substr(hs, he == std::string::npos ? std::string::npos : he - hs);
    size_t c = hp.rfind(':');
    if (c == std::string::npos) { host = hp; port = 2855; }
    else { host = hp.substr(0, c); port = atoi(hp.c_str() + c + 1); }
    if (!host.empty() && host[0] == '[') host = host.substr(1, host.find(']') - 1);
    return !host.empty() && port > 0;
}

std::string localPath(const std::string& ip, const std::string& session) {
    return "msrp://" + ip + ":" + std::to_string(kLocalPort) + "/" + session + ";tcp";
}

// ── Client ──
bool Client::Connect(const std::string& host, int port, int timeoutMs) {
    Close();
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        hostent* he = gethostbyname(host.c_str());
        if (!he || he->h_addrtype != AF_INET) return false;
        memcpy(&a.sin_addr, he->h_addr_list[0], sizeof(a.sin_addr));
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, (sockaddr*)&a, sizeof(a));
    if (r != 0 && errno != EINPROGRESS) { close(fd); return false; }
    if (r != 0) {
        pollfd pf{fd, POLLOUT, 0};
        if (poll(&pf, 1, timeoutMs) <= 0) { close(fd); return false; }
        int err = 0; socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) { close(fd); return false; }
    }
    fcntl(fd, F_SETFL, fl);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    m_fd = fd;
    m_buf.clear();
    return true;
}

void Client::Close() {
    if (m_fd >= 0) { close(m_fd); m_fd = -1; }
}

bool Client::Send(const std::string& data) {
    if (m_fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(m_fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        off += (size_t)n;
    }
    return true;
}

bool Client::RecvFrame(Frame& out, int timeoutMs) {
    if (extractFrame(m_buf, out)) return true;
    if (m_fd < 0) return false;
    char tmp[65536];
    for (;;) {
        pollfd pf{m_fd, POLLIN, 0};
        int pr = poll(&pf, 1, timeoutMs);
        if (pr <= 0) return false;
        ssize_t n = recv(m_fd, tmp, sizeof(tmp), 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; Close(); return false; }
        m_buf.append(tmp, (size_t)n);
        if (extractFrame(m_buf, out)) return true;
    }
}

}  // namespace csim_msrp
