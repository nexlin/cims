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

// disposition 통지 규격형(TS 24.282 §12.2.1.1) — 대상 = resource-lists entry 하나, 그룹이면 <mcdata-calling-group-id>
TEST(SdsCodec, NotificationSpecForm) {
    std::string conv = mcdata::conversationIdOf("g005"), msg = mcdata::newMessageId();
    mcdata::Body g = mcdata::buildNotification(conv, msg, 2, 1700000002L, "tel:+82500000013", "tel:g005");
    EXPECT_NE(g.body.find("<mcdata-calling-group-id type=\"Normal\"><mcdataURI>tel:g005</mcdataURI></mcdata-calling-group-id>"),
              std::string::npos);
    EXPECT_NE(g.body.find("Content-Type: application/resource-lists+xml\r\nContent-Disposition: recipient-list"), std::string::npos);
    EXPECT_NE(g.body.find("<entry uri=\"tel:+82500000013\"/>"), std::string::npos);
    const size_t info = g.body.find("mcdata-info+xml"), sig = g.body.find("mcdata-signalling"), rl = g.body.find("resource-lists+xml");
    EXPECT_TRUE(info < sig && sig < rl);                        // mcdata-info · SDS NOTIFICATION · resource-lists
    SdsMessage nt;
    ASSERT_TRUE(mcdata::parse(g.contentType, g.body, nt));
    EXPECT_TRUE(nt.notification);
    EXPECT_EQ(nt.notifType, 2);
    EXPECT_EQ(nt.msgId, msg);
    EXPECT_EQ(nt.groupUri, "tel:g005");
    // 1:1 — mcdata-info 없음
    mcdata::Body o = mcdata::buildNotification(conv, msg, 2, 1700000002L, "tel:+82500000013");
    EXPECT_EQ(o.body.find("mcdata-info+xml"), std::string::npos);
    EXPECT_NE(o.body.find("<entry uri=\"tel:+82500000013\"/>"), std::string::npos);
    // 전환기 형식(대상 없음) — SDS NOTIFICATION 한 파트
    mcdata::Body l = mcdata::buildNotification(conv, msg, 2, 1700000002L);
    EXPECT_EQ(l.body.find("resource-lists"), std::string::npos);
    EXPECT_EQ(l.body.find("mcdata-info"), std::string::npos);
}

// 받은 SDS 의 보낸 사용자·그룹 = <mcdata-calling-user-id>·<mcdata-calling-group-id>(§12.2.1.1) — 없으면 From·request-uri
TEST(SdsCodec, CallingIdentitiesFromMcdataInfo) {
    std::string conv = mcdata::conversationIdOf("g005"), msg = mcdata::newMessageId();
    const std::string sig = mcdata::base64Encode(mcdata::sdsSignallingTlv(conv, msg, true, 1700000003L));
    const std::string info = "<?xml version=\"1.0\"?><mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\"><mcdata-Params>"
                             "<request-type>group-sds</request-type>"
                             "<mcdata-request-uri type=\"Normal\"><mcdataURI>tel:+82500000014</mcdataURI></mcdata-request-uri>"
                             "<mcdata-calling-user-id type=\"Normal\"><mcdataURI>tel:+82500000013</mcdataURI></mcdata-calling-user-id>"
                             "<mcdata-calling-group-id type=\"Normal\"><mcdataURI>tel:g005</mcdataURI></mcdata-calling-group-id>"
                             "</mcdata-Params></mcdatainfo>";
    std::string body = "--b\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n" + info +
                       "\r\n--b\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\nContent-Transfer-Encoding: base64\r\n\r\n" + sig +
                       "\r\n--b--\r\n";
    SdsMessage m;
    ASSERT_TRUE(mcdata::parse("multipart/mixed;boundary=b", body, m));
    EXPECT_EQ(m.fromUri, "tel:+82500000013");
    EXPECT_EQ(m.groupUri, "tel:g005");                         // request-uri(수신자)가 아니라 calling-group-id
    // 요소가 없는 옛 발신자 — fromUri 는 비고(호출자가 From), 그룹은 request-uri
    mcdata::Body b = mcdata::buildGroupSds("tel:g001", "x", conv, msg, true, 1700000003L);
    SdsMessage o;
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, o));
    EXPECT_TRUE(o.fromUri.empty());
    EXPECT_EQ(o.groupUri, "tel:g001");
    // 중계된 1:1 통지 — request-uri 는 나(통지 대상)라 그룹으로 읽지 않는다(§12.2.3 14))
    const std::string ntf = mcdata::base64Encode(std::string("\x05\x02", 2) + std::string(5, '\0') + mcdata::hexDecode(conv) + mcdata::hexDecode(msg));
    std::string relay = "--r\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n<mcdatainfo><mcdata-Params>"
                        "<mcdata-request-uri type=\"Normal\"><mcdataURI>tel:+82500000013</mcdataURI></mcdata-request-uri>"
                        "<mcdata-calling-user-id type=\"Normal\"><mcdataURI>tel:+82500000014</mcdataURI></mcdata-calling-user-id>"
                        "</mcdata-Params></mcdatainfo>\r\n--r\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\n"
                        "Content-Transfer-Encoding: base64\r\n\r\n" + ntf + "\r\n--r--\r\n";
    SdsMessage n;
    ASSERT_TRUE(mcdata::parse("multipart/mixed;boundary=r", relay, n));
    EXPECT_TRUE(n.notification);
    EXPECT_EQ(n.fromUri, "tel:+82500000014");
    EXPECT_TRUE(n.groupUri.empty());
}

