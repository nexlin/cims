/**
 * CTasModule — VoLTE 보조 서비스 (volte_supplementary_services.md)
 *
 * DND/착신거부/착신전환·당겨받기(피처코드/INVITE-Replaces)·호 전달(REFER blind/attended)·
 * dialog 이벤트 패키지(RFC 4235). B2BUA 골격(라우팅·relay 수명)은 ModuleDispatcher 가 유지하고,
 * 이 모듈은 보조 서비스 판정과 leg 재고정(RELAY_MODIFY — cmp_media_api.md §6.2)을 수행한다.
 * INVITE 경로에 DB 질의를 넣지 않는다 — 모든 판정은 인메모리 맵에서 답한다.
 */

#include "TasModule.h"

#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspAddressing.h"
#include "CspDispatchGroup.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "FmReporter.h"
#include "GroupCallService.h"
#include "Log.h"
#include "MediaSdes.h"
#include "ModuleDispatcher.h"
#include "RtpMap.h"  // SOCKET_COUNT_PER_MEDIA
#include "SipMessageLogger.h"
#include "SipServerSetup.h"
#include "SipStackThread.h"  // GetCurrentInboundListenerId()
#include "UserMap.h"

extern void SendDialogEventNotify( const std::string &strWatchedAor, const std::string &strDlgCallId,
                                   const std::string &strState, const std::string &strDir,
                                   const std::string &strLocalAor, const std::string &strRemoteAor,
                                   const std::string &strLocalTag, const std::string &strRemoteTag );

bool CTasModule::IsEnabled() const {
    return gclsSetup.m_bRoleTas;
}

// ──────────────────────────────────────────────────────────────
//  RecvRequest 시점 게이트
// ──────────────────────────────────────────────────────────────

bool CTasModule::OnSipRequest( int iThreadId, CSipMessage *pclsMessage ) {
    (void)iThreadId;

    // REFER(호 전달) 게이트 — 접속서비스 transfer_allowed=false 가입자의 전달 요청은 403
    //   (volte_supplementary_services.md §6.3). 통과 시 기존 흐름대로 psip 이 REFER 를 종단한다
    //   (B2BUA — OnTransfer/OnBlindTransfer).
    if ( pclsMessage->IsMethod( SIP_METHOD_REFER ) ) {
        const std::string strReferFrom = pclsMessage->m_clsFrom.m_clsUri.m_strUser;
        ServiceInfo clsXferSvc = gclsServiceMap.GetForUser( strReferFrom, "volte" );
        if ( clsXferSvc.id > 0 && clsXferSvc.transfer_allowed == false ) {
            CLog::Print( LOG_INFO, "TAS: REFER from(%s) denied — service '%s' transfer_allowed=false → 403",
                         strReferFrom.c_str(), clsXferSvc.name.c_str() );
            gclsDispatcher.SendResponse( pclsMessage, SIP_FORBIDDEN );
            return true;
        }
    }
    return false;
}

bool CTasModule::ScreenInvite( CSipMessage *pclsMessage, const char *pszFrom, const char *pszTo ) {
    CspUser clsToUser;
    if ( gclsCspUserMap.isAlive( pszTo, clsToUser ) ) {
        if ( clsToUser.isDnd() || clsToUser.isReject( pszFrom ) ) {
            CLog::Print( LOG_INFO, "TAS: Rejected (DND/Reject) From=%s To=%s", pszFrom, pszTo );
            gclsDispatcher.SendResponse( pclsMessage, SIP_DECLINE );
            return true;
        }

        // 서비스 모드 체크
        if ( gclsSetup.m_strServiceMode == "ptt" ) {
            gclsDispatcher.SendResponse( pclsMessage, SIP_FORBIDDEN );
            return true;
        }
    }
    return false;
}

// ──────────────────────────────────────────────────────────────
//  착신 서비스 — Replaces / DND·착신거부·착신전환 / 픽업 다이얼
// ──────────────────────────────────────────────────────────────

EModuleRouteResult CTasModule::OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                               CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    (void)pszTo;
    // 수신 INVITE 의 Replaces(RFC 3891) — 관제 BLF 당겨받기·표준 attended 완결. 헤더가 있으면
    //   대상 다이얼로그를 pszCallId 로 교체하고 여기서 종결한다(정상 라우팅 미진입).
    if ( HandleIncomingReplaces( pszCallId, pszFrom, pclsRtp, pclsMessage ) ) return E_ROUTE_HANDLED;
    // 수신 INVITE 의 Join(RFC 3911) — 업무망 합법감청 합류 (dispatch_center.md §5.3)
    if ( HandleIncomingJoin( pszCallId, pszFrom, pclsRtp, pclsMessage ) ) return E_ROUTE_HANDLED;
    return E_ROUTE_PASS;
}

bool CTasModule::ApplyTerminationServices( const char *pszCallId, const char *pszFrom, const CspUser &clsUser ) {
    if ( clsUser.isDnd() || clsUser.isReject( pszFrom ) ) {
        gclsDispatcher.StopCall( pszCallId, SIP_DECLINE );
        return true;
    }

    if ( clsUser.isCallForward() ) {
        CSipMessage *pclsInvite = gclsUserAgent.DeleteIncomingCall( pszCallId );
        if ( pclsInvite ) {
            CSipMessage *pclsResponse = pclsInvite->CreateResponseWithToTag( SIP_MOVED_TEMPORARILY );
            if ( pclsResponse ) {
                CSipFrom clsContact;
                clsContact.m_clsUri.m_strProtocol = SIP_PROTOCOL;
                clsContact.m_clsUri.m_strUser = clsUser.m_strForward;
                // T4: 302 Moved Temporarily 는 수신 listener 의 bind_ip:bind_port 로 Contact 생성.
                const int iListenerId = GetCurrentInboundListenerId();
                clsContact.m_clsUri.m_strHost = CspAddressing::GetLocalSipAddress( iListenerId );
                clsContact.m_clsUri.m_iPort = CspAddressing::GetLocalSipPort( iListenerId, gclsSetup.m_iUdpPort );
                pclsResponse->m_clsContactList.push_back( clsContact );
                gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
                return true;
            }
        }
        gclsDispatcher.StopCall( pszCallId, SIP_MOVED_TEMPORARILY );
        return true;
    }
    return false;
}

bool CTasModule::TryPickupDial( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp ) {
    std::string strPickupTarget;
    if ( IsPickupDial( pszFrom, pszTo, strPickupTarget ) == false ) return false;
    gclsDispatcher.SetCallOwner( pszCallId, this );
    PickUp( pszCallId, pszFrom, strPickupTarget.empty() ? NULL : strPickupTarget.c_str(), pclsRtp );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  dialog 이벤트 패키지 (RFC 4235) + blind transfer 진행/완결
// ──────────────────────────────────────────────────────────────

// dialog-event 상태 통지 — 호의 두 당사자 각각에게 "그 당사자가 가진 dialog"(자기 leg Call-ID) 를 자기 감시자에게
//   낸다. 당사자·개시 방향은 CallLegParty(leg 원단 사용자·CSP 수신 leg 여부)로 읽는다 — psip From/To 를
//   caller/callee 로 읽으면 수신 leg(A) 에서 BYE 가 왔을 때 두 당사자가 뒤바뀐다(대표번호 A-leg BYE 결함).
//   local = entity 자신, remote = 상대 leg 의 원단, direction = 개시자면 initiator (RFC 4235 §4.1).
//   picker 는 착신자 entity 의 dialog id(B-leg Call-ID)를 Replaces 대상으로 쓴다.
void CTasModule::NotifyDialogState( const char *pszCallId, const char *pszState ) {
    CallLegParty clsThis, clsPeer;
    if ( !gclsCallMap.ResolveLegParties( pszCallId, clsThis, clsPeer ) ) return;
    const CallLegParty *arr[2] = { &clsThis, &clsPeer };
    for ( int i = 0; i < 2; ++i ) {
        const CallLegParty &clsMe = *arr[i], &clsOther = *arr[1 - i];
        if ( clsMe.strUser.empty() || clsMe.strCallId.empty() ) continue;
        std::string lt, rt;
        gclsUserAgent.GetDialogTags( clsMe.strCallId.c_str(), lt, rt );
        SendDialogEventNotify( clsMe.strUser, clsMe.strCallId, pszState, clsMe.bInitiator ? "initiator" : "recipient",
                               clsMe.strUser, clsOther.strUser, lt, rt );
    }
}

bool CTasModule::OnCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) {
    (void)pclsRtp;
    // 대표번호 포크 대기 leg — 첫 180 만 A 에게 전달, 183 은 전달하지 않는다 (dispatch_center.md §4.3)
    if ( OnForkRing( pszCallId, iSipStatus ) ) return true;
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        // dialog-event: 착신 링잉(early) 통지 — 감시자(BLF)가 당겨받기 대상을 알 수 있게 (§6.2)
        if ( iSipStatus >= 180 && iSipStatus < 200 ) NotifyDialogState( pszCallId, "early" );
        return false;
    }
    if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) ) {
        // blind transfer 진행 보고 — 전환 leg 의 18x 를 REFER 지시자에게 NOTIFY (RFC 3515)
        gclsUserAgent.SendNotify( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus );
        return true;
    }
    return false;
}

bool CTasModule::OnCallEnd( const char *pszCallId, int iSipStatus ) {
    // 감청 leg(M) BYE — CMP tap 회수 (dispatch_center.md §5.3)
    if ( HandleMonitorLegEnd( pszCallId ) ) return true;
    // 대표번호 포크 — 대기 leg 최종 응답 / A 취소 (dispatch_center.md §4.4)
    if ( OnForkEnd( pszCallId, iSipStatus ) ) return true;
    // 원 통화 종료 — 그 세션의 감청 leg 를 회수한다 (CallMap leg 삭제 전에 relay session id 를 읽는다)
    {
        CCallInfo clsCiForMon;
        if ( gclsCallMap.Select( pszCallId, clsCiForMon ) && !clsCiForMon.m_strRelaySessionId.empty() )
            ReleaseSessionMonitors( clsCiForMon.m_strRelaySessionId );
    }
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        // dialog-event: 종료(terminated) 통지 — CallMap 삭제 전에 leg 식별이 살아 있을 때 낸다 (§6.2).
        NotifyDialogState( pszCallId, "terminated" );
        // 대표번호로 확립된 호 — 대표번호 AoR 감시자에게도 terminated 1회 (§4.5). id·remote 는 포크 집합에서
        //   고정한 값(A-leg Call-ID·발신자)을 그대로 써 어느 leg 의 BYE 든 early/confirmed 와 같은 dialog 를 닫는다.
        {
            std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
            auto it = m_mapPilotOfCall.find( pszCallId );
            if ( it != m_mapPilotOfCall.end() ) {
                const PilotCall clsPilotCall = it->second;
                std::string lt, rt;
                gclsUserAgent.GetDialogTags( clsPilotCall.strACallId.c_str(), lt, rt );
                SendDialogEventNotify( clsPilotCall.strPilot, clsPilotCall.strACallId, "terminated", "recipient",
                                       clsPilotCall.strPilot, clsPilotCall.strCaller, lt, rt );
                m_mapPilotOfCall.erase( it );
                m_mapPilotOfCall.erase( clsCallInfo.m_strPeerCallId );
            }
        }
        return false;
    }
    std::string strCallId;
    if ( gclsTransCallMap.Select( pszCallId, strCallId ) ) {
        // blind transfer 전환 leg 실패/종료 — REFER 지시자에게 최종 NOTIFY 후 trans entry 정리
        gclsUserAgent.SendNotify( strCallId.c_str(), iSipStatus );
        gclsTransCallMap.Delete( pszCallId );
        return true;
    }
    return false;
}

