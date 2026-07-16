#include "PRtpRelay.h"
#include "PLog.h"
#include "PMcpttGroup.h"
#include "PSyncRtpRecorder.h"
#include <sys/time.h>

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

PRtpRelay::PRtpRelay(const std::string& name)
    : PHandler(name), _sessionId(name)
{
    time(&_lastActivityTime);
}

PRtpRelay::~PRtpRelay() { final(); }

bool PRtpRelay::init(const std::string& ip, unsigned int rtpPort, unsigned int videoPort) {
    PAutoLock lock(_mutex);

    if (!_rtpSock.open(ip, rtpPort)) return false;
    LOG_INFO("PRtpRelay", "init rtp %s:%d", ip.c_str(), rtpPort);

    if (!_rtcpSock.open(ip, rtpPort + 1)) return false;
    LOG_INFO("PRtpRelay", "init rtcp %s:%d", ip.c_str(), rtpPort + 1);

    if (videoPort > 0) {
        if (!_videoRtpSock.open(ip, videoPort)) return false;
        LOG_INFO("PRtpRelay", "init video rtp %s:%d", ip.c_str(), videoPort);
        if (!_videoRtcpSock.open(ip, videoPort + 1)) return false;
        LOG_INFO("PRtpRelay", "init video rtcp %s:%d", ip.c_str(), videoPort + 1);
    }

    _localPort = rtpPort;
    _localVideoPort = videoPort;
    return true;
}

bool PRtpRelay::final() {
    PAutoLock lock(_mutex);
    _rtcpSock.close();
    _videoRtpSock.close();
    _videoRtcpSock.close();
    _rtpSock.close();
    return true;
}

void PRtpRelay::reset() {
    // 녹취 종료 먼저 — _recorder 포인터는 lock 하에서 nullptr 로 교체되어
    // 이후 proc() 의 writePacket 경합이 차단된다. 실제 finishSegment + delete
    // 는 lock 을 놓은 상태에서 수행되어 파일 I/O 가 세션 상태 정리를 지연시키지 않는다.
    stopRecording();
    PAutoLock lock(_mutex);
    _sessionId = "";
    _group = nullptr;
    for (int i = 0; i < 2; ++i) _peers[i] = PeerInfo{};
}

void PRtpRelay::setGroup(PMcpttGroup* g) {
    PAutoLock lock(_mutex);
    _group = g;
}

// ═══════════════════════════════════════════════════════════════
//  Peer 설정
// ═══════════════════════════════════════════════════════════════

bool PRtpRelay::setRemote(const std::string& ip, unsigned int port, unsigned int videoPort, int peerIdx) {
    PAutoLock lock(_mutex);

    int idx = peerIdx;
    if (idx == -1) {
        if (_peers[0].active && _peers[0].ip == ip) idx = 0;
        else if (_peers[1].active && _peers[1].ip == ip) idx = 1;
        else if (!_peers[0].active) idx = 0;
        else if (!_peers[1].active) idx = 1;
        else idx = 1;
    }
    if (idx < 0 || idx > 1) return false;

    auto makeAddr = [](struct sockaddr_in& addr, const std::string& ip, int port) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        addr.sin_port = htons(port);
    };

    PeerInfo& p = _peers[idx];
    p.ip = ip; p.port = port; p.videoPort = videoPort; p.active = true;
    makeAddr(p.addrRtp, ip, port);
    makeAddr(p.addrRtcp, ip, port + 1);
    if (videoPort > 0) {
        makeAddr(p.addrVideoRtp, ip, videoPort);
        makeAddr(p.addrVideoRtcp, ip, videoPort + 1);
    }

    if (idx == 0) {
        _rtpSock.setRemote(ip, port);
        _rtcpSock.setRemote(ip, port + 1);
        if (videoPort > 0) {
            _videoRtpSock.setRemote(ip, videoPort);
            _videoRtcpSock.setRemote(ip, videoPort + 1);
        }
    }

    LOG_INFO("PRtpRelay", "setRemote peer[%d]=%s:%d video=%d session=%s",
             idx, ip.c_str(), port, videoPort, _sessionId.c_str());
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  전송
// ═══════════════════════════════════════════════════════════════

void PRtpRelay::sendVideoTo(const std::string& ip, int port, char* data, int len) {
    PAutoLock lock(_mutex);
    if (_videoRtpSock.getFd() != INVALID_SOCKET)
        _videoRtpSock.sendTo(data, len, ip, port);
}

// ═══════════════════════════════════════════════════════════════
//  Peer 식별 — SDP 선언 IP:포트 정확 일치만.
//    (NAT 대응 IP 학습·symmetric latch 제거 — 2026-07-16) 상용은 내부망(no-NAT)
//    이라 도착 소스가 SDP 선언 주소와 동일 → 정확 일치로 충분. 공인 노출 테스트망의
//    포트변환 NAT 는 미디어 미지원(호 연결만 검증, 미디어는 현장 확인).
// ═══════════════════════════════════════════════════════════════

