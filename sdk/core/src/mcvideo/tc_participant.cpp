#include "tc_participant.h"

#include <pjlib.h>

namespace cimsue {
namespace mcvideo {

namespace {
bool sameUser(const std::string& a, const std::string& b) {
    auto bare = [](const std::string& s) {
        size_t c = s.find(':');
        std::string t = c == std::string::npos ? s : s.substr(c + 1);
        size_t at = t.find('@');
        return at == std::string::npos ? t : t.substr(0, at);
    };
    return !a.empty() && !b.empty() && bare(a) == bare(b);
}
bool is(const Message& m, AppName app, uint8_t op) { return m.app == app && m.op == op; }
}  // namespace

Participant::Participant(int callId, uint32_t localSsrc, const std::string& userId, Callbacks cb, TcTimers timers)
    : callId_(callId), localSsrc_(localSsrc), userId_(userId), cb_(std::move(cb)), timers_(timers) {}

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
    if (cb_.log) cb_.log(3, "mcvideo tc socket bound :" + std::to_string(localPort_) + " (call " + std::to_string(callId_) + ")");
    return true;
}

void Participant::close() {
    {
        std::lock_guard<std::mutex> lk(m_);
        releasing_ = true;                                   // 'Call releasing' — 제어 메시지를 더 보내지 않는다(§6.2.4.7.2 1)
        deadline_ = {};
        for (auto& r : receptions_) r.deadline = {};
    }
    if (!running_.exchange(false)) { if (rx_.joinable()) rx_.join(); return; }
    if (rx_.joinable()) rx_.join();
    if (sock_ >= 0) { pj_sock_close((pj_sock_t)sock_); sock_ = -1; }
}

void Participant::setRemote(const std::string& ip, int port, uint32_t remoteSsrc) {
    std::lock_guard<std::mutex> lk(m_);
    remoteIp_ = ip;
    remotePort_ = port;
    remoteSsrc_ = remoteSsrc;
    if (cb_.log) cb_.log(3, "mcvideo tc remote " + ip + ":" + std::to_string(port) + " (call " + std::to_string(callId_) + ")");
}

void Participant::setCallIndicator(int bits) {
    std::lock_guard<std::mutex> lk(m_);
    indicator_ = bits;
}

void Participant::send(const std::string& pkt) {
    if (releasing_ || pkt.empty()) return;
    if (remotePort_ <= 0 || sock_ < 0) { if (cb_.log) cb_.log(2, "mcvideo tc send before remote learned"); return; }
    pj_sockaddr_in to;
    pj_str_t ip = pj_str(const_cast<char*>(remoteIp_.c_str()));
    if (pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)remotePort_) != PJ_SUCCESS) return;
    pj_ssize_t n = (pj_ssize_t)pkt.size();
    if (pj_sock_sendto((pj_sock_t)sock_, pkt.data(), &n, 0, &to, sizeof(to)) == PJ_SUCCESS) ++sent_;
}

void Participant::ack(const Message& m) {
    if (m.ackRequired) send(ackOf(hdrSsrc(), m.app, m.op));
}

void Participant::sendKeepalive() {
    // 빈 RTCP RR(RFC 3550 §6.4.2 — V=2 · RC=0 · PT=201 · length 1) — 헤더 SSRC 는 전송 제어 메시지와 같다(서버가 기대하는 값)
    const uint32_t ssrc = hdrSsrc();
    const char rr[8] = {(char)0x80, (char)201, 0, 1, (char)(ssrc >> 24), (char)(ssrc >> 16), (char)(ssrc >> 8), (char)ssrc};
    send(std::string(rr, sizeof(rr)));
}

void Participant::flush(Out& out) {
    for (auto& f : out) f();
    out.clear();
}

// ── 이벤트 ──