bool CTasModule::OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    // 대표번호 포크 — 최초 200 = 승자 (dispatch_center.md §4.4). 승자는 (A, 승자) 쌍을 CallMap 에 넣고
    //   false 를 돌려 디스패처의 정상 answer 경로(RELAY_MODIFY peer1·A 에게 200)가 이어받는다.
    if ( OnForkStart( pszCallId, pclsRtp ) ) return true;
    CCallInfo clsCallInfo;
    if ( gclsCallMap.Select( pszCallId ) ) {
        // dialog-event: 호 확립(confirmed) 통지 (§6.2). answer 처리는 디스패처 정상 경로가 계속한다.
        NotifyDialogState( pszCallId, "confirmed" );
        return false;
    }
    if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) == false ) return false;

    // blind transfer 완결 — 전환 대상 leg 의 answer. 원 통화의 relay 세션을 유지한 채 전환
    //   leg 를 RELAY_MODIFY 로 재고정하고 남는 leg 와 재결합한다 (포트 산술 금지 —
    //   cmp_media_api.md §6.2, media_security.md §5.2).
    const std::string strTransferorCallId = clsCallInfo.m_strPeerCallId;  // REFER 를 보낸(떠나는) leg
    CCallInfo clsOldInfo, clsStayInfo;
    if ( gclsCallMap.Select( strTransferorCallId.c_str(), clsOldInfo ) == false ||
         gclsCallMap.Select( clsOldInfo.m_strPeerCallId.c_str(), clsStayInfo ) == false ) {
        gclsUserAgent.SendNotify( strTransferorCallId.c_str(), SIP_OK );
        gclsTransCallMap.Delete( pszCallId, false );
        gclsUserAgent.StopCall( pszCallId );
        return true;
    }
    const std::string strStayCallId = clsOldInfo.m_strPeerCallId;  // 남는(전환받는) leg
    const bool bRelay = !clsOldInfo.m_strRelaySessionId.empty();
    const int iStayIdx = clsStayInfo.m_bRecv ? 0 : 1;  // 남는 leg 의 relay peer index
    const int iNewIdx = 1 - iStayIdx;                  // 전환 leg 가 승계하는 index

    // 전환 leg answer SDES 검증 — offer 상태는 OnBlindTransfer 가 trans entry 에 저장.
    //   SAVP offer 에 crypto 없는 answer 는 전환만 중단(평문 폴백 금지) — 원 통화는 유지.
    RelaySdesLeg clsNewLeg = clsCallInfo.m_clsSdesLeg[iNewIdx];
    CmpMediaCrypto clsNewAudioCrypto, clsNewVideoCrypto;
    if ( bRelay && pclsRtp &&
         ( !MediaSdes::EvalRelayAnswerSdes( pclsRtp->m_clsMediaList, "audio", clsNewLeg.clsAudio, clsNewAudioCrypto ) ||
           !MediaSdes::EvalRelayAnswerSdes( pclsRtp->m_clsMediaList, "video", clsNewLeg.clsVideo,
                                            clsNewVideoCrypto ) ) ) {
        CLog::Print( LOG_ERROR,
                     "OnCallStart: transfer leg SDES answer missing/mismatched crypto — 전환 중단, 원 통화 유지 "
                     "(CallId=%s)",
                     pszCallId );
        gclsUserAgent.SendNotify( strTransferorCallId.c_str(), SIP_NOT_ACCEPTABLE_HERE );
        gclsTransCallMap.Delete( pszCallId, false );
        gclsUserAgent.StopCall( pszCallId );
        return true;
    }
    gclsUserAgent.SendNotify( strTransferorCallId.c_str(), SIP_OK );

    std::string strNewUserId;
    gclsUserAgent.GetToId( pszCallId, strNewUserId );
    const std::string strNewCaller = iNewIdx == 0 ? strNewUserId : clsOldInfo.m_strRelayCaller;
    const std::string strNewCallee = iNewIdx == 1 ? strNewUserId : clsOldInfo.m_strRelayCallee;

    if ( bRelay && pclsRtp ) {
        // 전환 leg 를 기존 relay 의 승계 index 로 재고정 — 주소·NAT·PT·crypto (정상 answer 경로와 동형)
        int iAudioPort = pclsRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
        if ( iAudioPort > 0 ) {
            int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
            int iNewNat = 0;
            std::string strNewGuardIp;
            {
                ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strNewUserId, "volte" );
                std::string strSigIp;
                CUserInfo clsNewUserInfo;
                if ( !strNewUserId.empty() && gclsUserMap.Select( strNewUserId.c_str(), clsNewUserInfo ) )
                    strSigIp = clsNewUserInfo.m_strIp;
                if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strNewGuardIp ) ) iNewNat = 1;
            }
            int iNewPt = 0, iNewSrcPt = 0, iNewTePt = 0, iNewSrcTePt = 0;
            std::string strNewCodec;
            CGroupCallService::GetLegPt( pszCallId, true, iNewPt, iNewSrcPt, iNewTePt, iNewSrcTePt, &strNewCodec );
            gclsCmpClient.ModifySession(
                clsOldInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort, iVideoPort > 0 ? iVideoPort : 0, iNewIdx,
                strNewCaller, strNewCallee, clsOldInfo.m_strRelaySesId, iNewNat, strNewGuardIp, iNewPt, iNewSrcPt,
                iNewTePt, iNewSrcTePt, strNewCodec, clsNewAudioCrypto.bEnabled ? &clsNewAudioCrypto : NULL,
                clsNewVideoCrypto.bEnabled ? &clsNewVideoCrypto : NULL );
        }
    }

    // 떠나는 leg 종료 + 원 pair 해체 — relay 는 계속 쓰므로 회수 금지(bStopPort=false)
    gclsUserAgent.StopCall( strTransferorCallId.c_str() );
    gclsCallMap.Delete( strTransferorCallId.c_str(), false );

    // 새 pair — entry 포트 = 그 leg 의 peer 에게 광고하는 relay 포트(각 leg 포트 불변),
    //   m_bRecv 는 peer0 표식(EventReInvite/EventCallStart 의 index 판정 근거).
    if ( iStayIdx == 0 )
        gclsCallMap.Insert( strStayCallId.c_str(), pszCallId, clsStayInfo.m_iPeerRtpPort, clsOldInfo.m_iPeerRtpPort );
    else
        gclsCallMap.Insert( pszCallId, strStayCallId.c_str(), clsOldInfo.m_iPeerRtpPort, clsStayInfo.m_iPeerRtpPort );
    if ( bRelay ) {
        gclsCallMap.SetRelayInfo( pszCallId, clsOldInfo.m_strRelaySessionId, clsOldInfo.m_strRelaySesId,
                                  clsOldInfo.m_strRelayLocalIp, strNewCaller, strNewCallee );
        gclsCallMap.SetRelaySdesLeg( pszCallId, iStayIdx, clsOldInfo.m_clsSdesLeg[iStayIdx] );
        gclsCallMap.SetRelaySdesLeg( pszCallId, iNewIdx, clsNewLeg );
    }
    gclsCallMap.SetEstablished( pszCallId );

    // 남는 leg 로 re-INVITE — 전환 leg answer 를 남는 leg 상태로 재작성 + relay 주소·기존 포트 재광고
    if ( bRelay && pclsRtp && clsOldInfo.m_iPeerRtpPort > 0 ) {
        MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsOldInfo.m_clsSdesLeg[iStayIdx], true );
        std::string strRelayIp =
            clsOldInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsOldInfo.m_strRelayLocalIp;
        pclsRtp->SetIpPort( strRelayIp.c_str(), clsOldInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
    }
    gclsUserAgent.SendReInvite( strStayCallId.c_str(), pclsRtp );
    gclsTransCallMap.Delete( pszCallId, false );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  호 전달 (Call Transfer — volte_supplementary_services.md §6)
// ──────────────────────────────────────────────────────────────

bool CTasModule::OnTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreened ) {
    CCallInfo clsCallInfo, clsReferToCallInfo;
    CSipCallRtp clsRtp;

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) == false ) return false;
    if ( gclsCallMap.Select( pszReferToCallId, clsReferToCallInfo ) == false ) return false;

    // pszCallId/pszReferToCallId = 전환 지시자(REFER 발신 단말)의 두 다이얼로그 leg. 남는 당사자는
    //   각 pair 의 peer leg — 유지(stay) = 원 통화의 peer, 합류(join) = 상담 통화의 peer.
    //   원 통화의 relay 세션을 유지해 stay leg 의 미디어 앵커(광고된 relay 포트)를 불변으로 두고,
    //   전환 지시자가 쓰던 peer index 로 join 단말을 RELAY_MODIFY 재고정한다 (cmp_media_api.md §6.2).
    //   상담 통화의 relay 는 pair 해체 시 회수한다. (구 ±4 leg 블록 포트 산술 제거.)
    const std::string strStayCallId = clsCallInfo.m_strPeerCallId;
    const std::string strJoinCallId = clsReferToCallInfo.m_strPeerCallId;
    CCallInfo clsStayInfo, clsJoinInfo;
    if ( gclsCallMap.Select( strStayCallId.c_str(), clsStayInfo ) == false ) return false;
    if ( gclsCallMap.Select( strJoinCallId.c_str(), clsJoinInfo ) == false ) return false;

    if ( gclsUserAgent.GetRemoteCallRtp( strStayCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );  // join 단말로 보낼 재-offer 본문 (stay 원격 SDP 기반)

    const bool bRelay = !clsCallInfo.m_strRelaySessionId.empty();
    const int iStayIdx = clsStayInfo.m_bRecv ? 0 : 1;  // stay leg 의 relay peer index
    const int iNewIdx = 1 - iStayIdx;                  // join 단말이 승계하는 index
    const int iStayPort = clsCallInfo.m_iPeerRtpPort;  // stay 에게 광고된 relay 포트 (불변)
    const int iJoinPort = clsStayInfo.m_iPeerRtpPort;  // 전환 지시자 leg 포트 → join 이 승계
    const std::string strRelayIp =
        clsCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsCallInfo.m_strRelayLocalIp;
    std::string strStayUserId, strJoinUserId;
    gclsUserAgent.GetToId( strStayCallId.c_str(), strStayUserId );
    gclsUserAgent.GetToId( strJoinCallId.c_str(), strJoinUserId );
    const std::string strNewCaller = iStayIdx == 0 ? strStayUserId : strJoinUserId;
    const std::string strNewCallee = iStayIdx == 0 ? strJoinUserId : strStayUserId;

    if ( bScreened ) {
        CSipCallRtp clsReferToRtp;
        if ( gclsUserAgent.GetRemoteCallRtp( strJoinCallId.c_str(), &clsReferToRtp ) == false ) return false;
        clsReferToRtp.SetDirection( E_RTP_SEND_RECV );  // stay 로 보낼 재-offer 본문 (join 원격 SDP 기반)

        // join 단말이 상담 통화에서 확립한 SDES leg 상태를 그대로 이관 — 같은 서버 키를 재광고하고
        //   기존 UE 선언 키를 유지 relay 의 승계 index 로 내린다 (재키잉 없음, media_security.md §5.2).
        const int iJoinOldIdx = clsJoinInfo.m_bRecv ? 0 : 1;
        RelaySdesLeg clsJoinLeg = clsJoinInfo.m_clsSdesLeg[iJoinOldIdx];
        CmpMediaCrypto clsJoinAudioCrypto, clsJoinVideoCrypto;
        if ( bRelay && ( ( clsJoinLeg.clsAudio.bSrtp &&
                           !MediaSdes::BuildCmpKeys( clsJoinLeg.clsAudio.strSuite, clsJoinLeg.clsAudio.strUeKey,
                                                     clsJoinLeg.clsAudio.strSrvKey, clsJoinAudioCrypto ) ) ||
                         ( clsJoinLeg.clsVideo.bSrtp &&
                           !MediaSdes::BuildCmpKeys( clsJoinLeg.clsVideo.strSuite, clsJoinLeg.clsVideo.strUeKey,
                                                     clsJoinLeg.clsVideo.strSrvKey, clsJoinVideoCrypto ) ) ) ) {
            CLog::Print( LOG_ERROR, "OnTransfer: SRTP key carry-over failed — 전환 거부 (CallId=%s)", pszCallId );
            return false;
        }

        // pair 해체 — 유지 relay(원 통화)는 회수 금지(bStopPort=false), 상담 통화 relay 는 여기서 회수
        gclsCallMap.Delete( pszCallId, false );
        gclsCallMap.Delete( pszReferToCallId, true );

        if ( bRelay ) {
            // join 단말의 미디어를 유지 relay 의 승계 index 로 재고정 — 주소는 join 의 UE 선언 주소
            int iAudioPort = clsReferToRtp.GetAudioPort();
            if ( iAudioPort <= 0 && clsReferToRtp.m_iPort > 0 ) iAudioPort = clsReferToRtp.m_iPort;
            if ( iAudioPort > 0 ) {
                int iVideoPort = ( clsReferToRtp.GetMediaCount() >= 2 ) ? clsReferToRtp.GetVideoPort() : 0;
                int iJoinNat = 0;
                std::string strJoinGuardIp;
                {
                    ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strJoinUserId, "volte" );
                    std::string strSigIp;
                    CUserInfo clsJoinUserInfo;
                    if ( !strJoinUserId.empty() && gclsUserMap.Select( strJoinUserId.c_str(), clsJoinUserInfo ) )
                        strSigIp = clsJoinUserInfo.m_strIp;
                    if ( CCspServiceMap::EvalMediaNat( clsNatSvc, clsReferToRtp.m_strIp, strSigIp, strJoinGuardIp ) )
                        iJoinNat = 1;
                }
                int iJoinPt = 0, iJoinSrcPt = 0, iJoinTePt = 0, iJoinSrcTePt = 0;
                std::string strJoinCodec;
                CGroupCallService::GetLegPt( strJoinCallId, clsJoinInfo.m_bRecv ? false : true, iJoinPt, iJoinSrcPt,
                                             iJoinTePt, iJoinSrcTePt, &strJoinCodec );
                gclsCmpClient.ModifySession( clsCallInfo.m_strRelaySessionId, clsReferToRtp.m_strIp, iAudioPort,
                                             iVideoPort > 0 ? iVideoPort : 0, iNewIdx, strNewCaller, strNewCallee,
                                             clsCallInfo.m_strRelaySesId, iJoinNat, strJoinGuardIp, iJoinPt, iJoinSrcPt,
                                             iJoinTePt, iJoinSrcTePt, strJoinCodec,
                                             clsJoinAudioCrypto.bEnabled ? &clsJoinAudioCrypto : NULL,
                                             clsJoinVideoCrypto.bEnabled ? &clsJoinVideoCrypto : NULL );
            }
        }

        // 새 pair + relay descriptor — entry 포트 = 그 leg 의 peer 에게 광고하는 relay 포트
        if ( iStayIdx == 0 )
            gclsCallMap.Insert( strStayCallId.c_str(), strJoinCallId.c_str(), iJoinPort, iStayPort );
        else
            gclsCallMap.Insert( strJoinCallId.c_str(), strStayCallId.c_str(), iStayPort, iJoinPort );
        if ( bRelay ) {
            gclsCallMap.SetRelayInfo( strStayCallId.c_str(), clsCallInfo.m_strRelaySessionId,
                                      clsCallInfo.m_strRelaySesId, clsCallInfo.m_strRelayLocalIp, strNewCaller,
                                      strNewCallee );
            gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iStayIdx, clsCallInfo.m_clsSdesLeg[iStayIdx] );
            gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iNewIdx, clsJoinLeg );
        }
        gclsCallMap.SetEstablished( strStayCallId.c_str() );

        // 양 leg 재-offer — 각 수신 leg 의 SDES 상태로 재작성 + relay 주소·기존 leg 포트 재광고
        if ( bRelay ) {
            MediaSdes::RewriteRelaySdpForLeg( clsReferToRtp.m_clsMediaList, clsCallInfo.m_clsSdesLeg[iStayIdx], true );
            clsReferToRtp.SetIpPort( strRelayIp.c_str(), iStayPort, SOCKET_COUNT_PER_MEDIA );
            MediaSdes::RewriteRelaySdpForLeg( clsRtp.m_clsMediaList, clsJoinLeg, true );
            clsRtp.SetIpPort( strRelayIp.c_str(), iJoinPort, SOCKET_COUNT_PER_MEDIA );
        } else {
            // 직결(비 relay) — crypto 투과만 차단 (기존 동작 보존)
            MediaSdes::StripCrypto( clsRtp.m_clsMediaList );
            MediaSdes::StripCrypto( clsReferToRtp.m_clsMediaList );
        }
        gclsUserAgent.SendReInvite( strStayCallId.c_str(), &clsReferToRtp );
        gclsUserAgent.SendReInvite( strJoinCallId.c_str(), &clsRtp );

        gclsUserAgent.StopCall( pszCallId );
        gclsUserAgent.StopCall( pszReferToCallId, SIP_REQUEST_TERMINATED );
        return true;
    }

    // unscreened — 상담 통화 미확립: 상담 pair 전체 해체(relay 회수) 후, 유지 relay 의 승계 index
    //   포트로 join 대상에게 신규 INVITE. answer 재고정은 EventCallStart 정상 경로가 수행한다.
    gclsCallMap.Delete( pszCallId, false );
    gclsCallMap.Delete( pszReferToCallId, true );
    gclsUserAgent.StopCall( pszCallId );
    gclsUserAgent.StopCall( pszReferToCallId, SIP_REQUEST_TERMINATED );

    std::string strNewCallId;
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    if ( gclsUserMap.Select( strJoinUserId.c_str(), clsUserInfo ) == false ) {
        gclsUserAgent.StopCall( strStayCallId.c_str() );
        gclsUserAgent.StopCall( strJoinCallId.c_str() );
        return true;
    }
    gclsUserAgent.StopCall( strJoinCallId.c_str() );  // 미확립 상담 leg 정리(CANCEL)

    RelaySdesLeg clsNewLeg;
    MediaSdes::StripCrypto( clsRtp.m_clsMediaList );  // stay leg 키 누출 차단
    if ( bRelay ) {
        // 신규 leg offer — 정책 × join 단말의 mediasec 능력으로 형태 결정, 서버 키는 새로 생성
        //   (떠나는 전환 지시자에게 알려진 키를 재사용하지 않는다).
        bool bNewSdes = false;
        ServiceInfo clsJoinSvc = gclsServiceMap.GetForUser( strJoinUserId, "volte" );
        if ( clsJoinSvc.media_srtp == "required" )
            bNewSdes = true;
        else if ( clsJoinSvc.media_srtp == "optional" )
            bNewSdes = clsUserInfo.m_bMediaSecSdes;
        if ( !MediaSdes::ApplyRelayLegOffer( clsRtp.m_clsMediaList, "audio", bNewSdes, clsNewLeg.clsAudio ) ||
             !MediaSdes::ApplyRelayLegOffer( clsRtp.m_clsMediaList, "video", bNewSdes, clsNewLeg.clsVideo ) ) {
            CLog::Print( LOG_ERROR, "OnTransfer: transfer leg SRTP key build failed (CallId=%s)", pszCallId );
            gclsUserAgent.StopCall( strStayCallId.c_str() );
            return false;
        }
        clsRtp.SetIpPort( strRelayIp.c_str(), iJoinPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    if ( gclsUserAgent.StartCall( strStayUserId.c_str(), strJoinUserId.c_str(), &clsRtp, &clsRoute, strNewCallId ) ==
         false ) {
        gclsUserAgent.StopCall( strStayCallId.c_str() );
        return false;
    }
    if ( iStayIdx == 0 )
        gclsCallMap.Insert( strStayCallId.c_str(), strNewCallId.c_str(), iJoinPort, iStayPort );
    else
        gclsCallMap.Insert( strNewCallId.c_str(), strStayCallId.c_str(), iStayPort, iJoinPort );
    if ( bRelay ) {
        gclsCallMap.SetRelayInfo( strStayCallId.c_str(), clsCallInfo.m_strRelaySessionId, clsCallInfo.m_strRelaySesId,
                                  clsCallInfo.m_strRelayLocalIp, strNewCaller, strNewCallee );
        gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iStayIdx, clsCallInfo.m_clsSdesLeg[iStayIdx] );
        gclsCallMap.SetRelaySdesLeg( strStayCallId.c_str(), iNewIdx, clsNewLeg );
    }
    return true;
}

