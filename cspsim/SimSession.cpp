#include "SimSession.h"
#include "SipUtility.h"
#include "SipMd5.h"
#include "SdpMedia.h"
#include "Log.h"
#include <sstream>
#include <chrono>
#include <cstring>

// ─────────────────────────────────────────────
//  유틸
// ─────────────────────────────────────────────
long long SimSession::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ─────────────────────────────────────────────
//  생성자 / 소멸자
// ─────────────────────────────────────────────
SimSession::SimSession(int id,
                       const std::string& strUser,
                       const std::string& strAuthId,
                       const std::string& strDomain,
                       const std::string& strPwd,
                       const std::string& strServerIp,
                       int iServerPort,
                       const std::string& strLocalIp,
                       int iLocalPort,
                       bool bPttMode,
                       const std::string& strGroupId)
    : m_iId(id), m_strUser(strUser), m_strAuthId(strAuthId),
      m_strDomain(strDomain), m_strPwd(strPwd),
      m_strServerIp(strServerIp), m_iServerPort(iServerPort),
      m_strLocalIp(strLocalIp), m_iLocalPort(iLocalPort),
      m_bPttMode(bPttMode), m_strGroupId(strGroupId),
      m_bRegistered(false), m_bInCall(false),
      m_bGmsSubscribed(false), m_bCmsSubscribed(false),
      m_iGmsSeq(1), m_iCmsSeq(1)
{
    m_pSipClient = new SessionSipClient(this);
    m_pSipClient->m_bPttMode    = m_bPttMode;
    m_pSipClient->m_pRtpThread  = &m_clsRtpThread;
    m_pSipClient->m_pInviteId   = &m_strInviteId;
    m_pSipClient->m_pUserAgent  = &m_clsUserAgent;

    m_clsServerInfo.m_strIp          = m_strServerIp;
    m_clsServerInfo.m_strDomain      = m_strDomain;
    m_clsServerInfo.m_strUserId      = m_strUser;
    m_clsServerInfo.m_strAuthId      = m_strAuthId.empty() ? m_strUser : m_strAuthId;
    m_clsServerInfo.m_strPassWord    = m_strPwd;
    m_clsServerInfo.m_eTransport     = E_SIP_UDP;
    m_clsServerInfo.m_iPort          = m_iServerPort;
    m_clsServerInfo.m_iLoginTimeout  = 600;

    m_clsSetup.m_iLocalUdpPort = m_iLocalPort;
    m_clsSetup.m_strLocalIp    = m_strLocalIp;
    m_clsSetup.m_strDomain     = m_strDomain;

    // REGISTER 자동 송신은 Start() 에서 m_bNoRegister 검사 후 결정.
    // m_bNoRegister=true (외부 SIP peer 모드) 면 InsertRegisterInfo 자체 skip
    // → psip 의 m_clsRegisterList 가 비어서 자동 REGISTER 안 나감.
}

SimSession::~SimSession() {
    Stop();
    delete m_pSipClient;
}

// ─────────────────────────────────────────────
//  시작 / 정지
// ─────────────────────────────────────────────
bool SimSession::Start() {
    m_stats.tRegStart = NowMs();

    if (!m_bNoRegister) {
        m_clsUserAgent.InsertRegisterInfo(m_clsServerInfo);
    }

    if (!m_clsUserAgent.Start(m_clsSetup, m_pSipClient)) {
        printf("[%d] SIP stack start error (port %d)\n", m_iId, m_iLocalPort);
        m_stats.iRegFail++;
        return false;
    }

    // port 0 자동할당 시 SipStack에서 실제 포트를 읽어 반영
    if (m_iLocalPort == 0) {
        m_iLocalPort = m_clsUserAgent.m_clsSipStack.m_clsSetup.m_iLocalUdpPort;
        m_clsSetup.m_iLocalUdpPort = m_iLocalPort;
    }

    // 추가 콜백 등록: SUBSCRIBE/NOTIFY는 여기서 처리
    m_clsUserAgent.m_clsSipStack.AddCallBack(this);

    if (!m_clsRtpThread.Create()) {
        printf("[%d] RTP thread create error\n", m_iId);
        return false;
    }

    printf("[%d] User=%s started on %s:%d\n",
           m_iId, m_strUser.c_str(), m_strLocalIp.c_str(), m_iLocalPort);
    return true;
}

