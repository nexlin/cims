#include "floor_participant.h"

#include <pjlib.h>

namespace cimsue {
namespace floor {


Participant::Participant(int callId, uint32_t ssrc, const std::string& userId, Callbacks cb)
    : callId_(callId), ssrc_(ssrc), userId_(userId), cb_(std::move(cb)) {}

Participant::~Participant() { close(); }

bool Participant::open(int localPort) {
    pj_sock_t s;
    if (pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s) != PJ_SUCCESS) return false;
    pj_sockaddr_in addr;
    pj_sockaddr_in_init(&addr, nullptr, (pj_uint16_t)localPort);
    if (pj_sock_bind(s, &addr, sizeof(addr)) != PJ_SUCCESS) { pj_sock_close(s); return false; }
    int len = sizeof(addr);
    if (pj_sock_getsockname(s, &addr, &len) == PJ_SUCCESS) localPort_ = pj_ntohs(addr.sin_port);
    sock_ = (std::intptr_t)s;
    running_ = true;
    rx_ = std::thread([this] { rxLoop(); });
    if (cb_.log) cb_.log(3, "floor socket bound :" + std::to_string(localPort_) + " (call " + std::to_string(callId_) + ")");
    return true;
}

void Participant::close() {
    if (!running_.exchange(false)) { if (rx_.joinable()) rx_.join(); return; }
    if (rx_.joinable()) rx_.join();
    if (sock_ >= 0) { pj_sock_close((pj_sock_t)sock_); sock_ = -1; }
}

void Participant::setRemote(const std::string& ip, int port) {
    std::lock_guard<std::mutex> lk(m_);
    remoteIp_ = ip;
    remotePort_ = port;
    if (nextAck_ == Clock::time_point{}) nextAck_ = Clock::now();   // 즉시 1회 + 주기
    if (cb_.log) cb_.log(3, "floor remote " + ip + ":" + std::to_string(port) + " (call " + std::to_string(callId_) + ")");
}

void Participant::send(const std::string& pkt) {
    if (remotePort_ <= 0 || sock_ < 0) { if (cb_.log) cb_.log(2, "floor send before remote learned"); return; }
    pj_sockaddr_in to;
    pj_str_t ip = pj_str(const_cast<char*>(remoteIp_.c_str()));
    if (pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)remotePort_) != PJ_SUCCESS) return;
    pj_ssize_t n = (pj_ssize_t)pkt.size();
    pj_sock_sendto((pj_sock_t)sock_, pkt.data(), &n, 0, &to, sizeof(to));
}

void Participant::emit(FloorEvent ev) {
    ev.callId = callId_;
    if (cb_.onEvent) cb_.onEvent(ev);
}

void Participant::setMic(bool on) {
    if (!on) micOpenAt_ = {};                               // 지연 개방 대기 중에 발언을 잃었다 — 열지 않는다
    if (micOn_ == on) return;
    micOn_ = on;
    if (cb_.onMic) cb_.onMic(on);
}

bool Participant::sameUser(const std::string& a, const std::string& b) const {
    auto bare = [](const std::string& s) {
        size_t c = s.find(':');
        std::string t = c == std::string::npos ? s : s.substr(c + 1);
        size_t at = t.find('@');
        return at == std::string::npos ? t : t.substr(0, at);
    };
    return !a.empty() && !b.empty() && bare(a) == bare(b);
}

std::vector<Talker> Participant::markSelf(const std::vector<Speaker>& in) const {
    std::vector<Talker> out;
    for (auto& t : in) out.push_back(Talker{t.id, t.ssrc, sameUser(t.id, userId_)});
    return out;
}

bool Participant::isStaleSeq(int seq) const {
    if (lastMsgSeq_ < 0) return false;
    int d = (lastMsgSeq_ - seq) & 0xffff;
    return d >= 0 && d < kSeqReorderWindow;
}

// ── 명령 ──

void Participant::sendRequest(int priority, bool emergency) {
    releaseRetxLeft_ = 0;
    pendingRelease_ = false;
    // 긴급 세션의 발언은 Floor Indicator emergency 비트 — CMP tier 상향/선점(TS 24.380).
    //   일제 통화 개시자는 호 종류 B-bit 를 함께 싣는다(§6.2.4.3.5 1.b, 비트 OR — §8.2.3.15).
    int ind = (emergency ? (int)indicator::EMERGENCY : 0) | (broadcastInitiator_ ? (int)indicator::BROADCAST_GROUP : 0);
    send(floor::request(ssrc_, userId_, priority, ind ? ind : -1));
    state_ = FloorState::Requesting;
    requestDeadline_ = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
}

