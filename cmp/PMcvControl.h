// PMcvControl.h — MCVideo 전송 제어 서버 상태 머신 (3GPP TS 24.581 §6.3.4 송출 · §6.3.5 참가자 송출 · §6.3.6 수신 · §6.3.7 참가자 수신).
//
// MCVideo 호 하나에 하나. 소켓·락·시계 없는 순수 로직이다 — 입력 = 참가자 추가/제거 · 해석된 전송 제어 메시지(ParsedTransmission) ·
// 미디어 도착 · 시각(ms) 틱, 출력 = 훅(send · inactivity · transmittersChanged · log). 소유자(PMcvideoGroup)가 그룹 락 아래 부르고
// send 를 BuildTransmissionMessage → 멤버 유닛 제어 채널로 잇는다. 단위시험 tests/cmp_mcvideo_control_test.cpp 는 이 파일과
// PTransmissionCodec.cpp 만 링크한다(설계 정본 mcvideo.md §5.3.1).
//
// 규격의 네 기계를 그대로 둔다:
//   일반 송출(G — Transmit Idle / Taken, Cx = 허가된 송출 수 · 상한 max_transmitters) — 'G: pending Transmission Revoke' 는 송출마다의
//     표식(revoking)이다. 동시 송출에서 회수는 송출 하나에 걸린다.
//   참가자 송출(U — not permitted and Transmit Idle / not permitted and Transmit Taken / permitted / pending Transmit Revoke /
//     not permitted but sends media).
//   일반 수신(Gr — Reception Idle / Reception accepted, C7 = Active SSRC List 항목 수의 합 = 송출별 C11 의 합).
//   참가자 수신(U — not permitted / permitted to receive, C9 = 그 참가자 Active SSRC List 의 송출 수, 상한 max_rx_streams).
//
// C7·C11 은 수로만 쓴다(0 이냐 아니냐 — T5·T11 의 시작·정지). 규격 본문에 두 카운터의 상한으로 요청을 거절하는 절차가 없어
// 계산한 값(목록 크기)으로 두고 service configuration 의 C7·C11 상한은 읽기만 한다. 수신 허가의 상한은 C9 다(§6.3.7.4.10 1a).
#ifndef __PMCV_CONTROL_H__
#define __PMCV_CONTROL_H__

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "PTransmissionCodec.h"
#include "PTransmissionDefs.h"

// 서버 전송 제어 타이머·카운터 (TS 24.581 §11.1.3·§11.2.3 — 정본 mcvideo_tc_defs.yaml, 생성 상수 MCV_*).
//   T1 = 그룹 on-network-hang-timer · T5 = 그룹 on-network-reception-hang-timer · 나머지 = MCVideo service configuration
//   `<tc-timers-counters-R14>`. CSP 가 PTT_GROUP_ADD.tc_timers 로 싣고, 미지정 필드는 K5 기본값이다. t1Ms·t5Ms 0 = 그 타이머 미사용.
struct McvTimers {
    int t1Ms = MCV_T1_MS;    // Inactivity
    int t2Ms = MCV_T2_MS;    // Transmission Idle
    int t3Ms = MCV_T3_MS;    // Transmission Revoke
    int t4Ms = MCV_T4_MS;    // Transmission Granted
    int t5Ms = MCV_T5_MS;    // Reception Inactivity
    int t6Ms = MCV_T6_MS;    // Reception Granted
    int t11Ms = MCV_T11_MS;  // Stream Reception Idle
    int c2 = MCV_C2;         // Transmission Idle
    int c4 = MCV_C4;         // Transmission Granted
    int c6 = MCV_C6;         // Reception Granted
    int c7 = MCV_C7;         // Reception Accepted (수로만 — 헤더 머리말)
    int c11 = MCV_C11;       // Count of active receivers for the stream (수로만)
};

