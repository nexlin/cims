#include "PRtpMulticast.h"
#include "PLog.h"
#include "PMcpttGroup.h"

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

PRtpMulticast::PRtpMulticast(const std::string& name)
    : PHandler(name), _sessionId(name)
{
    time(&_lastActivityTime);
}

PRtpMulticast::~PRtpMulticast() { final(); }

bool PRtpMulticast::init(const std::string& ip, unsigned int rtpPort, unsigned int floorPort, unsigned int videoPort) {
    PAutoLock lock(_mutex);

    if (!_rtpSock.open(ip, rtpPort)) return false;
    LOG_INFO("PRtpMulticast", "init rtp %s:%d", ip.c_str(), rtpPort);

    if (floorPort > 0) {
        if (!_floorSock.open(ip, floorPort)) return false;
        LOG_INFO("PRtpMulticast", "init floor %s:%d", ip.c_str(), floorPort);
    }

    if (videoPort > 0) {
        if (!_videoRtpSock.open(ip, videoPort)) return false;
        LOG_INFO("PRtpMulticast", "init video %s:%d", ip.c_str(), videoPort);
    }

    _localRtpPort = rtpPort;
    _localFloorPort = floorPort;
    _localVideoPort = videoPort;
    return true;
}

bool PRtpMulticast::final() {
    PAutoLock lock(_mutex);
    _videoRtpSock.close();
    _floorSock.close();
    _rtpSock.close();
    return true;
}

void PRtpMulticast::reset() {
    PAutoLock lock(_mutex);
    _sessionId = "";
    _group = nullptr;
}

void PRtpMulticast::setGroup(PMcpttGroup* g) {
    PAutoLock lock(_mutex);
    _group = g;
}

// ═══════════════════════════════════════════════════════════════
//  전송 (McpttGroup에서 호출)
// ═══════════════════════════════════════════════════════════════

void PRtpMulticast::sendAudioTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_rtpSock.getFd() != INVALID_SOCKET)
        _rtpSock.sendTo(data, len, ip, port);
}

void PRtpMulticast::sendFloorTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_floorSock.getFd() != INVALID_SOCKET)
        _floorSock.sendTo(data, len, ip, port);
}

void PRtpMulticast::sendVideoTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_videoRtpSock.getFd() != INVALID_SOCKET)
        _videoRtpSock.sendTo(data, len, ip, port);
}

// ═══════════════════════════════════════════════════════════════
//  Worker 메인 루프
// ═══════════════════════════════════════════════════════════════

template<typename Callback>
void PRtpMulticast::_drainSocket(PRtpSocket& sock, Callback cb) {
    std::string ip;
    int port;
    char pkt[2048];

    while (true) {
        if (sock.getFd() == INVALID_SOCKET) break;

        int len;
        PMcpttGroup* pGroup;
        {
            PAutoLock lock(_mutex);
            len = sock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
        }
        if (len <= 0) break;

        if (pGroup) cb(pGroup, ip, port, pkt, len);
    }
}

bool PRtpMulticast::proc() {
    // Floor control → 상태 먼저 갱신
    _drainSocket(_floorSock, [](PMcpttGroup* g, const std::string& ip, int port, char* pkt, int len) {
        g->onFloorPacket(ip, port, pkt, len);
    });

    // Audio RTP
    _drainSocket(_rtpSock, [this](PMcpttGroup* g, const std::string& ip, int port, char* pkt, int len) {
        touchActivity();
        g->onRtpPacket(ip, port, pkt, len);
    });

    // Video RTP
    _drainSocket(_videoRtpSock, [this](PMcpttGroup* g, const std::string& ip, int port, char* pkt, int len) {
        touchActivity();
        g->onVideoRtpPacket(ip, port, pkt, len);
    });

    return false;
}

void PRtpMulticast::collectFds(std::vector<int>& out) const {
    const int fds[] = { _rtpSock.getFd(), _floorSock.getFd(), _videoRtpSock.getFd() };
    for (int fd : fds)
        if (fd != INVALID_SOCKET) out.push_back(fd);
}