void Participant::sendRelease() {
    releaseRetxPkt_ = floor::release(ssrc_, userId_);
    send(releaseRetxPkt_);
    pendingRelease_ = true;
    releaseRetxLeft_ = kReleaseRetxMax;
    releaseRetxAt_ = Clock::now() + std::chrono::milliseconds(kReleaseRetxMs);
}

void Participant::grantSelf(int durationSec) {
    revokePending_ = false; releaseRetxLeft_ = 0; requestDeadline_ = {}; pendingRelease_ = false;
    bool haveSelf = false;
    for (auto& t : talkers_) if (t.self) haveSelf = true;
    if (!haveSelf) talkers_.insert(talkers_.begin(), Talker{userId_, ssrc_, true});
    state_ = FloorState::Speaking;
    long d = durationSec > 0 ? durationSec * 1000L : 0;
    talkDeadline_ = d > kTalkEndMarginMs ? Clock::now() + std::chrono::milliseconds(d - kTalkEndMarginMs) : Clock::time_point{};
    if (micDelayMs_ > 0 && !micOn_) micOpenAt_ = Clock::now() + std::chrono::milliseconds(micDelayMs_.load());   // 승인 톤 뒤(tick)
    else setMic(true);
}

void Participant::armImplicitRequest(bool emergency) {
    std::lock_guard<std::mutex> lk(m_);
    if (listenOnly_) return;
    implicitPending_ = true;
    implicitEmergency_ = emergency;
    releaseOnAnswer_ = false;
    state_ = FloorState::Requesting;             // 'U: pending Request' — 시한(T101)은 answer 에서 건다(목적지를 그때 안다)
}

void Participant::onInitialAnswer(bool granted, bool accepted) {
    FloorEvent ev;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!implicitPending_) return;
        implicitPending_ = false;
        if (releaseOnAnswer_) {
            // 호 성립 전에 놓았다 — 받아들여졌거나 승인됐을 발언권을 돌려준다(받아들여지지 않은 요청의 Release 는 서버가 무시한다).
            releaseOnAnswer_ = false;
            sendRelease();
            return;
        }
        if (state_ == FloorState::Speaking) return;                   // Floor Granted 가 answer 보다 먼저 왔다
        if (state_ != FloorState::Requesting) return;                 // Deny·Taken 등으로 이미 결정됐다
        if (granted) {
            grantSelf(-1);                                            // §6.2.4.4.2 — 200 OK 의 floor granted 표시, 시한은 Floor Granted 의 Duration 이 준다
            grantedCount_++;
            ev.kind = FloorEvent::Kind::Granted;
            notify = true;
        } else if (accepted) {
            requestDeadline_ = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);   // Floor Granted 대기(T101)
        } else {
            if (cb_.log) cb_.log(3, "floor implicit request not accepted — explicit Floor Request (call " + std::to_string(callId_) + ")");
            sendRequest(-1, implicitEmergency_);
        }
        if (notify) { ev.state = state_; ev.talkers = talkers_; ev.indicator = indicator_; }
    }
    if (notify) emit(ev);
}

void Participant::request(int priority, bool emergency) {
    std::lock_guard<std::mutex> lk(m_);
    if (listenOnly_ || !canRequest_) {
        FloorEvent ev; ev.kind = FloorEvent::Kind::Denied; ev.state = state_; ev.cause = 5;
        ev.causeText = "Receive only"; emit(ev); return;
    }
    if (state_ == FloorState::Speaking || state_ == FloorState::Requesting || state_ == FloorState::Queued) return;
    if (implicitPending_) {                                           // answer 전에 놓았다가 다시 눌렀다 — 암묵 요청이 아직 유효하다
        releaseOnAnswer_ = false;
        state_ = FloorState::Requesting;
        return;
    }
    sendRequest(priority, emergency);
}

