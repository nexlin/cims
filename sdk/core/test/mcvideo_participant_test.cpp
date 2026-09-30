// libcimsue 단위시험 — MCVideo 전송 제어 participant(TS 24.581 §6.2.4 송출 · §6.2.5 수신) — 루프백 가짜 전송 제어 서버.
// 서버 메시지는 코어 코덱(encode)으로 만든다(CMP 코덱과의 바이트 호환은 mcvideo_xcheck_test.cpp).
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "mcvideo/tc_participant.h"
#include "pj_scope.h"

using namespace cimsue;
using namespace cimsue::mcvideo;

namespace {

/** 전송 제어 서버 자리 — participant 가 보낸 것을 받고 서버 메시지를 돌려준다. */
struct FakeServer {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    int peerPort = 0;
    FakeServer() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s);
        pj_sockaddr_in a;
        pj_sockaddr_in_init(&a, nullptr, 0);
        pj_sock_bind(s, &a, sizeof(a));
        int l = sizeof(a);
        pj_sock_getsockname(s, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeServer() { if (s != PJ_INVALID_SOCKET) pj_sock_close(s); }
    /** 다음 메시지 하나(시한 안에 없으면 false). */
    bool next(Message& m, int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 20};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            uint8_t buf[1500];
            pj_ssize_t n = sizeof(buf);
            if (pj_sock_recv(s, buf, &n, 0) != PJ_SUCCESS || n <= 0) continue;
            if (decode(buf, (size_t)n, m)) return true;
        }
        return false;
    }
    /** (app, op) 가 맞는 메시지가 올 때까지 — 다른 메시지는 건너뛴다. */
    bool expect(AppName app, uint8_t op, Message* out = nullptr, int ms = 2000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        Message m;
        while (std::chrono::steady_clock::now() < end) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (!next(m, left > 0 ? left : 1)) return false;
            if (m.app == app && m.op == op) { if (out) *out = m; return true; }
        }
        return false;
    }
    /** 시한 동안 (app, op) 메시지 개수. */
    int count(AppName app, uint8_t op, int ms) {
        int c = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        Message m;
        while (std::chrono::steady_clock::now() < end) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (next(m, left > 0 ? left : 1) && m.app == app && m.op == op) ++c;
        }
        return c;
    }
    void send(AppName app, uint8_t op, std::vector<Tlv> fields, bool ackRequired = false) {
        Message m;
        m.app = app;
        m.op = op;
        m.ackRequired = ackRequired;
        m.ssrc = 0x0C0C0C0C;
        m.fields = std::move(fields);
        std::string pkt = encode(m);
        ASSERT_FALSE(pkt.empty());
        pj_sockaddr_in to;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)peerPort);
        pj_ssize_t len = (pj_ssize_t)pkt.size();
        pj_sock_sendto(s, pkt.data(), &len, 0, &to, sizeof(to));
    }
};

template <typename F>
bool waitFor(F cond, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

/** 콜백 기록. */
struct Rec {
    std::mutex m;
    std::vector<TransmissionEvent> tx;
    std::vector<ReceptionEvent> rx;
    std::vector<std::pair<bool, std::pair<uint32_t, uint32_t>>> send;
    std::vector<std::pair<std::string, bool>> recv;
    Participant::Callbacks cb() {
        Participant::Callbacks c;
        c.onTransmission = [this](const TransmissionEvent& e) { std::lock_guard<std::mutex> l(m); tx.push_back(e); };
        c.onReception = [this](const ReceptionEvent& e) { std::lock_guard<std::mutex> l(m); rx.push_back(e); };
        c.onSend = [this](bool on, uint32_t a, uint32_t v) { std::lock_guard<std::mutex> l(m); send.push_back({on, {a, v}}); };
        c.onReceive = [this](const VideoTransmitter& t, bool on) { std::lock_guard<std::mutex> l(m); recv.push_back({t.userId, on}); };
        return c;
    }
    bool hasTx(TransmissionEvent::Kind k) { std::lock_guard<std::mutex> l(m); for (auto& e : tx) if (e.kind == k) return true; return false; }
    bool hasRx(ReceptionEvent::Kind k) { std::lock_guard<std::mutex> l(m); for (auto& e : rx) if (e.kind == k) return true; return false; }
    TransmissionEvent lastTx() { std::lock_guard<std::mutex> l(m); return tx.empty() ? TransmissionEvent() : tx.back(); }
    ReceptionEvent lastRx() { std::lock_guard<std::mutex> l(m); return rx.empty() ? ReceptionEvent() : rx.back(); }
    size_t sendCount() { std::lock_guard<std::mutex> l(m); return send.size(); }
    size_t recvCount() { std::lock_guard<std::mutex> l(m); return recv.size(); }
};

const std::string kMe = "tel:+82500000013";
const std::string kPeer = "tel:+82500000014";

/** participant 를 열고 가짜 서버에 잇는다. */
void wire(Participant& p, FakeServer& srv) {
    ASSERT_TRUE(p.open());
    srv.peerPort = p.localPort();
    p.setRemote("127.0.0.1", srv.port, 0x51515151);
}

std::vector<Tlv> transmission(const std::string& id, uint32_t a, uint32_t v) {
    return {strField(Field::TRANSMITTING_USER_ID, id), ssrcField(Field::AUDIO_SSRC, a), ssrcField(Field::VIDEO_SSRC, v)};
}

}  // namespace

