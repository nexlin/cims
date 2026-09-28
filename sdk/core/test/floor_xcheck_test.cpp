// S1-UE-FLOOR-CODEC — CMP 서버 코덱(cmp/PFloorCodec.cpp)과 코어 코덱의 교차 검증 (ue_sdk.md §4.6).
//  ① 코어 빌더(participant 메시지) → CMP ParseFloorMessage
//  ② CMP BuildFloorMessage(서버 메시지: Granted/Taken 리스트/Ack 요구) → 코어 decode
// 상수는 생성 헤더(floor_defs.h)와 CMP 헤더(PMcpttGroup.h)를 각각 쓰므로 값 드리프트도 여기서 걸린다.
#include <gtest/gtest.h>

#include "PMcpttGroup.h"           // CMP — FloorTlv/ParsedFloor/BuildFloorMessage/ParseFloorMessage
#include "../src/floor/floor_codec.h"

namespace core = cimsue::floor;

TEST(FloorXCheck, DefsMatchCmpHeader) {
    EXPECT_EQ((int)core::Op::REQUEST, (int)FLOOR_REQUEST);
    EXPECT_EQ((int)core::Op::GRANTED, (int)FLOOR_GRANT);
    EXPECT_EQ((int)core::Op::TAKEN, (int)FLOOR_TAKEN);
    EXPECT_EQ((int)core::Op::DENY, (int)FLOOR_REJECT);
    EXPECT_EQ((int)core::Op::RELEASE, (int)FLOOR_RELEASE);
    EXPECT_EQ((int)core::Op::IDLE, (int)FLOOR_IDLE);
    EXPECT_EQ((int)core::Op::REVOKE, (int)FLOOR_REVOKE);
    EXPECT_EQ((int)core::Op::QUEUE_POS_INFO, (int)FLOOR_QUEUE_POS_INFO);
    EXPECT_EQ((int)core::Op::ACK, (int)FLOOR_ACK);
    EXPECT_EQ((int)core::Op::QUEUED_CANCEL, (int)FLOOR_QUEUED_CANCEL);
    EXPECT_EQ((int)core::Op::RELEASE_MULTI, (int)FLOOR_RELEASE_MULTI);
    EXPECT_EQ((int)core::Field::USER_ID, (int)FF_USER_ID);
    EXPECT_EQ((int)core::Field::GRANTED_PARTY, (int)FF_GRANTED_PARTY);
    EXPECT_EQ((int)core::Field::FLOOR_INDICATOR, (int)FF_FLOOR_INDICATOR);
    EXPECT_EQ((int)core::Field::GRANTED_USERS, (int)FF_GRANTED_USERS);
    EXPECT_EQ((int)core::Field::SSRC_LIST, (int)FF_SSRC_LIST);
    EXPECT_EQ((int)core::Field::MEDIA_FLOW, (int)FF_MEDIA_FLOW);
    EXPECT_EQ(core::kAckRequiredBit, FLOOR_ACK_REQ_BIT);
}

TEST(FloorXCheck, CoreRequestParsedByCmp) {
    std::string pkt = core::request(0xCAFEBABE, "tel:+82500000001", 3, (int)core::indicator::EMERGENCY);
    ParsedFloor pf;
    ASSERT_TRUE(ParseFloorMessage(pkt.data(), (int)pkt.size(), pf));
    EXPECT_EQ(pf.subtype, (int)FLOOR_REQUEST);
    EXPECT_EQ(pf.ssrc, 0xCAFEBABEu);
    EXPECT_EQ(pf.userId(), "tel:+82500000001");
    EXPECT_EQ(pf.priority(), 3);
    EXPECT_EQ(pf.indicator(), 0x1000);

    std::string rel = core::release(7, "tel:+82500000001");
    ASSERT_TRUE(ParseFloorMessage(rel.data(), (int)rel.size(), pf));
    EXPECT_EQ(pf.subtype, (int)FLOOR_RELEASE);
    EXPECT_EQ(pf.userId(), "tel:+82500000001");

    std::string ack = core::ackOf(7, (uint8_t)(FLOOR_GRANT | FLOOR_ACK_REQ_BIT));
    ASSERT_TRUE(ParseFloorMessage(ack.data(), (int)ack.size(), pf));
    EXPECT_EQ(pf.subtype, (int)FLOOR_ACK);
    EXPECT_EQ(pf.u16(FF_SOURCE), (int)FLOOR_SRC_PARTICIPANT);
    EXPECT_EQ((unsigned char)pf.str(FF_MSG_TYPE)[0], FLOOR_GRANT | FLOOR_ACK_REQ_BIT);

    std::string cancel = core::cancelQueuedRequest(7);
    ASSERT_TRUE(ParseFloorMessage(cancel.data(), (int)cancel.size(), pf));
    EXPECT_EQ(pf.subtype, (int)FLOOR_QUEUED_CANCEL);
    EXPECT_EQ(pf.u16(FF_QUEUED_PURPOSE), 0);
}