void Participant::release() {
    std::lock_guard<std::mutex> lk(m_);
    releaseRetxLeft_ = 0;
    talkDeadline_ = {};
    requestDeadline_ = {};
    if (implicitPending_) {
        // 개시 INVITE 의 answer 전 — 목적지를 몰라 Release 를 보낼 수 없다. answer 에서 보낸다(onInitialAnswer).
        releaseOnAnswer_ = true;
        state_ = FloorState::Idle;
        setMic(false);
        return;
    }
    // 대기 중이면 대기 요청부터 취소(§8.2.15) — 발언 중이 아닌 leg 의 Release 는 서버가 무시한다.
    if (state_ == FloorState::Queued) send(cancelQueuedRequest(ssrc_));
    // 요청/점유한 적이 있을 때만 Release — 그 외의 Release 는 고아 메시지.
    //   U: pending Release — T100 으로 재전송(§6.2.4.6.2), Idle·Taken 이 오면 멈춘다. 유실되면 서버가 발언권을 계속 쥔다.
    if (state_ == FloorState::Speaking || state_ == FloorState::Requesting || state_ == FloorState::Queued) sendRelease();
    queuePos_ = -1;
    setMic(false);
    // 내 발언만 끝난다 — 동시 발언 중이면 남은 화자를 계속 듣는다.
    std::vector<Talker> rest;
    for (auto& t : talkers_) if (!t.self) rest.push_back(t);
    talkers_ = rest;
    state_ = rest.empty() ? FloorState::Idle : FloorState::Listening;
}

void Participant::cancelQueued() {
    std::lock_guard<std::mutex> lk(m_);
    send(cancelQueuedRequest(ssrc_));
    if (state_ == FloorState::Queued) state_ = FloorState::Idle;
}

FloorInfo Participant::info() const {
    std::lock_guard<std::mutex> lk(m_);
    FloorInfo i;
    i.state = state_; i.talkers = talkers_; i.canRequest = canRequest_ && !listenOnly_; i.indicator = indicator_;
    i.queuePosition = state_ == FloorState::Queued ? queuePos_ : -1;
    i.localPort = localPort_; i.remoteIp = remoteIp_; i.remotePort = remotePort_;
    i.grantedCount = grantedCount_; i.takenCount = takenCount_; i.denyCount = denyCount_;
    return i;
}

// ── 수신 ──

void Participant::rxLoop() {
    pj_thread_desc desc;
    pj_thread_t* th = nullptr;
    pj_bzero(desc, sizeof(desc));
    pj_thread_register("floor-rx", desc, &th);
    unsigned char buf[1500];
    while (running_) {
        pj_fd_set_t fds;
        PJ_FD_ZERO(&fds);
        PJ_FD_SET((pj_sock_t)sock_, &fds);
        pj_time_val tv = {0, 100};                          // 틱 ≤100 ms — 지연 개방이 그보다 가까우면 그때 깬다
        {
            std::lock_guard<std::mutex> lk(m_);
            if (micOpenAt_ != Clock::time_point{}) {
                long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(micOpenAt_ - Clock::now()).count();
                if (ms < tv.msec) tv.msec = ms > 0 ? ms : 0;
            }
        }
        int n = pj_sock_select((int)sock_ + 1, &fds, nullptr, nullptr, &tv);
        if (n > 0 && PJ_FD_ISSET((pj_sock_t)sock_, &fds)) {
            pj_ssize_t len = sizeof(buf);
            pj_sockaddr_in from; int fl = sizeof(from);
            if (pj_sock_recvfrom((pj_sock_t)sock_, buf, &len, 0, &from, &fl) == PJ_SUCCESS && len > 0) {
                Message m;
                if (decode(buf, (size_t)len, m)) handle(m);
            }
        }
        tick();
    }
}

void Participant::tick() {
    std::vector<FloorEvent> pending;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto now = Clock::now();
        if (nextAck_ != Clock::time_point{} && now >= nextAck_ && remotePort_ > 0) {
            send(ack(ssrc_, userId_));                                    // NAT keepalive(≤20s)
            nextAck_ = now + std::chrono::seconds(kAckPeriodSec);
        }
        if (micOpenAt_ != Clock::time_point{} && now >= micOpenAt_) {    // 승인 톤 뒤 마이크 개방
            micOpenAt_ = {};
            if (state_ == FloorState::Speaking) setMic(true);
        }
        if (releaseRetxLeft_ > 0 && now >= releaseRetxAt_) {              // Release 재전송(T100, §6.2.4.6.2)
            send(releaseRetxPkt_);
            if (--releaseRetxLeft_ > 0) releaseRetxAt_ = now + std::chrono::milliseconds(kReleaseRetxMs);
            else pendingRelease_ = false;                                 // T100 N회 만료 → U: has no permission(§6.2.4.6.3)
        }
        if (requestDeadline_ != Clock::time_point{} && now >= requestDeadline_) {
            requestDeadline_ = {};
            if (state_ == FloorState::Requesting) {                      // GRANT/DENY 무응답 → Idle
                state_ = FloorState::Idle;
                setMic(false);
                FloorEvent ev; ev.kind = FloorEvent::Kind::RequestTimeout; ev.state = state_;
                pending.push_back(ev);
            }
        }
        if (talkDeadline_ != Clock::time_point{} && now >= talkDeadline_) {   // Granted Duration(T2) 자체 종료
            talkDeadline_ = {};
            if (state_ == FloorState::Speaking) {
                releaseRetxPkt_ = floor::release(ssrc_, userId_);
                send(releaseRetxPkt_);
                pendingRelease_ = true;
                releaseRetxLeft_ = kReleaseRetxMax;
                releaseRetxAt_ = now + std::chrono::milliseconds(kReleaseRetxMs);
                setMic(false);
                std::vector<Talker> rest;
                for (auto& t : talkers_) if (!t.self) rest.push_back(t);
                talkers_ = rest;
                state_ = rest.empty() ? FloorState::Idle : FloorState::Listening;
                FloorEvent ev; ev.kind = FloorEvent::Kind::TalkLimit; ev.state = state_; ev.talkers = talkers_;
                pending.push_back(ev);
            }
        }
    }
    for (auto& ev : pending) emit(ev);
}