void Participant::emitTx(Out& out, TransmissionEvent::Kind k, const Message* m, int cause, const char* causeTable) {
    TransmissionEvent ev;
    ev.kind = k;
    ev.callId = callId_;
    ev.state = state_;
    ev.cause = cause;
    ev.queuePosition = queuePosition_;
    ev.audioSsrc = txAudioSsrc_;
    ev.videoSsrc = txVideoSsrc_;
    if (m) {
        ev.causeText = m->causePhrase();
        ev.durationSec = m->durationSec();
        ev.priority = m->priority();
        ev.indicator = m->indicator() < 0 ? 0 : m->indicator();
        ev.rawType = m->op;
        if (k == TransmissionEvent::Kind::ReceiverJoined) ev.receiverId = m->userId();
    }
    if (ev.causeText.empty() && causeTable) ev.causeText = causeTable;
    auto f = cb_.onTransmission;
    if (f) out.push_back([f, ev] { f(ev); });
}

void Participant::emitRx(Out& out, ReceptionEvent::Kind k, const VideoTransmitter& t, const Message* m, int cause) {
    ReceptionEvent ev;
    ev.kind = k;
    ev.callId = callId_;
    ev.transmitter = t;
    ev.cause = cause;
    if (m) {
        ev.causeText = m->causePhrase();
        ev.rawType = m->op;
    }
    if (ev.causeText.empty() && cause >= 0) {
        const char* s = receiveRejectCauseText(cause);
        if (s) ev.causeText = s;
    }
    auto f = cb_.onReception;
    if (f) out.push_back([f, ev] { f(ev); });
}

void Participant::setSending(Out& out, bool on) {
    if (sending_ == on) return;
    sending_ = on;
    auto f = cb_.onSend;
    uint32_t a = txAudioSsrc_, v = txVideoSsrc_;
    if (f) out.push_back([f, on, a, v] { f(on, a, v); });
}

// 수신 결선 알림 — on 이면 'has permission to receive' 에 넣고, off 면 호출자가 새 상태를 먼저 정한 뒤 부른다.
void Participant::setReceiving(Out& out, Reception& r, bool on) {
    if (on) r.t.state = ReceptionState::Receiving;
    auto f = cb_.onReceive;
    VideoTransmitter t = r.t;
    if (f) out.push_back([f, t, on] { f(t, on); });
}

// ── 송신 + 타이머 무장 ──

void Participant::sendTransmissionRequest() {
    int ind = indicator_ ? indicator_ : -1;                  // 일반 호는 지시자를 싣지 않는다(§6.2.4.3.2 2b)
    send(transmissionRequest(hdrSsrc(), priority_, ind));
    deadline_ = Clock::now() + std::chrono::milliseconds(timers_.t100Ms);
}

void Participant::sendEndRequest() {
    send(transmissionEndRequest(hdrSsrc()));
    deadline_ = Clock::now() + std::chrono::milliseconds(timers_.t101Ms);
}

void Participant::sendQueuePositionRequest() {
    send(queuePositionRequest(hdrSsrc()));
    deadline_ = Clock::now() + std::chrono::milliseconds(timers_.t102Ms);
}

void Participant::sendReceiveRequest(Reception& r) {
    int ind = indicator_ ? indicator_ : -1;
    send(receiveMediaRequest(hdrSsrc(), r.t.userId, r.t.audioSsrc, r.t.videoSsrc, r.priority, ind));
    r.deadline = Clock::now() + std::chrono::milliseconds(timers_.t103Ms);
}

void Participant::sendReceptionEnd(Reception& r) {
    send(mediaReceptionEndRequest(hdrSsrc(), r.t.userId, r.t.audioSsrc, r.t.videoSsrc));
    r.deadline = Clock::now() + std::chrono::milliseconds(timers_.t104Ms);
}

Participant::Reception* Participant::findReception(const std::string& id, uint32_t videoSsrc) {
    for (auto& r : receptions_)
        if ((!id.empty() && sameUser(r.t.userId, id)) || (videoSsrc && r.t.videoSsrc == videoSsrc)) return &r;
    return nullptr;
}

// ── 호 성립 ──

void Participant::armImplicitRequest() {
    std::lock_guard<std::mutex> lk(m_);
    if (established_) return;
    state_ = TransmissionState::PendingRequest;              // T100 은 호 성립에서(목적지를 answer 로 안다)
}

