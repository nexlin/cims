/**
 * @file AuthzRevoke.cpp
 * @brief 인가 회수 — 역할이 바뀌면 그 역할로 성립한 구독·leg 을 걷는다 (dispatch_center.md §5.10).
 */
#include "AuthzRevoke.h"

#include <atomic>
#include <list>
#include <string>

#include "CspPhoneGroup.h"
#include "CspRole.h"
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
        if ( CGroupCallService::CheckConferenceSubscribe( sub.strResourceId, sub.strUserId, strWarning, strReason ) ==
             0 )
            continue;
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
