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

bool PRtpMulticast::init(const std::string& ip, unsigned int floorPort) {
    PAutoLock lock(_mutex);

    if (!_floorSock.open(ip, floorPort)) return false;
    LOG_INFO("PRtpMulticast", "init floor %s:%d", ip.c_str(), floorPort);

    _localFloorPort = floorPort;
    return true;
}

bool PRtpMulticast::final() {
    PAutoLock lock(_mutex);
    _floorSock.close();
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

void PRtpMulticast::sendFloorTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_floorSock.getFd() != INVALID_SOCKET)
        _floorSock.sendTo(data, len, ip, port);
}

// ═══════════════════════════════════════════════════════════════
//  Worker 메인 루프
// ═══════════════════════════════════════════════════════════════

bool PRtpMulticast::proc() {
    std::string ip;
    int port;
    char pkt[2048];

    while (_floorSock.getFd() != INVALID_SOCKET) {
        int len;
        PMcpttGroup* pGroup;
        {
            PAutoLock lock(_mutex);
            len = _floorSock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
        }
        if (len <= 0) break;
        if (pGroup) pGroup->onFloorPacket(ip, port, pkt, len);
    }

    return false;
}

void PRtpMulticast::collectFds(std::vector<int>& out) const {
    if (_floorSock.getFd() != INVALID_SOCKET) out.push_back(_floorSock.getFd());
}
