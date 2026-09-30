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
               "default": "TLS", "enforced": true, "domain": "ptt.example.org", "udpNoTcpSwitch": true },
      "account": { "msisdn": "+82500000001", "imsi": "4503382500000001", "sipHa1": null, "mcpttId": "tel:+82500000001",
                   "authScheme": "aka", "aka": { "k": "00112233", "opc": "44556677", "amf": "8000" } } }
  ],
  "dispatch": { "groupId": "dg-1", "groupName": "관제1", "pilotId": "+8215001000", "monitorScope": "all", "pttListen": "listed", "listenVisibility": "hidden",
                "directoryAdmin": "own", "orgCode": "DIV1" }
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
    EXPECT_FALSE(v->udpNoTcpSwitch);                                  // 없으면 false(규격대로 승격)
    ASSERT_EQ(v->secMechanisms.size(), 1u);
    EXPECT_EQ(v->sipHa1, "0123456789abcdef0123456789abcdef");
    AccountConfig a = v->toAccount();
    EXPECT_EQ(a.digestUsername(), "45033821300000001@ims.example.org");
    EXPECT_TRUE(a.isComplete());
    const ServiceProfile* t = p.service("ptt");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->udpNoTcpSwitch);
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
    EXPECT_EQ(p.dispatch.directoryAdmin, "own");
    EXPECT_EQ(p.dispatch.orgCode, "DIV1");
    Profile none;
    ASSERT_TRUE(CscClient::parseProfile(R"({"services":[]})", none));
    EXPECT_FALSE(none.dispatch.present);
    EXPECT_EQ(none.dispatch.directoryAdmin, "none");
    EXPECT_FALSE(none.allowGroupCreation);
    EXPECT_TRUE(none.dispatch.members.empty());
    EXPECT_FALSE(CscClient::parseProfile("not json", none));
}

// 전화 회선 선택 — 유선 voip 우선, 없으면 이동 volte(android_ue_provisioning.md §3). 관제 앱의 전화 계정 규칙.
TEST(Csc, PhoneServicePrefersVoipOverVolte) {
    Profile p;
    ASSERT_TRUE(CscClient::parseProfile(kProfile, p));
    ASSERT_NE(p.phoneService(), nullptr);
    EXPECT_EQ(p.phoneService(), p.service("volte"));            // voip 없음 → volte 폴백
    Profile both;
    ASSERT_TRUE(CscClient::parseProfile(R"({"services":[
        {"kind":"volte","sip":{"host":"10.0.0.1","port":5060,"domain":"volte.example"},"account":{"msisdn":"+8213000"}},
        {"kind":"voip","sip":{"host":"10.0.0.1","port":5060,"transport":"TLS","domain":"voip.example"},"account":{"msisdn":"+8213001"}},
        {"kind":"ptt","sip":{"host":"10.0.0.1","port":5061,"domain":"ptt.example"},"account":{"msisdn":"+8250000"}}]})", both));
    ASSERT_NE(both.phoneService(), nullptr);
    EXPECT_EQ(both.phoneService()->kind, "voip");
    EXPECT_EQ(both.phoneService()->domain, "voip.example");
    EXPECT_EQ(both.service("volte")->domain, "volte.example");   // 이동 앱은 volte 를 그대로 본다
    Profile pttOnly;
    ASSERT_TRUE(CscClient::parseProfile(R"({"services":[{"kind":"ptt"}]})", pttOnly));
    EXPECT_EQ(pttOnly.phoneService(), nullptr);
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
    <mcpttgi:on-network-invite-members>false</mcpttgi:on-network-invite-members>
    <mcpttgi:on-network-max-participant-count>20</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-require-affiliation>false</mcpttgi:on-network-require-affiliation>
    <mcpttgi:on-network-hang-timer>PT30S</mcpttgi:on-network-hang-timer>
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

    // 그룹 종류 = on-network-invite-members(TS 24.481 §7.2.2 a) — 직렬화는 규격 요소만, session-type 은 싣지 않는다
    EXPECT_EQ(back.toXml().find("session-type"), std::string::npos);
    EXPECT_NE(back.toXml().find("<mcpttgi:on-network-invite-members>false</mcpttgi:on-network-invite-members>"), std::string::npos);
    EXPECT_NE(fresh.toXml().find("<mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>"), std::string::npos);
    // invite-members 가 없는 옛 문서만 비규격 session-type 으로 판정(값 broadcast 같은 옛 유형은 prearranged)
    GroupDoc legacy;
    ASSERT_TRUE(GroupDoc::parse("<group><list-service uri=\"sip:g9@d\"><list></list><mcpttgi:session-type>chat</mcpttgi:session-type></list-service></group>", legacy));
    EXPECT_EQ(legacy.sessionType, "chat");
    ASSERT_TRUE(GroupDoc::parse("<group><list-service uri=\"sip:g9@d\"><list></list><mcpttgi:session-type>broadcast</mcpttgi:session-type></list-service></group>", legacy));
    EXPECT_EQ(legacy.sessionType, "prearranged");
}