// 호 종류 (PTT_GROUP_ADD.call_type) — Transmission Indicator 와 수신 모드를 정한다(§9.2.3.11 · §6.3.6.3.3).
enum McvCallType { MCV_CALL_NORMAL = 0, MCV_CALL_IMMINENT = 1, MCV_CALL_EMERGENCY = 2 };

// 참가자 선언 — 전송 제어가 쓰는 값만(주소·포트는 그룹 몫).
struct McvParticipantDecl {
    std::string userId;       // MCVideo ID — User ID 필드 값
    bool recvOnly = false;    // 그룹 문서 <on-network-recvonly> — 송출 요청 거절 #5 (§6.3.4.3.3 1b)
    bool queueing = false;    // SDP mc_queueing 협상 (§14.2.2)
    int rosterPriority = 0;   // 그룹 문서 <user-priority> — 기본 우선순위(default priority)
    int maxPriority = -1;     // 협상 mc_priority 상한 (-1 = 미협상 — 요청의 Transmission Priority 를 쓰지 않는다)
    int maxRxPriority = -1;   // 협상 mc_reception_priority 상한 (§14.3.6, -1 = 미협상). 수신 우선순위 = min(요청, 상한)
                              //   (§6.3.7.3.4) — 수신 선점(override)이 없어 판정에는 아직 쓰지 않는다(mcvideo.md §5.3.1)
    bool chair = false;       // 로스터 role "chair"
    int maxRxStreams = MCV_C9;       // C9 — 동시 수신 스트림 상한 (user profile MaxSimultaneousVideoStreams)
    unsigned int preferredAudioSsrc = 0;  // 멤버 offer 의 audio a=ssrc (송출 SSRC 할당 선호값, 0 = 없음)
    unsigned int preferredVideoSsrc = 0;  // 멤버 offer 의 video a=ssrc
};

// 진행 중 송출 하나 (TRANSMITTERS 이벤트·분배용).
struct McvTransmitter {
    std::string memberId;
    std::string userId;
    unsigned int audioSsrc = 0;
    unsigned int videoSsrc = 0;
    bool revoking = false;
};

// 참가자 송출 상태 (§6.3.5).
enum McvTxState {
    MCV_U_IDLE = 0,          // 'U: not permitted and Transmit Idle'
    MCV_U_TAKEN,             // 'U: not permitted and Transmit Taken'
    MCV_U_PERMITTED,         // 'U: permitted'
    MCV_U_PENDING_REVOKE,    // 'U: pending Transmit Revoke' (서버가 Revoked·End Request 를 보냈다)
    MCV_U_SENDS_MEDIA        // 'U: not permitted but sends media'
};

class PMcvControl {
public:
    struct Hooks {
        // 멤버 하나에게 전송 제어 메시지 하나. subtype 은 메시지 타입(| MCV_ACK_REQ_BIT). 소유자가 부호화·송신한다.
        std::function<void(const std::string& memberId, int app, int subtype, const std::vector<McvTlv>& fields)> send;
        // T1(Inactivity)·T5(Reception Inactivity) 만료 — 호 해제는 신호 평면 정책(§6.3.4.3.5·§6.3.6.3.5). 만료 1회 뒤 다시 무장한다.
        std::function<void(const char* timer)> inactivity;
        // 송출 집합이 바뀌었다(허가·종료·회수·이탈) — TRANSMITTERS 이벤트.
        std::function<void(const std::vector<McvTransmitter>& transmitters)> transmittersChanged;
        // 수신자가 송출 하나를 받기 시작했다(Active SSRC List 에 들어갔다 — 허가·알림·[받기] 어느 경로든). 영상은 다음 키프레임부터
        //   풀리므로 미디어 평면이 송출자에게 키프레임을 요청한다(RFC 4585 PLI — mcvideo.md §5.3 B6). 소유자는 _ctl 을 다시 부르지 않는다.
        std::function<void(const std::string& receiverId, const std::string& senderId)> receptionStarted;
        std::function<void(const std::string& line)> log;
    };