bool CTasModule::OnBlindTransfer( const char *pszCallId, const char *pszReferToId ) {
    std::string strCallId, strInviteCallId, strToId;
    CSipCallRtp clsRtp;
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    int iStartPort = -1;

    if ( gclsCallMap.Select( pszCallId, strCallId ) == false ) return false;
    if ( gclsUserAgent.GetToId( strCallId.c_str(), strToId ) == false ) return false;
    if ( gclsUserMap.Select( pszReferToId, clsUserInfo ) == false ) return false;
    if ( gclsUserAgent.GetRemoteCallRtp( strCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );

    // 원 통화의 relay 세션을 유지 — 전환 대상은 REFER 지시자 leg 의 peer index·포트를 승계한다
    //   (신규 AddSession 없음: 실패·거절 시 원 통화가 그대로 남고, relay 누수 경로도 사라진다).
    //   완결(answer 재고정·pair 재결합)은 OnCallStart 의 trans 분기가 RELAY_MODIFY 로 수행.
    CCallInfo clsOldInfo, clsStayInfo;
    const bool bRelay = gclsCallMap.Select( pszCallId, clsOldInfo ) && !clsOldInfo.m_strRelaySessionId.empty() &&
                        gclsCallMap.Select( strCallId.c_str(), clsStayInfo );
    RelaySdesLeg clsNewLeg;
    int iNewIdx = 1;
    MediaSdes::StripCrypto( clsRtp.m_clsMediaList );  // 남는 leg 키 누출 차단
    if ( bRelay ) {
        iNewIdx = clsOldInfo.m_bRecv ? 0 : 1;     // 지시자 leg 의 index = 전환 대상이 승계
        iStartPort = clsStayInfo.m_iPeerRtpPort;  // 지시자 leg 의 relay 포트 → 전환 대상에게 광고
        // 신규 leg offer — 정책 × 전환 대상의 mediasec 능력, 서버 키는 새로 생성 (떠나는 단말에
        //   알려진 키 재사용 금지 — media_security.md §5.2)
        bool bNewSdes = false;
        ServiceInfo clsNewSvc = gclsServiceMap.GetForUser( pszReferToId ? pszReferToId : "", "volte" );
        if ( clsNewSvc.media_srtp == "required" )
            bNewSdes = true;
        else if ( clsNewSvc.media_srtp == "optional" )
            bNewSdes = clsUserInfo.m_bMediaSecSdes;
        if ( !MediaSdes::ApplyRelayLegOffer( clsRtp.m_clsMediaList, "audio", bNewSdes, clsNewLeg.clsAudio ) ||
             !MediaSdes::ApplyRelayLegOffer( clsRtp.m_clsMediaList, "video", bNewSdes, clsNewLeg.clsVideo ) ) {
            CLog::Print( LOG_ERROR, "OnBlindTransfer: transfer leg SRTP key build failed (CallId=%s)", pszCallId );
            return false;
        }
        std::string strRelayIp =
            clsOldInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsOldInfo.m_strRelayLocalIp;
        clsRtp.SetIpPort( strRelayIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    if ( gclsUserAgent.StartCall( strToId.c_str(), pszReferToId, &clsRtp, &clsRoute, strInviteCallId ) == false )
        return false;
    gclsTransCallMap.Insert( pszCallId, strInviteCallId.c_str(), iStartPort );
    // 전환 leg 의 SDES offer 상태만 trans entry 에 보관 — relay descriptor 는 보관하지 않는다
    //   (trans Delete 가 계속 쓰는 원 통화의 relay 를 회수하지 않도록).
    if ( bRelay ) gclsTransCallMap.SetRelaySdesLeg( pszCallId, iNewIdx, clsNewLeg );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  당겨받기 (Call Pickup — volte_supplementary_services.md §5)
// ──────────────────────────────────────────────────────────────

bool CTasModule::IsPickupDial( const char *pszFrom, const char *pszTo, std::string &strTarget ) {
    strTarget.clear();
    if ( pszTo == NULL || pszTo[0] == '\0' ) return false;

    // 코드 결정 — 발신 가입자의 접속서비스 pickup_feature_code. 필드 미지정(레거시 레코드)이면
    //   전역 Setup.Sip.CallPickupId 폴백(전환기 호환), 빈 값이면 그 서비스에서 픽업 비활성 (§5.2).
    ServiceInfo clsSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
    std::string strCode;
    if ( clsSvc.id > 0 && clsSvc.pickup_code_set )
        strCode = clsSvc.pickup_feature_code;
    else
        strCode = gclsSetup.m_strCallPickupId;
    if ( strCode.empty() ) return false;

    const size_t iLen = strCode.size();
    if ( strncmp( pszTo, strCode.c_str(), iLen ) != 0 ) return false;
    if ( pszTo[iLen] == '\0' ) return true;  // 그룹 픽업
    strTarget = pszTo + iLen;                // 지정 픽업 — "<code><내선>"
    return true;
}

void CTasModule::PickUp( const char *pszCallId, const char *pszFrom, const char *pszTarget, CSipCallRtp *pclsRtp ) {
    CspUser xmlFrom;
    USER_ID_LIST clsUserIdList;
    bool bCallPickup = false;
    bool bCommitted = false;  // 재키잉(원 착신 leg 해체) 이후에는 다음 후보로 넘어갈 수 없다

    // 후보 결정 — 축은 픽업 그룹(pickup_group, 미지정 시 org 폴백 — §5.1).
    //   그룹 픽업: 그룹 인덱스의 등록 그룹원 전체. 지정 픽업: 대상 내선 하나 — 같은 그룹일 때만(403).
    // 픽업자의 유효 그룹 축 값 — 멤버 인덱스 → pickup_group → org 폴백 (dispatch_center.md §3.3)
    const std::string strPickerGroup = gclsDispatchGroupMap.EffectiveGroupOf( pszFrom );
    if ( gclsCspUserMap.Select( pszFrom, xmlFrom ) ) {
        if ( pszTarget != NULL && pszTarget[0] != '\0' ) {
            // 대표번호 지정 픽업 (§4.4 F5) — "<code><대표번호>": 포크 중인 대표번호 호를 가져간다. 인가 = 그 그룹의
            // 멤버.
            CspDispatchGroup clsPilotGroup;
            if ( gclsDispatchGroupMap.SelectByPilot( pszTarget, clsPilotGroup ) ) {
                if ( clsPilotGroup.m_strId != strPickerGroup ) {
                    CLog::Print( LOG_INFO, "PickUp: pilot(%s) group(%s) ≠ picker(%s) group(%s) → 403", pszTarget,
                                 clsPilotGroup.m_strId.c_str(), pszFrom, strPickerGroup.c_str() );
                    return gclsDispatcher.StopCall( pszCallId, SIP_FORBIDDEN );
                }
                std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
                const std::string strACallId = FindForkForPickup( strPickerGroup, pszTarget );
                if ( strACallId.empty() ) return gclsDispatcher.StopCall( pszCallId, SIP_NOT_FOUND );
                int iRc = PickUpFork( pszCallId, pszFrom, pclsRtp, strACallId );
                if ( iRc != 0 ) gclsDispatcher.StopCall( pszCallId, iRc );
                return;
            }
            CUserInfo clsTargetInfo;
            if ( gclsUserMap.Select( pszTarget, clsTargetInfo ) == false )
                return gclsDispatcher.StopCall( pszCallId, SIP_NOT_FOUND );
            if ( clsTargetInfo.m_strGroupId != xmlFrom.EffectivePickupGroup() ) {
                CLog::Print( LOG_INFO, "PickUp: directed target(%s) not in picker(%s) group(%s) → 403", pszTarget,
                             pszFrom, xmlFrom.EffectivePickupGroup().c_str() );
                return gclsDispatcher.StopCall( pszCallId, SIP_FORBIDDEN );
            }
            clsUserIdList.push_back( pszTarget );
        } else {
            gclsUserMap.SelectGroup( xmlFrom.EffectivePickupGroup().c_str(), clsUserIdList );
        }
    }

    for ( USER_ID_LIST::iterator itUIL = clsUserIdList.begin();
          itUIL != clsUserIdList.end() && bCallPickup == false && bCommitted == false; ++itUIL ) {
        std::string strOldCallId;
        if ( gclsCallMap.SelectToRing( itUIL->c_str(), strOldCallId ) == false ) continue;
        // SDES 협상 불가(488)는 원 호를 건드리지 않고 즉시 종료; 재키잉 이후 실패는 후보 순회 중단.
        int iRc = PickUpLeg( pszCallId, pszFrom, pclsRtp, strOldCallId );
        if ( iRc == 0 ) {
            bCallPickup = true;
        } else if ( iRc == SIP_NOT_ACCEPTABLE_HERE ) {
            return gclsDispatcher.StopCall( pszCallId, iRc );
        } else {
            bCommitted = true;  // 재키잉 후 실패 — 다른 후보로 넘어가지 않는다
        }
    }

    // CallMap 에 링잉 호가 없으면 포크 중인 대표번호 호(대기 leg 는 CallMap 밖) — 그룹 픽업은 픽업자 그룹의 포크 집합,
    //   지정 픽업은 대상 내선이 대기 leg 로 링 중인 집합 (§4.4 "대기 leg 를 대표번호의 링잉 호로 본다").
    if ( bCallPickup == false && bCommitted == false && !strPickerGroup.empty() ) {
        std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
        const std::string strACallId = FindForkForPickup( strPickerGroup, pszTarget );
        if ( !strACallId.empty() ) {
            int iRc = PickUpFork( pszCallId, pszFrom, pclsRtp, strACallId );
            if ( iRc == 0 ) return;
            return gclsDispatcher.StopCall( pszCallId, iRc );
        }
    }

    if ( bCallPickup == false ) gclsDispatcher.StopCall( pszCallId, SIP_NOT_FOUND );
}

std::string CTasModule::FindForkForPickup( const std::string &strPickerGroup, const char *pszTarget ) {
    const std::string strTarget = pszTarget ? pszTarget : "";
    for ( const auto &kv : m_mapFork ) {
        const CTasForkSet &clsSet = kv.second;
        if ( clsSet.strGroupId != strPickerGroup || clsSet.setPending.empty() ) continue;
        if ( strTarget.empty() || strTarget == clsSet.strPilot ) return kv.first;
        for ( const auto &strLeg : clsSet.setPending ) {
            auto it = clsSet.mapLegUser.find( strLeg );
            if ( it != clsSet.mapLegUser.end() && it->second == strTarget ) return kv.first;
        }
    }
    return "";
}

// 포크 집합 당겨받기 — 승자 확정(OnForkStart)과 같은 재키잉이되, 픽업 leg 는 수신 INVITE 라 answer 를 여기서 직접 낸다
//   (PickUpLeg 와 동형: 픽업 offer 로 A·픽업 양측 200, relay peer1 = 픽업 단말).
int CTasModule::PickUpFork( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                            const std::string &strACallId ) {
    auto itSet = m_mapFork.find( strACallId );
    if ( itSet == m_mapFork.end() ) return SIP_NOT_FOUND;
    if ( !itSet->second.bRelay || pclsRtp == NULL ) return SIP_SERVICE_UNAVAILABLE;  // 포크는 relay 위에서만 성립
    const std::string strPicker = pszFrom ? pszFrom : "";

    // 픽업 단말 offer 의 SDES 평가(정책 = 픽업 단말의 접속서비스) — 협상 불가는 포크를 건드리지 않고 488.
    RelaySdesLeg clsNewLeg;
    CmpMediaCrypto clsNewAudioCrypto, clsNewVideoCrypto;
    {
        ServiceInfo clsPickSvc = gclsServiceMap.GetForUser( strPicker, "volte" );
        if ( MediaSdes::EvalRelayOfferSdes( clsPickSvc.media_srtp, pclsRtp->m_clsMediaList, "audio",
                                            clsNewLeg.clsAudio ) < 0 ||
             MediaSdes::EvalRelayOfferSdes( clsPickSvc.media_srtp, pclsRtp->m_clsMediaList, "video",
                                            clsNewLeg.clsVideo ) < 0 ||
             ( clsNewLeg.clsAudio.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsNewLeg.clsAudio.strSuite, clsNewLeg.clsAudio.strUeKey,
                                         clsNewLeg.clsAudio.strSrvKey, clsNewAudioCrypto ) ) ||
             ( clsNewLeg.clsVideo.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsNewLeg.clsVideo.strSuite, clsNewLeg.clsVideo.strUeKey,
                                         clsNewLeg.clsVideo.strSrvKey, clsNewVideoCrypto ) ) ) {
            CLog::Print( LOG_INFO, "PickUpFork: picker(%s) SDES offer not acceptable → 488", pszFrom );
            return SIP_NOT_ACCEPTABLE_HERE;
        }
    }

    // 커밋 — 집합 제거, 대기 leg 전원 CANCEL (최종 응답 487 은 m_mapForkLeg 로 OnForkEnd 가 흡수)
    CTasForkSet clsSet = itSet->second;
    m_mapFork.erase( itSet );
    for ( const auto &strLeg : clsSet.setPending ) {
        gclsUserAgent.StopCall(
            strLeg.c_str() );  // 패자 CANCEL — dialog 는 아래 confirmed 로 이어진다(terminated 아님)
    }

    // (A, 픽업) 쌍 — OnForkStart 와 동일 (entry 포트 = 그 leg 의 peer 에게 광고하는 relay 포트)
    gclsCallMap.Insert( strACallId.c_str(), pszCallId, clsSet.iPortB, clsSet.iPortA );
    gclsCallMap.SetRelayInfo( strACallId.c_str(), clsSet.strRelaySessionId, clsSet.strRelaySesId,
                              clsSet.strRelayLocalIp, clsSet.strCaller, strPicker );
    gclsCallMap.SetRelaySdesLeg( strACallId.c_str(), 0, clsSet.clsSdesA );
    gclsCallMap.SetRelaySdesLeg( strACallId.c_str(), 1, clsNewLeg );
    const std::string strSesIdA = gclsSipLogger.GetSesIdByCallId( strACallId );
    if ( !strSesIdA.empty() ) gclsSipLogger.SetCallSesId( pszCallId, strSesIdA );
    if ( gclsCallDir.IsEnabled() && !clsSet.strSessionId.empty() )
        gclsCallDir.MapCallToSession( pszCallId, clsSet.strSessionId );

    // relay peer1 = 픽업 단말 (RELAY_MODIFY — 주소·NAT·PT·crypto, PickUpLeg 와 동형)
    int iAudioPort = pclsRtp->GetAudioPort();
    if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
    if ( iAudioPort > 0 ) {
        int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
        int iPickNat = 0;
        std::string strPickGuardIp;
        {
            ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( strPicker, "volte" );
            std::string strSigIp;
            CUserInfo clsPickUserInfo;
            if ( gclsUserMap.Select( strPicker.c_str(), clsPickUserInfo ) ) strSigIp = clsPickUserInfo.m_strIp;
            if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strPickGuardIp ) ) iPickNat = 1;
        }
        int iPickPt = 0, iPickSrcPt = 0, iPickTePt = 0, iPickSrcTePt = 0;
        std::string strPickCodec;
        CGroupCallService::GetLegPt( pszCallId, false, iPickPt, iPickSrcPt, iPickTePt, iPickSrcTePt, &strPickCodec );
        gclsCmpClient.ModifySession( clsSet.strRelaySessionId, pclsRtp->m_strIp, iAudioPort,
                                     iVideoPort > 0 ? iVideoPort : 0, 1, clsSet.strCaller, strPicker,
                                     clsSet.strRelaySesId, iPickNat, strPickGuardIp, iPickPt, iPickSrcPt, iPickTePt,
                                     iPickSrcTePt, strPickCodec, clsNewAudioCrypto.bEnabled ? &clsNewAudioCrypto : NULL,
                                     clsNewVideoCrypto.bEnabled ? &clsNewVideoCrypto : NULL );
    }

    const std::string strRelayIp =
        clsSet.strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsSet.strRelayLocalIp;
    // A(발신자)에게 200 answer — 픽업 offer 를 A leg 상태로 재작성 + peer0 포트
    CSipCallRtp clsAnswerRtp = *pclsRtp;
    MediaSdes::RewriteRelaySdpForLeg( clsAnswerRtp.m_clsMediaList, clsSet.clsSdesA, false );
    clsAnswerRtp.SetIpPort( strRelayIp.c_str(), clsSet.iPortA, SOCKET_COUNT_PER_MEDIA );
    if ( gclsUserAgent.AcceptCall( strACallId.c_str(), &clsAnswerRtp ) == false ) {
        gclsUserAgent.StopCall( strACallId.c_str() );
        return SIP_INTERNAL_SERVER_ERROR;
    }
    // 픽업 단말에게 200 answer — 자기 offer echo(신규 leg 상태) + peer1 포트
    MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsNewLeg, false );
    pclsRtp->SetIpPort( strRelayIp.c_str(), clsSet.iPortB, SOCKET_COUNT_PER_MEDIA );
    gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
    gclsCallMap.SetEstablished( pszCallId );

    m_mapPilotOfCall[strACallId] = { clsSet.strPilot, strACallId, clsSet.strCaller };
    m_mapPilotOfCall[pszCallId] = { clsSet.strPilot, strACallId, clsSet.strCaller };
    if ( gclsCallDir.IsEnabled() && !clsSet.strSessionId.empty() ) {
        gclsCallDir.WriteSessionMapping( clsSet.strSessionId, strACallId, pszCallId, clsSet.strRelaySesId );
        gclsCallDir.VoipAddParticipant( strACallId, strPicker, "callee" );
        gclsCallDir.VoipCallAnswer( strACallId );
    }
    NotifyPilotDialog( clsSet, "confirmed" );
    CLog::Print( LOG_INFO, "PickUpFork: pilot(%s) picked up by %s (leg %s) — %d pending cancelled [TAS]",
                 clsSet.strPilot.c_str(), strPicker.c_str(), pszCallId, (int)clsSet.setPending.size() );
    return 0;
}

