// libcimsue 단위시험 — floor 코덱 (S1-UE-FLOOR-CODEC 의 코어 측). CMP 코덱과의 교차 검증은 floor_xcheck_test.cpp.
#include <gtest/gtest.h>

#include "../src/floor/floor_codec.h"

using namespace cimsue::floor;

static std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

TEST(FloorCodec, RequestLayoutAndPadding) {
    // User ID "tel:+8250" (9B) → 헤더2+9=11 → 4옥텟 정렬 패딩 1. 전체 = 12 + 12 = 24B, length words = 5.
    std::string pkt = request(0x11223344, "tel:+8250");
    ASSERT_EQ(pkt.size(), 24u);
    EXPECT_EQ((uint8_t)pkt[0], 0x80 | (uint8_t)Op::REQUEST);
    EXPECT_EQ((uint8_t)pkt[1], kRtcpPtApp);
    EXPECT_EQ(((uint8_t)pkt[2] << 8) | (uint8_t)pkt[3], 24 / 4 - 1);
    EXPECT_EQ(pkt.substr(8, 4), "MCPT");
    EXPECT_EQ((uint8_t)pkt[12], (uint8_t)Field::USER_ID);
    EXPECT_EQ((uint8_t)pkt[13], 9);
    EXPECT_EQ(pkt.substr(14, 9), "tel:+8250");
    EXPECT_EQ(pkt[23], '\0');
    Message m;
    ASSERT_TRUE(decode((const uint8_t*)pkt.data(), pkt.size(), m));
    EXPECT_EQ(m.op, (uint8_t)Op::REQUEST);
    EXPECT_FALSE(m.ackRequired);
    EXPECT_EQ(m.ssrc, 0x11223344u);
    EXPECT_EQ(m.userId(), "tel:+8250");
    EXPECT_EQ(m.priority(), -1);                       // 미기재 — 유효 우선순위를 깎지 않는다
}

TEST(FloorCodec, RequestWithPriorityAndEmergencyIndicator) {
    std::string pkt = request(1, "u", 7, (int)indicator::EMERGENCY);
    Message m;
    ASSERT_TRUE(decode((const uint8_t*)pkt.data(), pkt.size(), m));
    EXPECT_EQ(m.priority(), 7);
    EXPECT_EQ(m.indicator(), (int)indicator::EMERGENCY);
    EXPECT_EQ(m.userId(), "u");
}

TEST(FloorCodec, AckRequiredBitAndAckOf) {
    Message taken; taken.op = (uint8_t)Op::TAKEN; taken.ackRequired = true; taken.ssrc = 5;
    taken.fields.push_back(Tlv{(uint8_t)Field::GRANTED_PARTY, "tel:+8251"});
    taken.fields.push_back(u16Field((uint8_t)Field::PERMISSION, (int)Permission::DENIED));
    taken.fields.push_back(u16Field((uint8_t)Field::MSG_SEQ, 42));
    std::string pkt = encode(taken);
    EXPECT_EQ((uint8_t)pkt[0] & 0x1F, (uint8_t)Op::TAKEN | kAckRequiredBit);
    Message m;
    ASSERT_TRUE(decode((const uint8_t*)pkt.data(), pkt.size(), m));
    EXPECT_TRUE(m.ackRequired);
    EXPECT_EQ(m.op, (uint8_t)Op::TAKEN);
    EXPECT_EQ(m.permission(), 0);
    EXPECT_EQ(m.msgSeq(), 42);
    ASSERT_EQ(m.talkers().size(), 1u);
    EXPECT_EQ(m.talkers()[0].id, "tel:+8251");

    std::string ack = ackOf(9, (uint8_t)((uint8_t)Op::TAKEN | kAckRequiredBit));
    Message a;
    ASSERT_TRUE(decode((const uint8_t*)ack.data(), ack.size(), a));
    EXPECT_EQ(a.op, (uint8_t)Op::ACK);
    EXPECT_EQ(a.u16((uint8_t)Field::SOURCE), (int)Source::PARTICIPANT);
    const Tlv* mt = a.field((uint8_t)Field::MSG_TYPE);
    ASSERT_NE(mt, nullptr);
    EXPECT_EQ((uint8_t)mt->value[0], (uint8_t)Op::TAKEN | kAckRequiredBit);   // 상위 옥텟 subtype(ack 비트 포함)
}

TEST(FloorCodec, MultiTalkerLists) {
    Message t; t.op = (uint8_t)Op::TAKEN; t.ssrc = 1;
    std::string users; users += (char)2; users += (char)3; users += "aaa"; users += (char)4; users += "bbbb";
    t.fields.push_back(Tlv{(uint8_t)Field::GRANTED_USERS, users});
    std::string ss(2, '\0'); ss[0] = 2;
    for (uint32_t v : {0x01020304u, 0x0A0B0C0Du}) { ss += (char)(v >> 24); ss += (char)(v >> 16); ss += (char)(v >> 8); ss += (char)v; }
    t.fields.push_back(Tlv{(uint8_t)Field::SSRC_LIST, ss});
    std::string pkt = encode(t);
    EXPECT_EQ(pkt.size() % 4, 0u);
    Message m;
    ASSERT_TRUE(decode((const uint8_t*)pkt.data(), pkt.size(), m));
    auto tk = m.talkers();
    ASSERT_EQ(tk.size(), 2u);
    EXPECT_EQ(tk[0].id, "aaa"); EXPECT_EQ(tk[0].ssrc, 0x01020304u);
    EXPECT_EQ(tk[1].id, "bbbb"); EXPECT_EQ(tk[1].ssrc, 0x0A0B0C0Du);
}

TEST(FloorCodec, ReleaseCancelAndKeepalive) {
    Message m;
    std::string r = release(3, "tel:+1", (int)indicator::DUAL_FLOOR);
    ASSERT_TRUE(decode((const uint8_t*)r.data(), r.size(), m));
    EXPECT_EQ(m.op, (uint8_t)Op::RELEASE); EXPECT_EQ(m.indicator(), (int)indicator::DUAL_FLOOR);
    std::string c = cancelQueuedRequest(3);
    ASSERT_TRUE(decode((const uint8_t*)c.data(), c.size(), m));
    EXPECT_EQ(m.op, (uint8_t)Op::QUEUED_CANCEL); EXPECT_EQ(m.queuedPurpose(), (int)QueuedPurpose::CANCEL_REQUEST);
    std::string k = ack(3, "tel:+1");
    ASSERT_TRUE(decode((const uint8_t*)k.data(), k.size(), m));
    EXPECT_EQ(m.op, (uint8_t)Op::ACK); EXPECT_EQ(m.userId(), "tel:+1");
}

TEST(FloorCodec, RejectsNonMcpt) {
    std::string pkt = request(1, "u");
    pkt[9] = 'X';
    Message m;
    EXPECT_FALSE(decode((const uint8_t*)pkt.data(), pkt.size(), m));
    EXPECT_FALSE(decode((const uint8_t*)pkt.data(), 5, m));
    EXPECT_STREQ(opName((uint8_t)Op::REVOKE), "REVOKE");
    EXPECT_STREQ(rejectCauseText(5), "Receive only");
    EXPECT_EQ(rejectCauseText(200), nullptr);
}
