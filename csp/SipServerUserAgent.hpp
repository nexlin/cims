
#ifndef _SIP_SERVER_USER_AGENT_HPP_
#define _SIP_SERVER_USER_AGENT_HPP_

#include "GroupMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "SipServerSetup.h"
#include "CscfModule.h"
#include "ModuleDispatcher.h"
#include "CspSipServer.h"


bool CSipServer::CheckAuthrization( CSipMessage *pclsMessage ) {
    return CCscfModule::CheckAuthrization( pclsMessage );
}

/**
 * @ingroup CspServer
 * @brief SIP REGISTER 응답 메시지 수신 이벤트 핸들러
 * @param pclsInfo	SIP REGISTER 응답 메시지를 전송한 IP-PBX 정보 저장 객체
 * @param iStatus		SIP REGISTER 응답 코드
 */
void CSipServer::EventRegister( CSipServerInfo *pclsInfo, int iStatus ) {
    gclsSipServerMap.Set( pclsInfo, iStatus );
}

/**
 * @ingroup CspServer
 * @brief SIP 통화 요청 수신에 대한 인증 확인 이벤트 핸들러
 * @param pclsMessage	SIP INVITE 요청 메시지
 * @return 인증에 성공하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServer::EventIncomingRequestAuth( CSipMessage *pclsMessage ) {
    std::string strIp;
    int iPort;
    CUserInfo clsUserInfo;

    if ( pclsMessage->GetTopViaIpPort( strIp, iPort ) == false ) {
        CLog::Print( LOG_ERROR, "EventIncomingRequestAuth - GetTopViaIpPort error" );
        SendResponse( pclsMessage, SIP_BAD_REQUEST );
        return false;
    }

    CLog::Print( LOG_DEBUG, "EventIncomingCall: Step 2 (SipServerMap)" );
    // IP-PBX 에서 전송한 SIP 요청 메시지는 인증 허용으로 처리한다.
    if ( gclsSipServerMap.Select( strIp.c_str(), pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str() ) ) {
        CLog::Print( LOG_DEBUG, "EventIncomingRequestAuth ip(%s) user(%s) IP-PBX => allowed", strIp.c_str(),
                     pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str() );
        return true;
    }


    CspUser clsCspUser; 
    bool bCspUserFound = gclsCspUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsCspUser );
    
    if ( gclsUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsUserInfo ) == false && !bCspUserFound ) {
        std::string strDestToId;

        // IP-PBX 에서 수신한 BYE 메시지를 정상적으로 처리하기 위한 기능
        if ( pclsMessage->IsMethod( SIP_METHOD_BYE ) ) {
            std::string strCallId;

            pclsMessage->GetCallId( strCallId );

            if ( gclsCallMap.Select( strCallId.c_str() ) ) {
                CLog::Print( LOG_DEBUG, "EventIncomingRequestAuth BYE CallId(%s) is found", strCallId.c_str() );
                return true;
            }
        } else if ( gclsSipServerMap.SelectIncomingRoute(
                        strIp.c_str(), pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str(), strDestToId ) ) {
            CLog::Print( LOG_DEBUG, "EventIncomingRequestAuth ip(%s) user(%s) IP-PBX dest_user(%s)", strIp.c_str(),
                         pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str(), strDestToId.c_str() );
            return true;
        }

        if ( CheckAuthrization( pclsMessage ) == false ) {
            CLog::Print( LOG_DEBUG, "EventIncomingRequestAuth no UserInfo and CheckAuthrization() return false" );
            return false;
        }

        if ( gclsUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsUserInfo ) == false && 
             !gclsCspUserMap.Select( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsCspUser )) {
            CLog::Print( LOG_DEBUG,
                         "EventIncomingRequestAuth no UserInfo and CheckAuthrization() return true and no UserInfo" );
            return false;
        }
    }

    if ( strcmp( clsUserInfo.m_strIp.c_str(), strIp.c_str() ) || clsUserInfo.m_iPort != iPort ) {
        CLog::Print( LOG_DEBUG, "EventIncomingRequestAuth current(%s:%d) != recv(%s:%d)", clsUserInfo.m_strIp.c_str(),
                     clsUserInfo.m_iPort, strIp.c_str(), iPort );

        if ( CheckAuthrization( pclsMessage ) == false ) {
            CLog::Print( LOG_DEBUG, "EventIncomingRequestAuth CheckAuthrization() return false" );
            return false;
        }

        gclsUserMap.SetIpPort( pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), strIp.c_str(), iPort );
    }

    return true;
}

/**
 * @ingroup CspServer
 * @brief SIP 통화 요청 수신 이벤트 핸들러
 * @param	pszCallId	SIP Call-ID
 * @param pszFrom		SIP From 사용자 아이디
 * @param pszTo			SIP To 사용자 아이디
 * @param pclsRtp		RTP 정보 저장 객체
 */
