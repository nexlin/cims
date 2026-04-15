/**
 * CModuleDispatcher — CSipServer 를 완전히 대체하는 중앙 디스패처
 *
 * ISipStackCallBack: REGISTER, SUBSCRIBE, Proxy INVITE
 * ISipUserAgentCallBack: B2BUA 호 이벤트 (모듈별 분배)
 *
 * RecvRequest 콜백 순서: [ModuleDispatcher, CSipUserAgent]
 *  → Proxy 대상 INVITE: ModuleDispatcher 직접 처리 (return true)
 *  → B2BUA 대상 INVITE: return false → CSipUserAgent 처리
 */

#include "ModuleDispatcher.h"

#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspServer.h"
#include "CspSipServer.h"
#include "CspUser.h"
#include "CspPttGroup.h"
#include "DbManager.h"
#include "Directory.h"
#include "GroupCallService.h"
#include "GroupMap.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "NonceMap.h"
#include "RtpMap.h"
#include "SipMd5.h"
#include "SipServerMap.h"
#include "SipServerSetup.h"
#include "SipUtility.h"
#include "SipUserAgent.h"
#include "SubscriptionManager.h"
#include "TimeString.h"
#include "UserMap.h"

CModuleDispatcher gclsDispatcher;

extern void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);
extern void SendInitialNotify(const SubscriptionInfo& sub);

// ──────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ──────────────────────────────────────────────────────────────

CModuleDispatcher::CModuleDispatcher() {}
CModuleDispatcher::~CModuleDispatcher() {}

// ──────────────────────────────────────────────────────────────
//  Start — 콜백 순서: [ModuleDispatcher, CSipUserAgent]
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::InitModules() {
    CLog::Print(LOG_SYSTEM, "ModuleDispatcher: Roles CSCF=%s TAS=%s PTT-AS=%s IBCF=%s",
        m_clsCscf.IsEnabled()  ? "ON" : "OFF",
        m_clsTas.IsEnabled()   ? "ON" : "OFF",
        m_clsPttAs.IsEnabled() ? "ON" : "OFF",
        m_clsIbcf.IsEnabled()  ? "ON" : "OFF");
}

bool CModuleDispatcher::Start(CSipStackSetup& clsSetup) {
    gclsSipServerMap.SetSipUserAgentRegisterInfo();

    // UserAgent 시작 (내부적으로 CSipStack 시작 + UserAgent 를 콜백 등록)
    if (gclsUserAgent.Start(clsSetup, this, this) == false) return false;

    // 콜백 순서를 [ModuleDispatcher, CSipUserAgent] 로 재배치
    // → Proxy INVITE 를 ModuleDispatcher 가 먼저 가로챌 수 있음
    gclsUserAgent.m_clsSipStack.DeleteCallBack(&gclsUserAgent);
    gclsUserAgent.m_clsSipStack.AddCallBack(this);
    gclsUserAgent.m_clsSipStack.AddCallBack(&gclsUserAgent);

    InitModules();
    return true;
}

// ──────────────────────────────────────────────────────────────
//  Call ownership tracking
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::SetCallOwner(const char* pszCallId, IModule* pModule) {
    m_clsOwnerMutex.acquire();
    m_mapCallOwner[pszCallId] = pModule;
    m_clsOwnerMutex.release();
}

IModule* CModuleDispatcher::GetCallOwner(const char* pszCallId) {
    IModule* pOwner = NULL;
    m_clsOwnerMutex.acquire();
    auto it = m_mapCallOwner.find(pszCallId);
    if (it != m_mapCallOwner.end()) pOwner = it->second;
    m_clsOwnerMutex.release();
    return pOwner;
}

void CModuleDispatcher::RemoveCallOwner(const char* pszCallId) {
    m_clsOwnerMutex.acquire();
    m_mapCallOwner.erase(pszCallId);
    m_clsOwnerMutex.release();
}

// ──────────────────────────────────────────────────────────────
//  Proxy call tracking
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::SetProxyCall(const std::string& strCallId, const ProxyCallInfo& info) {
    m_clsProxyMutex.acquire();
    m_mapProxyCall[strCallId] = info;
    m_clsProxyMutex.release();
}

bool CModuleDispatcher::GetProxyCall(const std::string& strCallId, ProxyCallInfo& info) {
    bool bFound = false;
    m_clsProxyMutex.acquire();
    auto it = m_mapProxyCall.find(strCallId);
    if (it != m_mapProxyCall.end()) { info = it->second; bFound = true; }
    m_clsProxyMutex.release();
    return bFound;
}

void CModuleDispatcher::RemoveProxyCall(const std::string& strCallId) {
    m_clsProxyMutex.acquire();
    m_mapProxyCall.erase(strCallId);
    m_clsProxyMutex.release();
}

// ──────────────────────────────────────────────────────────────
//  Shared helpers
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::SendResponse(CSipMessage* pclsMessage, int iStatusCode) {
    CSipMessage* pclsResponse = pclsMessage->CreateResponseWithToTag(iStatusCode);
    if (pclsResponse == NULL) return false;

    gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
    return true;
}

