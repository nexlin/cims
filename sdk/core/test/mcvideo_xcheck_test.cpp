// S1-UE-UNIT — CMP 전송 제어 코덱(cmp/PTransmissionCodec.cpp)과 코어 코덱의 교차 검증 (mcvideo.md §5.3·§5.4, 계약 K5).
//  ① 두 생성 헤더(tc_defs.h · PTransmissionDefs.h)의 표가 같다 — 메시지 집합·필드 모양·메시지별 필드 전부
//  ② 코어 참여자 빌더 → CMP ParseTransmissionMessage
//  ③ CMP BuildTransmissionMessage(서버 메시지) → 코어 decode
// CMP 코덱은 pasf 에 기대지 않아 Linux·Windows 모두 링크한다.
#include <gtest/gtest.h>

#include "PTransmissionCodec.h"
#include "mcvideo/tc_codec.h"

namespace mv = cimsue::mcvideo;
using mv::AppName;

static bool dec(const char* buf, int n, mv::Message& m) { return mv::decode((const uint8_t*)buf, (size_t)n, m); }
static bool cmpParse(const std::string& pkt, ParsedTransmission& p) {
    return ParseTransmissionMessage(pkt.data(), (int)pkt.size(), p);
}

TEST(McvXCheck, DefsTablesIdentical) {
    const AppName apps[] = {AppName::MCV0, AppName::MCV1, AppName::MCV2};
    for (int a = 0; a < 3; ++a) {
        for (int st = 0; st < 32; ++st) {
            EXPECT_EQ(mv::knownMessage(apps[a], (uint8_t)st), McvKnownMessage(a, st)) << a << "/" << st;
            EXPECT_EQ(mv::allowedFields(apps[a], (uint8_t)st), McvAllowedFields(a, st)) << a << "/" << st;
            EXPECT_STREQ(mv::msgName(apps[a], (uint8_t)st), McvMessageName(a, st)) << a << "/" << st;
        }
    }
    for (int id = 0; id < 256; ++id) {
        EXPECT_EQ(mv::fixedLength(mv::fieldKind((uint8_t)id)), McvFixedLength(McvFieldKindOf(id))) << id;
        EXPECT_EQ((int)mv::fieldKind((uint8_t)id) == (int)mv::FieldKind::Unknown, McvFieldKindOf(id) == TFK_UNKNOWN) << id;
    }
    EXPECT_EQ(mv::kAckRequiredBit, MCV_ACK_REQ_BIT);
    EXPECT_EQ(mv::kRtcpPtApp, MCV_RTCP_PT_APP);
    EXPECT_EQ((int)mv::Mcv1::TRANSMISSION_GRANTED, (int)MCV1_TRANSMISSION_GRANTED);
    EXPECT_EQ((int)mv::Mcv2::TRANSMISSION_CONTROL_ACK, (int)MCV2_TRANSMISSION_CONTROL_ACK);
    EXPECT_EQ((int)mv::Field::VIDEO_SSRC, (int)TF_VIDEO_SSRC);
    EXPECT_EQ((int)mv::indicator::EMERGENCY, (int)TI_EMERGENCY);
    EXPECT_EQ((int)mv::RevokeCause::PREEMPTED, (int)TC_REVOKE_PREEMPTED);
    EXPECT_EQ((int)mv::ReceiveRejectCause::MAX_STREAMS, (int)TC_RECV_REJECT_MAX_STREAMS);
    EXPECT_EQ(mv::timer::T3_MS, MCV_T3_MS);
    EXPECT_EQ(mv::timer::C9, MCV_C9);
}