void CSipServer::EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                    CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    CLog::Print( LOG_DEBUG, "EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo );
    CspUser clsUser;
    CUserInfo clsUserInfo;
    bool bRoutePrefix = false;
    std::string strTo;

    if ( strlen( pszTo ) == 0 ) {
        CLog::Print( LOG_DEBUG, "EventIncomingCall to(%s) is not defined", pszTo );
        return StopCall( pszCallId, SIP_DECLINE );
    }

    // ── 모듈 라우팅 ──────────────────────────────────────────

    // 1. PTT-AS: 그룹 번호 대상
    if ( gclsDispatcher.GetPttAs()->IsEnabled() && gclsGroupMap.Contains( pszTo ) ) {
        // 단말 직접 그룹 INVITE는 거부 (CSP가 그룹 세션을 관리)
        CLog::Print( LOG_INFO, "EventIncomingCall: PTT terminal(%s) sent INVITE to group(%s) - rejected 403", pszFrom, pszTo );
        gclsDispatcher.SetCallOwner( pszCallId, gclsDispatcher.GetPttAs() );
        return StopCall( pszCallId, SIP_FORBIDDEN );
    }

    // 서비스 모드 체크
    {
        CspUser clsFromUser;
        bool bFromKnown = gclsCspUserMap.isAlive( pszFrom, clsFromUser );
        const std::string& mode = gclsSetup.m_strServiceMode;

        if ( mode == "ptt" ) {
            CLog::Print( LOG_INFO, "EventIncomingCall: VoIP call rejected (ServiceMode=ptt)" );
            return StopCall( pszCallId, SIP_FORBIDDEN );
        }
        if ( bFromKnown && !clsFromUser.m_strServiceType.empty() && clsFromUser.m_strServiceType == "ptt" ) {
            CLog::Print( LOG_INFO, "EventIncomingCall: PTT-only user(%s) cannot originate VoIP call - rejected 403", pszFrom );
            return StopCall( pszCallId, SIP_FORBIDDEN );
        }
    }

    if ( gclsCspUserMap.isAlive( pszTo, clsUser ) == false ) {
        // 2. PTT-AS: 그룹콜 (비등록 대상)
        CspPttGroup clsGroup;
        if ( gclsDispatcher.GetPttAs()->IsEnabled() && gclsGroupMap.Select( pszTo, clsGroup ) ) {
            CLog::Print( LOG_DEBUG, "EventIncomingCall to(%s) is Group(%s)", pszTo, clsGroup._name.c_str() );
            CSipCallRoute clsRouteTemp;
            clsUserInfo.GetCallRoute( clsRouteTemp );

            if ( gclsGroupCallService.ProcessGroupCall( pszTo, pszFrom, pszCallId, pclsRtp, &clsRouteTemp ) ) {
                gclsDispatcher.SetCallOwner( pszCallId, gclsDispatcher.GetPttAs() );
                return;
            }
        }

        CLog::Print( LOG_DEBUG, "## 1 EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo );

        // 3. IBCF: 트렁크 라우팅
        CspSipServer clsSipServer;
        if ( gclsDispatcher.GetIbcf()->IsEnabled() && gclsSipServerMap.SelectRoutePrefix( pszTo, clsSipServer, strTo ) ) {
            clsUser.m_strId = clsSipServer.m_strUserId;
            clsUser.m_strPassWord = clsSipServer.m_strPassWord;

            clsUserInfo.m_strIp = clsSipServer.m_strIp;
            clsUserInfo.m_iPort = clsSipServer.m_iPort;
            clsUserInfo.m_eTransport = E_SIP_UDP;

            pszFrom = clsUser.m_strId.c_str();
            pszTo = strTo.c_str();

            bRoutePrefix = true;
            gclsDispatcher.SetCallOwner( pszCallId, gclsDispatcher.GetIbcf() );
            CLog::Print( LOG_DEBUG, "EventIncomingCall routePrefix IP-PBX(%s:%d) [IBCF]", clsUserInfo.m_strIp.c_str(),
                         clsUserInfo.m_iPort );
        } else if ( gclsDispatcher.GetIbcf()->IsEnabled() && gclsSipServerMap.SelectIncomingRoute( NULL, pszTo, strTo ) ) {
            if ( gclsCspUserMap.isAlive( strTo, clsUser ) == false ) {
                CLog::Print( LOG_DEBUG, "EventIncomingCall from(%s) to(%s) is not found - dest to(%s)", pszFrom, pszTo, strTo.c_str() );
                return StopCall( pszCallId, SIP_NOT_FOUND );
            }
            gclsDispatcher.SetCallOwner( pszCallId, gclsDispatcher.GetIbcf() );
        } else if ( gclsSetup.IsCallPickupId( pszTo ) ) {
            // 4. TAS: 콜픽업
            gclsDispatcher.SetCallOwner( pszCallId, gclsDispatcher.GetTas() );
            return PickUp( pszCallId, pszFrom, pszTo, pclsRtp );
        } else {
            CLog::Print( LOG_DEBUG, "EventIncomingCall from(%s) to(%s) is not found in XML or DB", pszFrom, pszTo );
            return StopCall( pszCallId, SIP_NOT_FOUND );
        }
    }

    // 5. TAS: 서비스 로직 (DND, 착신전환, 착신거부)
    if ( gclsDispatcher.GetCallOwner( pszCallId ) == NULL ) {
        gclsDispatcher.SetCallOwner( pszCallId, gclsDispatcher.GetTas() );
    }

    if ( clsUser.isDnd() || clsUser.isReject(pszFrom) ) {
        CLog::Print( LOG_DEBUG, "EventIncomingCall from(%s) to(%s) is DND or Reject [TAS]", pszFrom, pszTo );
        return StopCall( pszCallId, SIP_DECLINE );
    }

    if ( clsUser.isCallForward() ) {
        CLog::Print( LOG_DEBUG, "EventIncomingCall from(%s) to(%s) is CallForward(%s) [TAS]", pszFrom, pszTo,
                     clsUser.m_strForward.c_str() );

        CSipMessage *pclsInvite = gclsUserAgent.DeleteIncomingCall( pszCallId );
        if ( pclsInvite ) {
            CSipMessage *pclsResponse = pclsInvite->CreateResponseWithToTag( SIP_MOVED_TEMPORARILY );
            if ( pclsResponse ) {
                CSipFrom clsContact;

                clsContact.m_clsUri.m_strProtocol = SIP_PROTOCOL;
                clsContact.m_clsUri.m_strUser = clsUser.m_strForward;
                clsContact.m_clsUri.m_strHost = gclsSetup.m_strLocalIp;
                clsContact.m_clsUri.m_iPort = gclsSetup.m_iUdpPort;

                pclsResponse->m_clsContactList.push_back( clsContact );

                gclsUserAgent.m_clsSipStack.SendSipMessage( pclsResponse );
                return;
            }
        } else {
            CLog::Print( LOG_ERROR, "EventIncomingCall(%s) INVITE it not found", pszCallId );
        }

        return StopCall( pszCallId, SIP_MOVED_TEMPORARILY );
    }

    // ── B2BUA 호 설정 (TAS/IBCF 공통) ──────────────────────

    if ( bRoutePrefix == false ) {
        CLog::Print( LOG_DEBUG, "## 5 EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo );
        if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) {
            CLog::Print( LOG_DEBUG, "EventIncomingCall(%s) to(%s) is not found", pszCallId, pszTo );
            return StopCall( pszCallId, SIP_NOT_FOUND );
        }
    }

    int iStartPort = -1;
    std::string strCallId;
    CSipCallRoute clsRoute;

    if ( gclsSetup.m_bUseRtpRelay ) {
        iStartPort = gclsRtpMap.CreatePort( SOCKET_COUNT_PER_MEDIA * pclsRtp->GetMediaCount() );
        if ( iStartPort == -1 ) {
            return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
        }

        CLog::Print( LOG_DEBUG, "EventIncomingCall: Update Caller Info (Peer A) for Port %d", iStartPort );

        if (pclsRtp->GetMediaCount() >= 2) {
             int iVideoPort = pclsRtp->GetVideoPort();
             if (iVideoPort > 0) {
                  gclsRtpMap.SetIpPort(iStartPort, 2, inet_addr(pclsRtp->m_strIp.c_str()), iVideoPort);
             }
        }

        int iAudioPort = pclsRtp->GetAudioPort();
        if (iAudioPort <= 0 && pclsRtp->m_iPort > 0) {
             iAudioPort = pclsRtp->m_iPort;
        }

        if (iAudioPort > 0) {
             gclsRtpMap.SetIpPort(iStartPort, 0, inet_addr(pclsRtp->m_strIp.c_str()), iAudioPort);
        }

        std::string strRelayIp = gclsSetup.m_strLocalIp;
        std::string strAllocatedIp;
        if (gclsRtpMap.GetLocalIp(iStartPort, strAllocatedIp) && !strAllocatedIp.empty()) {
            strRelayIp = strAllocatedIp;
        }

        pclsRtp->SetIpPort( strRelayIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );
    clsRoute.m_b100rel = gclsUserAgent.Is100rel( pszCallId );

    CSipMessage *pclsInvite;

    CLog::Print( LOG_DEBUG, "EventIncomingCall: Step 5 (CreateCall)" );
    if ( gclsUserAgent.CreateCall( pszFrom, pszTo, pclsRtp, &clsRoute, strCallId, &pclsInvite ) == false ) {
        CLog::Print( LOG_ERROR, "EventIncomingCall(%s) CreateCall errr", pszCallId );
        return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
    }

    gclsCallMap.Insert( pszCallId, strCallId.c_str(), iStartPort );
    // Peer CallId 에도 동일한 소유권 설정
    gclsDispatcher.SetCallOwner( strCallId.c_str(), gclsDispatcher.GetCallOwner( pszCallId ) );

    if ( gclsUserAgent.StartCall( strCallId.c_str(), pclsInvite ) == false ) {
        gclsCallMap.Delete( pszCallId );

        CLog::Print( LOG_ERROR, "EventIncomingCall(%s) StartCall error", pszCallId );
        return StopCall( pszCallId, SIP_INTERNAL_SERVER_ERROR );
    }

    // [CALL LOG] VoIP 세션 기록 — INVITE 수신 시점
    if ( gclsDbManager.IsConnected() ) {
        gclsDbManager.InsertCallLog( pszCallId, false, "", pszFrom, pszTo );
        gclsDbManager.InsertParticipant( pszCallId, pszFrom, "caller", true );
        gclsDbManager.InsertParticipant( pszCallId, pszTo,   "callee", false );
    }
}

/**
 * @ingroup CspServer
 * @brief SIP Ring / Session Progress 수신 이벤트 핸들러
 * @param	pszCallId		SIP Call-ID
 * @param iSipStatus	SIP 응답 코드
 * @param pclsRtp			RTP 정보 저장 객체
 */
void CSipServer::EventCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;

    CLog::Print( LOG_DEBUG, "EventCallRing(%s,%d)", pszCallId, iSipStatus );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            pclsRtp->m_iPort = clsCallInfo.m_iPeerRtpPort;
            pclsRtp->m_strIp = gclsSetup.m_strLocalIp;
        }

        int iRSeq = gclsUserAgent.GetRSeq( pszCallId );
        if ( iRSeq != -1 ) {
            gclsUserAgent.SetRSeq( clsCallInfo.m_strPeerCallId.c_str(), iRSeq );
        }

        gclsUserAgent.RingCall( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus, pclsRtp );
    } else if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) ) {
        gclsUserAgent.SendNotify( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus );
    }
}