void SimSession::Stop() {
    if (!m_strInviteId.empty()) {
        m_clsUserAgent.StopCall(m_strInviteId.c_str());
    }
    m_clsRtpThread.Stop();
    m_clsUserAgent.Stop();
}

// ─────────────────────────────────────────────
//  SUBSCRIBE 전송 (GMS / CMS 공통)
// ─────────────────────────────────────────────
void SimSession::SendSubscribe(const std::string& strPsi,
                                std::string& strCallIdOut,
                                int& iSeqOut,
                                std::string& strFromTagOut)
{
    const std::string& strLocalIp = m_clsSetup.m_strLocalIp;
    int iLocalPort = m_iLocalPort;

    // Call-ID 생성
    char szCallId[128];
    snprintf(szCallId, sizeof(szCallId), "sub_%s_%s_%d_%d",
             strPsi.c_str(), m_strUser.c_str(), m_iId, (int)time(NULL));
    strCallIdOut = szCallId;
    iSeqOut = 1;

    // From-tag 생성
    char szTag[64];
    SipMakeTag(szTag, sizeof(szTag));
    strFromTagOut = szTag;

    CSipMessage* pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "SUBSCRIBE";

    // Request-URI: sip:gms_psi@domain
    pMsg->m_clsReqUri.Set("sip", strPsi.c_str(), m_strDomain.c_str(), m_iServerPort);

    // Via
    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch(szBranch, sizeof(szBranch));
    pMsg->AddVia(strLocalIp.c_str(), iLocalPort, szBranch);

    // From
    pMsg->m_clsFrom.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), 0);
    pMsg->m_clsFrom.InsertParam(SIP_TAG, szTag);

    // To
    pMsg->m_clsTo.m_clsUri.Set("sip", strPsi.c_str(), m_strDomain.c_str(), 0);

    // Call-ID
    pMsg->m_clsCallId.Parse(szCallId, (int)strlen(szCallId));

    // CSeq
    pMsg->m_clsCSeq.Set(iSeqOut, "SUBSCRIBE");

    pMsg->m_iMaxForwards = 70;
    pMsg->AddHeader("Expires", "3600");
    pMsg->AddHeader("Event", "xcap-diff");
    pMsg->AddHeader("Accept", "application/xcap-diff+xml");

    // Contact
    char szContact[128];
    snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>",
             m_strUser.c_str(), strLocalIp.c_str(), iLocalPort);
    pMsg->AddHeader("Contact", szContact);

    // Body: resource-lists (구독할 문서 목록)
    std::string strBody;
    strBody  = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    strBody += "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\">\r\n";
    strBody += "  <list>\r\n";
    if (strPsi == "gms_psi") {
        strBody += "    <entry uri=\"org.openmobilealliance.groups/users/tel:"
                + m_strUser + "\"/>\r\n";
    } else {
        strBody += "    <entry uri=\"org.3gpp.mcptt.user-profile/users/tel:"
                + m_strUser + "/user-profile\"/>\r\n";
        strBody += "    <entry uri=\"org.3gpp.mcptt.service-config/users/tel:"
                + m_strUser + "/service-config\"/>\r\n";
    }
    strBody += "  </list>\r\n";
    strBody += "</resource-lists>\r\n";

    pMsg->m_clsContentType.Set("application", "resource-lists+xml");
    pMsg->m_clsContentType.InsertParam("charset", "utf-8");
    pMsg->m_strBody = strBody;
    pMsg->m_iContentLength = (int)strBody.size();

    // Route to server
    pMsg->AddRoute(m_strServerIp.c_str(), m_iServerPort, E_SIP_UDP);

    printf("[%d] SUBSCRIBE %s Call-ID=%s\n", m_iId, strPsi.c_str(), szCallId);
    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

