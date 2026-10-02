// S1-UE-UNIT — MCVideo 전송 제어 코어 코덱(TS 24.581 §9) — 골든 바이트·정렬·§9.1.4 검사·참여자 빌더.
#include <gtest/gtest.h>

#include "mcvideo/tc_codec.h"

namespace mv = cimsue::mcvideo;
using mv::AppName;
using mv::Field;

static std::string bytes(std::initializer_list<int> b) {
    std::string s;
    for (int v : b) s.push_back((char)v);
    return s;
}
static bool dec(const std::string& pkt, mv::Message& m) { return mv::decode((const uint8_t*)pkt.data(), pkt.size(), m); }
// 검사 없이 TLV 를 늘어놓는 원시 패킷 — 수신 검사(§9.1.4) 시험용
static std::string raw(const char* name, uint8_t subtype, const std::vector<std::pair<int, std::string>>& tlvs) {
    std::string body;
    for (auto& t : tlvs) {
        body += (char)t.first;
        body += (char)t.second.size();
        body += t.second;
        while (body.size() % 4) body += '\0';                          // 필드 단위 4옥텟 정렬
    }
    std::string h(12, '\0');
    h[0] = (char)(0x80 | subtype);
    h[1] = (char)204;
    size_t words = (12 + body.size()) / 4 - 1;
    h[2] = (char)(words >> 8);
    h[3] = (char)words;
    h[7] = 1;
    h.replace(8, 4, name, 4);
    return h + body;
}

// §9.2.4 Transmission Request — Transmission Priority(u8 + spare) · Transmission Indicator(D = emergency)
TEST(McvCodec, GoldenTransmissionRequest) {
    std::string pkt = mv::transmissionRequest(0x11223344, 5, mv::indicator::EMERGENCY);
    EXPECT_EQ(pkt, bytes({0x80, 0xCC, 0x00, 0x04, 0x11, 0x22, 0x33, 0x44, 'M', 'C', 'V', '0',
                          0x00, 0x02, 0x05, 0x00, 0x0D, 0x02, 0x10, 0x00}));
    // 기본 우선순위·일반 호는 필드를 싣지 않는다(§6.2.4.3.2 2a·2b)
    EXPECT_EQ(mv::transmissionRequest(1).size(), 12u);
}

// §9.2.31 Ack — MCV2 subtype 4, Source 0, Message Name = 확인 대상 name, Message Type = subtype(첫 비트 0)
TEST(McvCodec, GoldenAckCarriesNameAndTypeWithoutAckBit) {
    std::string pkt = mv::ackOf(7, AppName::MCV1, (uint8_t)((uint8_t)mv::Mcv1::TRANSMISSION_GRANTED | mv::kAckRequiredBit));
    EXPECT_EQ(pkt, bytes({0x84, 0xCC, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07, 'M', 'C', 'V', '2',
                          0x0A, 0x02, 0x00, 0x00,
                          0x10, 0x06, 'M', 'C', 'V', '1', 0x00, 0x00,
                          0x0C, 0x02, 0x00, 0x00}));
    mv::Message m;
    ASSERT_TRUE(dec(mv::ackOf(7, AppName::MCV2, (uint8_t)mv::Mcv2::MEDIA_RECEPTION_END_REQUEST), m));
    EXPECT_EQ(m.app, AppName::MCV2);
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::TRANSMISSION_CONTROL_ACK);
    EXPECT_EQ(m.source(), (int)mv::Source::PARTICIPANT);
    EXPECT_EQ(m.messageName(), "MCV2");
    EXPECT_EQ(m.messageType(), (int)mv::Mcv2::MEDIA_RECEPTION_END_REQUEST);
}

