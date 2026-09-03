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
    bool micOn_ = false;
    unsigned grantedCount_ = 0, takenCount_ = 0, denyCount_ = 0;
    // 타이머 (Clock::time_point, 0 = 비활성)
    Clock::time_point nextAck_{}, requestDeadline_{}, talkDeadline_{}, releaseRetxAt_{};
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