// 당겨받기 재고정 코어 — 링잉/대상 leg(strOldCallId)를 pszCallId(신규 단말)로 승계·재고정.
//   PickUp(피처코드)·HandleIncomingReplaces(RFC 3891) 공용. 반환 0=성공, >0=실패 SIP 코드.
int CTasModule::PickUpLeg( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                           const std::string &strOldCallId ) {
    // 링잉 호의 양 leg — 발신자에게 광고할 relay 포트(peer0)는 링잉(착신) leg entry 에,
    //   신규(픽업) 단말에게 광고할 relay 포트(peer1)는 발신 leg entry 에 있다.
    CCallInfo clsOldCallInfo, clsPeerCallInfo;
    if ( gclsCallMap.Select( strOldCallId.c_str(), clsOldCallInfo ) == false ) return SIP_NOT_FOUND;
    if ( gclsCallMap.Select( clsOldCallInfo.m_strPeerCallId.c_str(), clsPeerCallInfo ) == false ) return SIP_NOT_FOUND;

    const bool bRelay = !clsOldCallInfo.m_strRelaySessionId.empty();
    RelaySdesLeg clsNewLeg;
    CmpMediaCrypto clsNewAudioCrypto, clsNewVideoCrypto;
    if ( bRelay && pclsRtp ) {
        // 신규 단말 offer 의 SDES 평가(정책 = 신규 단말의 접속서비스). 협상 불가는 원 호를
        //   건드리지 않고 488 (평문 폴백 금지) — 아직 재키잉 전이라 원 호가 그대로 유지된다.
        ServiceInfo clsPickSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
        if ( MediaSdes::EvalRelayOfferSdes( clsPickSvc.media_srtp, pclsRtp->m_clsMediaList, "audio",
                                            clsNewLeg.clsAudio ) < 0 ||
             MediaSdes::EvalRelayOfferSdes( clsPickSvc.media_srtp, pclsRtp->m_clsMediaList, "video",
                                            clsNewLeg.clsVideo ) < 0 ||
             ( clsNewLeg.clsAudio.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsNewLeg.clsAudio.strSuite, clsNewLeg.clsAudio.strUeKey,
                                         clsNewLeg.clsAudio.strSrvKey, clsNewAudioCrypto ) ) ||
             ( clsNewLeg.clsVideo.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsNewLeg.clsVideo.strSuite, clsNewLeg.clsVideo.strUeKey,
                                         clsNewLeg.clsVideo.strSrvKey, clsNewVideoCrypto ) ) ) {
            CLog::Print( LOG_INFO, "PickUpLeg: picker(%s) SDES offer not acceptable → 488", pszFrom );
            return SIP_NOT_ACCEPTABLE_HERE;
        }
    }

    // 재키잉 — 신규 단말 leg 가 링잉 착신 leg 의 자리를(relay peer index·포트 포함) 승계한다.
    if ( gclsCallMap.Insert( pszCallId, clsOldCallInfo ) == false ) return SIP_INTERNAL_SERVER_ERROR;
    gclsCallMap.Update( clsOldCallInfo.m_strPeerCallId.c_str(), pszCallId );
    gclsCallMap.DeleteOne( strOldCallId.c_str() );
    gclsUserAgent.StopCall( strOldCallId.c_str() );

    if ( bRelay && pclsRtp ) {
        // 신규 단말을 relay 착신(peer1) index 로 재고정 — 주소·NAT·PT·crypto (RELAY_MODIFY,
        //   cmp_media_api.md §6.2). 대상은 항상 발신(peer0)이 건 링잉 호이므로 승계 index 는 1.
        int iAudioPort = pclsRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
        if ( iAudioPort > 0 ) {
            int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
            int iPickNat = 0;
            std::string strPickGuardIp;
            {
                ServiceInfo clsNatSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
                std::string strSigIp;
                CUserInfo clsPickUserInfo;
                if ( gclsUserMap.Select( pszFrom, clsPickUserInfo ) ) strSigIp = clsPickUserInfo.m_strIp;
                if ( CCspServiceMap::EvalMediaNat( clsNatSvc, pclsRtp->m_strIp, strSigIp, strPickGuardIp ) )
                    iPickNat = 1;
            }
            int iPickPt = 0, iPickSrcPt = 0, iPickTePt = 0, iPickSrcTePt = 0;
            std::string strPickCodec;
            CGroupCallService::GetLegPt( pszCallId, false, iPickPt, iPickSrcPt, iPickTePt, iPickSrcTePt,
                                         &strPickCodec );
            gclsCmpClient.ModifySession( clsOldCallInfo.m_strRelaySessionId, pclsRtp->m_strIp, iAudioPort,
                                         iVideoPort > 0 ? iVideoPort : 0, 1, clsOldCallInfo.m_strRelayCaller,
                                         pszFrom ? pszFrom : "", clsOldCallInfo.m_strRelaySesId, iPickNat,
                                         strPickGuardIp, iPickPt, iPickSrcPt, iPickTePt, iPickSrcTePt, strPickCodec,
                                         clsNewAudioCrypto.bEnabled ? &clsNewAudioCrypto : NULL,
                                         clsNewVideoCrypto.bEnabled ? &clsNewVideoCrypto : NULL );
        }
        gclsCallMap.SetRelayInfo( pszCallId, clsOldCallInfo.m_strRelaySessionId, clsOldCallInfo.m_strRelaySesId,
                                  clsOldCallInfo.m_strRelayLocalIp, clsOldCallInfo.m_strRelayCaller,
                                  pszFrom ? pszFrom : "" );
        gclsCallMap.SetRelaySdesLeg( pszCallId, 1, clsNewLeg );

        const std::string strRelayIp = clsOldCallInfo.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress()
                                                                                : clsOldCallInfo.m_strRelayLocalIp;
        // 발신자에게 200 answer — 신규 offer 를 발신 leg 상태(offer echo)로 재작성 + peer0 포트
        CSipCallRtp clsAnswerRtp = *pclsRtp;
        MediaSdes::RewriteRelaySdpForLeg( clsAnswerRtp.m_clsMediaList, clsOldCallInfo.m_clsSdesLeg[0], false );
        clsAnswerRtp.SetIpPort( strRelayIp.c_str(), clsOldCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        if ( gclsUserAgent.AcceptCall( clsOldCallInfo.m_strPeerCallId.c_str(), &clsAnswerRtp ) == false ) {
            gclsUserAgent.StopCall( clsOldCallInfo.m_strPeerCallId.c_str() );
            return SIP_INTERNAL_SERVER_ERROR;
        }
        // 신규 단말에게 200 answer — 자기 offer echo(신규 leg 상태) + peer1 포트
        MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsNewLeg, false );
        pclsRtp->SetIpPort( strRelayIp.c_str(), clsPeerCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
        gclsCallMap.SetEstablished( pszCallId );
        return 0;
    }

    // 직결(비 relay) — 기존 포트 재사용 동작 보존
    CSipCallRtp clsRemoteRtp;
    if ( gclsUserAgent.GetRemoteCallRtp( clsOldCallInfo.m_strPeerCallId.c_str(), &clsRemoteRtp ) ) {
        if ( pclsRtp ) {
            if ( clsOldCallInfo.m_iPeerRtpPort > 0 ) {
                pclsRtp->m_iPort = clsOldCallInfo.m_iPeerRtpPort;
                pclsRtp->m_strIp = CspAddressing::GetLocalRtpAddress();
            }
            pclsRtp->m_iCodec = clsRemoteRtp.m_iCodec;
        }
        if ( gclsUserAgent.AcceptCall( clsOldCallInfo.m_strPeerCallId.c_str(), pclsRtp ) ) {
            if ( pclsRtp ) {
                if ( clsOldCallInfo.m_iPeerRtpPort > 0 )
                    pclsRtp->m_iPort = clsPeerCallInfo.m_iPeerRtpPort;
                else
                    pclsRtp = &clsRemoteRtp;
            }
            gclsUserAgent.AcceptCall( pszCallId, pclsRtp );
            gclsCallMap.SetEstablished( pszCallId );
            return 0;
        }
        gclsUserAgent.StopCall( clsOldCallInfo.m_strPeerCallId.c_str() );
    }
    return SIP_INTERNAL_SERVER_ERROR;
}

