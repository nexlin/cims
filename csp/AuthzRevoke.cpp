/**
 * @file AuthzRevoke.cpp
 * @brief 인가 회수 — 역할이 바뀌면 그 역할로 성립한 구독·leg 을 걷는다 (dispatch_center.md §5.10).
 */
#include "AuthzRevoke.h"

#include <atomic>
#include <list>
#include <mutex>
#include <string>

#include "CspPhoneGroup.h"
#include "CspRole.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupCallService.h"
#include "Log.h"
#include "ModuleDispatcher.h"
#include "SubscriptionManager.h"
#include "TasModule.h"

extern void SendTerminatedNotify( const SubscriptionInfo &sub, const char *pszReason = "timeout" );

/** 감시 대상 AoR → 판정에 쓸 전화 그룹 id. 대표번호면 그 그룹, 아니면 가입자의 소속 그룹.
 *  CscfModule 의 SUBSCRIBE 인가와 **같은 규칙**이어야 한다 — 다르면 통과한 구독을 스윕이 걷거나 그 반대가 된다. */
static std::string AuthzWatchedGroupOf( const std::string &strWatchedAor ) {
    CspPhoneGroup clsPilotGroup;
    if ( gclsPhoneGroupMap.SelectByPilot( strWatchedAor.c_str(), clsPilotGroup ) ) return clsPilotGroup.m_strId;
    return gclsPhoneGroupMap.EffectiveGroupOf( strWatchedAor.c_str() );
}

/** 종료 사유(RFC 6665 §4.1.3) — 계기에 따라 다르다.
 *
 *  `rejected`("terminated due to change in authorization policy", 재구독하지 말 것)는 **인가 정책 자체가 바뀐**
 *  계기에만 쓴다. 전화 그룹 멤버십·회선 변경은 CSC 가 한 사람의 이동을 **통지 둘**(옛 그룹 PUT + 새 그룹 POST)로
 *  보내므로, 그 사이에 스윕이 돌면 «아직 어느 그룹에도 없는» 순간을 볼 수 있다. 그때 `rejected` 를 보내면 정당한
 *  관제사가 재구독하지 않아 **영구히 눈이 먼다.** 그래서 이쪽은 `deactivated`("SHOULD retry immediately")로 보내
 *  즉시 재구독하게 하고 서버가 그때 다시 판정한다 — 자격을 정말 잃었으면 그 재구독이 403 이므로 회수는 그대로
 *  성립한다. 즉 사유 선택은 **안전성이 아니라 복구 가능성**의 문제다. */
static const char *AuthzTerminateReason( const char *pszWhy ) {
    const std::string strWhy = pszWhy ? pszWhy : "";
    if ( strWhy == "PHONE_GROUP_CHANGED" || strWhy == "USER_CHANGED" ) return "deactivated";
    return "rejected";
}

/** dialog 구독 스윕 — 감시 인가(CanWatch)를 잃은 구독을 걷는다.
 *  자기 자신 감시는 역할과 무관하므로 건너뛴다(SUBSCRIBE 경로와 같은 예외). */
static int AuthzSweepDialogSubscriptions( const char *pszWhy ) {
    std::list<SubscriptionInfo> lstSubs;
    gclsSubscriptionManager.GetSubscriptionsByEvent( "dialog", lstSubs );
    int iRevoked = 0;
    for ( const auto &sub : lstSubs ) {
        if ( sub.strResourceId.empty() || sub.strResourceId == sub.strUserId ) continue;
        const std::string strGroup = AuthzWatchedGroupOf( sub.strResourceId );
        if ( gclsRoleMap.CanWatch( sub.strUserId.c_str(), strGroup ) ) continue;
        CLog::Print( LOG_INFO, "AuthzRevoke(%s): dialog subscription revoked — %s watch %s (group %s, role %s)", pszWhy,
                     sub.strUserId.c_str(), sub.strResourceId.c_str(), strGroup.c_str(),
                     gclsRoleMap.RoleIdForLine( sub.strUserId.c_str() ).c_str() );
        SendTerminatedNotify( sub, AuthzTerminateReason( pszWhy ) );
        gclsSubscriptionManager.RemoveSubscription( sub.strCallId );
        ++iRevoked;
    }
    return iRevoked;
}

