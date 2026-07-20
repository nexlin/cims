#ifndef __PRTP_RELAY_H__
#define __PRTP_RELAY_H__

#include <string>
#include <vector>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"

class PSyncRtpRecorder;

/**
 * VoIP 1:1 RTP Relay (B2BUA)
 *
 * peer 별 전용 포트 블록 — leg 별 포트셋 (docs/design/features/ue_nat_traversal.md §3.1):
 *   peer[0](발신 A): base+0 rtp / base+1 rtcp / base+2 video / base+3 video rtcp
 *   peer[1](착신 B): base+4 rtp / base+5 rtcp / base+6 video / base+7 video rtcp
 * 각 peer 는 자기 전용 포트로만 송신하므로 수신 소켓이 곧 peer 신원이다
 * (소스 주소로 peer 를 추측하지 않는다). relay 는 peer i 수신 → peer 1-i 소켓 송신.
 * 녹취 track a/b = 수신 소켓 기준 — NAT/도착 순서와 무관하게 발/착 귀속이 정확하다.
 */
class PRtpRelay : public PHandler
{
public:
    PRtpRelay(const std::string& name);
    virtual ~PRtpRelay();

    // basePort 부터 8포트(peer 별 4포트 블록 × 2)를 연다.
    bool init(const std::string& ip, unsigned int basePort);
    bool final();
    void reset();

    // nat=true 면 이 peer 의 전용 포트에서 목적지 latch 허용 (ue_nat_traversal.md §5).
    //   sigIp 가 비어있지 않으면 latch 소스 IP 는 sigIp 와 일치해야 한다 (latch IP guard).
    //   주소 갱신(re-INVITE) 시 latch 상태를 리셋해 재-latch 를 허용한다.
    bool setRemote(const std::string& ip, unsigned int port, unsigned int videoPort = 0, int peerIdx = -1,
                   bool nat = false, const std::string& sigIp = "");

    // NAT latch 관측 (STATS detail.nat) — latch 완료 시 학습 주소 반환.
    bool getNatLatched(int peerIdx, std::string& learnedIp, int& learnedPort) const;

    unsigned int getLocalPort(int peerIdx = 0) const { return _legs[peerIdx & 1].localPort; }
    unsigned int getLocalVideoPort(int peerIdx = 0) const { return _legs[peerIdx & 1].localVideoPort; }

    void setSessionId(const std::string& id) { _sessionId = id; }
    std::string getSessionId() const { return _sessionId; }
    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }
    void touchActivity() { time(&_lastActivityTime); _everReceivedRtp = true; }
    // 세션 바인딩 시점 초기화 — 풀에서 재사용되는 relay 의 잔존 활동시각/수신이력/카운터 제거
    //   (없으면 idle=풀 생성시각 기준으로 계산돼 신규 세션이 즉시 orphan 회수됨).
    void resetActivity() { time(&_lastActivityTime); _everReceivedRtp = false; _srcDrop = 0; }
    time_t getLastActivityTime() const { return _lastActivityTime; }
    // RTP 를 한 번이라도 받았는가 — 고아(setup 실패) relay 빠른 회수 vs 활성/홀드 호 보존 구분용.
    bool everReceivedRtp() const { return _everReceivedRtp; }
    // 미협상 소스 드롭 누적 (STATS rtp_src_drop)
    long getSrcDrop() const { return _srcDrop; }

    void startRecording(const std::string& rawDir, const std::string& sessionId,
                        const std::string& caller = "", const std::string& callee = "",
                        int segmentIntervalSec = 60);
    void stopRecording();
    bool isRecording() const { return _recorder != nullptr; }

    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

    // epoll 리액터 등록용: 이 relay 의 유효 소켓 fd 를 수집(peer 별 audio/video RTP+RTCP).
    //   소켓은 init 때 열려 프로세스 내내 유지되므로 등록은 1회면 충분.
    void collectFds(std::vector<int>& out) const;

private:
    // peer 하나의 leg — 전용 소켓 4개 + 상대(SDP 선언) 주소
    struct Leg {
        PRtpSocket rtp;
        PRtpSocket rtcp;
        PRtpSocket videoRtp;
        PRtpSocket videoRtcp;
        unsigned int localPort = 0;
        unsigned int localVideoPort = 0;

        std::string ip;                 // 상대 주소 (SDP 선언 → latch 시 학습 주소로 갱신)
        unsigned int port = 0;
        unsigned int videoPort = 0;
        std::string declIp;             // 마지막 SDP 선언 주소 원본 (latch 와 무관하게 보존 —
        unsigned int declPort = 0;      //   setRemote 재수신 시 선언 불변 여부 비교용)
        unsigned int declVideoPort = 0;
        struct sockaddr_in addrRtp{};   // 송신 목적지
        struct sockaddr_in addrRtcp{};
        struct sockaddr_in addrVideoRtp{};
        struct sockaddr_in addrVideoRtcp{};
        bool active = false;

        // NAT 목적지 latch (제어평면이 nat 지정한 leg 만 — ue_nat_traversal.md §5)
        bool nat = false;
        std::string sigIp;              // latch IP guard 기준 (빈 값 = guard 없음)
        bool latched = false;           // audio RTP latch 완료
        bool latchedVideo = false;
        bool latchedRtcp = false;       // RTCP 관측 소스로 목적지 교정 완료
        bool latchedVideoRtcp = false;
        uint32_t latchSsrc = 0;         // latch 시 고정 — 재-latch 는 동일 SSRC(NAT rebind)만
        uint32_t latchVideoSsrc = 0;
    };

    // nat leg 의 audio/video RTP 수신 판정+latch (호출자가 _mutex 보유). 수락 시 true.
    bool _acceptNatRtp(int legIdx, bool isVideo, const std::string& ip, int port, const char* pkt, int len);

    // 미협상 소스 드롭 (호출자가 _mutex 보유) — 카운터 + rate-limited WARN
    void _dropSrc(int legIdx, const char* what, const std::string& ip, int port, int expectedPort);

    PMutex      _mutex;
    std::string _sessionId;
    std::string _workerName;
    time_t      _lastActivityTime = 0;
    bool        _everReceivedRtp = false;
    long        _srcDrop = 0;
    time_t      _lastDropWarn = 0;
    Leg         _legs[2];

    // 녹취
    PSyncRtpRecorder* _recorder = nullptr;
    int _segmentIntervalSec = 60;
    int64_t _segStartUsec = 0;
    bool _firstRtpReceived = false;
};

#endif // __PRTP_RELAY_H__