TEST(McvXCheck, CoreParticipantMessagesParsedByCmp) {
    ParsedTransmission p;
    ASSERT_TRUE(cmpParse(mv::transmissionRequest(0xCAFEBABE, 9, mv::indicator::IMMINENT_PERIL), p));
    EXPECT_EQ(p.app, MCV_APP_0);
    EXPECT_EQ(p.op(), MCV0_TRANSMISSION_REQUEST);
    EXPECT_EQ(p.ssrc, 0xCAFEBABEu);
    EXPECT_EQ(p.u8(TF_TRANSMISSION_PRIORITY), 9);
    EXPECT_EQ(p.u16(TF_TRANSMISSION_INDICATOR), (int)TI_IMMINENT_PERIL);

    ASSERT_TRUE(cmpParse(mv::receiveMediaRequest(5, "tel:+82500000013", 0x1111, 0x2222, 4), p));
    EXPECT_EQ(p.op(), MCV0_RECEIVE_MEDIA_REQUEST);
    EXPECT_EQ(p.str(TF_TRANSMITTING_USER_ID), "tel:+82500000013");
    EXPECT_EQ(p.ssrcOf(TF_AUDIO_SSRC), 0x1111u);
    EXPECT_EQ(p.ssrcOf(TF_VIDEO_SSRC), 0x2222u);
    EXPECT_EQ(p.u8(TF_RECEPTION_PRIORITY), 4);

    ASSERT_TRUE(cmpParse(mv::transmissionEndRequest(5), p));
    EXPECT_EQ(p.app, MCV_APP_2);
    EXPECT_EQ(p.op(), MCV2_TRANSMISSION_END_REQUEST);
    ASSERT_TRUE(cmpParse(mv::queuePositionRequest(5), p));
    EXPECT_EQ(p.op(), MCV0_QUEUE_POSITION_REQUEST);
    ASSERT_TRUE(cmpParse(mv::mediaReceptionEndRequest(5, "tel:+82500000013", 0x1111, 0x2222, (int)TI_NORMAL), p));
    EXPECT_EQ(p.op(), MCV2_MEDIA_RECEPTION_END_REQUEST);
    EXPECT_EQ(p.ssrcOf(TF_VIDEO_SSRC), 0x2222u);
    EXPECT_EQ(p.u16(TF_TRANSMISSION_INDICATOR), (int)TI_NORMAL);
    ASSERT_TRUE(cmpParse(mv::transmissionEndResponse(5), p));
    EXPECT_EQ(p.op(), MCV2_TRANSMISSION_END_RESPONSE);
    ASSERT_TRUE(cmpParse(mv::mediaReceptionEndResponse(5, "tel:+82500000013", 0x1111, 0x2222), p));
    EXPECT_EQ(p.op(), MCV2_MEDIA_RECEPTION_END_RESPONSE);

    ASSERT_TRUE(cmpParse(mv::ackOf(5, AppName::MCV1, (uint8_t)(MCV1_MEDIA_TRANSMISSION_NOTIFICATION | MCV_ACK_REQ_BIT)), p));
    EXPECT_EQ(p.app, MCV_APP_2);
    EXPECT_EQ(p.op(), MCV2_TRANSMISSION_CONTROL_ACK);
    EXPECT_FALSE(p.ackRequired());
    EXPECT_EQ(p.u16(TF_SOURCE), (int)TC_SRC_PARTICIPANT);
    EXPECT_EQ(p.messageName(), MCV_NAME_1);
    EXPECT_EQ(p.u8(TF_MSG_TYPE), (int)MCV1_MEDIA_TRANSMISSION_NOTIFICATION);
}