    // 암묵적 송출 요청(JOIN implicit_request)의 결과 — CSP 가 answer 의 mc_implicit_request·mc_granted·mc_audio_ssrc·mc_video_ssrc 로 싣는다.
    struct ImplicitResult {
        bool granted = false;
        unsigned int audioSsrc = 0;
        unsigned int videoSsrc = 0;
    };

    PMcvControl();
    ~PMcvControl();

    void setHooks(const Hooks& h) { _hooks = h; }
    // 호 속성 (PTT_GROUP_ADD·MODIFY). 진행 중 호에서 바뀐 상한은 다음 요청부터 반영한다(진행 중 송출은 회수하지 않는다).
    void configure(int maxTransmitters, bool receptionAutomatic, McvCallType callType, const McvTimers& timers);

    // ── 참가자 (§6.3.5.2 · §6.3.7.2) ──
    // 주소가 등록된 참가자(JOIN ②). 이미 있으면 협상 값만 갱신한다(상태 유지 — JOIN ② 재전송·refresh).
    //   implicitRequest = 새 prearranged 세션 개시의 암묵적 송출 요청(JOIN implicit_request, §6.3.5.2.2 1). res 에 결과를 채운다.
    void addParticipant(const std::string& id, const McvParticipantDecl& decl, int64_t nowMs, bool implicitRequest = false,
                        ImplicitResult* res = nullptr);
    // 이탈(LEAVE) — 송출 중이면 송출을 끝내고(End Notify 전원 · Cx−1 · Idle/큐), 큐에서 빼고, 수신 몫을 정리한다(§6.3.5.8.2 · §6.3.4.4.11).
    void removeParticipant(const std::string& id, int64_t nowMs);
    // 제어 채널 목적지가 잡혔다·바뀌었다(NAT latch) — 합류 때 알린 지금 상태를 다시 보낸다: 진행 중 송출이 없으면 Transmission Idle,
    //   있으면 송출마다 Media Transmission Notification(§6.3.5.2.2 2a·4 · §6.3.7.2.2 2b ii · §6.3.7.3.3 1). JOIN ② 의 알림은 SDP 주소로
    //   나가 NAT 뒤 단말에 닿지 않는다(단말은 호가 선 뒤 빈 RR 로 하향 경로를 연다). 허가·요청 중인 참가자는 스스로 보냈으니 두지 않는다.
    void resendJoinState(const std::string& id, int64_t nowMs);
    // 호 해제 — 타이머를 풀고 SSRC 를 반환한다. 메시지는 보내지 않는다(§6.3.4.6.2 · §6.3.4.7.2).
    void close();
    bool hasParticipant(const std::string& id) const { return _parts.count(id) != 0; }

    // ── 입력 ──
    void onMessage(const std::string& id, const ParsedTransmission& m, int64_t nowMs);
    // 미디어 도착(payload 있는 RTP 만 — 헤더만 = keepalive 는 부르지 않는다). 분배할 송출이면 true + 그 송출의 할당 SSRC 쌍.
    //   허가 없는 참가자면 버림(false) + Revoked #3 (§6.3.5.4.6 · §6.3.5.3.8).
    bool onMedia(const std::string& id, int64_t nowMs, unsigned int& audioSsrc, unsigned int& videoSsrc);
    // receiverId 의 Active SSRC List 에 senderId 의 송출이 있는가 (§6.3.7.4.3).
    bool receives(const std::string& receiverId, const std::string& senderId) const;
    void tick(int64_t nowMs);

    // ── 관측 ──
    std::vector<McvTransmitter> transmitters() const;
    int transmitterCount() const { return (int)_tx.size(); }   // Cx
    int receptionCount() const;                                  // C7
    McvTxState txState(const std::string& id) const;
    int queuePosition(const std::string& id) const;              // 1.., 큐에 없으면 0
    int participantCount() const { return (int)_parts.size(); }

