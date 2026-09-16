#include "CsimPeer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <strings.h>

#include "SipCodecTable.h"
#include "SipMessage.h"

/** 오퍼 첫 audio m-line 에서 엔트리 코덱이 선언된 PT — rtpmap 이름 일치, 없으면 정적 PT(<96) 가 fmt 목록에 있을 때 그 번호. -1 = 없음.
 *  RFC 3264: answer 는 오퍼가 선언한 PT 를 echo 한다. */
static int offeredPt(const CSipCallRtp& offer, const CSipCodecEntry& e) {
    for (const CSdpMedia& m : offer.m_clsMediaList) {
        if (strcasecmp(m.m_strMedia.c_str(), "audio") || m.m_iPort <= 0) continue;
        for (const CSdpAttribute& a : m.m_clsAttributeList) {
            if (strcasecmp(a.m_strName.c_str(), "rtpmap")) continue;
            const char* sp = strchr(a.m_strValue.c_str(), ' ');
            if (!sp) continue;
            size_t n = e.m_strName.size();
            if (strncasecmp(sp + 1, e.m_strName.c_str(), n) == 0 && (sp[1 + n] == '/' || sp[1 + n] == '\0'))
                return atoi(a.m_strValue.c_str());
        }
        if (e.m_iPt < 96)
            for (const std::string& f : m.m_clsFmtList)
                if (atoi(f.c_str()) == e.m_iPt) return e.m_iPt;
        return -1;
    }
    return -1;
}

/** answer audio m-line — 고른 엔트리 하나, 오퍼 PT echo. 반환 = wire PT. */
static int buildAnswerAudio(CSipCallRtp& clsRtp, int iPort, const CSipCodecEntry& e, const CSipCallRtp& offer) {
    int pt = offeredPt(offer, e);
    if (pt < 0) pt = e.m_iPt;
    CSdpMedia clsAudio("audio", iPort, "RTP/AVP");
    clsAudio.AddFmt(pt);
    char szVal[192];
    snprintf(szVal, sizeof(szVal), "%d %s", pt, e.GetRtpmap().c_str());
    clsAudio.AddAttribute("rtpmap", szVal);
    if (!e.m_strFmtp.empty()) {
        snprintf(szVal, sizeof(szVal), "%d %s", pt, e.m_strFmtp.c_str());
        clsAudio.AddAttribute("fmtp", szVal);
    }
    clsRtp.m_clsMediaList.push_back(clsAudio);
    return pt;
}

long long CsimPeer::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string CsimPeer::genIcid() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[40];
    unsigned long long a = rng(), b = rng();
    snprintf(buf, sizeof(buf), "%016llx%08llx", a, b & 0xffffffffULL);
    return buf;
}

std::vector<std::string> CsimPeer::DefaultCodecs(const std::string& profile) {
    // §3.2 — IP-PBX 는 G.711 필수(SIPconnect 2.0), IM-MGW/타 IMS 는 AMR-WB 를 오퍼한다(TS 29.163 §9, IR.92)
    if (profile == "pbx") return { "PCMA", "PCMU" };
    return { "AMR-WB", "AMR", "PCMU", "PCMA" };
}

CsimPeer::CsimPeer(const CsimPeerConfig& cfg) : m_cfg(cfg) {
    if (m_cfg.codecs.empty()) m_cfg.codecs = DefaultCodecs(m_cfg.profile);
    if (m_cfg.userAgent.empty()) m_cfg.userAgent = "cims-tester-peer/" + m_cfg.profile;
    m_clsSetup.m_strLocalIp = m_cfg.bindIp;
    m_clsSetup.m_strDomain = m_cfg.domain;
    m_clsSetup.m_strUserAgent = m_cfg.userAgent;
    if (m_cfg.transport == E_SIP_TLS) {
        m_clsSetup.m_iLocalTlsPort = m_cfg.port;
        m_clsSetup.m_strCertFile = m_cfg.certFile;
    } else if (m_cfg.transport == E_SIP_TCP) {
        m_clsSetup.m_iLocalTcpPort = m_cfg.port;
    } else {
        m_clsSetup.m_iLocalUdpPort = m_cfg.port;
    }
}

CsimPeer::~CsimPeer() { Stop(); }