void CModuleDispatcher::StopCall(const char* pszCallId, int iResponseCode) {
    CLog::Print(LOG_DEBUG, "StopCall: CallId=%s Code=%d", pszCallId, iResponseCode);
    SaveCdr(pszCallId, iResponseCode);
    gclsUserAgent.StopCall(pszCallId, iResponseCode);
}

void CModuleDispatcher::SaveCdr(const char* pszCallId, int iSipStatus) {
    if (gclsSetup.m_strCdrFolder.empty()) return;

    CSipCdr clsCdr;
    if (gclsUserAgent.GetCdr(pszCallId, &clsCdr)) {
        char szInviteTime[15], szStartTime[15], szEndTime[15];
        if (clsCdr.m_sttInviteTime.tv_sec) GetDateTimeString(clsCdr.m_sttInviteTime.tv_sec, szInviteTime, sizeof(szInviteTime));
        else szInviteTime[0] = '\0';
        if (clsCdr.m_sttStartTime.tv_sec) GetDateTimeString(clsCdr.m_sttStartTime.tv_sec, szStartTime, sizeof(szStartTime));
        else szStartTime[0] = '\0';
        if (clsCdr.m_sttEndTime.tv_sec) GetDateTimeString(clsCdr.m_sttEndTime.tv_sec, szEndTime, sizeof(szEndTime));
        else GetDateTimeString(szEndTime, sizeof(szEndTime));

        std::string strFileName = gclsSetup.m_strCdrFolder;
        char szFileName[20];
        GetDateString(szFileName, sizeof(szFileName));
        strcat(szFileName, ".csv");
        CDirectory::AppendName(strFileName, szFileName);

        m_clsMutex.acquire();
        FILE* fd = fopen(strFileName.c_str(), "a");
        if (fd) {
            fprintf(fd, "%s,%s,%s,%s,%s,%d,%s\n", clsCdr.m_strFromId.c_str(), clsCdr.m_strToId.c_str(),
                    szInviteTime, szStartTime, szEndTime, iSipStatus, clsCdr.m_strCallId.c_str());
            fclose(fd);
        }
        m_clsMutex.release();

        if (gclsDbManager.IsConnected()) {
            time_t tAnswer = clsCdr.m_sttStartTime.tv_sec;
            time_t tEnd = clsCdr.m_sttEndTime.tv_sec ? clsCdr.m_sttEndTime.tv_sec : time(nullptr);
            gclsDbManager.UpdateCallLogEnded(clsCdr.m_strCallId, tAnswer, tEnd, iSipStatus);
        }
        if (gclsCallDir.IsEnabled()) {
            int dur = (int)(clsCdr.m_sttEndTime.tv_sec - clsCdr.m_sttStartTime.tv_sec);
            gclsCallDir.VoipCallEnd(clsCdr.m_strCallId, "normal", dur > 0 ? dur : 0);
        }
    }
}

// (Proxy 모드 제거됨 — 모든 VoIP INVITE는 B2BUA + CMP 경유)

// ──────────────────────────────────────────────────────────────
//  ISipStackCallBack — RecvRequest
//  순서: ModuleDispatcher (1st) → CSipUserAgent (2nd)
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::RecvRequest(int iThreadId, CSipMessage* pclsMessage) {
    std::string strCallId;
    pclsMessage->GetCallId(strCallId);

    // REGISTER, SUBSCRIBE → CSCF 모듈
    if (m_clsCscf.IsEnabled() && m_clsCscf.OnSipRequest(iThreadId, pclsMessage)) {
        return true;
    }

    // INVITE → Proxy 가능 여부 판단
    if (pclsMessage->IsMethod(SIP_METHOD_INVITE)) {
        std::string strTo = pclsMessage->m_clsTo.m_clsUri.m_strUser;
        std::string strFrom = pclsMessage->m_clsFrom.m_clsUri.m_strUser;

        // PTT 그룹 → B2BUA (return false → UserAgent 처리)
        if (gclsGroupMap.Contains(strTo.c_str())) {
            return false;
        }

        // 트렁크 라우팅 → B2BUA
        CspSipServer clsSipServer;
        std::string strRouteTo;
        if (gclsSipServerMap.SelectRoutePrefix(strTo.c_str(), clsSipServer, strRouteTo)) {
            return false;
        }

        // TAS 서비스 판단: DND, 착신거부
        CspUser clsToUser;
        if (gclsCspUserMap.isAlive(strTo.c_str(), clsToUser)) {
            if (clsToUser.isDnd() || clsToUser.isReject(strFrom.c_str())) {
                CLog::Print(LOG_INFO, "CSCF: Rejected by TAS (DND/Reject) From=%s To=%s", strFrom.c_str(), strTo.c_str());
                SendResponse(pclsMessage, SIP_DECLINE);
                return true;
            }

            // 서비스 모드 체크
            const std::string& mode = gclsSetup.m_strServiceMode;
            if (mode == "ptt") {
                SendResponse(pclsMessage, SIP_FORBIDDEN);
                return true;
            }
        }

        // 모든 VoIP INVITE → B2BUA (CMP 경유)
        return false;
    }

    return false;
}

