#include "PRtpTap.h"
#include "PLog.h"
#include <openssl/rand.h>
#include <string.h>

PRtpTap::PRtpTap(const std::string &name) : PHandler(name) {
    time(&_lastActivityTime);
}

PRtpTap::~PRtpTap() { final(); }

bool PRtpTap::init(const std::string &ip, unsigned int basePort) {
    PAutoLock lock(_mutex);
    if (!_rtp.open(ip, basePort))
        return false;
    if (!_rtcp.open(ip, basePort + 1))
        return false;
    if (!_videoRtp.open(ip, basePort + 2))
        return false;
    if (!_videoRtcp.open(ip, basePort + 3))
        return false;
    _localPort = basePort;
    _localVideoPort = basePort + 2;
    LOG_INFO("PRtpTap", "init %s:%u-%u", ip.c_str(), basePort, basePort + 3);
    return true;
}

bool PRtpTap::final() {
    PAutoLock lock(_mutex);
    _rtcp.close();
    _videoRtp.close();
    _videoRtcp.close();
    _rtp.close();
    return true;
}

void PRtpTap::reset() {
    PAutoLock lock(_mutex);
    _sessionId.clear();
    _tapId.clear();
    _monitor.clear();
    _ip.clear();
    _port = _videoPort = 0;
    memset(&_addrRtp, 0, sizeof(_addrRtp));
    memset(&_addrRtcp, 0, sizeof(_addrRtcp));
    memset(&_addrVideoRtp, 0, sizeof(_addrVideoRtp));
    memset(&_addrVideoRtcp, 0, sizeof(_addrVideoRtcp));
    _active = false;
    _mode = MODE_BOTH;
    _ptOut = _tePtOut = 0;
    _ssrc[0] = _ssrc[1] = 0;
    _crypto.reset(); // 풀 재사용 시 이전 세션 키 잔존 금지
    _cryptoVideo.reset();
    _sent = 0;
}

uint32_t PRtpTap::_randSsrc() {
    uint32_t v = 0;
    if (RAND_bytes((unsigned char *)&v, sizeof(v)) != 1 || v == 0)
        v = (uint32_t)time(NULL) ^ 0x5bd1e995u;
    return v;
}

void PRtpTap::bind(const std::string &sessionId, const std::string &tapId,
                                      const std::string &monitor) {
    PAutoLock lock(_mutex);
    _sessionId = sessionId;
    _tapId = tapId;
    _monitor = monitor;
    // tap 전용 SSRC 2개 — 원본 SSRC 와 무관하게 유일·구분 보장 (RFC 5576 라벨링의
    // 대상 값)
    _ssrc[0] = _randSsrc();
    do {
        _ssrc[1] = _randSsrc();
    } while (_ssrc[1] == _ssrc[0]);
    time(&_lastActivityTime);
    _sent = 0;
}

static void _makeAddr(struct sockaddr_in &addr, const std::string &ip,
                                            int port) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port = htons(port);
}

bool PRtpTap::setRemote(const std::string &ip, unsigned int port,
                                                unsigned int videoPort) {
    if (ip.empty() || ip == "0.0.0.0" || port == 0)
        return false;
    PAutoLock lock(_mutex);
    _ip = ip;
    _port = port;
    _videoPort = videoPort;
    _active = true;
    _makeAddr(_addrRtp, ip, port);
    _makeAddr(_addrRtcp, ip, port + 1);
    if (videoPort > 0) {
        _makeAddr(_addrVideoRtp, ip, videoPort);
        _makeAddr(_addrVideoRtcp, ip, videoPort + 1);
    }
    LOG_INFO("PRtpTap", "setRemote %s:%u video=%u (local=%u) session=%s tap=%s",
                      ip.c_str(), port, videoPort, _localPort, _sessionId.c_str(),
                      _tapId.c_str());
    return true;
}

PRtpTap::Mode PRtpTap::ParseMode(const std::string &s) {
    if (s == "a")
        return MODE_A;
    if (s == "b")
        return MODE_B;
    return MODE_BOTH;
}

bool PRtpTap::setCrypto(bool video, const std::string &alg,
                                                const std::string &txKey, const std::string &txSalt,
                                                std::string &err) {
    PAutoLock lock(_mutex);
    std::unique_ptr<PMediaCrypto> &slot = video ? _cryptoVideo : _crypto;
    if (!slot)
        slot.reset(new PMediaCrypto());
    // rx 는 쓰지 않는다(상향 폐기) — 세션 구성 요건상 tx 키를 양쪽에 채운다
    if (!slot->init(alg, txKey, txSalt, txKey, txSalt, err)) {
        slot.reset();
        return false;
    }
    return true;
}

