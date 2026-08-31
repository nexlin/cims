#ifndef __PRTP_RELAY_H__
#define __PRTP_RELAY_H__

#include <string>
#include <vector>
#include <memory>
#include "pbase.h"
#include "pmodule.h"
#include "PMPBase.h"
#include "PRtpSocket.h"
#include "PMediaCrypto.h"

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

    // leg 별 PT 재작성 파라미터 (RELAY_ADD/MODIFY remote_pt 계열, 0=재작성 없음).
    //   pt/tePt: 이 leg 로 송신 시 스탬프할 audio/TE PT. srcPt/srcTePt: 이 leg 가
    //   송신에 쓰는 PT(TE 분류 기준). 규격 준수 단말은 비대칭 PT 통과 가능 — 보험 필드.
    //   codec: 협상 오디오 코덱(remote_codec, 예 "AMR-WB/16000") — 녹취 세그먼트 메타용.
    void setPeerPt(int peerIdx, int pt, int srcPt, int tePt, int srcTePt, const std::string& codec = "");

    // leg 미디어 SRTP 컨텍스트 설정/재키잉 (RELAY_ADD/MODIFY media_crypto[_video] —
    //   media_security.md §6.2~6.3). rx*=UE 상향(UE 의 a=crypto 선언), tx*=CMP 하향
    //   (CSP 생성). key/salt 는 디코드된 바이트열(16B/14B). 동일 구성 재선언은 세션
    //   유지, 변경은 세션 재생성(ROC 리셋). 실패 시 err — 호출자가 명령을 거부한다
    //   (평문 조용 폴백 금지).
    bool setLegCrypto(int peerIdx, bool video, const std::string& alg,
                      const std::string& rxKey, const std::string& rxSalt,
                      const std::string& txKey, const std::string& txSalt, std::string& err);

    unsigned int getLocalPort(int peerIdx = 0) const { return _legs[peerIdx & 1].localPort; }
    unsigned int getLocalVideoPort(int peerIdx = 0) const { return _legs[peerIdx & 1].localVideoPort; }

    void setSessionId(const std::string& id) { _sessionId = id; }
    std::string getSessionId() const { return _sessionId; }
    void setWorkerName(const std::string& n) { _workerName = n; }
    std::string getWorkerName() const { return _workerName; }
    void touchActivity() { time(&_lastActivityTime); _everReceivedRtp = true; }
    // 세션 바인딩 시점 초기화 — 풀에서 재사용되는 relay 의 잔존 활동시각/수신이력/카운터 제거
    //   (없으면 idle=풀 생성시각 기준으로 계산돼 신규 세션이 즉시 orphan 회수됨).
    //   _createdTime 도 여기서 세션 바인딩 시각으로 고정한다(RTP 수신과 무관하게 단조) — audit
    //   SESSION_LIST 의 grace(min_age) 판정 기준. 재사용 relay 는 새 바인딩마다 갱신된다.
    void resetActivity() { time(&_lastActivityTime); time(&_createdTime); _everReceivedRtp = false; _srcDrop = 0; _srtpDrop = 0; }
    time_t getLastActivityTime() const { return _lastActivityTime; }
    // 세션 바인딩(생성) 시각 — audit grace 필터용(now-created = 세션 존재기간, RTP 무관 단조 증가).
    time_t getCreatedTime() const { return _createdTime; }
    // RTP 를 한 번이라도 받았는가 — 고아(setup 실패) relay 빠른 회수 vs 활성/홀드 호 보존 구분용.
    bool everReceivedRtp() const { return _everReceivedRtp; }
    // 미협상 소스 드롭 누적 (STATS rtp_src_drop)
    long getSrcDrop() const { return _srcDrop; }
    // SRTP unprotect 실패(인증 태그 불일치·재전송 창 밖) 폐기 누적 (STATS srtp_drop)
    long getSrtpDrop() const { return _srtpDrop; }

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

        // leg 별 PT 재작성 (setPeerPt — 0 = 재작성 없음, 현행 PT-blind 통과)
        int ptOut = 0;
        int srcPt = 0;
        int tePtOut = 0;
        int srcTePt = 0;
        std::string codec;              // 협상 오디오 코덱 (remote_codec) — 녹취 세그먼트 메타용

        // NAT 목적지 latch (제어평면이 nat 지정한 leg 만 — ue_nat_traversal.md §5)
        bool nat = false;
        std::string sigIp;              // latch IP guard 기준 (빈 값 = guard 없음)
        bool latched = false;           // audio RTP 추종 학습 완료 (관측용)
        bool latchedVideo = false;
        bool latchedRtcp = false;       // RTCP 관측 소스로 목적지 교정 완료
        bool latchedVideoRtcp = false;
        int64_t followLogUsec = 0;      // dest follow 로그 rate-limit (소스 경합 시 폭주 방지)

        // 미디어 SRTP 컨텍스트 (media_crypto[_video] — media_security.md §6.2). null=평문 leg.
        //   접근은 relay 직렬화 범위(같은 리액터) 안 — PMediaCrypto.h 스레드 규약 참조.
        std::unique_ptr<PMediaCrypto> crypto;        // audio RTP + RTCP(SRTCP)
        std::unique_ptr<PMediaCrypto> cryptoVideo;   // video RTP + RTCP(SRTCP)
    };

    // nat leg 수신 형식 검사 (RTP v2 + 최소 길이 + guard IP + 기대 ingress PT — 상태 불변).
    bool _natFormatOk(const Leg& leg, bool isVideo, const std::string& ip, int port,
                      const char* pkt, int len) const;
    // nat leg 목적지 latch 적용 (호출자가 _mutex 보유). SRTP leg 는 unprotect 성공 후에만
    //   호출된다 — 제3자 주입으로 latch 가 오염되지 않는다 (media_security.md §6.2).
    void _natLatch(int legIdx, bool isVideo, const std::string& ip, int port);

    // 미협상 소스 드롭 (호출자가 _mutex 보유) — 카운터 + rate-limited WARN
    void _dropSrc(int legIdx, const char* what, const std::string& ip, int port, int expectedPort);

    // SRTP unprotect 실패 드롭 (호출자가 _mutex 보유) — 카운터 + rate-limited WARN
    void _dropSrtp(int legIdx, const char* what);

    PMutex      _mutex;
    std::string _sessionId;
    std::string _workerName;
    time_t      _lastActivityTime = 0;
    time_t      _createdTime = 0;       // 세션 바인딩 시각 (audit grace) — resetActivity 에서 고정
    bool        _everReceivedRtp = false;
    long        _srcDrop = 0;
    time_t      _lastDropWarn = 0;
    long        _srtpDrop = 0;
    time_t      _lastSrtpWarn = 0;
    Leg         _legs[2];

    // 녹취
    PSyncRtpRecorder* _recorder = nullptr;
    int _segmentIntervalSec = 60;
    int64_t _segStartUsec = 0;
    bool _firstRtpReceived = false;
};

#endif // __PRTP_RELAY_H__