// §6.2.4.3.2 → §6.2.4.4.6(Granted, ack) → §6.2.4.5.3 → §6.2.4.6.4 — 요청·허가·[보내기 끝]
TEST(McvParticipant, TransmitGrantedThenEnd) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11111111, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();

    ASSERT_TRUE(p.requestTransmission(5).ok);
    Message m;
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST, &m));
    EXPECT_EQ(m.priority(), 5);
    EXPECT_EQ(m.ssrc, 0x51515151u);                           // 서버가 answer 로 기대한 RTCP 헤더 SSRC(§4.3.3.1)
    EXPECT_EQ(p.info().state, TransmissionState::PendingRequest);

    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_GRANTED,
             {u16Field(Field::DURATION, 60), ssrcField(Field::AUDIO_SSRC, 0xA1), ssrcField(Field::VIDEO_SSRC, 0xB1)}, true);
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_CONTROL_ACK, &m));
    EXPECT_EQ(m.messageName(), "MCV1");
    EXPECT_EQ(m.messageType(), (int)Mcv1::TRANSMISSION_GRANTED);
    EXPECT_EQ(m.source(), (int)Source::PARTICIPANT);
    ASSERT_TRUE(waitFor([&] { return rec.hasTx(TransmissionEvent::Kind::Granted); }, 1000));
    EXPECT_EQ(p.info().state, TransmissionState::Permitted);
    EXPECT_EQ(rec.lastTx().durationSec, 60);
    ASSERT_EQ(rec.sendCount(), 1u);
    EXPECT_TRUE(rec.send[0].first);
    EXPECT_EQ(rec.send[0].second.first, 0xA1u);               // Granted 의 SSRC 로 송출(§6.2.4.4.6 2)
    EXPECT_EQ(rec.send[0].second.second, 0xB1u);

    ASSERT_TRUE(p.releaseTransmission().ok);
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_REQUEST));
    EXPECT_EQ(p.info().state, TransmissionState::PendingEnd);
    ASSERT_EQ(rec.sendCount(), 2u);
    EXPECT_FALSE(rec.send[1].first);
    srv.send(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_RESPONSE, {});
    ASSERT_TRUE(waitFor([&] { return rec.hasTx(TransmissionEvent::Kind::Ended); }, 1000));
    EXPECT_EQ(p.info().state, TransmissionState::NoPermission);
    p.close();
}

// §6.2.4.4.3·§6.2.4.4.4 — T100 만료마다 재전송, C100 번째 만료에 시한 → 'U: has no permission'
TEST(McvParticipant, RequestRetransmitsThenTimesOut) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    TcTimers t;
    t.t100Ms = 80;
    t.c100 = 3;
    Participant p(1, 0x11, kMe, rec.cb(), t);
    wire(p, srv);
    p.onEstablished();
    ASSERT_TRUE(p.requestTransmission().ok);
    EXPECT_EQ(srv.count(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST, 600), 3);   // 처음 1 + 재전송 2
    ASSERT_TRUE(waitFor([&] { return rec.hasTx(TransmissionEvent::Kind::RequestTimeout); }, 500));
    EXPECT_EQ(p.info().state, TransmissionState::NoPermission);
    p.close();
}