// 그룹 호 타이머 · 참가자 정보 · MCData 크기 한도(TS 24.481) — **미기재가 기본값**이고 미기재면 싣지 않는다.
//   이 성질이 깨지면 폼에서 이 칸을 다루지 않는 앱이 저장할 때마다 콘솔이 정한 값을 기본값으로 덮어쓴다.
TEST(GroupDoc, CallTimersConferenceStateAndSizeLimits) {
    static const char* kXml = R"(<group><list-service uri="sip:g005@ptt.example.org">
    <display-name>일제</display-name>
    <list><entry uri="tel:+82510001001"><mcpttgi:participant-type>chair</mcpttgi:participant-type>
      <mcpttgi:user-priority>9</mcpttgi:user-priority></entry>
      <entry uri="tel:+82510001002"><mcpttgi:user-priority>2</mcpttgi:user-priority></entry></list>
    <mcpttgi:mcdata-on-network-max-data-size-for-SDS>1000</mcpttgi:mcdata-on-network-max-data-size-for-SDS>
    <mcpttgi:mcdata-on-network-max-data-size-auto-recv>5000</mcpttgi:mcdata-on-network-max-data-size-auto-recv>
    <mcpttgi:on-network-hang-timer>PT5S</mcpttgi:on-network-hang-timer>
    <mcpttgi:on-network-maximum-duration>PT3600S</mcpttgi:on-network-maximum-duration>
    <cp:ruleset><cp:rule id="a7c"><cp:actions>
      <mcpttgi:on-network-allow-conference-state>false</mcpttgi:on-network-allow-conference-state>
    </cp:actions></cp:rule></cp:ruleset>
  </list-service></group>)";
    GroupDoc d;
    ASSERT_TRUE(GroupDoc::parse(kXml, d));
    EXPECT_EQ(d.hangTimerSec, 5);
    EXPECT_EQ(d.maxDurationSec, 3600);
    EXPECT_EQ(d.allowConferenceState, 0);
    EXPECT_EQ(d.maxSdsSize, 1000);
    EXPECT_EQ(d.maxAutoRecv, 5000);

    // 왕복 — 값이 그대로 돌아온다. 멤버별 우선순위(9·2)도 보존된다(앱이 버리지 않는 한).
    GroupDoc back;
    ASSERT_TRUE(GroupDoc::parse(d.toXml(), back));
    EXPECT_EQ(back.hangTimerSec, 5); EXPECT_EQ(back.maxDurationSec, 3600);
    EXPECT_EQ(back.allowConferenceState, 0); EXPECT_EQ(back.maxSdsSize, 1000); EXPECT_EQ(back.maxAutoRecv, 5000);
    ASSERT_EQ(back.members.size(), 2u);
    EXPECT_EQ(back.members[0].priority, 9);
    EXPECT_EQ(back.members[1].priority, 2);
    // 참가자 정보 구독은 서버가 cp:actions 안에서 찾는다
    const std::string x = d.toXml();
    const size_t a = x.find("<cp:actions>"), e = x.find("</cp:actions>"), c = x.find("on-network-allow-conference-state");
    ASSERT_NE(c, std::string::npos);
    EXPECT_TRUE(a < c && c < e);
    EXPECT_NE(x.find("<mcpttgi:on-network-hang-timer>PT5S</mcpttgi:on-network-hang-timer>"), std::string::npos);

    // **미기재 → 싣지 않는다** — 새로 만든 문서(앱이 폼으로 짓는 경우)는 다섯 요소를 하나도 싣지 않는다
    GroupDoc fresh;
    fresh.uri = "sip:g-new@ptt.example.org";
    EXPECT_EQ(fresh.hangTimerSec, GroupDoc::kUnset);
    const std::string fx = fresh.toXml();
    for (const char* tag : {"on-network-hang-timer", "on-network-maximum-duration", "on-network-allow-conference-state",
                            "max-data-size-for-SDS", "max-data-size-auto-recv"})
        EXPECT_EQ(fx.find(tag), std::string::npos) << tag << " 가 미기재인데 실렸다";
    GroupDoc none;                                              // 문서에 없던 요소는 미기재로 읽는다(0 이 아니다)
    ASSERT_TRUE(GroupDoc::parse(fx, none));
    EXPECT_EQ(none.hangTimerSec, GroupDoc::kUnset); EXPECT_EQ(none.maxDurationSec, GroupDoc::kUnset);
    EXPECT_EQ(none.allowConferenceState, GroupDoc::kUnset);
    EXPECT_EQ(none.maxSdsSize, GroupDoc::kUnset); EXPECT_EQ(none.maxAutoRecv, GroupDoc::kUnset);

    // **0 은 값이다** — 미사용·무제한을 뜻하므로 실어야 한다(미기재와 다르다)
    GroupDoc zero;
    zero.uri = "sip:g0@d"; zero.hangTimerSec = 0; zero.maxDurationSec = 0; zero.maxSdsSize = 0; zero.maxAutoRecv = 0;
    zero.allowConferenceState = 1;
    const std::string zx = zero.toXml();
    EXPECT_NE(zx.find("<mcpttgi:on-network-hang-timer>PT0S</mcpttgi:on-network-hang-timer>"), std::string::npos);
    EXPECT_NE(zx.find("<mcpttgi:on-network-maximum-duration>PT0S</mcpttgi:on-network-maximum-duration>"), std::string::npos);
    EXPECT_NE(zx.find(">0</mcpttgi:mcdata-on-network-max-data-size-for-SDS>"), std::string::npos);
    EXPECT_NE(zx.find("<mcpttgi:on-network-allow-conference-state>true"), std::string::npos);
}

