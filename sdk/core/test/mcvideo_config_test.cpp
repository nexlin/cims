// S1-UE-UNIT — MCVideo 설정 문서 해석(C2). 계약 K2 골든(tests/fixtures/mcvideo/*.xml)을 CSC 생성 시험(tests/test_csc_mcvideo.py)과
// **같은 파일**로 읽는다 — 생성 쪽 «이 파일과 같게», 해석 쪽 «이 파일을 읽어 README 의 값»(tests/fixtures/mcvideo/README.md).
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <sstream>

#include "../src/http/https_client.h"
#include "cimsue/csc.h"
#include "mcvideo/tc_defs.h"

using namespace cimsue;

namespace {

std::string fixture(const char* name) {
    std::ifstream f(std::string(CIMS_SOURCE_ROOT) + "/tests/fixtures/mcvideo/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/** MCVideo 몫 줄(요소 이름에 mcvideo·on-network-reception-hang-timer) — 앞뒤 공백을 지우고 순서대로. */
std::vector<std::string> mcvideoLines(const std::string& xml) {
    std::vector<std::string> out;
    std::istringstream in(xml);
    std::string l;
    while (std::getline(in, l)) {
        if (l.find("mcvideo") == std::string::npos && l.find("on-network-reception-hang-timer") == std::string::npos) continue;
        size_t b = l.find_first_not_of(" \t\r"), e = l.find_last_not_of(" \t\r");
        out.push_back(l.substr(b, e - b + 1));
    }
    return out;
}

struct FakeTransport : http::ITransport {
    http::Response next;
    std::string lastUrl;
    std::map<std::string, std::string> lastHeaders;
    http::Response request(const std::string&, const std::string& url, const std::map<std::string, std::string>& headers,
                           const std::string&) override {
        lastUrl = url;
        lastHeaders = headers;
        return next;
    }
};

}  // namespace

// group_g101.xml — MCPTT(prearranged) + MCVideo(chat) + MCData SDS. 같은 접미 요소(invite-members·maximum-duration·group-priority·
// allow-conference-state)가 서비스별로 따로 읽힌다.
TEST(McvConfig, GroupDocG101) {
    std::string xml = fixture("group_g101.xml");
    ASSERT_FALSE(xml.empty()) << "fixture 없음 — CIMS_SOURCE_ROOT=" << CIMS_SOURCE_ROOT;
    GroupDoc d;
    std::string err;
    ASSERT_TRUE(GroupDoc::parse(xml, d, &err)) << err;
    EXPECT_EQ(d.uri, "tel:g101");
    EXPECT_EQ(d.sessionType, "prearranged");                // MCPTT on-network-invite-members true
    EXPECT_EQ(d.maxDurationSec, 3600);
    EXPECT_EQ(d.priority, 5);
    EXPECT_EQ(d.allowConferenceState, 1);
    const McVideoGroupAttrs& v = d.mcvideo;
    EXPECT_TRUE(v.present);
    EXPECT_FALSE(v.inviteMembers);                          // chat(D5)
    EXPECT_EQ(v.maxDurationSec, 1800);
    EXPECT_FALSE(v.protectMedia);
    EXPECT_FALSE(v.protectTransmissionControl);
    ASSERT_EQ(v.audioEncodings.size(), 1u);
    EXPECT_EQ(v.audioEncodings[0], "AMR-WB");
    ASSERT_EQ(v.videoEncodings.size(), 1u);
    EXPECT_EQ(v.videoEncodings[0], "H264");
    EXPECT_EQ(v.videoResolutions, "1280x720,640x480");
    EXPECT_EQ(v.videoFrameRate, "30,15");
    EXPECT_EQ(v.nonUrgentRealTimeVideoMode, 1);
    EXPECT_EQ(v.urgentRealTimeVideoMode, -1);
    EXPECT_EQ(v.activeRealTimeVideoMode, "non-urgent-real-time");
    EXPECT_EQ(v.maxTransmitters, 2);
    EXPECT_EQ(v.minNumberToStart, 0);
    EXPECT_EQ(v.groupPriority, 100);
    EXPECT_EQ(v.receptionHangTimerSec, 30);
    EXPECT_EQ(v.allowConferenceState, 1);
    EXPECT_EQ(v.allowEmergencyCall, 0);
    EXPECT_EQ(v.allowEmergencyAlert, 0);
    EXPECT_EQ(v.allowImminentPerilCall, 0);
    ASSERT_EQ(d.members.size(), 3u);
    for (auto& m : d.members) EXPECT_EQ(m.mcvideoId, m.uri);   // entry MCVideo ID = entry uri(D1)
    EXPECT_EQ(d.members[2].role, "chair");
    EXPECT_TRUE(d.allowSds);

    // 되돌려 낸 문서 — MCVideo 몫이 골든과 같은 줄·순서이고 다시 읽으면 같은 값
    std::string out = d.toXml();
    EXPECT_GE(mcvideoLines(xml).size(), 20u);              // entry 3 · 속성 · 규칙 · <service> — 비교가 비지 않게
    EXPECT_EQ(mcvideoLines(out), mcvideoLines(xml));
    GroupDoc back;
    ASSERT_TRUE(GroupDoc::parse(out, back));
    EXPECT_TRUE(back.mcvideo.present);
    EXPECT_EQ(back.mcvideo.maxDurationSec, 1800);
    EXPECT_EQ(back.mcvideo.videoEncodings, v.videoEncodings);
    EXPECT_EQ(back.mcvideo.receptionHangTimerSec, 30);
    EXPECT_FALSE(back.mcvideo.protectMedia);
    EXPECT_EQ(back.maxDurationSec, 3600);
}

// group_g102_mcptt_only.xml — MCVideo 아님. 되돌려 낸 PUT 에도 MCVideo 가 없다(서버가 MCVideo 설정을 건드리지 않는다 — 전환기).
TEST(McvConfig, GroupDocMcpttOnly) {
    GroupDoc d;
    ASSERT_TRUE(GroupDoc::parse(fixture("group_g102_mcptt_only.xml"), d));
    EXPECT_EQ(d.uri, "tel:g102");
    EXPECT_FALSE(d.mcvideo.present);
    for (auto& m : d.members) EXPECT_TRUE(m.mcvideoId.empty());
    EXPECT_EQ(d.toXml().find("mcvideo"), std::string::npos);
}

// 보호 요소가 없으면 true 로 읽는다(TS 24.481 §7.2.8 — mcvideo.md §7 D7)
TEST(McvConfig, GroupDocProtectDefaultsTrue) {
    GroupDoc d;
    ASSERT_TRUE(GroupDoc::parse(
        "<group><list-service uri=\"tel:g9\"><list></list><oxe:supported-services>"
        "<oxe:service enabler=\"urn:urn-7:3gpp-service.ims.icsi.mcvideo\"/></oxe:supported-services></list-service></group>", d));
    EXPECT_TRUE(d.mcvideo.present);
    EXPECT_TRUE(d.mcvideo.protectMedia);
    EXPECT_TRUE(d.mcvideo.protectTransmissionControl);
    EXPECT_FALSE(d.mcvideo.inviteMembers);
    EXPECT_EQ(d.mcvideo.maxTransmitters, -1);
}

// mcvideo_user_profile.xml(TS 24.484 §9.3) — tel:+82510002001
TEST(McvConfig, UserProfile) {
    McVideoUserProfileDoc d;
    std::string err;
    ASSERT_TRUE(McVideoUserProfileDoc::parse(fixture("mcvideo_user_profile.xml"), d, &err)) << err;
    EXPECT_EQ(d.userUri, "tel:+82510002001");
    EXPECT_EQ(d.mcvideoId, "tel:+82510002001");
    ASSERT_EQ(d.groups.size(), 1u);
    EXPECT_EQ(d.groups[0], "tel:g101");                     // g102(MCPTT 만)는 없다
    EXPECT_EQ(d.maxSimultaneousVideoStreams, 1);            // C9 — 1차 수신 스트림 1
    EXPECT_EQ(d.maxSimultaneousCallsN6, 1);
    EXPECT_EQ(d.maxAffiliationsN2, 4); // MCVideo N2 — 회선 값(기본 4)
    EXPECT_EQ(d.emergencyGroup.uri, "tel:g101");
    EXPECT_EQ(d.emergencyGroup.mode, "UseCurrentlySelectedGroup");
    EXPECT_EQ(d.imminentPerilGroup.uri, "tel:g101");
    EXPECT_EQ(d.emergencyAlertGroup.uri, "tel:g101");
    EXPECT_FALSE(d.allowPrivateCall);                       // 인가 전부 false(1차 범위 밖)
    EXPECT_FALSE(d.allowEmergencyGroupCall);
    EXPECT_FALSE(d.allowEmergencyPrivateCall);
    EXPECT_FALSE(d.allowImminentPerilCall);
    EXPECT_FALSE(d.allowActivateEmergencyAlert);
    EXPECT_FALSE(d.allowRevokeTransmit);
    EXPECT_FALSE(d.allowRemoteAmbientViewing);
    EXPECT_FALSE(d.allowLocalAmbientViewing);
    EXPECT_FALSE(d.allowAdhocGroupCall);

    McVideoUserProfileDoc bare;                             // ruleset 이 없으면 허용(서버가 최종 판정)
    ASSERT_TRUE(McVideoUserProfileDoc::parse("<mcvideo-user-profile XUI-URI=\"tel:1\"/>", bare));
    EXPECT_TRUE(bare.allowRevokeTransmit);
    EXPECT_EQ(bare.maxSimultaneousVideoStreams, -1);
    EXPECT_FALSE(McVideoUserProfileDoc::parse("<mcptt-user-profile/>", bare));
}

// mcvideo_service_config.xml(TS 24.484 §9.4) — 참여자 타이머 T100~T104 = K5 정의 테이블 기본값(두 계약이 같은 값을 말한다)
TEST(McvConfig, ServiceConfig) {
    McVideoServiceConfigDoc d;
    std::string err;
    ASSERT_TRUE(McVideoServiceConfigDoc::parse(fixture("mcvideo_service_config.xml"), d, &err)) << err;
    EXPECT_EQ(d.domain, "ptt.cims.example.kr");
    EXPECT_FALSE(d.confidentialityProtection);
    EXPECT_FALSE(d.integrityProtection);
    EXPECT_EQ(d.rpEmergency, "mcpttp.15");
    EXPECT_EQ(d.rpImminentPeril, "mcpttp.8");
    EXPECT_EQ(d.rpNormal, "mcpttp.0");
    EXPECT_EQ(d.t100Sec * 1000, mcvideo::timer::T100_MS);
    EXPECT_EQ(d.t101Sec * 1000, mcvideo::timer::T101_MS);
    EXPECT_EQ(d.t102Sec * 1000, mcvideo::timer::T102_MS);
    EXPECT_EQ(d.t103Sec * 1000, mcvideo::timer::T103_MS);
    EXPECT_EQ(d.t104Sec * 1000, mcvideo::timer::T104_MS);

    McVideoServiceConfigDoc bare;                           // signalling-protection 이 없으면 켜진 것(TS 24.281 §6.6.2.1)
    ASSERT_TRUE(McVideoServiceConfigDoc::parse(
        "<service-configuration-info><service-configuration-params domain=\"d\"><on-network/></service-configuration-params>"
        "</service-configuration-info>", bare));
    EXPECT_TRUE(bare.confidentialityProtection);
    EXPECT_TRUE(bare.integrityProtection);
    EXPECT_EQ(bare.t100Sec, -1);
}

// ue_init_config.xml(TS 24.484 §7.2) — <anyExt> MCPTT → MCVideo → MCData Service-Details
TEST(McvConfig, UeInitConfig) {
    UeInitConfigDoc d;
    ASSERT_TRUE(UeInitConfigDoc::parse(fixture("ue_init_config.xml"), d));
    EXPECT_EQ(d.mcvideoServerUri, "sip:mcvideo_psi@ptt.cims.example.kr");
    EXPECT_EQ(d.mcpttServerUri, "sip:mcptt_psi@ptt.cims.example.kr");
    EXPECT_EQ(d.mcdataServerUri, "sip:mcdata_psi@ptt.cims.example.kr");
}

// CMS XCAP 경로·Accept(csc/src/services/mcptt.py handle_mcvideo_*) · 404 = 자격 없음 · 304 규약
TEST(McvConfig, FetchPathsAndStatus) {
    auto tp = std::make_shared<FakeTransport>();
    CscEndpoint ep;
    ep.host = "csc.example";
    CscClient c(ep, tp);
    EXPECT_NE(ep.scope.find("3gpp:mc:video_config_management_service"), std::string::npos);

    tp->next.status = 200;
    tp->next.body = fixture("mcvideo_user_profile.xml");
    tp->next.headers = {{"etag", "\"up1\""}};
    McVideoUserProfileDoc up;
    Result r = c.fetchMcVideoUserProfile("tok", "tel:+82510002001", "", up);
    ASSERT_TRUE(r.ok) << r.reason;
    EXPECT_EQ(tp->lastUrl, ep.baseUrl() + "/org.3gpp.mcvideo.user-profile/users/" + CscClient::enc("tel:+82510002001") +
                               "/mcvideo-user-profile-1.xml");
    EXPECT_EQ(tp->lastHeaders.at("Accept"), "application/vnd.3gpp.mcvideo-user-profile+xml");
    EXPECT_EQ(tp->lastHeaders.at("Authorization"), "Bearer tok");
    EXPECT_EQ(up.etag, "\"up1\"");
    EXPECT_EQ(up.maxSimultaneousVideoStreams, 1);

    tp->next = http::Response();
    tp->next.status = 304;
    McVideoUserProfileDoc cached = up;
    ASSERT_TRUE(c.fetchMcVideoUserProfile("tok", "tel:+82510002001", "\"up1\"", cached).ok);
    EXPECT_TRUE(cached.notModified);
    EXPECT_EQ(tp->lastHeaders.at("If-None-Match"), "\"up1\"");

    tp->next = http::Response();
    tp->next.status = 404;                                  // MCVideo 이용 자격 없음
    McVideoUserProfileDoc none;
    r = c.fetchMcVideoUserProfile("tok", "tel:+82510002009", "", none);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, 404);
    EXPECT_FALSE(c.fetchMcVideoUserProfile("tok", "", "", none).ok);

    tp->next = http::Response();
    tp->next.status = 200;
    tp->next.body = fixture("mcvideo_service_config.xml");
    McVideoServiceConfigDoc sc;
    ASSERT_TRUE(c.fetchMcVideoServiceConfig("tok", "", sc).ok);
    EXPECT_EQ(tp->lastUrl, ep.baseUrl() + "/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml");
    EXPECT_EQ(tp->lastHeaders.at("Accept"), "application/vnd.3gpp.mcvideo-service-config+xml");
    EXPECT_EQ(sc.t103Sec, 1);
}
