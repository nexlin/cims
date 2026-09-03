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
