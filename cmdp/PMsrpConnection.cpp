#include "PMsrpConnection.h"
#include "PCmdpServer.h"
#include "PLog.h"
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// 리액터 1회 호출당 읽기 상한 — 한 연결이 리액터를 독점하지 않게 한다 (level-triggered
// 라 미처리분은 다음 epoll_wait 가 재통지).
static const size_t kReadCapPerProc = 256 * 1024;

PMsrpConnection::PMsrpConnection(int fd, int epfd, PCmdpServer* server, const std::string& peer)
    : PHandler("msrp_conn"), _fd(fd), _epfd(epfd), _server(server), _peer(peer) {
}

PMsrpConnection::~PMsrpConnection() {
    if (_fd >= 0) ::close(_fd);
}

void PMsrpConnection::markClosed() {
    if (_closed) return;
    _closed = true;
    _server->onConnectionClosed(this);  // epoll DEL + 세션 분리 + tombstone
}

bool PMsrpConnection::proc() {
    if (_closed) return false;

    flushWrite();

    char buf[16 * 1024];
    size_t total = 0;
    while (total < kReadCapPerProc) {
        ssize_t n = recv(_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            total += (size_t)n;
            _parser.feed(buf, (size_t)n);
            PMsrpMessage msg;
            while (_parser.next(msg)) {
                if (!_server->onMsrpFrame(this, msg)) {
                    markClosed();
                    return false;
                }
            }
            if (_parser.hasError()) {
                LOG_WARN("PMsrpConnection", "framing error from %s: %s", _peer.c_str(),
                         _parser.errorReason().c_str());
                markClosed();
                return false;
            }
            continue;
        }
        if (n == 0) {  // 상대 종료
            markClosed();
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_WARN("PMsrpConnection", "recv error from %s: %s", _peer.c_str(), strerror(errno));
        markClosed();
        return false;
    }
    return true;
}

void PMsrpConnection::queueWrite(const std::string& data) {
    if (_closed || data.empty()) return;
    std::lock_guard<std::mutex> lk(_outMtx);
    _outBuf.append(data);
    // 즉시 송신 시도 — 남으면 EPOLLOUT arm
    while (!_outBuf.empty()) {
        ssize_t n = send(_fd, _outBuf.data(), _outBuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            _outBuf.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            armEpollOut(true);
            return;
        }
        if (n < 0 && errno == EINTR) continue;
        // 송신 실패 — 리액터의 다음 이벤트(RDHUP)에서 정리됨
        LOG_WARN("PMsrpConnection", "send error to %s: %s", _peer.c_str(), strerror(errno));
        return;
    }
    armEpollOut(false);
}

void PMsrpConnection::flushWrite() {
    std::lock_guard<std::mutex> lk(_outMtx);
    while (!_outBuf.empty()) {
        ssize_t n = send(_fd, _outBuf.data(), _outBuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            _outBuf.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  // EPOLLOUT 유지
        if (n < 0 && errno == EINTR) continue;
        return;
    }
    armEpollOut(false);
}

void PMsrpConnection::armEpollOut(bool on) {
    if (_epollOutArmed == on || _epfd < 0 || _fd < 0) return;
    struct epoll_event ev {};
    ev.events = EPOLLIN | EPOLLRDHUP | (on ? EPOLLOUT : 0);
    ev.data.ptr = static_cast<PHandler*>(this);
    if (epoll_ctl(_epfd, EPOLL_CTL_MOD, _fd, &ev) == 0) _epollOutArmed = on;
}