// 수신 INVITE 의 Replaces(RFC 3891) 처리 — 관제 BLF 당겨받기·표준 attended 완결.
bool CTasModule::HandleIncomingReplaces( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                                         CSipMessage *pclsMessage ) {
    if ( pclsMessage == NULL ) return false;
    CSipHeader *pclsRep = pclsMessage->GetHeader( "Replaces" );
    if ( pclsRep == NULL || pclsRep->m_strValue.empty() ) return false;

    // Replaces: <call-id>;to-tag=<t>;from-tag=<f>  (RFC 3891, 미이스케이프 표준형)
    std::string strVal = pclsRep->m_strValue, strTargetCallId, strToTag, strFromTag;
    {
        size_t p = strVal.find( ';' );
        strTargetCallId = ( p == std::string::npos ) ? strVal : strVal.substr( 0, p );
        // 앞뒤 공백 제거
        while ( !strTargetCallId.empty() && ( strTargetCallId.back() == ' ' ) ) strTargetCallId.pop_back();
        auto param = [&]( const char *pszKey ) -> std::string {
            std::string strKey = pszKey;
            size_t q = strVal.find( strKey );
            if ( q == std::string::npos ) return "";
            q += strKey.size();
            size_t e = strVal.find( ';', q );
            std::string v = strVal.substr( q, e == std::string::npos ? std::string::npos : e - q );
            while ( !v.empty() && ( v.back() == ' ' ) ) v.pop_back();
            return v;
        };
        strToTag = param( "to-tag=" );
        strFromTag = param( "from-tag=" );
    }
    if ( strTargetCallId.empty() ) return false;

    // 대상 다이얼로그 확인 — CallMap 에 존재하고 psip 다이얼로그 태그가 일치해야 한다.
    CCallInfo clsTarget;
    if ( gclsCallMap.Select( strTargetCallId.c_str(), clsTarget ) == false ) {
        CLog::Print( LOG_INFO, "Replaces target %s not found → 481 (from %s)", strTargetCallId.c_str(), pszFrom );
        gclsDispatcher.StopCall( pszCallId, SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
        return true;
    }
    if ( gclsUserAgent.MatchReplacesDialog( strTargetCallId.c_str(), strToTag.c_str(), strFromTag.c_str() ) == false ) {
        CLog::Print( LOG_INFO, "Replaces tag mismatch for %s → 481 (from %s)", strTargetCallId.c_str(), pszFrom );
        gclsDispatcher.StopCall( pszCallId, SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
        return true;
    }

    // 인가 — Replaces 발신자와 대상 호 당사자는 같은 픽업 그룹이어야 한다(§6.2, 무단 가로채기 방지).
    //   대상 leg 의 당사자(원 착신자) 그룹으로 판정, 발신자 자신·착신자 자신의 Replaces 는 허용.
    {
        CallLegParty clsTgtParty, clsOtherParty;
        gclsCallMap.ResolveLegParties( strTargetCallId.c_str(), clsTgtParty, clsOtherParty );
        const std::string &strCallee = clsTgtParty.strUser, &strCaller = clsOtherParty.strUser;
        CspUser clsPicker, clsCallee;
        std::string strGP, strGC;
        if ( gclsCspUserMap.Select( pszFrom, clsPicker ) ) strGP = clsPicker.EffectivePickupGroup();
        if ( gclsCspUserMap.Select( strCallee.c_str(), clsCallee ) ) strGC = clsCallee.EffectivePickupGroup();
        const bool bSelf = ( pszFrom && ( strCallee == pszFrom || strCaller == pszFrom ) );
        if ( !bSelf && ( strGP.empty() || strGP != strGC ) ) {
            CLog::Print( LOG_INFO, "Replaces denied — %s not same pickup group as %s (%s vs %s) → 403", pszFrom,
                         strCallee.c_str(), strGP.c_str(), strGC.c_str() );
            gclsDispatcher.StopCall( pszCallId, SIP_FORBIDDEN );
            return true;
        }
    }

    CLog::Print( LOG_INFO, "Replaces: %s replaces dialog %s (picker %s)", pszCallId, strTargetCallId.c_str(), pszFrom );
    int iRc = PickUpLeg( pszCallId, pszFrom, pclsRtp, strTargetCallId );
    if ( iRc != 0 ) gclsDispatcher.StopCall( pszCallId, iRc );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  대표번호 병렬 호출 (Flexible Alerting — dispatch_center.md §4, TS 24.239)
// ──────────────────────────────────────────────────────────────

bool CTasModule::IsUserBusy( const std::string &strUserId ) {
    SIP_CALL_ID_LIST clsList;
    gclsUserAgent.GetCallIdList( clsList );
    for ( const auto &strCallId : clsList ) {
        std::string strFrom, strTo;
        gclsUserAgent.GetFromId( strCallId.c_str(), strFrom );
        gclsUserAgent.GetToId( strCallId.c_str(), strTo );
        if ( strFrom == strUserId || strTo == strUserId ) return true;
    }
    return false;
}

void CTasModule::ResolveForkTargets( const CspDispatchGroup &clsGroup, const std::string &strCaller,
                                     std::vector<std::string> &vecTargets ) {
    vecTargets.clear();
    const bool bSkipBusy = ( clsGroup.m_strBusyMembers != "alert" );
    for ( const auto &m : clsGroup.m_vecMembers ) {  // alert_order 오름차순(적재 시 정렬)
        if ( m.strUserId == strCaller ) continue;
        if ( gclsUserMap.Select( m.strUserId.c_str() ) == false ) continue;  // 등록·생존 바인딩만
        if ( bSkipBusy && IsUserBusy( m.strUserId ) ) continue;
        vecTargets.push_back( m.strUserId );
        if ( (int)vecTargets.size() >= gclsSetup.m_iDispatchMaxForkTargets ) break;  // 팬아웃 상한 절삭
    }
}

void CTasModule::NotifyPilotDialog( const CTasForkSet &clsSet, const char *pszState, const std::string &strRemote ) {
    // 포크 집합당 dialog 하나 — id·태그를 A-leg(발신자→대표번호 INVITE) Call-ID 로 고정해 early→confirmed→terminated 가
    //   같은 dialog 를 갱신한다. 종전엔 B-leg(그룹원)별 Call-ID 를 id 로 써 착신 한 건이 감시 앱에 N 행으로
    //   떴다(§4.5·RFC 4235).
    std::string lt, rt;
    gclsUserAgent.GetDialogTags( clsSet.strACallId.c_str(), lt, rt );
    // recipient 방향, local=대표번호, remote=발신자(응답한 그룹원 표시는 앱 몫)
    SendDialogEventNotify( clsSet.strPilot, clsSet.strACallId, pszState, "recipient", clsSet.strPilot,
                           strRemote.empty() ? clsSet.strCaller : strRemote, lt, rt );
}

void CTasModule::CollectPilotDialogs( const std::string &strPilotAor, std::vector<PilotDialogSnapshot> &vecOut ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
    // 울리는 중 — 포크 집합(early 를 이미 낸 것만: 실제 링잉 상태)
    for ( const auto &kv : m_mapFork )
        if ( kv.second.strPilot == strPilotAor && kv.second.bDialogOpen )
            vecOut.push_back( { kv.second.strACallId, strPilotAor, kv.second.strCaller, false } );
    // 확립 — 양 leg 이 같은 PilotCall 을 가리키므로 dialog id(A-leg Call-ID)로 1건씩.
    std::set<std::string> setSeen;
    for ( const auto &kv : m_mapPilotOfCall ) {
        if ( kv.second.strPilot != strPilotAor ) continue;
        if ( !setSeen.insert( kv.second.strACallId ).second ) continue;
        vecOut.push_back( { kv.second.strACallId, strPilotAor, kv.second.strCaller, true } );
    }
}

int CTasModule::ForkAlert( CTasForkSet &clsSet, const std::vector<std::string> &vecTargets ) {
    int iCount = 0;
    const std::string strSesIdA = gclsSipLogger.GetSesIdByCallId( clsSet.strACallId );
    for ( const auto &strMember : vecTargets ) {
        CUserInfo clsUserInfo;
        if ( gclsUserMap.Select( strMember.c_str(), clsUserInfo ) == false ) continue;

        // leg 전용 서버 offer — 정책 × 그룹원 mediasec 능력으로 SAVP/AVP 결정, 서버 키는 leg 마다 새로 생성
        //   (media_security.md §5.2). 승자의 키만 RELAY_MODIFY 로 peer1 에 내려간다.
        CSipCallRtp clsLegRtp = clsSet.clsBaseOffer;
        RelaySdesLeg clsLegSdes;
        if ( clsSet.bRelay ) {
            bool bSdes = false;
            ServiceInfo clsSvc = gclsServiceMap.GetForUser( strMember, "volte" );
            if ( clsSvc.media_srtp == "required" )
                bSdes = true;
            else if ( clsSvc.media_srtp == "optional" )
                bSdes = clsUserInfo.m_bMediaSecSdes;
            if ( !MediaSdes::ApplyRelayLegOffer( clsLegRtp.m_clsMediaList, "audio", bSdes, clsLegSdes.clsAudio ) ||
                 !MediaSdes::ApplyRelayLegOffer( clsLegRtp.m_clsMediaList, "video", bSdes, clsLegSdes.clsVideo ) ) {
                CLog::Print( LOG_ERROR, "ForkAlert: member(%s) SRTP key build failed — skip", strMember.c_str() );
                continue;
            }
        }

        CSipCallRoute clsRoute;
        clsUserInfo.GetCallRoute( clsRoute );
        clsRoute.m_b100rel = gclsUserAgent.Is100rel( clsSet.strACallId.c_str() );
        std::string strLegCallId;
        CSipMessage *pclsInvite = NULL;
        if ( gclsUserAgent.CreateCall( clsSet.strCaller.c_str(), strMember.c_str(), &clsLegRtp, &clsRoute, strLegCallId,
                                       &pclsInvite ) == false ) {
            CLog::Print( LOG_ERROR, "ForkAlert: CreateCall to member(%s) failed", strMember.c_str() );
            continue;
        }
        // 대표번호로 온 호 표시 (RFC 3455 / TS 24.229) — To 는 B2BUA 관례대로 그룹원 AoR (GroupCallService 와 동형).
        //   재타게팅 이력의 표준 표현 History-Info(RFC 7044)는 향후 과제 (§10).
        pclsInvite->AddHeader( "P-Called-Party-ID",
                               ( "<sip:" + clsSet.strPilot + "@" + clsSet.strDomain + ">" ).c_str() );
        gclsDispatcher.SetCallOwner( strLegCallId.c_str(), this );
        if ( !strSesIdA.empty() ) gclsSipLogger.SetCallSesId( strLegCallId, strSesIdA );
        if ( gclsCallDir.IsEnabled() && !clsSet.strSessionId.empty() )
            gclsCallDir.MapCallToSession( strLegCallId, clsSet.strSessionId );
        if ( gclsUserAgent.StartCall( strLegCallId.c_str(), pclsInvite ) == false ) {
            CLog::Print( LOG_ERROR, "ForkAlert: StartCall to member(%s) failed", strMember.c_str() );
            gclsDispatcher.RemoveCallOwner( strLegCallId.c_str() );
            continue;
        }
        clsSet.setPending.insert( strLegCallId );
        clsSet.mapLegUser[strLegCallId] = strMember;
        clsSet.mapLegSdes[strLegCallId] = clsLegSdes;
        clsSet.vecAlerted.push_back( strMember );
        m_mapForkLeg[strLegCallId] = clsSet.strACallId;
        ++iCount;
    }
    return iCount;
}

int CTasModule::StartAlert( CTasForkSet &clsSet, const std::vector<std::string> &vecTargets ) {
    clsSet.vecQueue.clear();
    if ( !clsSet.bSequential ) return ForkAlert( clsSet, vecTargets );
    // sequential alerting (TS 24.239) — alert_order 순으로 한 명씩, 단계 시한 = iNoAnswerSec
    clsSet.vecQueue = vecTargets;
    return AdvanceSequential( clsSet ) ? 1 : 0;
}

bool CTasModule::AdvanceSequential( CTasForkSet &clsSet ) {
    while ( !clsSet.vecQueue.empty() ) {
        const std::string strNext = clsSet.vecQueue.front();
        clsSet.vecQueue.erase( clsSet.vecQueue.begin() );
        std::vector<std::string> vecOne( 1, strNext );
        clsSet.tStart = time( NULL );
        if ( ForkAlert( clsSet, vecOne ) > 0 ) {
            CLog::Print( LOG_INFO, "AdvanceSequential: pilot(%s) → member(%s) step=%ds remaining=%d [TAS]",
                         clsSet.strPilot.c_str(), strNext.c_str(), clsSet.iNoAnswerSec, (int)clsSet.vecQueue.size() );
            return true;
        }
    }
    return false;
}

bool CTasModule::TryDispatchPilot( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp,
                                   CSipMessage *pclsMessage ) {
    CspDispatchGroup clsGroup;
    if ( pszTo == NULL || gclsDispatchGroupMap.SelectByPilot( pszTo, clsGroup ) == false ) return false;
    gclsDispatcher.SetCallOwner( pszCallId, this );
    const std::string strCaller = pszFrom ? pszFrom : "";

    // 포크는 공유 relay(peer1 포트 공용) 위에서만 성립한다 — 직결 모드에서는 미디어를 여러 leg 에 나눌 수 없다.
    if ( !gclsSetup.m_bUseRtpRelay ) {
        CLog::Print( LOG_ERROR, "TryDispatchPilot: pilot(%s) requires RTP relay (Setup.MediaServer) → 503", pszTo );
        gclsDispatcher.StopCall( pszCallId, SIP_SERVICE_UNAVAILABLE );
        return true;
    }

    std::vector<std::string> vecTargets;
    ResolveForkTargets( clsGroup, strCaller, vecTargets );
    if ( vecTargets.empty() && clsGroup.m_strOverflowTarget.empty() ) {
        CLog::Print( LOG_INFO, "TryDispatchPilot: pilot(%s) group(%s) has no reachable member → 480", pszTo,
                     clsGroup.m_strId.c_str() );
        gclsDispatcher.StopCall( pszCallId, SIP_TEMPORARILY_UNAVAILABLE );
        return true;
    }

    CTasForkSet clsSet;
    clsSet.strACallId = pszCallId;
    clsSet.strCaller = strCaller;
    clsSet.strGroupId = clsGroup.m_strId;
    clsSet.strPilot = pszTo;
    clsSet.strOverflow = clsGroup.m_strOverflowTarget;
    clsSet.bSequential = ( clsGroup.m_strAlertMode == "sequential" );
    clsSet.iNoAnswerSec = clsGroup.m_iNoAnswerSec > 0 ? clsGroup.m_iNoAnswerSec : 30;
    if ( clsSet.iNoAnswerSec > gclsSetup.m_iDispatchForkRingTimeoutSec )
        clsSet.iNoAnswerSec = gclsSetup.m_iDispatchForkRingTimeoutSec;
    clsSet.tStart = time( NULL );
    {
        ServiceInfo clsPilotSvc = gclsServiceMap.GetByName( clsGroup.m_strServiceRef );
        clsSet.strDomain = clsPilotSvc.id > 0 ? clsPilotSvc.domain : gclsServiceMap.GetDomainByKind( "volte" );
    }

    // CallDir 세션 — 대표번호 호 1건 (녹취·이력은 relay 1개, §4.6)
    if ( gclsCallDir.IsEnabled() ) {
        clsSet.strSessionId = CCallDir::GenerateSessionId();
        gclsCallDir.MapCallToSession( pszCallId, clsSet.strSessionId );
        gclsCallDir.GetVoipDir( pszCallId, strCaller, pszTo );
    }

    // ── A(발신) leg relay — 디스패처 EventIncomingCall 의 relay 블록과 동형 (peer0=A, peer1 미확정) ──
    {
        std::string strRecordDir;
        if ( gclsSetup.m_bRecordEnable && gclsCallDir.IsEnabled() )
            strRecordDir = gclsCallDir.GetVoipDir( pszCallId, strCaller, pszTo );
        int iAudioPort = pclsRtp->GetAudioPort();
        if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
        int iVideoPort = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
        clsSet.strRelaySesId = gclsSipLogger.GetOrIssueSesId( pszCallId, strCaller );

        int iCallerNat = 0;
        std::string strCallerGuardIp;
        ServiceInfo clsVolteSvc = gclsServiceMap.GetForUser( strCaller, "volte" );
        {
            std::string strSigIp;
            int iSigPort = 0;
            if ( pclsMessage ) pclsMessage->GetTopViaIpPort( strSigIp, iSigPort );
            if ( strSigIp.empty() ) {
                CUserInfo clsFromInfo;
                if ( !strCaller.empty() && gclsUserMap.Select( strCaller.c_str(), clsFromInfo ) )
                    strSigIp = clsFromInfo.m_strIp;
            }
            if ( CCspServiceMap::EvalMediaNat( clsVolteSvc, pclsRtp->m_strIp, strSigIp, strCallerGuardIp ) )
                iCallerNat = 1;
        }
        CmpMediaCrypto clsCallerAudioCrypto, clsCallerVideoCrypto;
        if ( MediaSdes::EvalRelayOfferSdes( clsVolteSvc.media_srtp, pclsRtp->m_clsMediaList, "audio",
                                            clsSet.clsSdesA.clsAudio ) < 0 ||
             MediaSdes::EvalRelayOfferSdes( clsVolteSvc.media_srtp, pclsRtp->m_clsMediaList, "video",
                                            clsSet.clsSdesA.clsVideo ) < 0 ||
             ( clsSet.clsSdesA.clsAudio.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsSet.clsSdesA.clsAudio.strSuite, clsSet.clsSdesA.clsAudio.strUeKey,
                                         clsSet.clsSdesA.clsAudio.strSrvKey, clsCallerAudioCrypto ) ) ||
             ( clsSet.clsSdesA.clsVideo.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsSet.clsSdesA.clsVideo.strSuite, clsSet.clsSdesA.clsVideo.strUeKey,
                                         clsSet.clsSdesA.clsVideo.strSrvKey, clsCallerVideoCrypto ) ) ) {
            CLog::Print( LOG_INFO, "TryDispatchPilot: caller(%s) SDES offer not acceptable → 488", pszFrom );
            gclsDispatcher.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            return true;
        }
        // B-leg 공통 offer 원본 — 수신 crypto strip(E2E 차단). leg 별 서버 키는 ForkAlert 가 얹는다.
        clsSet.clsBaseOffer = *pclsRtp;
        MediaSdes::StripCrypto( clsSet.clsBaseOffer.m_clsMediaList );

        clsSet.strRelaySessionId = CCmpClient::IssueSessionId();
        std::string strAllocatedIp;
        int iLocalPort = 0, iLocalVideoPort = 0, iLocalPortB = 0, iLocalVideoPortB = 0;
        int iCallerPt = 0, iCallerSrcPt = 0, iCallerTePt = 0, iCallerSrcTePt = 0;
        std::string strCallerCodec;
        CGroupCallService::GetLegPt( pszCallId, false, iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt,
                                     &strCallerCodec );
        if ( !gclsCmpClient.AddSession( clsSet.strRelaySessionId, strAllocatedIp, iLocalPort, iLocalVideoPort,
                                        iLocalPortB, iLocalVideoPortB, strRecordDir, strCaller, pszTo, pclsRtp->m_strIp,
                                        iAudioPort, iVideoPort, clsSet.strRelaySesId, iCallerNat, strCallerGuardIp,
                                        iCallerPt, iCallerSrcPt, iCallerTePt, iCallerSrcTePt, strCallerCodec,
                                        clsCallerAudioCrypto.bEnabled ? &clsCallerAudioCrypto : NULL,
                                        clsCallerVideoCrypto.bEnabled ? &clsCallerVideoCrypto : NULL ) ) {
            gclsDispatcher.StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
            return true;
        }
        clsSet.bRelay = true;
        clsSet.iPortA = iLocalPort;
        clsSet.iPortB = iLocalPortB;
        clsSet.strRelayLocalIp = strAllocatedIp;
        clsSet.strMediaNode = strAllocatedIp;
        std::string strRelayIp = strAllocatedIp.empty() ? CspAddressing::GetLocalRtpAddress() : strAllocatedIp;
        // 대기 leg 전원에게 같은 peer1 포트 — 승자만 RELAY_MODIFY 로 고정 (§4.1 핵심 계약)
        clsSet.clsBaseOffer.SetIpPort( strRelayIp.c_str(), iLocalPortB, SOCKET_COUNT_PER_MEDIA );
    }

    int iLegs = 0;
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
        iLegs = StartAlert( clsSet, vecTargets );
        if ( iLegs == 0 && !clsSet.strOverflow.empty() ) {
            // 등록 그룹원이 없는 대표번호 — 즉시 overflow 로 (무응답 대기 없음)
            m_mapFork[pszCallId] = clsSet;
            OverflowFork( pszCallId );
            return true;
        }
        if ( iLegs == 0 ) {
            gclsCmpClient.RemoveSession( clsSet.strRelaySessionId, strCaller, pszTo, clsSet.strRelaySesId );
            gclsDispatcher.StopCall( pszCallId, SIP_TEMPORARILY_UNAVAILABLE );
            return true;
        }
        m_mapFork[pszCallId] = clsSet;
    }

    if ( gclsCallDir.IsEnabled() ) {
        bool bVideo = ( pclsRtp->GetMediaCount() >= 2 && pclsRtp->GetVideoPort() > 0 );
        gclsCallDir.VoipCallStart( pszCallId, strCaller, pszTo, bVideo, clsSet.strMediaNode );
        gclsCallDir.VoipAddParticipant( pszCallId, strCaller, "caller" );
    }
    CLog::Print( LOG_INFO,
                 "TryDispatchPilot: pilot(%s) group(%s) caller(%s) %s to %d member(s) (targets=%d) relay=%s "
                 "no_answer=%ds overflow=%s [TAS]",
                 pszTo, clsGroup.m_strId.c_str(), pszFrom, clsSet.bSequential ? "sequential" : "forked", iLegs,
                 (int)vecTargets.size(), clsSet.strRelaySessionId.c_str(), clsSet.iNoAnswerSec,
                 clsSet.strOverflow.empty() ? "-" : clsSet.strOverflow.c_str() );
    return true;
}