TEST(SdsCodec, OneToOneConversationIdIsPairSorted) {
    // 쌍을 정렬하므로 **양쪽 단말이 같은 값**을 만든다 — 그러지 않으면 같은 대화가 둘로 갈라진다.
    EXPECT_EQ(mcdata::conversationIdOneToOne("1001", "1002"),
              mcdata::conversationIdOneToOne("1002", "1001"));
    EXPECT_NE(mcdata::conversationIdOneToOne("1001", "1002"),
              mcdata::conversationIdOneToOne("1001", "1003"));
    // 그룹 축과도 겹치지 않는다(접두어가 다르다).
    EXPECT_NE(mcdata::conversationIdOneToOne("1001", "1002"), mcdata::conversationIdOf("1002"));
    // cspsim `conversationIdOneToOne` · Kotlin `McDataCodec.conversationIdOf(a,b)` 와 **같은 값**이어야
    //   상대 단말이 만든 대화와 하나가 된다. 세 구현이 같은 name-UUID 규칙을 쓰는지 값으로 못 박는다.
    EXPECT_EQ(mcdata::conversationIdOneToOne("1001", "1002"), "0b03318bcafc3c6997000599d3549ecc");
}

TEST(SdsCodec, OneToOneSdsRoundTrip) {
    std::string conv = mcdata::conversationIdOneToOne("1001", "1002"), msg = mcdata::newMessageId();
    mcdata::Body b = mcdata::buildOneToOneSds("tel:1002", "안녕 1:1", conv, msg, true, 1700000000L);
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, out));
    EXPECT_EQ(out.convId, conv);
    EXPECT_EQ(out.msgId, msg);
    EXPECT_EQ(out.text, "안녕 1:1");
    EXPECT_FALSE(out.notification);
    // 그룹과 갈리는 자리(TS 24.282 §9.2.2.2.1 2)) — request-type 과 대상 = resource-lists entry(받는 사람). <mcdata-request-uri> 는 싣지 않는다
    EXPECT_NE(b.body.find("<request-type>one-to-one-sds</request-type>"), std::string::npos);
    EXPECT_EQ(b.body.find("group-sds"), std::string::npos);
    EXPECT_EQ(b.body.find("mcdata-request-uri"), std::string::npos);
    EXPECT_NE(b.body.find("Content-Type: application/resource-lists+xml\r\nContent-Disposition: recipient-list"), std::string::npos);
    EXPECT_NE(b.body.find("<entry uri=\"tel:1002\"/>"), std::string::npos);
    // 받는 쪽에서 request-uri 는 나 자신이다 — 그룹으로 오인하지 않는다(내 번호 스레드가 생기지 않게).
    EXPECT_EQ(out.groupUri, "");
}

