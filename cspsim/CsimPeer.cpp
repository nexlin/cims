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

/** 상대 audio m-line 의 telephone-event(RFC 4733) PT 와 클록 — 없으면 pt=-1. */
static void remoteTelephoneEvent(const CSipCallRtp& sdp, int& pt, int& clock) {
    pt = -1;
    clock = 8000;
    for (const CSdpMedia& m : sdp.m_clsMediaList) {
        if (strcasecmp(m.m_strMedia.c_str(), "audio") || m.m_iPort <= 0) continue;
        for (const CSdpAttribute& a : m.m_clsAttributeList) {
            if (strcasecmp(a.m_strName.c_str(), "rtpmap")) continue;
            const char* sp = strchr(a.m_strValue.c_str(), ' ');
            if (!sp || strncasecmp(sp + 1, "telephone-event", 15)) continue;
            pt = atoi(a.m_strValue.c_str());
            const char* sl = strchr(sp + 1, '/');
            if (sl) clock = atoi(sl + 1);
            return;
        }
        return;
    }
}

/** audio m-line 에 telephone-event 를 덧붙인다(fmt + rtpmap + fmtp 0-16). */
static void addTelephoneEvent(CSdpMedia& clsAudio, int pt, int clock) {
    char szVal[64];
    clsAudio.AddFmt(pt);
    snprintf(szVal, sizeof(szVal), "%d telephone-event/%d", pt, clock);
    clsAudio.AddAttribute("rtpmap", szVal);
    snprintf(szVal, sizeof(szVal), "%d 0-16", pt);
    clsAudio.AddAttribute("fmtp", szVal);
}