int PRtpRelay::_findPeerIndex(const std::string& ip, int port, bool isVideo) {
    for (int i = 0; i < 2; ++i) {
        if (!_peers[i].active) continue;
        int peerPort = isVideo ? (int)_peers[i].videoPort : (int)_peers[i].port;
        if (peerPort == port && _peers[i].ip == ip) return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════
//  Worker 메인 루프
// ═══════════════════════════════════════════════════════════════

static int64_t _getTimeUsec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

bool PRtpRelay::proc() {
    std::string ip;
    int port;
    char pkt[2048];

    // ── Audio RTCP relay ──
    while (_rtcpSock.getFd() != INVALID_SOCKET) {
        int len;
        PMcpttGroup* pGroup;
        {
            PAutoLock lock(_mutex);
            len = _rtcpSock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
        }
        if (len <= 0) break;
        if (pGroup) {
            pGroup->onRtcpPacket(ip, port, pkt, len);
        } else {
            PAutoLock lock(_mutex);
            int srcIdx = _findPeerIndex(ip, port - 1, false);
            if (srcIdx >= 0) {
                int dst = 1 - srcIdx;
                if (_peers[dst].active)
                    _rtcpSock.sendTo(pkt, len, &_peers[dst].addrRtcp);
            }
        }
    }

    // ── Video RTCP relay ──
    while (_videoRtcpSock.getFd() != INVALID_SOCKET) {
        int len;
        {
            PAutoLock lock(_mutex);
            len = _videoRtcpSock.recv(pkt, sizeof(pkt), ip, port);
            if (len > 0) {
                int srcIdx = _findPeerIndex(ip, port - 1, true);
                if (srcIdx >= 0) {
                    int dst = 1 - srcIdx;
                    if (_peers[dst].active && _peers[dst].videoPort > 0)
                        _videoRtcpSock.sendTo(pkt, len, &_peers[dst].addrVideoRtcp);
                }
            }
        }
        if (len <= 0) break;
    }

    // ── Audio RTP relay + 녹취 ──
    while (_rtpSock.getFd() != INVALID_SOCKET) {
        int len;
        PMcpttGroup* pGroup;
        {
            PAutoLock lock(_mutex);
            len = _rtpSock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
        }
        if (len <= 0) break;
        touchActivity();

        if (pGroup) {
            pGroup->onRtpPacket(ip, port, pkt, len);
        } else {
            PAutoLock lock(_mutex);
            int srcIdx = _findPeerIndex(ip, port, false);
            if (srcIdx >= 0) {
                int dst = 1 - srcIdx;
                if (_peers[dst].active)
                    _rtpSock.sendTo(pkt, len, &_peers[dst].addrRtp);

                if (_recorder) {
                    if (!_firstRtpReceived) {
                        _firstRtpReceived = true;
                        _segStartUsec = _getTimeUsec();
                    }
                    _recorder->writePacket(srcIdx == 0 ? "a" : "b", pkt, len);

                    int64_t now = _getTimeUsec();
                    if (_segStartUsec > 0 && (now - _segStartUsec) >= (int64_t)_segmentIntervalSec * 1000000LL) {
                        _recorder->finishSegment();
                        _recorder->startSegment(_recorder->getCurrentSeq() + 1);
                        _segStartUsec = now;
                    }
                }
            }
        }
    }

    // ── Video RTP relay + 녹취 ──
    while (_videoRtpSock.getFd() != INVALID_SOCKET) {
        int len;
        PMcpttGroup* pGroup;
        {
            PAutoLock lock(_mutex);
            len = _videoRtpSock.recv(pkt, sizeof(pkt), ip, port);
            pGroup = _group;
        }
        if (len <= 0) break;

        if (pGroup) {
            pGroup->onVideoRtpPacket(ip, port, pkt, len);
        } else {
            PAutoLock lock(_mutex);
            int srcIdx = _findPeerIndex(ip, port, true);
            if (srcIdx >= 0) {
                int dst = 1 - srcIdx;
                if (_peers[dst].active && _peers[dst].videoPort > 0)
                    _videoRtpSock.sendTo(pkt, len, &_peers[dst].addrVideoRtp);

                if (_recorder)
                    _recorder->writePacket(srcIdx == 0 ? "va" : "vb", pkt, len);
            }
        }
    }

    return false;
}

void PRtpRelay::collectFds(std::vector<int>& out) const {
    const int fds[] = { _rtpSock.getFd(), _rtcpSock.getFd(),
                        _videoRtpSock.getFd(), _videoRtcpSock.getFd() };
    for (int fd : fds)
        if (fd != INVALID_SOCKET) out.push_back(fd);
}

// ═══════════════════════════════════════════════════════════════
//  녹취
// ═══════════════════════════════════════════════════════════════

void PRtpRelay::startRecording(const std::string& rawDir, const std::string& sessionId,
                               const std::string& caller, const std::string& callee,
                               int segmentIntervalSec) {
    _segmentIntervalSec = segmentIntervalSec;
    _segStartUsec = 0;
    _firstRtpReceived = false;

    _recorder = new PSyncRtpRecorder(rawDir, "voip", caller, callee);
    _recorder->addTrack("a");
    _recorder->addTrack("b");
    _recorder->addTrack("va");
    _recorder->addTrack("vb");
    _recorder->startSegment(1);

    LOG_INFO("PRtpRelay", "Recording started: dir=%s session=%s interval=%ds",
             rawDir.c_str(), sessionId.c_str(), segmentIntervalSec);
}

void PRtpRelay::stopRecording() {
    // 포인터 swap 은 lock 하에 수행해 proc() 의 동시 writePacket 경합을 막는다.
    // finishSegment + delete 는 lock 바깥에서 실행되어 파일 I/O 중에도 RTP relay 가
    // 블로킹되지 않는다. finishSegment 내부는 _closeTrack() 가 fclose → rename 을
    // 수행하므로 .recording 임시 파일이 최종 파일로 승격되어 녹취가 온전히 마감된다.
    PSyncRtpRecorder* oldRecorder = nullptr;
    {
        PAutoLock lock(_mutex);
        oldRecorder = _recorder;
        _recorder = nullptr;
        _firstRtpReceived = false;
        _segStartUsec = 0;
    }
    if (!oldRecorder) return;

    if (oldRecorder->isActive())
        oldRecorder->finishSegment();

    delete oldRecorder;

    LOG_INFO("PRtpRelay", "Recording stopped");
}