// xs:duration — 서버 parse_xs_duration 과 같은 관대함. 모르는 형식은 미기재(0 으로 읽으면 «미사용» 이라는 다른 뜻).
TEST(GroupDoc, MemberTitleIsReadOnly) {
    GroupDoc d;
    ASSERT_TRUE(GroupDoc::parse("<group><list-service uri=\"sip:g@d\"><list><entry uri=\"tel:+1\"><display-name>가</display-name>"
                                "<cims:user-title>반장</cims:user-title></entry></list></list-service></group>", d));
    ASSERT_EQ(d.members.size(), 1u);
    EXPECT_EQ(d.members[0].title, "반장");
    EXPECT_EQ(d.toXml().find("user-title"), std::string::npos);        // PUT 에는 싣지 않는다
}

TEST(GroupDoc, HangTimerDurationForms) {
    auto hang = [](const std::string& v) {
        GroupDoc d;
        GroupDoc::parse("<group><list-service uri=\"sip:g@d\"><list></list><mcpttgi:on-network-hang-timer>" + v +
                        "</mcpttgi:on-network-hang-timer></list-service></group>", d);
        return d.hangTimerSec;
    };
    EXPECT_EQ(hang("PT30S"), 30);
    EXPECT_EQ(hang("PT1M"), 60);
    EXPECT_EQ(hang("PT1H30M"), 5400);
    EXPECT_EQ(hang("P1DT2H"), 93600);
    EXPECT_EQ(hang("PT1.9S"), 1);                              // 소수 초는 절사
    EXPECT_EQ(hang("45"), 45);                                 // 순수 정수(초)도 받는다
    EXPECT_EQ(hang(" PT10S "), 10);
    for (const char* bad : {"", "P", "PT", "abc", "PT1X", "1H", "PTS", "P1H", "PT1.5M"})
        EXPECT_EQ(hang(bad), GroupDoc::kUnset) << "'" << bad << "'";
}

// 서버(csc get_user_profile_xml) 산출 모양 — PrivateCall 의 EmergencyCall 이 그룹콜 쪽보다 먼저 나온다.
static const char* kUserProfileXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<mcptt-user-profile xmlns="urn:3gpp:mcptt:user-profile:1.0"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy" xmlns:cims="urn:cims:mcptt:ext:1.0"
  XUI-URI="tel:+82500000002" user-profile-index="1">
  <Name xml:lang="ko">테스트002</Name>
  <Status>true</Status>
  <Common index="1"><UserAlias><alias-entry index="1" xml:lang="ko">테스트002</alias-entry></UserAlias><MCPTTUserID><uri-entry>tel:+82500000002</uri-entry></MCPTTUserID><PrivateCall><PrivateCallList><PrivateCallURI><uri-entry>tel:+82500000001</uri-entry><display-name>테스트001</display-name></PrivateCallURI></PrivateCallList><EmergencyCall><MCPTTPrivateRecipient><entry entry-info="UsePreConfigured"><uri-entry>tel:+82500000001</uri-entry></entry><ProSeUserID-entry><User-Info-ID>000000000000</User-Info-ID></ProSeUserID-entry></MCPTTPrivateRecipient></EmergencyCall></PrivateCall><MCPTT-group-call><MaxSimultaneousCallsN6>5</MaxSimultaneousCallsN6><EmergencyCall><MCPTTGroupInitiation><entry entry-info="DedicatedGroup"><uri-entry>sip:g002@ptt.example.org</uri-entry><display-name>음성그룹2</display-name></entry></MCPTTGroupInitiation></EmergencyCall><ImminentPerilCall><MCPTTGroupInitiation><entry entry-info="DedicatedGroup"><uri-entry>sip:g002@ptt.example.org</uri-entry></entry></MCPTTGroupInitiation></ImminentPerilCall><EmergencyAlert><entry entry-info="DedicatedGroup"><uri-entry>sip:g002@ptt.example.org</uri-entry></entry></EmergencyAlert><Priority>5</Priority></MCPTT-group-call><ParticipantType>normal</ParticipantType></Common>
  <cp:ruleset>
    <cp:rule id="mcptt-user-authorisation">
      <cp:actions>
        <allow-emergency-group-call>true</allow-emergency-group-call>
        <allow-activate-emergency-alert>false</allow-activate-emergency-alert>
        <allow-cancel-emergency-alert>false</allow-cancel-emergency-alert>
        <allow-emergency-private-call>true</allow-emergency-private-call>
        <allow-ambient-listening>false</allow-ambient-listening>
        <cims:allow-adhoc-group-call>false</cims:allow-adhoc-group-call>
      </cp:actions>
    </cp:rule>
  </cp:ruleset>
  <OnNetwork index="1"><MCPTTGroupInfo><entry><uri-entry>sip:g002@ptt.example.org</uri-entry><display-name>음성그룹2</display-name><anyExt><cims:authorized-user>true</cims:authorized-user></anyExt></entry><entry><uri-entry>sip:g005@ptt.example.org</uri-entry></entry></MCPTTGroupInfo><MaxAffiliationsN2>8</MaxAffiliationsN2><ImplicitAffiliations><entry><uri-entry>sip:g002@ptt.example.org</uri-entry></entry></ImplicitAffiliations><PrivateEmergencyAlert><entry entry-info="UsePreConfigured"><uri-entry>tel:+82500000001</uri-entry></entry></PrivateEmergencyAlert></OnNetwork>
