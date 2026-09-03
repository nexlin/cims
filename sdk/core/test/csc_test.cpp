// libcimsue 단위시험 — CSC 프로비저닝 파서·dialog-info·SSRC 라벨 (S1-UE-UNIT)
#include <gtest/gtest.h>

#include "../src/mcptt/mcptt_xml.h"
#include "cimsue/csc.h"

using namespace cimsue;

static const char* kProfile = R"({
  "user": { "displayName": "테스트001", "loginId": "test001" },
  "csc": { "host": "121.161.164.48", "port": 4430 },
  "countryCode": "82",
  "services": [
    { "kind": "volte",
      "sip": { "host": "121.161.164.48", "port": 5060, "transport": "UDP",
               "transports": [ { "transport": "UDP", "port": 5060 }, { "transport": "TLS", "port": 5061 } ],
               "default": "UDP", "domain": "ims.example.org", "mediaSecurity": "optional", "security": ["tls"] },
      "account": { "msisdn": "+821300000001", "imsi": "45033821300000001", "authId": "", "sipHa1": "0123456789abcdef0123456789abcdef", "sipPassword": null } },
    { "kind": "ptt",
      "sip": { "host": "121.161.164.48", "port": 5061, "transport": "TLS", "transports": [ { "transport": "TLS", "port": 5061 } ],
               "default": "TLS", "enforced": true, "domain": "ptt.example.org" },
      "account": { "msisdn": "+82500000001", "imsi": "4503382500000001", "sipHa1": null, "mcpttId": "tel:+82500000001",
                   "authScheme": "aka", "aka": { "k": "00112233", "opc": "44556677", "amf": "8000" } } }
  ],
  "dispatch": { "groupId": "dg-1", "groupName": "관제1", "pilotId": "+8215001000", "monitorScope": "all", "pttListen": "listed", "listenVisibility": "hidden" }
})";

TEST(Csc, ParseProfile) {
    Profile p;
    std::string err;
    ASSERT_TRUE(CscClient::parseProfile(kProfile, p, &err)) << err;
    EXPECT_EQ(p.displayName, "테스트001");
    EXPECT_EQ(p.countryCode, "82");
    EXPECT_EQ(p.cscPort, 4430);
    ASSERT_EQ(p.services.size(), 2u);
    const ServiceProfile* v = p.service("volte");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->sipPort, 5060);
    EXPECT_EQ(v->transport, Transport::UDP);
    ASSERT_EQ(v->transports.size(), 2u);
    EXPECT_EQ(v->transports[1].transport, Transport::TLS);
    EXPECT_EQ(v->mediaSecurity, MediaSecurity::Optional);
    ASSERT_EQ(v->secMechanisms.size(), 1u);
    EXPECT_EQ(v->sipHa1, "0123456789abcdef0123456789abcdef");
    AccountConfig a = v->toAccount();
    EXPECT_EQ(a.digestUsername(), "45033821300000001@ims.example.org");
    EXPECT_TRUE(a.isComplete());
    const ServiceProfile* t = p.service("ptt");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->enforced);
    EXPECT_EQ(t->transport, Transport::TLS);
    EXPECT_EQ(t->sipPort, 5061);
    EXPECT_EQ(t->mcpttId, "tel:+82500000001");
    EXPECT_EQ(t->authScheme, AuthScheme::Aka);
    EXPECT_EQ(t->akaK, "00112233");
    EXPECT_EQ(t->toAccount().effectiveMcpttId(), "tel:+82500000001");
    EXPECT_TRUE(t->toAccount().isComplete());                  // AKA K 로 완성
    EXPECT_TRUE(p.dispatch.present);
    EXPECT_EQ(p.dispatch.groupId, "dg-1");
    EXPECT_EQ(p.dispatch.pilotId, "+8215001000");
    EXPECT_EQ(p.dispatch.monitorScope, "all");
    Profile none;
    ASSERT_TRUE(CscClient::parseProfile(R"({"services":[]})", none));
    EXPECT_FALSE(none.dispatch.present);
    EXPECT_FALSE(CscClient::parseProfile("not json", none));
}

TEST(DialogInfo, ParseAndJoinHeader) {
    std::string xml = R"(<?xml version="1.0"?>
<dialog-info xmlns="urn:ietf:params:xml:ns:dialog-info" version="2" state="full" entity="sip:+821300000002@ims.example.org">
 <dialog id="d1" call-id="abc-123@10.0.0.1" local-tag="LT" remote-tag="RT" direction="recipient">
  <state>confirmed</state>
  <remote><identity>sip:+821300000001@ims.example.org</identity></remote>
 </dialog>
</dialog-info>)";
    std::vector<DialogInfo> dl;
    ASSERT_TRUE(mcptt::parseDialogInfo(xml, dl));
    ASSERT_EQ(dl.size(), 1u);
    EXPECT_EQ(dl[0].watched, "sip:+821300000002@ims.example.org");
    EXPECT_TRUE(dl[0].full);
    EXPECT_EQ(dl[0].callId, "abc-123@10.0.0.1");
    EXPECT_EQ(dl[0].state, "confirmed");
    EXPECT_EQ(dl[0].direction, "recipient");
    EXPECT_EQ(dl[0].remoteIdentity, "sip:+821300000001@ims.example.org");
    EXPECT_EQ(dl[0].joinHeader(), "abc-123@10.0.0.1;to-tag=RT;from-tag=LT");   // cspsim/CSP 규약
    std::vector<DialogInfo> empty;
    ASSERT_TRUE(mcptt::parseDialogInfo(R"(<dialog-info entity="sip:x" state="full" version="1"/>)", empty));
    EXPECT_TRUE(empty.empty());
    EXPECT_FALSE(mcptt::parseDialogInfo("<other/>", empty));
}

TEST(SsrcLabels, ParseFromSdp) {
    std::string sdp = "v=0\r\nm=audio 50152 RTP/AVP 99\r\na=sendonly\r\na=ssrc:1111 label:caller\r\na=ssrc:2222 label:callee\r\na=ssrc:1111 cname:x\r\n";
    auto s = mcptt::sdpSsrcLabels(sdp);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].ssrc, 1111u); EXPECT_EQ(s[0].label, "caller"); EXPECT_TRUE(s[0].active);
    EXPECT_EQ(s[1].ssrc, 2222u); EXPECT_EQ(s[1].label, "callee");
    EXPECT_TRUE(mcptt::sdpSsrcLabels("v=0\r\nm=audio 1 RTP/AVP 0\r\n").empty());
}
