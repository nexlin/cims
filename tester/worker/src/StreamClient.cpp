#include "StreamClient.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static const size_t kMaxBacklog = 20000;

void StreamClient::start(const std::string& dest, const std::string& hello) {
    size_t c = dest.rfind(':');
    m_host = c == std::string::npos ? dest : dest.substr(0, c);
    m_port = c == std::string::npos ? 7110 : atoi(dest.c_str() + c + 1);
    m_hello = hello;
    m_stop = false;
    m_thread = std::thread([this] { loop(); });
}

void StreamClient::stop() {
    if (!m_thread.joinable()) return;
    m_stop = true;
    m_cv.notify_all();
    m_thread.join();
    if (m_fd >= 0) { close(m_fd); m_fd = -1; }
    m_connected = false;
}

void StreamClient::send(const std::string& line) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_q.size() >= kMaxBacklog) m_q.pop_front();
    m_q.push_back(line + "\n");
    m_cv.notify_one();
}

size_t StreamClient::backlog() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_q.size();
}

bool StreamClient::connectOnce() {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(m_host.c_str(), std::to_string(m_port).c_str(), &hints, &res) != 0 || !res) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return false; }
    timeval tv{ 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    bool ok = connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) { close(fd); return false; }
    m_fd = fd;
    return true;
}

bool StreamClient::writeAll(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(m_fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

void StreamClient::loop() {
    int backoffMs = 500;
    while (!m_stop) {
        if (m_fd < 0) {
            if (!connectOnce()) {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait_for(lk, std::chrono::milliseconds(backoffMs), [this] { return m_stop.load(); });
                backoffMs = backoffMs < 8000 ? backoffMs * 2 : 8000;
                continue;
            }
            backoffMs = 500;
            m_connected = true;
            if (!m_hello.empty() && !writeAll(m_hello + "\n")) { close(m_fd); m_fd = -1; m_connected = false; continue; }
        }
        std::string line;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait_for(lk, std::chrono::milliseconds(500), [this] { return m_stop.load() || !m_q.empty(); });
            if (m_q.empty()) continue;
            line = m_q.front();
            m_q.pop_front();
        }
        if (!writeAll(line)) {
            close(m_fd); m_fd = -1; m_connected = false;
            std::lock_guard<std::mutex> lk(m_mtx);
            m_q.push_front(line);   // 재접속 뒤 다시 보낸다
        }
    }
    if (m_fd >= 0) {
        // 종료 전 남은 것 flush (최대 2 초)
        std::deque<std::string> rest;
        { std::lock_guard<std::mutex> lk(m_mtx); rest.swap(m_q); }
        for (auto& l : rest) if (!writeAll(l)) break;
    }
}
