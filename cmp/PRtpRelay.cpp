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
    p.declaredIp = ip;   // SDP 선언 주소 보존 — latch 자격(사설 IP) 판정용
    p.natLatchedRtp = p.natLatchedVideo = false;
    p.natLatchedRtcp = p.natLatchedVideoRtcp = false;   // 재-INVITE 로 주소 갱신 시 재latch 허용
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
//  Peer 식별 (포트 매칭 + IP 학습)
// ═══════════════════════════════════════════════════════════════

// RFC1918/링크로컬 — SDP 선언 주소가 사설이면 NAT 뒤 단말로 보고 latch 자격을 준다.
static bool _isPrivateIp(const std::string& ip) {
    unsigned a = 0, b = 0;
    if (sscanf(ip.c_str(), "%u.%u", &a, &b) != 2) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;
    return false;
}

int PRtpRelay::_findPeerIndex(const std::string& ip, int port, bool isVideo) {
    // 1) 관측 소스 정확 일치 (latch 완료된 peer 포함)
    for (int i = 0; i < 2; ++i) {
        if (!_peers[i].active) continue;
        int peerPort = isVideo ? (int)_peers[i].videoPort : (int)_peers[i].port;
        if (peerPort == port && _peers[i].ip == ip) return i;
    }
    // 2) 선언 포트 일치 — IP 만 학습 (포트 보존형 NAT / 멀티홈, 기존 동작)
    for (int i = 0; i < 2; ++i) {
        if (!_peers[i].active) continue;
        int peerPort = isVideo ? (int)_peers[i].videoPort : (int)_peers[i].port;
        if (peerPort == port) {
            if (_peers[i].ip != ip) {
                LOG_INFO("PRtpRelay", "IP learned peer[%d] %s->%s (port %d)",
                         i, _peers[i].ip.c_str(), ip.c_str(), port);
                _peers[i].ip = ip;
                _peers[i].addrRtp.sin_addr.s_addr = inet_addr(ip.c_str());
                _peers[i].addrRtcp.sin_addr.s_addr = inet_addr(ip.c_str());
                _peers[i].addrVideoRtp.sin_addr.s_addr = inet_addr(ip.c_str());
                _peers[i].addrVideoRtcp.sin_addr.s_addr = inet_addr(ip.c_str());
            }
            return i;
        }
    }
    // 3) symmetric-RTP latch — 포트변환 NAT. 선언 주소가 사설(그대로는 도달 불가)이고
    //    아직 latch 안 된 슬롯에 관측 소스를 latch. 미지 소스 유입은 사설 선언 슬롯에만
    //    허용해 제3자 스푸핑 표면을 줄인다. 두 flow 의 슬롯 배정은 도착 순서라 발/착
    //    라벨(녹취 a/b)이 뒤바뀔 수 있으나 relay 동작(상호 전달)에는 영향이 없다.
    for (int i = 0; i < 2; ++i) {
        PeerInfo& p = _peers[i];
        if (!p.active || !_isPrivateIp(p.declaredIp)) continue;
        bool& latched = isVideo ? p.natLatchedVideo : p.natLatchedRtp;
        if (latched) continue;
        p.ip = ip;
        if (isVideo) {
            p.videoPort = port;
            p.addrVideoRtp.sin_addr.s_addr = inet_addr(ip.c_str());
            p.addrVideoRtp.sin_port = htons(port);
            // RTCP 는 실제 소스 관측 전까지 port+1 로 추정 (RTCP latch 시 교정)
            if (!p.natLatchedVideoRtcp) {
                p.addrVideoRtcp.sin_addr.s_addr = inet_addr(ip.c_str());
                p.addrVideoRtcp.sin_port = htons(port + 1);
            }
        } else {
            p.port = port;
            p.addrRtp.sin_addr.s_addr = inet_addr(ip.c_str());
            p.addrRtp.sin_port = htons(port);
            if (!p.natLatchedRtcp) {
                p.addrRtcp.sin_addr.s_addr = inet_addr(ip.c_str());
                p.addrRtcp.sin_port = htons(port + 1);
            }
        }
        latched = true;
        LOG_INFO("PRtpRelay", "%s addr latched (NAT) peer[%d] %s:%d session=%s",
                 isVideo ? "Video RTP" : "RTP", i, ip.c_str(), port, _sessionId.c_str());
        return i;
    }
    return -1;
}

// RTCP 전용 매칭 — RTP latch 를 오염시키지 않도록 분리. port 는 관측 RTCP 소스 포트 원본.
int PRtpRelay::_findPeerIndexRtcp(const std::string& ip, int port, bool isVideo) {
    // 1) 현재 RTCP 목적지와 정확 일치 (선언 port+1 또는 latch 된 소스)
    for (int i = 0; i < 2; ++i) {
        if (!_peers[i].active) continue;
        const struct sockaddr_in& a = isVideo ? _peers[i].addrVideoRtcp : _peers[i].addrRtcp;
        if (_peers[i].ip == ip && ntohs(a.sin_port) == (unsigned short)port) return i;
    }
    // 2) 선언 RTP 포트+1 규칙 (기존 동작: port-1 == RTP 포트)
    for (int i = 0; i < 2; ++i) {
        if (!_peers[i].active) continue;
        int peerPort = isVideo ? (int)_peers[i].videoPort : (int)_peers[i].port;
        if (peerPort == port - 1) return i;
    }
    // 3) RTCP latch — RTP 가 이미 latch 된 peer 와 같은 소스 IP 의 미지 RTCP 포트를 교정.
    //    (같은 공인 IP 뒤 두 단말이면 도착 순서 배정 — RTCP 오배달은 리포트 수준 영향)
    for (int i = 0; i < 2; ++i) {
        PeerInfo& p = _peers[i];
        if (!p.active || p.ip != ip) continue;
        bool rtpLatched = isVideo ? p.natLatchedVideo : p.natLatchedRtp;
        bool& rtcpLatched = isVideo ? p.natLatchedVideoRtcp : p.natLatchedRtcp;
        if (!rtpLatched || rtcpLatched) continue;
        struct sockaddr_in& a = isVideo ? p.addrVideoRtcp : p.addrRtcp;
        a.sin_addr.s_addr = inet_addr(ip.c_str());
        a.sin_port = htons(port);
        rtcpLatched = true;
        LOG_INFO("PRtpRelay", "%s addr latched (NAT) peer[%d] %s:%d session=%s",
                 isVideo ? "Video RTCP" : "RTCP", i, ip.c_str(), port, _sessionId.c_str());
        return i;
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
            int srcIdx = _findPeerIndexRtcp(ip, port, false);
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
                int srcIdx = _findPeerIndexRtcp(ip, port, true);
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
