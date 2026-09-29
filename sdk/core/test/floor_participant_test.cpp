// libcimsue 단위시험 — floor participant 상태 기계(TS 24.380 §6.2.4) 중 'U: pending Release' 절차.
// 서버 메시지는 코어 코덱(encode)으로 만든다 — CMP 코덱이 필요 없어 Windows·Linux 공통(CMP 교차 검증은 floor_xcheck_test.cpp).
#include <gtest/gtest.h>

#include <pjlib.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../src/floor/floor_codec.h"
#include "../src/floor/floor_participant.h"

using namespace cimsue;
using namespace cimsue::floor;

namespace {

void pjReady() {
    static bool inited = (pj_init() == PJ_SUCCESS);
    (void)inited;
    if (!pj_thread_is_registered()) {
        static thread_local pj_thread_desc desc;
        pj_thread_t* th = nullptr;
        pj_bzero(desc, sizeof(desc));
        pj_thread_register("floor-test", desc, &th);
    }
}

/** floor control server 자리 — participant 가 보낸 것을 받고 서버 메시지를 돌려준다. */
struct FakeServer {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
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
    /** op 가 맞는 메시지가 올 때까지(Ack keepalive 는 건너뜀) — 시한 안에 없으면 false. */
    bool expect(Op op, int ms = 2000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            uint8_t buf[1500];
            pj_ssize_t n = sizeof(buf);
            if (pj_sock_recv(s, buf, &n, 0) != PJ_SUCCESS || n <= 0) continue;
            Message m;
            if (decode(buf, (size_t)n, m) && m.op == (uint8_t)op) return true;
        }
        return false;
    }
    void send(int toPort, Op op, std::vector<Tlv> fields) {
        Message m;
        m.op = (uint8_t)op;
        m.ssrc = 0x01;
        m.fields = std::move(fields);
        std::string pkt = encode(m);
        pj_sockaddr_in to;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)toPort);
        pj_ssize_t len = (pj_ssize_t)pkt.size();
        pj_sock_sendto(s, pkt.data(), &len, 0, &to, sizeof(to));
    }
};

template <typename F>
bool waitFor(F cond, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

}  // namespace

// §6.2.4.6.8 — PTT 를 짧게 눌러 Granted 가 Release 뒤에 오면(요청→놓음→늦은 승인) 무시하고 'U: pending Release' 에 남는다.
//   예전에는 늦은 Granted 가 마이크를 열고 Speaking 으로 가 뒤이은 Idle 을 무시했다 — 서버는 유휴인데 단말만 송출하고,
//   일제 통화 개시자는 호 해제(§6.2.4.6.4)를 놓쳤다.
TEST(FloorParticipant, GrantedAfterReleaseIsIgnoredAndBroadcastStillEnds) {
    pjReady();
    FakeServer srv;
    std::atomic<int> micOn{0}, granted{0}, ends{0};
    Participant::Callbacks cb;
    cb.onEvent = [&](const FloorEvent& ev) { if (ev.kind == FloorEvent::Kind::Granted) granted++; };
    cb.onMic = [&](bool on) { if (on) micOn++; };
    cb.onBroadcastEnd = [&] { ends++; };
    Participant p(1, 0x1234u, "tel:+82500000001", cb);
    ASSERT_TRUE(p.open(0));
    p.setBroadcastInitiator(true);
    p.setRemote("127.0.0.1", srv.port);

    p.request();
    ASSERT_TRUE(srv.expect(Op::REQUEST));
    p.release();                                                     // 승인 전에 놓음
    ASSERT_TRUE(srv.expect(Op::RELEASE));
    srv.send(p.localPort(), Op::GRANTED, {u16Field((uint8_t)Field::DURATION, 30), u16Field((uint8_t)Field::FLOOR_INDICATOR, 0xC000)});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(micOn.load(), 0);                                      // 마이크를 열지 않는다
    EXPECT_EQ(granted.load(), 0);                                    // 앱에 발언 중을 알리지 않는다
    EXPECT_NE(p.info().state, FloorState::Speaking);

    // 서버가 Release 를 받아 보내는 Idle(B-bit) — 일제 통화 개시 단말은 호를 해제한다
    srv.send(p.localPort(), Op::IDLE, {u16Field((uint8_t)Field::MSG_SEQ, 1), u16Field((uint8_t)Field::FLOOR_INDICATOR, 0xC000)});
    EXPECT_TRUE(waitFor([&] { return ends.load() == 1; }, 2000));
    p.close();
}