TEST(FloorXCheck, CmpServerMessagesDecodedByCore) {
    char buf[512];
    // Granted (ack 요구) — Duration 30, Indicator normal
    std::vector<FloorTlv> f;
    f.push_back(FloorTlv(FF_DURATION, FloorU16(30)));
    f.push_back(FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0x8000)));
    int n = BuildFloorMessage(buf, sizeof buf, (unsigned char)(FLOOR_GRANT | FLOOR_ACK_REQ_BIT), 0x01, f);
    ASSERT_GT(n, 0);
    core::Message m;
    ASSERT_TRUE(core::decode((const uint8_t*)buf, n, m));
    EXPECT_EQ(m.op, (uint8_t)core::Op::GRANTED);
    EXPECT_TRUE(m.ackRequired);
    EXPECT_EQ(m.durationSec(), 30);
    EXPECT_EQ(m.indicator(), 0x8000);

    // Taken — 동시 발언 2명 리스트 + Permission=0 + MSN
    f.clear();
    f.push_back(FloorTlv(FF_GRANTED_USERS, FloorUserList({"tel:+82500000001", "tel:+82500000002"})));
    f.push_back(FloorTlv(FF_SSRC_LIST, FloorSsrcList({0x1111u, 0x2222u})));
    f.push_back(FloorTlv(FF_PERMISSION, FloorU16(FLOOR_PERM_DENIED)));
    f.push_back(FloorTlv(FF_MSG_SEQ, FloorU16(1000)));
    f.push_back(FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0x0080)));
    n = BuildFloorMessage(buf, sizeof buf, (unsigned char)FLOOR_TAKEN, 0x02, f);
    ASSERT_GT(n, 0);
    ASSERT_TRUE(core::decode((const uint8_t*)buf, n, m));
    EXPECT_EQ(m.op, (uint8_t)core::Op::TAKEN);
    auto tk = m.talkers();
    ASSERT_EQ(tk.size(), 2u);
    EXPECT_EQ(tk[0].id, "tel:+82500000001"); EXPECT_EQ(tk[0].ssrc, 0x1111u);
    EXPECT_EQ(tk[1].id, "tel:+82500000002"); EXPECT_EQ(tk[1].ssrc, 0x2222u);
    EXPECT_EQ(m.permission(), 0);
    EXPECT_EQ(m.msgSeq(), 1000);
    EXPECT_EQ(m.indicator() & (int)core::indicator::MULTI_TALKER, (int)core::indicator::MULTI_TALKER);

    // Taken 단일 화자 — Granted Party + SSRC(6옥텟 필드)
    f.clear();
    f.push_back(FloorTlv(FF_GRANTED_PARTY, "tel:+82500000003"));
    f.push_back(FloorTlv(FF_SSRC, FloorSsrc(0xDEADBEEFu)));
    n = BuildFloorMessage(buf, sizeof buf, (unsigned char)FLOOR_TAKEN, 0x02, f);
    ASSERT_TRUE(core::decode((const uint8_t*)buf, n, m));
    tk = m.talkers();
    ASSERT_EQ(tk.size(), 1u);
    EXPECT_EQ(tk[0].id, "tel:+82500000003");
    EXPECT_EQ(tk[0].ssrc, 0xDEADBEEFu);

    // Deny cause 5 / Revoke cause 2 / Queue position
    f.clear(); f.push_back(FloorTlv(FF_REJECT_CAUSE, FloorU16(5)));
    n = BuildFloorMessage(buf, sizeof buf, (unsigned char)FLOOR_REJECT, 0x02, f);
    ASSERT_TRUE(core::decode((const uint8_t*)buf, n, m));
    EXPECT_EQ(m.cause(), 5);
    f.clear(); f.push_back(FloorTlv(FF_QUEUE_INFO, FloorQueueInfo(2, 1)));
    n = BuildFloorMessage(buf, sizeof buf, (unsigned char)FLOOR_QUEUE_POS_INFO, 0x02, f);
    ASSERT_TRUE(core::decode((const uint8_t*)buf, n, m));
    EXPECT_EQ(m.queuePosition(), 2);
}

// ── 일제 통화(TS 24.379 §4.12) — 코어 participant ↔ CMP 가 만든 서버 메시지(루프백 UDP) ──
//   개시자: Floor Request 에 B-bit(TS 24.380 §6.2.4.3.5) → Granted → 발언을 놓음(U: pending Release) →
//   B-bit Floor Idle 이면 호 해제 콜백(§6.2.4.6.4). 일반 그룹 Idle·해제 전 Idle 은 호를 끝내지 않는다.
#include <pjlib.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../src/floor/floor_participant.h"