void SimSession::SubscribeGms() {
    SendSubscribe("gms_psi", m_strGmsCallId, m_iGmsSeq, m_strGmsFromTag);
}

void SimSession::SubscribeCms() {
    SendSubscribe("cms_psi", m_strCmsCallId, m_iCmsSeq, m_strCmsFromTag);
}

// ─────────────────────────────────────────────
//  일반 VoIP 통화
// ─────────────────────────────────────────────
void SimSession::StartCall(const std::string& strTarget) {
    if (!m_strInviteId.empty()) return;

    CSipCallRtp clsRtp;
    CSipCallRoute clsRoute;

    clsRtp.m_strIp  = m_clsSetup.m_strLocalIp;
    clsRtp.m_iPort  = m_clsRtpThread.m_iPort;
    // AMR-WB (PT=99) when media file is provided, otherwise PCMU (PT=0)
    clsRtp.m_iCodec = m_clsRtpThread.m_strMediaFile.empty() ? 0 : 99;

#ifdef USE_MEDIA_LIST
    // Audio media line
    {
        int audioCodec = clsRtp.m_iCodec;
        CSdpMedia clsAudio("audio", m_clsRtpThread.m_iPort, "RTP/AVP");
        clsAudio.AddFmt(audioCodec);
        if (audioCodec == 99) {
            clsAudio.AddAttribute("rtpmap", "99 AMR-WB/16000/1");
            clsAudio.AddAttribute("fmtp", "99 mode-change-capability=2; max-red=0; octet-align=1");
        } else {
            clsAudio.AddAttribute("rtpmap", "0 PCMU/8000");
        }
        clsRtp.m_clsMediaList.push_back(clsAudio);
    }
    // Video media line (if video file set)
    if (m_clsRtpThread.m_iVideoPort > 0) {
        CSdpMedia clsVideo("video", m_clsRtpThread.m_iVideoPort, "RTP/AVP");
        clsVideo.AddFmt(96);
        clsVideo.AddAttribute("rtpmap", "96 H264/90000");
        clsVideo.AddAttribute("fmtp", "96 profile-level-id=42C016; packetization-mode=1");
        clsRtp.m_clsMediaList.push_back(clsVideo);
    }
#endif

    clsRoute.m_strDestIp  = m_strServerIp;
    clsRoute.m_iDestPort  = m_iServerPort;
    clsRoute.m_eTransport = E_SIP_UDP;

    std::string strDst = strTarget.empty() ? m_strServerIp : strTarget;
    m_stats.tCallStart = NowMs();

    printf("[%d] INVITE → %s\n", m_iId, strDst.c_str());
    m_clsUserAgent.StartCall(m_strUser.c_str(), strDst.c_str(),
                              &clsRtp, &clsRoute, m_strInviteId);
}

void SimSession::StopCall() {
    if (!m_strInviteId.empty()) {
        m_clsUserAgent.StopCall(m_strInviteId.c_str());
        m_clsRtpThread.Stop();
        m_strInviteId.clear();
    }
}

// ─────────────────────────────────────────────
//  PTT 그룹통화 시작: 그룹 ID로 INVITE 발신
// ─────────────────────────────────────────────
void SimSession::StartGroupCall(const std::string& strGroupId) {
    if (!m_strInviteId.empty()) return;
    std::string strTarget = strGroupId.empty() ? m_strGroupId : strGroupId;
    if (strTarget.empty()) {
        printf("[%d] StartGroupCall: no group ID configured\n", m_iId);
        return;
    }
    StartCall(strTarget);
}

// ─────────────────────────────────────────────
//  RTP Floor Control
// ─────────────────────────────────────────────
void SimSession::SendPttRequest()  { if (m_bPttMode) m_clsRtpThread.SendFloorControl(1); }
void SimSession::SendPttRelease()  { if (m_bPttMode) m_clsRtpThread.SendFloorControl(4); }

