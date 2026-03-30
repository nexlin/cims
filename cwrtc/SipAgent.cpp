#include "SipAgent.h"
#include "SessionMap.h"
#include "CwrtcSetup.h"
#include "RtpThread.h"
#include "HttpCallBack.h"
#include "Log.h"
#include "SipStackSetup.h"
#include "SimpleJson.h"
#include "MemoryDebug.h"
#include <set>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>

CSipUserAgent gclsSipUserAgent;
CSipAgent     gclsSipAgent;

static std::set<int> gsUsedPorts;
static std::mutex    gsPortMutex;

// ── Start / Stop ───────────────────────────────────────────────────────────────

bool CSipAgent::Start()
{
    CSipStackSetup clsSetup;
    clsSetup.m_strLocalIp      = gclsCwrtcSetup.m_strLocalIp;
    clsSetup.m_iLocalUdpPort   = gclsCwrtcSetup.m_iSipLocalPort;
    clsSetup.m_iUdpThreadCount = 2;
    clsSetup.m_iLocalTcpPort   = 0;
    clsSetup.m_iLocalTlsPort   = 0;
    if (!gclsCwrtcSetup.m_strUserAgent.empty())
        clsSetup.m_strUserAgent = gclsCwrtcSetup.m_strUserAgent;

    if (!gclsSipUserAgent.Start(clsSetup, this)) {
        CLog::Print(LOG_ERROR, "CSipAgent: SipUserAgent.Start failed");
        return false;
    }
    CLog::Print(LOG_INFO, "CSipAgent: SIP UA started on port %d", gclsCwrtcSetup.m_iSipLocalPort);
    return true;
}

void CSipAgent::Stop()
{
    gclsSipUserAgent.Stop();
}

// ── Port allocation ────────────────────────────────────────────────────────────

bool CSipAgent::AllocPorts(int& iDtlsPort, int& iRtpPort)
{
    std::lock_guard<std::mutex> lock(gsPortMutex);
    int base  = gclsCwrtcSetup.m_iRtpPortBase;
    int count = gclsCwrtcSetup.m_iRtpPortCount;
    // 3포트 단위: dtls, rtp(=dtls+1), rtcp(=dtls+2)
    for (int i = 0; i < count; ++i) {
        int dtls = base + i * 3;
        int rtp  = dtls + 1;
        int rtcp = dtls + 2;
        if (!gsUsedPorts.count(dtls) && !gsUsedPorts.count(rtp) && !gsUsedPorts.count(rtcp)) {
            gsUsedPorts.insert(dtls);
            gsUsedPorts.insert(rtp);
            gsUsedPorts.insert(rtcp);
            iDtlsPort = dtls;
            iRtpPort  = rtp;
            return true;
        }
    }
    return false;
}

static void FreePorts(int d, int r)
{
    std::lock_guard<std::mutex> lock(gsPortMutex);
    gsUsedPorts.erase(d);
    gsUsedPorts.erase(r);
    gsUsedPorts.erase(r + 1);  // RTCP 포트도 해제
}

// ── SDP builders ──────────────────────────────────────────────────────────────

std::string CSipAgent::BuildDtlsSdp(int iDtlsPort, int iAudioPt, bool bVideoEnabled)
{
    char buf[2048];
    const char* ip  = gclsCwrtcSetup.m_strLocalIp.c_str();
    const char* fp  = gclsKeyCert.m_strFingerPrint.c_str();
    const char* pwd = "FNPRfT4qUaVOKa0ivkn64mMY";

    // 오디오 코덱 라인 (Opus PT=111 또는 AMR-WB PT=99)
    char szAudioCodec[128];
    if (iAudioPt == 111)
        snprintf(szAudioCodec, sizeof(szAudioCodec),
            "a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=20;useinbandfec=1\r\n");
    else
        snprintf(szAudioCodec, sizeof(szAudioCodec),
            "a=rtpmap:99 AMR-WB/16000\r\na=fmtp:99 octet-align=1\r\n");

    // 오디오 섹션
    char szAudio[768];
    snprintf(szAudio, sizeof(szAudio),
        "m=audio %d UDP/TLS/RTP/SAVPF %d\r\nc=IN IP4 %s\r\n"
        "%s"
        "a=sendrecv\r\na=ice-ufrag:lMRb\r\na=ice-pwd:%s\r\n"
        "a=fingerprint:sha-256 %s\r\na=setup:active\r\n"
        "a=candidate:1 1 udp 2130706431 %s %d typ host\r\na=rtcp-mux\r\n",
        iDtlsPort, iAudioPt, ip,
        szAudioCodec,
        pwd, fp, ip, iDtlsPort);

    // 비디오 섹션 (옵션)
    char szVideo[512] = "";
    if (bVideoEnabled) {
        snprintf(szVideo, sizeof(szVideo),
            "m=video %d UDP/TLS/RTP/SAVPF 97\r\nc=IN IP4 %s\r\n"
            "a=rtpmap:97 H264/90000\r\n"
            "a=fmtp:97 profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1\r\n"
            "a=sendrecv\r\na=ice-ufrag:lMRb\r\na=ice-pwd:%s\r\n"
            "a=fingerprint:sha-256 %s\r\na=setup:active\r\n"
            "a=candidate:1 1 udp 2130706431 %s %d typ host\r\na=rtcp-mux\r\n",
            iDtlsPort + 2, ip,
            pwd, fp, ip, iDtlsPort + 2);
    }

    snprintf(buf, sizeof(buf),
        "v=0\r\no=cwrtc 0 0 IN IP4 %s\r\ns=-\r\nt=0 0\r\n%s%s",
        ip, szAudio, szVideo);
    return buf;
}