TEST(SdsCodec, FdSignallingTlvLayout) {
    // cspsim fdSignallingTlv · Android McDataCodec.buildFd 와 같은 바이트(mcdata_messaging.md §4.5)
    std::string conv(32, 'a'), msg(32, 'b');
    FdFile f; f.url = "https://csc:4430/mcdata/fd/0123"; f.name = "현장\"사진\".jpg"; f.size = 1234; f.type = "image/jpeg";
    std::string tlv = mcdata::fdSignallingTlv(conv, msg, f, 0x0102030405L);
    EXPECT_EQ((uint8_t)tlv[0], mcdata::kMsgFdSignalling);
    EXPECT_EQ((uint8_t)tlv[1], 0x01); EXPECT_EQ((uint8_t)tlv[5], 0x05);
    ASSERT_EQ((uint8_t)tlv[38], 0x78);                       // Payload IE (TLV-E)
    size_t l = ((uint8_t)tlv[39] << 8) | (uint8_t)tlv[40];
    EXPECT_EQ(l, 1 + f.url.size());
    EXPECT_EQ((uint8_t)tlv[41], 0x04);                       // FILEURL
    EXPECT_EQ(tlv.substr(42, f.url.size()), f.url);
    size_t m = 41 + l;
    ASSERT_EQ((uint8_t)tlv[m], 0x79);                        // Metadata IE (TLV-E)
    EXPECT_EQ(tlv.substr(m + 3), "name:\"현장사진.jpg\" size:1234 type:image/jpeg");   // 이름의 따옴표는 뺀다
    FdFile noType = f; noType.type.clear();
    EXPECT_NE(mcdata::fdSignallingTlv(conv, msg, noType, 0).find("type:application/octet-stream"), std::string::npos);
}

TEST(SdsCodec, GroupFdRoundTrip) {
    std::string conv = mcdata::conversationIdOf("g001"), msg = mcdata::newMessageId();
    FdFile f; f.url = "https://10.0.0.1:4430/mcdata/fd/0123456789abcdef0123456789abcdef"; f.name = "보고서 1.pdf"; f.size = 52428800; f.type = "application/pdf";
    mcdata::Body b = mcdata::buildGroupFd("tel:g001", f, conv, msg, 1700000000L);
    EXPECT_NE(b.body.find("<request-type>group-fd</request-type>"), std::string::npos);
    EXPECT_EQ(b.body.find(mcdata::kCtPayload), std::string::npos);      // DATA PAYLOAD 파트 없음 — 두 파트
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, out));
    EXPECT_TRUE(out.fd);
    EXPECT_EQ(out.groupUri, "tel:g001");
    EXPECT_EQ(out.convId, conv); EXPECT_EQ(out.msgId, msg); EXPECT_EQ(out.timeSec, 1700000000L);
    EXPECT_EQ(out.fileUrl, f.url); EXPECT_EQ(out.fileName, f.name); EXPECT_EQ(out.fileSize, f.size); EXPECT_EQ(out.fileType, f.type);
    EXPECT_EQ(out.text, "");
}

TEST(SdsCodec, OneToOneFdHasNoGroupUri) {
    std::string conv = mcdata::conversationIdOneToOne("1001", "1002"), msg = mcdata::newMessageId();
    FdFile f; f.url = "https://csc/mcdata/fd/ab"; f.name = "a.txt"; f.size = 3; f.type = "text/plain";
    mcdata::Body b = mcdata::buildOneToOneFd("tel:1002", f, conv, msg, 1700000000L);
    EXPECT_NE(b.body.find("<request-type>one-to-one-fd</request-type>"), std::string::npos);
    EXPECT_EQ(b.body.find("mcdata-request-uri"), std::string::npos);                  // 대상 = resource-lists(§10.2.4.2.1 2))
    EXPECT_NE(b.body.find("<entry uri=\"tel:1002\"/>"), std::string::npos);
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, out));
    EXPECT_TRUE(out.fd);
    EXPECT_EQ(out.groupUri, "");
    EXPECT_EQ(out.fileName, "a.txt"); EXPECT_EQ(out.fileSize, 3);
}