// ─────────────────────────────────────────────
//  mcptt-info+xml 파싱 유틸 (로그용)
// ─────────────────────────────────────────────
static std::string ExtractXmlTag(const std::string& body, const std::string& tag) {
    std::string open  = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t s = body.find(open);
    if (s == std::string::npos) return "";
    s += open.size();
    size_t e = body.find(close, s);
    if (e == std::string::npos) return "";
    return body.substr(s, e - s);
}

static void ParseAndLogMcpttInfo(int iId, const std::string& strBody) {
    // multipart boundary 탐색
    size_t pos = strBody.find("mcptt-info+xml");
    if (pos == std::string::npos) return;

    // XML 섹션 추출 (\r\n\r\n 이후 ~ 다음 boundary 전)
    size_t xmlStart = strBody.find("\r\n\r\n", pos);
    if (xmlStart == std::string::npos) return;
    xmlStart += 4;
    size_t xmlEnd = strBody.find("\r\n--", xmlStart);
    std::string strXml = (xmlEnd != std::string::npos)
                         ? strBody.substr(xmlStart, xmlEnd - xmlStart)
                         : strBody.substr(xmlStart);

    std::string sessionType  = ExtractXmlTag(strXml, "session-type");
    std::string requestUri   = ExtractXmlTag(strXml, "mcptt-request-uri");
    std::string callingUser  = ExtractXmlTag(strXml, "mcptt-calling-user-id");
    std::string callingGroup = ExtractXmlTag(strXml, "mcptt-calling-group-id");

    printf("[%d] [MCPTT-INFO] session=%s request-uri=%s caller=%s group=%s\n",
           iId, sessionType.c_str(), requestUri.c_str(),
           callingUser.c_str(), callingGroup.c_str());
}

// ─────────────────────────────────────────────
//  ISipStackCallBack - 수신 요청 처리
// ─────────────────────────────────────────────
bool SimSession::RecvRequest(int /*iThreadId*/, CSipMessage* pclsMessage) {
    // INVITE: mcptt-info+xml 파싱만 하고 처리는 SipUserAgent에 위임
    if (pclsMessage->IsMethod("INVITE")) {
        if (!pclsMessage->m_strBody.empty()) {
            ParseAndLogMcpttInfo(m_iId, pclsMessage->m_strBody);
        }
        return false;  // SipUserAgent가 계속 처리하도록
    }

    if (!pclsMessage->IsMethod("NOTIFY")) return false;

    // To 헤더의 사용자가 나인지 확인
    if (pclsMessage->m_clsTo.m_clsUri.m_strUser != m_strUser) return false;

    // Conference Event NOTIFY (RFC 4575, in-dialog) 처리
    CSipHeader* pEvtHdr = pclsMessage->GetHeader("Event");
    std::string strEvt = pEvtHdr ? pEvtHdr->m_strValue : "";
    if (strEvt.find("conference") != std::string::npos) {
        m_stats.iConfNotify++;
        printf("[%d] [CONF] Conference NOTIFY received (v%d)\n", m_iId, m_stats.iConfNotify.load());
        // conference-info+xml 에서 user entity/status 추출
        const std::string& body = pclsMessage->m_strBody;
        // <user entity="tel:+82571900005" state="full">
        size_t upos = body.find("entity=\"tel:");
        if (upos != std::string::npos) {
            size_t uend = body.find("\"", upos + 12);
            std::string user = (uend != std::string::npos) ? body.substr(upos + 12, uend - upos - 12) : "?";
            // <status>connected</status>
            size_t spos = body.find("<status>");
            std::string status = "?";
            if (spos != std::string::npos) {
                size_t send = body.find("</status>", spos);
                if (send != std::string::npos) status = body.substr(spos + 8, send - spos - 8);
            }
            printf("[%d] [CONF]   user=%s status=%s\n", m_iId, user.c_str(), status.c_str());
        }
        // 200 OK 응답
        CSipMessage* pRes = pclsMessage->CreateResponseWithToTag(200);
        if (pRes) m_clsUserAgent.m_clsSipStack.SendSipMessage(pRes);
        return true;
    }

    HandleNotify(pclsMessage);

    // 200 OK 응답
    CSipMessage* pRes = pclsMessage->CreateResponseWithToTag(200);
    if (pRes) m_clsUserAgent.m_clsSipStack.SendSipMessage(pRes);

    return true;
}