bool CModuleDispatcher::RecvResponse(int iThreadId, CSipMessage* pclsMessage) {
    // 메시지 로깅
    return false;
}

bool CModuleDispatcher::SendTimeout(int iThreadId, CSipMessage* pclsMessage) {
    return false;
}

// ──────────────────────────────────────────────────────────────
//  ISipStackSecurityCallBack
// ──────────────────────────────────────────────────────────────

bool CModuleDispatcher::IsAllowUserAgent(const char* pszSipUserAgent) {
    return gclsSetup.IsAllowUserAgent(pszSipUserAgent);
}

bool CModuleDispatcher::IsDenyUserAgent(const char* pszSipUserAgent) {
    return gclsSetup.IsDenyUserAgent(pszSipUserAgent);
}

bool CModuleDispatcher::IsAllowIp(const char* pszIp) { return true; }
bool CModuleDispatcher::IsDenyIp(const char* pszIp) { return false; }

// ──────────────────────────────────────────────────────────────
//  ISipUserAgentCallBack — B2BUA 이벤트
//  (CSipUserAgent 가 return false 된 INVITE 를 B2BUA 처리 후 호출)
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::EventRegister(CSipServerInfo* pclsInfo, int iStatus) {
    gclsSipServerMap.Set(pclsInfo, iStatus);
}

bool CModuleDispatcher::EventIncomingRequestAuth(CSipMessage* pclsMessage) {
    std::string strIp;
    int iPort;
    CUserInfo clsUserInfo;

    if (pclsMessage->GetTopViaIpPort(strIp, iPort) == false) {
        CLog::Print(LOG_ERROR, "EventIncomingRequestAuth - GetTopViaIpPort error");
        SendResponse(pclsMessage, SIP_BAD_REQUEST);
        return false;
    }

    if (gclsSipServerMap.Select(strIp.c_str(), pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str())) {
        return true;
    }

    CspUser clsCspUser;
    bool bCspUserFound = gclsCspUserMap.Select(pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsCspUser);

    if (gclsUserMap.Select(pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsUserInfo) == false && !bCspUserFound) {
        std::string strDestToId;
        if (pclsMessage->IsMethod(SIP_METHOD_BYE)) {
            std::string strCallId;
            pclsMessage->GetCallId(strCallId);
            if (gclsCallMap.Select(strCallId.c_str())) return true;
        } else if (gclsSipServerMap.SelectIncomingRoute(strIp.c_str(), pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str(), strDestToId)) {
            return true;
        }

        if (CCscfModule::CheckAuthrization(pclsMessage) == false) return false;

        if (gclsUserMap.Select(pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsUserInfo) == false &&
            !gclsCspUserMap.Select(pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), clsCspUser)) {
            return false;
        }
    }

    if (strcmp(clsUserInfo.m_strIp.c_str(), strIp.c_str()) || clsUserInfo.m_iPort != iPort) {
        if (CCscfModule::CheckAuthrization(pclsMessage) == false) return false;
        gclsUserMap.SetIpPort(pclsMessage->m_clsFrom.m_clsUri.m_strUser.c_str(), strIp.c_str(), iPort);
    }

    return true;
}