TEST(SdsCodec, FdParserSkipsOptionalIesAndOldSenderIsGroup) {
    // 선택 IE(§15.1.3) — FD disposition 0x9x·mandatory download 0xAx(TV 1)·InReplyTo 0x21(TV 17)·Application ID 0x22(TV 2) 가
    //   Payload 앞에 와도 FILEURL·Metadata 를 읽는다. request-type 이 없는 옛 발신자의 request-uri 는 그룹으로 본다.
    FdFile f; f.url = "https://csc/mcdata/fd/cd"; f.name = "x.bin"; f.size = 9; f.type = "application/octet-stream";
    std::string tlv = mcdata::fdSignallingTlv(std::string(32, '1'), std::string(32, '2'), f, 1);
    tlv.insert(38, std::string("\x91\xA1\x21", 3) + std::string(16, '\x07') + std::string("\x22\x05", 2));
    std::string body = "--b\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n"
                       "<mcdatainfo><mcdata-Params><mcdata-request-uri><mcdataURI>tel:g009</mcdataURI></mcdata-request-uri></mcdata-Params></mcdatainfo>\r\n"
                       "--b\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\nContent-Transfer-Encoding: base64\r\n\r\n" +
                       mcdata::base64Encode(tlv) + "\r\n--b--\r\n";
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse("multipart/mixed;boundary=b", body, out));
    EXPECT_TRUE(out.fd);
    EXPECT_EQ(out.fileUrl, f.url); EXPECT_EQ(out.fileName, "x.bin"); EXPECT_EQ(out.fileSize, 9);
    EXPECT_EQ(out.groupUri, "tel:g009");
}

TEST(McpttXml, InfoBuildParseAndBareId) {
    std::string x = mcptt::mcpttInfo("prearranged", "tel:g001", "tel:+82500000001", "tel:g001", 1, 0);
    EXPECT_NE(x.find("<session-type>prearranged</session-type>"), std::string::npos);
    EXPECT_NE(x.find("<emergency-ind type=\"Normal\"><mcpttBoolean>true</mcpttBoolean></emergency-ind>"), std::string::npos);   // Annex F.1 contentType
    EXPECT_NE(x.find("<mcptt-request-uri type=\"Normal\"><mcpttURI>tel:g001</mcpttURI></mcptt-request-uri>"), std::string::npos);
    EXPECT_EQ(x.find("imminentperil"), std::string::npos);
    std::string whole = "INVITE sip:x SIP/2.0\r\nContent-Type: multipart/mixed;boundary=b\r\n\r\n--b\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n\r\n" + x +
                        "\r\n--b\r\nContent-Type: application/sdp\r\n\r\nm=application 5001 UDP MCPTT\r\na=fmtp:MCPTT mc_queueing;mc_no_floor_ctrl\r\n--b--";
    McpttInfo mi = mcptt::parseMcpttInfo(whole);
    EXPECT_TRUE(mi.present);
    EXPECT_EQ(mi.sessionType, "prearranged");
    EXPECT_EQ(mi.callingUserId, "tel:+82500000001");
    EXPECT_TRUE(mi.emergency);
    EXPECT_FALSE(mi.privateCall);
    EXPECT_FALSE(mi.noFloorCtrl);            // floor 없음 = 개별 호 offer 에 m=application 이 없을 때만(TS 24.379 §11.1.2.2) — fmtp 는 보지 않는다
    EXPECT_FALSE(mcptt::parseMcpttInfo("INVITE sip:x SIP/2.0\r\n\r\nv=0").present);
    EXPECT_EQ(mcptt::bareId("<sip:g001@ims.example.org>;tag=1"), "g001");
    EXPECT_EQ(mcptt::bareId("tel:+82500000001"), "+82500000001");
    EXPECT_EQ(mcptt::bareId("\"name\" <sip:+8210@d>"), "+8210");
    std::string aff = mcptt::affiliationCommand("tel:g001", false);
    EXPECT_NE(aff.find("<de-affiliate group=\"tel:g001\"/>"), std::string::npos);
    EXPECT_FALSE(mi.broadcast);
}

// 지시자 순서 = mcptt-ParamsType sequence(Annex F.1): emergency-ind · alert-ind · imminentperil-ind
TEST(McpttXml, IndicatorOrderAndAlert) {
    std::string x = mcptt::mcpttInfo("prearranged", "tel:g001", "tel:+82500000001", "tel:g001", 1, 0, false, -1);
    const size_t e = x.find("<emergency-ind"), a = x.find("<alert-ind type=\"Normal\"><mcpttBoolean>false</mcpttBoolean>");
    ASSERT_NE(e, std::string::npos);
    ASSERT_NE(a, std::string::npos);
    EXPECT_LT(e, a);
    EXPECT_EQ(x.find("imminentperil-ind"), std::string::npos);  // §6.3.3.1.17 — emergency-ind true 에 임박 지시자 없음
    std::string i = mcptt::mcpttInfo("prearranged", "tel:g001", "u", "tel:g001", 0, 1);
    EXPECT_EQ(i.find("alert-ind"), std::string::npos);
    EXPECT_EQ(i.find("emergency-ind"), std::string::npos);
    EXPECT_EQ(mcptt::indicator(x, "alert-ind"), -1);
}