TEST(McvXCheck, CmpServerMessagesDecodedByCore) {
    char buf[512];
    mv::Message m;
    std::vector<McvTlv> f;

    // Transmission Granted(ack 요구) — Duration · 송출자가 RTP 에 쓸 Audio/Video SSRC(§6.2.4.4.6 2) · 우선순위 · 지시자
    f = {McvTlv(TF_DURATION, McvU16(60)), McvTlv(TF_AUDIO_SSRC, McvSsrc(0xA0000001)),
         McvTlv(TF_VIDEO_SSRC, McvSsrc(0xB0000001)), McvTlv(TF_TRANSMISSION_PRIORITY, McvU8(3)),
         McvTlv(TF_TRANSMISSION_INDICATOR, McvU16(TI_NORMAL))};
    int n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_TRANSMISSION_GRANTED | MCV_ACK_REQ_BIT, 0x01, f);
    ASSERT_GT(n, 0);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.app, AppName::MCV1);
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv1::TRANSMISSION_GRANTED);
    EXPECT_TRUE(m.ackRequired);
    EXPECT_EQ(m.durationSec(), 60);
    EXPECT_EQ(m.audioSsrc(), 0xA0000001u);
    EXPECT_EQ(m.videoSsrc(), 0xB0000001u);
    EXPECT_EQ(m.priority(), 3);
    EXPECT_EQ(m.indicator(), 0x8000);

    // Transmission Rejected — 원인 #1 + phrase
    f = {McvTlv(TF_REJECT_CAUSE, McvCause(TC_REJECT_TRANSMISSION_LIMIT, "max 2"))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_TRANSMISSION_REJECTED, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv1::TRANSMISSION_REJECTED);
    EXPECT_EQ(m.cause(), 1);
    EXPECT_EQ(m.causePhrase(), "max 2");

    // Transmission Revoked — 원인 #4(pre-empted)
    f = {McvTlv(TF_REJECT_CAUSE, McvCause(TC_REVOKE_PREEMPTED))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_TRANSMISSION_REVOKED, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.cause(), (int)mv::RevokeCause::PREEMPTED);
    EXPECT_TRUE(m.causePhrase().empty());

    // Queue Position Info
    f = {McvTlv(TF_QUEUE_INFO, McvQueueInfo(2, 7))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_QUEUE_POSITION_INFO, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.queuePosition(), 2);
    EXPECT_EQ(m.queuePriority(), 7);

    // Media Transmission Notification — manual 수신(§9.2.3.22 '1')
    f = {McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000014"), McvTlv(TF_AUDIO_SSRC, McvSsrc(0xA0000002)),
         McvTlv(TF_VIDEO_SSRC, McvSsrc(0xB0000002)), McvTlv(TF_PERMISSION, McvU16(TC_PERM_ALLOWED)),
         McvTlv(TF_FUNCTIONAL_ALIAS, "sip:fa-camera1@ptt.cims.example.kr"),
         McvTlv(TF_RECEPTION_MODE, McvU16(TC_RECEPTION_MANUAL))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_MEDIA_TRANSMISSION_NOTIFICATION | MCV_ACK_REQ_BIT, 0x01, f);
    ASSERT_GT(n, 0);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv1::MEDIA_TRANSMISSION_NOTIFICATION);
    EXPECT_EQ(m.transmittingUserId(), "tel:+82500000014");
    EXPECT_EQ(m.audioSsrc(), 0xA0000002u);
    EXPECT_EQ(m.videoSsrc(), 0xB0000002u);
    EXPECT_EQ(m.permission(), 1);
    EXPECT_EQ(m.functionalAlias(), "sip:fa-camera1@ptt.cims.example.kr");
    EXPECT_EQ(m.receptionMode(), (int)mv::ReceptionMode::MANUAL);

    // Receive Media Response — 허가 / 거절 #7(동시 수신 상한 — C9)
    f = {McvTlv(TF_RESULT, McvU16(TC_RESULT_GRANTED)), McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000014"),
         McvTlv(TF_AUDIO_SSRC, McvSsrc(0xA0000002)), McvTlv(TF_VIDEO_SSRC, McvSsrc(0xB0000002))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.result(), (int)mv::ReceiveResult::GRANTED);
    EXPECT_EQ(m.videoSsrc(), 0xB0000002u);
    f = {McvTlv(TF_RESULT, McvU16(TC_RESULT_REJECTED)), McvTlv(TF_REJECT_CAUSE, McvCause(TC_RECV_REJECT_MAX_STREAMS)),
         McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000014")};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_RECEIVE_MEDIA_RESPONSE, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.result(), 0);
    EXPECT_EQ(m.cause(), 7);
    EXPECT_STREQ(mv::receiveRejectCauseText(m.cause()), "Max no of simultaneous stream to receive is reached");

    // Media Reception Notification(송출자에게 — 받는 사람) · Transmission End Notify · Transmission Idle
    f = {McvTlv(TF_USER_ID, "tel:+82500000015"), McvTlv(TF_AUDIO_SSRC, McvSsrc(0xA0000001)),
         McvTlv(TF_VIDEO_SSRC, McvSsrc(0xB0000001))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_MEDIA_RECEPTION_NOTIFICATION, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.userId(), "tel:+82500000015");
    f = {McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000014"), McvTlv(TF_AUDIO_SSRC, McvSsrc(0xA0000002)),
         McvTlv(TF_VIDEO_SSRC, McvSsrc(0xB0000002))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_TRANSMISSION_END_NOTIFY, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv1::TRANSMISSION_END_NOTIFY);
    EXPECT_EQ(m.transmittingUserId(), "tel:+82500000014");
    f = {McvTlv(TF_MSG_SEQ, McvU16(65535)), McvTlv(TF_TRANSMISSION_INDICATOR, McvU16(TI_NORMAL))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_TRANSMISSION_IDLE, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.msgSeq(), 65535);

    // 서버 발 MCV2 — Transmission End Request(원인 #5 terminate) · Media Reception End Request · Ack(Source = controlling)
    f = {McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000013"), McvTlv(TF_REJECT_CAUSE, McvCause(TC_REVOKE_TERMINATE_STREAM))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_2, MCV2_TRANSMISSION_END_REQUEST, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.app, AppName::MCV2);
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::TRANSMISSION_END_REQUEST);
    EXPECT_EQ(m.cause(), (int)mv::RevokeCause::TERMINATE_STREAM);
    f = {McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000014"), McvTlv(TF_AUDIO_SSRC, McvSsrc(0xA0000002)),
         McvTlv(TF_VIDEO_SSRC, McvSsrc(0xB0000002))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_2, MCV2_MEDIA_RECEPTION_END_REQUEST | MCV_ACK_REQ_BIT, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.op, (uint8_t)mv::Mcv2::MEDIA_RECEPTION_END_REQUEST);
    EXPECT_TRUE(m.ackRequired);
    f = {McvTlv(TF_SOURCE, McvU16(TC_SRC_CONTROLLING)), McvTlv(TF_MESSAGE_NAME, McvName(MCV_NAME_0)),
         McvTlv(TF_MSG_TYPE, McvU8(MCV0_RECEIVE_MEDIA_REQUEST))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_2, MCV2_TRANSMISSION_CONTROL_ACK, 0x01, f);
    ASSERT_TRUE(dec(buf, n, m));
    EXPECT_EQ(m.source(), (int)mv::Source::CONTROLLING);
    EXPECT_EQ(m.messageName(), "MCV0");
    EXPECT_EQ(m.messageType(), (int)mv::Mcv0::RECEIVE_MEDIA_REQUEST);
}