void CModuleDispatcher::EventIncomingCall(const char* pszCallId, const char* pszFrom, const char* pszTo,
                                           CSipCallRtp* pclsRtp, CSipMessage* pclsMessage) {
    CLog::Print(LOG_DEBUG, "EventIncomingCall: CallId=%s From=%s To=%s", pszCallId, pszFrom, pszTo);
    CspUser clsUser;
    CUserInfo clsUserInfo;
    bool bRoutePrefix = false;
    std::string strTo;

    if (strlen(pszTo) == 0) return StopCall(pszCallId, SIP_DECLINE);

    // 1. PTT-AS: 그룹콜
    if (m_clsPttAs.IsEnabled() && gclsGroupMap.Contains(pszTo)) {
        CLog::Print(LOG_INFO, "EventIncomingCall: PTT terminal(%s) INVITE to group(%s) - rejected 403 [PTT-AS]", pszFrom, pszTo);
        SetCallOwner(pszCallId, &m_clsPttAs);
        return StopCall(pszCallId, SIP_FORBIDDEN);
    }

    // 서비스 모드 체크
    {
        CspUser clsFromUser;
        bool bFromKnown = gclsCspUserMap.isAlive(pszFrom, clsFromUser);
        const std::string& mode = gclsSetup.m_strServiceMode;
        if (mode == "ptt") return StopCall(pszCallId, SIP_FORBIDDEN);
        if (bFromKnown && !clsFromUser.m_strServiceType.empty() && clsFromUser.m_strServiceType == "ptt")
            return StopCall(pszCallId, SIP_FORBIDDEN);
    }

    if (gclsCspUserMap.isAlive(pszTo, clsUser) == false) {
        CspPttGroup clsGroup;
        if (m_clsPttAs.IsEnabled() && gclsGroupMap.Select(pszTo, clsGroup)) {
            CSipCallRoute clsRouteTemp;
            clsUserInfo.GetCallRoute(clsRouteTemp);
            if (gclsGroupCallService.ProcessGroupCall(pszTo, pszFrom, pszCallId, pclsRtp, &clsRouteTemp)) {
                SetCallOwner(pszCallId, &m_clsPttAs);
                return;
            }
        }

        // IBCF: 트렁크
        CspSipServer clsSipServer;
        if (m_clsIbcf.IsEnabled() && gclsSipServerMap.SelectRoutePrefix(pszTo, clsSipServer, strTo)) {
            clsUser.m_strId = clsSipServer.m_strUserId;
            clsUser.m_strPassWord = clsSipServer.m_strPassWord;
            clsUserInfo.m_strIp = clsSipServer.m_strIp;
            clsUserInfo.m_iPort = clsSipServer.m_iPort;
            clsUserInfo.m_eTransport = E_SIP_UDP;
            pszFrom = clsUser.m_strId.c_str();
            pszTo = strTo.c_str();
            bRoutePrefix = true;
            SetCallOwner(pszCallId, &m_clsIbcf);
        } else if (m_clsIbcf.IsEnabled() && gclsSipServerMap.SelectIncomingRoute(NULL, pszTo, strTo)) {
            if (gclsCspUserMap.isAlive(strTo, clsUser) == false) return StopCall(pszCallId, SIP_NOT_FOUND);
            SetCallOwner(pszCallId, &m_clsIbcf);
        } else if (gclsSetup.IsCallPickupId(pszTo)) {
            SetCallOwner(pszCallId, &m_clsTas);
            return PickUp(pszCallId, pszFrom, pszTo, pclsRtp);
        } else {
            return StopCall(pszCallId, SIP_NOT_FOUND);
        }
    }

    if (GetCallOwner(pszCallId) == NULL) SetCallOwner(pszCallId, &m_clsTas);

    // TAS: DND/착신전환/착신거부
    if (clsUser.isDnd() || clsUser.isReject(pszFrom)) return StopCall(pszCallId, SIP_DECLINE);

    if (clsUser.isCallForward()) {
        CSipMessage* pclsInvite = gclsUserAgent.DeleteIncomingCall(pszCallId);
        if (pclsInvite) {
            CSipMessage* pclsResponse = pclsInvite->CreateResponseWithToTag(SIP_MOVED_TEMPORARILY);
            if (pclsResponse) {
                CSipFrom clsContact;
                clsContact.m_clsUri.m_strProtocol = SIP_PROTOCOL;
                clsContact.m_clsUri.m_strUser = clsUser.m_strForward;
                clsContact.m_clsUri.m_strHost = gclsSetup.m_strLocalIp;
                clsContact.m_clsUri.m_iPort = gclsSetup.m_iUdpPort;
                pclsResponse->m_clsContactList.push_back(clsContact);
                gclsUserAgent.m_clsSipStack.SendSipMessage(pclsResponse);
                return;
            }
        }
        return StopCall(pszCallId, SIP_MOVED_TEMPORARILY);
    }

    // B2BUA 호 설정
    if (bRoutePrefix == false) {
        if (gclsUserMap.Select(pszTo, clsUserInfo) == false) return StopCall(pszCallId, SIP_NOT_FOUND);
    }

    int iStartPort = -1;
    std::string strCallId;
    CSipCallRoute clsRoute;

    // B2BUA: Session-ID 생성 + 발신 leg 매핑
    std::string strSessionId;
    if (gclsCallDir.IsEnabled()) {
        strSessionId = CCallDir::GenerateSessionId();
        gclsCallDir.MapCallToSession(pszCallId, strSessionId);
        // Session-ID로 디렉터리 생성
        gclsCallDir.GetVoipDir(pszCallId, pszFrom, pszTo);
    }

    if (gclsSetup.m_bUseRtpRelay) {
        // 녹취 경로: Recording 활성화 시 세션 디렉터리 사용
        std::string strRecordDir, strLogDir;
        if (gclsSetup.m_bRecordEnable && gclsCallDir.IsEnabled()) {
            strRecordDir = gclsCallDir.GetVoipDir(pszCallId, pszFrom, pszTo);
            strLogDir = strRecordDir;
        }
        // 발신측 RTP 주소를 ADD_SESSION에 포함 (생성 + peer[0] 한번에)
        int iAudioPort = pclsRtp->GetAudioPort();
        if (iAudioPort <= 0 && pclsRtp->m_iPort > 0) iAudioPort = pclsRtp->m_iPort;
        int iVideoPort = (pclsRtp->GetMediaCount() >= 2) ? pclsRtp->GetVideoPort() : 0;

        iStartPort = gclsRtpMap.CreatePort(SOCKET_COUNT_PER_MEDIA * pclsRtp->GetMediaCount(), strRecordDir, strLogDir,
                                          pszFrom ? pszFrom : "", pszTo ? pszTo : "",
                                          pclsRtp->m_strIp, iAudioPort, iVideoPort);
        if (iStartPort == -1) return StopCall(pszCallId, SIP_INTERNAL_SERVER_ERROR);

        std::string strRelayIp = gclsSetup.m_strLocalIp;
        std::string strAllocatedIp;
        if (gclsRtpMap.GetLocalIp(iStartPort, strAllocatedIp) && !strAllocatedIp.empty()) strRelayIp = strAllocatedIp;
        pclsRtp->SetIpPort(strRelayIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA);

    }

    clsUserInfo.GetCallRoute(clsRoute);
    clsRoute.m_b100rel = gclsUserAgent.Is100rel(pszCallId);

    CSipMessage* pclsInvite;
    if (gclsUserAgent.CreateCall(pszFrom, pszTo, pclsRtp, &clsRoute, strCallId, &pclsInvite) == false)
        return StopCall(pszCallId, SIP_INTERNAL_SERVER_ERROR);

    gclsCallMap.Insert(pszCallId, strCallId.c_str(), iStartPort);
    SetCallOwner(strCallId.c_str(), GetCallOwner(pszCallId));

    // B2BUA: 착신 leg도 같은 Session-ID에 매핑
    if (gclsCallDir.IsEnabled() && !strSessionId.empty()) {
        gclsCallDir.MapCallToSession(strCallId, strSessionId);
        gclsCallDir.WriteSessionMapping(strSessionId, pszCallId, strCallId);
    }

    if (gclsUserAgent.StartCall(strCallId.c_str(), pclsInvite) == false) {
        gclsCallMap.Delete(pszCallId);
        return StopCall(pszCallId, SIP_INTERNAL_SERVER_ERROR);
    }

    if (gclsDbManager.IsConnected()) {
        gclsDbManager.InsertCallLog(pszCallId, false, "", pszFrom, pszTo);
        gclsDbManager.InsertParticipant(pszCallId, pszFrom, "caller", true);
        gclsDbManager.InsertParticipant(pszCallId, pszTo, "callee", false);
    }
    if (gclsCallDir.IsEnabled()) {
        bool bVideo = (pclsRtp->GetMediaCount() >= 2 && pclsRtp->GetVideoPort() > 0);
        gclsCallDir.VoipCallStart(pszCallId, pszFrom, pszTo, bVideo);
        gclsCallDir.VoipAddParticipant(pszCallId, pszFrom, "caller");
        gclsCallDir.VoipAddParticipant(pszCallId, pszTo, "callee");
    }
}

