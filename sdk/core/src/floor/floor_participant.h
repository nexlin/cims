// libcimsue 내부 — MCPTT floor participant (TS 24.380 §6.2.4) + RTCP-APP UDP 전송.
// 상태(FloorState)는 규격 상태에 대응한다: Idle·Listening = 'U: has no permission'(화자 없음·있음), Requesting = 'U: pending Request',
// Queued = 'U: queued', Speaking = 'U: has permission'. 'U: pending Release' 는 pendingRelease_ 로 든다(화면 상태는 has no permission).
// 그 상태에 절차가 없는 메시지는 버리고 상태를 유지한다(§6.2.4.1).
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
    /** 발언권 참여자 타이머·카운터(TS 24.380 표 11.1.1-1·11.2.1-1 — AccountConfig.floorTimers, 0 = 기본값). 다음 무장부터 쓴다. */
    void setTimers(const FloorTimers& t);
    /** RTP 미디어를 받았다 — 소유자(Engine)가 수신 패킷 수가 늘 때마다 알린다. T103(End of RTP media)을 다시 건다
     *  (§6.2.4.3.4 2. · §6.2.4.4.7 2. · §6.2.4.9.2 2.). 알림이 한 번도 없으면 T103 은 돌지 않는다(선택 타이머). */
    void onMedia();

    /** 개시 INVITE 가 암묵적 발언 요청이다(`mc_implicit_request`, TS 24.380 §14.2.5) — 호 성립 전부터 'U: pending Request'
     *  (§6.2.4.2.2 4.)로 둔다. Floor Request 는 보내지 않는다(요청은 INVITE 가 싣는다). emergency = 대체 명시 요청의 긴급 비트. */
    void armImplicitRequest(bool emergency);
    /** 개시 INVITE 의 200 OK answer(§14.3.4·§14.3.5) — granted(answer `mc_granted`) = 'U: has permission'(§6.2.4.4.2),
     *  accepted(answer `mc_implicit_request`) = Floor Granted 대기(요청 시한), 둘 다 없음 = 서버가 암묵 요청으로 받지 않았다(진행 중
     *  호 합류 등, §14.3.5) → 누르고 있으니 명시 Floor Request 로 잇는다. 그 전에 release() 했으면 발언권을 Floor Release 로 돌려준다.
     *  answer 뒤에 오는 Floor Granted 는 서버가 규격대로 따로 보내는 것이라(§6.3.4.4.2 1.) 'U: has permission' 에 머문다(§6.2.4.5.5). */
    void onInitialAnswer(bool granted, bool accepted);

    /** PTT 누름 — 'U: has no permission' 이면 Floor Request(§6.2.4.3.5, T101·C101 재전송), 대기 끝에 승인돼 T132 가 도는 중이면
     *  송출 의사(§6.2.4.9.12 — 'U: has permission'). */
    void request(int priority = -1, bool emergency = false);
    /** PTT 뗌 — 요청 중·발언 중·대기 중이면 Floor Release(§6.2.4.4.8 · §6.2.4.5.3 · §6.2.4.9.6, T100·C100 재전송). */
    void release();
    /** 내 대기 요청 취소 = Floor Release(§6.2.4.9.6 — Queued Floor Requests 는 남의 대기 요청을 지우는 인가 사용자의 절차다,
     *  §6.2.4.7.4). 대기 중이 아니면 아무것도 안 한다. */
    void cancelQueued();
    /** 대기열 위치 요청(§6.2.4.9.9) — 'U: queued' 이고 T132 가 돌지 않을 때. T104·C104 로 재전송, C104 회 무응답이면 대기 시간
     *  초과 + Floor Release(§6.2.4.9.11). */
    void requestQueuePosition();
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
    void sendRequest(int priority, bool emergency);                 // m_ 잡은 채 — Request + T101·C101 무장
    void sendRelease();                                             // m_ 잡은 채 — Release + T100·C100 무장
    void startRelease(const std::string& pkt);                      // m_ 잡은 채 — 그 Release 를 보내고 T100·C100 무장
    void stopRelease();                                             // m_ 잡은 채 — T100 정지('U: pending Release' 를 나간다)
    FloorState noPermissionState() const { return talkers_.empty() ? FloorState::Idle : FloorState::Listening; }
    void grantSelf(int durationSec);                                // m_ 잡은 채 — 'U: has permission' 진입
    bool sameUser(const std::string& a, const std::string& b) const;
    static bool isStaleSeq(int seq, int last);
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
    int lastTakenSeq_ = -1, lastIdleSeq_ = -1;            // Message Sequence Number — Floor Taken 묶음·Floor Idle 묶음 따로(§8.2.3.10)
    bool revokePending_ = false;
    bool pendingRelease_ = false;                 // U: pending Release — Floor Release 를 보낸 뒤 Idle 대기(§6.2.4.6)
    bool implicitPending_ = false;                // 암묵 요청을 실은 개시 INVITE 의 answer 대기(§6.2.4.2.2)
    bool implicitEmergency_ = false;
    bool releaseOnAnswer_ = false;                // answer 전에 놓았다 — answer 에서 Release(목적지는 answer 로 안다)
    bool micOn_ = false;
    unsigned grantedCount_ = 0, takenCount_ = 0, denyCount_ = 0;
    bool pttHeld_ = false;                        // request() 뒤 release() 전 — 사용자가 누르고 있다
    bool requestEmergency_ = false;               // 선점(긴급) 요청 — Floor Taken 에도 'U: pending Request' 유지(§6.2.4.4.11 7.)
    // 타이머 (Clock::time_point, 0 = 비활성)
    Clock::time_point nextAck_{}, requestDeadline_{}, talkDeadline_{}, releaseRetxAt_{};
    Clock::time_point micOpenAt_{};               // 승인 뒤 지연 개방 예정(setMicOpenDelay)
    Clock::time_point mediaEndAt_{};              // T103 — 받는 미디어의 끝
    Clock::time_point queuePosAt_{};              // T104 — 대기열 위치 요청 응답 대기
    Clock::time_point queuedGrantAt_{};           // T132 — 대기 끝 승인 뒤 사용자 조작 대기
    int queuedGrantDuration_ = -1;                // T132 동안 들고 있는 승인의 Duration
    std::string requestPkt_, releaseRetxPkt_, queuePosPkt_;
    int requestSends_ = 0, releaseSends_ = 0, queuePosSends_ = 0;   // C101 · C100 · C104
    int t100Ms_ = kDefT100Ms, t101Ms_ = kDefT101Ms, t103Ms_ = kDefT103Ms, t104Ms_ = kDefT104Ms, t132Ms_ = kDefT132Ms;
    int c100_ = kDefCounter, c101_ = kDefCounter, c104_ = kDefCounter;
    int ackStartLeft_ = 0;                        // 시작 Ack 연속 송신 남은 횟수(kAckStartCount)

    static constexpr int kAckPeriodSec = 15;      // NAT UDP 매핑 유지 요건 ≤20s
    // 시작 Ack 연속 — 목적지는 착신 offer(180 전)로 알지만 서버(CMP)는 200 OK 뒤 PTT_JOIN 에서야 멤버를 받고, NAT 멤버의 floor
    //   목적지를 그 멤버가 보낸 패킷으로 latch 한다. 첫 Ack 가 JOIN 보다 먼저 닿으면 버려져 다음 주기(15 s)까지 Floor Taken·Idle 이
    //   닿지 않는다(영상 협상으로 200 OK 가 늦어지면 재현) — pjmedia 시작 keep-alive(PJMEDIA_STREAM_START_KA_CNT)와 같은 규칙.
    static constexpr int kAckStartCount = 2;
    static constexpr int kAckStartIntervalMs = 1000;
    // 타이머·카운터 기본값(TS 24.380 표 11.1.1-1·11.2.1-1) — T100·T101 은 재전송 총 시간 6초 미만(NOTE 1·2), T103 = 서버 T1 기본,
    //   T132 = 규격 기본 2 s, C100·C101·C104 = 규격 기본 3. UE initial configuration 이 주면 그 값(setTimers).
    static constexpr int kDefT100Ms = 1000;
    static constexpr int kDefT101Ms = 1000;
    static constexpr int kDefT103Ms = 4000;
    static constexpr int kDefT104Ms = 4000;
    static constexpr int kDefT132Ms = 2000;
    static constexpr int kDefCounter = 3;
    static constexpr int kTalkEndMarginMs = 300;  // Granted Duration 마감 직전 자체 종료
    static constexpr int kSeqReorderWindow = 64;
};

}  // namespace floor
}  // namespace cimsue