// 일제 통화 = prearranged + <broadcast-ind>true (TS 24.379 §6.2.8.2 · Annex F.1) — session-type 에 broadcast 는 없다
TEST(McpttXml, BroadcastIndicator) {
    std::string x = mcptt::mcpttInfo("prearranged", "tel:g001", "tel:+82500000001", "tel:g001", 0, 0, true);
    EXPECT_NE(x.find("<session-type>prearranged</session-type>"), std::string::npos);
    EXPECT_NE(x.find("<broadcast-ind>true</broadcast-ind>"), std::string::npos);
    EXPECT_EQ(mcptt::mcpttInfo("prearranged", "tel:g001", "u", "tel:g001").find("broadcast-ind"), std::string::npos);
    McpttInfo mi = mcptt::parseMcpttInfo("INVITE sip:x SIP/2.0\r\n\r\n" + x);
    EXPECT_TRUE(mi.present);
    EXPECT_EQ(mi.sessionType, "prearranged");
    EXPECT_TRUE(mi.broadcast);
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

// SDS SIGNALLING PAYLOAD 의 선택 IE(TS 24.282 표 15.1.2.1-1 — 0x21 → 0x22 → 0x8- → 0x7D → 0x7E → 0x51 → 0x53)와 응용 대상 판정(§9.2.1.2 7)·8))
TEST(SdsCodec, OptionalIesAndApplicationTargetedMessages) {
    const std::string conv = mcdata::conversationIdOf("g001"), msg = mcdata::newMessageId();
    mcdata::Body b = mcdata::buildGroupSds("tel:g001", "hi", conv, msg, false, 1700000000L);
    const std::string base = mcdata::sdsSignallingTlv(conv, msg, false, 1700000000L);          // 고정부 38 octet
    auto withSig = [&](const std::string& sig) {
        // 서명 파트(base64 본문)를 바꿔 끼운다 — buildGroupSds 가 실은 것과 같은 인코딩
        std::string body = b.body;
        const std::string was = mcdata::base64Encode(base);
        size_t p = body.find(was);
        EXPECT_NE(p, std::string::npos);
        if (p != std::string::npos) body.replace(p, was.size(), mcdata::base64Encode(sig));
        return body;
    };
    auto tlve = [](int iei, const std::string& v) {
        std::string s; s += (char)iei; s += (char)(v.size() >> 8); s += (char)(v.size() & 0xFF); return s + v;
    };

    // InReplyTo(0x21, TV 17) 뒤의 disposition 요청 — 사용자용
    SdsMessage m; bool app = true;
    ASSERT_TRUE(mcdata::parse(b.contentType, withSig(base + std::string("\x21", 1) + std::string(16, '\x11') + std::string("\x83", 1)), m, app));
    EXPECT_FALSE(app); EXPECT_EQ(m.dispositionReq, 3); EXPECT_EQ(m.text, "hi");

    // Application ID(0x22, TV 2) — 응용 대상. 그 뒤의 disposition 요청도 읽는다(전엔 0x22 에서 멈췄다)
    SdsMessage a; app = false;
    ASSERT_TRUE(mcdata::parse(b.contentType, withSig(base + std::string("\x22\x05", 2) + std::string("\x81", 1)), a, app));
    EXPECT_TRUE(app); EXPECT_EQ(a.dispositionReq, 1);

    // Extended application ID(0x7D, TLV-E) — 응용 대상. User location(0x7E)·Sender ID(0x51)·metadata(0x53)는 건너뛴다
    SdsMessage e; app = false;
    ASSERT_TRUE(mcdata::parse(b.contentType, withSig(base + std::string("\x82", 1) + tlve(0x7D, std::string("\x01", 1) + "app.example")), e, app));
    EXPECT_TRUE(app); EXPECT_EQ(e.dispositionReq, 2);
    SdsMessage u; app = true;
    ASSERT_TRUE(mcdata::parse(b.contentType, withSig(base + std::string("\x81", 1) + tlve(0x7E, "loc") + tlve(0x51, "tel:+821") + tlve(0x53, "meta")), u, app));
    EXPECT_FALSE(app); EXPECT_EQ(u.dispositionReq, 1); EXPECT_EQ(u.text, "hi");

    SdsMessage plain;                                           // 옛 시그니처 — 응용 대상 여부를 묻지 않는 호출자
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, plain));
    EXPECT_EQ(plain.text, "hi");
}