// §6.2.4.4.2 — 거절(원인 #1 + phrase)
TEST(McvParticipant, RejectedCarriesCause) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    ASSERT_TRUE(p.requestTransmission().ok);
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_REJECTED, {causeField((int)RejectCause::TRANSMISSION_LIMIT)});
    ASSERT_TRUE(waitFor([&] { return rec.hasTx(TransmissionEvent::Kind::Rejected); }, 1000));
    EXPECT_EQ(rec.lastTx().cause, 1);
    EXPECT_EQ(rec.lastTx().causeText, "Transmission limit reached");
    EXPECT_EQ(p.info().state, TransmissionState::NoPermission);
    EXPECT_EQ(rec.sendCount(), 0u);                           // 송출을 연 적이 없다
    p.close();
}

// §6.2.4.5.5 — 회수: #4(선점)는 End Request → 'U: pending end', #7(대기)은 Queue Position Request → 'U: queued' → Granted
TEST(McvParticipant, RevokedPreemptedEndsAndQueueRequeues) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    auto grant = [&] {
        ASSERT_TRUE(p.requestTransmission().ok);
        ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST));
        srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_GRANTED, {ssrcField(Field::AUDIO_SSRC, 1), ssrcField(Field::VIDEO_SSRC, 2)});
        ASSERT_TRUE(waitFor([&] { return p.info().state == TransmissionState::Permitted; }, 1000));
    };
    grant();
    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_REVOKED, {causeField((int)RevokeCause::PREEMPTED)});
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_REQUEST));
    ASSERT_TRUE(waitFor([&] { return rec.hasTx(TransmissionEvent::Kind::Revoked); }, 1000));
    EXPECT_EQ(p.info().state, TransmissionState::PendingEnd);
    EXPECT_EQ(rec.lastTx().causeText, "Media Burst pre-empted");
    EXPECT_FALSE(rec.send.back().first);                      // 송출을 닫았다
    srv.send(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_RESPONSE, {});
    ASSERT_TRUE(waitFor([&] { return p.info().state == TransmissionState::NoPermission; }, 1000));

    grant();
    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_REVOKED, {causeField((int)RevokeCause::QUEUE_TRANSMISSION)});
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::QUEUE_POSITION_REQUEST));
    ASSERT_TRUE(waitFor([&] { return p.info().state == TransmissionState::Queued; }, 1000));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::QUEUE_POSITION_INFO, {Tlv{(uint8_t)Field::QUEUE_INFO, std::string("\x02\x00", 2)}});
    ASSERT_TRUE(waitFor([&] { return p.info().queuePosition == 2; }, 1000));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_GRANTED, {ssrcField(Field::AUDIO_SSRC, 3), ssrcField(Field::VIDEO_SSRC, 4)});
    ASSERT_TRUE(waitFor([&] { return p.info().state == TransmissionState::Permitted; }, 1000));
    EXPECT_EQ(rec.send.back().second.second, 4u);
    p.close();
}

// §6.2.4.5.7 — 서버가 송출을 끝낸다(Transmission End Request) → End Response, 'U: has no permission'
TEST(McvParticipant, ServerEndRequestAnswered) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    ASSERT_TRUE(p.requestTransmission().ok);
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_GRANTED, {ssrcField(Field::AUDIO_SSRC, 1), ssrcField(Field::VIDEO_SSRC, 2)});
    ASSERT_TRUE(waitFor([&] { return p.info().state == TransmissionState::Permitted; }, 1000));
    srv.send(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_REQUEST, {causeField((int)RevokeCause::TERMINATE_STREAM)}, true);
    Message m;
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_CONTROL_ACK, &m));
    EXPECT_EQ(m.messageName(), "MCV2");
    EXPECT_EQ(m.messageType(), (int)Mcv2::TRANSMISSION_END_REQUEST);
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_END_RESPONSE));
    ASSERT_TRUE(waitFor([&] { return rec.hasTx(TransmissionEvent::Kind::EndRequested); }, 1000));
    EXPECT_EQ(p.info().state, TransmissionState::NoPermission);
    EXPECT_EQ(rec.lastTx().cause, (int)RevokeCause::TERMINATE_STREAM);
    EXPECT_FALSE(rec.send.back().first);
    p.close();
}