/** answer audio m-line — 고른 엔트리 하나, 오퍼 PT echo. 반환 = wire PT. */
static int buildAnswerAudio(CSipCallRtp& clsRtp, int iPort, const CSipCodecEntry& e, const CSipCallRtp& offer,
                            bool bDtmf, int& iDtmfPt, int& iDtmfClock) {
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
    // telephone-event 는 오퍼가 냈을 때만 echo (RFC 3264 §6.1 — answer 는 오퍼의 부분집합)
    remoteTelephoneEvent(offer, iDtmfPt, iDtmfClock);
    if (bDtmf && iDtmfPt >= 0) addTelephoneEvent(clsAudio, iDtmfPt, iDtmfClock);
    else iDtmfPt = -1;
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

int CsimPeer::parseQ850(CSipMessage* pclsMessage) {
    // RFC 3326: Reason: Q.850;cause=16;text="Normal call clearing" (여러 값 가능 — Q.850 첫 항목)
    if (!pclsMessage) return 0;
    CSipHeader* h = pclsMessage->GetHeader("Reason");
    if (!h) return 0;
    const char* p = strcasestr(h->m_strValue.c_str(), "Q.850");
    if (!p) return 0;
    const char* c = strcasestr(p, "cause=");
    if (!c) return 0;
    int v = atoi(c + 6);
    return v > 0 ? v : 0;
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
    m_clsUserAgent.m_clsSipStack.AddCallBack(this);   // BYE 응답·Reason 관측 — UA 보다 먼저
    if (!m_clsUserAgent.Start(m_clsSetup, this)) {
        err = "sip stack start failed (" + m_cfg.bindIp + ":" + std::to_string(m_cfg.port) + ")";
        return false;
    }
    m_bStarted = true;
    printf("[peer %s] started %s:%d %s domain=%s profile=%s codecs=%zu prack=%d dtmf=%d%s%s\n", m_cfg.name.c_str(),
           m_cfg.bindIp.c_str(), m_cfg.port, m_cfg.transport == E_SIP_TLS ? "tls" : m_cfg.transport == E_SIP_TCP ? "tcp" : "udp",
           m_cfg.domain.c_str(), m_cfg.profile.c_str(), m_codecs.size(), m_cfg.prack ? 1 : 0, m_cfg.dtmf ? 1 : 0,
           m_cfg.silent ? " (silent)" : "", m_cfg.trunk.user.empty() ? "" : " (trunk register)");
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
    m_bRegistered = false;
}

bool CsimPeer::Register() {
    if (!m_bStarted || m_cfg.trunk.user.empty()) return false;
    const CsimPeerTrunkRegister& t = m_cfg.trunk;
    m_clsServerInfo.m_strIp = t.ip;
    m_clsServerInfo.m_iPort = t.port;
    m_clsServerInfo.m_eTransport = t.transport;
    m_clsServerInfo.m_strDomain = t.realm;
    m_clsServerInfo.m_strUserId = t.user;
    m_clsServerInfo.m_strAuthId = t.user;
    m_clsServerInfo.m_strPassWord = t.password;
    m_clsServerInfo.m_strHa1 = t.ha1;
    m_clsServerInfo.m_iLoginTimeout = t.expires;
    // 트렁크 계정 Contact — SIPconnect 2.0 §8 등록 모드: 계정 하나가 DID 범위를 대표한다(개별 DID 등록 없음)
    m_tRegStart = nowMs();
    m_bRegistered = false;
    return m_clsUserAgent.InsertRegisterInfo(m_clsServerInfo);
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
    if (m_cfg.dtmf) {
        // 클록 = 첫 코덱 클록(RFC 4733 §2.1 — 오디오와 같은 타임스탬프 축)
        int clock = m_codecs[0]->m_iClockRate > 0 ? m_codecs[0]->m_iClockRate : 8000;
        addTelephoneEvent(clsAudio, m_cfg.dtmfPt, clock);
        rtp->m_iDtmfClock = clock;
    }
    clsRtp.m_clsMediaList.push_back(clsAudio);
    rtp->m_iAudioPt = m_codecs[0]->m_iPt;
}

const CSipCodecEntry* CsimPeer::pickAnswerCodec(const CSipCallRtp& offer) const {
    for (const CSipCodecEntry* e : m_codecs)
        if (offeredPt(offer, *e) >= 0) return e;
    return nullptr;
}

void CsimPeer::applyRemoteDtmf(CRtpThread* rtp, const CSipCallRtp& remote) {
    int pt, clock;
    remoteTelephoneEvent(remote, pt, clock);
    rtp->m_iDtmfPt = pt;
    if (pt >= 0) rtp->m_iDtmfClock = clock;
}

bool CsimPeer::buildAnswer(Call& c) {
    if (c.hasAnswer) return true;
    if (!c.hasOffer) return false;
    const CSipCodecEntry* e = (c.offer.m_bRemoteSavp || !c.offer.m_strRemoteCryptoKey.empty()) ? nullptr : pickAnswerCodec(c.offer);
    if (!e) return false;
    CSipCallRtp clsLocal;
    clsLocal.m_strIp = m_clsSetup.m_strLocalIp;
    clsLocal.m_iPort = c.rtp->m_iPort;
    clsLocal.m_iCodec = e->m_iPt;
    int dtmfPt = -1, dtmfClock = 8000;
    c.rtp->m_iAudioPt = buildAnswerAudio(clsLocal, c.rtp->m_iPort, *e, c.offer, m_cfg.dtmf, dtmfPt, dtmfClock);
    c.rtp->m_iDtmfPt = dtmfPt;
    if (dtmfPt >= 0) c.rtp->m_iDtmfClock = dtmfClock;
    // 파일 미디어(AMR-WB)는 그 코덱으로 합의됐을 때만, 아니면 합성 PCMU
    c.rtp->m_bUseMediaFile = strcasecmp(e->m_strName.c_str(), "AMR-WB") == 0;
    if (!c.rtp->m_bUseMediaFile) c.rtp->SetMediaFile("");
    c.answer = clsLocal;
    c.hasAnswer = true;
    return true;
}

std::string CsimPeer::StartCall(const std::string& fromUser, const std::string& toUser, const std::string& toDomain,
                                const std::string& destIp, int destPort, ESipTransport eTransport, int iMediaMode) {
    if (!m_bStarted) return "";
    CRtpThread* rtp = newRtp();
    if (!rtp) return "";
    rtp->SetMediaMode(iMediaMode);
    CSipCallRtp clsRtp;
    buildOffer(clsRtp, rtp);
    CSipCallRoute clsRoute;
    clsRoute.m_strDestIp = destIp;
    clsRoute.m_iDestPort = destPort;
    clsRoute.m_eTransport = eTransport;
    clsRoute.m_b100rel = m_cfg.prack;   // RFC 3262 — INVITE 에 Supported/Require: 100rel
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

int CsimPeer::Progress(const std::string& callId) {
    CSipCallRtp answer, offer;
    CRtpThread* rtp = nullptr;
    bool has100rel = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end() || it->second.outbound || it->second.connected) return 481;
        if (!buildAnswer(it->second)) { m_clsUserAgent.StopCall(callId.c_str(), 488); return 488; }
        answer = it->second.answer;
        offer = it->second.offer;
        rtp = it->second.rtp;
        has100rel = it->second.has100rel;
        it->second.early = true;
    }
    if (has100rel && m_cfg.prack) {
        // RFC 3262 §3 — RSeq 는 1~2^31-1 임의 시작. psip RingCall(status,rtp) 은 m_iRSeq 가 있으면 Require: 100rel + RSeq 를 싣는다.
        static std::mt19937 rng{std::random_device{}()};
        m_clsUserAgent.SetRSeq(callId.c_str(), 1 + (int)(rng() % 100000));
    }
    if (!m_clsUserAgent.RingCall(callId.c_str(), 183, &answer)) return 481;
    // early media — 링백/안내음을 오퍼 주소로 송신, 수신 통계도 시작
    rtp->ResetRecvStats();
    rtp->Start(offer.m_strIp.c_str(), offer.m_iPort);
    return 0;
}