// 그룹 SDS·FD 의 mcdata-info 는 MCData client ID 를 싣는다(TS 24.282 §9.2.2.2.1 3)b)iv) · §10.2.4.2.1 3)b)iii)) — 1:1 은 싣지 않는다
TEST(SdsCodec, GroupRequestsCarryClientId) {
    const std::string cid = "urn:uuid:00000000-0000-4000-8000-000000000001";
    const std::string elem = "<mcdata-client-id type=\"Normal\"><mcdataString>" + cid + "</mcdataString></mcdata-client-id>";
    mcdata::Body b = mcdata::buildGroupSds("tel:g001", "hi", mcdata::conversationIdOf("g001"), mcdata::newMessageId(), false, 1700000000L, cid);
    EXPECT_NE(b.body.find(elem), std::string::npos);
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse(b.contentType, b.body, out));               // 받는 쪽 해석은 그대로
    EXPECT_EQ(out.groupUri, "tel:g001"); EXPECT_EQ(out.text, "hi");
    FdFile f; f.url = "https://c/x"; f.name = "a.jpg"; f.size = 3;
    b = mcdata::buildGroupFd("tel:g001", f, mcdata::conversationIdOf("g001"), mcdata::newMessageId(), 1700000000L, cid);
    EXPECT_NE(b.body.find(elem), std::string::npos);
    b = mcdata::buildGroupSds("tel:g001", "hi", mcdata::conversationIdOf("g001"), mcdata::newMessageId(), false, 1700000000L);
    EXPECT_EQ(b.body.find("mcdata-client-id"), std::string::npos);        // client ID 를 모르면 싣지 않는다
    b = mcdata::buildOneToOneSds("tel:+821", "hi", mcdata::conversationIdOneToOne("+820", "+821"), mcdata::newMessageId(), false, 1700000000L);
    EXPECT_EQ(b.body.find("mcdata-client-id"), std::string::npos);
}

// DATA PAYLOAD 의 Payload IE 여러 개(TS 24.282 §15.2.13 · §9.2.1.2 6)d)) — TEXT·HYPERLINKS 는 온 순서대로 잇는다(전엔 마지막 TEXT 만 남았다)
TEST(SdsCodec, MultiplePayloadsAndHyperlinks) {
    const std::string conv = mcdata::conversationIdOf("g001"), msg = mcdata::newMessageId();
    mcdata::Body b = mcdata::buildGroupSds("tel:g001", "첫 줄", conv, msg, false, 1700000000L);
    auto ie = [](int ctype, const std::string& data) {
        std::string s; s += (char)0x78; s += (char)((data.size() + 1) >> 8); s += (char)((data.size() + 1) & 0xFF); s += (char)ctype; return s + data;
    };
    std::string payload;
    payload += (char)mcdata::kMsgDataPayload; payload += (char)4;                 // payload 수 4
    payload += ie(0x01, "첫 줄") + ie(0x03, "https://example.org/a") + ie(0x02, std::string("\x00\x01", 2)) + ie(0x01, "끝 줄");
    std::string body = b.body;
    const std::string was = mcdata::base64Encode(mcdata::sdsPayloadTlv("첫 줄"));
    size_t p = body.find(was);
    ASSERT_NE(p, std::string::npos);
    body.replace(p, was.size(), mcdata::base64Encode(payload));
    SdsMessage out;
    ASSERT_TRUE(mcdata::parse(b.contentType, body, out));
    EXPECT_EQ(out.text, "첫 줄\nhttps://example.org/a\n끝 줄");                    // BINARY 는 넘긴다
    EXPECT_FALSE(out.fd);
}
