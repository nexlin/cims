// libcimsue 단위시험 — MCData SDS 코덱 + MCPTT XML (S1-UE-UNIT)
#include <gtest/gtest.h>

#include "../src/mcdata/sds_codec.h"
#include "../src/mcptt/mcptt_xml.h"

using namespace cimsue;

TEST(SdsCodec, ConversationIdMatchesJavaNameUuid) {
    // Java UUID.nameUUIDFromBytes("cims-mcdata:g001") — Android 단말과 같은 대화 ID 여야 한다.
    EXPECT_EQ(mcdata::conversationIdOf("g001"), "5a78397f798b38f295168af7f9d22be3");
    EXPECT_EQ(mcdata::conversationIdOf("g002"), "cab453e80cb837e0a3a4fa274ca4d1c0");
    std::string id = mcdata::newMessageId();
    EXPECT_EQ(id.size(), 32u);
    EXPECT_EQ(id[12], '4');                         // UUID v4
}

TEST(SdsCodec, Base64AndHex) {
    EXPECT_EQ(mcdata::base64Encode("Man"), "TWFu");
    EXPECT_EQ(mcdata::base64Encode("Ma"), "TWE=");
    EXPECT_EQ(mcdata::base64Encode("M"), "TQ==");
    EXPECT_EQ(mcdata::base64Decode("TWFu"), "Man");
    EXPECT_EQ(mcdata::base64Decode("TW\r\nE="), "Ma");
    EXPECT_EQ(mcdata::hexEncode("\x01\xab"), "01ab");
    EXPECT_EQ(mcdata::hexDecode("01ab"), std::string("\x01\xab", 2));
}

TEST(SdsCodec, SignallingTlvLayout) {
    std::string conv(32, 'a'), msg(32, 'b');
    std::string tlv = mcdata::sdsSignallingTlv(conv, msg, true, 0x0102030405L);
    ASSERT_EQ(tlv.size(), 39u);                     // type(1)+datetime(5)+conv(16)+msg(16)+disposition TV(1)
    EXPECT_EQ((uint8_t)tlv[0], mcdata::kMsgSdsSignalling);
    EXPECT_EQ((uint8_t)tlv[1], 0x01); EXPECT_EQ((uint8_t)tlv[5], 0x05);
    EXPECT_EQ((uint8_t)tlv[38], 0x80 | mcdata::kDispReqDelivery);
    EXPECT_EQ(mcdata::sdsSignallingTlv(conv, msg, false, 0).size(), 38u);
    std::string pl = mcdata::sdsPayloadTlv("hi");
    ASSERT_EQ(pl.size(), 2u + 3 + 1 + 2);
    EXPECT_EQ((uint8_t)pl[0], mcdata::kMsgDataPayload);
    EXPECT_EQ((uint8_t)pl[2], 0x78);
    EXPECT_EQ((uint8_t)pl[4], 3);                   // len = content-type(1) + "hi"
    EXPECT_EQ((uint8_t)pl[5], 0x01);                // TEXT
}

TEST(SdsCodec, GroupSdsRoundTrip) {
    std::string conv = mcdata::conversationIdOf("g001"), msg = mcdata::newMessageId();
    mcdata::Body b = mcdata::buildGroupSds("tel:g001", "안녕 group", conv, msg, true, 1700000000L);
    EXPECT_EQ(b.contentType.rfind("multipart/mixed;boundary=", 0), 0u);
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, out));
    EXPECT_EQ(out.groupUri, "tel:g001");
    EXPECT_EQ(out.convId, conv);
    EXPECT_EQ(out.msgId, msg);
    EXPECT_EQ(out.timeSec, 1700000000L);
    EXPECT_EQ(out.dispositionReq, mcdata::kDispReqDelivery);
    EXPECT_EQ(out.text, "안녕 group");
    EXPECT_FALSE(out.notification);

    // Content-Type 에 boundary 가 빠진 경우(pjsua2 msgBody 경로) — 본문 첫 줄 폴백
    SdsMessage out2;
    ASSERT_TRUE(mcdata::parse("multipart/mixed", b.body, out2));
    EXPECT_EQ(out2.text, "안녕 group");

    mcdata::Body n = mcdata::buildNotification(conv, msg, 2, 1700000001L);
    SdsMessage nt;
    ASSERT_TRUE(mcdata::parse(n.contentType, n.body, nt));
    EXPECT_TRUE(nt.notification);
    EXPECT_EQ(nt.notifType, 2);
    EXPECT_EQ(nt.msgId, msg);
    SdsMessage none;
    EXPECT_FALSE(mcdata::parse("text/plain", "hello", none));
}

TEST(McpttXml, InfoBuildParseAndBareId) {
    std::string x = mcptt::mcpttInfo("prearranged", "tel:g001", "tel:+82500000001", "tel:g001", 1, 0);
    EXPECT_NE(x.find("<session-type>prearranged</session-type>"), std::string::npos);
    EXPECT_NE(x.find("<emergency-ind>true</emergency-ind>"), std::string::npos);
    EXPECT_EQ(x.find("imminentperil"), std::string::npos);
    std::string whole = "INVITE sip:x SIP/2.0\r\nContent-Type: multipart/mixed;boundary=b\r\n\r\n--b\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n" + x +
                        "\r\n--b\r\nContent-Type: application/sdp\r\n\r\nm=application 5001 UDP MCPTT\r\na=fmtp:MCPTT mc_queueing;mc_no_floor_ctrl\r\n--b--";
    McpttInfo mi = mcptt::parseMcpttInfo(whole);
    EXPECT_TRUE(mi.present);
    EXPECT_EQ(mi.sessionType, "prearranged");
    EXPECT_EQ(mi.callingUserId, "tel:+82500000001");
    EXPECT_TRUE(mi.emergency);
    EXPECT_FALSE(mi.privateCall);
    EXPECT_TRUE(mi.noFloorCtrl);
    EXPECT_FALSE(mcptt::parseMcpttInfo("INVITE sip:x SIP/2.0\r\n\r\nv=0").present);
    EXPECT_EQ(mcptt::bareId("<sip:g001@ims.example.org>;tag=1"), "g001");
    EXPECT_EQ(mcptt::bareId("tel:+82500000001"), "+82500000001");
    EXPECT_EQ(mcptt::bareId("\"name\" <sip:+8210@d>"), "+8210");
    std::string aff = mcptt::affiliationCommand("tel:g001", false);
    EXPECT_NE(aff.find("<de-affiliate group=\"tel:g001\"/>"), std::string::npos);
}

TEST(McpttXml, ConferenceInfo) {
    std::string xml = "<?xml version=\"1.0\"?><conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\" state=\"full\" version=\"3\">"
                      "<users><user entity=\"tel:+82500000001\"><endpoint entity=\"e1\"><status>connected</status></endpoint></user>"
                      "<user entity=\"sip:+82500000002@d\"><endpoint><status>disconnected</status></endpoint></user></users></conference-info>";
    std::vector<RosterEntry> users; bool full = false;
    ASSERT_TRUE(mcptt::parseConferenceInfo(xml, users, full));
    EXPECT_TRUE(full);
    ASSERT_EQ(users.size(), 2u);
    EXPECT_EQ(users[0].uri, "tel:+82500000001"); EXPECT_EQ(users[0].status, "connected");
    EXPECT_EQ(users[1].status, "disconnected");
    EXPECT_FALSE(mcptt::parseConferenceInfo("<other/>", users, full));
}