int CsimPeer::Answer(const std::string& callId) {
    CSipCallRtp answer, offer;
    CRtpThread* rtp = nullptr;
    bool early = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end() || it->second.outbound || it->second.connected) return 481;
        if (!it->second.hasOffer) return 488;
        if (!buildAnswer(it->second)) { m_clsUserAgent.StopCall(callId.c_str(), 488); return 488; }
        answer = it->second.answer;
        offer = it->second.offer;
        rtp = it->second.rtp;
        early = it->second.early;
    }
    if (!m_clsUserAgent.AcceptCall(callId.c_str(), &answer)) return 481;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it != m_calls.end()) it->second.connected = true;
    }
    if (!early) {
        rtp->ResetRecvStats();
        rtp->Start(offer.m_strIp.c_str(), offer.m_iPort);
    }
    return 0;
}

bool CsimPeer::Reject(const std::string& callId, int iCode, int iQ850) {
    if (iQ850 > 0) {
        char szReason[64];
        snprintf(szReason, sizeof(szReason), "Q.850;cause=%d", iQ850);
        return m_clsUserAgent.StopCall(callId.c_str(), iCode, szReason);
    }
    return m_clsUserAgent.StopCall(callId.c_str(), iCode);
}

bool CsimPeer::Bye(const std::string& callId, int iQ850) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end()) return false;
        it->second.tBye = nowMs();
    }
    if (iQ850 > 0) {
        char szReason[64];
        snprintf(szReason, sizeof(szReason), "Q.850;cause=%d", iQ850);
        return m_clsUserAgent.StopCall(callId.c_str(), 0, szReason);
    }
    return m_clsUserAgent.StopCall(callId.c_str());
}

bool CsimPeer::Hold(const std::string& callId) {
    if (!Connected(callId)) return false;
    return m_clsUserAgent.HoldCall(callId.c_str(), E_RTP_SEND);   // a=sendonly (RFC 3264 §8.4)
}