    // ── SSRC — 프로세스 전역 유일 (TS 24.581 §6.3.4.3.3 d · 전송 제어 채널 tc_ssrc) ──
    // preferred = 단말이 offer a=ssrc 로 준 값(0 = 없음) — 전역에서 쓰이지 않으면 그 값, 아니면 새 값(§14.3.7·§14.3.8).
    static unsigned int AllocSsrc(unsigned int preferred = 0);
    static void FreeSsrc(unsigned int ssrc);

    // 서버가 회수·종료 요구(Revoked · End Request)를 T3 마다 다시 보내는 횟수 — 넘으면 서버에서 송출을 끝낸다(§6.3.5.6.3 NOTE: 구현 선택).
    static const int kT3Retries = 5;
    // 송출이 끝난 직후의 무허가 미디어 유예(ms) — End Request 와 엇갈려 이미 떠난 RTP 는 회수하지 않고 버린다(구현 선택 — 규격이 막는 것은
    //   허가 뒤에도 «계속» 보내는 참가자다, §6.3.5.3.8·§6.3.5.4.6).
    static const int kEndGraceMs = 500;
    // 늦은 암묵 허가의 대기 한도(ms) — 참여자는 T100×C100 동안만 Granted 를 기다리고 그 뒤엔 'U: has no permission' 이라 늦은 Granted 를
    //   버린다(TS 24.581 §6.2.4.4.4). 그 안에 첫 초대 참가자가 오지 않으면 예약을 풀고 Transmission Idle 을 보낸다. 값 = 참여자 타이머
    //   기본값(K5 — service configuration 이 바꾸면 CSP 가 PTT_GROUP_ADD 로 싣는 몫, 1차는 기본값).
    static const int kImplicitWaitMs = MCV_T100_MS * MCV_C100;

private:
    // 유효 우선순위(§4.1.1.4) — tier(긴급 > 임박 > 일반, CSP 지시) → chair → 수치.
    struct Prio {
        int tier = 0;
        bool chair = false;
        int value = 0;
    };
    struct Tx {                         // 허가된 송출 하나
        std::string member;
        std::string userId;             // Transmitting User ID — 송출자가 이탈한 뒤의 End Notify 에도 쓴다
        unsigned int audioSsrc = 0, videoSsrc = 0;
        Prio prio;
        int64_t grantMs = 0;
        bool revoking = false;          // 서버가 Revoked(#4 선점) 또는 End Request(#8 받는 이 없음)를 보냈다
        bool endRequest = false;        // revoking 이 End Request(#8)인가 (아니면 Revoked)
        int revokeCause = 0;
        int64_t t3At = 0;               // T3 만료 시각(0 = 정지)
        int t3Count = 0;
        int64_t t4At = 0;               // T4 — 기다린 요청(큐 · 늦은 암묵 요청)의 Granted 재송신(첫 미디어에 정지)
        int c4 = 0;
        int64_t t11At = 0;              // T11 — manual 수신에서 받는 이 없이 지난 시간
    };
    struct RxGrant {                    // Active SSRC List 한 항목(송출 하나)
        int64_t t6At = 0;               // T6 — Receive Media Response(Granted) 재송신(Ack 에 정지)
        int c6 = 0;
    };
    struct Part {
        std::string id;
        McvParticipantDecl decl;
        McvTxState state = MCV_U_IDLE;
        bool endedPermitted = false;    // 허가 송출을 End Request 로 끝낸 적 — Idle 에서의 무허가 미디어 판정(§6.3.5.3.8)
        int64_t endMs = 0;              // 마지막 송출이 끝난 시각 (kEndGraceMs)
        int64_t t3At = 0;               // 'U: not permitted but sends media' 의 T3 (Revoked #3 재송신)
        int t3Count = 0;
        bool implicitPending = false;   // 암묵적 요청을 받았고 첫 초대 참가자를 기다린다(§6.3.2.2)
        int64_t implicitUntil = 0;      // 기다림 한도 (kImplicitWaitMs)
        unsigned int implicitAudio = 0, implicitVideo = 0;   // 암묵적 요청으로 예약한 SSRC 쌍
        std::map<std::string, RxGrant> active;   // senderMember → Active SSRC List 항목 (C9 = 크기)
    };
    struct Queued {
        std::string member;
        Prio prio;
        bool preemptive = false;  // 선점으로 큐 맨 앞에 넣은 요청 — 그 동안 다른 선점 요청은 선점하지 못한다(§6.3.5.4.4 5))
    };