bool SimSession::RecvResponse(int /*iThreadId*/, CSipMessage* pclsMessage) {
    if (pclsMessage->m_clsCSeq.m_strMethod != "SUBSCRIBE") return false;

    std::string strCallId;
    pclsMessage->GetCallId(strCallId);
    int iStatus = pclsMessage->m_iStatusCode;

    if (iStatus == 200) {
        if (strCallId == m_strGmsCallId) {
            m_bGmsSubscribed = true;
            m_stats.iGmsOk++;
            printf("[%d] GMS SUBSCRIBED OK\n", m_iId);
        } else if (strCallId == m_strCmsCallId) {
            m_bCmsSubscribed = true;
            m_stats.iCmsOk++;
            printf("[%d] CMS SUBSCRIBED OK\n", m_iId);
        }
    } else if (iStatus >= 400) {
        printf("[%d] SUBSCRIBE %d error (CallId=%s)\n",
               m_iId, iStatus, strCallId.c_str());
    }
    return true;
}

bool SimSession::SendTimeout(int /*iThreadId*/, CSipMessage* /*pclsMessage*/) {
    return false;
}

// ─────────────────────────────────────────────
//  NOTIFY 처리: xcap-diff 본문 파싱 + 통계
// ─────────────────────────────────────────────
void SimSession::HandleNotify(CSipMessage* pclsMessage) {
    m_stats.iNotifyRecv++;

    // Event 헤더 확인
    CSipHeader* pEvt = pclsMessage->GetHeader("Event");
    std::string strEvent = pEvt ? pEvt->m_strValue : "unknown";

    // Subscription-State 헤더
    CSipHeader* pState = pclsMessage->GetHeader("Subscription-State");
    std::string strState = pState ? pState->m_strValue : "";

    printf("[%d] NOTIFY Event=%s State=%s\n", m_iId, strEvent.c_str(), strState.c_str());

    // xcap-diff 본문에서 sel(문서 경로) 추출
    const std::string& strBody = pclsMessage->m_strBody;
    if (!strBody.empty()) {
        // sel="..." 값 간단히 추출
        size_t pos = strBody.find("sel=\"");
        while (pos != std::string::npos) {
            size_t end = strBody.find("\"", pos + 5);
            if (end != std::string::npos) {
                std::string strSel = strBody.substr(pos + 5, end - pos - 5);
                printf("[%d]   sel: %s\n", m_iId, strSel.c_str());
            }
            pos = strBody.find("sel=\"", pos + 1);
        }
        // new-etag 추출
        pos = strBody.find("new-etag=\"");
        if (pos != std::string::npos) {
            size_t end = strBody.find("\"", pos + 10);
            if (end != std::string::npos) {
                std::string strEtag = strBody.substr(pos + 10, end - pos - 10);
                printf("[%d]   etag: %s\n", m_iId, strEtag.c_str());
            }
        }
    }
}

// ─────────────────────────────────────────────
//  SessionSipClient 콜백
// ─────────────────────────────────────────────
void SessionSipClient::EventRegister(CSipServerInfo* pclsInfo, int iStatus) {
    if (iStatus == 200) {
        m_pOwner->m_bRegistered = true;
        m_pOwner->m_stats.iRegOk++;
        long long ms = SimSession::NowMs() - m_pOwner->m_stats.tRegStart;
        m_pOwner->m_stats.llTotalRegMs += ms;
        printf("[%d] REGISTERED User=%s (%lldms)\n",
               m_pOwner->m_iId, pclsInfo->m_strUserId.c_str(), ms);
    } else {
        m_pOwner->m_stats.iRegFail++;
        printf("[%d] REGISTER FAILED User=%s status=%d\n",
               m_pOwner->m_iId, pclsInfo->m_strUserId.c_str(), iStatus);
    }
}

