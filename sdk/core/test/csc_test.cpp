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
    EXPECT_FALSE(none.allowGroupCreation);
    EXPECT_TRUE(none.dispatch.members.empty());
    EXPECT_FALSE(CscClient::parseProfile("not json", none));
}

// dispatch 발견 확장(members[]/pttTargets[])·그룹 생성 자격 — 서버 요청서 §1.2·§2 계약. 없으면 빈 배열/false.
TEST(Csc, ParseProfileDispatchDiscovery) {
    static const char* kJson = R"({
      "services": [ { "kind": "ptt", "sip": { "domain": "ptt.example.org" }, "account": { "msisdn": "+82500000001" } } ],
      "ptt": { "allowCreateGroup": true },
      "dispatch": { "groupId": "dg-1", "monitorScope": "listed", "pttListen": "listed",
        "members": [ { "userId": 12, "name": "관제2석", "volteAor": "tel:+82310001002", "pttId": "sip:+82510001002@ptt.example.org", "extension": "1002", "groupId": "dg-1" },
                     { "userId": 13, "name": "빈항목" } ],
        "pttTargets": [ { "id": "g002", "uri": "sip:g002@ptt.example.org", "name": "음성그룹2" }, { "uri": "sip:g003@ptt.example.org" } ] }
    })";
    Profile p;
    ASSERT_TRUE(CscClient::parseProfile(kJson, p));
    EXPECT_TRUE(p.allowGroupCreation);
    ASSERT_EQ(p.dispatch.members.size(), 1u);                  // 주소가 하나도 없는 항목은 버린다
    EXPECT_EQ(p.dispatch.members[0].userId, "12");
    EXPECT_EQ(p.dispatch.members[0].volteAor, "tel:+82310001002");
    EXPECT_EQ(p.dispatch.members[0].extension, "1002");
    EXPECT_EQ(p.dispatch.members[0].groupId, "dg-1");
    ASSERT_EQ(p.dispatch.pttTargets.size(), 2u);
    EXPECT_EQ(p.dispatch.pttTargets[0].name, "음성그룹2");
    EXPECT_EQ(p.dispatch.pttTargets[1].id, "g003");            // id 생략 → uri user part
    Profile svc;                                               // 서비스 항목 표기(호환)도 인정
    ASSERT_TRUE(CscClient::parseProfile(R"({"services":[{"kind":"ptt","allowCreateGroup":true}]})", svc));
    EXPECT_TRUE(svc.allowGroupCreation);
}