/** conference 구독 스윕 — 청취 인가를 잃은 구독을 걷는다. 판정은 SUBSCRIBE 경로와 같은 함수다. */
static int AuthzSweepConferenceSubscriptions( const char *pszWhy ) {
    std::list<SubscriptionInfo> lstSubs;
    gclsSubscriptionManager.GetSubscriptionsByEvent( "conference", lstSubs );
    int iRevoked = 0;
    for ( const auto &sub : lstSubs ) {
        if ( sub.strResourceId.empty() ) continue;
        std::string strWarning, strReason;
        bool bUnavail = false;
        if ( CGroupCallService::CheckConferenceSubscribe( sub.strResourceId, sub.strUserId, strWarning, strReason,
                                                          &bUnavail ) == 0 )
            continue;
        if ( bUnavail ) {
            // **조회 불능은 권한 상실이 아니다** — 이미 선 구독은 걷지 않는다(fail open, §5.10). 다만
            //   «걷지 않는다» 와 «잊는다» 는 다르다 — 빚으로 남겨야 DB 가 복구된 뒤 다시 판정한다.
            CLog::Print( LOG_ERROR, "AuthzRevoke(%s): conference 구독 판정 불능 — %s on %s (%s) — 회수 보류", pszWhy,
                         sub.strUserId.c_str(), sub.strResourceId.c_str(), strReason.c_str() );
            CspAuthz::NotePolicyReloadOwed( pszWhy );
            continue;
        }
        CLog::Print( LOG_INFO, "AuthzRevoke(%s): conference subscription revoked — %s on group %s (%s)", pszWhy,
                     sub.strUserId.c_str(), sub.strResourceId.c_str(), strReason.c_str() );
        SendTerminatedNotify( sub, AuthzTerminateReason( pszWhy ) );
        gclsSubscriptionManager.RemoveSubscription( sub.strCallId );
        ++iRevoked;
    }
    return iRevoked;
}

namespace CspAuthz {

    static std::atomic<unsigned> g_uPolicyGen{ 1 };

    unsigned PolicyGeneration() {
        return g_uPolicyGen.load( std::memory_order_acquire );
    }

    void BumpPolicyGeneration() {
        g_uPolicyGen.fetch_add( 1, std::memory_order_acq_rel );
    }

    // ── 밀린 정책 적재 ──────────────────────────────────────────────────────────
    static std::mutex g_clsOwedMutex;
    static bool g_bReloadOwed = false;
    static std::string g_strOwedWhy;
    static time_t g_tNextReloadTry = 0;
    static int g_iReloadTries = 0;

    // 빚에 **세대**를 붙인다. 적재는 락을 놓고 도는데(DB 왕복), 그 사이 다른 스레드(CSC 수신)가 새 실패를
    //   기록할 수 있다. 완료 처리에서 플래그를 무조건 내리면 **그 새 빚까지 지워진다** — 정책 변경 하나가
    //   통째로 유실된다. 그래서 «내가 집어 온 세대» 와 지금 세대가 같을 때만 갚은 것으로 친다.
    static unsigned g_uOwedSeq = 0;

    void NotePolicyReloadOwed( const char *pszWhy ) {
        std::lock_guard<std::mutex> lock( g_clsOwedMutex );
        if ( !g_bReloadOwed ) {
            g_bReloadOwed = true;
            g_iReloadTries = 0;
            g_strOwedWhy = pszWhy ? pszWhy : "authz";
            g_tNextReloadTry = time( NULL ) + 2;  // 첫 기록만 기한을 잡는다
        }
        // 이미 빚이 있으면 **기한을 미루지 않는다** — 연속 실패 통지가 올 때마다 2초씩 밀면 영원히 안 돈다.
        ++g_uOwedSeq;
    }