bool CTasModule::OnForkRing( const char *pszCallId, int iSipStatus ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
    auto itLeg = m_mapForkLeg.find( pszCallId );
    if ( itLeg == m_mapForkLeg.end() ) return false;
    auto itSet = m_mapFork.find( itLeg->second );
    if ( itSet == m_mapFork.end() ) return true;  // 승자 확정 후 패자의 18x — 무시
    CTasForkSet &clsSet = itSet->second;
    if ( iSipStatus >= 180 && iSipStatus < 200 ) {
        if ( !clsSet.bRang ) {
            // 첫 180 만 A 에게, SDP 없이 — 대기 leg 의 미디어는 CMP 가 폐기하므로 183 조기 미디어를 주면 무음 구간.
            //   대표번호 dialog early 도 여기서 1회만 낸다(leg 마다가 아니라 착신 한 건에 대해 한 번).
            clsSet.bRang = true;
            gclsUserAgent.RingCall( clsSet.strACallId.c_str(), SIP_RINGING, NULL );
            clsSet.bDialogOpen = true;
            NotifyPilotDialog( clsSet, "early" );
        }
    }
    return true;
}

bool CTasModule::OnForkStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    (void)pclsRtp;
    std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
    auto itLeg = m_mapForkLeg.find( pszCallId );
    if ( itLeg == m_mapForkLeg.end() ) return false;
    const std::string strACallId = itLeg->second;
    auto itSet = m_mapFork.find( strACallId );
    if ( itSet == m_mapFork.end() ) {
        // 승자 확정 후 도착한 두 번째 200 OK (CANCEL 교차) — ACK 는 스택이 냈고 여기서 즉시 BYE (RFC 3261 §16.7 등가)
        CLog::Print( LOG_INFO, "OnForkStart: late 200 on loser leg(%s) → BYE", pszCallId );
        m_mapForkLeg.erase( itLeg );
        gclsDispatcher.RemoveCallOwner( pszCallId );
        gclsUserAgent.StopCall( pszCallId );
        return true;
    }
    CTasForkSet clsSet = itSet->second;
    const std::string strWinner = clsSet.mapLegUser[pszCallId];
    // 패자 CANCEL — 최종 응답(487)은 OnForkEnd 가 흡수 (m_mapForkLeg 유지)
    for ( const auto &strOther : clsSet.setPending ) {
        if ( strOther == pszCallId ) continue;
        gclsUserAgent.StopCall( strOther.c_str() );
    }
    m_mapFork.erase( itSet );
    m_mapForkLeg.erase( pszCallId );

    // (A, 승자) 쌍 — 이후는 기존 1:1 경로. entry 포트 = 그 leg 의 peer 에게 광고하는 relay 포트.
    gclsCallMap.Insert( strACallId.c_str(), pszCallId, clsSet.iPortB, clsSet.iPortA );
    if ( clsSet.bRelay ) {
        gclsCallMap.SetRelayInfo( strACallId.c_str(), clsSet.strRelaySessionId, clsSet.strRelaySesId,
                                  clsSet.strRelayLocalIp, clsSet.strCaller, strWinner );
        gclsCallMap.SetRelaySdesLeg( strACallId.c_str(), 0, clsSet.clsSdesA );
        gclsCallMap.SetRelaySdesLeg( strACallId.c_str(), 1, clsSet.mapLegSdes[pszCallId] );
    }
    gclsCallMap.SetEstablished( pszCallId );
    m_mapPilotOfCall[strACallId] = { clsSet.strPilot, strACallId, clsSet.strCaller };
    m_mapPilotOfCall[pszCallId] = { clsSet.strPilot, strACallId, clsSet.strCaller };
    if ( gclsCallDir.IsEnabled() && !clsSet.strSessionId.empty() ) {
        gclsCallDir.WriteSessionMapping( clsSet.strSessionId, strACallId, pszCallId, clsSet.strRelaySesId );
        gclsCallDir.VoipAddParticipant( strACallId, strWinner, "callee" );
    }
    NotifyPilotDialog( clsSet, "confirmed" );
    CLog::Print( LOG_INFO, "OnForkStart: pilot(%s) answered by %s (leg %s) — %d loser(s) cancelled [TAS]",
                 clsSet.strPilot.c_str(), strWinner.c_str(), pszCallId, (int)clsSet.setPending.size() - 1 );
    return false;  // 디스패처 정상 answer 경로가 RELAY_MODIFY(peer1)·A 200 OK 를 수행
}