</mcptt-user-profile>)";

static const char* kServiceConfigXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<service-configuration-info xmlns="urn:3gpp:ns:mcpttServiceConfig:1.0">
  <service-configuration-params domain="ptt.example.org">
    <common>
      <broadcast-group>
        <num-levels-group-hierarchy>3</num-levels-group-hierarchy>
        <num-levels-user-hierarchy>4</num-levels-user-hierarchy>
      </broadcast-group>
    </common>
    <on-network>
      <fc-timers-counters><T1-end-of-rtp-media>PT4S</T1-end-of-rtp-media></fc-timers-counters>
      <emergency-resource-priority>
        <resource-priority-namespace>mcpttp</resource-priority-namespace>
        <resource-priority-priority>14</resource-priority-priority>
      </emergency-resource-priority>
      <imminent-peril-resource-priority>
        <resource-priority-namespace>mcpttp</resource-priority-namespace>
        <resource-priority-priority>8</resource-priority-priority>
      </imminent-peril-resource-priority>
      <normal-resource-priority>
        <resource-priority-namespace>mcpttp</resource-priority-namespace>
        <resource-priority-priority>0</resource-priority-priority>
      </normal-resource-priority>
    </on-network>
  </service-configuration-params>
</service-configuration-info>)";

TEST(CmsDoc, ParseUserProfile) {
    UserProfileDoc d; d.etag = "\"up1\"";
    std::string err;
    ASSERT_TRUE(UserProfileDoc::parse(kUserProfileXml, d, &err)) << err;
    EXPECT_EQ(d.etag, "\"up1\"");                              // 호출자가 채운 값 유지
    EXPECT_EQ(d.userUri, "tel:+82500000002");
    // 그룹 긴급 대상은 MCPTT-group-call 쪽 — 앞선 PrivateCall/EmergencyCall 을 잡지 않는다
    EXPECT_EQ(d.emergencyGroup.uri, "sip:g002@ptt.example.org");
    EXPECT_EQ(d.emergencyGroup.mode, "DedicatedGroup");
    EXPECT_EQ(d.imminentPerilGroup.uri, "sip:g002@ptt.example.org");
    EXPECT_EQ(d.emergencyAlertGroup.uri, "sip:g002@ptt.example.org");
    EXPECT_EQ(d.emergencyPrivateRecipient.uri, "tel:+82500000001");
    EXPECT_EQ(d.emergencyPrivateRecipient.mode, "UsePreConfigured");
    ASSERT_EQ(d.groups.size(), 2u);
    EXPECT_EQ(d.groups[1], "sip:g005@ptt.example.org");
    ASSERT_EQ(d.implicitAffiliations.size(), 1u);
    EXPECT_EQ(d.maxAffiliationsN2, 8);
    EXPECT_TRUE(d.allowEmergencyGroupCall);
    EXPECT_TRUE(d.allowImminentPerilCall);                     // 요소 없음 = 허용
    EXPECT_FALSE(d.allowActivateEmergencyAlert);
    EXPECT_FALSE(d.allowCancelEmergencyAlert);
    EXPECT_TRUE(d.allowEmergencyPrivateCall);
    EXPECT_FALSE(d.allowAdhocGroupCall);                       // cims: 확장도 로컬 이름으로 읽힌다

    // 규격 요소 <allow-adhoc-group-call>(TS 24.484 §8.3.2.1) 도 같은 필드로
    UserProfileDoc s;
    ASSERT_TRUE(UserProfileDoc::parse("<mcptt-user-profile><ruleset><actions><allow-adhoc-group-call>false</allow-adhoc-group-call>"
                                      "</actions></ruleset></mcptt-user-profile>", s));
    EXPECT_FALSE(s.allowAdhocGroupCall);
    EXPECT_TRUE(s.emergencyGroup.uri.empty());
    EXPECT_EQ(s.maxAffiliationsN2, -1);
    EXPECT_FALSE(UserProfileDoc::parse("<group/>", s, &err));
}