bool CsimPeer::Start(std::string& err) {
    if (m_bStarted) return true;
    if (m_cfg.bindIp.empty() || m_cfg.port <= 0) { err = "bind ip/port required"; return false; }
    if (m_cfg.transport == E_SIP_TLS && m_cfg.certFile.empty()) { err = "tls transport needs certFile"; return false; }
    m_codecs.clear();
    for (const auto& name : m_cfg.codecs) {
        const CSipCodecEntry* found = nullptr;
        for (const auto& e : CSipCodecTable::GetList())
            if (strcasecmp(e.m_strName.c_str(), name.c_str()) == 0) { found = &e; break; }
        if (found) m_codecs.push_back(found);
        else printf("[peer %s] codec %s not in table — skipped\n", m_cfg.name.c_str(), name.c_str());
    }
    if (m_codecs.empty()) { err = "no usable codec"; return false; }
    m_clsUserAgent.m_clsSipStack.AddCallBack(this);   // BYE 응답 관측 — UA 보다 먼저
    if (!m_clsUserAgent.Start(m_clsSetup, this)) {
        err = "sip stack start failed (" + m_cfg.bindIp + ":" + std::to_string(m_cfg.port) + ")";
        return false;
    }
    m_bStarted = true;
    printf("[peer %s] started %s:%d %s domain=%s profile=%s codecs=%zu%s\n", m_cfg.name.c_str(), m_cfg.bindIp.c_str(),
           m_cfg.port, m_cfg.transport == E_SIP_TLS ? "tls" : m_cfg.transport == E_SIP_TCP ? "tcp" : "udp",
           m_cfg.domain.c_str(), m_cfg.profile.c_str(), m_codecs.size(), m_cfg.silent ? " (silent)" : "");
    return true;
}

void CsimPeer::Stop() {
    if (!m_bStarted) return;
    m_clsUserAgent.StopCallAll();
    m_clsUserAgent.Stop();
    // Final() 은 부르지 않는다 — psip 의 Final 은 전역 SSLFinal() 이라 같은 프로세스의 다른 엔진(피어·SimSession)이 쓰는
    //   OpenSSL 전역을 해제한다(엔진 둘일 때 먼저 만든 쪽을 내리면 free(): invalid pointer). SimSession::Stop 과 같은 범위.
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& kv : m_calls) freeRtp(kv.second.rtp);
    m_calls.clear();
    m_bStarted = false;
}

CRtpThread* CsimPeer::newRtp() {
    CRtpThread* rtp = new CRtpThread();
    if (!m_cfg.mediaFile.empty()) rtp->SetMediaFile(m_cfg.mediaFile);
    if (!rtp->Create()) { delete rtp; return nullptr; }
    return rtp;
}

void CsimPeer::freeRtp(CRtpThread* rtp) {
    if (!rtp) return;
    rtp->Stop();
    delete rtp;   // 소멸자가 Destroy()
}

void CsimPeer::buildOffer(CSipCallRtp& clsRtp, CRtpThread* rtp) {
    clsRtp.m_strIp = m_clsSetup.m_strLocalIp;
    clsRtp.m_iPort = rtp->m_iPort;
    clsRtp.m_iCodec = m_codecs[0]->m_iPt;
    CSdpMedia clsAudio("audio", rtp->m_iPort, "RTP/AVP");
    char szVal[192];
    for (const CSipCodecEntry* e : m_codecs) {
        clsAudio.AddFmt(e->m_iPt);
        snprintf(szVal, sizeof(szVal), "%d %s", e->m_iPt, e->GetRtpmap().c_str());
        clsAudio.AddAttribute("rtpmap", szVal);
        if (!e->m_strFmtp.empty()) {
            snprintf(szVal, sizeof(szVal), "%d %s", e->m_iPt, e->m_strFmtp.c_str());
            clsAudio.AddAttribute("fmtp", szVal);
        }
    }
    clsRtp.m_clsMediaList.push_back(clsAudio);
    rtp->m_iAudioPt = m_codecs[0]->m_iPt;
}

const CSipCodecEntry* CsimPeer::pickAnswerCodec(const CSipCallRtp& offer) const {
    for (const CSipCodecEntry* e : m_codecs)
        if (offeredPt(offer, *e) >= 0) return e;
    return nullptr;
}