bool CTasModule::OnForkEnd( const char *pszCallId, int iSipStatus ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
    // A(발신) leg 취소/실패 — 대기 leg 전원 CANCEL, relay 회수
    auto itSetA = m_mapFork.find( pszCallId );
    if ( itSetA != m_mapFork.end() ) {
        CLog::Print( LOG_INFO, "OnForkEnd: caller leg(%s) ended(%d) while forking — cancel %d pending [TAS]", pszCallId,
                     iSipStatus, (int)itSetA->second.setPending.size() );
        FailFork( pszCallId, 0 );
        return true;
    }
    auto itLeg = m_mapForkLeg.find( pszCallId );
    if ( itLeg == m_mapForkLeg.end() ) return false;
    const std::string strACallId = itLeg->second;
    m_mapForkLeg.erase( itLeg );
    gclsDispatcher.RemoveCallOwner( pszCallId );
    auto itSet = m_mapFork.find( strACallId );
    if ( itSet == m_mapFork.end() ) return true;  // 승자 확정/취소 후 패자 최종 응답 — 흡수
    CTasForkSet &clsSet = itSet->second;
    clsSet.setPending.erase( pszCallId );
    if ( iSipStatus == SIP_BUSY_HERE ) clsSet.bBusySeen = true;
    if ( !clsSet.setPending.empty() ) return true;  // 다른 대기 leg 진행 중 — A 에게 전달하지 않는다
    // sequential — 이 순번 실패(거절·통화중·무응답 CANCEL) → 다음 순번
    if ( clsSet.bSequential && AdvanceSequential( clsSet ) ) return true;
    // 전원 최종 실패
    if ( !clsSet.strOverflow.empty() && clsSet.iDepth == 0 )
        OverflowFork( strACallId );
    else
        FailFork( strACallId, clsSet.bBusySeen ? SIP_BUSY_HERE : SIP_TEMPORARILY_UNAVAILABLE );
    return true;
}

void CTasModule::FailFork( const std::string &strACallId, int iSipCode ) {
    // m_mutexFork 보유 상태에서 호출된다
    auto itSet = m_mapFork.find( strACallId );
    if ( itSet == m_mapFork.end() ) return;
    CTasForkSet clsSet = itSet->second;
    m_mapFork.erase( itSet );
    for ( const auto &strLeg : clsSet.setPending )
        gclsUserAgent.StopCall( strLeg.c_str() );  // CANCEL — 최종 응답은 OnForkEnd 가 흡수
    // 착신 한 건이 완전 실패 — 대표번호 dialog terminated 를 집합당 1회만(early/confirmed 를 낸 적 있을 때).
    if ( clsSet.bDialogOpen ) NotifyPilotDialog( clsSet, "terminated" );
    if ( clsSet.bRelay )
        gclsCmpClient.RemoveSession( clsSet.strRelaySessionId, clsSet.strCaller, clsSet.strPilot,
                                     clsSet.strRelaySesId );
    if ( gclsCallDir.IsEnabled() ) gclsCallDir.VoipCallEnd( strACallId, iSipCode == 0 ? "normal" : "error", 0 );
    if ( iSipCode > 0 ) gclsDispatcher.StopCall( strACallId.c_str(), iSipCode );
    gclsDispatcher.RemoveCallOwner( strACallId.c_str() );
    CLog::Print( LOG_INFO, "FailFork: pilot(%s) caller(%s) → %s (alerted=%d) [TAS]", clsSet.strPilot.c_str(),
                 clsSet.strCaller.c_str(), iSipCode == 0 ? "caller cancelled" : std::to_string( iSipCode ).c_str(),
                 (int)clsSet.vecAlerted.size() );
}

void CTasModule::OverflowFork( const std::string &strACallId ) {
    // m_mutexFork 보유 상태에서 호출된다. 무응답·전원 부재 → overflow_target 으로 1단계 재시도 (§4.4).
    auto itSet = m_mapFork.find( strACallId );
    if ( itSet == m_mapFork.end() ) return;
    CTasForkSet &clsSet = itSet->second;
    const std::string strTarget = clsSet.strOverflow;
    clsSet.strOverflow.clear();
    clsSet.iDepth = 1;
    for ( const auto &strLeg : clsSet.setPending ) gclsUserAgent.StopCall( strLeg.c_str() );
    clsSet.setPending.clear();

    std::vector<std::string> vecTargets;
    CspDispatchGroup clsNext;
    if ( gclsDispatchGroupMap.SelectByPilot( strTarget.c_str(), clsNext ) ) {
        // 다른 대표번호 — 그 그룹원에게 재포크 (순환 금지: depth 1 에서 더 넘기지 않는다)
        ResolveForkTargets( clsNext, clsSet.strCaller, vecTargets );
        clsSet.bSequential = ( clsNext.m_strAlertMode == "sequential" );
        clsSet.iNoAnswerSec = clsNext.m_iNoAnswerSec > 0 ? clsNext.m_iNoAnswerSec : clsSet.iNoAnswerSec;
        if ( clsSet.iNoAnswerSec > gclsSetup.m_iDispatchForkRingTimeoutSec )
            clsSet.iNoAnswerSec = gclsSetup.m_iDispatchForkRingTimeoutSec;
    } else if ( strTarget != clsSet.strCaller && gclsUserMap.Select( strTarget.c_str() ) ) {
        clsSet.bSequential = false;
        vecTargets.push_back( strTarget );  // 내선 — 단일 leg 포크
    }
    clsSet.tStart = time( NULL );
    clsSet.bRang = false;
    int iLegs = vecTargets.empty() ? 0 : StartAlert( clsSet, vecTargets );
    CLog::Print( LOG_INFO, "OverflowFork: pilot(%s) → overflow(%s) %d leg(s) [TAS]", clsSet.strPilot.c_str(),
                 strTarget.c_str(), iLegs );
    if ( iLegs == 0 ) FailFork( strACallId, SIP_TEMPORARILY_UNAVAILABLE );
}

void CTasModule::Tick() {
    std::lock_guard<std::recursive_mutex> lock( m_mutexFork );
    if ( m_mapFork.empty() ) return;
    const time_t tNow = time( NULL );
    std::vector<std::string> vecExpired;
    for ( const auto &kv : m_mapFork )
        if ( kv.second.tStart > 0 && tNow - kv.second.tStart >= kv.second.iNoAnswerSec )
            vecExpired.push_back( kv.first );
    for ( const auto &strACallId : vecExpired ) {
        auto itSet = m_mapFork.find( strACallId );
        if ( itSet == m_mapFork.end() ) continue;
        if ( itSet->second.bSequential && !itSet->second.vecQueue.empty() ) {
            // sequential 단계 시한 만료 — 현 순번 CANCEL 후 다음 순번 (487 은 OnForkEnd 가 흡수)
            CTasForkSet &clsSeq = itSet->second;
            for ( const auto &strLeg : clsSeq.setPending )
                gclsUserAgent.StopCall( strLeg.c_str() );  // 현 순번 CANCEL — dialog 는 early 유지(다음 순번 계속)
            clsSeq.setPending.clear();
            if ( AdvanceSequential( clsSeq ) ) continue;
        }
        CLog::Print( LOG_INFO, "Tick: pilot(%s) no answer in %ds (%d pending) → %s [TAS]",
                     itSet->second.strPilot.c_str(), itSet->second.iNoAnswerSec, (int)itSet->second.setPending.size(),
                     ( !itSet->second.strOverflow.empty() && itSet->second.iDepth == 0 ) ? "overflow" : "480" );
        if ( !itSet->second.strOverflow.empty() && itSet->second.iDepth == 0 )
            OverflowFork( strACallId );
        else
            FailFork( strACallId, SIP_TEMPORARILY_UNAVAILABLE );
    }
}

// ──────────────────────────────────────────────────────────────
//  업무망 합법감청 — 합류(INVITE-Join) + tap 관리 (dispatch_center.md §5)
// ──────────────────────────────────────────────────────────────

// dispatch_center.md §5.7 — 감청 감사 이벤트(E-AUD-016 call_monitored). 시작/종료 각 1건.
static void _emitCallMonitored( const CTasModule::MonitorLeg &m, const char *pszPhase, int iDurMs ) {
    // FmReporter 는 헤더온리 싱글턴(gclsFmReporter) — 여기서 직접 접근한다.
    if ( !gclsFmReporter.IsEnabled() ) return;
    SimpleJson::JsonNode p;
    p.Set( "phase", pszPhase );
    p.Set( "monitor", m.strMonitor );
    p.Set( "group", m.strGroupId );
    p.Set( "session", m.strSessionId );
    if ( !m.strSesId.empty() ) p.Set( "sesid", m.strSesId );
    p.Set( "target_a", m.strTargetA );
    p.Set( "target_b", m.strTargetB );
    p.Set( "tap_mode", m.strTapMode );
    if ( iDurMs >= 0 ) p.Set( "dur_ms", iDurMs );
    gclsFmReporter.SendEvent( "call_monitored", "audit", gclsFmReporter.Node() + "/csp", p );
}