bool CsimPeer::Resume(const std::string& callId) {
    if (!Connected(callId)) return false;
    return m_clsUserAgent.ResumeCall(callId.c_str());
}

bool CsimPeer::Refer(const std::string& callId, const std::string& toUser) {
    if (!Connected(callId) || toUser.empty()) return false;
    return m_clsUserAgent.TransferCallBlind(callId.c_str(), toUser.c_str());
}

bool CsimPeer::SendDtmf(const std::string& callId, const std::string& digits) {
    CRtpThread* rtp = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(callId);
        if (it == m_calls.end() || !it->second.connected) return false;
        rtp = it->second.rtp;
    }
    return rtp && rtp->SendDtmf(digits);
}

bool CsimPeer::SetMediaMode(const std::string& callId, int iMediaMode) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end() || !it->second.rtp) return false;
    it->second.rtp->SetMediaMode(iMediaMode);
    return true;
}

bool CsimPeer::MediaSend(const std::string& callId, bool bDefault, const std::string& amrWbFile, const std::string& pcmuFile,
                         const std::string& pcmaFile, bool bLoop) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end() || !it->second.rtp || !it->second.rtp->MediaRunning()) return false;
    if (bDefault) it->second.rtp->MediaSendDefault();
    else it->second.rtp->MediaSend(amrWbFile, pcmuFile, pcmaFile, bLoop);
    return true;
}

bool CsimPeer::MediaStop(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end() || !it->second.rtp || !it->second.rtp->MediaRunning()) return false;
    it->second.rtp->MediaStop();
    return true;
}

unsigned long long CsimPeer::RtpSent(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    return (it == m_calls.end() || !it->second.rtp) ? 0 : it->second.rtp->m_ullSentTotal.load();
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

bool CsimPeer::RtpQuality(const std::string& callId, int& pt, int& rtcpRx, int& rrFractionLost) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end() || !it->second.rtp) return false;
    pt = it->second.rtp->m_iRecvPt.load();
    rtcpRx = it->second.rtp->m_iRtcpRecv.load();
    rrFractionLost = it->second.rtp->m_iRtcpRrFractionLost.load();
    return true;
}

bool CsimPeer::DtmfStats(const std::string& callId, int& sent, int& recv, std::string& recvDigits, int& negotiatedPt) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end() || !it->second.rtp) return false;
    negotiatedPt = it->second.rtp->m_iDtmfPt;
    sent = it->second.rtp->m_iDtmfSent.load();
    recv = it->second.rtp->m_iDtmfRecv.load();
    recvDigits = it->second.rtp->DtmfRecv();
    return true;
}

void CsimPeer::ResetRtpStats(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it != m_calls.end() && it->second.rtp) { it->second.rtp->ResetRecvStats(); it->second.rtp->ResetDtmf(); }
}

size_t CsimPeer::CallCount() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_calls.size();
}

// ── 스택 콜백 ─────────────────────────────────────────────────────────────
void CsimPeer::EventRegister(CSipServerInfo* /*pclsInfo*/, int iStatus) {
    // psip 등록 스레드: 401 챌린지는 안에서 재시도하고 최종 응답만 여기로 온다(만료 갱신도 같은 경로)
    bool ok = iStatus == 200;
    long long rrd = m_tRegStart ? nowMs() - m_tRegStart : 0;
    m_bRegistered = ok;
    printf("[peer %s] trunk REGISTER %s → %d (%lld ms)\n", m_cfg.name.c_str(), m_cfg.trunk.user.c_str(), iStatus, rrd);
    if (m_pObserver) m_pObserver->OnPeerRegister(this, iStatus, rrd);
    m_tRegStart = 0;
}