void CModuleDispatcher::EventCallRing(const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRtp) {
    CCallInfo clsCallInfo;
    CLog::Print(LOG_DEBUG, "EventCallRing(%s,%d)", pszCallId, iSipStatus);

    if (gclsCallMap.Select(pszCallId, clsCallInfo)) {
        if (pclsRtp && clsCallInfo.m_iPeerRtpPort > 0) {
            pclsRtp->m_iPort = clsCallInfo.m_iPeerRtpPort;
            pclsRtp->m_strIp = gclsSetup.m_strLocalIp;
        }
        int iRSeq = gclsUserAgent.GetRSeq(pszCallId);
        if (iRSeq != -1) gclsUserAgent.SetRSeq(clsCallInfo.m_strPeerCallId.c_str(), iRSeq);
        gclsUserAgent.RingCall(clsCallInfo.m_strPeerCallId.c_str(), iSipStatus, pclsRtp);
    } else if (gclsTransCallMap.Select(pszCallId, clsCallInfo)) {
        gclsUserAgent.SendNotify(clsCallInfo.m_strPeerCallId.c_str(), iSipStatus);
    }
}

void CModuleDispatcher::EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) {
    CCallInfo clsCallInfo;
    CLog::Print(LOG_DEBUG, "EventCallStart(%s)", pszCallId);

    if (gclsCallMap.Select(pszCallId, clsCallInfo)) {
        // Service log: VoipCallAnswer
        if (gclsCallDir.IsEnabled()) {
            std::string strOrigCallId = pszCallId;
            if (!gclsCallDir.GetSessionId(pszCallId).empty()) strOrigCallId = pszCallId;
            else if (!clsCallInfo.m_strPeerCallId.empty() && !gclsCallDir.GetSessionId(clsCallInfo.m_strPeerCallId).empty())
                strOrigCallId = clsCallInfo.m_strPeerCallId;
            else if (!clsCallInfo.m_strPeerCallId.empty()) strOrigCallId = clsCallInfo.m_strPeerCallId;
            std::string gid = gclsGroupCallService.GetGroupIdByCallId(pszCallId);
            if (gid.empty()) {
                gclsCallDir.VoipCallAnswer(strOrigCallId);
            }
        }
        if (pclsRtp && clsCallInfo.m_iPeerRtpPort > 0) {
            int iRtpPort = clsCallInfo.m_iPeerRtpPort;
            std::string strAllocatedIp;
            if (!gclsRtpMap.GetLocalIp(iRtpPort, strAllocatedIp)) {
                if (gclsRtpMap.GetLocalIp(iRtpPort - 2, strAllocatedIp)) iRtpPort = iRtpPort - 2;
            }
            if (!strAllocatedIp.empty()) {
                if (pclsRtp->GetMediaCount() >= 2) {
                    int iVideoPort = pclsRtp->GetVideoPort();
                    if (iVideoPort > 0) gclsRtpMap.SetIpPort(iRtpPort, 2, inet_addr(pclsRtp->m_strIp.c_str()), iVideoPort, 1);
                }
                int iAudioPort = pclsRtp->GetAudioPort();
                if (iAudioPort <= 0 && pclsRtp->m_iPort > 0) iAudioPort = pclsRtp->m_iPort;
                if (iAudioPort > 0) gclsRtpMap.SetIpPort(iRtpPort, 0, inet_addr(pclsRtp->m_strIp.c_str()), iAudioPort, 1);
            }

            int iRemoteAudio = pclsRtp->GetAudioPort();
            if (iRemoteAudio <= 0 && pclsRtp->m_iPort > 0) iRemoteAudio = pclsRtp->m_iPort;
            if (iRemoteAudio > 0) {
                int iRemoteVideo = pclsRtp->GetVideoPort();
                gclsGroupCallService.OnCallStarted(pszCallId, pclsRtp->m_strIp, iRemoteAudio, 0, iRemoteVideo);
            }

            std::string strRelayIp = gclsSetup.m_strLocalIp;
            if (!strAllocatedIp.empty()) strRelayIp = strAllocatedIp;
            pclsRtp->SetIpPort(strRelayIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA);
        } else if (clsCallInfo.m_iPeerRtpPort > 0) {
            gclsGroupCallService.OnCallStarted(pszCallId, gclsSetup.m_strLocalIp, clsCallInfo.m_iPeerRtpPort);
        }

        if (gclsUserAgent.IsConnected(clsCallInfo.m_strPeerCallId.c_str())) {
            gclsUserAgent.SendReInvite(clsCallInfo.m_strPeerCallId.c_str(), pclsRtp);
        } else {
            gclsUserAgent.AcceptCall(clsCallInfo.m_strPeerCallId.c_str(), pclsRtp);
        }
    } else if (gclsTransCallMap.Select(pszCallId, clsCallInfo)) {
        gclsUserAgent.SendNotify(clsCallInfo.m_strPeerCallId.c_str(), SIP_OK);
        std::string strReferToCallId;
        int iStartPort = -1;
        if (gclsCallMap.Select(clsCallInfo.m_strPeerCallId.c_str(), strReferToCallId)) {
            gclsUserAgent.StopCall(clsCallInfo.m_strPeerCallId.c_str());
            gclsCallMap.Delete(clsCallInfo.m_strPeerCallId.c_str());
        }
        if (pclsRtp && clsCallInfo.m_iPeerRtpPort > 0) {
            iStartPort = clsCallInfo.m_iPeerRtpPort - 2;
            pclsRtp->SetIpPort(gclsSetup.m_strLocalIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA);
        }
        gclsUserAgent.SendReInvite(strReferToCallId.c_str(), pclsRtp);
        gclsCallMap.Insert(strReferToCallId.c_str(), pszCallId, iStartPort);
        gclsTransCallMap.Delete(pszCallId, false);
    } else {
        gclsUserAgent.StopCall(pszCallId);
    }
}