TEST(CmsDoc, ParseServiceConfig) {
    // TS 24.484 §8.4 구조 — 루트 service-configuration-info › service-configuration-params
    ServiceConfigDoc d;
    std::string err;
    ASSERT_TRUE(ServiceConfigDoc::parse(kServiceConfigXml, d, &err)) << err;
    EXPECT_EQ(d.domain, "ptt.example.org");
    EXPECT_EQ(d.numLevelsGroupHierarchy, 3);
    EXPECT_EQ(d.numLevelsUserHierarchy, 4);
    EXPECT_EQ(d.rpEmergency, "mcpttp.14");                     // RFC 4412 r-value = namespace.priority
    EXPECT_EQ(d.rpImminentPeril, "mcpttp.8");
    EXPECT_EQ(d.rpNormal, "mcpttp.0");
    ServiceConfigDoc m;
    ASSERT_TRUE(ServiceConfigDoc::parse("<service-configuration-info/>", m));   // params 없음 = 설정 없음
    EXPECT_TRUE(m.rpEmergency.empty());
    EXPECT_EQ(m.numLevelsGroupHierarchy, -1);
    EXPECT_FALSE(ServiceConfigDoc::parse("<mcptt-service-config/>", m, &err));  // 옛 비규격 루트
    EXPECT_FALSE(ServiceConfigDoc::parse("<mcptt-user-profile/>", m, &err));
}