/**
 * @ingroup CspServer
 * @brief SIP 통화 연결 이벤트 핸들러
 * @param	pszCallId	SIP Call-ID
 * @param pclsRtp		RTP 정보 저장 객체
 */
 
void CSipServer::EventCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;

    CLog::Print( LOG_DEBUG, "EventCallStart(%s), ", pszCallId );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            // [MODIFIED] Update Local RtpMap with Remote Info so it triggers UpdateSession
            // [MODIFIED] Update Local RtpMap with Remote Info so it triggers UpdateSession
            // Thread-safe implementation using GetLocalIp for existence check and SetIpPort
            int iRtpPort = clsCallInfo.m_iPeerRtpPort;
            
            std::string strAllocatedIp;
            if (!gclsRtpMap.GetLocalIp(iRtpPort, strAllocatedIp)) {
                // If Leg 2 (Send side), CallMap might have stored StartPort + 2, 
                // but EventIncomingCall used StartPort. Try accounting for that.
                if (gclsRtpMap.GetLocalIp(iRtpPort - 2, strAllocatedIp)) {
                     iRtpPort = iRtpPort - 2;
                }
            }

            if (!strAllocatedIp.empty()) {

                // Update Video First (Index 2) so Audio (Index 0) trigger has it
                if (pclsRtp->GetMediaCount() >= 2) { 
                     int iVideoPort = pclsRtp->GetVideoPort();
                     if (iVideoPort > 0) {
                         gclsRtpMap.SetIpPort(iRtpPort, 2, inet_addr(pclsRtp->m_strIp.c_str()), iVideoPort, 1);
                     }
                }
                
                int iAudioPort = pclsRtp->GetAudioPort();
                
                if (iAudioPort <= 0 && pclsRtp->m_iPort > 0) {
                    iAudioPort = pclsRtp->m_iPort;
                }

                if (iAudioPort > 0) {
                     // This triggers UpdateSession with Peer Idx 1
                     gclsRtpMap.SetIpPort(iRtpPort, 0, inet_addr(pclsRtp->m_strIp.c_str()), iAudioPort, 1);
                }
            }
            
            // [GROUP CALL FIX] Notify Service independent of RtpMap existence (Shared Session Support)
            int iRemoteAudio = pclsRtp->GetAudioPort();
            CLog::Print( LOG_DEBUG, "EventCallStart: GetAudioPort=%d m_iPort=%d IP=%s", iRemoteAudio, pclsRtp->m_iPort, pclsRtp->m_strIp.c_str() );

            if (iRemoteAudio <= 0 && pclsRtp->m_iPort > 0) iRemoteAudio = pclsRtp->m_iPort;

            if (iRemoteAudio > 0) {
                int iRemoteVideo = pclsRtp->GetVideoPort();
                CLog::Print( LOG_DEBUG, "EventCallStart: Calling OnCallStarted with %s:%d video=%d", pclsRtp->m_strIp.c_str(), iRemoteAudio, iRemoteVideo );
                gclsGroupCallService.OnCallStarted(pszCallId, pclsRtp->m_strIp, iRemoteAudio, 0, iRemoteVideo);
            }

            std::string strRelayIp = gclsSetup.m_strLocalIp;
            if (!strAllocatedIp.empty()) {
                strRelayIp = strAllocatedIp;
            }

            pclsRtp->SetIpPort( strRelayIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        } else if ( clsCallInfo.m_iPeerRtpPort > 0 ) {
            // pclsRtp 없는 PTT 그룹 통화: cwrtc RTP 포트로 JoinGroup
            CLog::Print( LOG_DEBUG, "EventCallStart: PTT fallback OnCallStarted (no RTP info) sharedPort=%d", clsCallInfo.m_iPeerRtpPort );
            gclsGroupCallService.OnCallStarted(pszCallId, gclsSetup.m_strLocalIp, clsCallInfo.m_iPeerRtpPort);
        }


        if ( gclsUserAgent.IsConnected( clsCallInfo.m_strPeerCallId.c_str() ) ) {
            gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
        } else {
            gclsUserAgent.AcceptCall( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
        }
    } else if ( gclsTransCallMap.Select( pszCallId, clsCallInfo ) ) {
        gclsUserAgent.SendNotify( clsCallInfo.m_strPeerCallId.c_str(), SIP_OK );

        std::string strReferToCallId;
        int iStartPort = -1;

        if ( gclsCallMap.Select( clsCallInfo.m_strPeerCallId.c_str(), strReferToCallId ) ) {
            gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str() );
            gclsCallMap.Delete( clsCallInfo.m_strPeerCallId.c_str() );
        }

        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            iStartPort = clsCallInfo.m_iPeerRtpPort - 2;

            pclsRtp->SetIpPort( gclsSetup.m_strLocalIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        }

        gclsUserAgent.SendReInvite( strReferToCallId.c_str(), pclsRtp );

        gclsCallMap.Insert( strReferToCallId.c_str(), pszCallId, iStartPort );
        gclsTransCallMap.Delete( pszCallId, false );
    } else {
        gclsUserAgent.StopCall( pszCallId );
    }
}