void Participant::onEstablished(bool implicitAccepted, bool granted, uint32_t audioSsrc, uint32_t videoSsrc) {
    Out out;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (established_) return;
        established_ = true;
        if (audioSsrc) txAudioSsrc_ = audioSsrc;             // §14.4 — answer 의 값을 쓴다
        if (videoSsrc) txVideoSsrc_ = videoSsrc;
        if (state_ == TransmissionState::PendingRequest) {   // 암묵적 송출 요청(§6.2.4.2.2 4)
            if (granted) {
                state_ = TransmissionState::Permitted;
                setSending(out, true);
                emitTx(out, TransmissionEvent::Kind::Granted);
            } else {
                retries_ = 1;
                if (implicitAccepted) deadline_ = Clock::now() + std::chrono::milliseconds(timers_.t100Ms);   // Granted 대기
                else sendTransmissionRequest();              // 받지 않았다(§14.3.5) — 명시 요청으로 잇는다
            }
        }
        std::vector<Message> early;
        early.swap(early_);
        for (auto& m : early) handle(m, out);                // 호 성립 전에 받아 둔 메시지(§6.2.4.2.2 2·4c)
        kaSent_ = 0;                                         // 제어 채널 NAT 유지 — 성립 즉시 1회(tick)
        kaNext_ = Clock::now();
    }
    flush(out);
}

// ── 사용자 조작 ──

Result Participant::requestTransmission(int priority) {
    Out out;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!established_ || releasing_) return Result::fail(-2, "mcvideo call not established");
        if (state_ != TransmissionState::NoPermission) return Result::fail(-2, std::string("transmission ") + toString(state_));
        priority_ = priority;
        retries_ = 1;
        sendTransmissionRequest();
        state_ = TransmissionState::PendingRequest;
    }
    flush(out);
    return Result::success();
}

Result Participant::releaseTransmission() {
    Out out;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!established_ || releasing_) return Result::fail(-2, "mcvideo call not established");
        if (state_ != TransmissionState::PendingRequest && state_ != TransmissionState::Permitted &&
            state_ != TransmissionState::Queued)
            return Result::fail(-2, std::string("transmission ") + toString(state_));
        setSending(out, false);
        retries_ = 1;
        sendEndRequest();
        state_ = TransmissionState::PendingEnd;
    }
    flush(out);
    return Result::success();
}

Result Participant::acceptReception(const std::string& transmitterId, int priority) {
    std::lock_guard<std::mutex> lk(m_);
    if (!established_ || releasing_) return Result::fail(-2, "mcvideo call not established");
    Reception* r = findReception(transmitterId, 0);
    if (!r) return Result::fail(-2, "no such transmission: " + transmitterId);
    if (r->t.state != ReceptionState::Notified) return Result::fail(-2, std::string("reception ") + toString(r->t.state));
    r->priority = priority;
    r->retries = 1;
    sendReceiveRequest(*r);
    r->t.state = ReceptionState::PendingRequest;
    return Result::success();
}

Result Participant::endReception(const std::string& transmitterId) {
    Out out;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!established_ || releasing_) return Result::fail(-2, "mcvideo call not established");
        Reception* r = findReception(transmitterId, 0);
        if (!r) return Result::fail(-2, "no such transmission: " + transmitterId);
        if (r->t.state != ReceptionState::PendingRequest && r->t.state != ReceptionState::Receiving)
            return Result::fail(-2, std::string("reception ") + toString(r->t.state));
        bool wasReceiving = r->t.state == ReceptionState::Receiving;
        r->t.state = ReceptionState::PendingRelease;
        if (wasReceiving) setReceiving(out, *r, false);
        r->retries = 1;
        sendReceptionEnd(*r);
    }
    flush(out);
    return Result::success();
}

TransmissionInfo Participant::info() const {
    std::lock_guard<std::mutex> lk(m_);
    TransmissionInfo ti;
    ti.state = state_;
    for (auto& r : receptions_) ti.transmitters.push_back(r.t);
    ti.queuePosition = queuePosition_;
    ti.localPort = localPort_;
    ti.remoteIp = remoteIp_;
    ti.remotePort = remotePort_;
    return ti;
}

// ── 수신 ──