std::string CsimPeer::StartCall(const std::string& fromUser, const std::string& toUser, const std::string& toDomain,
                                const std::string& destIp, int destPort, ESipTransport eTransport) {
    if (!m_bStarted) return "";
    CRtpThread* rtp = newRtp();
    if (!rtp) return "";
    CSipCallRtp clsRtp;
    buildOffer(clsRtp, rtp);
    CSipCallRoute clsRoute;
    clsRoute.m_strDestIp = destIp;
    clsRoute.m_iDestPort = destPort;
    clsRoute.m_eTransport = eTransport;
    std::string callId;
    CSipMessage* pInvite = nullptr;
    if (!m_clsUserAgent.CreateCall(fromUser.c_str(), toUser.c_str(), &clsRtp, &clsRoute, callId, &pInvite, NULL) || !pInvite) {
        freeRtp(rtp);
        return "";
    }
    // Request-URI/To = user@상대 도메인 (psip 기본은 user@접속IP). 다음 홉은 Route(접속점)가 정한다.
    if (!toDomain.empty()) {
        pInvite->m_clsReqUri.Set(SIP_PROTOCOL, toUser.c_str(), toDomain.c_str(), 0);
        pInvite->m_clsReqUri.InsertTransport(eTransport);
        pInvite->m_clsTo.m_clsUri.Set(SIP_PROTOCOL, toUser.c_str(), toDomain.c_str(), 0);
    }
    if (m_cfg.profile == "ibcf") {
        // TS 24.229 §7.2A.5 / RFC 7315 — 타 IMS 코어가 II-NNI 로 넘길 때 싣는 과금 상관 벡터
        std::string pcv = "icid-value=" + genIcid() + ";orig-ioi=" + m_cfg.domain;
        pInvite->AddHeader("P-Charging-Vector", pcv.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        Call& c = m_calls[callId];
        c.callId = callId;
        c.fromUser = fromUser;
        c.toUser = toUser;
        c.outbound = true;
        c.rtp = rtp;
        c.tStart = nowMs();
    }
    if (!m_clsUserAgent.StartCall(callId.c_str(), pInvite)) {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it != m_calls.end()) { freeRtp(it->second.rtp); m_calls.erase(it); }
        return "";
    }
    return callId;
}

bool CsimPeer::Ring(const std::string& callId, int iCode) {
    return m_clsUserAgent.RingCall(callId.c_str(), iCode, NULL);
}

int CsimPeer::Answer(const std::string& callId) {
    CSipCallRtp offer;
    CRtpThread* rtp = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end() || it->second.outbound || it->second.connected) return 481;
        if (!it->second.hasOffer) return 488;
        offer = it->second.offer;
        rtp = it->second.rtp;
    }
    const CSipCodecEntry* e = (offer.m_bRemoteSavp || !offer.m_strRemoteCryptoKey.empty()) ? nullptr : pickAnswerCodec(offer);
    if (!e) {
        m_clsUserAgent.StopCall(callId.c_str(), 488);
        return 488;
    }
    CSipCallRtp clsLocal;
    clsLocal.m_strIp = m_clsSetup.m_strLocalIp;
    clsLocal.m_iPort = rtp->m_iPort;
    clsLocal.m_iCodec = e->m_iPt;
    rtp->m_iAudioPt = buildAnswerAudio(clsLocal, rtp->m_iPort, *e, offer);
    // 파일 미디어(AMR-WB)는 그 코덱으로 합의됐을 때만, 아니면 합성 PCMU
    if (strcasecmp(e->m_strName.c_str(), "AMR-WB") != 0) rtp->SetMediaFile("");
    if (!m_clsUserAgent.AcceptCall(callId.c_str(), &clsLocal)) return 481;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it != m_calls.end()) it->second.connected = true;
    }
    rtp->ResetRecvStats();
    rtp->Start(offer.m_strIp.c_str(), offer.m_iPort);
    return 0;
}

bool CsimPeer::Reject(const std::string& callId, int iCode) {
    return m_clsUserAgent.StopCall(callId.c_str(), iCode);
}

bool CsimPeer::Bye(const std::string& callId) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end()) return false;
        it->second.tBye = nowMs();
    }
    return m_clsUserAgent.StopCall(callId.c_str());
}