/**
 * @ingroup CspServer
 * @brief SIP 통화 종료 이벤트 핸들러
 * @param	pszCallId		SIP Call-ID
 * @param iSipStatus	SIP 응답 코드. INVITE 에 대한 오류 응답으로 전화가 종료된 경우, INVITE 의 응답 코드를 저장한다.
 */
void CSipServer::EventCallEnd( const char *pszCallId, int iSipStatus ) {
    CCallInfo clsCallInfo;

    CLog::Print( LOG_DEBUG, "EventCallEnd(%s:%d)", pszCallId, iSipStatus );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( clsCallInfo.m_bRecv ) {
            SaveCdr( pszCallId, iSipStatus );
        } else {
            SaveCdr( clsCallInfo.m_strPeerCallId.c_str(), iSipStatus );
        }

        gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str() );

        bool bIsGroup = gclsGroupCallService.OnCallTerminated( pszCallId );

        gclsCallMap.Delete( pszCallId, !bIsGroup );

        // 콜 소유권 정리
        gclsDispatcher.RemoveCallOwner( pszCallId );
        gclsDispatcher.RemoveCallOwner( clsCallInfo.m_strPeerCallId.c_str() );
    } else {
        std::string strCallId;

        if ( gclsTransCallMap.Select( pszCallId, strCallId ) ) {
            gclsUserAgent.SendNotify( strCallId.c_str(), iSipStatus );
            gclsTransCallMap.Delete( pszCallId );
        }

        gclsDispatcher.RemoveCallOwner( pszCallId );
    }
}

