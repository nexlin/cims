#include "HttpServer.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

bool HttpServer::start(const std::string& ip, int port, Handler h, std::string& err) {
    m_handler = h;
    m_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_fd < 0) { err = "socket() failed"; return false; }
    int one = 1;
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = ip.empty() ? htonl(INADDR_ANY) : inet_addr(ip.c_str());
    if (bind(m_fd, (sockaddr*)&a, sizeof(a)) < 0) {
        err = "bind " + ip + ":" + std::to_string(port) + " failed: " + strerror(errno);
        close(m_fd); m_fd = -1;
        return false;
    }
    if (listen(m_fd, 16) < 0) { err = "listen() failed"; close(m_fd); m_fd = -1; return false; }
    m_port = port;
    if (port == 0) {   // OS 자동 포트(단위시험) — 실제 bind 포트를 기록
        sockaddr_in b{}; socklen_t bl = sizeof(b);
        if (getsockname(m_fd, (sockaddr*)&b, &bl) == 0) m_port = ntohs(b.sin_port);
    }
    m_stop = false;
    m_thread = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop() {
    if (m_fd < 0) return;
    m_stop = true;
    shutdown(m_fd, SHUT_RDWR);
    close(m_fd);
    m_fd = -1;
    if (m_thread.joinable()) m_thread.join();
}

void HttpServer::acceptLoop() {
    while (!m_stop) {
        pollfd pfd{ m_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0) continue;
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        int cfd = accept(m_fd, (sockaddr*)&ca, &cl);
        if (cfd < 0) continue;
        std::thread([this, cfd] { serve(cfd); }).detach();
    }
}

static bool readAll(int fd, std::string& buf, size_t& headerEnd, size_t& contentLen) {
    char tmp[4096];
    headerEnd = std::string::npos;
    contentLen = 0;
    for (int iter = 0; iter < 4096; ++iter) {
        if (headerEnd != std::string::npos && buf.size() >= headerEnd + 4 + contentLen) return true;
        pollfd pfd{ fd, POLLIN, 0 };
        if (poll(&pfd, 1, 10000) <= 0) return false;   // 10 초 무입력
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return headerEnd != std::string::npos && buf.size() >= headerEnd + 4 + contentLen;
        buf.append(tmp, (size_t)n);
        if (headerEnd == std::string::npos) {
            headerEnd = buf.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                // Content-Length (대소문자 무시)
                std::string lower = buf.substr(0, headerEnd);
                for (auto& c : lower) c = (char)tolower(c);
                size_t p = lower.find("content-length:");
                if (p != std::string::npos) contentLen = (size_t)strtoul(lower.c_str() + p + 15, nullptr, 10);
                if (contentLen > 64 * 1024 * 1024) return false;
            }
        }
    }
    return false;
}

void HttpServer::serve(int cfd) {
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::string buf;
    size_t he = 0, cl = 0;
    HttpResponse resp;
    if (!readAll(cfd, buf, he, cl)) {
        resp.status = 400;
        resp.body = "{\"error\":\"bad_request\"}";
    } else {
        HttpRequest req;
        size_t sp1 = buf.find(' ');
        size_t sp2 = sp1 == std::string::npos ? std::string::npos : buf.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 < he) {
            req.method = buf.substr(0, sp1);
            std::string target = buf.substr(sp1 + 1, sp2 - sp1 - 1);
            size_t q = target.find('?');
            req.path = q == std::string::npos ? target : target.substr(0, q);
            req.query = q == std::string::npos ? "" : target.substr(q + 1);
            req.body = buf.substr(he + 4, cl);
            try {
                resp = m_handler(req);
            } catch (const std::exception& e) {
                resp.status = 500;
                resp.body = std::string("{\"error\":\"handler_exception\",\"detail\":\"") + e.what() + "\"}";
            }
        } else {
            resp.status = 400;
            resp.body = "{\"error\":\"bad_request_line\"}";
        }
    }
    const char* reason = resp.status == 200 ? "OK" : resp.status == 201 ? "Created" : resp.status == 202 ? "Accepted"
                       : resp.status == 400 ? "Bad Request" : resp.status == 404 ? "Not Found"
                       : resp.status == 409 ? "Conflict" : resp.status == 503 ? "Service Unavailable" : "Error";
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + reason + "\r\n"
                      "Content-Type: " + resp.contentType + "\r\n"
                      "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + resp.body;
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = send(cfd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += (size_t)n;
    }
    shutdown(cfd, SHUT_WR);
    close(cfd);
}
