#include "PPttMemberPort.h"
#include "PLog.h"
#include "PMcpttGroup.h"

PPttMemberPort::PPttMemberPort(const std::string& name)
    : PHandler(name)
{
}

PPttMemberPort::~PPttMemberPort() { final(); }

bool PPttMemberPort::init(const std::string& ip, unsigned int audioPort, unsigned int videoPort) {
    PAutoLock lock(_mutex);

    if (!_audioSock.open(ip, audioPort)) return false;
    if (videoPort > 0) {
        if (!_videoSock.open(ip, videoPort)) return false;
    }
    _localAudioPort = audioPort;
    _localVideoPort = videoPort;
    LOG_INFO("PPttMemberPort", "init %s audio=%d video=%d", ip.c_str(), audioPort, videoPort);
    return true;
}

bool PPttMemberPort::final() {
    PAutoLock lock(_mutex);
    _videoSock.close();
    _audioSock.close();
    return true;
}

void PPttMemberPort::bind(PMcpttGroup* g, const std::string& memberId) {
    PAutoLock lock(_mutex);
    _group = g;
    _memberId = memberId;
}

void PPttMemberPort::reset() {
    PAutoLock lock(_mutex);
    _group = nullptr;
    _memberId = "";
}

void PPttMemberPort::sendAudioTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_audioSock.getFd() != INVALID_SOCKET)
        _audioSock.sendTo(data, len, ip, port);
}

void PPttMemberPort::sendVideoTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_videoSock.getFd() != INVALID_SOCKET)
        _videoSock.sendTo(data, len, ip, port);
}

bool PPttMemberPort::proc() {
    std::string ip;
    int port;
    char pkt[2048];

    // Audio RTP — 이 소켓의 멤버 신원으로 그룹에 전달
    while (_audioSock.getFd() != INVALID_SOCKET) {
        int len;
        PMcpttGroup* pGroup;
        std::string memberId;
        {
            PAutoLock lock(_mutex);
            len = _audioSock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
            memberId = _memberId;
        }
        if (len <= 0) break;
        if (pGroup && !memberId.empty())
            pGroup->onMemberRtpPacket(memberId, ip, port, pkt, len);
    }

    // Video RTP
    while (_videoSock.getFd() != INVALID_SOCKET) {
        int len;
        PMcpttGroup* pGroup;
        std::string memberId;
        {
            PAutoLock lock(_mutex);
            len = _videoSock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
            memberId = _memberId;
        }
        if (len <= 0) break;
        if (pGroup && !memberId.empty())
            pGroup->onMemberVideoRtpPacket(memberId, ip, port, pkt, len);
    }

    return false;
}

void PPttMemberPort::collectFds(std::vector<int>& out) const {
    const int fds[] = { _audioSock.getFd(), _videoSock.getFd() };
    for (int fd : fds)
        if (fd != INVALID_SOCKET) out.push_back(fd);
}