// §6.2.4.6.2 — Floor Release 는 T100 으로 재전송하고 Idle 이 오면 멈춘다(유실되면 서버가 발언권을 계속 쥔다).
TEST(FloorParticipant, ReleaseRetransmittedUntilIdle) {
    pjReady();
    FakeServer srv;
    Participant::Callbacks cb;
    Participant p(2, 0x5678u, "tel:+82500000002", cb);
    ASSERT_TRUE(p.open(0));
    p.setRemote("127.0.0.1", srv.port);

    p.request();
    ASSERT_TRUE(srv.expect(Op::REQUEST));
    srv.send(p.localPort(), Op::GRANTED, {u16Field((uint8_t)Field::DURATION, 30), u16Field((uint8_t)Field::FLOOR_INDICATOR, 0x8000)});
    ASSERT_TRUE(waitFor([&] { return p.info().state == FloorState::Speaking; }, 2000));
    p.release();
    ASSERT_TRUE(srv.expect(Op::RELEASE));
    EXPECT_TRUE(srv.expect(Op::RELEASE, 2000));                      // 응답이 없으면 다시 보낸다
    srv.send(p.localPort(), Op::IDLE, {u16Field((uint8_t)Field::MSG_SEQ, 1), u16Field((uint8_t)Field::FLOOR_INDICATOR, 0x8000)});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(srv.expect(Op::RELEASE, 1500));                     // Idle 뒤에는 멈춘다
    p.close();
}

// ── 암묵적 발언 요청(TS 24.380 §14.2.5 mc_implicit_request, §6.2.4.2.2 'U: pending Request') ──
//   engine 은 개시 INVITE 전에 armImplicitRequest, 200 OK answer 에서 setRemote → onInitialAnswer(answer fmtp) 순으로 부른다.

namespace {
struct Counters {
    std::atomic<int> micOn{0}, granted{0}, ends{0};
    Participant::Callbacks cb() {
        Participant::Callbacks c;
        c.onEvent = [this](const FloorEvent& ev) { if (ev.kind == FloorEvent::Kind::Granted) granted++; };
        c.onMic = [this](bool on) { if (on) micOn++; };
        c.onBroadcastEnd = [this] { ends++; };
        return c;
    }
};
}  // namespace

// §14.3.4 — answer 의 mc_granted = 200 OK 로 승인: Floor Request 없이 'U: has permission', 뒤따르는 Floor Granted 에도 머문다(§6.2.4.5.5).
TEST(FloorParticipant, ImplicitRequestGrantedInAnswer) {
    pjReady();
    FakeServer srv;
    Counters k;
    Participant p(3, 0x1111u, "tel:+82500000003", k.cb());
    ASSERT_TRUE(p.open(0));
    p.armImplicitRequest(false);
    EXPECT_EQ(p.info().state, FloorState::Requesting);
    p.setRemote("127.0.0.1", srv.port);
    p.onInitialAnswer(true, true);
    EXPECT_EQ(p.info().state, FloorState::Speaking);
    EXPECT_EQ(k.micOn.load(), 1);
    EXPECT_EQ(k.granted.load(), 1);
    EXPECT_FALSE(srv.expect(Op::REQUEST, 400));                      // 요청은 INVITE 가 실었다 — 따로 보내지 않는다
    srv.send(p.localPort(), Op::GRANTED, {u16Field((uint8_t)Field::DURATION, 30)});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(p.info().state, FloorState::Speaking);
    p.close();
}