    void RetryPendingPolicyReload() {
        std::string strWhy;
        unsigned uSeq = 0;
        {
            std::lock_guard<std::mutex> lock( g_clsOwedMutex );
            if ( !g_bReloadOwed || g_tNextReloadTry > time( NULL ) ) return;
            strWhy = g_strOwedWhy;
            uSeq = g_uOwedSeq;
        }
        // 어느 맵이 실패했는지 가리지 않고 둘 다 다시 적재한다 — 적재는 싸고, 가리려다 틀리면 빚이 남는다.
        //   사용자 캐시도 같이 읽는다 — `EffectiveGroupOf` 의 폴백이 그 캐시라, 낡은 채로 판정하면
        //   그룹에서 빠진 사람이 «여전히 같은 그룹» 으로 보인다(§5.10 의 PUT 계기와 같은 이유).
        //   `LoadFromDb` 의 반환값은 «적재된 행이 있는가» 라 가입자 0명과 조회 실패를 못 가른다 —
        //   빚의 성패는 **조회 불능** 여부로 판정한다(0명인 현장을 영구 미납으로 만들지 않게).
        bool bUsersUnavail = false;
        gclsCspUserMap.LoadFromDb( &bUsersUnavail );
        bool bOk = !bUsersUnavail;
        if ( gclsDbManager.HasPhoneGroupTables() ) bOk = gclsPhoneGroupMap.LoadFromDb() && bOk;
        if ( gclsDbManager.HasRoleTables() ) bOk = gclsRoleMap.LoadFromDb() && bOk;
        if ( bOk ) {
            bool bStillMine = false;
            {
                std::lock_guard<std::mutex> lock( g_clsOwedMutex );
                bStillMine = ( g_uOwedSeq == uSeq );
                if ( bStillMine ) {
                    g_bReloadOwed = false;
                    g_iReloadTries = 0;
                } else {
                    // 적재 도중 새 빚이 들어왔다 — 지우지 않고 바로 다시 돌게 둔다.
                    g_tNextReloadTry = time( NULL );
                }
            }
            CLog::Print( LOG_SYSTEM, "AuthzRevoke: 밀린 정책 적재 성공(%s)%s — 회수한다", strWhy.c_str(),
                         bStillMine ? "" : " · 도중 새 요청 있음(빚 유지)" );
            RevokeUnauthorized( strWhy.c_str() );
            return;
        }
        std::lock_guard<std::mutex> lock( g_clsOwedMutex );
        ++g_iReloadTries;
        int iWait = 2 << ( g_iReloadTries < 5 ? g_iReloadTries : 5 );  // 4·8·16·32·64 → 상한 60
        if ( iWait > 60 ) iWait = 60;
        g_tNextReloadTry = time( NULL ) + iWait;
        if ( g_iReloadTries <= 3 || g_iReloadTries % 20 == 0 )  // 로그 폭주 방지
            CLog::Print( LOG_ERROR, "AuthzRevoke: 밀린 정책 적재 %d회 실패(%s) — %d초 뒤 재시도", g_iReloadTries,
                         strWhy.c_str(), iWait );
    }

    int RevokeUnauthorized( const char *pszWhy ) {
        const char *pszReason = ( pszWhy && *pszWhy ) ? pszWhy : "authz";
        // 스윕보다 **먼저** 올린다 — 개설 중이라 스윕이 못 본 leg 이 등록 때 재판정을 받게 하려면,
        //   그 leg 의 판정 시점 세대보다 큰 값이 스윕 전에 서 있어야 한다.
        BumpPolicyGeneration();
        int iTotal = 0;
        iTotal += AuthzSweepDialogSubscriptions( pszReason );
        iTotal += AuthzSweepConferenceSubscriptions( pszReason );
        CTasModule *pclsTas = gclsDispatcher.GetTas();
        if ( pclsTas ) iTotal += pclsTas->RevokeUnauthorizedMonitors( pszReason );
        iTotal += gclsGroupCallService.RevokeUnauthorizedListeners( pszReason );
        if ( iTotal > 0 )
            CLog::Print( LOG_SYSTEM, "AuthzRevoke(%s): %d subscription(s)/leg(s) revoked", pszReason, iTotal );
        return iTotal;
    }

}  // namespace CspAuthz