/**
 * @ingroup CspServer
 * @brief SIP ReINVITE 수신 이벤트 핸들러
 * @param	pszCallId	SIP Call-ID
 * @param pclsRemoteRtp		상대방 RTP 정보 저장 객체
 * @param pclsLocalRtp		내 RTP 정보 저장 객체
 */
void CSipServer::EventReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) {
    CCallInfo clsCallInfo;

    CLog::Print( LOG_DEBUG, "EventReInvite(%s)", pszCallId );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRemoteRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            pclsRemoteRtp->SetIpPort( gclsSetup.m_strLocalIp.c_str(), clsCallInfo.m_iPeerRtpPort,
                                      SOCKET_COUNT_PER_MEDIA );
        }

        gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), pclsRemoteRtp );
    }
}

/**
 * @ingroup CspServer
 * @brief SIP PRACK 수신 이벤트 핸들러
 * @param	pszCallId	SIP Call-ID
 * @param pclsRtp		RTP 정보 저장 객체
 */
void CSipServer::EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    CCallInfo clsCallInfo;

    CLog::Print( LOG_DEBUG, "EventPrack(%s)", pszCallId );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) ) {
        if ( pclsRtp && clsCallInfo.m_iPeerRtpPort > 0 ) {
            pclsRtp->SetIpPort( gclsSetup.m_strLocalIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA );
        }

        gclsUserAgent.SendPrack( clsCallInfo.m_strPeerCallId.c_str(), pclsRtp );
    }
}