std::string CSipAgent::BuildPlainRtpSdp(int iRtpPort, int iAudioPt)
{
    char buf[512];
    const char* ip = gclsCwrtcSetup.m_strLocalIp.c_str();
    if (iAudioPt == 111) {
        snprintf(buf, sizeof(buf),
            "v=0\r\no=cwrtc 0 0 IN IP4 %s\r\ns=-\r\nt=0 0\r\n"
            "m=audio %d RTP/AVP 111\r\nc=IN IP4 %s\r\n"
            "a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=20;useinbandfec=1\r\na=sendrecv\r\n",
            ip, iRtpPort, ip);
    } else {
        snprintf(buf, sizeof(buf),
            "v=0\r\no=cwrtc 0 0 IN IP4 %s\r\ns=-\r\nt=0 0\r\n"
            "m=audio %d RTP/AVP 99\r\nc=IN IP4 %s\r\n"
            "a=rtpmap:99 AMR-WB/16000\r\na=fmtp:99 octet-align=1\r\na=sendrecv\r\n",
            ip, iRtpPort, ip);
    }
    return buf;
}

// ── WS send helper ────────────────────────────────────────────────────────────

void CSipAgent::SendWsJson(const std::string& wsIp, int wsPort, const std::string& json)
{
    gclsHttpCallBack.SendText(wsIp.c_str(), wsPort, json.c_str());
}

// ── Registration ──────────────────────────────────────────────────────────────

bool CSipAgent::RegisterUser(const std::string& userId, const std::string& password,
                             const std::string& domain, const std::string& authId)
{
    CSipServerInfo info;
    info.m_strIp         = gclsCwrtcSetup.m_strSipIp;
    info.m_iPort         = gclsCwrtcSetup.m_iSipPort;
    info.m_strDomain     = domain.empty() ? gclsCwrtcSetup.m_strSipDomain : domain;
    info.m_strUserId     = userId;
    info.m_strPassWord   = password;
    info.m_strAuthId     = authId.empty() ? userId : authId;
    info.m_iLoginTimeout = 3600;

    // P-Preferred-Identity: sip:{userId}@{domain}
    {
        std::string strPPId = "<sip:" + userId + "@" + info.m_strDomain + ">";
        info.m_strPPreferredIdentity = strPPId;
    }

    // P-Access-Network-Info: 설정값 사용, 없으면 기본값
    info.m_strPAccessNetworkInfo = gclsCwrtcSetup.m_strPAccessNetworkInfo.empty()
        ? ""
        : gclsCwrtcSetup.m_strPAccessNetworkInfo;

    if (!gclsSipUserAgent.InsertRegisterInfo(info))
        gclsSipUserAgent.UpdateRegisterInfo(info);

    CLog::Print(LOG_INFO, "RegisterUser: %s@%s", userId.c_str(), info.m_strDomain.c_str());
    return true;
}

bool CSipAgent::UnregisterUser(const std::string& userId, const std::string& domain)
{
    CSipServerInfo info;
    info.m_strIp         = gclsCwrtcSetup.m_strSipIp;
    info.m_iPort         = gclsCwrtcSetup.m_iSipPort;
    info.m_strDomain     = domain.empty() ? gclsCwrtcSetup.m_strSipDomain : domain;
    info.m_strUserId     = userId;
    info.m_iLoginTimeout = 0;
    gclsSipUserAgent.DeleteRegisterInfo(info);
    return true;
}