void Participant::rxLoop() {
    pj_thread_desc desc;
    pj_thread_t* th = nullptr;
    pj_bzero(desc, sizeof(desc));
    pj_thread_register("mcvideo-tc-rx", desc, &th);
    unsigned char buf[1500];
    while (running_) {
        pj_fd_set_t fds;
        PJ_FD_ZERO(&fds);
        PJ_FD_SET((pj_sock_t)sock_, &fds);
        pj_time_val tv = {0, 100};
        int n = pj_sock_select((int)sock_ + 1, &fds, nullptr, nullptr, &tv);
        if (n > 0 && PJ_FD_ISSET((pj_sock_t)sock_, &fds)) {
            pj_ssize_t len = sizeof(buf);
            pj_sockaddr_in from;
            int fl = sizeof(from);
            if (pj_sock_recvfrom((pj_sock_t)sock_, buf, &len, 0, &from, &fl) == PJ_SUCCESS && len > 0) {
                // 한 IP 패킷에 메시지가 여럿일 수 있다(§9.1.1) — 헤더 length 로 끊어 읽는다
                size_t p = 0;
                Out out;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    while (p + 12 <= (size_t)len) {
                        size_t mlen = ((((size_t)buf[p + 2]) << 8) | buf[p + 3]) * 4 + 4;
                        Message m;
                        if (decode(buf + p, (size_t)len - p, m)) handle(m, out);
                        if (mlen < 12) break;
                        p += mlen;
                    }
                }
                flush(out);
            }
        }
        tick();
    }
}

void Participant::handle(const Message& m, Out& out) {
    if (releasing_) return;
    if (!established_) { early_.push_back(m); return; }      // §6.2.4.2.2 2 — 200 OK 전 메시지는 담아 둔다
    if (m.app == AppName::MCV0) return;                      // 서버는 MCV0 을 보내지 않는다
    handleTransmission(m, out);
    handleReception(m, out);
}

void Participant::handleTransmission(const Message& m, Out& out) {
    using K = TransmissionEvent::Kind;
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_GRANTED)) {
        if (state_ == TransmissionState::PendingRequest || state_ == TransmissionState::Queued) {   // §6.2.4.4.6·§6.2.4.9.9
            ack(m);
            if (m.audioSsrc()) txAudioSsrc_ = m.audioSsrc();  // 송출 RTP 에 쓸 값(2)
            if (m.videoSsrc()) txVideoSsrc_ = m.videoSsrc();
            deadline_ = {};
            queuePosition_ = -1;
            state_ = TransmissionState::Permitted;
            setSending(out, true);
            emitTx(out, K::Granted, &m);
        } else if (state_ == TransmissionState::Permitted) {
            // answer mc_granted 뒤 서버가 따로 보낸 Granted — 확인만 하고, 값이 다르면 송출 SSRC 를 새 값으로
            ack(m);
            if ((m.audioSsrc() && m.audioSsrc() != txAudioSsrc_) || (m.videoSsrc() && m.videoSsrc() != txVideoSsrc_)) {
                if (m.audioSsrc()) txAudioSsrc_ = m.audioSsrc();
                if (m.videoSsrc()) txVideoSsrc_ = m.videoSsrc();
                auto f = cb_.onSend;
                uint32_t a = txAudioSsrc_, v = txVideoSsrc_;
                if (f) out.push_back([f, a, v] { f(true, a, v); });
            }
        }
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_REJECTED)) {
        if (state_ != TransmissionState::PendingRequest) return;
        ack(m);                                                                                   // §6.2.4.4.2
        deadline_ = {};
        state_ = TransmissionState::NoPermission;
        emitTx(out, K::Rejected, &m, m.cause(), rejectCauseText(m.cause()));
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::QUEUE_POSITION_INFO)) {
        if (state_ != TransmissionState::PendingRequest && state_ != TransmissionState::Queued) return;
        ack(m);                                                                                   // §6.2.4.4.5·§6.2.4.9.2
        deadline_ = {};                                      // T100(normal stop) 또는 T102
        queuePosition_ = m.queuePosition();
        state_ = TransmissionState::Queued;
        emitTx(out, K::QueuePosition, &m);
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_REVOKED)) {
        if (state_ != TransmissionState::Permitted) return;
        ack(m);                                                                                   // §6.2.4.5.5
        setSending(out, false);
        int cause = m.cause();
        retries_ = 1;
        if (cause == (int)RevokeCause::QUEUE_TRANSMISSION) {
            sendQueuePositionRequest();
            state_ = TransmissionState::Queued;
        } else {
            sendEndRequest();
            state_ = TransmissionState::PendingEnd;
        }
        emitTx(out, K::Revoked, &m, cause, revokeCauseText(cause));
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::MEDIA_RECEPTION_NOTIFICATION)) {
        if (state_ != TransmissionState::Permitted) return;
        ack(m);                                                                                   // §6.2.4.5.6
        emitTx(out, K::ReceiverJoined, &m);
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_CANCEL_REQUEST_NOTIFY)) {
        if (state_ != TransmissionState::Queued) return;
        ack(m);                                                                                   // §6.2.4.9.6
        deadline_ = {};
        queuePosition_ = -1;
        state_ = TransmissionState::NoPermission;
        emitTx(out, K::QueueCancelled, &m);
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_IDLE)) {
        ack(m);
        emitTx(out, K::Idle, &m);
        return;
    }
    if (is(m, AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_REQUEST)) {                          // 서버가 송출을 끝낸다
        if (state_ != TransmissionState::Permitted && state_ != TransmissionState::PendingEnd) return;
        ack(m);                                                                                   // §6.2.4.5.7
        setSending(out, false);
        send(transmissionEndResponse(hdrSsrc()));
        deadline_ = {};
        state_ = TransmissionState::NoPermission;
        emitTx(out, K::EndRequested, &m, m.cause(), revokeCauseText(m.cause()));
        return;
    }
    if (is(m, AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_RESPONSE)) {
        if (state_ != TransmissionState::PendingEnd) return;
        ack(m);                                                                                   // §6.2.4.6.4
        deadline_ = {};
        state_ = TransmissionState::NoPermission;
        emitTx(out, K::Ended, &m);
        return;
    }
}