// §9.1.3 — 가변 필드는 (헤더 2 + 값)을 4옥텟 경계로 패딩, 뒤 필드도 경계에서 시작한다
TEST(McvCodec, VariableFieldPaddingKeepsFieldsAligned) {
    for (size_t n = 1; n <= 9; ++n) {
        std::string id(n, 'u');
        std::string pkt = mv::receiveMediaRequest(9, id, 0xA1A2A3A4, 0xB1B2B3B4, 3);
        ASSERT_EQ(pkt.size() % 4, 0u) << n;
        size_t userField = 2 + n + ((4 - (2 + n) % 4) % 4);
        EXPECT_EQ((uint8_t)pkt[12 + userField], (uint8_t)Field::AUDIO_SSRC) << n;   // 다음 필드가 경계에서 시작
        mv::Message m;
        ASSERT_TRUE(dec(pkt, m)) << n;
        EXPECT_EQ(m.transmittingUserId(), id);
        EXPECT_EQ(m.audioSsrc(), 0xA1A2A3A4u);
        EXPECT_EQ(m.videoSsrc(), 0xB1B2B3B4u);
        EXPECT_EQ(m.receptionPriority(), 3);
        EXPECT_EQ(m.indicator(), -1);
    }
}

TEST(McvCodec, ParticipantBuildersRoundTrip) {
    mv::Message m;
    ASSERT_TRUE(dec(mv::transmissionEndRequest(3, mv::indicator::NORMAL), m));
    EXPECT_EQ(m.app, AppName::MCV2);
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::TRANSMISSION_END_REQUEST);
    EXPECT_EQ(m.indicator(), 0x8000);
    ASSERT_TRUE(dec(mv::transmissionEndResponse(3), m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::TRANSMISSION_END_RESPONSE);
    EXPECT_TRUE(m.fields.empty());
    // 표 9.2.20-1·9.2.21-1 — 끝낼 송출의 User ID of the Transmitting User·Audio SSRC·Video SSRC
    ASSERT_TRUE(dec(mv::transmissionEndRequest(3, -1, "tel:+82500000013", 0x1111, 0x2222), m));
    EXPECT_EQ(m.transmittingUserId(), "tel:+82500000013");
    EXPECT_EQ(m.audioSsrc(), 0x1111u);
    EXPECT_EQ(m.videoSsrc(), 0x2222u);
    EXPECT_LT(m.indicator(), 0);
    ASSERT_TRUE(dec(mv::transmissionEndRequest(3, -1, "tel:+82500000013"), m));      // 허가 전 취소 — SSRC 를 아직 모른다
    EXPECT_EQ(m.fields.size(), 1u);
    ASSERT_TRUE(dec(mv::transmissionEndResponse(3, "tel:+82500000013", 0x1111, 0x2222), m));
    EXPECT_EQ(m.transmittingUserId(), "tel:+82500000013");
    EXPECT_EQ(m.videoSsrc(), 0x2222u);
    ASSERT_TRUE(dec(mv::queuePositionRequest(3), m));
    EXPECT_EQ(m.app, AppName::MCV0);
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv0::QUEUE_POSITION_REQUEST);
    ASSERT_TRUE(dec(mv::mediaReceptionEndRequest(3, "tel:+82500000014", 10, 20), m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::MEDIA_RECEPTION_END_REQUEST);
    EXPECT_EQ(m.transmittingUserId(), "tel:+82500000014");
    EXPECT_EQ(m.audioSsrc(), 10u);
    EXPECT_EQ(m.videoSsrc(), 20u);
    ASSERT_TRUE(dec(mv::mediaReceptionEndResponse(3, "tel:+82500000014", 10, 20), m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::MEDIA_RECEPTION_END_RESPONSE);
    EXPECT_EQ(m.videoSsrc(), 20u);
}

// 서버 쪽 메시지도 코어 encode 로 만들 수 있다 — Reject Cause + phrase(§9.2.3.4), ack 요구 비트
TEST(McvCodec, CauseWithPhraseAndAckRequired) {
    mv::Message m;
    m.app = AppName::MCV1;
    m.op = (uint8_t)mv::Mcv1::TRANSMISSION_REJECTED;
    m.ackRequired = true;
    m.fields.push_back(mv::causeField((int)mv::RejectCause::TRANSMISSION_LIMIT, "limit"));
    std::string pkt = mv::encode(m);
    ASSERT_FALSE(pkt.empty());
    EXPECT_EQ((uint8_t)pkt[0], 0x80 | 0x10 | 0x01);
    EXPECT_EQ((uint8_t)pkt[13], 7u);                                   // Length = 원인 2 + phrase 5(패딩 제외)
    mv::Message d;
    ASSERT_TRUE(dec(pkt, d));
    EXPECT_TRUE(d.ackRequired);
    EXPECT_EQ(d.cause(), 1);
    EXPECT_EQ(d.causePhrase(), "limit");
    EXPECT_STREQ(mv::rejectCauseText(d.cause()), "Transmission limit reached");
}

// §9.1.4 1 — 모르는 subtype(void 포함)·다른 APP 이면 메시지 전체를 버린다
TEST(McvCodec, DecodeRejectsUnknownMessage) {
    mv::Message m;
    EXPECT_FALSE(dec(raw("MCV0", 0x05, {}), m));                        // MCV0 0101 = void
    EXPECT_FALSE(dec(raw("MCV1", 0x09, {}), m));                        // MCV1 1001 = void
    EXPECT_FALSE(dec(raw("MCV2", 0x05, {}), m));
    EXPECT_FALSE(dec(raw("MCPT", 0x00, {}), m));                        // MCPTT floor
    EXPECT_FALSE(dec(raw("MCV3", 0x00, {}), m));                        // MBMS subchannel — 전송 제어 아님
    EXPECT_TRUE(dec(raw("MCV1", 0x0F, {}), m));                         // Transmission Idle
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv1::TRANSMISSION_IDLE);
}