// ── Outgoing call ─────────────────────────────────────────────────────────────

bool CSipAgent::StartOutgoingCall(const std::string& fromUser, const std::string& toUser,
                                  const std::string& browserSdp,
                                  const std::string& wsIp, int wsPort,
                                  std::string& strCallIdOut)
{
    int iDtlsPort, iRtpPort;
    if (!AllocPorts(iDtlsPort, iRtpPort)) {
        CLog::Print(LOG_ERROR, "StartOutgoingCall: no free ports");
        return false;
    }

    // RTP thread arg 미리 생성
    CRtpThreadArg* pArg = new CRtpThreadArg();
    if (!pArg->CreateSocket(iDtlsPort, iRtpPort)) {
        delete pArg; FreePorts(iDtlsPort, iRtpPort);
        return false;
    }
    pArg->m_strUserId     = fromUser;
    pArg->m_strBrowserSdp = browserSdp;
    pArg->m_strWsIp       = wsIp;
    pArg->m_iWsPort       = wsPort;

    // CSP로 보낼 평문 RTP SDP
    CSipCallRtp clsRtp;
    clsRtp.SetIpPort(gclsCwrtcSetup.m_strLocalIp.c_str(), iRtpPort, 1);
    clsRtp.m_iCodec = 0;
    clsRtp.m_clsCodecList = {0, 8};

    // Per-user domain stored at registration time (call → m_strSipDomain, ptt → m_strPttDomain)
    CWsClient cli;
    std::string strUserDomain = gclsCwrtcSetup.m_strSipDomain;
    if (gclsSessionMap.GetClientByUser(fromUser, cli) && !cli.strDomain.empty())
        strUserDomain = cli.strDomain;
    std::string strFrom = fromUser + "@" + strUserDomain;
    std::string strTo   = toUser   + "@" + strUserDomain;

    CSipCallRoute clsRoute;
    clsRoute.m_strDestIp  = gclsCwrtcSetup.m_strSipIp;
    clsRoute.m_iDestPort  = gclsCwrtcSetup.m_iSipPort;
    clsRoute.m_eTransport = E_SIP_UDP;

    std::string strCallId;
    if (!gclsSipUserAgent.StartCall(strFrom.c_str(), strTo.c_str(), &clsRtp, &clsRoute, strCallId)) {
        CLog::Print(LOG_ERROR, "StartOutgoingCall: StartCall failed");
        delete pArg; FreePorts(iDtlsPort, iRtpPort);
        return false;
    }

    pArg->m_strCallId = strCallId;

    CCallSession sess;
    sess.strCallId     = strCallId;
    sess.strUserId     = fromUser;
    sess.strWsIp       = wsIp;
    sess.iWsPort       = wsPort;
    sess.strPeerUserId = toUser;
    sess.bOutgoing     = true;
    sess.strBrowserSdp = browserSdp;
    sess.iDtlsPort     = iDtlsPort;
    sess.iRtpPort      = iRtpPort;
    sess.pclsRtpArg    = pArg;
    gclsSessionMap.InsertCall(sess);

    strCallIdOut = strCallId;
    CLog::Print(LOG_INFO, "StartOutgoingCall: %s→%s callId=%s dtls=%d rtp=%d",
        fromUser.c_str(), toUser.c_str(), strCallId.c_str(), iDtlsPort, iRtpPort);
    return true;
}

// ── Accept incoming ───────────────────────────────────────────────────────────