void CModuleDispatcher::EventCallEnd(const char* pszCallId, int iSipStatus) {
    CCallInfo clsCallInfo;
    CLog::Print(LOG_DEBUG, "EventCallEnd(%s:%d)", pszCallId, iSipStatus);

    if (gclsCallMap.Select(pszCallId, clsCallInfo)) {
        // Service log: VoipCallEnd
        if (gclsCallDir.IsEnabled()) {
            std::string strOrigCallId = pszCallId;
            if (!gclsCallDir.GetSessionId(pszCallId).empty()) strOrigCallId = pszCallId;
            else if (!clsCallInfo.m_strPeerCallId.empty() && !gclsCallDir.GetSessionId(clsCallInfo.m_strPeerCallId).empty())
                strOrigCallId = clsCallInfo.m_strPeerCallId;
            else if (!clsCallInfo.m_strPeerCallId.empty()) strOrigCallId = clsCallInfo.m_strPeerCallId;
            std::string gid = gclsGroupCallService.GetGroupIdByCallId(pszCallId);
            if (gid.empty()) {
                gclsCallDir.VoipCallEnd(strOrigCallId, iSipStatus == 200 ? "normal" : "error", 0);
            }
        }
        if (clsCallInfo.m_bRecv) SaveCdr(pszCallId, iSipStatus);
        else SaveCdr(clsCallInfo.m_strPeerCallId.c_str(), iSipStatus);

        // CMP 리소스 해제 → BYE 순서 (리소스 먼저 해제 후 SIP 종료)
        bool bIsGroup = gclsGroupCallService.OnCallTerminated(pszCallId);
        gclsCallMap.Delete(pszCallId, !bIsGroup);
        gclsUserAgent.StopCall(clsCallInfo.m_strPeerCallId.c_str());

        RemoveCallOwner(pszCallId);
        RemoveCallOwner(clsCallInfo.m_strPeerCallId.c_str());
    } else {
        std::string strCallId;
        if (gclsTransCallMap.Select(pszCallId, strCallId)) {
            gclsUserAgent.SendNotify(strCallId.c_str(), iSipStatus);
            gclsTransCallMap.Delete(pszCallId);
        }
        RemoveCallOwner(pszCallId);
    }
}