TEST(CmsDoc, CapabilitiesFromUserProfile) {
    // 미수신 — 게이트 없음
    Capabilities none = Capabilities::of(nullptr, nullptr);
    EXPECT_FALSE(none.userProfileKnown);
    EXPECT_TRUE(none.emergencyGroupCall && none.emergencyAlert && none.privateCall && none.adhocGroupCall);
    EXPECT_EQ(none.maxAffiliationsN2, 0);

    UserProfileDoc up; ServiceConfigDoc sc;
    ASSERT_TRUE(UserProfileDoc::parse(kUserProfileXml, up));
    ASSERT_TRUE(ServiceConfigDoc::parse(kServiceConfigXml, sc));
    Capabilities c = Capabilities::of(&up, &sc);
    EXPECT_TRUE(c.userProfileKnown && c.serviceConfigKnown);
    EXPECT_TRUE(c.emergencyGroupCall);                         // 인가는 user profile 만(service config 에 인가 요소 없음)
    EXPECT_TRUE(c.imminentPerilCall);
    EXPECT_TRUE(c.privateCall && c.emergencyPrivateCall);      // allow-private-call 요소 없음 = 허용
    EXPECT_FALSE(c.emergencyAlert);                            // 사용자 불허
    EXPECT_FALSE(c.adhocGroupCall);
    EXPECT_EQ(c.maxAffiliationsN2, 8);                         // user profile MaxAffiliationsN2
    EXPECT_EQ(Capabilities::of(&up, nullptr).maxAffiliationsN2, 8);

    UserProfileDoc np;
    ASSERT_TRUE(UserProfileDoc::parse("<mcptt-user-profile><ruleset><actions><allow-private-call>false</allow-private-call>"
                                      "<allow-private-call-media-protection>true</allow-private-call-media-protection>"
                                      "</actions></ruleset></mcptt-user-profile>", np));
    Capabilities p = Capabilities::of(&np, nullptr);
    EXPECT_FALSE(p.privateCall);                               // 이름 경계 — -media-protection 을 잡지 않는다
    EXPECT_FALSE(p.emergencyPrivateCall);
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

// TS 24.380 §14.2 — 개시 offer 의 암묵 요청은 mc_implicit_request 와 200 OK 승인 수용 mc_granted 를 함께, 이어지는 offer 엔 둘 다 없이(§14.5).
TEST(FloorSdp, OfferFmtp) {
    EXPECT_NE(mcptt::floorSdp(5000, false, true).find("a=fmtp:MCPTT mc_queueing;mc_implicit_request;mc_granted"), std::string::npos);
    std::string plain = mcptt::floorSdp(5000, false);
    EXPECT_NE(plain.find("m=application 5000 UDP MCPTT"), std::string::npos);
    EXPECT_EQ(plain.find("mc_implicit_request"), std::string::npos);
    EXPECT_EQ(plain.find("mc_granted"), std::string::npos);
    std::string full = mcptt::floorSdp(5000, true, true);                    // 전이중엔 floor 가 없다 — 암묵 요청 무시
    EXPECT_NE(full.find("mc_no_floor_ctrl"), std::string::npos);
    EXPECT_EQ(full.find("mc_implicit_request"), std::string::npos);
}

// §14.3.4·§14.3.5 — answer 의 fmtp:MCPTT(m=application 섹션만) → 승인·받아들임 판정.
TEST(FloorSdp, ParseAnswerFmtp) {
    std::string a = "v=0\r\nm=audio 4000 RTP/AVP 96\r\na=fmtp:96 mode-set=2\r\nm=application 4002 UDP MCPTT\r\n"
                    "a=fmtp:MCPTT mc_queueing;mc_priority=3;mc_implicit_request; MC_GRANTED\r\n";
    auto f = mcptt::parseFloorFmtp(a);
    EXPECT_TRUE(f.present); EXPECT_TRUE(f.queueing); EXPECT_TRUE(f.implicitRequest); EXPECT_TRUE(f.granted); EXPECT_FALSE(f.noFloorCtrl);
    auto g = mcptt::parseFloorFmtp("v=0\r\nm=application 4002 UDP MCPTT\r\na=fmtp:MCPTT mc_queueing;mc_priority=3\r\n");
    EXPECT_TRUE(g.present); EXPECT_FALSE(g.implicitRequest); EXPECT_FALSE(g.granted);
    EXPECT_FALSE(mcptt::parseFloorFmtp("v=0\r\nm=audio 4000 RTP/AVP 96\r\n").present);
    auto h = mcptt::parseFloorFmtp("v=0\r\nm=application 4002 UDP MCPTT\r\na=floorid:0 mstrm:audio\r\nm=video 0 RTP/AVP 97\r\na=fmtp:MCPTT mc_granted\r\n");
    EXPECT_FALSE(h.present);                                                  // 다른 m= 섹션의 fmtp 는 floor 협상이 아니다
}

// ── 범용 요청(request) — 헤더 조립·이진 본문 왕복·상태 매핑(2xx/304 = ok, 4xx = fail + 산출 유지, 전송 실패 = -1) ──
#include "../src/http/https_client.h"

namespace {
struct FakeTransport : http::ITransport {
    http::Response next;
    std::string lastMethod, lastUrl, lastBody;
    std::map<std::string, std::string> lastHeaders;
    http::Response request(const std::string& method, const std::string& url,
                           const std::map<std::string, std::string>& headers, const std::string& body) override {
        lastMethod = method; lastUrl = url; lastHeaders = headers; lastBody = body;
        return next;
    }
};
}  // namespace

TEST(Csc, FdUploadDownload) {
    auto tp = std::make_shared<FakeTransport>();
    CscEndpoint ep; ep.host = "csc.example"; ep.port = 4430;
    CscClient c(ep, tp);

    // 그룹 FD 업로드 — octet-stream 본문 그대로, 쿼리 name·type·group(인코딩), 201 → url/size
    tp->next.status = 201;
    tp->next.body = "{\"id\":\"0123456789abcdef0123456789abcdef\",\"url\":\"https://10.0.0.1:4430/mcdata/fd/0123456789abcdef0123456789abcdef\",\"size\":5,\"name\":\"현장 1.jpg\"}";
    FdUpload up;
    Result r = c.uploadFd("tok", std::string("\x00\x01\x02\x03\x04", 5), "현장 1.jpg", "image/jpeg", "g001", up);
    ASSERT_TRUE(r.ok) << r.reason;
    EXPECT_EQ(tp->lastMethod, "POST");
    EXPECT_EQ(tp->lastUrl.rfind(ep.baseUrl() + "/mcdata/fd?name=", 0), 0u);
    EXPECT_NE(tp->lastUrl.find("&type=image%2Fjpeg"), std::string::npos);
    EXPECT_NE(tp->lastUrl.find("&group=g001"), std::string::npos);
    EXPECT_EQ(tp->lastUrl.find(' '), std::string::npos);
    EXPECT_EQ(tp->lastHeaders.at("Content-Type"), "application/octet-stream");
    EXPECT_EQ(tp->lastHeaders.at("Authorization"), "Bearer tok");
    EXPECT_EQ(tp->lastBody.size(), 5u);
    EXPECT_EQ(up.id, "0123456789abcdef0123456789abcdef"); EXPECT_EQ(up.size, 5); EXPECT_EQ(up.name, "현장 1.jpg");
    EXPECT_EQ(up.url, "https://10.0.0.1:4430/mcdata/fd/0123456789abcdef0123456789abcdef");

    // 1:1 — group 쿼리 없음. 서버가 Host 없이 상대 경로를 주면 이 CSC 절대 URL 로 채운다
    tp->next.body = "{\"id\":\"ab\",\"url\":\"/mcdata/fd/ab\",\"size\":1}";
    r = c.uploadFd("tok", "x", "a.txt", "", "", up);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(tp->lastUrl.find("group="), std::string::npos);
    EXPECT_NE(tp->lastUrl.find("type=application%2Foctet-stream"), std::string::npos);
    EXPECT_EQ(up.url, ep.baseUrl() + "/mcdata/fd/ab");

    // 게이트 거부 = HTTP 상태 그대로
    tp->next = http::Response(); tp->next.status = 403; tp->next.body = "{\"error\":\"file distribution disabled for this group\"}";
    r = c.uploadFd("tok", "x", "a.txt", "text/plain", "g002", up);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.code, 403);
    EXPECT_FALSE(c.uploadFd("tok", "", "a.txt", "", "", up).ok);            // 빈 파일은 보내지 않는다

    // 다운로드 — FILEURL 의 호스트가 달라도 경로만 취해 이 CSC 로(Bearer 를 다른 호스트로 보내지 않는다)
    tp->next = http::Response(); tp->next.status = 200; tp->next.body = std::string("\x89PNG\x00", 5);
    tp->next.headers = {{"content-type", "image/png"}};
    HttpResult out;
    r = c.downloadFd("tok", "https://other.example:9999/mcdata/fd/0123456789abcdef0123456789abcdef", out);
    ASSERT_TRUE(r.ok) << r.reason;
    EXPECT_EQ(tp->lastMethod, "GET");
    EXPECT_EQ(tp->lastUrl, ep.baseUrl() + "/mcdata/fd/0123456789abcdef0123456789abcdef");
    EXPECT_EQ(out.body.size(), 5u); EXPECT_EQ(out.contentType, "image/png");
    EXPECT_EQ(CscClient::fdPathOf("/mcdata/fd/ab?x=1"), "/mcdata/fd/ab");
    EXPECT_EQ(CscClient::fdPathOf("https://h/other/ab"), "");
    EXPECT_EQ(CscClient::fdPathOf("https://h/mcdata/fd/../../etc"), "");
    EXPECT_EQ(c.downloadFd("tok", "https://h/provisioning/me", out).code, -2);
}

