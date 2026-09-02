#ifndef __PRTP_TAP_H__
#define __PRTP_TAP_H__

#include "PMPBase.h"
#include "PMediaCrypto.h"
#include "PRtpSocket.h"
#include "pbase.h"
#include "pmodule.h"
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

/**
  * 청취 leg(tap) — relay 세션에 붙는 3자 leg
  * (docs/design/features/dispatch_center.md §5.4·§6, docs/api/cmp_media_api.md
  * §6.5). 업무망 합법감청의 미디어 인도 단위.
  *
  *  - 양 peer 의 ingress(SRTP 복호 후 평문 — 녹취 탭 지점과 동일)를 **복사**해
  * 청취 단말로 송신한다. 트랜스코딩·믹싱 없음: peer0/peer1 은 tap 전용 SSRC
  * 2개로 분리 인도하고(귀속 보존), 믹싱은 단말 몫.
  *  - egress 에서 SSRC 를 tap 고유 값(ssrcA/ssrcB)으로 재매핑한다 — 원본 SSRC
  * 우연 충돌 방지 + CSP 가 SDP `a=ssrc … label:caller|callee`(RFC 5576)로
  * 광고하는 값과 일치. RTCP SR/RR 의 sender SSRC 도 같은 값으로 재매핑해 tap
  * 으로 송출한다(립싱크·수신 통계).
  *  - 상향 차단: tap 소켓으로 들어오는 RTP 는 폐기, RTCP 는 keepalive(활동
  * 시각)로만 받는다.
  *  - 포트 블록 4개(audio RTP/RTCP, video RTP/RTCP) — peer leg 와 동형. 소켓은
  * 기동 시 열려 epoll 에 영구 등록되고 풀에서 alloc/free 로 재사용된다. 수명은
  * relay 세션에 종속(RELAY_REMOVE/회수 시 일괄).
  *  - 스레드: 송신은 relay 의 리액터 스레드(relay _mutex 보유)에서, 수신 drain
  * 은 tap 자기 리액터에서 일어난다 — 주소/키/PT 는 _mutex 로 보호한다.
  */
class PRtpTap : public PHandler {
public:
    enum Mode { MODE_BOTH = 0, MODE_A = 1, MODE_B = 2 };

    PRtpTap(const std::string &name);
    virtual ~PRtpTap();

    bool init(const std::string &ip, unsigned int basePort);
    bool final();
    void reset();

    // 바인딩 (RELAY_TAP_ADD) / 갱신 (RELAY_TAP_MODIFY)
    void bind(const std::string &sessionId, const std::string &tapId,
                        const std::string &monitor);
    bool setRemote(const std::string &ip, unsigned int port,
                                  unsigned int videoPort);
    void setPt(int pt, int tePt) {
        PAutoLock lock(_mutex);
        _ptOut = pt;
        _tePtOut = tePt;
    }
    void setMode(Mode m) {
        PAutoLock lock(_mutex);
        _mode = m;
    }
    static Mode ParseMode(const std::string &s);
    // tap egress SRTP(CMP→단말 tx). rx 는 쓰지 않는다(상향 폐기) — libsrtp 세션
    // 구성상 같은 키로 채운다.
    bool setCrypto(bool video, const std::string &alg, const std::string &txKey,
                                  const std::string &txSalt, std::string &err);

    // relay 가 부르는 egress — legIdx: 0=peer0(caller) / 1=peer1(callee). isTe:
    // TE(DTMF) 패킷 분류 결과.
    bool wants(int legIdx) const;
    void sendRtp(int legIdx, bool video, const char *pkt, int len, bool isTe);
    void sendRtcp(int legIdx, bool video, const char *pkt, int len);

    unsigned int getLocalPort() const { return _localPort; }
    unsigned int getLocalVideoPort() const { return _localVideoPort; }
    uint32_t getSsrc(int legIdx) const { return _ssrc[legIdx & 1]; }
    std::string getSessionId() const { return _sessionId; }
    std::string getTapId() const { return _tapId; }
    std::string getMonitor() const { return _monitor; }
    Mode getMode() const { return _mode; }
    bool isBound() const { return !_sessionId.empty(); }
    time_t getLastActivityTime() const { return _lastActivityTime; }
    long getSent() const { return _sent; }

    void setWorkerName(const std::string &n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }

    bool proc();
    bool proc(int, const std::string &, PEvent::Ptr) { return false; }
    void collectFds(std::vector<int> &out) const;

private:
    static uint32_t _randSsrc();

    PRtpSocket _rtp, _rtcp, _videoRtp, _videoRtcp;
    unsigned int _localPort = 0;
    unsigned int _localVideoPort = 0;

    PMutex _mutex;
    std::string _sessionId, _tapId, _monitor, _workerName;
    std::string _ip;
    unsigned int _port = 0, _videoPort = 0;
    struct sockaddr_in _addrRtp{}, _addrRtcp{}, _addrVideoRtp{}, _addrVideoRtcp{};
    bool _active = false;
    Mode _mode = MODE_BOTH;
    int _ptOut = 0, _tePtOut = 0;
    uint32_t _ssrc[2] = {0, 0};
    std::unique_ptr<PMediaCrypto> _crypto, _cryptoVideo;
    time_t _lastActivityTime = 0;
    long _sent = 0;
};

#endif // __PRTP_TAP_H__