// §6.2.5.3.2(manual) → §6.2.5.3.3 [받기] → §6.2.5.4.5 허가 → §6.2.5.5.3 [그만 보기] → §6.2.5.6.4
TEST(McvParticipant, ManualReceptionAcceptThenEnd) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    auto note = transmission(kPeer, 0xA2, 0xB2);
    note.push_back(u16Field(Field::RECEPTION_MODE, (int)ReceptionMode::MANUAL));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION, note);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Notified); }, 1000));
    auto info = p.info();
    ASSERT_EQ(info.transmitters.size(), 1u);
    EXPECT_EQ(info.transmitters[0].userId, kPeer);
    EXPECT_EQ(info.transmitters[0].state, ReceptionState::Notified);
    EXPECT_FALSE(info.transmitters[0].automatic);
    EXPECT_EQ(rec.recvCount(), 0u);                           // manual — [받기] 전에는 받지 않는다

    ASSERT_TRUE(p.acceptReception(kPeer, 2).ok);
    Message m;
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::RECEIVE_MEDIA_REQUEST, &m));
    EXPECT_EQ(m.transmittingUserId(), kPeer);
    EXPECT_EQ(m.videoSsrc(), 0xB2u);
    EXPECT_EQ(m.receptionPriority(), 2);
    auto resp = transmission(kPeer, 0xA2, 0xB2);
    resp.push_back(u16Field(Field::RESULT, (int)ReceiveResult::GRANTED));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::RECEIVE_MEDIA_RESPONSE, resp);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Granted); }, 1000));
    EXPECT_EQ(p.info().transmitters[0].state, ReceptionState::Receiving);
    ASSERT_EQ(rec.recvCount(), 1u);
    EXPECT_TRUE(rec.recv[0].second);

    ASSERT_TRUE(p.endReception(kPeer).ok);
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_REQUEST, &m));
    EXPECT_EQ(m.transmittingUserId(), kPeer);
    EXPECT_EQ(m.audioSsrc(), 0xA2u);
    ASSERT_EQ(rec.recvCount(), 2u);
    EXPECT_FALSE(rec.recv[1].second);
    srv.send(AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_RESPONSE, transmission(kPeer, 0xA2, 0xB2));
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Released); }, 1000));
    EXPECT_EQ(p.info().transmitters[0].state, ReceptionState::Notified);   // 다시 [받기] 할 수 있다
    p.close();
}

// §6.2.5.4.2 — 수신 거절 #7(동시 수신 상한 — C9) 뒤 다시 [받기]
TEST(McvParticipant, ReceptionRejectedThenRetry) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    srv.send(AppName::MCV1, (uint8_t)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION, transmission(kPeer, 1, 2));
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Notified); }, 1000));
    ASSERT_TRUE(p.acceptReception(kPeer).ok);
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::RECEIVE_MEDIA_REQUEST));
    auto resp = transmission(kPeer, 1, 2);
    resp.push_back(u16Field(Field::RESULT, (int)ReceiveResult::REJECTED));
    resp.push_back(causeField((int)ReceiveRejectCause::MAX_STREAMS));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::RECEIVE_MEDIA_RESPONSE, resp);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Rejected); }, 1000));
    EXPECT_EQ(rec.lastRx().cause, 7);
    EXPECT_EQ(rec.lastRx().causeText, "Max no of simultaneous stream to receive is reached");
    EXPECT_EQ(p.info().transmitters[0].state, ReceptionState::Notified);
    EXPECT_TRUE(p.acceptReception(kPeer).ok);
    EXPECT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::RECEIVE_MEDIA_REQUEST));
    p.close();
}

// §6.2.5.3.2 5 — automatic(Reception Mode '0') 는 곧바로 받는다 · §6.2.5.3.4 송출 종료가 수신도 닫는다
TEST(McvParticipant, AutomaticReceptionAndTransmissionEnd) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    auto note = transmission(kPeer, 5, 6);
    note.push_back(u16Field(Field::RECEPTION_MODE, (int)ReceptionMode::AUTOMATIC));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION, note, true);
    Message m;
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_CONTROL_ACK, &m));
    EXPECT_EQ(m.messageType(), (int)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Granted); }, 1000));
    EXPECT_EQ(p.info().transmitters[0].state, ReceptionState::Receiving);
    EXPECT_TRUE(p.info().transmitters[0].automatic);
    srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_END_NOTIFY, transmission(kPeer, 5, 6));
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Ended); }, 1000));
    EXPECT_TRUE(p.info().transmitters.empty());
    ASSERT_EQ(rec.recvCount(), 2u);
    EXPECT_FALSE(rec.recv[1].second);
    p.close();
}