// GMS 그룹 문서 — 서버(get_group_xml) 형식 파싱 + toXml 왕복. 요소는 접두사 무관 로컬 이름, 텍스트는 XML 이스케이프.
TEST(GroupDoc, ParseServerDocumentAndRoundTrip) {
    static const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service" xmlns:rl="urn:ietf:params:xml:ns:resource-lists" xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:oxe="urn:oma:xml:xdm:extensions" xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0" xmlns:cims="urn:cims:groupinfo:1.0">
  <list-service uri="sip:g002@ptt.example.org">
    <display-name xml:lang="en-us">음성그룹 &amp; 2</display-name>
    <list>
      <entry uri="tel:+82510001001">
        <rl:display-name>관제1석</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:participant-type>chair</mcpttgi:participant-type>
        <mcpttgi:user-priority>7</mcpttgi:user-priority>
        <cims:user-title>팀장</cims:user-title>
      </entry>
      <entry uri="tel:+82510001002">
        <rl:display-name>관제2석</rl:display-name>
        <mcpttgi:participant-type>participant</mcpttgi:participant-type>
        <mcpttgi:user-priority>5</mcpttgi:user-priority>
      </entry>
    </list>
    <mcpttgi:session-type>chat</mcpttgi:session-type>
    <mcpttgi:mcdata-allow-short-data-service>true</mcpttgi:mcdata-allow-short-data-service>
    <mcpttgi:mcdata-allow-file-distribution>false</mcpttgi:mcdata-allow-file-distribution>
    <mcpttgi:mcptt-video>true</mcpttgi:mcptt-video>
    <mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>
    <mcpttgi:on-network-max-participant-count>20</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-require-affiliation>false</mcpttgi:on-network-require-affiliation>
    <mcpttgi:on-network-hang-time>3</mcpttgi:on-network-hang-time>
    <mcpttgi:on-network-group-priority>3</mcpttgi:on-network-group-priority>
    <mcpttgi:on-network-encryption>false</mcpttgi:on-network-encryption>
    <cp:ruleset><cp:rule id="a7c"><cp:actions>
      <mcpttgi:allow-MCPTT-emergency-call>false</mcpttgi:allow-MCPTT-emergency-call>
      <mcpttgi:allow-imminent-peril-call>false</mcpttgi:allow-imminent-peril-call>
      <mcpttgi:allow-MCPTT-emergency-alert>true</mcpttgi:allow-MCPTT-emergency-alert>
    </cp:actions></cp:rule></cp:ruleset>
    <oxe:supported-services><oxe:service enabler="example.mcptt"><oxe:group-media><mcpttgi:mcptt-speech/></oxe:group-media></oxe:service></oxe:supported-services>
    <mcpttgi:org-code>ORG1</mcpttgi:org-code>
    <mcpttgi:authorized-user>tel:+82510001001</mcpttgi:authorized-user>
  </list-service>
</group>)";
    GroupDoc d; d.etag = "\"e1\"";
    std::string err;
    ASSERT_TRUE(GroupDoc::parse(kXml, d, &err)) << err;
    EXPECT_EQ(d.uri, "sip:g002@ptt.example.org");
    EXPECT_EQ(d.displayName, "음성그룹 & 2");
    EXPECT_EQ(d.etag, "\"e1\"");                               // 호출자가 채운 etag 는 유지
    ASSERT_EQ(d.members.size(), 2u);
    EXPECT_EQ(d.members[0].uri, "tel:+82510001001");
    EXPECT_EQ(d.members[0].name, "관제1석");
    EXPECT_EQ(d.members[0].role, "chair");
    EXPECT_EQ(d.members[0].priority, 7);
    EXPECT_EQ(d.members[1].role, "participant");
    EXPECT_EQ(d.sessionType, "chat");
    EXPECT_TRUE(d.allowSds); EXPECT_FALSE(d.allowFd); EXPECT_TRUE(d.videoEnabled);
    EXPECT_EQ(d.maxParticipants, 20); EXPECT_FALSE(d.requireAffiliation); EXPECT_EQ(d.priority, 3);
    EXPECT_FALSE(d.encryption); EXPECT_FALSE(d.emergencyCall); EXPECT_TRUE(d.emergencyAlert);
    EXPECT_EQ(d.orgCode, "ORG1");
    EXPECT_EQ(d.authorizedUser, "tel:+82510001001");

    // 왕복 — toXml 결과를 다시 파싱하면 같은 모델
    GroupDoc back;
    ASSERT_TRUE(GroupDoc::parse(d.toXml(), back, &err)) << err;
    EXPECT_EQ(back.uri, d.uri); EXPECT_EQ(back.displayName, d.displayName);
    ASSERT_EQ(back.members.size(), 2u);
    EXPECT_EQ(back.members[0].role, "chair"); EXPECT_EQ(back.members[0].priority, 7); EXPECT_EQ(back.members[1].name, "관제2석");
    EXPECT_EQ(back.sessionType, "chat"); EXPECT_TRUE(back.videoEnabled); EXPECT_EQ(back.maxParticipants, 20);
    EXPECT_FALSE(back.emergencyCall); EXPECT_TRUE(back.emergencyAlert); EXPECT_EQ(back.orgCode, "ORG1");
    EXPECT_NE(d.toXml().find("<display-name xml:lang=\"en-us\">음성그룹 &amp; 2</display-name>"), std::string::npos);

    GroupDoc fresh;                                            // 최소 문서(멤버 없음·기본값) 도 유효
    fresh.uri = "sip:g-new@ptt.example.org"; fresh.displayName = "새 그룹";
    GroupDoc parsed;
    ASSERT_TRUE(GroupDoc::parse(fresh.toXml(), parsed));
    EXPECT_TRUE(parsed.members.empty()); EXPECT_EQ(parsed.sessionType, "prearranged"); EXPECT_EQ(parsed.maxParticipants, 0);
    EXPECT_TRUE(parsed.requireAffiliation); EXPECT_TRUE(parsed.emergencyCall);
    EXPECT_FALSE(GroupDoc::parse("<other/>", parsed, &err));
    EXPECT_FALSE(err.empty());
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