void CModuleDispatcher::EventReInvite(const char* pszCallId, CSipCallRtp* pclsRemoteRtp, CSipCallRtp* pclsLocalRtp) {
    CCallInfo clsCallInfo;
    if (gclsCallMap.Select(pszCallId, clsCallInfo)) {
        if (pclsRemoteRtp && clsCallInfo.m_iPeerRtpPort > 0) {
            pclsRemoteRtp->SetIpPort(gclsSetup.m_strLocalIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA);
        }
        gclsUserAgent.SendReInvite(clsCallInfo.m_strPeerCallId.c_str(), pclsRemoteRtp);
    }
}

void CModuleDispatcher::EventPrack(const char* pszCallId, CSipCallRtp* pclsRtp) {
    CCallInfo clsCallInfo;
    if (gclsCallMap.Select(pszCallId, clsCallInfo)) {
        if (pclsRtp && clsCallInfo.m_iPeerRtpPort > 0) {
            pclsRtp->SetIpPort(gclsSetup.m_strLocalIp.c_str(), clsCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA);
        }
        gclsUserAgent.SendPrack(clsCallInfo.m_strPeerCallId.c_str(), pclsRtp);
    }
}

bool CModuleDispatcher::EventTransfer(const char* pszCallId, const char* pszReferToCallId, bool bScreenedTransfer) {
    CCallInfo clsCallInfo, clsReferToCallInfo;
    CSipCallRtp clsRtp;

    if (gclsCallMap.Select(pszCallId, clsCallInfo) == false) return false;
    if (gclsCallMap.Select(pszReferToCallId, clsReferToCallInfo) == false) return false;

    gclsCallMap.Delete(pszCallId);
    gclsCallMap.Delete(pszReferToCallId, false);

    if (gclsUserAgent.GetRemoteCallRtp(clsCallInfo.m_strPeerCallId.c_str(), &clsRtp) == false) return false;
    clsRtp.SetDirection(E_RTP_SEND_RECV);

    if (gclsSetup.m_bUseRtpRelay) {
        if (bScreenedTransfer) clsRtp.SetIpPort(gclsSetup.m_strLocalIp.c_str(), clsReferToCallInfo.m_iPeerRtpPort, SOCKET_COUNT_PER_MEDIA);
        else clsRtp.SetIpPort(gclsSetup.m_strLocalIp.c_str(), clsReferToCallInfo.m_iPeerRtpPort + 2, SOCKET_COUNT_PER_MEDIA);
    }

    if (bScreenedTransfer) {
        CSipCallRtp clsReferToRtp;
        if (gclsUserAgent.GetRemoteCallRtp(clsReferToCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp) == false) return false;
        clsReferToRtp.SetDirection(E_RTP_SEND_RECV);
        if (gclsSetup.m_bUseRtpRelay)
            clsReferToRtp.SetIpPort(gclsSetup.m_strLocalIp.c_str(), clsReferToCallInfo.m_iPeerRtpPort + 2, SOCKET_COUNT_PER_MEDIA);
        gclsCallMap.Insert(clsCallInfo.m_strPeerCallId.c_str(), clsReferToCallInfo.m_strPeerCallId.c_str(), clsReferToCallInfo.m_iPeerRtpPort);
        gclsUserAgent.SendReInvite(clsCallInfo.m_strPeerCallId.c_str(), &clsReferToRtp);
        gclsUserAgent.SendReInvite(clsReferToCallInfo.m_strPeerCallId.c_str(), &clsRtp);
    }

    gclsUserAgent.StopCall(pszCallId);
    gclsUserAgent.StopCall(pszReferToCallId, SIP_REQUEST_TERMINATED);

    if (bScreenedTransfer == false) {
        std::string strNewCallId, strFromId, strToId;
        CUserInfo clsUserInfo;
        CSipCallRoute clsRoute;
        gclsUserAgent.GetToId(clsCallInfo.m_strPeerCallId.c_str(), strFromId);
        gclsUserAgent.GetToId(clsReferToCallInfo.m_strPeerCallId.c_str(), strToId);
        if (gclsUserMap.Select(strToId.c_str(), clsUserInfo)) {
            clsUserInfo.GetCallRoute(clsRoute);
            gclsUserAgent.StopCall(clsReferToCallInfo.m_strPeerCallId.c_str());
            gclsUserAgent.StartCall(strFromId.c_str(), strToId.c_str(), &clsRtp, &clsRoute, strNewCallId);
            gclsCallMap.Insert(strNewCallId.c_str(), clsCallInfo.m_strPeerCallId.c_str(), clsReferToCallInfo.m_iPeerRtpPort);
        } else {
            gclsUserAgent.StopCall(clsCallInfo.m_strPeerCallId.c_str());
            gclsUserAgent.StopCall(clsReferToCallInfo.m_strPeerCallId.c_str());
        }
    }

    if (gclsSetup.m_bUseRtpRelay) gclsRtpMap.ReSetIpPort(clsReferToCallInfo.m_iPeerRtpPort);
    return true;
}