// §9.1.4 2·3 — 그 메시지 표 밖 필드·고정 길이가 틀린 필드는 버리고 나머지는 읽는다
TEST(McvCodec, DecodeDropsFieldsOutsideTableOrMalformed) {
    std::string ssrc6 = bytes({0, 0, 0, 9, 0, 0});
    std::string pkt = raw("MCV0", 0x00, {{(int)Field::AUDIO_SSRC, ssrc6},             // Transmission Request 표 밖
                                         {(int)Field::TRANSMISSION_PRIORITY, bytes({5, 0, 0, 0})},   // Length 4 — 틀림
                                         {60, bytes({1, 2})},                          // 모르는 ID
                                         {(int)Field::TRANSMISSION_INDICATOR, bytes({0x80, 0})}});
    mv::Message m;
    ASSERT_TRUE(dec(pkt, m));
    ASSERT_EQ(m.fields.size(), 1u);
    EXPECT_EQ(m.indicator(), 0x8000);
    EXPECT_EQ(m.priority(), -1);
}

// encode 는 표 밖 필드·틀린 모양을 싣지 않는다
TEST(McvCodec, EncodeRejectsFieldOutsideTable) {
    mv::Message m;
    m.app = AppName::MCV0;
    m.op = (uint8_t)mv::Mcv0::TRANSMISSION_REQUEST;
    m.fields.push_back(mv::ssrcField(Field::AUDIO_SSRC, 1));
    EXPECT_TRUE(mv::encode(m).empty());
    m.fields = {mv::u8Field(Field::TRANSMISSION_PRIORITY, 5)};
    EXPECT_FALSE(mv::encode(m).empty());
    m.fields = {mv::Tlv{(uint8_t)Field::TRANSMISSION_PRIORITY, std::string(1, '\5')}};   // Length 1 — 틀림
    EXPECT_TRUE(mv::encode(m).empty());
    m.op = 0x05;                                                         // void
    m.fields.clear();
    EXPECT_TRUE(mv::encode(m).empty());
}

// §9.1.1 — 한 IP 패킷에 메시지가 여럿일 수 있다: 헤더 length 가 가리키는 끝까지만 읽는다
TEST(McvCodec, DeclaredLengthBoundsTheMessage) {
    std::string a = mv::transmissionRequest(1, 7);
    std::string b = mv::receiveMediaRequest(1, "sip:x@y", 1, 2);
    std::string both = a + b;
    mv::Message m;
    ASSERT_TRUE(dec(both, m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv0::TRANSMISSION_REQUEST);
    EXPECT_EQ(m.fields.size(), 1u);
    EXPECT_EQ(m.priority(), 7);
    ASSERT_TRUE(mv::decode((const uint8_t*)both.data() + a.size(), b.size(), m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv0::RECEIVE_MEDIA_REQUEST);
    std::string bad = a;
    bad[2] = 0; bad[3] = 1;                                              // length 가 헤더보다 짧다
    EXPECT_FALSE(dec(bad, m));
}