namespace {

void pjReady() {
    static bool inited = (pj_init() == PJ_SUCCESS);
    (void)inited;
    if (!pj_thread_is_registered()) {
        static thread_local pj_thread_desc desc;
        pj_thread_t* th = nullptr;
        pj_bzero(desc, sizeof(desc));
        pj_thread_register("xcheck", desc, &th);
    }
}

/** CMP 자리 UDP 소켓 — participant 가 보낸 것을 받고 서버 메시지를 돌려준다. */
struct FakeCmp {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    FakeCmp() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s);
        pj_sockaddr_in a;
        pj_sockaddr_in_init(&a, nullptr, 0);
        pj_sock_bind(s, &a, sizeof(a));
        int l = sizeof(a);
        pj_sock_getsockname(s, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeCmp() { if (s != PJ_INVALID_SOCKET) pj_sock_close(s); }
    /** subtype 이 맞는 메시지가 올 때까지(Ack keepalive 는 건너뜀) — 시한 안에 없으면 false. */
    bool expect(int subtype, ParsedFloor& out, int ms = 2000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 50};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char buf[1500];
            pj_ssize_t n = sizeof(buf);
            if (pj_sock_recv(s, buf, &n, 0) != PJ_SUCCESS || n <= 0) continue;
            ParsedFloor pf;
            if (ParseFloorMessage(buf, (int)n, pf) && pf.subtype == subtype) { out = pf; return true; }
        }
        return false;
    }
    void sendTo(int toPort, unsigned char subtype, const std::vector<FloorTlv>& f) {
        char buf[512];
        int n = BuildFloorMessage(buf, sizeof buf, subtype, 0x01, f);
        pj_sockaddr_in to;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)toPort);
        pj_ssize_t len = n;
        pj_sock_sendto(s, buf, &len, 0, &to, sizeof(to));
    }
};

bool waitTrue(const std::atomic<int>& v, int want, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (v.load() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return v.load() >= want;
}

}  // namespace

TEST(FloorXCheck, BroadcastInitiatorReleasesCallOnIdle) {
    pjReady();
    FakeCmp cmp;
    std::atomic<int> ends{0}, idles{0};
    core::Participant::Callbacks cb;
    cb.onEvent = [&](const cimsue::FloorEvent& ev) { if (ev.kind == cimsue::FloorEvent::Kind::Idle) idles++; };
    cb.onBroadcastEnd = [&] { ends++; };
    core::Participant p(1, 0x1234u, "tel:+82500000001", cb);
    ASSERT_TRUE(p.open(0));
    p.setBroadcastInitiator(true);
    p.setRemote("127.0.0.1", cmp.port);

    // 해제 전에 온 B-bit Idle(다른 참가자 관점의 상태 동기화)은 호를 끝내지 않는다
    cmp.sendTo(p.localPort(), FLOOR_IDLE, {FloorTlv(FF_MSG_SEQ, FloorU16(1)), FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0xC000))});
    ASSERT_TRUE(waitTrue(idles, 1, 2000));
    EXPECT_EQ(ends.load(), 0);

    p.request();
    ParsedFloor rq;
    ASSERT_TRUE(cmp.expect(FLOOR_REQUEST, rq));
    EXPECT_EQ(rq.indicator() & 0x4000, 0x4000);                      // B-bit (R8)
    cmp.sendTo(p.localPort(), FLOOR_GRANT, {FloorTlv(FF_DURATION, FloorU16(30)), FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0xC000))});
    auto t0 = std::chrono::steady_clock::now();
    while (p.info().state != cimsue::FloorState::Speaking && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(p.info().state, cimsue::FloorState::Speaking);

    p.release();
    ParsedFloor rl;
    ASSERT_TRUE(cmp.expect(FLOOR_RELEASE, rl));
    cmp.sendTo(p.localPort(), FLOOR_IDLE, {FloorTlv(FF_MSG_SEQ, FloorU16(2)), FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0xC000))});
    EXPECT_TRUE(waitTrue(ends, 1, 2000));                             // R9 — 개시 단말이 호를 해제한다
    p.close();
}

TEST(FloorXCheck, NormalGroupIdleAfterReleaseKeepsCall) {
    pjReady();
    FakeCmp cmp;
    std::atomic<int> ends{0}, idles{0};
    core::Participant::Callbacks cb;
    cb.onEvent = [&](const cimsue::FloorEvent& ev) { if (ev.kind == cimsue::FloorEvent::Kind::Idle) idles++; };
    cb.onBroadcastEnd = [&] { ends++; };
    core::Participant p(2, 0x5678u, "tel:+82500000002", cb);
    ASSERT_TRUE(p.open(0));
    p.setRemote("127.0.0.1", cmp.port);                               // 개시자 아님(일반 그룹 통화)

    p.request();
    ParsedFloor rq;
    ASSERT_TRUE(cmp.expect(FLOOR_REQUEST, rq));
    EXPECT_EQ(rq.indicator() < 0 ? 0 : rq.indicator() & 0x4000, 0);  // B-bit 없음
    cmp.sendTo(p.localPort(), FLOOR_GRANT, {FloorTlv(FF_DURATION, FloorU16(30)), FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0x8000))});
    auto t0 = std::chrono::steady_clock::now();
    while (p.info().state != cimsue::FloorState::Speaking && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    p.release();
    ParsedFloor rl;
    ASSERT_TRUE(cmp.expect(FLOOR_RELEASE, rl));
    cmp.sendTo(p.localPort(), FLOOR_IDLE, {FloorTlv(FF_MSG_SEQ, FloorU16(3)), FloorTlv(FF_FLOOR_INDICATOR, FloorU16(0x8000))});
    ASSERT_TRUE(waitTrue(idles, 1, 2000));
    EXPECT_EQ(ends.load(), 0);
    p.close();
}