// 두 코덱이 같은 바이트를 낸다 — 같은 메시지를 양쪽에서 만들어 비교
TEST(McvXCheck, SameBytesBothEnds) {
    char buf[256];
    std::vector<McvTlv> f = {McvTlv(TF_TRANSMITTING_USER_ID, "tel:+82500000013"), McvTlv(TF_AUDIO_SSRC, McvSsrc(1)),
                             McvTlv(TF_VIDEO_SSRC, McvSsrc(2)), McvTlv(TF_RECEPTION_PRIORITY, McvU8(6))};
    int n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_0, MCV0_RECEIVE_MEDIA_REQUEST, 0x77, f);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, n), mv::receiveMediaRequest(0x77, "tel:+82500000013", 1, 2, 6));
    f = {McvTlv(TF_SOURCE, McvU16(TC_SRC_PARTICIPANT)), McvTlv(TF_MESSAGE_NAME, McvName(MCV_NAME_1)),
         McvTlv(TF_MSG_TYPE, McvU8(MCV1_TRANSMISSION_GRANTED))};
    n = BuildTransmissionMessage(buf, sizeof buf, MCV_APP_2, MCV2_TRANSMISSION_CONTROL_ACK, 0x77, f);
    EXPECT_EQ(std::string(buf, n), mv::ackOf(0x77, AppName::MCV1, (uint8_t)(MCV1_TRANSMISSION_GRANTED | MCV_ACK_REQ_BIT)));
}

// CMP 쪽도 같은 검사(§9.1.4·표 밖 필드 빌드 거절)
TEST(McvXCheck, CmpRejectsWhatCoreRejects) {
    char buf[128];
    std::vector<McvTlv> f = {McvTlv(TF_AUDIO_SSRC, McvSsrc(1))};                       // Transmission Request 표 밖
    EXPECT_EQ(BuildTransmissionMessage(buf, sizeof buf, MCV_APP_0, MCV0_TRANSMISSION_REQUEST, 1, f), 0);
    f = {McvTlv(TF_DURATION, std::string(4, '\0'))};                                 // Length 4 — u16 모양 아님
    EXPECT_EQ(BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, MCV1_TRANSMISSION_GRANTED, 1, f), 0);
    EXPECT_EQ(BuildTransmissionMessage(buf, sizeof buf, MCV_APP_1, 0x09, 1, {}), 0);     // void
    EXPECT_EQ(BuildTransmissionMessage(buf, 8, MCV_APP_1, MCV1_TRANSMISSION_IDLE, 1, {}), 0);   // 버퍼 부족

    ParsedTransmission p;
    std::string unknown = mv::transmissionRequest(1);
    unknown[0] = (char)(0x80 | 0x05);                                                 // MCV0 void
    EXPECT_FALSE(cmpParse(unknown, p));
    std::string floor = mv::transmissionRequest(1);
    floor.replace(8, 4, "MCPT");
    EXPECT_FALSE(cmpParse(floor, p));
    // 코어가 받아들이는 입력을 CMP 도 같게 읽는다 — 표 밖 필드(Audio SSRC in Transmission Request)는 양쪽 다 버린다
    std::string pkt = mv::transmissionRequest(1, 4);
    std::string extra = std::string("\x0E\x06\x00\x00\x00\x09\x00\x00", 8);
    pkt += extra;
    pkt[3] = (char)(pkt.size() / 4 - 1);
    ASSERT_TRUE(cmpParse(pkt, p));
    EXPECT_FALSE(p.has(TF_AUDIO_SSRC));
    EXPECT_EQ(p.u8(TF_TRANSMISSION_PRIORITY), 4);
    mv::Message m;
    ASSERT_TRUE(mv::decode((const uint8_t*)pkt.data(), pkt.size(), m));
    EXPECT_FALSE(m.has(mv::Field::AUDIO_SSRC));
}