// §6.2.5.5.5 — 서버 Media Reception End Request(ack 요구) → Ack(Message Name MCV2) + End Response
TEST(McvParticipant, ServerReceptionEndRequestAnswered) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    auto note = transmission(kPeer, 5, 6);
    note.push_back(u16Field(Field::RECEPTION_MODE, (int)ReceptionMode::AUTOMATIC));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION, note);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Granted); }, 1000));
    srv.send(AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_REQUEST, transmission(kPeer, 5, 6), true);
    Message m;
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_CONTROL_ACK, &m));
    EXPECT_EQ(m.messageName(), "MCV2");
    EXPECT_EQ(m.messageType(), (int)Mcv2::MEDIA_RECEPTION_END_REQUEST);
    ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::MEDIA_RECEPTION_END_RESPONSE, &m));
    EXPECT_EQ(m.transmittingUserId(), kPeer);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::EndRequested); }, 1000));
    EXPECT_EQ(p.info().transmitters[0].state, ReceptionState::Notified);
    p.close();
}

// §6.2.4.2.2 — 암묵적 송출 요청: 200 OK 전에 온 Granted 는 담아 두었다가 호 성립에 처리 · 받지 않은 answer 는 명시 요청으로 · answer mc_granted
TEST(McvParticipant, ImplicitRequestPaths) {
    cimsue_test::PjScope pj;
    {
        FakeServer srv;
        Rec rec;
        Participant p(1, 0x11, kMe, rec.cb());
        wire(p, srv);
        p.armImplicitRequest();
        EXPECT_EQ(p.info().state, TransmissionState::PendingRequest);
        srv.send(AppName::MCV1, (uint8_t)Mcv1::TRANSMISSION_GRANTED, {ssrcField(Field::AUDIO_SSRC, 7), ssrcField(Field::VIDEO_SSRC, 8)});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        EXPECT_EQ(p.info().state, TransmissionState::PendingRequest);   // 아직 200 OK 전 — 담아 둔다(2)
        p.onEstablished(true, false);
        ASSERT_TRUE(waitFor([&] { return p.info().state == TransmissionState::Permitted; }, 1000));
        EXPECT_EQ(rec.send.back().second.second, 8u);
        p.close();
    }
    {
        FakeServer srv;
        Rec rec;
        Participant p(2, 0x11, kMe, rec.cb());
        wire(p, srv);
        p.armImplicitRequest();
        p.onEstablished(false, false);                        // chat 합류 — 서버가 암묵 요청을 받지 않았다(§14.3.5)
        EXPECT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST, nullptr, 500));
        p.close();
    }
    {
        FakeServer srv;
        Rec rec;
        Participant p(3, 0x11, kMe, rec.cb());
        wire(p, srv);
        p.armImplicitRequest();
        p.onEstablished(true, true, 0xAA, 0xBB);              // answer mc_granted + mc_audio_ssrc·mc_video_ssrc(§14.4)
        EXPECT_EQ(p.info().state, TransmissionState::Permitted);
        ASSERT_TRUE(waitFor([&] { return rec.sendCount() == 1; }, 500));
        EXPECT_EQ(rec.send[0].second.first, 0xAAu);
        EXPECT_EQ(rec.send[0].second.second, 0xBBu);
        EXPECT_EQ(srv.count(AppName::MCV0, (uint8_t)Mcv0::TRANSMISSION_REQUEST, 200), 0);
        p.close();
    }
}

// 상태에 맞지 않는 조작은 실패 — 보내지 않는다
TEST(McvParticipant, StateGuards) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    EXPECT_FALSE(p.requestTransmission().ok);                 // 호 성립 전
    p.onEstablished();
    EXPECT_FALSE(p.releaseTransmission().ok);                 // 'U: has no permission'
    EXPECT_FALSE(p.acceptReception(kPeer).ok);                // 알림 받은 송출 없음
    EXPECT_FALSE(p.endReception(kPeer).ok);
    ASSERT_TRUE(p.requestTransmission().ok);
    EXPECT_FALSE(p.requestTransmission().ok);                 // 이미 요청 중
    p.close();
    EXPECT_FALSE(p.releaseTransmission().ok);                 // 'Call releasing'
}