bool CTasModule::HandleIncomingJoin( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                                     CSipMessage *pclsMessage ) {
    if ( pclsMessage == NULL ) return false;
    CSipHeader *pclsJoin = pclsMessage->GetHeader( "Join" );
    if ( pclsJoin == NULL || pclsJoin->m_strValue.empty() ) return false;

    // Join: <call-id>;to-tag=<t>;from-tag=<f> (RFC 3911, 미이스케이프 표준형 — Replaces 파싱과 동일)
    std::string strVal = pclsJoin->m_strValue, strTargetCallId, strToTag, strFromTag;
    {
        size_t p = strVal.find( ';' );
        strTargetCallId = ( p == std::string::npos ) ? strVal : strVal.substr( 0, p );
        while ( !strTargetCallId.empty() && strTargetCallId.back() == ' ' ) strTargetCallId.pop_back();
        auto param = [&]( const char *pszKey ) -> std::string {
            size_t q = strVal.find( pszKey );
            if ( q == std::string::npos ) return "";
            q += strlen( pszKey );
            size_t e = strVal.find( ';', q );
            std::string v = strVal.substr( q, e == std::string::npos ? std::string::npos : e - q );
            while ( !v.empty() && v.back() == ' ' ) v.pop_back();
            return v;
        };
        strToTag = param( "to-tag=" );
        strFromTag = param( "from-tag=" );
    }
    if ( strTargetCallId.empty() ) return false;

    const std::string strMonitor = pszFrom ? pszFrom : "";

    // 대상 다이얼로그 확인 — CallMap 존재 + psip 태그 대조(MatchReplacesDialog 를 Join 대상에도 재사용).
    CCallInfo clsTarget;
    if ( gclsCallMap.Select( strTargetCallId.c_str(), clsTarget ) == false ) {
        CLog::Print( LOG_INFO, "Join target %s not found → 481 (from %s)", strTargetCallId.c_str(),
                     strMonitor.c_str() );
        gclsDispatcher.StopCall( pszCallId, SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
        return true;
    }
    if ( gclsUserAgent.MatchReplacesDialog( strTargetCallId.c_str(), strToTag.c_str(), strFromTag.c_str() ) == false ) {
        CLog::Print( LOG_INFO, "Join tag mismatch for %s → 481 (from %s)", strTargetCallId.c_str(),
                     strMonitor.c_str() );
        gclsDispatcher.StopCall( pszCallId, SIP_CALL_TRANSACTION_DOES_NOT_EXIST );
        return true;
    }
    if ( clsTarget.m_strRelaySessionId.empty() ) {
        CLog::Print( LOG_INFO, "Join target %s has no relay session → 488 (from %s)", strTargetCallId.c_str(),
                     strMonitor.c_str() );
        gclsDispatcher.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
        return true;
    }

    // 인가 — 감청자의 관제 그룹 monitor_scope 가 대상 호의 어느 당사자 그룹이라도 포함하면 허용 (§5.2).
    //   당사자는 leg 원단 기준(CallLegParty) — 대표번호 호의 A-leg 를 지목해도 다이얼된 번호가 아니라 승자가 잡힌다.
    std::string strCaller, strCallee;
    {
        CallLegParty clsTgtParty, clsOtherParty;
        gclsCallMap.ResolveLegParties( strTargetCallId.c_str(), clsTgtParty, clsOtherParty );
        strCaller = clsTgtParty.bInitiator ? clsTgtParty.strUser : clsOtherParty.strUser;
        strCallee = clsTgtParty.bInitiator ? clsOtherParty.strUser : clsTgtParty.strUser;
    }
    {
        const std::string strGW = gclsDispatchGroupMap.EffectiveGroupOf( strMonitor.c_str() );
        const std::string strGA = gclsDispatchGroupMap.EffectiveGroupOf( strCaller.c_str() );
        const std::string strGB = gclsDispatchGroupMap.EffectiveGroupOf( strCallee.c_str() );
        const bool bSelf = ( strMonitor == strCaller || strMonitor == strCallee );
        if ( !bSelf && !gclsDispatchGroupMap.CanWatch( strGW, strGA ) &&
             !gclsDispatchGroupMap.CanWatch( strGW, strGB ) ) {
            CLog::Print( LOG_INFO, "Join denied — %s cannot monitor %s/%s (scope) → 403", strMonitor.c_str(),
                         strCaller.c_str(), strCallee.c_str() );
            gclsDispatcher.StopCall( pszCallId, SIP_FORBIDDEN );
            _emitCallMonitored( { clsTarget.m_strRelaySessionId, clsTarget.m_strRelaySesId, "", "", strMonitor, "",
                                  strCaller, strCallee, "", time( NULL ) },
                                "denied", -1 );
            return true;
        }
    }

    // 세션당 tap 상한 (§5.5) — CSP 인메모리 카운트 + CMP 기능 광고(resource.tap).
    if ( gclsCmpClient.SupportsTap() == false ) {
        CLog::Print( LOG_INFO, "Join — CMP does not advertise resource.tap → 488 (from %s)", strMonitor.c_str() );
        gclsDispatcher.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
        return true;
    }
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutexMonitor );
        auto it = m_mapSessionMonitors.find( clsTarget.m_strRelaySessionId );
        int iCur = it == m_mapSessionMonitors.end() ? 0 : (int)it->second.size();
        if ( iCur >= gclsSetup.m_iDispatchMaxTapsPerSession ) {
            CLog::Print( LOG_INFO, "Join — session %s tap limit %d reached → 486",
                         clsTarget.m_strRelaySessionId.c_str(), gclsSetup.m_iDispatchMaxTapsPerSession );
            gclsDispatcher.StopCall( pszCallId, SIP_BUSY_HERE );
            return true;
        }
    }

    // 미디어 — 감청자 offer 는 recvonly 여야 한다(hold 는 488). 통화의 협상 코덱을 그대로 tap 으로 인도한다.
    if ( pclsRtp->m_eDirection != E_RTP_RECV && pclsRtp->m_eDirection != E_RTP_SEND_RECV ) {
        CLog::Print( LOG_INFO, "Join — monitor offer not recvonly (dir=%d) → 488", (int)pclsRtp->m_eDirection );
        gclsDispatcher.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
        return true;
    }

    // tap egress SRTP — 정책 × 감청자 offer 로 서버 키 생성(감청자가 answer 로 받는 키 = CMP→M tx).
    ServiceInfo clsMonSvc = gclsServiceMap.GetForUser( strMonitor, "volte" );
    RelaySdesLeg clsTapLeg;
    CmpMediaCrypto clsTapAudioCrypto, clsTapVideoCrypto;
    {
        int iA =
            MediaSdes::EvalRelayOfferSdes( clsMonSvc.media_srtp, pclsRtp->m_clsMediaList, "audio", clsTapLeg.clsAudio );
        int iV =
            MediaSdes::EvalRelayOfferSdes( clsMonSvc.media_srtp, pclsRtp->m_clsMediaList, "video", clsTapLeg.clsVideo );
        if ( iA < 0 || iV < 0 ) {
            CLog::Print( LOG_INFO, "Join — monitor(%s) SDES offer not acceptable → 488", strMonitor.c_str() );
            gclsDispatcher.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            return true;
        }
        if ( ( clsTapLeg.clsAudio.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsTapLeg.clsAudio.strSuite, clsTapLeg.clsAudio.strUeKey,
                                         clsTapLeg.clsAudio.strSrvKey, clsTapAudioCrypto ) ) ||
             ( clsTapLeg.clsVideo.bSrtp &&
               !MediaSdes::BuildCmpKeys( clsTapLeg.clsVideo.strSuite, clsTapLeg.clsVideo.strUeKey,
                                         clsTapLeg.clsVideo.strSrvKey, clsTapVideoCrypto ) ) ) {
            CLog::Print( LOG_ERROR, "Join — monitor(%s) SRTP key build failed → 488", strMonitor.c_str() );
            gclsDispatcher.StopCall( pszCallId, SIP_NOT_ACCEPTABLE_HERE );
            return true;
        }
    }

    // 감청자 RTP 주소 + tap_mode
    int iMonAudio = pclsRtp->GetAudioPort();
    if ( iMonAudio <= 0 && pclsRtp->m_iPort > 0 ) iMonAudio = pclsRtp->m_iPort;
    int iMonVideo = ( pclsRtp->GetMediaCount() >= 2 ) ? pclsRtp->GetVideoPort() : 0;
    int iMonPt = 0, iMonSrcPt = 0, iMonTePt = 0, iMonSrcTePt = 0;
    std::string strMonCodec;
    CGroupCallService::GetLegPt( pszCallId, false, iMonPt, iMonSrcPt, iMonTePt, iMonSrcTePt, &strMonCodec );

    std::string strTapId = std::string( "tap-" ) + pszCallId;
    std::string strTapMode = "both";
    {
        CSipHeader *pMode = pclsMessage->GetHeader( "X-Tap-Mode" );  // 운용 선택(민원인만 등) — 확장 헤더
        if ( pMode && ( pMode->m_strValue == "a" || pMode->m_strValue == "b" ) ) strTapMode = pMode->m_strValue;
    }

    uint32_t uSsrcA = 0, uSsrcB = 0;
    std::string strTapLocalIp, strErrCode;
    int iTapLocalPort = 0, iTapLocalVideoPort = 0;
    if ( !gclsCmpClient.AddTap( clsTarget.m_strRelaySessionId, strTapId, pclsRtp->m_strIp, iMonAudio, iMonVideo,
                                strTapMode, strMonitor, iMonPt, iMonTePt, clsTarget.m_strRelaySesId, "volte",
                                clsTapAudioCrypto.bEnabled ? &clsTapAudioCrypto : NULL,
                                clsTapVideoCrypto.bEnabled ? &clsTapVideoCrypto : NULL, uSsrcA, uSsrcB, strTapLocalIp,
                                iTapLocalPort, iTapLocalVideoPort, strErrCode ) ) {
        int iRc = ( strErrCode == "LIMIT" ) ? SIP_BUSY_HERE : SIP_NOT_ACCEPTABLE_HERE;
        CLog::Print( LOG_ERROR, "Join — AddTap failed (%s) → %d", strErrCode.c_str(), iRc );
        gclsDispatcher.StopCall( pszCallId, iRc );
        return true;
    }

    // 200 OK — sendonly + tap 포트 + 서버 키(a=crypto) + a=ssrc 라벨(RFC 5576, 귀속). A/B 무영향(은닉).
    const std::string strRelayIp = strTapLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : strTapLocalIp;
    MediaSdes::RewriteRelaySdpForLeg( pclsRtp->m_clsMediaList, clsTapLeg, false );
    pclsRtp->SetIpPort( strRelayIp.c_str(), iTapLocalPort, SOCKET_COUNT_PER_MEDIA );
    pclsRtp->SetDirection( E_RTP_SEND );  // 서버→감청자 단방향
    // a=ssrc 라벨 — 첫 audio m-line 에 caller/callee SSRC 표기.
    for ( auto &clsMedia : pclsRtp->m_clsMediaList ) {
        if ( clsMedia.m_strMedia != "audio" ) continue;
        char szA[96], szB[96];
        snprintf( szA, sizeof( szA ), "%u cname:tap-%s", uSsrcA, strTapId.c_str() );
        snprintf( szB, sizeof( szB ), "%u cname:tap-%s", uSsrcB, strTapId.c_str() );
        clsMedia.AddAttribute( "ssrc", szA );
        clsMedia.AddAttribute( "ssrc", ( std::string( szA ) + " label:caller" ).c_str() );
        clsMedia.AddAttribute( "ssrc", szB );
        clsMedia.AddAttribute( "ssrc", ( std::string( szB ) + " label:callee" ).c_str() );
        break;
    }
    if ( gclsUserAgent.AcceptCall( pszCallId, pclsRtp ) == false ) {
        gclsCmpClient.RemoveTap( clsTarget.m_strRelaySessionId, strTapId, strMonitor, clsTarget.m_strRelaySesId,
                                 "volte" );
        gclsUserAgent.StopCall( pszCallId );
        return true;
    }

    MonitorLeg leg;
    leg.strSessionId = clsTarget.m_strRelaySessionId;
    leg.strSesId = clsTarget.m_strRelaySesId;
    leg.strService = "volte";
    leg.strTapId = strTapId;
    leg.strMonitor = strMonitor;
    leg.strGroupId = gclsDispatchGroupMap.EffectiveGroupOf( strMonitor.c_str() );
    leg.strTargetA = strCaller;
    leg.strTargetB = strCallee;
    leg.strTapMode = strTapMode;
    leg.tStart = time( NULL );
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutexMonitor );
        m_mapMonitorLeg[pszCallId] = leg;
        m_mapSessionMonitors[leg.strSessionId].insert( pszCallId );
    }
    gclsDispatcher.SetCallOwner( pszCallId, this );
    _emitCallMonitored( leg, "started", -1 );
    CLog::Print( LOG_INFO, "Join — %s monitoring session=%s (targets %s/%s) tap=%s mode=%s ssrc=%u/%u [TAS]",
                 strMonitor.c_str(), leg.strSessionId.c_str(), strCaller.c_str(), strCallee.c_str(), strTapId.c_str(),
                 strTapMode.c_str(), uSsrcA, uSsrcB );
    return true;
}

bool CTasModule::HandleMonitorLegEnd( const char *pszCallId ) {
    MonitorLeg leg;
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutexMonitor );
        auto it = m_mapMonitorLeg.find( pszCallId );
        if ( it == m_mapMonitorLeg.end() ) return false;
        leg = it->second;
        m_mapMonitorLeg.erase( it );
        auto itS = m_mapSessionMonitors.find( leg.strSessionId );
        if ( itS != m_mapSessionMonitors.end() ) {
            itS->second.erase( pszCallId );
            if ( itS->second.empty() ) m_mapSessionMonitors.erase( itS );
        }
    }
    gclsCmpClient.RemoveTap( leg.strSessionId, leg.strTapId, leg.strMonitor, leg.strSesId, leg.strService );
    gclsDispatcher.RemoveCallOwner( pszCallId );
    int iDurMs = leg.tStart > 0 ? (int)( ( time( NULL ) - leg.tStart ) * 1000 ) : -1;
    _emitCallMonitored( leg, "ended", iDurMs );
    CLog::Print( LOG_INFO, "Join — monitor leg(%s) ended, tap=%s released [TAS]", pszCallId, leg.strTapId.c_str() );
    return true;
}

void CTasModule::ReleaseSessionMonitors( const std::string &strRelaySessionId ) {
    std::vector<std::string> vecLegs;
    {
        std::lock_guard<std::recursive_mutex> lock( m_mutexMonitor );
        auto it = m_mapSessionMonitors.find( strRelaySessionId );
        if ( it == m_mapSessionMonitors.end() ) return;
        vecLegs.assign( it->second.begin(), it->second.end() );
    }
    // 원 통화 종료 — 감청자에게 BYE. tap 은 RELAY_REMOVE 로 CMP 가 일괄 회수하므로 RemoveTap 은 생략 가능하나,
    //   HandleMonitorLegEnd 가 지도 정리·감사 종료를 수행하도록 StopCall→EventCallEnd 경로를 태운다.
    for ( const auto &strLeg : vecLegs ) {
        gclsUserAgent.StopCall( strLeg.c_str() );
        HandleMonitorLegEnd( strLeg.c_str() );
    }
}
