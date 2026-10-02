// libcimsue 단위시험 — 등록에 묶인 게시·구독의 유지 규칙 (S1-UE-UNIT, ue_sdk.md §4.2 «등록에 묶인 것의 유지»)
// 노리는 것: 등록이 끊겼다 다시 선 것·망 변경 뒤 첫 성공을 놓치는 것(①·②), 첫 등록에 곧바로 겹쳐 싣는 것, 수명 절반 갱신(③)이
// 만료 없는 부여(2^32-1)까지 되풀이하는 것, 거절된 것을 매 분 다시 보내는 것, 끈 것을 되살리는 것. 그리고 Warning 해석(K1).
#include <gtest/gtest.h>

#include "../src/account_map.h"
#include "../src/upkeep.h"

using namespace cimsue::detail;

namespace {
const UpkeepKey kAff{1, UpkeepKind::McpttAffiliation, ""};
const UpkeepKey kConf{1, UpkeepKind::Conference, "g001"};
}  // namespace

TEST(Upkeep, RenewsWhenRegistrationComesBack) {
    Upkeep u;
    EXPECT_FALSE(u.onRegEvent(1, true));         // 첫 등록 — 처음 것은 앱이 건다
    EXPECT_FALSE(u.onRegEvent(1, true));         // 갱신 REGISTER — 그대로
    EXPECT_FALSE(u.onRegEvent(1, false));        // 끊김
    EXPECT_TRUE(u.onRegEvent(1, true));          // 다시 섰다 — 등록에 묶인 것을 다시 싣는다(①)
    EXPECT_FALSE(u.onRegEvent(1, true));
}

TEST(Upkeep, RenewsAfterNetworkChangeEvenIfStillRegistered) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.onRegEvent(2, true);
    u.networkChanged({1});
    EXPECT_FALSE(u.onRegEvent(2, true));         // 망 변경 표시가 없는 계정
    EXPECT_FALSE(u.onRegEvent(1, false));        // 실패는 표시를 지우지 않는다
    EXPECT_TRUE(u.onRegEvent(1, true));          // 망 변경 뒤 첫 성공(②)
    EXPECT_FALSE(u.onRegEvent(1, true));         // 한 번만
}

TEST(Upkeep, RefreshesAtHalfLifetime) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.want(kConf);
    u.sent(kConf, 0);
    EXPECT_TRUE(u.due(10).empty());              // 응답 대기
    u.result(kConf, 200, 3600, 1000);
    EXPECT_TRUE(u.due(1000 + 1799 * 1000).empty());
    auto d = u.due(1000 + 1800 * 1000);          // 수명 절반(③)
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0], kConf);
}

TEST(Upkeep, NoRefreshForUnlimitedGrant) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.want(kAff);
    u.sent(kAff, 0);
    u.result(kAff, 200, Upkeep::kNoExpirySec, 0);   // 규격형 제휴(TS 24.379 §9.2.2.2.3 8)a))
    EXPECT_TRUE(u.due(10LL * 24 * 3600 * 1000).empty());
}

TEST(Upkeep, MissingExpiresUsesRequestedLifetime) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.want(kConf);
    u.result(kConf, 202, 0, 0);
    EXPECT_EQ(u.due(Upkeep::kDefaultLifetimeSec * 500).size(), 1u);
}

TEST(Upkeep, BacksOffAfterRejection) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.want(kAff);
    u.sent(kAff, 0);
    u.result(kAff, 403, 0, 0);
    EXPECT_TRUE(u.failing(kAff));
    EXPECT_TRUE(u.due(59999).empty());
    EXPECT_EQ(u.due(60000).size(), 1u);          // 1분
    u.sent(kAff, 60000);
    u.result(kAff, 500, 0, 60000);
    EXPECT_TRUE(u.due(60000 + 119999).empty());
    EXPECT_EQ(u.due(60000 + 120000).size(), 1u); // 배로
    EXPECT_EQ(Upkeep::retryDelayMs(9), Upkeep::kRetryMaxMs);   // 최대 30분
    u.result(kAff, 200, 3600, 200000);
    EXPECT_FALSE(u.failing(kAff));
}

TEST(Upkeep, NoAnswerCountsAsFailure) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.want(kConf);
    u.sent(kConf, 0);
    EXPECT_TRUE(u.due(Upkeep::kInflightTimeoutMs - 1).empty());
    EXPECT_TRUE(u.due(Upkeep::kInflightTimeoutMs).empty());     // 실패로 바뀌고 물러남이 시작된다(보낸 시각부터 1분)
    EXPECT_TRUE(u.failing(kConf));
    EXPECT_EQ(u.due(60000).size(), 1u);
}

TEST(Upkeep, OnlyRegisteredAccountsAndWantedItems) {
    Upkeep u;
    u.want(kConf);
    u.result(kConf, 200, 60, 0);
    EXPECT_TRUE(u.due(60000).empty());           // 등록 안 된 계정은 싣지 않는다(등록되면 ①이 싣는다)
    u.onRegEvent(1, true);
    EXPECT_EQ(u.due(60000).size(), 1u);
    u.unwant(kConf);                             // 앱이 껐다 — 되살리지 않는다
    EXPECT_TRUE(u.due(60000).empty());
    u.result(kConf, 200, 60, 0);                 // 해지 요청의 결과는 무시
    EXPECT_FALSE(u.wanted(kConf));
    EXPECT_TRUE(u.wantedFor(1).empty());
}

TEST(Upkeep, DropAccountForgetsEverything) {
    Upkeep u;
    u.onRegEvent(1, true);
    u.want(kAff);
    u.want(kConf);
    u.want({2, UpkeepKind::Dialog, "sip:+8213000000002@volte.test"});
    EXPECT_EQ(u.wantedFor(1).size(), 2u);
    u.dropAccount(1);
    EXPECT_TRUE(u.wantedFor(1).empty());
    EXPECT_EQ(u.size(), 1u);
    EXPECT_FALSE(u.registered(1));
}

TEST(Warning, ParsesFirstValue) {
    int code = 0;
    std::string text;
    ASSERT_TRUE(parseWarning("120 ptt.test \"user is not affiliated to this group\"", code, text));
    EXPECT_EQ(code, 120);
    EXPECT_EQ(text, "user is not affiliated to this group");
    ASSERT_TRUE(parseWarning("399 csp \"a \\\"quoted\\\" word\", 301 x \"second\"", code, text));
    EXPECT_EQ(code, 399);
    EXPECT_EQ(text, "a \"quoted\" word");
    ASSERT_TRUE(parseWarning("118 host", code, text));       // warn-text 없음 — 코드만
    EXPECT_EQ(code, 118);
    EXPECT_TRUE(text.empty());
    EXPECT_FALSE(parseWarning("abc host \"x\"", code, text));
    EXPECT_EQ(code, 0);
    EXPECT_FALSE(parseWarning("1200 host \"x\"", code, text));
    EXPECT_FALSE(parseWarning("", code, text));
}