bool PRtpTap::wants(int legIdx) const {
    if (!_active)
        return false;
    if (_mode == MODE_A)
        return legIdx == 0;
    if (_mode == MODE_B)
        return legIdx == 1;
    return true;
}

void PRtpTap::sendRtp(int legIdx, bool video, const char *pkt, int len,
                                            bool isTe) {
    if (len < 12)
        return;
    PAutoLock lock(_mutex);
    if (!_active)
        return;
    if (video && _videoPort == 0)
        return;
    char out[2048];
    if (len > (int)sizeof(out) - PMediaCrypto::kMaxOverhead)
        return;
    memcpy(out, pkt, len);
    int outLen = len;
    // SSRC 재매핑 — tap 전용 값(귀속·유일성). 시퀀스/타임스탬프는 원본 유지(연속
    // 스트림 복사).
    uint32_t ssrc = htonl(_ssrc[legIdx & 1]);
    memcpy(out + 8, &ssrc, 4);
    // PT 스탬프 — 청취 단말이 수신 선언한 PT (TE 는 별도), marker bit 보존
    if (!video) {
        int stamp = isTe ? _tePtOut : _ptOut;
        if (stamp > 0)
            out[1] = (char)((out[1] & 0x80) | (stamp & 0x7F));
    }
    PMediaCrypto *c = video ? _cryptoVideo.get() : _crypto.get();
    if (c && c->enabled() && !c->protectRtp(out, outLen, sizeof(out))) {
        LOG_ERROR("PRtpTap", "SRTP protect failed session=%s tap=%s",
                            _sessionId.c_str(), _tapId.c_str());
        return;
    }
    if (video)
        _videoRtp.sendTo(out, outLen, &_addrVideoRtp);
    else
        _rtp.sendTo(out, outLen, &_addrRtp);
    ++_sent;
}

void PRtpTap::sendRtcp(int legIdx, bool video, const char *pkt, int len) {
    if (len < 8)
        return;
    PAutoLock lock(_mutex);
    if (!_active)
        return;
    if (video && _videoPort == 0)
        return;
    char out[2048];
    if (len > (int)sizeof(out) - PMediaCrypto::kMaxOverhead)
        return;
    memcpy(out, pkt, len);
    int outLen = len;
    // 첫 패킷이 SR(200)/RR(201) 이면 sender SSRC 를 tap SSRC 로 재매핑 — 단말이
    // RTP 와 같은 소스로 인식
    unsigned char pt = (unsigned char)out[1];
    if (pt == 200 || pt == 201) {
        uint32_t ssrc = htonl(_ssrc[legIdx & 1]);
        memcpy(out + 4, &ssrc, 4);
    }
    PMediaCrypto *c = video ? _cryptoVideo.get() : _crypto.get();
    if (c && c->enabled() && !c->protectRtcp(out, outLen, sizeof(out)))
        return;
    if (video)
        _videoRtcp.sendTo(out, outLen, &_addrVideoRtcp);
    else
        _rtcp.sendTo(out, outLen, &_addrRtcp);
}

bool PRtpTap::proc() {
    // 상향 차단 — 청취 단말이 보내는 RTP 는 폐기(어디로도 중계하지 않는다). RTCP
    // 는 keepalive 로만.
    std::string ip;
    int port;
    char pkt[2048];
    PRtpSocket *socks[4] = {&_rtp, &_rtcp, &_videoRtp, &_videoRtcp};
    for (int i = 0; i < 4; ++i) {
        while (socks[i]->getFd() != INVALID_SOCKET) {
            int len = socks[i]->recv(pkt, sizeof(pkt), ip, port);
            if (len <= 0)
                break;
            if (i == 1 || i == 3)
                time(&_lastActivityTime); // RTCP keepalive
        }
    }
    return false;
}

void PRtpTap::collectFds(std::vector<int> &out) const {
    const int fds[] = {_rtp.getFd(), _rtcp.getFd(), _videoRtp.getFd(),
                                          _videoRtcp.getFd()};
    for (int fd : fds)
        if (fd != INVALID_SOCKET)
            out.push_back(fd);
}