    // 판정
    Prio _effectivePrio(const Part& p, const ParsedTransmission* req) const;
    static bool _outranks(const Prio& a, const Prio& b);
    Tx* _weakestTx();
    Tx* _txOf(const std::string& member);
    Tx* _txByIdentity(const ParsedTransmission& m);
    Part* _part(const std::string& id);
    int _indicator() const;
    bool _automaticReception() const;

    // 송출
    void _onTxRequest(Part& p, const ParsedTransmission& m, int64_t now);
    void _grant(Part& p, const Prio& prio, bool retransmit, int64_t now, unsigned int audio = 0, unsigned int video = 0);
    void _sendGranted(const Tx& t);
    void _onEndRequest(Part& p, const ParsedTransmission& m, int64_t now);
    void _onEndResponse(Part& p, int64_t now);
    void _endTransmission(const std::string& member, int64_t now, const char* why);
    bool _afterSlotFreed(int64_t now);
    void _enterIdle(int64_t now);
    void _revoke(Tx& t, int cause, bool endRequest, int64_t now);
    void _sendRevoke(const Tx& t);
    void _enqueue(Part& p, const Prio& prio, bool front);
    void _dequeue(const std::string& member);
    void _sendQueueInfo(Part& p);
    void _sendReject(Part& p, int cause);
    void _sendIdle(const std::string& member);
    void _sendNotification(const std::string& to, const Tx& t);
    void _notifyCurrent(Part& p, int64_t now);   // 진행 중 송출마다 Notification(+ automatic 이면 수신 허가)

    // 수신
    void _onRxRequest(Part& r, const ParsedTransmission& m, int64_t now);
    void _onRxEnd(Part& r, const ParsedTransmission& m, bool request, int64_t now);
    void _onAck(Part& r, const ParsedTransmission& m);
    int _receiversOf(const std::string& sender) const;             // C11
    void _addReception(Part& r, Tx& t, bool manual, int64_t now);
    void _dropReception(Part& r, const std::string& sender, int64_t now);
    void _sendRxResponse(const std::string& to, const Tx* t, const ParsedTransmission* req, bool granted, int cause,
                         bool ackReq);
    void _rearmT5(int64_t now);

    // 공통
    void _send(const std::string& member, int app, int subtype, const std::vector<McvTlv>& fields);
    void _ack(const std::string& member, const ParsedTransmission& m);
    void _txFields(std::vector<McvTlv>& f, const Tx& t) const;   // Transmitting User ID + SSRC 쌍
    void _indicatorField(std::vector<McvTlv>& f) const;
    void _transmittersChanged();
    void _log(const std::string& line);

    Hooks _hooks;
    int _maxTransmitters = 1;
    bool _receptionAutomatic = false;
    McvCallType _callType = MCV_CALL_NORMAL;
    McvTimers _timers;

    std::map<std::string, Part> _parts;
    std::map<std::string, Tx> _tx;          // senderMember → 송출 (Cx = 크기)
    std::deque<Queued> _queue;              // active Transmission request queue
    bool _started = false;                  // 첫 참가자 — 호 시작(T1·T5 무장)
    unsigned int _msgSeq = 0;               // Message Sequence Number (§9.2.3.9)
    int64_t _t1At = 0;                      // T1 Inactivity (G: Idle 동안)
    int64_t _t2At = 0;                      // T2 Transmission Idle 재송신
    int _c2 = 0;
    int64_t _t5At = 0;                      // T5 Reception Inactivity (Gr: Idle 동안)
};

#endif  // __PMCV_CONTROL_H__
