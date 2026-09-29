// libcimsue 단위시험 — config → pjsua2 매핑 규칙 (S1-UE-UNIT, ue_sdk.md §9)
#include <gtest/gtest.h>

#include "../src/account_map.h"
#include "cimsue/cimsue.h"

using namespace cimsue;
using namespace cimsue::detail;

static AccountConfig base() {
    AccountConfig c;
    c.serverHost = "10.0.0.1"; c.serverPort = 5060; c.domain = "ims.example.org";
    c.msisdn = "+821300000001"; c.imsi = "450330000000001"; c.ha1 = std::string(32, 'a');
    return c;
}

TEST(AccountConfig, IdentityAndCompleteness) {
    AccountConfig c = base();
    EXPECT_EQ(c.aor(), "sip:+821300000001@ims.example.org");
    EXPECT_EQ(c.digestUsername(), "450330000000001@ims.example.org");   // IMPI — msisdn 폴백 없음
    EXPECT_TRUE(c.isComplete());
    c.imsi.clear();
    EXPECT_EQ(c.digestUsername(), "");
    EXPECT_FALSE(c.isComplete());                                        // IMSI/authId 없으면 등록 차단(서버 403 선차단)
    c.authId = "custom@other.org";
    EXPECT_EQ(c.digestUsername(), "custom@other.org");
    EXPECT_TRUE(c.isComplete());
}

TEST(AccountMap, DigestHa1RealmStarProxiesLr) {
    std::string note;
    pj::AccountConfig ac = buildPjAccountConfig(base(), &note);
    EXPECT_EQ(ac.idUri, "sip:+821300000001@ims.example.org");
    EXPECT_EQ(ac.regConfig.registrarUri, "sip:ims.example.org:5060;transport=udp");
    ASSERT_EQ(ac.sipConfig.authCreds.size(), 1u);
    EXPECT_EQ(ac.sipConfig.authCreds[0].realm, "*");                     // challenge realm echo
    EXPECT_EQ(ac.sipConfig.authCreds[0].username, "450330000000001@ims.example.org");
    EXPECT_EQ(ac.sipConfig.authCreds[0].dataType, PJSIP_CRED_DATA_DIGEST);
    ASSERT_EQ(ac.sipConfig.proxies.size(), 1u);
    EXPECT_EQ(ac.sipConfig.proxies[0], "sip:10.0.0.1:5060;transport=udp;lr");
    EXPECT_FALSE(ac.regConfig.registerOnAdd);
    EXPECT_EQ(ac.mediaConfig.srtpUse, PJMEDIA_SRTP_DISABLED);
    EXPECT_TRUE(ac.regConfig.headers.empty());
    EXPECT_NE(note.find("auth=ha1"), std::string::npos);
}

TEST(AccountMap, PlainPasswordWhenNoHa1AndAkaPrecedence) {
    AccountConfig c = base();
    c.ha1.clear(); c.password = "pw";
    pj::AccountConfig ac = buildPjAccountConfig(c);
    EXPECT_EQ(ac.sipConfig.authCreds[0].dataType, PJSIP_CRED_DATA_PLAIN_PASSWD);
    c.authScheme = AuthScheme::Aka; c.akaK = std::string(32, '1'); c.akaOpc = std::string(32, '2');
    ac = buildPjAccountConfig(c);
    EXPECT_EQ(ac.sipConfig.authCreds[0].dataType, PJSIP_CRED_DATA_EXT_AKA);
    EXPECT_EQ(ac.sipConfig.authCreds[0].akaOp, c.akaOpc);                // OPc 직접 소비(pjsip 패치)
}

TEST(AccountMap, SrtpOnlyOverTlsAndSecAgree) {
    AccountConfig c = base();
    c.mediaSecurity = MediaSecurity::Required;
    c.secMechanisms = {"tls"};
    pj::AccountConfig ac = buildPjAccountConfig(c);                       // UDP: SRTP·sec-agree 모두 꺼짐
    EXPECT_EQ(ac.mediaConfig.srtpUse, PJMEDIA_SRTP_DISABLED);
    EXPECT_TRUE(ac.regConfig.headers.empty());

    c.transport = Transport::TLS; c.serverPort = 5061;
    ac = buildPjAccountConfig(c);
    EXPECT_EQ(ac.mediaConfig.srtpUse, PJMEDIA_SRTP_MANDATORY);
    EXPECT_EQ(ac.regConfig.registrarUri, "sip:ims.example.org:5061;transport=tls");
    ASSERT_EQ(ac.regConfig.headers.size(), 3u);
    EXPECT_EQ(ac.regConfig.headers[0].hName, "Security-Client");
    EXPECT_EQ(ac.regConfig.headers[0].hValue, "tls, sdes-srtp;mediasec");  // 미디어 SRTP 정책 → mediasec 병기
    EXPECT_EQ(ac.regConfig.headers[1].hValue, "sec-agree");

    c.mediaSecurity = MediaSecurity::Optional;
    ac = buildPjAccountConfig(c);
    EXPECT_EQ(ac.mediaConfig.srtpUse, PJMEDIA_SRTP_OPTIONAL);
    c.mediaSecurity = MediaSecurity::Off;
    ac = buildPjAccountConfig(c);
    EXPECT_EQ(ac.regConfig.headers[0].hValue, "tls");
}

