#include "PMcvMemberPort.h"
#include "PLog.h"
#include "PMcvideoGroup.h"

PMcvMemberPort::PMcvMemberPort(const std::string& name)
    : PHandler(name)
{
}

PMcvMemberPort::~PMcvMemberPort() { final(); }

bool PMcvMemberPort::init(const std::string& ip, unsigned int basePort) {
    PAutoLock lock(_mutex);

    if (!_audioSock.open(ip, basePort)) return false;
    if (!_videoSock.open(ip, basePort + 2)) return false;
    if (!_videoRtcpSock.open(ip, basePort + 3)) return false;
    if (!_controlSock.open(ip, basePort + 4)) return false;
    _basePort = basePort;
    LOG_INFO("PMcvMemberPort", "init %s audio=%u video=%u video_rtcp=%u control=%u", ip.c_str(), basePort,
             basePort + 2, basePort + 3, basePort + 4);
    return true;
}

bool PMcvMemberPort::final() {
    PAutoLock lock(_mutex);
    _controlSock.close();
    _videoRtcpSock.close();
    _videoSock.close();
    _audioSock.close();
    _group.reset();
    return true;
}

void PMcvMemberPort::bind(const std::shared_ptr<PMcvideoGroup>& g, const std::string& memberId) {
    PAutoLock lock(_mutex);
    _group = g;
    _memberId = memberId;
}

void PMcvMemberPort::reset() {
    PAutoLock lock(_mutex);
    _group.reset();
    _memberId = "";
}

PRtpSocket* PMcvMemberPort::_sock(McvChannel ch) {
    switch (ch) {
        case MCV_CH_AUDIO:      return &_audioSock;
        case MCV_CH_VIDEO:      return &_videoSock;
        case MCV_CH_VIDEO_RTCP: return &_videoRtcpSock;
        case MCV_CH_CONTROL:    return &_controlSock;
    }
    return nullptr;
}

void PMcvMemberPort::sendTo(McvChannel ch, const std::string& ip, int port, const char* data, int len) {
    PAutoLock lock(_mutex);
    PRtpSocket* s = _sock(ch);
    if (s && s->getFd() != INVALID_SOCKET && port > 0)
        s->sendTo(data, len, ip, port);
}

bool PMcvMemberPort::proc() {
    std::string ip;
    int port;
    char pkt[2048];
    static const McvChannel kChannels[] = { MCV_CH_AUDIO, MCV_CH_VIDEO, MCV_CH_VIDEO_RTCP, MCV_CH_CONTROL };

    // 채널마다 drain — 이 소켓의 멤버 신원으로 그룹에 전달. 그룹 콜백은 유닛 락 밖에서 부른다
    //   (그룹이 하향 송신으로 다른 유닛 락을 잡으므로 — 락 순서 = 그룹 → 유닛).
    for (McvChannel ch : kChannels) {
        PRtpSocket* s = _sock(ch);
        while (s->getFd() != INVALID_SOCKET) {
            int len;
            std::shared_ptr<PMcvideoGroup> pGroup;
            std::string memberId;
            {
                PAutoLock lock(_mutex);
                len = s->recv(pkt, sizeof(pkt), ip, port);
                pGroup = _group;
                memberId = _memberId;
            }
            if (len <= 0) break;
            if (pGroup && !memberId.empty())
                pGroup->onMemberPacket(memberId, ch, ip, port, pkt, len);
        }
    }
    return false;
}

void PMcvMemberPort::collectFds(std::vector<int>& out) const {
    const int fds[] = { _audioSock.getFd(), _videoSock.getFd(), _videoRtcpSock.getFd(), _controlSock.getFd() };
    for (int fd : fds)
        if (fd != INVALID_SOCKET) out.push_back(fd);
}
