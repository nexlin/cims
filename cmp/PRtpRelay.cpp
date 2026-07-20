#include "PRtpRelay.h"
#include "PLog.h"
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

bool PRtpRelay::init(const std::string& ip, unsigned int basePort) {
    PAutoLock lock(_mutex);

    for (int i = 0; i < 2; ++i) {
        unsigned int p = basePort + i * 4;
        Leg& leg = _legs[i];
        if (!leg.rtp.open(ip, p)) return false;
        if (!leg.rtcp.open(ip, p + 1)) return false;
        if (!leg.videoRtp.open(ip, p + 2)) return false;
        if (!leg.videoRtcp.open(ip, p + 3)) return false;
        leg.localPort = p;
        leg.localVideoPort = p + 2;
    }
    LOG_INFO("PRtpRelay", "init %s:%d-%d (peer0=%d peer1=%d)", ip.c_str(),
             basePort, basePort + 7, basePort, basePort + 4);
    return true;
}

bool PRtpRelay::final() {
    PAutoLock lock(_mutex);
    for (int i = 0; i < 2; ++i) {
        _legs[i].rtcp.close();
        _legs[i].videoRtp.close();
        _legs[i].videoRtcp.close();
        _legs[i].rtp.close();
    }
    return true;
}

void PRtpRelay::reset() {
    // 녹취 종료 먼저 — _recorder 포인터는 lock 하에서 nullptr 로 교체되어
    // 이후 proc() 의 writePacket 경합이 차단된다. 실제 finishSegment + delete
    // 는 lock 을 놓은 상태에서 수행되어 파일 I/O 가 세션 상태 정리를 지연시키지 않는다.
    stopRecording();
    PAutoLock lock(_mutex);
    _sessionId = "";
    for (int i = 0; i < 2; ++i) {
        Leg& leg = _legs[i];
        leg.ip.clear();
        leg.port = leg.videoPort = 0;
        leg.declIp.clear();
        leg.declPort = leg.declVideoPort = 0;
        leg.active = false;
        memset(&leg.addrRtp, 0, sizeof(leg.addrRtp));
        memset(&leg.addrRtcp, 0, sizeof(leg.addrRtcp));
        memset(&leg.addrVideoRtp, 0, sizeof(leg.addrVideoRtp));
        memset(&leg.addrVideoRtcp, 0, sizeof(leg.addrVideoRtcp));
        leg.nat = false;
        leg.sigIp.clear();
        leg.latched = leg.latchedVideo = leg.latchedRtcp = leg.latchedVideoRtcp = false;
        leg.latchSsrc = leg.latchVideoSsrc = 0;
    }
}

// ═══════════════════════════════════════════════════════════════
//  Peer 설정
// ═══════════════════════════════════════════════════════════════

static void _makeAddr(struct sockaddr_in& addr, const std::string& ip, int port) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port = htons(port);
}

bool PRtpRelay::setRemote(const std::string& ip, unsigned int port, unsigned int videoPort, int peerIdx,
                          bool nat, const std::string& sigIp) {
    // 미확정 주소(빈 IP / 0.0.0.0 / port 0) — 목적지 미설정 유지 (0.0.0.0 sendto = 자기 자신 오송신 방지).
    //   해당 peer 주소는 이후 RELAY_MODIFY 로 확정된다 (cmp_media_api.md §6.1).
    if (ip.empty() || ip == "0.0.0.0" || port == 0) {
        LOG_INFO("PRtpRelay", "setRemote peer[%d] skip — undetermined remote (%s:%u) session=%s",
                 peerIdx, ip.c_str(), port, _sessionId.c_str());
        return false;
    }
    PAutoLock lock(_mutex);

    int idx = peerIdx;
    if (idx == -1) {
        if (_legs[0].active && _legs[0].ip == ip) idx = 0;
        else if (_legs[1].active && _legs[1].ip == ip) idx = 1;
        else if (!_legs[0].active) idx = 0;
        else if (!_legs[1].active) idx = 1;
        else idx = 1;
    }
    if (idx < 0 || idx > 1) return false;

    Leg& leg = _legs[idx];
    // 동일 선언 재수신(주소·nat·guard 불변) — latch/학습 목적지 유지. refresh 성 re-INVITE 나
    //   MODIFY 재전송이 활성 latch 를 풀어 선언(사설) 주소로 역행하는 것을 방지한다.
    //   비교는 선언 원본(decl*) 기준 — leg.ip/port 는 latch 시 학습 주소로 덮인다.
    if (leg.active && leg.declIp == ip && leg.declPort == port && leg.declVideoPort == videoPort &&
        leg.nat == nat && leg.sigIp == sigIp) {
        LOG_INFO("PRtpRelay", "setRemote peer[%d]=%s:%d unchanged — keep latch session=%s",
                 idx, ip.c_str(), port, _sessionId.c_str());
        return true;
    }
    leg.declIp = ip; leg.declPort = port; leg.declVideoPort = videoPort;
    leg.ip = ip; leg.port = port; leg.videoPort = videoPort; leg.active = true;
    _makeAddr(leg.addrRtp, ip, port);
    _makeAddr(leg.addrRtcp, ip, port + 1);
    if (videoPort > 0) {
        _makeAddr(leg.addrVideoRtp, ip, videoPort);
        _makeAddr(leg.addrVideoRtcp, ip, videoPort + 1);
    }
    // NAT latch 상태 리셋 — 주소 갱신(re-INVITE) 시 재-latch 허용
    leg.nat = nat;
    leg.sigIp = sigIp;
    leg.latched = leg.latchedVideo = leg.latchedRtcp = leg.latchedVideoRtcp = false;
    leg.latchSsrc = leg.latchVideoSsrc = 0;

    LOG_INFO("PRtpRelay", "setRemote peer[%d]=%s:%d video=%d nat=%d guard=%s (local=%d) session=%s",
             idx, ip.c_str(), port, videoPort, nat ? 1 : 0, sigIp.c_str(), leg.localPort, _sessionId.c_str());
    return true;
}