// §6.2.5.4.2 — 서버가 Ack 을 못 받아 Receive Media Response(Granted)를 다시 보내면(T6·C6 — mcvideo.md §5.3.1) 이미 받는 중이어도 Ack 한다
TEST(McvParticipant, RetransmittedReceiveResponseAcked) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    p.onEstablished();
    auto note = transmission(kPeer, 0xA3, 0xB3);
    note.push_back(u16Field(Field::RECEPTION_MODE, (int)ReceptionMode::MANUAL));
    srv.send(AppName::MCV1, (uint8_t)Mcv1::MEDIA_TRANSMISSION_NOTIFICATION, note);
    ASSERT_TRUE(waitFor([&] { return rec.hasRx(ReceptionEvent::Kind::Notified); }, 1000));
    ASSERT_TRUE(p.acceptReception(kPeer).ok);
    ASSERT_TRUE(srv.expect(AppName::MCV0, (uint8_t)Mcv0::RECEIVE_MEDIA_REQUEST));
    auto resp = transmission(kPeer, 0xA3, 0xB3);
    resp.push_back(u16Field(Field::RESULT, (int)ReceiveResult::GRANTED));
    for (int i = 0; i < 2; ++i) {                              // 첫 전송 + 재송신
        srv.send(AppName::MCV1, (uint8_t)Mcv1::RECEIVE_MEDIA_RESPONSE, resp, true);
        Message m;
        ASSERT_TRUE(srv.expect(AppName::MCV2, (uint8_t)Mcv2::TRANSMISSION_CONTROL_ACK, &m)) << "ack #" << i;
        EXPECT_EQ(m.messageName(), "MCV1");
        EXPECT_EQ(m.messageType(), (int)Mcv1::RECEIVE_MEDIA_RESPONSE);
    }
    EXPECT_EQ(p.info().transmitters[0].state, ReceptionState::Receiving);
    EXPECT_EQ(rec.recvCount(), 1u);                           // 재송신은 수신을 다시 결선하지 않는다
    p.close();
}

// 제어 채널 NAT 유지(ue_nat_traversal.md §7.1) — 성립 즉시 빈 RTCP RR(PT 201, 헤더 SSRC = 서버가 기대하는 값), 1 s 간격으로 이어진다
TEST(McvParticipant, KeepaliveReceiverReports) {
    cimsue_test::PjScope pj;
    FakeServer srv;
    Rec rec;
    Participant p(1, 0x11, kMe, rec.cb());
    wire(p, srv);
    auto rr = [&](int ms) -> std::vector<uint8_t> {            // 다음 RR 한 개(8 바이트) — 시한 안에 없으면 빈 값
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(srv.s, &fds);
            pj_time_val tv = {0, 20};
            if (pj_sock_select((int)srv.s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            uint8_t buf[64];
            pj_ssize_t n = sizeof(buf);
            if (pj_sock_recv(srv.s, buf, &n, 0) == PJ_SUCCESS && n == 8 && buf[1] == 201) return std::vector<uint8_t>(buf, buf + 8);
        }
        return {};
    };
    EXPECT_TRUE(rr(300).empty());                             // 호 성립 전에는 보내지 않는다
    p.onEstablished();
    auto t0 = std::chrono::steady_clock::now();
    auto a = rr(500);
    ASSERT_EQ(a.size(), 8u);
    EXPECT_EQ(a[0], 0x80);                                    // V=2 · P=0 · RC=0
    EXPECT_EQ(a[3], 1);                                       // length 1(32비트 워드 − 1)
    EXPECT_EQ(((uint32_t)a[4] << 24) | ((uint32_t)a[5] << 16) | ((uint32_t)a[6] << 8) | a[7], 0x51515151u);
    auto b = rr(1500);
    ASSERT_EQ(b.size(), 8u);
    auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(gap, 800);
    EXPECT_LE(gap, 1500);
    p.close();
}