// 단말 인스턴스 ID(TS 24.229 §5.1.1.2 · RFC 5626 §4.1) — TCP/TLS 는 outbound 경로, UDP 는 REGISTER Contact 직접(한 번만)
TEST(AccountMap, InstanceIdPerTransport) {
    AccountConfig c = base();
    pj::AccountConfig ac = buildPjAccountConfig(c);                       // 비면 pjsip 기본값(건드리지 않음)
    EXPECT_TRUE(ac.natConfig.sipOutboundInstanceId.empty());
    EXPECT_TRUE(ac.regConfig.contactParams.empty());

    c.instanceId = "urn:gsma:imei:49015420-323751-0";
    ac = buildPjAccountConfig(c);                                         // UDP
    EXPECT_EQ(ac.regConfig.contactParams, ";+sip.instance=\"<urn:gsma:imei:49015420-323751-0>\"");
    EXPECT_EQ(ac.natConfig.sipOutboundUse, 0);

    // TCP/TLS 도 Contact 파라미터로 — pjsua outbound 경로는 서버가 outbound 를 모르면(OUTBOUND_NA) NAT 재작성 때 instance 를 뺀다
    c.transport = Transport::TLS; c.serverPort = 5061;
    ac = buildPjAccountConfig(c);
    EXPECT_EQ(ac.regConfig.contactParams, ";+sip.instance=\"<urn:gsma:imei:49015420-323751-0>\"");
    EXPECT_EQ(ac.natConfig.sipOutboundUse, 0);
}

// RFC 7254 IMEI URN — TAC 8 · SNR 6 · spare. 셋째 칸은 검사 숫자가 아니라 spare 라 항상 0(RFC 7254 §4.2.3 예 90420156-025763-0)
TEST(Helpers, ImeiUrnAndUserAgent) {
    EXPECT_EQ(imeiUrn("490154203237518"), "urn:gsma:imei:49015420-323751-0");   // 검사 숫자 형식(Luhn 8) → spare 0
    EXPECT_EQ(imeiUrn("49015420323751"), "urn:gsma:imei:49015420-323751-0");    // 14자리(TAC+SNR)
    EXPECT_EQ(imeiUrn("490154203237510"), "urn:gsma:imei:49015420-323751-0");   // 전송 형식(끝 0)
    EXPECT_EQ(imeiUrn("490154203237517"), "");                          // 검사 숫자 불일치
    EXPECT_EQ(imeiUrn("4901542032375"), "");                            // 13자리
    EXPECT_EQ(imeiUrn("49015420323751A"), "");
    EXPECT_EQ(userAgentOf("CIMS-PTT", "1.4.2", "Android 15", "SM-S921N"), "CIMS-PTT/1.4.2 (Android 15; SM-S921N)");
    EXPECT_EQ(userAgentOf("CIMS-Dispatch", "0.3", "Windows 11", ""), "CIMS-Dispatch/0.3 (Windows 11)");
    EXPECT_EQ(userAgentOf("cimsue-cli", "", "", ""), "cimsue-cli");
    // comment 정리 — 괄호·역슬래시 제거, 공백·제어 문자 접기, OS 의 `;`(구분자) 제거. 비면 괄호째 빠진다
    EXPECT_EQ(userAgentOf("CIMS-Dispatch", "0.1.0", "Windows 11 25H2", "Standard PC (Q35 + ICH9, 2009)"),
              "CIMS-Dispatch/0.1.0 (Windows 11 25H2; Standard PC Q35 + ICH9, 2009)");
    EXPECT_EQ(userAgentOf("P", "1", "  Linux;\tx86  ", " a\x5C" "b\n"), "P/1 (Linux x86; ab)");
    EXPECT_EQ(userAgentOf("P", "1", "()", " "), "P/1");
}

TEST(Helpers, TargetAndHeaders) {
    EXPECT_EQ(normalizeTarget("+8210", "d.org"), "sip:+8210@d.org");
    EXPECT_EQ(normalizeTarget("sip:a@b", "d.org"), "sip:a@b");
    EXPECT_EQ(normalizeTarget("tel:+8210", "d.org"), "tel:+8210");
    EXPECT_EQ(normalizeTarget("a@b.org", "d.org"), "sip:a@b.org");
    const std::string msg =
        "INVITE sip:+821300000001@ims.example.org SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 10.0.0.1\r\n"
        "p-called-party-id: <sip:+8215001000@ims.example.org>;x=1\r\n"
        "Content-Length: 0\r\n\r\nbody P-Called-Party-ID: nope";
    EXPECT_EQ(headerValue(msg, "P-Called-Party-ID"), "<sip:+8215001000@ims.example.org>;x=1");
    EXPECT_EQ(uriUser(headerValue(msg, "P-Called-Party-ID")), "+8215001000");
    EXPECT_EQ(headerValue(msg, "Nope"), "");
    EXPECT_EQ(uriUser("tel:+821300"), "+821300");
}

TEST(Types, ToString) {
    EXPECT_STREQ(toString(RegState::Registered), "registered");
    EXPECT_STREQ(toString(CallState::Held), "held");
    EXPECT_STREQ(toString(Transport::TLS), "tls");
}