// §14.3.5 — answer 의 mc_implicit_request = 받아들임(승인 아님, §12.1.2.2 NOTE 4): 요청 중에 머물다 Floor Granted 로 발언.
TEST(FloorParticipant, ImplicitRequestAcceptedWaitsForFloorGranted) {
    pjReady();
    FakeServer srv;
    Counters k;
    Participant p(4, 0x2222u, "tel:+82500000004", k.cb());
    ASSERT_TRUE(p.open(0));
    p.armImplicitRequest(false);
    p.setRemote("127.0.0.1", srv.port);
    p.onInitialAnswer(false, true);
    EXPECT_EQ(p.info().state, FloorState::Requesting);
    EXPECT_EQ(k.micOn.load(), 0);
    EXPECT_FALSE(srv.expect(Op::REQUEST, 400));
    srv.send(p.localPort(), Op::GRANTED, {u16Field((uint8_t)Field::DURATION, 30)});
    EXPECT_TRUE(waitFor([&] { return p.info().state == FloorState::Speaking; }, 2000));
    EXPECT_EQ(k.micOn.load(), 1);
    p.close();
}

// §14.3.5 — 서버가 암묵 요청으로 받지 않았다(진행 중 호 합류 등): 누르고 있으니 명시 Floor Request 로 잇는다.
TEST(FloorParticipant, ImplicitRequestNotAcceptedFallsBackToExplicit) {
    pjReady();
    FakeServer srv;
    Counters k;
    Participant p(5, 0x3333u, "tel:+82500000005", k.cb());
    ASSERT_TRUE(p.open(0));
    p.armImplicitRequest(false);
    p.setRemote("127.0.0.1", srv.port);
    p.onInitialAnswer(false, false);
    EXPECT_TRUE(srv.expect(Op::REQUEST));
    EXPECT_EQ(p.info().state, FloorState::Requesting);
    p.close();
}

// 호 성립 전에 놓았다(짧은 탭) — 그 사이 온 Floor Granted 는 무시하고, answer 에서 Release 로 돌려준다. 일제 통화 개시자는 이어 오는
//   B-bit Floor Idle 로 호를 해제한다(§6.2.4.6.4).
TEST(FloorParticipant, ImplicitRequestReleasedBeforeAnswerReturnsFloor) {
    pjReady();
    FakeServer srv;
    Counters k;
    Participant p(6, 0x4444u, "tel:+82500000006", k.cb());
    ASSERT_TRUE(p.open(0));
    p.setBroadcastInitiator(true);
    p.armImplicitRequest(false);
    p.release();
    EXPECT_EQ(p.info().state, FloorState::Idle);
    srv.send(p.localPort(), Op::GRANTED, {u16Field((uint8_t)Field::DURATION, 30), u16Field((uint8_t)Field::FLOOR_INDICATOR, 0xC000)});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(k.micOn.load(), 0);
    EXPECT_EQ(k.granted.load(), 0);
    p.setRemote("127.0.0.1", srv.port);
    p.onInitialAnswer(true, true);
    EXPECT_TRUE(srv.expect(Op::RELEASE));
    EXPECT_NE(p.info().state, FloorState::Speaking);
    srv.send(p.localPort(), Op::IDLE, {u16Field((uint8_t)Field::MSG_SEQ, 1), u16Field((uint8_t)Field::FLOOR_INDICATOR, 0xC000)});
    EXPECT_TRUE(waitFor([&] { return k.ends.load() == 1; }, 2000));
    p.close();
}

// Floor Granted 가 answer 보다 먼저 왔다(서버는 PTT_JOIN 처리 중에 보낸다) — answer 는 아무것도 바꾸지 않는다.
TEST(FloorParticipant, ImplicitRequestGrantedBeforeAnswer) {
    pjReady();
    FakeServer srv;
    Counters k;
    Participant p(7, 0x5555u, "tel:+82500000007", k.cb());
    ASSERT_TRUE(p.open(0));
    p.armImplicitRequest(false);
    srv.send(p.localPort(), Op::GRANTED, {u16Field((uint8_t)Field::DURATION, 30)});
    ASSERT_TRUE(waitFor([&] { return p.info().state == FloorState::Speaking; }, 2000));
    p.setRemote("127.0.0.1", srv.port);
    p.onInitialAnswer(false, false);
    EXPECT_FALSE(srv.expect(Op::REQUEST, 400));
    EXPECT_EQ(p.info().state, FloorState::Speaking);
    EXPECT_EQ(k.micOn.load(), 1);
    p.close();
}