/**
 * @ingroup CspServer
 * @brief Screened / Unscreened Transfer 요청 수신 이벤트 핸들러
 * @param pszCallId					SIP Call-ID
 * @param pszReferToCallId	전화가 전달될 SIP Call-ID
 * @param bScreenedTransfer Screened Transfer 이면 true 가 입력되고 Unscreened Transfer 이면 false 가 입력된다.
 * @returns 요청을 수락하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServer::EventTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreenedTransfer ) {
    CCallInfo clsCallInfo, clsReferToCallInfo;
    CSipCallRtp clsRtp;

    CLog::Print( LOG_DEBUG, "EventTransfer(%s,%s,%s)", pszCallId, pszReferToCallId,
                 ( bScreenedTransfer ? "screened" : "unscreened" ) );

    if ( gclsCallMap.Select( pszCallId, clsCallInfo ) == false ) return false;
    if ( gclsCallMap.Select( pszReferToCallId, clsReferToCallInfo ) == false ) return false;

    gclsCallMap.Delete( pszCallId );
    gclsCallMap.Delete( pszReferToCallId, false );

    if ( gclsUserAgent.GetRemoteCallRtp( clsCallInfo.m_strPeerCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );

    if ( gclsSetup.m_bUseRtpRelay ) {
        if ( bScreenedTransfer ) {
            clsRtp.SetIpPort( gclsSetup.m_strLocalIp.c_str(), clsReferToCallInfo.m_iPeerRtpPort,
                              SOCKET_COUNT_PER_MEDIA );
        } else {
            clsRtp.SetIpPort( gclsSetup.m_strLocalIp.c_str(), clsReferToCallInfo.m_iPeerRtpPort + 2,
                              SOCKET_COUNT_PER_MEDIA );
        }
    }

    if ( bScreenedTransfer ) {
        CSipCallRtp clsReferToRtp;

        if ( gclsUserAgent.GetRemoteCallRtp( clsReferToCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp ) == false )
            return false;
        clsReferToRtp.SetDirection( E_RTP_SEND_RECV );

        if ( gclsSetup.m_bUseRtpRelay ) {
            clsReferToRtp.SetIpPort( gclsSetup.m_strLocalIp.c_str(), clsReferToCallInfo.m_iPeerRtpPort + 2,
                                     SOCKET_COUNT_PER_MEDIA );
        }

        gclsCallMap.Insert( clsCallInfo.m_strPeerCallId.c_str(), clsReferToCallInfo.m_strPeerCallId.c_str(),
                            clsReferToCallInfo.m_iPeerRtpPort );
        gclsUserAgent.SendReInvite( clsCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp );
        gclsUserAgent.SendReInvite( clsReferToCallInfo.m_strPeerCallId.c_str(), &clsRtp );
    }

    gclsUserAgent.StopCall( pszCallId );
    gclsUserAgent.StopCall( pszReferToCallId, SIP_REQUEST_TERMINATED );

    if ( bScreenedTransfer == false ) {
        std::string strNewCallId, strFromId, strToId;
        CUserInfo clsUserInfo;
        CSipCallRoute clsRoute;

        gclsUserAgent.GetToId( clsCallInfo.m_strPeerCallId.c_str(), strFromId );
        gclsUserAgent.GetToId( clsReferToCallInfo.m_strPeerCallId.c_str(), strToId );

        if ( gclsUserMap.Select( strToId.c_str(), clsUserInfo ) ) {
            clsUserInfo.GetCallRoute( clsRoute );

            gclsUserAgent.StopCall( clsReferToCallInfo.m_strPeerCallId.c_str() );
            gclsUserAgent.StartCall( strFromId.c_str(), strToId.c_str(), &clsRtp, &clsRoute, strNewCallId );

            gclsCallMap.Insert( strNewCallId.c_str(), clsCallInfo.m_strPeerCallId.c_str(),
                                clsReferToCallInfo.m_iPeerRtpPort );
        } else {
            gclsUserAgent.StopCall( clsCallInfo.m_strPeerCallId.c_str() );
            gclsUserAgent.StopCall( clsReferToCallInfo.m_strPeerCallId.c_str() );
        }
    }

    if ( gclsSetup.m_bUseRtpRelay ) {
        gclsRtpMap.ReSetIpPort( clsReferToCallInfo.m_iPeerRtpPort );
    }

    return true;
}

/**
 * @ingroup CspServer
 * @brief Blind Transfer 요청 수신 이벤트 핸들러
 * @param pszCallId			SIP Call-ID
 * @param pszReferToId	전화가 전달될 사용자 아이디
 * @returns 요청을 수락하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CSipServer::EventBlindTransfer( const char *pszCallId, const char *pszReferToId ) {
    std::string strCallId, strInviteCallId, strToId;
    CSipCallRtp clsRtp;
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    int iStartPort = -1;

    CLog::Print( LOG_DEBUG, "EventBlindTransfer(%s,%s)", pszCallId, pszReferToId );

    if ( gclsCallMap.Select( pszCallId, strCallId ) == false ) return false;
    if ( gclsUserAgent.GetToId( strCallId.c_str(), strToId ) == false ) return false;
    if ( gclsUserMap.Select( pszReferToId, clsUserInfo ) == false ) return false;

    if ( gclsUserAgent.GetRemoteCallRtp( strCallId.c_str(), &clsRtp ) == false ) return false;
    clsRtp.SetDirection( E_RTP_SEND_RECV );

    if ( gclsSetup.m_bUseRtpRelay ) {
        iStartPort = gclsRtpMap.CreatePort( SOCKET_COUNT_PER_MEDIA * clsRtp.GetMediaCount() );
        if ( iStartPort == -1 ) return false;

        clsRtp.SetIpPort( gclsSetup.m_strLocalIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA );
    }

    clsUserInfo.GetCallRoute( clsRoute );

    if ( gclsUserAgent.StartCall( strToId.c_str(), pszReferToId, &clsRtp, &clsRoute, strInviteCallId ) == false ) {
        return false;
    }

    gclsTransCallMap.Insert( pszCallId, strInviteCallId.c_str(), iStartPort );

    return true;
}

/**
 * @ingroup CspServer
 * @brief SIP MESSAGE 메시지 수신 이벤트 핸들러
 * @param pszFrom			발신자 아이디
 * @param pszTo				수신자 아이디
 * @param pclsMessage 수신된 SIP 메시지
 * @returns
 */
bool CSipServer::EventMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;

    if ( gclsUserMap.Select( pszTo, clsUserInfo ) == false ) return false;

    clsUserInfo.GetCallRoute( clsRoute );

    return gclsUserAgent.SendSms( pszFrom, pszTo, pclsMessage->m_strBody.c_str(), &clsRoute );
}

#endif