TEST(Csc, GenericRequestHeadersBinaryAndStatusMapping) {
    auto tp = std::make_shared<FakeTransport>();
    CscEndpoint ep; ep.host = "csc.example"; ep.port = 4430;
    CscClient c(ep, tp);
    HttpResult out;

    // PUT JSON + If-Match → 200 이진 응답(NUL 포함)이 길이 그대로 온다
    tp->next.status = 200;
    tp->next.headers = {{"content-type", "audio/mp4"}, {"etag", "\"e1\""}};
    tp->next.body = std::string("\x00\x00\x00\x18" "ftyp", 8);
    Result r = c.request("tok", "PUT", "/provisioning/directory/orgs/T1", "application/json", "{\"name\":\"x\"}", "", "\"e0\"", "", out);
    EXPECT_TRUE(r.ok) << r.reason;
    EXPECT_EQ(tp->lastMethod, "PUT");
    EXPECT_EQ(tp->lastUrl, ep.baseUrl() + "/provisioning/directory/orgs/T1");
    EXPECT_EQ(tp->lastHeaders.at("Authorization"), "Bearer tok");
    EXPECT_EQ(tp->lastHeaders.at("Content-Type"), "application/json");
    EXPECT_EQ(tp->lastHeaders.at("If-Match"), "\"e0\"");
    EXPECT_EQ(tp->lastHeaders.at("Accept"), "*/*");
    EXPECT_EQ(tp->lastHeaders.count("If-None-Match"), 0u);
    EXPECT_EQ(out.status, 200); EXPECT_EQ(out.contentType, "audio/mp4"); EXPECT_EQ(out.etag, "\"e1\"");
    EXPECT_EQ(out.body.size(), 8u); EXPECT_EQ(out.body[3], '\x18');

    // GET 본문 없음 → Content-Type 헤더 없음, 304 = ok(NotModified 는 status 로)
    tp->next = http::Response(); tp->next.status = 304;
    r = c.request("tok", "GET", "/provisioning/directory/admin", "", "", "application/json", "", "\"e1\"", out);
    EXPECT_TRUE(r.ok); EXPECT_EQ(out.status, 304);
    EXPECT_EQ(tp->lastHeaders.count("Content-Type"), 0u);
    EXPECT_EQ(tp->lastHeaders.at("If-None-Match"), "\"e1\"");
    EXPECT_EQ(tp->lastHeaders.at("Accept"), "application/json");

    // 403 = fail(code=403) 이되 오류 본문은 산출에 남는다
    tp->next = http::Response(); tp->next.status = 403; tp->next.body = "{\"error\":\"out_of_scope\"}";
    tp->next.headers = {{"content-type", "application/json"}};
    r = c.request("tok", "DELETE", "/provisioning/directory/orgs/T2", "", "", "", "", "", out);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.code, 403);
    EXPECT_EQ(out.status, 403); EXPECT_EQ(out.body, "{\"error\":\"out_of_scope\"}");

    // 전송 실패 = -1
    tp->next = http::Response(); tp->next.status = 0; tp->next.error = "connect refused";
    r = c.request("tok", "GET", "/x", "", "", "", "", "", out);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.code, -1); EXPECT_EQ(out.status, 0);
}

