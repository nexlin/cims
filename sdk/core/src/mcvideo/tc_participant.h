// libcimsue 내부 — MCVideo 전송 제어 participant (TS 24.581 §6.2.4 송출 · §6.2.5 수신) + 제어 채널 RTCP UDP.
//
// MCVideo 호 하나에 하나 — 'basic transmission control' 상태 머신 하나(내 송출) + 'general reception control' 하나 + 송출마다
// 'basic reception control' 하나(§6.2.5.1). 메시지 코덱은 tc_codec(정의 정본 mcvideo_tc_defs.yaml).
//
// 스레딩: floor::Participant 와 같다 — 수신 스레드 1개(pjlib 등록)가 소켓 select(≤100 ms) → decode·상태 갱신·타이머 tick.
// 공개 메서드는 임의 스레드에서 부르며 mutex 로 직렬화한다. 콜백은 **락 밖에서**(수신 스레드 또는 호출 스레드) 부른다 —
// 소유자(Engine)가 이벤트 스레드로 넘긴다.
//
// 규격 해석(mcvideo.md §5.4):
//  - Transmission Revoked 를 받으면 송출을 닫고 원인 #7(Queue the transmission)이면 Queue Position Request 로 'U: queued', 그 밖의
//    원인은 Transmission End Request 로 'U: pending end' 에 간다 — §6.2.4.5.5 4 는 #5·#7 만 적었지만 서버는 회수 뒤 End Request 를
//    기다린다(§6.3.5.6 — T3 재전송, End Response 로 끝).
//  - 암묵적 송출 요청을 서버가 받지 않으면(answer 에 mc_implicit_request 없음 — chat 합류·진행 중 합류, §14.3.5) 곧바로 명시
//    Transmission Request 를 보낸다(T100 만료를 기다리지 않는다).
//  - Transmission End Notify(§6.2.5.3.4)는 그 송출의 수신도 닫는다 — 송출이 끝났으니 수신 인스턴스를 남기지 않는다.
//  - 제어 채널 NAT 유지(ue_nat_traversal.md §7.1) — 사용자가 누르기 전에는 보낼 전송 제어 메시지가 없어 NAT 뒤 단말의 하향 경로가
//    열리지 않는다. 호 성립 때 1회 + 1 s 간격 2회 + 15 s 주기로 빈 RTCP RR(RFC 3550 §6.4.2, 헤더 SSRC = 전송 제어와 같은 값)을 보낸다.
//    서버(CMP)는 그 소켓의 첫 패킷으로 제어 목적지를 latch 하고 APP 이 아닌 RTCP 는 해석하지 않고 버린다(cmp_media_api.md §7.9).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/types.h"
#include "tc_codec.h"

namespace cimsue {
namespace mcvideo {

/** 참여자 타이머·카운터(§11.1.1·§11.2.1) — 값의 정본은 service configuration `<tc-timers-counters-R14>`(초), 없으면 K5 기본값. */
struct TcTimers {
    int t100Ms = timer::T100_MS, t101Ms = timer::T101_MS, t102Ms = timer::T102_MS, t103Ms = timer::T103_MS,
        t104Ms = timer::T104_MS;
    int c100 = timer::C100, c101 = timer::C101, c102 = timer::C102, c103 = timer::C103, c104 = timer::C104;
};

class Participant {
public:
    struct Callbacks {
        std::function<void(const TransmissionEvent&)> onTransmission;
        std::function<void(const ReceptionEvent&)> onReception;
        /** 송출 게이트 — 'U: has permission to transmit' 에 들면 true(audio·video SSRC = Granted·answer 가 준 값, §6.2.4.4.6 2·§14.4),
         *  떠나면 false. 소유자가 audio·video 송출을 연다/닫는다. */
        std::function<void(bool on, uint32_t audioSsrc, uint32_t videoSsrc)> onSend;
        /** 수신 결선 — 한 송출의 수신 허가 시작(true)/끝(false). 소유자가 그 SSRC 의 렌더를 켠다/끈다. */
        std::function<void(const VideoTransmitter& t, bool on)> onReceive;
        std::function<void(int level, const std::string&)> log;
    };

    /** localSsrc = 이 단말이 서버에게서 기대하는 RTCP 헤더 SSRC — offer 의 `mc_transmission_ssrc` 로 광고한다(§4.3.3.1). */
    Participant(int callId, uint32_t localSsrc, const std::string& userId, Callbacks cb, TcTimers timers = TcTimers());
    ~Participant();

    /** 제어 채널 UDP 소켓 바인드(IPv4 any). localPort() = SDP `m=application <port> udp MCVideo` 광고 포트. */
    bool open(int localPort = 0);
    int localPort() const { return localPort_; }
    uint32_t localSsrc() const { return localSsrc_; }
    /** SDP answer 에서 배운 서버 제어 채널 목적지와 서버가 기대하는 RTCP 헤더 SSRC(answer `mc_transmission_ssrc`, 없으면 0 —
     *  다중화하지 않는 상대라 아무 값이나 된다, §4.3.3.1 NOTE 5). */
    void setRemote(const std::string& ip, int port, uint32_t remoteSsrc);
    /** 호 종류 지시자(§9.2.3.11) — 방송·system·긴급·임박 호면 요청에 싣는다. 일반 호는 0(싣지 않는다). */
    void setCallIndicator(int bits);

