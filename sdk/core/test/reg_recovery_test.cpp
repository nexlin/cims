// libcimsue 단위시험 — 망 변경 뒤 재등록의 줄 (S1-UE-UNIT, ue_sdk.md §4.2)
// 노리는 것: 앞 REGISTER 가 걸린 채 새 REGISTER 를 겹쳐 보내는 것(RFC 3261 §10.2), 등록을 끈 계정을 망 변경이 되살리는 것,
// 미뤄 둔 재등록이 두 번 나가는 것.
#include <gtest/gtest.h>

#include "../src/reg_recovery.h"

using namespace cimsue::detail;

TEST(RegRecovery, OnlyWantedAccountsAreRecovered) {
    RegRecovery r;
    r.want(1); r.want(2); r.unwant(2);
    EXPECT_EQ(r.targets(), std::vector<int>({1}));
}

TEST(RegRecovery, BusyWaitsForTheInFlightRegistration) {
    RegRecovery r;
    r.want(1);
    r.busy(1);                                   // 앞 등록 진행 중 — 지금은 보내지 않는다
    EXPECT_TRUE(r.settled(1));                   // 그 트랜잭션이 끝나면(성공이든 실패든) 한 번 더
    EXPECT_FALSE(r.settled(1));                  // 한 번만
}

TEST(RegRecovery, SettledWithoutPendingDoesNothing) {
    RegRecovery r;
    r.want(1);
    EXPECT_FALSE(r.settled(1));                  // 평소의 등록 결과(갱신 등)는 재등록을 부르지 않는다
}

TEST(RegRecovery, UnwantedAccountIsNeverRevived) {
    RegRecovery r;
    r.want(1);
    r.busy(1);
    r.unwant(1);                                 // 로그아웃·계정 제거 — 미뤄 둔 것도 버린다
    EXPECT_FALSE(r.settled(1));
    r.busy(3);                                   // 켠 적 없는 계정의 거절은 표시하지 않는다
    EXPECT_FALSE(r.settled(3));
}