void Participant::handleReception(const Message& m, Out& out) {
    using K = ReceptionEvent::Kind;
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION)) {                   // §6.2.5.3.2
        ack(m);
        std::string id = m.transmittingUserId();
        if (sameUser(id, userId_)) return;                   // 내 송출 알림은 받지 않는다
        Reception* r = findReception(id, m.videoSsrc());
        if (!r) {
            receptions_.push_back(Reception{});
            r = &receptions_.back();
            r->t.state = ReceptionState::Notified;
        }
        r->t.userId = id;
        r->t.audioSsrc = m.audioSsrc();
        r->t.videoSsrc = m.videoSsrc();
        r->t.functionalAlias = m.functionalAlias();
        r->t.automatic = m.receptionMode() == (int)ReceptionMode::AUTOMATIC;
        emitRx(out, K::Notified, r->t, &m);
        if (r->t.automatic && r->t.state == ReceptionState::Notified) {                          // 5 — 곧바로 'has permission'
            setReceiving(out, *r, true);
            emitRx(out, K::Granted, r->t, &m);
        }
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::RECEIVE_MEDIA_RESPONSE)) {
        ack(m);                                                                                   // §6.2.5.4.2·§6.2.5.4.5
        Reception* r = findReception(m.transmittingUserId(), m.videoSsrc());
        if (!r || r->t.state != ReceptionState::PendingRequest) return;          // 재송신(서버 T6·C6 — Ack 을 못 받았다)은 Ack 만
        r->deadline = {};
        if (m.result() == (int)ReceiveResult::GRANTED) {
            setReceiving(out, *r, true);
            emitRx(out, K::Granted, r->t, &m);
        } else {
            r->t.state = ReceptionState::Notified;           // 'U: terminated' — 다시 [받기] 할 수 있다
            emitRx(out, K::Rejected, r->t, &m, m.cause());
        }
        return;
    }
    if (is(m, AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_END_NOTIFY)) {                           // §6.2.5.3.4
        ack(m);
        for (size_t i = 0; i < receptions_.size(); ++i) {
            Reception& r = receptions_[i];
            if (!(sameUser(r.t.userId, m.transmittingUserId()) || (m.videoSsrc() && r.t.videoSsrc == m.videoSsrc())))
                continue;
            bool wasReceiving = r.t.state == ReceptionState::Receiving || r.t.state == ReceptionState::PendingRelease;
            r.t.state = ReceptionState::Ended;
            if (wasReceiving) setReceiving(out, r, false);
            emitRx(out, K::Ended, r.t, &m);
            receptions_.erase(receptions_.begin() + (long)i);
            break;
        }
        return;
    }
    if (is(m, AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_REQUEST)) {                       // §6.2.5.5.5
        Reception* r = findReception(m.transmittingUserId(), m.videoSsrc());
        if (!r || (r->t.state != ReceptionState::Receiving && r->t.state != ReceptionState::PendingRequest)) return;
        ack(m);
        bool wasReceiving = r->t.state == ReceptionState::Receiving;
        r->t.state = ReceptionState::Notified;
        if (wasReceiving) setReceiving(out, *r, false);
        send(mediaReceptionEndResponse(hdrSsrc(), r->t.userId, r->t.audioSsrc, r->t.videoSsrc));
        r->deadline = {};
        emitRx(out, K::EndRequested, r->t, &m);
        return;
    }
    if (is(m, AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_RESPONSE)) {                      // §6.2.5.6.4
        Reception* r = findReception(m.transmittingUserId(), m.videoSsrc());
        if (!r || r->t.state != ReceptionState::PendingRelease) return;
        ack(m);
        r->deadline = {};
        r->t.state = ReceptionState::Notified;
        emitRx(out, K::Released, r->t, &m);
        return;
    }
}