    // ── 호 성립 (§6.2.4.2 · §6.2.5.2) ──
    /** 개시 INVITE 가 암묵적 송출 요청이다(offer `mc_implicit_request`) — 'U: pending request to transmit'(§6.2.4.2.2 4). */
    void armImplicitRequest();
    /** 호 성립(개시 = 200 OK 수신, 착신 = 200 OK 송신). answer 해석: implicitAccepted = answer `mc_implicit_request`, granted =
     *  `mc_granted`, audio/videoSsrc = `mc_audio_ssrc`·`mc_video_ssrc`(§14.4 — 있으면 그 값을 쓴다, 0 = 없음). 호 성립 전에 받아 둔
     *  제어 메시지를 이때 처리한다(§6.2.4.2.2 2·4c). */
    void onEstablished(bool implicitAccepted = false, bool granted = false, uint32_t audioSsrc = 0, uint32_t videoSsrc = 0);

    // ── 사용자 조작 ──
    /** [영상 보내기] — 'U: has no permission' 에서만(§6.2.4.3.2). priority<0 = 기본 우선순위(싣지 않는다). */
    Result requestTransmission(int priority = -1);
    /** [보내기 끝] — 요청·허가·대기 중에서(§6.2.4.4.7·§6.2.4.5.3·§6.2.4.9.4). */
    Result releaseTransmission();
    /** [받기] — 알림을 받은 송출(§6.2.5.3.3). */
    Result acceptReception(const std::string& transmitterId, int priority = -1);
    /** [그만 보기] — 요청 중(취소, §6.2.5.4.6)·수신 중(§6.2.5.5.3). */
    Result endReception(const std::string& transmitterId);

    TransmissionInfo info() const;
    /** 호 해제(§6.2.4.7.2·§6.2.4.8.2·§6.2.5.3.5·§6.2.5.9.2) — 제어 메시지 송신을 멈추고 송출·수신을 닫고 타이머를 푼다. */
    void close();

    /** 누계 — 시험·계측용. */
    unsigned sentCount() const { return sent_.load(); }

    /** 제어 채널 NAT 유지 RR — 성립 뒤 kKeepaliveBurst 회는 kKeepaliveBurstGapMs 간격, 그 뒤 kKeepaliveIntervalMs 주기(CIMS 값 —
     *  UDP NAT 매핑 유지 15 s 는 SIP keepalive(account_map natConfig)와 같다). */
    static constexpr int kKeepaliveBurst = 3;
    static constexpr int kKeepaliveBurstGapMs = 1000;
    static constexpr int kKeepaliveIntervalMs = 15000;

private:
    using Clock = std::chrono::steady_clock;
    using Out = std::vector<std::function<void()>>;      // 락 밖에서 부를 콜백
    struct Reception {                                    // 'basic reception control' 한 인스턴스(+ 알림만 받은 송출)
        VideoTransmitter t;
        int priority = -1;
        int retries = 0;
        Clock::time_point deadline{};
    };

    void rxLoop();
    void tick();
    void handle(const Message& m, Out& out);                  // m_ 잡은 채
    void handleTransmission(const Message& m, Out& out);
    void handleReception(const Message& m, Out& out);
    void send(const std::string& pkt);                        // m_ 잡은 채
    void ack(const Message& m);
    void sendKeepalive();                                     // m_ 잡은 채
    uint32_t hdrSsrc() const { return remoteSsrc_ ? remoteSsrc_ : localSsrc_; }
    void emitTx(Out& out, TransmissionEvent::Kind k, const Message* m = nullptr, int cause = -1, const char* causeTable = nullptr);
    void emitRx(Out& out, ReceptionEvent::Kind k, const VideoTransmitter& t, const Message* m = nullptr, int cause = -1);
    void setSending(Out& out, bool on);
    void setReceiving(Out& out, Reception& r, bool on);
    void sendTransmissionRequest();                           // T100 무장
    void sendEndRequest();                                    // T101 무장
    void sendQueuePositionRequest();                          // T102 무장
    void sendReceiveRequest(Reception& r);                    // T103 무장
    void sendReceptionEnd(Reception& r);                      // T104 무장
    Reception* findReception(const std::string& id, uint32_t videoSsrc);
    static void flush(Out& out);

    const int callId_;
    const uint32_t localSsrc_;
    const std::string userId_;
    Callbacks cb_;
    const TcTimers timers_;
    std::intptr_t sock_ = -1;                             // pj_sock_t
    int localPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<unsigned> sent_{0};
    std::thread rx_;

    mutable std::mutex m_;
    std::string remoteIp_;
    int remotePort_ = 0;
    uint32_t remoteSsrc_ = 0;
    int indicator_ = 0;
    bool established_ = false;
    bool releasing_ = false;                              // 'Call releasing'
    std::vector<Message> early_;                          // 호 성립 전에 받은 메시지(§6.2.4.2.2 2)
    // 송출('basic transmission control')
    TransmissionState state_ = TransmissionState::NoPermission;
    int priority_ = -1;                                   // 요청 우선순위(재전송에 같게)
    int queuePosition_ = -1;
    bool sending_ = false;
    uint32_t txAudioSsrc_ = 0, txVideoSsrc_ = 0;
    int retries_ = 0;                                     // 지금 도는 타이머의 카운터(C100·C101·C102)
    Clock::time_point deadline_{};                        // T100·T101·T102 중 지금 도는 것
    // 수신('general reception control' + 송출별 'basic')
    std::vector<Reception> receptions_;
    // 제어 채널 NAT 유지(빈 RR)
    int kaSent_ = 0;
    Clock::time_point kaNext_{};
};

}  // namespace mcvideo
}  // namespace cimsue