void SessionSipClient::EventIncomingCall(const char* pszCallId, const char* pszFrom,
                                          const char* pszTo, CSipCallRtp* pclsRtp, CSipMessage* pclsMessage) {
    printf("[%d] INVITE from=%s to=%s\n", m_pOwner->m_iId, pszFrom, pszTo);

    if (m_pInviteId) *m_pInviteId = pszCallId;

    // PTT 모드: 180 Ringing → 200 OK 자동응답 (실 단말 동작과 동일)
    if (m_pOwner->m_bPttMode) {
        printf("[%d] [PTT] Group INVITE - sending 180 Ringing\n", m_pOwner->m_iId);
        m_pUserAgent->RingCall(pszCallId, 180, NULL);
        usleep(200000); // 200ms

        CSipCallRtp clsLocalRtp;
        clsLocalRtp.m_strIp  = m_pOwner->m_clsSetup.m_strLocalIp;
        clsLocalRtp.m_iPort  = m_pOwner->m_clsRtpThread.m_iPort;
        clsLocalRtp.m_iCodec = pclsRtp ? pclsRtp->m_iCodec : 0;

#ifdef USE_MEDIA_LIST
        // PTT 200 OK SDP: audio + video (비디오 파일이 있는 경우)
        {
            CSdpMedia clsAudio("audio", m_pOwner->m_clsRtpThread.m_iPort, "RTP/AVP");
            clsAudio.AddFmt(clsLocalRtp.m_iCodec);
            if (clsLocalRtp.m_iCodec == 99) {
                clsAudio.AddAttribute("rtpmap", "99 AMR-WB/16000/1");
                clsAudio.AddAttribute("fmtp", "99 mode-change-capability=2; max-red=0; octet-align=1");
            } else {
                clsAudio.AddAttribute("rtpmap", "0 PCMU/8000");
            }
            clsLocalRtp.m_clsMediaList.push_back(clsAudio);
        }
        if (m_pOwner->m_clsRtpThread.m_iVideoPort > 0) {
            CSdpMedia clsVideo("video", m_pOwner->m_clsRtpThread.m_iVideoPort, "RTP/AVP");
            clsVideo.AddFmt(96);
            clsVideo.AddAttribute("rtpmap", "96 H264/90000");
            clsVideo.AddAttribute("fmtp", "96 profile-level-id=42C016; packetization-mode=1");
            clsLocalRtp.m_clsMediaList.push_back(clsVideo);
        }
#endif

        printf("[%d] [PTT] Sending 200 OK\n", m_pOwner->m_iId);
        m_pUserAgent->AcceptCall(pszCallId, &clsLocalRtp);
        if (pclsRtp) {
            m_pOwner->m_clsRtpThread.Start(pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort);
            // SDP에서 m=application 포트 추출 (floor control)
            if (pclsMessage && !pclsMessage->m_strBody.empty()) {
                size_t pos = pclsMessage->m_strBody.find("m=application ");
                if (pos != std::string::npos) {
                    int floorPort = atoi(pclsMessage->m_strBody.c_str() + pos + 14);
                    if (floorPort > 0) {
                        m_pOwner->m_clsRtpThread.m_iDestFloorPort = floorPort;
                        printf("[%d] [PTT] Floor port from SDP: %d\n", m_pOwner->m_iId, floorPort);
                    }
                }
            }
            // X-Video-Port 헤더에서 비디오 포트 추출
            if (pclsMessage) {
                CSipHeader* pVideoHdr = pclsMessage->GetHeader("X-Video-Port");
                if (pVideoHdr && !pVideoHdr->m_strValue.empty()) {
                    int vp = atoi(pVideoHdr->m_strValue.c_str());
                    if (vp > 0) {
                        m_pOwner->m_clsRtpThread.m_iDestVideoPort = vp;
                        printf("[%d] [PTT] Video port from header: %d\n", m_pOwner->m_iId, vp);
                    }
                }
            }
        }

        // PTT 서버 초대 방식에서는 EventCallStart가 발생하지 않을 수 있으므로
        // AcceptCall 성공 후 직접 통화 성공 기록
        m_pOwner->m_bInCall = true;
        m_pOwner->m_stats.iCallOk++;
        printf("[%d] [PTT] Call accepted (group invite)\n", m_pOwner->m_iId);
    } else {
        // VoIP 모드: 180 Ringing → 1초 → 200 OK
        m_pUserAgent->RingCall(pszCallId, 180, NULL);
        sleep(1);
        CSipCallRtp clsLocalRtp;
        clsLocalRtp.m_strIp  = m_pOwner->m_clsSetup.m_strLocalIp;
        clsLocalRtp.m_iPort  = m_pOwner->m_clsRtpThread.m_iPort;
        clsLocalRtp.m_iCodec = pclsRtp ? pclsRtp->m_iCodec : 0;

#ifdef USE_MEDIA_LIST
        // 200 OK SDP에 audio + video 미디어 포함
        {
            CSdpMedia clsAudio("audio", m_pOwner->m_clsRtpThread.m_iPort, "RTP/AVP");
            clsAudio.AddFmt(clsLocalRtp.m_iCodec);
            if (clsLocalRtp.m_iCodec == 99) {
                clsAudio.AddAttribute("rtpmap", "99 AMR-WB/16000/1");
                clsAudio.AddAttribute("fmtp", "99 mode-change-capability=2; max-red=0; octet-align=1");
            } else {
                clsAudio.AddAttribute("rtpmap", "0 PCMU/8000");
            }
            clsLocalRtp.m_clsMediaList.push_back(clsAudio);
        }
        if (m_pOwner->m_clsRtpThread.m_iVideoPort > 0) {
            CSdpMedia clsVideo("video", m_pOwner->m_clsRtpThread.m_iVideoPort, "RTP/AVP");
            clsVideo.AddFmt(96);
            clsVideo.AddAttribute("rtpmap", "96 H264/90000");
            clsVideo.AddAttribute("fmtp", "96 profile-level-id=42C016; packetization-mode=1");
            clsLocalRtp.m_clsMediaList.push_back(clsVideo);
        }
#endif

        m_pUserAgent->AcceptCall(pszCallId, &clsLocalRtp);
        // 200 OK 후 150ms 대기 → RTP 송출 시작 (CMP 녹취 세그먼트 초반에 SPS/PPS 포함 보장)
        usleep(150000);
        if (pclsRtp) m_pOwner->m_clsRtpThread.Start(pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort);
    }
}

void SessionSipClient::EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) {
    CSipClient::EventCallStart(pszCallId, pclsRtp);
    // PTT 착신은 EventIncomingCall에서 이미 카운팅했으므로 중복 방지
    if (!m_pOwner->m_bInCall) {
        m_pOwner->m_bInCall = true;
        m_pOwner->m_stats.iCallOk++;
    }
    long long ms = SimSession::NowMs() - m_pOwner->m_stats.tCallStart;
    m_pOwner->m_stats.llTotalCallMs += ms;
    printf("[%d] CALL STARTED CallId=%s (%lldms)\n", m_pOwner->m_iId, pszCallId, ms);
}

void SessionSipClient::EventCallEnd(const char* pszCallId, int iSipStatus) {
    CSipClient::EventCallEnd(pszCallId, iSipStatus);
    m_pOwner->m_bInCall = false;
    m_pOwner->m_strInviteId.clear();
    m_pOwner->m_stats.iCallEnd++;
    printf("[%d] CALL ENDED CallId=%s status=%d\n", m_pOwner->m_iId, pszCallId, iSipStatus);
}
