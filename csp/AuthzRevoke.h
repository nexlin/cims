#ifndef _AUTHZ_REVOKE_H_
#define _AUTHZ_REVOKE_H_

#include <string>

/**
 * @ingroup CspServer
 * @brief 인가 회수 — 역할(배정·범위)이 바뀌면 그 역할로 이미 성립한 것을 실제로 걷는다.
 *
 * 왜 따로 있나(dispatch_center.md §5.10). 인가는 **성립 시점에 한 번** 판정하고 그 결과를 세션에
 * 담아 둔다 — 구독은 dialog 에, 감청 leg 는 tap 에, PTT 청취 leg 는 그룹 세션에. 그래서 역할을
 * 거둬도 이미 선 것은 저절로 무너지지 않는다. 구독은 최대 1시간(RFC 6665 §4.2.1.1) 갱신으로 살고,
 * leg 는 통화가 끝날 때까지 산다. 자격 회수가 **즉시**여야 하는 권한(감청·청취)에서 이것은 구멍이다.
 *
 * 회수는 두 겹이다.
 *   ① 능동 종료 — 여기. 역할 재적재(ROLE_CHANGED·USER_CHANGED) 직후 전수 재판정해 잃은 것을 걷는다.
 *   ② 갱신 시 재검사 — CscfModule 의 SUBSCRIBE 경로. ①을 놓친 잔여를 다음 갱신에서 막는다.
 *
 * 종료 사유는 RFC 6665 §4.1.3 `rejected` 를 쓴다 — 규격이 "terminated due to change in authorization
 * policy" 로 정의한 값이고, 구독자에게 재구독하지 말라는 뜻까지 실어 보낸다. `timeout`(만료)·
 * `deactivated`(즉시 재구독 권장)와 뜻이 다르다.
 */
namespace CspAuthz {

    /**
     * @brief 인가를 잃은 구독·감청 leg·PTT 청취 leg 를 걷는다.
     * @param pszWhy 로그·감사에 남길 계기 (예: "ROLE_CHANGED", "USER_CHANGED").
     * @return 걷어낸 건수 합계.
     *
     * 역할 맵(gclsRoleMap)이 이미 새 내용으로 적재된 뒤에 부른다 — 이 함수는 재적재하지 않는다.
     */
    int RevokeUnauthorized( const char *pszWhy );

    /**
     * @brief 인가 정책 세대 — 역할·전화 그룹이 바뀔 때마다 증가한다.
     *
     * 왜 필요한가. 스윕은 **이미 등록된** 성립물만 본다. 그런데 감청 leg·PTT 청취 leg 은 «인가 판정» 과
     * «맵 등록» 사이에 CMP 왕복이 끼어 수백 ms 가 걸린다. 그 사이에 자격을 거두면 스윕은 아직 없는 leg 을
     * 지나치고, 등록 경로는 판정을 다시 하지 않아 **권한 없는 leg 이 확립된다.**
     *
     * 그래서 개설 경로는 판정 직후 세대를 적어 두고, 등록 직전에 세대가 달라졌으면 **같은 판정을 한 번 더**
     * 한다. 세대 비교만으로 회수하지 않는 이유는 오탐 때문이다 — 맵 재적재와 세대 증가 사이에 시작한 개설은
     * 이미 새 정책으로 판정했는데도 세대가 달라 보인다. 재판정은 어느 경우에나 옳은 답을 낸다.
     */
    unsigned PolicyGeneration();

    /** 인가 근거가 바뀌었음을 알린다 — `RevokeUnauthorized` 가 스윕 전에 스스로 부른다. */
    void BumpPolicyGeneration();

    /**
     * @brief 정책 적재가 실패해 **회수를 못 했다**고 기록한다.
     *
     * 적재가 실패하면 판정 근거가 낡았으므로 그 자리에서 걷지 않는다(§5.10). 문제는 그것이 «지연» 이 아니라
     * «유실» 이라는 점이다 — 통지는 한 번뿐이라, DB 가 복구돼도 **다음 변경 통지가 올 때까지 옛 권한이 그대로
     * 유지된다.** 통지를 잃지 않으려면 빚으로 남겨 두고 갚아야 한다.
     */
    void NotePolicyReloadOwed( const char *pszWhy );

    /**
     * @brief 밀린 정책 적재를 다시 시도하고, 성공하면 그때 회수한다. 1초 주기에서 부른다.
     *
     * 빚이 없으면 즉시 반환한다. 재시도 간격은 2·4·8…60초로 늘리되 **포기하지 않는다** — 자원 회수와 달리
     * 정책 반영은 대신 갚아 줄 최종 안전망이 없다.
     */
    void RetryPendingPolicyReload();

}  // namespace CspAuthz

#endif