bool CSipAgent::AcceptIncomingCall(const std::string& callId, const std::string& browserSdp)
{
    CCallSession sess;
    if (!gclsSessionMap.GetCall(callId, sess)) return false;

    gclsSessionMap.UpdateCallBrowserSdp(callId, browserSdp);

    if (sess.bAutoAnswered) {
        // PTT Answer-Mode:Auto 통화: SIP 200 OK는 이미 전송됨.
        // 브라우저 SDP가 도착했으므로 RTP thread만 시작.
        if (sess.pclsRtpArg) {
            sess.pclsRtpArg->m_strBrowserSdp = browserSdp;
            StartRtpThread(sess.pclsRtpArg);
            CLog::Print(LOG_INFO, "AcceptIncomingCall: PTT RTP thread started for %s", callId.c_str());
        }

        // PTT 그룹 내 다른 멤버들에게 참석 알림
        {
            std::vector<CCallSession> groupSessions;
            gclsSessionMap.GetPttSessionsByGroup(sess.strPeerUserId, groupSessions);
            for (const auto& other : groupSessions) {
                if (other.strCallId == callId) continue;

                // 기존 멤버 → 새 멤버 참석 알림
                SimpleJson::JsonNode joinedOther;
                joinedOther.Set("type",    "ptt_member_joined");
                joinedOther.Set("call_id", other.strCallId);
                joinedOther.Set("user_id", sess.strUserId);
                SendWsJson(other.strWsIp, other.iWsPort, joinedOther.ToString());

                // 새 멤버 → 기존 멤버 목록 알림
                SimpleJson::JsonNode joinedSelf;
                joinedSelf.Set("type",    "ptt_member_joined");
                joinedSelf.Set("call_id", callId);
                joinedSelf.Set("user_id", other.strUserId);
                SendWsJson(sess.strWsIp, sess.iWsPort, joinedSelf.ToString());
            }
        }
        return true;
    }

    // 일반 착신 통화: RTP thread 시작 + 200 OK 전송
    if (sess.pclsRtpArg) {
        sess.pclsRtpArg->m_strBrowserSdp = browserSdp;
        StartRtpThread(sess.pclsRtpArg);
    }

    CSipCallRtp clsRtp;
    int iRtpPort = sess.pclsRtpArg ? sess.pclsRtpArg->m_iPbxUdpPort : 0;
    clsRtp.SetIpPort(gclsCwrtcSetup.m_strLocalIp.c_str(), iRtpPort, 1);
    clsRtp.m_iCodec = sess.iAudioPt;
    clsRtp.m_clsCodecList = {sess.iAudioPt, 0, 8};

    gclsSipUserAgent.AcceptCall(callId.c_str(), &clsRtp);
    CLog::Print(LOG_INFO, "AcceptIncomingCall: 200 OK for %s", callId.c_str());
    return true;
}

// ── Hangup ────────────────────────────────────────────────────────────────────

bool CSipAgent::HangupCall(const std::string& callId)
{
    CCallSession sess;
    if (gclsSessionMap.GetCall(callId, sess) && sess.pclsRtpArg)
        sess.pclsRtpArg->m_bStop = true;
    gclsSipUserAgent.StopCall(callId.c_str());
    return true;
}

// ── ISipUserAgentCallBack ─────────────────────────────────────────────────────

void CSipAgent::EventRegister(CSipServerInfo* pclsInfo, int iStatus)
{
    CLog::Print(LOG_INFO, "EventRegister: %s status=%d", pclsInfo->m_strUserId.c_str(), iStatus);
    CWsClient cli;
    if (!gclsSessionMap.GetClientByUser(pclsInfo->m_strUserId, cli)) return;

    bool bSuccess = (iStatus > 0);
    gclsSessionMap.SetClientSipRegistered(pclsInfo->m_strUserId, bSuccess);

    SimpleJson::JsonNode msg;
    if (bSuccess) {
        msg.Set("type", "registered");
        msg.Set("user", pclsInfo->m_strUserId);
    } else {
        msg.Set("type", "register_failed");
        msg.Set("reason", "auth");
    }
    SendWsJson(cli.strWsIp, cli.iWsPort, msg.ToString());
}

