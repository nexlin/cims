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
        // SRTP 컨텍스트 폐기 — 풀 재사용 relay 에 이전 세션 키가 잔존하면 안 된다
        leg.crypto.reset();
        leg.cryptoVideo.reset();
    }
    _taps.clear();  // 객체 회수는 PCmpServer(freeResource → collectTaps) 몫 — 여기서는 참조만 끊는다
}

void PRtpRelay::attachTap(PRtpTap* tap) {
    if (!tap) return;
    PAutoLock lock(_mutex);
    for (PRtpTap* t : _taps) if (t == tap) return;
    _taps.push_back(tap);
}

void PRtpRelay::detachTap(PRtpTap* tap) {
    PAutoLock lock(_mutex);
    for (auto it = _taps.begin(); it != _taps.end(); ++it) {
        if (*it == tap) { _taps.erase(it); return; }
    }
}

PRtpTap* PRtpRelay::findTap(const std::string& tapId) const {
    for (PRtpTap* t : _taps) if (t && t->getTapId() == tapId) return t;
    return nullptr;
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
        // 재전송·refresh — 학습(추종) 목적지 유지. 목적지 갱신은 추종 모델(_acceptNatRtp)
        //   이 미디어 소스로부터 계속 수행한다.
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

static int64_t _getTimeUsec();

// NAT leg 목적지 추종 — leg 전용 포트가 곧 신원이므로 형식 검사(RTP v2 + 최소 길이 +
//   (guard) 소스 IP==sigIp + (선언 시) 기대 ingress PT)를 통과한 소스로 송신 목적지를
//   계속 갱신한다. SSRC 핀·스테일 창은 두지 않는다 — 핀이 잘못된 소스에 걸리면 정당한
//   단말이 영구 차단되는 고착이 더 해악이고, 추종 모델은 선점 소스 소멸 즉시 자가 복구된다.
//   SRTP leg 는 형식 검사와 latch 적용 사이에 unprotect(인증)가 끼므로 둘을 분리한다.
bool PRtpRelay::_natFormatOk(const Leg& leg, bool isVideo, const std::string& ip, int port,
                             const char* pkt, int len) const {
    (void)port;
    if (len < 12 || (((unsigned char)pkt[0]) >> 6) != 2) return false;
    if (!leg.sigIp.empty() && leg.sigIp != ip) return false;
    // 기대 ingress PT 검사 (RELAY remote_src_pt 선언 시) — KA(empty RTP)도 협상 PT 를
    //   실어 보내므로 동일 기준으로 통과한다. TE 는 srcTePt(미선언=관례 101)도 허용.
    if (!isVideo && leg.srcPt > 0) {
        unsigned char pt = (unsigned char)(pkt[1] & 0x7F);
        unsigned char te = (unsigned char)((leg.srcTePt > 0 ? leg.srcTePt : 101) & 0x7F);
        if (pt != (unsigned char)(leg.srcPt & 0x7F) && pt != te) return false;
    }
    return true;
}

void PRtpRelay::_natLatch(int legIdx, bool isVideo, const std::string& ip, int port) {
    Leg& leg = _legs[legIdx];
    bool& latched = isVideo ? leg.latchedVideo : leg.latched;
    bool changed = (leg.ip != ip) ||
                   (isVideo ? (int)leg.videoPort != port : (int)leg.port != port) || !latched;
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
    if (changed) {
        // 소스 경합(두 소스가 번갈아 유입) 시 로그 폭주 방지 — leg 당 2s 간격 요약.
        int64_t now = _getTimeUsec();
        if (now - leg.followLogUsec >= 2000000LL) {
            leg.followLogUsec = now;
            LOG_INFO("PRtpRelay", "%s dest follow (NAT) peer[%d] %s:%d session=%s",
                     isVideo ? "video RTP" : "RTP", legIdx, ip.c_str(), port, _sessionId.c_str());
        }
    }
}

bool PRtpRelay::setLegCrypto(int peerIdx, bool video, const std::string& alg,
                             const std::string& rxKey, const std::string& rxSalt,
                             const std::string& txKey, const std::string& txSalt, std::string& err) {
    PAutoLock lock(_mutex);
    Leg& leg = _legs[peerIdx & 1];
    std::unique_ptr<PMediaCrypto>& sec = video ? leg.cryptoVideo : leg.crypto;
    if (!sec) sec.reset(new PMediaCrypto());
    if (!sec->init(alg, rxKey, rxSalt, txKey, txSalt, err)) {
        // 키 오류 leg 를 평문으로 조용히 폴백하지 않는다 — 컨텍스트 제거 후 명령 거부
        sec.reset();
        return false;
    }
    LOG_INFO("PRtpRelay", "SRTP %s peer[%d] alg=%s session=%s", video ? "video" : "audio",
             peerIdx & 1, sec->alg().c_str(), _sessionId.c_str());
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

// SRTP unprotect 실패(인증 태그 불일치·재전송 창 밖) — 위조/재전송 관측 (media_security.md §6.2)
void PRtpRelay::_dropSrtp(int legIdx, const char* what) {
    ++_srtpDrop;
    time_t now; time(&now);
    if (now - _lastSrtpWarn >= 5) {
        _lastSrtpWarn = now;
        LOG_WARN("PRtpRelay", "drop %s — SRTP unprotect failed peer[%d] session=%s (total=%ld)",
                 what, legIdx, _sessionId.c_str(), _srtpDrop);
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
            if ((src.ip != ip || (int)src.port + 1 != port) &&
                !(src.declIp == ip && (int)src.declPort + 1 == port)) {
                // nat leg: RTP latch 된(또는 선언) IP 와 일치하는 관측 소스로 RTCP 목적지 교정
                if (src.nat && (src.ip == ip || src.declIp == ip)) {
                    struct sockaddr_in want;
                    _makeAddr(want, ip, port);
                    // 목적지가 실제로 바뀔 때만 갱신·로그 (동일 소스 재관측은 무음 통과)
                    if (src.addrRtcp.sin_addr.s_addr != want.sin_addr.s_addr ||
                        src.addrRtcp.sin_port != want.sin_port) {
                        src.addrRtcp = want;
                        src.latchedRtcp = true;
                        LOG_INFO("PRtpRelay", "RTCP dest latched (NAT) peer[%d] %s:%d session=%s",
                                 i, ip.c_str(), port, _sessionId.c_str());
                    }
                } else {
                    _dropSrc(i, "rtcp", ip, port, (int)src.port + 1);
                    continue;
                }
            }
            // SRTCP 종단 — relay 경로는 RTCP 를 중계하므로 미디어 키로 함께 보호한다 (§6.2)
            if (src.crypto && src.crypto->enabled() && !src.crypto->unprotectRtcp(pkt, len)) {
                _dropSrtp(i, "rtcp");
                continue;
            }
            // 청취 leg — 이 peer 의 SR/RR 을 tap SSRC 로 재매핑해 송출 (립싱크·수신 통계, §5.4)
            for (PRtpTap* t : _taps) if (t->wants(i)) t->sendRtcp(i, false, pkt, len);
            if (_legs[dst].active) {
                Leg& d = _legs[dst];
                if (d.crypto && d.crypto->enabled()) {
                    if (d.crypto->protectRtcp(pkt, len, sizeof(pkt)))
                        d.rtcp.sendTo(pkt, len, &d.addrRtcp);
                    else
                        LOG_ERROR("PRtpRelay", "SRTCP protect failed peer[%d] session=%s", dst, _sessionId.c_str());
                } else {
                    d.rtcp.sendTo(pkt, len, &d.addrRtcp);
                }
            }
        }

        // ── Video RTCP relay ──
        while (_legs[i].videoRtcp.getFd() != INVALID_SOCKET) {
            PAutoLock lock(_mutex);
            int len = _legs[i].videoRtcp.recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0) break;
            Leg& src = _legs[i];
            if (!src.active || src.videoPort == 0) continue;
            if ((src.ip != ip || (int)src.videoPort + 1 != port) &&
                !(src.declIp == ip && (int)src.declVideoPort + 1 == port)) {
                if (src.nat && (src.ip == ip || src.declIp == ip)) {
                    struct sockaddr_in want;
                    _makeAddr(want, ip, port);
                    if (src.addrVideoRtcp.sin_addr.s_addr != want.sin_addr.s_addr ||
                        src.addrVideoRtcp.sin_port != want.sin_port) {
                        src.addrVideoRtcp = want;
                        src.latchedVideoRtcp = true;
                        LOG_INFO("PRtpRelay", "video RTCP dest latched (NAT) peer[%d] %s:%d session=%s",
                                 i, ip.c_str(), port, _sessionId.c_str());
                    }
                } else {
                    _dropSrc(i, "video rtcp", ip, port, (int)src.videoPort + 1);
                    continue;
                }
            }
            if (src.cryptoVideo && src.cryptoVideo->enabled() && !src.cryptoVideo->unprotectRtcp(pkt, len)) {
                _dropSrtp(i, "video rtcp");
                continue;
            }
            for (PRtpTap* t : _taps) if (t->wants(i)) t->sendRtcp(i, true, pkt, len);
            if (_legs[dst].active && _legs[dst].videoPort > 0) {
                Leg& d = _legs[dst];
                if (d.cryptoVideo && d.cryptoVideo->enabled()) {
                    if (d.cryptoVideo->protectRtcp(pkt, len, sizeof(pkt)))
                        d.videoRtcp.sendTo(pkt, len, &d.addrVideoRtcp);
                    else
                        LOG_ERROR("PRtpRelay", "video SRTCP protect failed peer[%d] session=%s", dst, _sessionId.c_str());
                } else {
                    d.videoRtcp.sendTo(pkt, len, &d.addrVideoRtcp);
                }
            }
        }

        // ── Audio RTP relay + 녹취 ──
        while (_legs[i].rtp.getFd() != INVALID_SOCKET) {
            PAutoLock lock(_mutex);
            int len = _legs[i].rtp.recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0) break;
            Leg& src = _legs[i];
            if (!src.active) continue;
            // 선언(SDP) 주소 일치는 latch 상태와 무관하게 항상 수락 — 미디어별 NAT 경로가
            //   갈리는 leg 에서 다른 미디어의 latch 가 ip 를 덮어써도 협상된 신원은 유지.
            bool srcOk = (src.ip == ip && (int)src.port == port) ||
                         (src.declIp == ip && (int)src.declPort == port);
            if (!srcOk && (!src.nat || !_natFormatOk(src, false, ip, port, pkt, len))) {
                _dropSrc(i, "rtp", ip, port, (int)src.port);
                continue;
            }
            // ingress unprotect — 이후 경로(녹취·relay·PT 분류)는 전부 평문 (§6.2).
            //   nat leg 의 latch 는 unprotect 성공 후에만 적용 — 제3자 주입으로 목적지
            //   오염 불가 (latch 첫 유효 RTP 판정에 인증 포함).
            if (src.crypto && src.crypto->enabled() && !src.crypto->unprotectRtp(pkt, len)) {
                _dropSrtp(i, "rtp");
                continue;
            }
            if (!srcOk) _natLatch(i, false, ip, port);
            touchActivity();

            // 청취 leg(tap) — 복호된 평문 ingress 복사(녹취 탭 지점과 동일), 원본 SSRC→tap SSRC 재매핑.
            //   상대 leg 로의 relay·녹취와 독립이라 A/B 에게는 아무 변화가 없다(은닉, dispatch_center.md §5.4).
            if (!_taps.empty()) {
                bool isTe = false;
                if (len >= 12) {
                    unsigned char inPt = (unsigned char)(pkt[1] & 0x7F);
                    isTe = (src.srcTePt > 0) ? (inPt == (unsigned char)(src.srcTePt & 0x7F)) : (inPt == 101);
                }
                for (PRtpTap* t : _taps) if (t->wants(i)) t->sendRtp(i, false, pkt, len, isTe);
            }

            if (_legs[dst].active) {
                // leg 별 PT 재작성 (cmp_media_api.md — remote_pt/remote_te_pt, 0=재작성 없음).
                //   녹취는 아래에서 talker 원본(pkt)을 기록하므로 egress 사본에만 스탬프.
                //   marker bit(0x80) 보존. TE 분류는 src leg 의 srcTePt(미지정=관례 101).
                int stampPt = 0;
                if (len >= 12) {
                    unsigned char inPt = (unsigned char)(pkt[1] & 0x7F);
                    bool isTe = (src.srcTePt > 0) ? (inPt == (unsigned char)(src.srcTePt & 0x7F))
                                                  : (inPt == 101);
                    stampPt = isTe ? _legs[dst].tePtOut : _legs[dst].ptOut;
                }
                Leg& d = _legs[dst];
                bool dstSec = d.crypto && d.crypto->enabled();
                if (stampPt > 0 || dstSec) {
                    // egress 사본 — 녹취용 평문(pkt)을 보존한 채 스탬프/protect
                    char out[2048];
                    int outLen = len;
                    memcpy(out, pkt, len);
                    if (stampPt > 0)
                        out[1] = (char)((out[1] & 0x80) | (stampPt & 0x7F));
                    if (dstSec && !d.crypto->protectRtp(out, outLen, sizeof(out)))
                        LOG_ERROR("PRtpRelay", "SRTP protect failed peer[%d] session=%s", dst, _sessionId.c_str());
                    else
                        d.rtp.sendTo(out, outLen, &d.addrRtp);
                } else {
                    d.rtp.sendTo(pkt, len, &d.addrRtp);
                }
            }

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
            bool srcOk = (src.ip == ip && (int)src.videoPort == port) ||
                         (src.declIp == ip && (int)src.declVideoPort == port);
            if (!srcOk && (!src.nat || !_natFormatOk(src, true, ip, port, pkt, len))) {
                _dropSrc(i, "video rtp", ip, port, (int)src.videoPort);
                continue;
            }
            if (src.cryptoVideo && src.cryptoVideo->enabled() && !src.cryptoVideo->unprotectRtp(pkt, len)) {
                _dropSrtp(i, "video rtp");
                continue;
            }
            if (!srcOk) _natLatch(i, true, ip, port);
            touchActivity();

            for (PRtpTap* t : _taps) if (t->wants(i)) t->sendRtp(i, true, pkt, len, false);

            if (_legs[dst].active && _legs[dst].videoPort > 0) {
                Leg& d = _legs[dst];
                if (d.cryptoVideo && d.cryptoVideo->enabled()) {
                    // egress 사본 — 녹취용 평문(pkt) 보존
                    char out[2048];
                    int outLen = len;
                    memcpy(out, pkt, len);
                    if (d.cryptoVideo->protectRtp(out, outLen, sizeof(out)))
                        d.videoRtp.sendTo(out, outLen, &d.addrVideoRtp);
                    else
                        LOG_ERROR("PRtpRelay", "video SRTP protect failed peer[%d] session=%s", dst, _sessionId.c_str());
                } else {
                    d.videoRtp.sendTo(pkt, len, &d.addrVideoRtp);
                }
            }

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

void PRtpRelay::setPeerPt(int peerIdx, int pt, int srcPt, int tePt, int srcTePt, const std::string& codec) {
    PAutoLock lock(_mutex);
    Leg& leg = _legs[peerIdx & 1];
    leg.ptOut = pt; leg.srcPt = srcPt; leg.tePtOut = tePt; leg.srcTePt = srcTePt;
    if (!codec.empty()) leg.codec = codec;
    // 녹취 활성이면 leg 트랙 메타(audio_pt_a/b, audio_codec_a/b) 갱신 — MODIFY(재협상)도 최신 반영
    if (_recorder)
        _recorder->setTrackPtCodec((peerIdx & 1) == 0 ? "a" : "b", leg.srcPt, leg.codec);
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
    // RELAY_ADD 는 setPeerPt → startRecording 순서 — 이미 선언된 leg PT/코덱을 트랙 메타로 반영
    for (int i = 0; i < 2; ++i) {
        if (_legs[i].srcPt > 0)
            _recorder->setTrackPtCodec(i == 0 ? "a" : "b", _legs[i].srcPt, _legs[i].codec);
    }
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