bool PRtpRelay::getNatLatched(int peerIdx, std::string& learnedIp, int& learnedPort) const {
    const Leg& leg = _legs[peerIdx & 1];
    if (!leg.active || !leg.nat || !leg.latched) return false;
    learnedIp = leg.ip;
    learnedPort = (int)leg.port;
    return true;
}

// nat leg 의 RTP 수신 판정 — 선언/latch 주소 일치가 아니면 목적지 latch 를 시도한다.
//   안전 조건: RTP v2 + 최소 길이 + (guard) 소스 IP == sigIp + SSRC 고정(재-latch 는
//   동일 SSRC = NAT rebind 추종만). 호출자가 _mutex 보유.
bool PRtpRelay::_acceptNatRtp(int legIdx, bool isVideo, const std::string& ip, int port, const char* pkt, int len) {
    Leg& leg = _legs[legIdx];
    if (len < 12 || (((unsigned char)pkt[0]) >> 6) != 2) return false;
    if (!leg.sigIp.empty() && leg.sigIp != ip) return false;

    uint32_t ssrc = ((uint32_t)(unsigned char)pkt[8] << 24) | ((uint32_t)(unsigned char)pkt[9] << 16) |
                    ((uint32_t)(unsigned char)pkt[10] << 8) | (uint32_t)(unsigned char)pkt[11];
    bool& latched = isVideo ? leg.latchedVideo : leg.latched;
    uint32_t& latchSsrc = isVideo ? leg.latchVideoSsrc : leg.latchSsrc;
    if (latched && ssrc != latchSsrc) return false;   // 제3자 주입 차단 (rebind 는 동일 SSRC)

    if (isVideo) {
        leg.videoPort = port;
        _makeAddr(leg.addrVideoRtp, ip, port);
        if (!leg.latchedVideoRtcp) _makeAddr(leg.addrVideoRtcp, ip, port + 1);
    } else {
        leg.port = port;
        _makeAddr(leg.addrRtp, ip, port);
        if (!leg.latchedRtcp) _makeAddr(leg.addrRtcp, ip, port + 1);
    }
    leg.ip = ip;
    latched = true;
    latchSsrc = ssrc;
    LOG_INFO("PRtpRelay", "%s dest latched (NAT) peer[%d] %s:%d ssrc=%u session=%s",
             isVideo ? "video RTP" : "RTP", legIdx, ip.c_str(), port, ssrc, _sessionId.c_str());
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Worker 메인 루프 — 수신 소켓이 곧 peer 신원 (leg 별 전용 포트)
// ═══════════════════════════════════════════════════════════════

static int64_t _getTimeUsec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

void PRtpRelay::_dropSrc(int legIdx, const char* what, const std::string& ip, int port, int expectedPort) {
    ++_srcDrop;
    time_t now; time(&now);
    if (now - _lastDropWarn >= 5) {
        _lastDropWarn = now;
        LOG_WARN("PRtpRelay", "drop %s from unnegotiated src %s:%d peer[%d] expected=%s:%d session=%s (total=%ld)",
                 what, ip.c_str(), port, legIdx, _legs[legIdx].ip.c_str(), expectedPort,
                 _sessionId.c_str(), _srcDrop);
    }
}

bool PRtpRelay::proc() {
    std::string ip;
    int port;
    char pkt[2048];

    for (int i = 0; i < 2; ++i) {
        const int dst = 1 - i;

        // ── Audio RTCP relay ──
        while (_legs[i].rtcp.getFd() != INVALID_SOCKET) {
            PAutoLock lock(_mutex);
            int len = _legs[i].rtcp.recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0) break;
            Leg& src = _legs[i];
            if (!src.active) continue;
            if (src.ip != ip || (int)src.port + 1 != port) {
                // nat leg: RTP latch 된(또는 선언) IP 와 일치하는 관측 소스로 RTCP 목적지 교정
                if (src.nat && src.ip == ip) {
                    _makeAddr(src.addrRtcp, ip, port);
                    src.latchedRtcp = true;
                    LOG_INFO("PRtpRelay", "RTCP dest latched (NAT) peer[%d] %s:%d session=%s",
                             i, ip.c_str(), port, _sessionId.c_str());
                } else {
                    _dropSrc(i, "rtcp", ip, port, (int)src.port + 1);
                    continue;
                }
            }
            if (_legs[dst].active)
                _legs[dst].rtcp.sendTo(pkt, len, &_legs[dst].addrRtcp);
        }

        // ── Video RTCP relay ──
        while (_legs[i].videoRtcp.getFd() != INVALID_SOCKET) {
            PAutoLock lock(_mutex);
            int len = _legs[i].videoRtcp.recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0) break;
            Leg& src = _legs[i];
            if (!src.active || src.videoPort == 0) continue;
            if (src.ip != ip || (int)src.videoPort + 1 != port) {
                if (src.nat && src.ip == ip) {
                    _makeAddr(src.addrVideoRtcp, ip, port);
                    src.latchedVideoRtcp = true;
                    LOG_INFO("PRtpRelay", "video RTCP dest latched (NAT) peer[%d] %s:%d session=%s",
                             i, ip.c_str(), port, _sessionId.c_str());
                } else {
                    _dropSrc(i, "video rtcp", ip, port, (int)src.videoPort + 1);
                    continue;
                }
            }
            if (_legs[dst].active && _legs[dst].videoPort > 0)
                _legs[dst].videoRtcp.sendTo(pkt, len, &_legs[dst].addrVideoRtcp);
        }

        // ── Audio RTP relay + 녹취 ──
        while (_legs[i].rtp.getFd() != INVALID_SOCKET) {
            PAutoLock lock(_mutex);
            int len = _legs[i].rtp.recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0) break;
            Leg& src = _legs[i];
            if (!src.active) continue;
            if (src.ip != ip || (int)src.port != port) {
                if (!src.nat || !_acceptNatRtp(i, false, ip, port, pkt, len)) {
                    _dropSrc(i, "rtp", ip, port, (int)src.port);
                    continue;
                }
            }
            touchActivity();

            if (_legs[dst].active)
                _legs[dst].rtp.sendTo(pkt, len, &_legs[dst].addrRtp);

            if (_recorder) {
                if (!_firstRtpReceived) {
                    _firstRtpReceived = true;
                    _segStartUsec = _getTimeUsec();
                }
                _recorder->writePacket(i == 0 ? "a" : "b", pkt, len);

                int64_t now = _getTimeUsec();
                if (_segStartUsec > 0 && (now - _segStartUsec) >= (int64_t)_segmentIntervalSec * 1000000LL) {
                    _recorder->finishSegment();
                    _recorder->startSegment(_recorder->getCurrentSeq() + 1);
                    _segStartUsec = now;
                }
            }
        }

        // ── Video RTP relay + 녹취 ──
        while (_legs[i].videoRtp.getFd() != INVALID_SOCKET) {
            PAutoLock lock(_mutex);
            int len = _legs[i].videoRtp.recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0) break;
            Leg& src = _legs[i];
            if (!src.active || src.videoPort == 0) continue;
            if (src.ip != ip || (int)src.videoPort != port) {
                if (!src.nat || !_acceptNatRtp(i, true, ip, port, pkt, len)) {
                    _dropSrc(i, "video rtp", ip, port, (int)src.videoPort);
                    continue;
                }
            }
            touchActivity();

            if (_legs[dst].active && _legs[dst].videoPort > 0)
                _legs[dst].videoRtp.sendTo(pkt, len, &_legs[dst].addrVideoRtp);

            if (_recorder)
                _recorder->writePacket(i == 0 ? "va" : "vb", pkt, len);
        }
    }

    return false;
}

void PRtpRelay::collectFds(std::vector<int>& out) const {
    for (int i = 0; i < 2; ++i) {
        const int fds[] = { _legs[i].rtp.getFd(), _legs[i].rtcp.getFd(),
                            _legs[i].videoRtp.getFd(), _legs[i].videoRtcp.getFd() };
        for (int fd : fds)
            if (fd != INVALID_SOCKET) out.push_back(fd);
    }
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