void CSipAgent::EventIncomingCall(const char* pszCallId, const char* pszFrom,
                                  const char* pszTo, CSipCallRtp* pclsRtp)
{
    // AOR에서 user 부분 추출
    auto strip = [](const char* s) -> std::string {
        std::string str = s;
        auto p = str.find('@');
        return p != std::string::npos ? str.substr(0, p) : str;
    };
    std::string strTo   = strip(pszTo);
    std::string strFrom = strip(pszFrom);

    CWsClient cli;
    if (!gclsSessionMap.GetClientByUser(strTo, cli)) {
        CLog::Print(LOG_INFO, "EventIncomingCall: no WS client for %s", strTo.c_str());
        gclsSipUserAgent.StopCall(pszCallId, SIP_NOT_FOUND);
        return;
    }

    std::string strCmpIp = pclsRtp ? pclsRtp->m_strIp : "";
    int iCmpPort         = pclsRtp ? pclsRtp->m_iPort : 0;
    // PT 99=AMR-WB PTT, PT 111=Opus PTT, PT 98=레거시 PTT
    // cwrtc는 브라우저와 항상 Opus(PT=111)로 통신 — AMR-WB는 브라우저 미지원
    bool bPtt     = (pclsRtp && (pclsRtp->m_iCodec == 99 || pclsRtp->m_iCodec == 111 || pclsRtp->m_iCodec == 98));
    int iAudioPt  = 111;  // 브라우저 WebRTC는 Opus만 지원

    int iDtlsPort, iRtpPort;
    if (!AllocPorts(iDtlsPort, iRtpPort)) {
        gclsSipUserAgent.StopCall(pszCallId, SIP_SERVICE_UNAVAILABLE);
        return;
    }

    CRtpThreadArg* pArg = new CRtpThreadArg();
    if (!pArg->CreateSocket(iDtlsPort, iRtpPort)) {
        delete pArg; FreePorts(iDtlsPort, iRtpPort);
        gclsSipUserAgent.StopCall(pszCallId, SIP_SERVICE_UNAVAILABLE);
        return;
    }
    pArg->m_strCallId = pszCallId;
    pArg->m_strUserId = strTo;
    pArg->m_strCmpIp  = strCmpIp;
    pArg->m_iCmpPort  = iCmpPort;
    pArg->m_strWsIp   = cli.strWsIp;
    pArg->m_iWsPort   = cli.iWsPort;

    CCallSession sess;
    sess.strCallId     = pszCallId;
    sess.strUserId     = strTo;
    sess.strWsIp       = cli.strWsIp;
    sess.iWsPort       = cli.iWsPort;
    sess.strPeerUserId = strFrom;
    sess.strCmpIp      = strCmpIp;
    sess.iCmpPort      = iCmpPort;
    sess.bPtt          = bPtt;
    sess.iAudioPt      = iAudioPt;
    sess.bVideoEnabled = false;  // TODO: 그룹 설정에서 전달받을 때 활성화
    sess.bOutgoing     = false;
    sess.iDtlsPort     = iDtlsPort;
    sess.iRtpPort      = iRtpPort;
    sess.pclsRtpArg    = pArg;
    gclsSessionMap.InsertCall(sess);

    // 180 Ringing (PTT 포함 모두 전송 — 3GPP 단말이 180 응답을 기대함)
    CSipCallRtp clsRingRtp;
    clsRingRtp.SetIpPort(gclsCwrtcSetup.m_strLocalIp.c_str(), iRtpPort, 1);
    clsRingRtp.m_iCodec = bPtt ? iAudioPt : 0;
    gclsSipUserAgent.RingCall(pszCallId, &clsRingRtp);

    std::string strDtlsSdp = BuildDtlsSdp(iDtlsPort, iAudioPt, sess.bVideoEnabled);
    SimpleJson::JsonNode msg;

    if (bPtt) {
        // Answer-Mode: Auto — CSP 측에 즉시 200 OK 전송
        // CSP→cwrtc SIP 구간은 CSP INVITE의 코덱(99=AMR-WB)으로 응답
        // cwrtc→브라우저는 별도로 Opus(111) 사용
        int iSipCodec = pclsRtp ? pclsRtp->m_iCodec : 99;
        CSipCallRtp clsAcceptRtp;
        clsAcceptRtp.SetIpPort(gclsCwrtcSetup.m_strLocalIp.c_str(), iRtpPort, 1);
        clsAcceptRtp.m_iCodec = iSipCodec;
        clsAcceptRtp.m_clsCodecList = {iSipCodec, 0, 8};
        gclsSipUserAgent.AcceptCall(pszCallId, &clsAcceptRtp);
        gclsSessionMap.SetCallAutoAnswered(pszCallId);  // 200 OK 전송 완료 마킹

        // 브라우저에 PTT 자동 참여 알림 (브라우저는 WebRTC 연결만 수립하면 됨)
        // group_id: scheme 제거 후 순수 그룹 번호 (예: "sip:1000" → "1000")
        std::string strGroupId = strFrom;
        if (strGroupId.substr(0, 4) == "sip:") strGroupId = strGroupId.substr(4);
        else if (strGroupId.substr(0, 4) == "tel:") strGroupId = strGroupId.substr(4);

        msg.Set("type",     "ptt_auto_answer");
        msg.Set("call_id",  pszCallId);
        msg.Set("from",     strFrom);
        msg.Set("group_id", strGroupId);
        msg.Set("sdp",      strDtlsSdp);

        CLog::Print(LOG_INFO, "EventIncomingCall: PTT Answer-Mode:Auto → 200 OK sent for %s", pszCallId);
    } else {
        // 일반 통화: 브라우저의 수락 대기
        msg.Set("type",    "incoming");
        msg.Set("call_id", pszCallId);
        msg.Set("from",    strFrom);
        msg.Set("sdp",     strDtlsSdp);
        msg.Set("ptt",     "false");
    }

    SendWsJson(cli.strWsIp, cli.iWsPort, msg.ToString());

    CLog::Print(LOG_INFO, "EventIncomingCall: %s→%s callId=%s dtls=%d rtp=%d cmp=%s:%d",
        strFrom.c_str(), strTo.c_str(), pszCallId,
        iDtlsPort, iRtpPort, strCmpIp.c_str(), iCmpPort);
}