void CsimPeer::EventIncomingCall(const char* pszCallId, const char* pszFrom, const char* pszTo, CSipCallRtp* pclsRtp,
                                 CSipMessage* pclsMessage) {
    if (m_cfg.silent) return;   // 죽은 피어 — 트랜잭션은 psip 이 타이머로 접는다
    if (m_cfg.rejectCode > 0) {
        // 오류 주입 — 즉시 최종 응답(5xx failover·사용자 측 거절·Reason Q.850 투과 시험). 호 상태를 만들지 않는다
        Reject(pszCallId, m_cfg.rejectCode, m_cfg.rejectQ850);
        if (m_pObserver) m_pObserver->OnPeerFaultReject(this, pszCallId, pszTo ? pszTo : "", m_cfg.rejectCode);
        return;
    }
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
        c.has100rel = pclsMessage && pclsMessage->Is100rel();
        if (pclsRtp) { c.offer = *pclsRtp; c.hasOffer = true; }
    }
    if (m_pObserver) m_pObserver->OnPeerIncoming(this, pszCallId, pszFrom ? pszFrom : "", pszTo ? pszTo : "", hasPai);
    else m_clsUserAgent.StopCall(pszCallId, 480);
}

void CsimPeer::EventCallRing(const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRtp) {
    CRtpThread* rtp = nullptr;
    bool outbound = false, early = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(pszCallId);
        if (it == m_calls.end()) return;
        outbound = it->second.outbound;
        rtp = it->second.rtp;
        early = it->second.early;
        if (pclsRtp && pclsRtp->m_iPort > 0 && !early) it->second.early = true;
    }
    if (!outbound) return;
    // RFC 3262 §4 — RSeq 가 실린 1xx 는 PRACK 으로 확인한다(psip 이 RSeq 를 dialog 에 적재, PRACK 은 UA 몫). 183 이 answer 를
    //   실었으면 우리 오퍼에 대한 answer 라 PRACK 은 SDP 없이 낸다.
    bool prackSent = false;
    if (m_cfg.prack && m_clsUserAgent.GetRSeq(pszCallId) != -1) prackSent = m_clsUserAgent.SendPrack(pszCallId, NULL);
    bool hasSdp = pclsRtp && pclsRtp->m_iPort > 0;
    if (hasSdp && !early && rtp) {
        // early media — answer 코덱으로 송신 시작(링백 수신 통계도 여기서부터)
        const CSipCodecEntry* e = CSipCodecTable::FindByPt(pclsRtp->m_iCodec);
        if (e) { rtp->m_iAudioPt = e->m_iPt; rtp->m_bUseMediaFile = strcasecmp(e->m_strName.c_str(), "AMR-WB") == 0; if (!rtp->m_bUseMediaFile) rtp->SetMediaFile(""); }
        applyRemoteDtmf(rtp, *pclsRtp);
        rtp->ResetRecvStats();
        rtp->Start(pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort);
    }
    if (m_pObserver) m_pObserver->OnPeerRing(this, pszCallId, iSipStatus, hasSdp, prackSent);
}

void CsimPeer::EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) {
    long long srd = 0;
    CRtpThread* rtp = nullptr;
    bool outbound = false, early = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(pszCallId);
        if (it == m_calls.end()) return;
        it->second.connected = true;
        srd = nowMs() - it->second.tStart;
        rtp = it->second.rtp;
        outbound = it->second.outbound;
        early = it->second.early;
    }
    if (outbound && rtp && pclsRtp) {
        // answer 가 고른 코덱 — 파일 미디어는 AMR-WB 합의일 때만. 183 early media 로 이미 흐르면 목적지만 갱신
        const CSipCodecEntry* e = CSipCodecTable::FindByPt(pclsRtp->m_iCodec);
        if (e) { rtp->m_iAudioPt = e->m_iPt; rtp->m_bUseMediaFile = strcasecmp(e->m_strName.c_str(), "AMR-WB") == 0; if (!rtp->m_bUseMediaFile) rtp->SetMediaFile(""); }
        applyRemoteDtmf(rtp, *pclsRtp);
        if (!early) rtp->ResetRecvStats();
        rtp->Start(pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort);
        if (m_pObserver) m_pObserver->OnPeerCallStart(this, pszCallId, srd);
    }
}