void Participant::handle(const Message& m) {
    FloorEvent ev;
    bool broadcastEnd = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        // Ack 요구 변종(§8.2.2) — 상태 처리보다 먼저 회신(없으면 상대가 T100 재전송).
        if (m.ackRequired) send(ackOf(ssrc_, (uint8_t)(m.op | kAckRequiredBit)));
        // Message Sequence Number(§8.2.3.10) — Taken/Idle 순서 역전·재전송 폐기.
        if (m.op == (uint8_t)Op::TAKEN || m.op == (uint8_t)Op::IDLE) {
            int seq = m.msgSeq();
            if (seq >= 0) {
                if (isStaleSeq(seq)) return;
                lastMsgSeq_ = seq;
            }
        }
        ev.rawType = m.op;
        ev.indicator = m.indicator() < 0 ? 0 : m.indicator();
        switch ((Op)m.op) {
            case Op::GRANTED: {
                // U: pending Release 에서 받은 Granted(§6.2.4.6.8) — 놓은 뒤 늦게 온 승인이다. Ack(위에서 회신)만 하고
                //   상태를 유지한다: 마이크를 열거나 Speaking 으로 가면 이어 오는 Idle 을 무시해 서버는 유휴인데 단말만
                //   발언 중이 되고, 일제 통화 개시자는 호 해제(§6.2.4.6.4)를 놓친다. 재전송 중인 Release 가 서버를 정리한다.
                //   개시 INVITE 의 answer 전에 놓은 경우(releaseOnAnswer_)도 같다 — Release 는 answer 에서 나간다.
                if (pendingRelease_ || releaseOnAnswer_) {
                    if (cb_.log) cb_.log(3, "floor recv GRANTED in pending Release — ignored (call " + std::to_string(callId_) + ")");
                    return;
                }
                ev.kind = FloorEvent::Kind::Granted;
                grantedCount_++;
                indicator_ = ev.indicator;
                ev.durationSec = m.durationSec();
                grantSelf(ev.durationSec);     // 200 OK 로 이미 승인됐으면 'U: has permission' 에 머물며 시한만 받는다(§6.2.4.5.5)
                break;
            }
            case Op::DENY:
                ev.kind = FloorEvent::Kind::Denied;
                denyCount_++;
                if (ev.indicator) indicator_ = ev.indicator;             // 호 종류(B-bit 등, §8.2.3.15) — 첫 서버 메시지가 Deny 여도 안다
                requestDeadline_ = {};
                state_ = talkers_.empty() ? FloorState::Idle : FloorState::Listening;
                ev.cause = m.cause();
                if (const char* t = rejectCauseText(ev.cause)) ev.causeText = t;
                setMic(false);
                break;
            case Op::IDLE: {
                ev.kind = FloorEvent::Kind::Idle;
                revokePending_ = false; releaseRetxLeft_ = 0;
                talkers_.clear();
                if (state_ != FloorState::Speaking) state_ = FloorState::Idle;
                int perm = m.permission();                               // 일제 통화 Idle 은 Permission 0 일 수 있다(§6.3.4.3.2)
                if (perm >= 0) canRequest_ = perm != (int)Permission::DENIED;
                ev.permission = perm;
                indicator_ = ev.indicator;
                // U: pending Release 에서 받은 Idle — 일제 통화로 개시한 호면 송출 완료 → Releasing(= 호 해제, §6.2.4.6.4 6.)
                if (pendingRelease_ && state_ != FloorState::Speaking) {
                    pendingRelease_ = false;
                    broadcastEnd = broadcastInitiator_ && (ev.indicator & indicator::BROADCAST_GROUP) != 0;
                }
                break;
            }
            case Op::TAKEN: {
                ev.kind = FloorEvent::Kind::Taken;
                takenCount_++;
                int perm = m.permission();
                if (perm >= 0) canRequest_ = perm != (int)Permission::DENIED;
                ev.permission = perm;
                indicator_ = ev.indicator;
                talkers_ = markSelf(m.talkers());
                bool me = false;
                for (auto& t : talkers_) if (t.self) me = true;
                // 동시 발언에서 뒤에 승급한 화자의 Taken 은 먼저 말하던 나에게도 온다 — 강등하지 않는다.
                // U: pending Release 에서 받은 Taken(§6.2.4.6.5) — T100 정지, U: has no permission.
                if (!me) { revokePending_ = false; releaseRetxLeft_ = 0; pendingRelease_ = false; talkDeadline_ = {}; state_ = FloorState::Listening; setMic(false); }
                else state_ = FloorState::Speaking;
                ev.meSpeaking = me;
                break;
            }
            case Op::RELEASE_MULTI: {                                    // 한 명만 이탈(§8.2.14)
                ev.kind = FloorEvent::Kind::TalkerLeft;
                std::string gone = m.userId();
                uint32_t goneSsrc = m.speakerSsrc();
                std::vector<Talker> rest;
                for (auto& t : talkers_) {
                    bool match = (!gone.empty() && sameUser(t.id, gone)) || (gone.empty() && goneSsrc && t.ssrc == goneSsrc);
                    if (!match) rest.push_back(t);
                }
                talkers_ = rest;
                bool me = false;
                for (auto& t : talkers_) if (t.self) me = true;
                state_ = me ? FloorState::Speaking : (rest.empty() ? FloorState::Idle : FloorState::Listening);
                ev.meSpeaking = me;
                break;
            }
            case Op::REVOKE: {                                           // §6.2.4.5.4 — Release 로 응답(재전송)
                if (ev.indicator) indicator_ = ev.indicator;
                int g = ev.indicator & indicator::DUAL_FLOOR;
                releaseRetxPkt_ = floor::release(ssrc_, userId_, g ? g : -1);
                send(releaseRetxPkt_);
                pendingRelease_ = true;
                releaseRetxLeft_ = kReleaseRetxMax;
                releaseRetxAt_ = Clock::now() + std::chrono::milliseconds(kReleaseRetxMs);
                if (revokePending_) return;                              // 서버 T8 재전송 — 이벤트 1회
                revokePending_ = true;
                talkDeadline_ = {};
                ev.kind = FloorEvent::Kind::Revoked;
                ev.cause = m.cause();
                if (const char* t = revokeCauseText(ev.cause)) ev.causeText = t;
                std::vector<Talker> rest;
                for (auto& t : talkers_) if (!t.self) rest.push_back(t);
                talkers_ = rest;
                state_ = rest.empty() ? FloorState::Idle : FloorState::Listening;
                setMic(false);
                break;
            }
            case Op::QUEUE_POS_INFO:
                ev.kind = FloorEvent::Kind::QueuePosition;
                requestDeadline_ = {};
                state_ = FloorState::Queued;
                queuePos_ = m.queuePosition();
                ev.queuePosition = queuePos_;
                break;
            case Op::QUEUED_CANCEL: {
                int purpose = m.queuedPurpose();
                if (purpose == (int)QueuedPurpose::CANCEL_REQUEST) return;   // 단말→서버 방향
                ev.kind = FloorEvent::Kind::QueueCancelled;
                ev.cause = m.queuedResult();
                if (const char* t = queuedResultText(ev.cause)) ev.causeText = t;
                if (state_ == FloorState::Queued) state_ = FloorState::Idle;
                break;
            }
            default:
                ev.kind = FloorEvent::Kind::Other;
                break;
        }
        ev.state = state_;
        ev.talkers = talkers_;
        if (cb_.log) cb_.log(3, std::string("floor recv ") + opName(m.op) + (m.ackRequired ? "(ack-req)" : "") +
                                " → " + toString(state_) + " (call " + std::to_string(callId_) + ")");
    }
    emit(ev);
    if (broadcastEnd && cb_.onBroadcastEnd) cb_.onBroadcastEnd();
}

}  // namespace floor
}  // namespace cimsue