bool CModuleDispatcher::EventBlindTransfer(const char* pszCallId, const char* pszReferToId) {
    std::string strCallId, strInviteCallId, strToId;
    CSipCallRtp clsRtp;
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    int iStartPort = -1;

    if (gclsCallMap.Select(pszCallId, strCallId) == false) return false;
    if (gclsUserAgent.GetToId(strCallId.c_str(), strToId) == false) return false;
    if (gclsUserMap.Select(pszReferToId, clsUserInfo) == false) return false;
    if (gclsUserAgent.GetRemoteCallRtp(strCallId.c_str(), &clsRtp) == false) return false;
    clsRtp.SetDirection(E_RTP_SEND_RECV);

    if (gclsSetup.m_bUseRtpRelay) {
        iStartPort = gclsRtpMap.CreatePort(SOCKET_COUNT_PER_MEDIA * clsRtp.GetMediaCount());
        if (iStartPort == -1) return false;
        clsRtp.SetIpPort(gclsSetup.m_strLocalIp.c_str(), iStartPort, SOCKET_COUNT_PER_MEDIA);
    }

    clsUserInfo.GetCallRoute(clsRoute);
    if (gclsUserAgent.StartCall(strToId.c_str(), pszReferToId, &clsRtp, &clsRoute, strInviteCallId) == false) return false;
    gclsTransCallMap.Insert(pszCallId, strInviteCallId.c_str(), iStartPort);
    return true;
}

bool CModuleDispatcher::EventMessage(const char* pszFrom, const char* pszTo, CSipMessage* pclsMessage) {
    CUserInfo clsUserInfo;
    CSipCallRoute clsRoute;
    if (gclsUserMap.Select(pszTo, clsUserInfo) == false) return false;
    clsUserInfo.GetCallRoute(clsRoute);
    return gclsUserAgent.SendSms(pszFrom, pszTo, pclsMessage->m_strBody.c_str(), &clsRoute);
}

// ──────────────────────────────────────────────────────────────
//  PickUp (from SipServerPickUp.hpp)
// ──────────────────────────────────────────────────────────────

void CModuleDispatcher::PickUp(const char* pszCallId, const char* pszFrom, const char* pszTo, CSipCallRtp* pclsRtp) {
    CspUser xmlFrom;
    USER_ID_LIST clsUserIdList;
    bool bCallPickup = false;

    if (gclsCspUserMap.Select(pszFrom, xmlFrom) && gclsUserMap.SelectGroup(xmlFrom.m_strOrganizationId.c_str(), clsUserIdList)) {
        USER_ID_LIST::iterator itUIL;
        std::string strOldCallId;

        for (itUIL = clsUserIdList.begin(); itUIL != clsUserIdList.end(); ++itUIL) {
            if (gclsCallMap.SelectToRing(itUIL->c_str(), strOldCallId) == false) continue;

            CCallInfo clsOldCallInfo;
            if (gclsCallMap.Select(strOldCallId.c_str(), clsOldCallInfo) &&
                gclsCallMap.Insert(pszCallId, clsOldCallInfo)) {
                gclsCallMap.DeleteOne(strOldCallId.c_str());
                gclsUserAgent.StopCall(strOldCallId.c_str());

                CSipCallRtp clsRemoteRtp;
                if (gclsUserAgent.GetRemoteCallRtp(clsOldCallInfo.m_strPeerCallId.c_str(), &clsRemoteRtp)) {
                    if (pclsRtp) {
                        if (clsOldCallInfo.m_iPeerRtpPort > 0) {
                            pclsRtp->m_iPort = clsOldCallInfo.m_iPeerRtpPort;
                            pclsRtp->m_strIp = gclsSetup.m_strLocalIp;
                        }
                        pclsRtp->m_iCodec = clsRemoteRtp.m_iCodec;
                    }
                    if (gclsUserAgent.AcceptCall(clsOldCallInfo.m_strPeerCallId.c_str(), pclsRtp)) {
                        CCallInfo clsPeerCallInfo;
                        if (gclsCallMap.Select(clsOldCallInfo.m_strPeerCallId.c_str(), clsPeerCallInfo)) {
                            gclsCallMap.Update(clsOldCallInfo.m_strPeerCallId.c_str(), pszCallId);
                            if (pclsRtp) {
                                if (clsOldCallInfo.m_iPeerRtpPort > 0) pclsRtp->m_iPort = clsPeerCallInfo.m_iPeerRtpPort;
                                else pclsRtp = &clsRemoteRtp;
                            }
                            gclsUserAgent.AcceptCall(pszCallId, pclsRtp);
                            bCallPickup = true;
                        }
                        if (bCallPickup == false) gclsUserAgent.StopCall(clsOldCallInfo.m_strPeerCallId.c_str());
                    }
                }
            }
            break;
        }
    }

    if (bCallPickup == false) StopCall(pszCallId, SIP_NOT_FOUND);
}