void CSipAgent::EventCallRing(const char* pszCallId, int iSipStatus, CSipCallRtp*)
{
    CCallSession sess;
    if (!gclsSessionMap.GetCall(pszCallId, sess)) return;

    SimpleJson::JsonNode msg;
    msg.Set("type",    "progress");
    msg.Set("call_id", pszCallId);
    msg.Set("status",  std::to_string(iSipStatus));
    SendWsJson(sess.strWsIp, sess.iWsPort, msg.ToString());
}

void CSipAgent::EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp)
{
    CCallSession sess;
    if (!gclsSessionMap.GetCall(pszCallId, sess)) return;

    CLog::Print(LOG_INFO, "EventCallStart: callId=%s cmp=%s:%d outgoing=%d",
        pszCallId,
        pclsRtp ? pclsRtp->m_strIp.c_str() : "?",
        pclsRtp ? pclsRtp->m_iPort : 0,
        (int)sess.bOutgoing);

    if (sess.bOutgoing && pclsRtp && !pclsRtp->m_strIp.empty()) {
        gclsSessionMap.UpdateCallCmp(pszCallId, pclsRtp->m_strIp, pclsRtp->m_iPort);
        if (sess.pclsRtpArg) {
            sess.pclsRtpArg->m_strCmpIp = pclsRtp->m_strIp;
            sess.pclsRtpArg->m_iCmpPort = pclsRtp->m_iPort;
            StartRtpThread(sess.pclsRtpArg);
        }

        int iDtlsPort = sess.pclsRtpArg ? sess.pclsRtpArg->m_iWebRtcUdpPort : 0;
        SimpleJson::JsonNode msg;
        msg.Set("type",    "answered");
        msg.Set("call_id", pszCallId);
        msg.Set("sdp",     BuildDtlsSdp(iDtlsPort, sess.iAudioPt, sess.bVideoEnabled));
        SendWsJson(sess.strWsIp, sess.iWsPort, msg.ToString());
    }
}

void CSipAgent::EventCallEnd(const char* pszCallId, int iSipCode)
{
    CLog::Print(LOG_INFO, "EventCallEnd: callId=%s sip=%d", pszCallId, iSipCode);

    CCallSession sess;
    if (!gclsSessionMap.GetCall(pszCallId, sess)) return;

    if (sess.pclsRtpArg) {
        sess.pclsRtpArg->m_bStop = true;
        // RtpThread 종료 대기 후 삭제 (짧은 대기 — thread는 poll 1s timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        delete sess.pclsRtpArg;
        gclsSessionMap.UpdateCallRtpArg(pszCallId, nullptr);
    }

    // PTT: 그룹 내 다른 멤버들에게 이탈 알림 (DeleteCall 전에 조회)
    if (sess.bPtt) {
        std::vector<CCallSession> groupSessions;
        gclsSessionMap.GetPttSessionsByGroup(sess.strPeerUserId, groupSessions);
        for (const auto& other : groupSessions) {
            if (other.strCallId == pszCallId) continue;
            SimpleJson::JsonNode leftMsg;
            leftMsg.Set("type",    "ptt_member_left");
            leftMsg.Set("call_id", other.strCallId);
            leftMsg.Set("user_id", sess.strUserId);
            SendWsJson(other.strWsIp, other.iWsPort, leftMsg.ToString());
        }
    }

    SimpleJson::JsonNode msg;
    msg.Set("type",     "ended");
    msg.Set("call_id",  pszCallId);
    msg.Set("reason",   "normal");
    msg.Set("sip_code", std::to_string(iSipCode));
    SendWsJson(sess.strWsIp, sess.iWsPort, msg.ToString());

    // 할당된 포트 해제 (포트 누수 방지)
    if (sess.iDtlsPort > 0 && sess.iRtpPort > 0)
        FreePorts(sess.iDtlsPort, sess.iRtpPort);

    gclsSessionMap.DeleteCall(pszCallId);
}