bool CsimPeer::HasCall(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_calls.count(callId) > 0;
}

bool CsimPeer::Connected(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    return it != m_calls.end() && it->second.connected;
}

bool CsimPeer::RtpStats(const std::string& callId, unsigned long long& rx, unsigned long long& lost, long long& jitterUs) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end() || !it->second.rtp) return false;
    rx = it->second.rtp->m_ullRecvTotal.load();
    lost = it->second.rtp->m_ullRecvLost.load();
    jitterUs = it->second.rtp->m_llRecvJitterUs.load();
    return true;
}

void CsimPeer::ResetRtpStats(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it != m_calls.end() && it->second.rtp) it->second.rtp->ResetRecvStats();
}

size_t CsimPeer::CallCount() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_calls.size();
}

// ── 스택 콜백 ─────────────────────────────────────────────────────────────
void CsimPeer::EventIncomingCall(const char* pszCallId, const char* pszFrom, const char* pszTo, CSipCallRtp* pclsRtp,
                                 CSipMessage* pclsMessage) {
    if (m_cfg.silent) return;   // 죽은 피어 — 트랜잭션은 psip 이 타이머로 접는다
    bool hasPai = pclsMessage && pclsMessage->GetHeader("P-Asserted-Identity") != NULL;
    CRtpThread* rtp = newRtp();
    if (!rtp) { m_clsUserAgent.StopCall(pszCallId, 500); return; }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        Call& c = m_calls[pszCallId];
        c.callId = pszCallId;
        c.fromUser = pszFrom ? pszFrom : "";
        c.toUser = pszTo ? pszTo : "";
        c.outbound = false;
        c.rtp = rtp;
        c.tStart = nowMs();
        if (pclsRtp) { c.offer = *pclsRtp; c.hasOffer = true; }
    }
    if (m_pObserver) m_pObserver->OnPeerIncoming(this, pszCallId, pszFrom ? pszFrom : "", pszTo ? pszTo : "", hasPai);
    else m_clsUserAgent.StopCall(pszCallId, 480);
}

void CsimPeer::EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) {
    long long srd = 0;
    CRtpThread* rtp = nullptr;
    bool outbound = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(pszCallId);
        if (it == m_calls.end()) return;
        it->second.connected = true;
        srd = nowMs() - it->second.tStart;
        rtp = it->second.rtp;
        outbound = it->second.outbound;
    }
    if (outbound && rtp && pclsRtp) {
        // answer 가 고른 코덱 — 파일 미디어는 AMR-WB 합의일 때만
        const CSipCodecEntry* e = CSipCodecTable::FindByPt(pclsRtp->m_iCodec);
        if (e) { rtp->m_iAudioPt = e->m_iPt; if (strcasecmp(e->m_strName.c_str(), "AMR-WB") != 0) rtp->SetMediaFile(""); }
        rtp->ResetRecvStats();
        rtp->Start(pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort);
        if (m_pObserver) m_pObserver->OnPeerCallStart(this, pszCallId, srd);
    }
}

void CsimPeer::EventCallEnd(const char* pszCallId, int iSipStatus) {
    CRtpThread* rtp = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(pszCallId);
        if (it == m_calls.end()) return;
        rtp = it->second.rtp;
        m_calls.erase(it);
    }
    freeRtp(rtp);
    if (m_pObserver) m_pObserver->OnPeerCallEnd(this, pszCallId, iSipStatus);
}

bool CsimPeer::RecvResponse(int /*iThreadId*/, CSipMessage* pclsMessage) {
    if (pclsMessage->m_clsCSeq.m_strMethod != "BYE" || pclsMessage->m_iStatusCode < 200) return false;
    std::string callId;
    pclsMessage->GetCallId(callId);
    long long tBye = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end() || it->second.tBye == 0) return false;
        tBye = it->second.tBye;
        it->second.tBye = 0;
    }
    if (m_pObserver) m_pObserver->OnPeerByeResponse(this, callId, pclsMessage->m_iStatusCode, nowMs() - tBye);
    return false;   // 처리는 UA 에 위임 (다이얼로그 정리 → EventCallEnd 없이 끝날 수 있어 아래에서도 정리)
}