TEST(Csc, FetchCmsDocsParseAndNotModified) {
    auto tp = std::make_shared<FakeTransport>();
    CscEndpoint ep; ep.host = "csc.example";
    CscClient c(ep, tp);

    tp->next.status = 200; tp->next.body = kUserProfileXml; tp->next.headers = {{"etag", "\"up1\""}};
    UserProfileDoc up;
    ASSERT_TRUE(c.fetchUserProfile("tok", "tel:+82500000002", "", up).ok);
    EXPECT_EQ(tp->lastUrl, ep.baseUrl() + "/org.3gpp.mcptt.user-profile/users/tel%3A%2B82500000002/user-profile");
    EXPECT_EQ(tp->lastHeaders.count("If-None-Match"), 0u);
    EXPECT_FALSE(up.notModified);
    EXPECT_EQ(up.etag, "\"up1\"");
    EXPECT_EQ(up.emergencyGroup.uri, "sip:g002@ptt.example.org");

    // 304 — 사본 그대로, notModified 만
    tp->next = http::Response(); tp->next.status = 304;
    ASSERT_TRUE(c.fetchUserProfile("tok", "tel:+82500000002", up.etag, up).ok);
    EXPECT_EQ(tp->lastHeaders.at("If-None-Match"), "\"up1\"");
    EXPECT_TRUE(up.notModified);
    EXPECT_EQ(up.emergencyGroup.uri, "sip:g002@ptt.example.org");

    // 본문이 문서가 아니면 해석 실패(-2)
    tp->next = http::Response(); tp->next.status = 200; tp->next.body = "<html/>";
    ServiceConfigDoc sc;
    Result r = c.fetchServiceConfig("tok", "tel:+82500000002", "", sc);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.code, -2);
    tp->next.body = kServiceConfigXml;
    ASSERT_TRUE(c.fetchServiceConfig("tok", "tel:+82500000002", "", sc).ok);
    EXPECT_EQ(sc.rpEmergency, "mcpttp.14");
}

// 확인 통화 설정(TS 24.481 §7.2.2 s)t)u)·§7.2.4.2) — 필수 멤버만 <on-network-required>, 세 요소는 미기재면 싣지 않는다
TEST(GroupDoc, AckCallSetupRoundTrip) {
    GroupDoc d;
    d.uri = "sip:g@d";
    GroupMember a; a.uri = "tel:+1"; a.required = true;
    GroupMember b; b.uri = "tel:+2";
    d.members = {a, b};
    std::string x = d.toXml();
    size_t n = 0;
    for (size_t p = x.find("on-network-required"); p != std::string::npos; p = x.find("on-network-required", p + 1)) ++n;
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(x.find("on-network-minimum-number-to-start"), std::string::npos);   // 미기재 — 서버 값 유지
    EXPECT_EQ(x.find("on-network-action-upon-expiration"), std::string::npos);
    d.minNumberToStart = 2; d.ackTimeoutSec = 7; d.ackAction = "proceed";
    GroupDoc r;
    ASSERT_TRUE(GroupDoc::parse(d.toXml(), r));
    ASSERT_EQ(r.members.size(), 2u);
    EXPECT_TRUE(r.members[0].required);
    EXPECT_FALSE(r.members[1].required);
    EXPECT_EQ(r.minNumberToStart, 2);
    EXPECT_EQ(r.ackTimeoutSec, 7);
    EXPECT_EQ(r.ackAction, "proceed");
    GroupDoc u;
    ASSERT_TRUE(GroupDoc::parse("<group><list-service uri=\"sip:g@d\"><list></list><mcpttgi:on-network-action-upon-expiration-"
                                "of-timeout-for-acknowledgement-of-required-members>later</mcpttgi:on-network-action-upon-"
                                "expiration-of-timeout-for-acknowledgement-of-required-members></list-service></group>", u));
    EXPECT_EQ(u.ackAction, "abandon");                                           // 정의 밖 값 = abandon (§7.2.2 u))
    EXPECT_EQ(u.minNumberToStart, GroupDoc::kUnset);
}
