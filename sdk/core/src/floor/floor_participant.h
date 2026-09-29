// libcimsue 내부 — MCPTT floor participant (TS 24.380 §6.2.4) + RTCP-APP UDP 전송.
// 원천: android/ptt-client floor/FloorClient.kt (상태머신·Ack keepalive·Revoke Release 재전송·MSN 폐기) +
// PttController 의 요청 시한·Granted Duration 자체 종료.
//
// 스레딩: 수신 스레드 1개(pjlib 등록)가 소켓 select(≤100ms) → 디코드·상태 갱신·타이머 tick 을 한다.
// 공개 메서드는 임의 스레드에서 호출되며 mutex 로 직렬화된다. 콜백(onEvent/onMic)은 수신 스레드 또는
// 호출 스레드에서 오므로 소유자(Engine)가 이벤트 스레드로 넘긴다.
#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/types.h"
#include "floor_codec.h"

namespace cimsue {
namespace floor {

class Participant {
public:
    struct Callbacks {
        std::function<void(const FloorEvent&)> onEvent;
        std::function<void(bool micOn)> onMic;              // 마이크 게이트 — Granted 에서만 true
        std::function<void(int level, const std::string&)> log;
        /** 일제 통화 개시자가 발언을 놓은 뒤 B-bit Floor Idle 을 받았다 — 소유자가 호를 해제한다(TS 24.380 §6.2.4.6.4). */
        std::function<void()> onBroadcastEnd;
    };

    Participant(int callId, uint32_t ssrc, const std::string& userId, Callbacks cb);
    ~Participant();

    /** UDP 소켓 바인드(IPv4 any). 실패 시 false. localPort()=SDP m=application 광고 포트. */
    bool open(int localPort = 0);
    int localPort() const { return localPort_; }
    /** SDP 에서 학습한 CMP floor 목적지 — 이후 송신 가능·Ack keepalive 시작. */
    void setRemote(const std::string& ip, int port);
    bool hasRemote() const { return remotePort_ > 0; }
    /** 청취 전용 leg(a=recvonly) — 요청을 보내지 않고 Denied 로 되돌린다. */
    void setListenOnly(bool on) { listenOnly_ = on; }
    /** 일제 통화 개시자(TS 24.379 §4.12) — Floor Request 에 B-bit(TS 24.380 §6.2.4.3.5), 발언을 놓은 뒤
     *  B-bit Floor Idle 이면 onBroadcastEnd(§6.2.4.6.4). 수신 멤버는 켜지 않는다(Taken 의 Permission 0 이 요청을 막는다). */
    void setBroadcastInitiator(bool on) { broadcastInitiator_ = on; }
    /** 승인 뒤 마이크 개방 지연(ms, 0 = 즉시) — 앱이 승인 톤을 재생하는 동안 톤이 그룹으로 나가지 않게(android_ue_client.md
     *  «삑 후 말하기»). 그 사이 놓거나·회수·시한으로 발언을 잃으면 열지 않는다. EngineConfig.grantMicDelayMs. */
    void setMicOpenDelay(int ms) { micDelayMs_ = ms > 0 ? ms : 0; }

    /** 개시 INVITE 가 암묵적 발언 요청이다(`mc_implicit_request`, TS 24.380 §14.2.5) — 호 성립 전부터 'U: pending Request'
     *  (§6.2.4.2.2 4.)로 둔다. Floor Request 는 보내지 않는다(요청은 INVITE 가 싣는다). emergency = 대체 명시 요청의 긴급 비트. */
    void armImplicitRequest(bool emergency);
    /** 개시 INVITE 의 200 OK answer(§14.3.4·§14.3.5) — granted(answer `mc_granted`) = 'U: has permission'(§6.2.4.4.2),
     *  accepted(answer `mc_implicit_request`) = Floor Granted 대기(요청 시한), 둘 다 없음 = 서버가 암묵 요청으로 받지 않았다(진행 중
     *  호 합류 등, §14.3.5) → 누르고 있으니 명시 Floor Request 로 잇는다. 그 전에 release() 했으면 발언권을 Floor Release 로 돌려준다.
     *  answer 뒤에 오는 Floor Granted 는 서버가 규격대로 따로 보내는 것이라(§6.3.4.4.2 1.) 'U: has permission' 에 머문다(§6.2.4.5.5). */
    void onInitialAnswer(bool granted, bool accepted);

    void request(int priority = -1, bool emergency = false);
    void release();
    void cancelQueued();
    FloorInfo info() const;
    void close();

private:
    using Clock = std::chrono::steady_clock;
    void rxLoop();
    void handle(const Message& m);
    void tick();
    void send(const std::string& pkt);
    void emit(FloorEvent ev);
    void setMic(bool on);
    void sendRequest(int priority, bool emergency);                 // m_ 잡은 채
    void sendRelease();                                             // m_ 잡은 채 — Release + T100 재전송 무장
    void grantSelf(int durationSec);                                // m_ 잡은 채 — 'U: has permission' 진입
    bool sameUser(const std::string& a, const std::string& b) const;
    bool isStaleSeq(int seq) const;
    std::vector<cimsue::Talker> markSelf(const std::vector<Speaker>& in) const;

    const int callId_;
    const uint32_t ssrc_;
    const std::string userId_;
    Callbacks cb_;
    std::intptr_t sock_ = -1;                     // pj_sock_t (Win64 SOCKET 은 64비트 — long 불가)
    int localPort_ = 0;
    std::string remoteIp_;
    int remotePort_ = 0;
    std::atomic<bool> listenOnly_{false};
    std::atomic<bool> broadcastInitiator_{false};
    std::atomic<int> micDelayMs_{0};
    std::atomic<bool> running_{false};
    std::thread rx_;

    mutable std::mutex m_;
    FloorState state_ = FloorState::Idle;
    std::vector<cimsue::Talker> talkers_;
    bool canRequest_ = true;
    int indicator_ = 0;
    int queuePos_ = -1;
    int lastMsgSeq_ = -1;
    bool revokePending_ = false;
    bool pendingRelease_ = false;                 // U: pending Release — Floor Release 를 보낸 뒤 Idle 대기(§6.2.4.6)
    bool implicitPending_ = false;                // 암묵 요청을 실은 개시 INVITE 의 answer 대기(§6.2.4.2.2)
    bool implicitEmergency_ = false;
    bool releaseOnAnswer_ = false;                // answer 전에 놓았다 — answer 에서 Release(목적지는 answer 로 안다)
    bool micOn_ = false;
    unsigned grantedCount_ = 0, takenCount_ = 0, denyCount_ = 0;
    // 타이머 (Clock::time_point, 0 = 비활성)
    Clock::time_point nextAck_{}, requestDeadline_{}, talkDeadline_{}, releaseRetxAt_{};
    Clock::time_point micOpenAt_{};               // 승인 뒤 지연 개방 예정(setMicOpenDelay)
    int releaseRetxLeft_ = 0;
    std::string releaseRetxPkt_;

    static constexpr int kAckPeriodSec = 15;      // NAT UDP 매핑 유지 요건 ≤20s
    static constexpr int kRequestTimeoutMs = 3000;
    static constexpr int kReleaseRetxMs = 800;
    static constexpr int kReleaseRetxMax = 2;
    static constexpr int kTalkEndMarginMs = 300;  // Granted Duration 마감 직전 자체 종료
    static constexpr int kSeqReorderWindow = 64;
};

}  // namespace floor
}  // namespace cimsue