void CsimPeer::EventCallEnd(const char* pszCallId, int iSipStatus) {
    CRtpThread* rtp = nullptr;
    int q850 = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(pszCallId);
        if (it == m_calls.end()) return;
        rtp = it->second.rtp;
        q850 = it->second.q850;
        m_calls.erase(it);
    }
    freeRtp(rtp);
    if (m_pObserver) m_pObserver->OnPeerCallEnd(this, pszCallId, iSipStatus, q850);
}

void CsimPeer::EventReInvite(const char* pszCallId, CSipCallRtp* pclsRemoteRtp, CSipCallRtp* /*pclsLocalRtp*/) {
    // psip 이 200 answer(로컬 SDP 유지)를 낸다 — 여기서는 상대 방향(hold = sendonly/inactive)만 관측한다(RFC 3264 §8.4)
    bool hold = pclsRemoteRtp && (pclsRemoteRtp->m_eDirection == E_RTP_SEND || pclsRemoteRtp->m_eDirection == E_RTP_INACTIVE);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_calls.find(pszCallId);
        if (it == m_calls.end()) return;
        it->second.remoteHold = hold;
        if (it->second.rtp) it->second.rtp->SetHoldPaused(hold);   // 상대가 sendonly/inactive 면 우리는 보내지 않는다
    }
    if (m_pObserver) m_pObserver->OnPeerReInvite(this, pszCallId, hold);
}

void CsimPeer::EventReInviteResponse(const char* pszCallId, int iSipStatus, CSipCallRtp* /*pclsRemoteRtp*/) {
    if (iSipStatus < 200) return;
    if (m_pObserver) m_pObserver->OnPeerReInviteResponse(this, pszCallId, iSipStatus);
}

void CsimPeer::EventPrack(const char* pszCallId, CSipCallRtp* /*pclsRtp*/) {
    if (m_pObserver) m_pObserver->OnPeerPrack(this, pszCallId);
}

void CsimPeer::EventTransferResponse(const char* pszCallId, int iSipStatus) {
    if (iSipStatus < 200) return;
    if (m_pObserver) m_pObserver->OnPeerReferResponse(this, pszCallId, iSipStatus);
}

bool CsimPeer::RecvRequest(int /*iThreadId*/, CSipMessage* pclsMessage) {
    // 상대 BYE/CANCEL 의 Reason(RFC 3326) — Q.850 cause 를 호에 남긴다(EventCallEnd 가 관측자에 전달). 처리는 UA 에 위임.
    if (!pclsMessage->IsMethod(SIP_METHOD_BYE) && !pclsMessage->IsMethod(SIP_METHOD_CANCEL)) return false;
    int q = parseQ850(pclsMessage);
    if (q <= 0) return false;
    std::string callId;
    pclsMessage->GetCallId(callId);
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it != m_calls.end()) it->second.q850 = q;
    return false;
}

bool CsimPeer::RecvResponse(int /*iThreadId*/, CSipMessage* pclsMessage) {
    if (pclsMessage->m_iStatusCode < 200) return false;
    std::string callId;
    pclsMessage->GetCallId(callId);
    if (pclsMessage->m_clsCSeq.m_strMethod == "INVITE" && pclsMessage->m_iStatusCode >= 300) {
        // 발신 실패 최종 응답의 Reason (MGCF 503 + Q.850;cause=34 등)
        int q = parseQ850(pclsMessage);
        if (q > 0) {
            std::lock_guard<std::mutex> lk(m_mtx);
            auto it = m_calls.find(callId);
            if (it != m_calls.end()) it->second.q850 = q;
        }
        return false;
    }
    if (pclsMessage->m_clsCSeq.m_strMethod != "BYE") return false;
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