// ── 타이머 (§11.1.1 · §11.2.1) ──

void Participant::tick() {
    Out out;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (releasing_) return;
        auto now = Clock::now();
        if (established_ && kaNext_ != Clock::time_point{} && now >= kaNext_ && remotePort_ > 0) {
            sendKeepalive();                                                                      // 성립 1 + 1 s 간격 2 + 15 s 주기
            ++kaSent_;
            kaNext_ = now + std::chrono::milliseconds(kaSent_ < kKeepaliveBurst ? kKeepaliveBurstGapMs : kKeepaliveIntervalMs);
        }
        if (deadline_ != Clock::time_point{} && now >= deadline_) {
            deadline_ = {};
            switch (state_) {
                case TransmissionState::PendingRequest:                                           // T100
                    if (retries_ < timers_.c100) { ++retries_; sendTransmissionRequest(); }        // §6.2.4.4.3
                    else {                                                                        // §6.2.4.4.4
                        state_ = TransmissionState::NoPermission;
                        emitTx(out, TransmissionEvent::Kind::RequestTimeout);
                    }
                    break;
                case TransmissionState::PendingEnd:                                               // T101
                    if (retries_ < timers_.c101) { ++retries_; sendEndRequest(); }                 // §6.2.4.6.2
                    else {                                                                        // §6.2.4.6.3
                        state_ = TransmissionState::NoPermission;
                        emitTx(out, TransmissionEvent::Kind::RequestTimeout);
                    }
                    break;
                case TransmissionState::Queued:                                                   // T102
                    if (retries_ < timers_.c102) { ++retries_; sendQueuePositionRequest(); }       // §6.2.4.9.7
                    else {                                                                        // §6.2.4.9.8 — 대기 시한 → End Request
                        retries_ = 1;
                        sendEndRequest();
                        state_ = TransmissionState::PendingEnd;
                        emitTx(out, TransmissionEvent::Kind::RequestTimeout);
                    }
                    break;
                default:
                    break;
            }
        }
        for (auto& r : receptions_) {
            if (r.deadline == Clock::time_point{} || now < r.deadline) continue;
            r.deadline = {};
            if (r.t.state == ReceptionState::PendingRequest) {                                    // T103
                if (r.retries < timers_.c103) { ++r.retries; sendReceiveRequest(r); }             // §6.2.5.4.3
                else {                                                                            // §6.2.5.4.4
                    r.t.state = ReceptionState::Notified;
                    emitRx(out, ReceptionEvent::Kind::RequestTimeout, r.t);
                }
            } else if (r.t.state == ReceptionState::PendingRelease) {                             // T104
                if (r.retries < timers_.c104) { ++r.retries; sendReceptionEnd(r); }               // §6.2.5.6.2
                else {                                                                            // §6.2.5.6.3
                    r.t.state = ReceptionState::Notified;
                    emitRx(out, ReceptionEvent::Kind::RequestTimeout, r.t);
                }
            }
        }
    }
    flush(out);
}

}  // namespace mcvideo
}  // namespace cimsue
