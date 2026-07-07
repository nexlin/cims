#include "SimSession.h"
#include "SipUtility.h"
#include "SipMd5.h"
#include "SdpMedia.h"
#include "Log.h"
#include <sstream>
#include <chrono>
#include <cstring>
#include <cctype>
#include <vector>
#include <utility>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

// ─────────────────────────────────────────────
//  XCAP / IdMS HTTP helper (Phase 3 — UE↔CSC)
//   cspsim 은 테스트 단말이므로 psip HttpStack 의존 없이 최소 raw-socket
//   HTTP/1.1 클라이언트(Connection: close, read-to-EOF)로 구현한다.
// ─────────────────────────────────────────────
namespace {

// RFC 3986 unreserved 외 문자는 %XX 로 percent-encode (query 값용)
std::string XcapUrlEncode(const std::string& s) {
    static const char* HEX = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0xF];
        }
    }
    return out;
}

// base64url (RFC 4648 §5, no padding) — PKCE code_challenge 생성용
std::string XcapBase64Url(const unsigned char* data, size_t len) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < len) v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned)data[i + 2];
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        if (i + 1 < len) out += T[(v >> 6) & 0x3F];
        if (i + 2 < len) out += T[v & 0x3F];
    }
    return out;
}

// 간이 JSON 문자열 값 추출: "key":"value"
std::string XcapJsonStr(const std::string& j, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p >= j.size() || j[p] != '"') return "";
    p++;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) { out += j[p + 1]; p += 2; }
        else out += j[p++];
    }
    return out;
}

// http://host:port/path 파싱
bool XcapParseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    std::string u = url;
    size_t s = u.find("://");
    bool https = false;
    if (s != std::string::npos) { https = (u.substr(0, s) == "https"); u = u.substr(s + 3); }
    port = https ? 443 : 80;
    size_t sl = u.find('/');
    std::string hostport = (sl == std::string::npos) ? u : u.substr(0, sl);
    path = (sl == std::string::npos) ? "/" : u.substr(sl);
    size_t c = hostport.find(':');
    if (c != std::string::npos) { host = hostport.substr(0, c); port = atoi(hostport.c_str() + c + 1); }
    else host = hostport;
    return !host.empty() && port > 0;
}

// raw-socket HTTP/1.1 요청. Connection: close + read-to-EOF (chunked 회피).
//   bTls=true 면 OpenSSL TLS (CSC McpttServer 는 cert 존재 시 https — peer 검증 생략, 테스트 단말).
//   반환: 송수신 성공(true) — status code/body/etag 는 out 인자. 연결 실패 시 false.
bool XcapHttp(const std::string& host, int port, bool bTls, const std::string& method,
              const std::string& path,
              const std::vector<std::pair<std::string, std::string> >& headers,
              const std::string& body, const std::string& contentType,
              int& outStatus, std::string& outBody, std::string& outEtag) {
    outStatus = 0; outBody.clear(); outEtag.clear();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he || !he->h_addr) { close(sock); return false; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return false; }

    // TLS 핸드셰이크 (https). peer 인증서 검증은 생략 (테스트 단말, self-signed CSC cert).
    SSL_CTX* ctx = NULL; SSL* ssl = NULL;
    if (bTls) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { close(sock); return false; }
        ssl = SSL_new(ctx);
        if (!ssl) { SSL_CTX_free(ctx); close(sock); return false; }
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, host.c_str());  // SNI
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(sock); return false;
        }
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Connection: close\r\n";
    for (size_t i = 0; i < headers.size(); ++i)
        req += headers[i].first + ": " + headers[i].second + "\r\n";
    if (!body.empty()) {
        req += "Content-Type: " + contentType + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    bool bSendOk = true;
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = bTls ? SSL_write(ssl, req.data() + sent, (int)(req.size() - sent))
                         : send(sock, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { bSendOk = false; break; }
        sent += (size_t)n;
    }

    std::string resp;
    if (bSendOk) {
        char buf[4096];
        while (true) {
            ssize_t n = bTls ? SSL_read(ssl, buf, sizeof(buf))
                            : recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, (size_t)n);
        }
    }

    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (ctx) SSL_CTX_free(ctx);
    close(sock);
    if (resp.empty()) return false;

    // status line: "HTTP/1.1 200 OK"
    size_t sp = resp.find(' ');
    if (sp != std::string::npos) outStatus = atoi(resp.c_str() + sp + 1);

    size_t hdrEnd = resp.find("\r\n\r\n");
    std::string head = (hdrEnd != std::string::npos) ? resp.substr(0, hdrEnd) : resp;
    outBody = (hdrEnd != std::string::npos) ? resp.substr(hdrEnd + 4) : "";

    // ETag 헤더 (case-insensitive) 추출
    std::string lower = head;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = (char)tolower((unsigned char)lower[i]);
    size_t ep = lower.find("\netag:");
    if (ep != std::string::npos) {
        size_t ls = ep + 6;  // past "\netag:"
        size_t le = head.find("\r\n", ls);
        std::string v = head.substr(ls, (le == std::string::npos ? head.size() : le) - ls);
        size_t a = v.find_first_not_of(" \t");
        size_t b = v.find_last_not_of(" \t\r");
        if (a != std::string::npos) outEtag = v.substr(a, b - a + 1);
    }
    return true;
}

}  // namespace

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
    // 3GPP IMS 헤더 — 실제 단말과 동일한 패턴
    m_clsServerInfo.m_strPPreferredIdentity = "<sip:" + m_strUser + "@" + m_strDomain + ">";
    m_clsServerInfo.m_strPAccessNetworkInfo = "3GPP-E-UTRAN-FDD;utran-cell-id-3gpp=0000000000000000";
    // Contact feature tag — PTT: mcptt, VoLTE: mmtel
    if( m_bPttMode ) {
        m_clsServerInfo.m_vecContactFeatureTags = {
            { "+g.3gpp.icsi-ref", "\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\"" },
            { "+g.3gpp.mcptt",    "" },
            { "video",            "" }
        };
    } else {
        m_clsServerInfo.m_vecContactFeatureTags = {
            { "+g.3gpp.icsi-ref", "\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel\"" },
            { "+g.3gpp.smsip",    "" },
            { "video",            "" }
        };
    }

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

    // IdMS auth — REGISTER 전에 먼저 수행 (올바른 순서: IdMS → REGISTER → SUBSCRIBE → ...)
    if (!m_strCscHost.empty() && m_iCscPort > 0) {
        printf("[%d] IdMS auth (pre-REGISTER) → %s:%d\n", m_iId, m_strCscHost.c_str(), m_iCscPort);
        if (!AcquireXcapToken(m_strCscHost, m_iCscPort, m_bCscTls)) {
            printf("[%d] IdMS auth failed — abort\n", m_iId);
            m_stats.iRegFail++;
            return false;
        }
    }

    if (!m_bNoRegister) {
        m_clsUserAgent.InsertRegisterInfo(m_clsServerInfo);
    }

    // SUBSCRIBE/NOTIFY 처리를 위한 stack 콜백 등록 — UserAgent 보다 먼저.
    //   psip 스택은 콜백을 등록 순서로 호출하고 첫 true 에서 멈춘다(SipStackCallBack.hpp).
    //   CSipUserAgent::RecvNotifyRequest 는 dialog 미존재(out-of-dialog) NOTIFY 를 404 로
    //   선소비(return true)하므로, cspsim 의 raw SUBSCRIBE 로 생긴 xcap-diff NOTIFY 가
    //   SimSession::RecvRequest 에 도달하지 못한다. SimSession 을 먼저 등록하면 NOTIFY 를
    //   먼저 받아 HandleNotify(→ XCAP GET) 로 처리하고 return true; 그 외(INVITE 등)는
    //   return false 로 UserAgent(이후 push_back)에 위임된다. (Start 전 등록 → 수신 스레드
    //   기동 전이라 list 동시변경 race 없음.)
    m_clsUserAgent.m_clsSipStack.AddCallBack(this);

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

    if (!m_clsRtpThread.Create()) {
        printf("[%d] RTP thread create error\n", m_iId);
        return false;
    }

    printf("[%d] User=%s started on %s:%d\n",
           m_iId, m_strUser.c_str(), m_strLocalIp.c_str(), m_iLocalPort);
    return true;
}

void SimSession::Stop() {
    // 통화 중이면 먼저 BYE
    if (!m_strInviteId.empty()) {
        m_clsUserAgent.StopCall(m_strInviteId.c_str());
    }
    // 표준 로그아웃: de-affiliate → SUBSCRIBE Expires=0 → REGISTER Expires=0
    Logout();
    // 메시지 전송 완료 대기 (UDP 소켓이 닫히기 전 패킷이 나가야 함)
    usleep(300000);
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
//  reg-event 구독 (RFC 3680) — 실제 UE 는 REGISTER 200 OK 직후
//  자신의 AoR 로 Event: reg SUBSCRIBE 를 보내 등록 상태를 구독한다.
//  Request-URI/From/To 모두 자신의 AoR, body 없음.
// ─────────────────────────────────────────────
void SimSession::SubscribeReg()
{
    const std::string& strLocalIp = m_clsSetup.m_strLocalIp;
    int iLocalPort = m_iLocalPort;

    char szCallId[128];
    snprintf(szCallId, sizeof(szCallId), "regsub_%s_%d_%d",
             m_strUser.c_str(), m_iId, (int)time(NULL));
    m_strRegSubCallId = szCallId;
    m_iRegSubSeq = 1;

    char szTag[64];
    SipMakeTag(szTag, sizeof(szTag));
    m_strRegSubFromTag = szTag;

    CSipMessage* pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "SUBSCRIBE";
    pMsg->m_clsReqUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), m_iServerPort);

    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch(szBranch, sizeof(szBranch));
    pMsg->AddVia(strLocalIp.c_str(), iLocalPort, szBranch);

    pMsg->m_clsFrom.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), 0);
    pMsg->m_clsFrom.InsertParam(SIP_TAG, szTag);
    pMsg->m_clsTo.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), 0);

    pMsg->m_clsCallId.Parse(szCallId, (int)strlen(szCallId));
    pMsg->m_clsCSeq.Set(m_iRegSubSeq, "SUBSCRIBE");
    pMsg->m_iMaxForwards = 70;
    pMsg->AddHeader("Expires", "3600");
    pMsg->AddHeader("Event", "reg");
    pMsg->AddHeader("Accept", "application/reginfo+xml");

    char szContact[128];
    snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>",
             m_strUser.c_str(), strLocalIp.c_str(), iLocalPort);
    pMsg->AddHeader("Contact", szContact);

    pMsg->AddRoute(m_strServerIp.c_str(), m_iServerPort, E_SIP_UDP);

    m_bRegSubscribed = true;
    printf("[%d] SUBSCRIBE reg-event Call-ID=%s\n", m_iId, szCallId);
    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

// ─────────────────────────────────────────────
//  SUBSCRIBE Expires=0 (구독 해제, RFC 3265 §3.1.4)
//  기존 다이얼로그(Call-ID / From-tag)를 재사용해야 서버가 같은 구독으로 인식한다.
// ─────────────────────────────────────────────
void SimSession::SendUnsubscribe(const std::string& strPsi,
                                  const std::string& strCallId,
                                  int& iSeq,
                                  const std::string& strFromTag)
{
    if (strCallId.empty() || strFromTag.empty()) return;  // 구독 없음 — skip

    const std::string& strLocalIp = m_clsSetup.m_strLocalIp;

    CSipMessage* pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "SUBSCRIBE";

    pMsg->m_clsReqUri.Set("sip", strPsi.c_str(), m_strDomain.c_str(), m_iServerPort);

    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch(szBranch, sizeof(szBranch));
    pMsg->AddVia(strLocalIp.c_str(), m_iLocalPort, szBranch);

    pMsg->m_clsFrom.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), 0);
    pMsg->m_clsFrom.InsertParam(SIP_TAG, strFromTag.c_str());

    pMsg->m_clsTo.m_clsUri.Set("sip", strPsi.c_str(), m_strDomain.c_str(), 0);

    pMsg->m_clsCallId.Parse(strCallId.c_str(), (int)strCallId.size());

    pMsg->m_clsCSeq.Set(++iSeq, "SUBSCRIBE");

    pMsg->m_iMaxForwards = 70;
    pMsg->AddHeader("Expires", "0");
    // reg-event 다이얼로그(자신의 AoR 구독)면 Event: reg, 그 외 xcap-diff
    pMsg->AddHeader("Event", strCallId == m_strRegSubCallId ? "reg" : "xcap-diff");

    char szContact[128];
    snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>",
             m_strUser.c_str(), strLocalIp.c_str(), m_iLocalPort);
    pMsg->AddHeader("Contact", szContact);

    pMsg->AddRoute(m_strServerIp.c_str(), m_iServerPort, E_SIP_UDP);

    printf("[%d] UNSUBSCRIBE %s Call-ID=%s CSeq=%d\n", m_iId, strPsi.c_str(), strCallId.c_str(), iSeq);
    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

// ─────────────────────────────────────────────
//  표준 로그아웃 플로우 (실제 단말 대응)
//  순서: de-affiliate PUBLISH → SUBSCRIBE Expires=0(gms) → SUBSCRIBE Expires=0(cms)
//        → REGISTER Expires=0 (DeRegister → psip 내부 스레드가 전송)
// ─────────────────────────────────────────────
void SimSession::Logout()
{
    // 앱 종료 시 XCAP ETag 캐시 초기화 (다음 세션은 새로 GET)
    m_mapXcapEtag.clear();

    // 1. MCPTT 그룹 affiliation 해제
    if (m_bPttMode && !m_strGroupId.empty()) {
        AffiliateGroup(true);
    }

    // 2. GMS / CMS / reg-event 구독 해제 (Expires=0, 기존 다이얼로그 재사용)
    if (m_bGmsSubscribed) {
        SendUnsubscribe("gms_psi", m_strGmsCallId, m_iGmsSeq, m_strGmsFromTag);
        m_bGmsSubscribed = false;
    }
    if (m_bCmsSubscribed) {
        SendUnsubscribe("cms_psi", m_strCmsCallId, m_iCmsSeq, m_strCmsFromTag);
        m_bCmsSubscribed = false;
    }
    if (m_bRegSubscribed) {
        SendUnsubscribe(m_strUser, m_strRegSubCallId, m_iRegSubSeq, m_strRegSubFromTag);
        m_bRegSubscribed = false;
    }

    // 3. REGISTER Expires=0 — m_clsUserAgent.Stop() 내부에서 자동 전송되므로 여기서는 생략
}

// MCPTT 그룹 affiliation (TS 24.379 §9): 그룹 URI 로 SIP PUBLISH 송신(RFC 3903).
//   Content-Type: application/vnd.3gpp.mcptt-affiliation-command+xml.
//   CSP CscfModule::RecvRequestPublish 가 Request-URI 그룹이면 (user,group,client) affiliation 등록.
//   Expires>0=affiliate, Expires:0(또는 body de-affiliate)=해제.
void SimSession::AffiliateGroup(bool bDeaffiliate) {
    if (!m_bPttMode || m_strGroupId.empty()) return;

    const std::string& strLocalIp = m_clsSetup.m_strLocalIp;
    int iLocalPort = m_iLocalPort;

    char szCallId[128];
    snprintf(szCallId, sizeof(szCallId), "aff_%s_%s_%d_%d",
             m_strGroupId.c_str(), m_strUser.c_str(), m_iId, (int)time(NULL));

    char szTag[64];
    SipMakeTag(szTag, sizeof(szTag));

    CSipMessage* pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "PUBLISH";
    // Request-URI: sip:{group}@domain — group id 로 affiliation 대상 지정
    pMsg->m_clsReqUri.Set("sip", m_strGroupId.c_str(), m_strDomain.c_str(), m_iServerPort);

    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch(szBranch, sizeof(szBranch));
    pMsg->AddVia(strLocalIp.c_str(), iLocalPort, szBranch);

    pMsg->m_clsFrom.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), 0);
    pMsg->m_clsFrom.InsertParam(SIP_TAG, szTag);
    pMsg->m_clsTo.m_clsUri.Set("sip", m_strGroupId.c_str(), m_strDomain.c_str(), 0);
    pMsg->m_clsCallId.Parse(szCallId, (int)strlen(szCallId));
    pMsg->m_clsCSeq.Set(1, "PUBLISH");
    pMsg->m_iMaxForwards = 70;
    pMsg->AddHeader("Expires", bDeaffiliate ? "0" : "3600");
    // Event 헤더(RFC 3903 필수). TS 24.379 §9 의 3GPP affiliation event = "mcptt".
    // (F-05: CSP RecvRequestPublish 가 Event != "mcptt" 시 489 Bad Event 로 거절)
    pMsg->AddHeader("Event", "mcptt");

    char szContact[128];
    snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>",
             m_strUser.c_str(), strLocalIp.c_str(), iLocalPort);
    pMsg->AddHeader("Contact", szContact);

    // application/vnd.3gpp.mcptt-affiliation-command+xml 본문
    std::string strBody;
    strBody  = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    strBody += "<mcptt-affiliation-command xmlns=\"urn:3gpp:ns:mcpttAffiliation:1.0\">\r\n";
    strBody += std::string("  <") + (bDeaffiliate ? "de-affiliate" : "affiliate")
             + " group=\"sip:" + m_strGroupId + "@" + m_strDomain + "\"/>\r\n";
    strBody += "</mcptt-affiliation-command>\r\n";
    pMsg->m_clsContentType.Set("application", "vnd.3gpp.mcptt-affiliation-command+xml");
    pMsg->m_strBody = strBody;
    pMsg->m_iContentLength = (int)strBody.size();

    pMsg->AddRoute(m_strServerIp.c_str(), m_iServerPort, E_SIP_UDP);

    printf("[%d] %s group=%s Call-ID=%s\n", m_iId, bDeaffiliate ? "DE-AFFILIATE" : "AFFILIATE",
           m_strGroupId.c_str(), szCallId);
    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
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
    // PTT: SDP에 m=application(floor 수신 포트) 광고
    if (m_bPttMode && m_clsRtpThread.m_iFloorRecvPort > 0)
        clsRtp.m_iApplicationPort = m_clsRtpThread.m_iFloorRecvPort;

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

    // MCPTT 긴급/임박 개시 (TS 24.379): mcptt-info 에 emergency-ind/imminentperil-ind 를 실어야 하므로
    //   UA 일괄 StartCall(바디 주입 불가) 대신 CreateCall→multipart 래핑→StartCall(callId,msg) 경로 사용.
    //   (UA 가 다이얼로그 관리 유지 → 200/ACK/미디어 정상.)
    if (m_iEmergencyCond > 0 || !m_vecAdhoc.empty()) {
        CSipMessage* pInvite = NULL;
        if (m_clsUserAgent.CreateCall(m_strUser.c_str(), strDst.c_str(), &clsRtp, &clsRoute,
                                       m_strInviteId, &pInvite, NULL) && pInvite) {
            // mcptt-info (condition 지시자 포함)
            std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\">\r\n"
                "  <mcptt-Params>\r\n"
                "    <session-type>prearranged</session-type>\r\n";
            if (m_iEmergencyCond >= 2) xml += "    <emergency-ind>true</emergency-ind>\r\n";
            else if (m_iEmergencyCond == 1) xml += "    <imminentperil-ind>true</imminentperil-ind>\r\n";
            xml += "    <mcptt-request-uri>tel:" + strDst + "</mcptt-request-uri>\r\n"
                   "    <mcptt-calling-user-id>tel:" + m_strUser + "</mcptt-calling-user-id>\r\n"
                   "  </mcptt-Params>\r\n"
                   "</mcpttinfo>\r\n";
            // ad hoc: resource-lists (동적 멤버)
            std::string rl;
            if (!m_vecAdhoc.empty()) {
                rl = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                     "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\">\r\n  <list>\r\n";
                for (const auto& m : m_vecAdhoc) rl += "    <entry uri=\"tel:" + m + "\"/>\r\n";
                rl += "  </list>\r\n</resource-lists>\r\n";
            }
            std::string sdp = pInvite->m_strBody;  // CreateCall 이 만든 SDP
            const std::string b = "mcptt";
            std::string body;
            body  = "--" + b + "\r\nContent-Type: application/vnd.3gpp.mcptt-info+xml\r\n";
            body += "Content-Length: " + std::to_string(xml.size()) + "\r\n\r\n" + xml + "\r\n";
            if (!rl.empty()) {
                body += "--" + b + "\r\nContent-Type: application/resource-lists+xml\r\n";
                body += "Content-Length: " + std::to_string(rl.size()) + "\r\n\r\n" + rl + "\r\n";
            }
            body += "--" + b + "\r\nContent-Type: application/sdp\r\n";
            body += "Content-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp + "\r\n";
            body += "--" + b + "--\r\n";
            pInvite->m_strBody = body;
            pInvite->m_iContentLength = (int)body.size();
            pInvite->m_clsContentType.Set("multipart", "mixed");
            pInvite->m_clsContentType.InsertParam("boundary", b.c_str());
            const char* tag = (m_iEmergencyCond >= 2) ? "EMERGENCY" : (m_iEmergencyCond == 1) ? "IMMINENT" : "";
            printf("[%d] INVITE → %s  [%s%s%zu adhoc]\n", m_iId, strDst.c_str(), tag,
                   m_vecAdhoc.empty() ? "" : " adhoc:", m_vecAdhoc.size());
            m_clsUserAgent.StartCall(m_strInviteId.c_str(), pInvite);
            return;
        }
        printf("[%d] CreateCall 실패 — 일반 INVITE 폴백\n", m_iId);
    }

    printf("[%d] INVITE → %s\n", m_iId, strDst.c_str());
    m_clsUserAgent.StartCall(m_strUser.c_str(), strDst.c_str(),
                              &clsRtp, &clsRoute, m_strInviteId);
}

void SimSession::StopCall() {
    if (!m_strInviteId.empty()) {
        // [TEARDOWN-DIAG] establish(200 OK 수신=m_bInCall) 여부 기록.
        //   inCall=0 이면 psip StopCall 이 BYE 대신 CANCEL/no-op → CSP no-BYE 누수 원인 후보.
        printf("[%d] [TD] StopCall callid=%s inCall=%d\n", m_iId, m_strInviteId.c_str(), m_bInCall ? 1 : 0);
        m_clsUserAgent.StopCall(m_strInviteId.c_str());
        m_clsRtpThread.Stop();
        m_strInviteId.clear();
    } else {
        printf("[%d] [TD] StopCall NOOP (no inviteId) inCall=%d\n", m_iId, m_bInCall ? 1 : 0);
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
// TS 24.380 §8.2 subtype: Floor Request=0, Floor Release=4 (CMP/단말 FloorCodec 와 동일).
void SimSession::SendPttRequest()  { if (m_bPttMode) m_clsRtpThread.SendFloorControl(0); }
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

    // 200 OK 를 먼저 응답 — HandleNotify 의 XCAP HTTP GET 이 블로킹이므로
    // NOTIFY 재전송을 막기 위해 응답을 선행한다.
    CSipMessage* pRes = pclsMessage->CreateResponseWithToTag(200);
    if (pRes) m_clsUserAgent.m_clsSipStack.SendSipMessage(pRes);

    HandleNotify(pclsMessage);

    return true;
}

bool SimSession::RecvResponse(int /*iThreadId*/, CSipMessage* pclsMessage) {
    // 발신자(UAC, PTT) 그룹콜 INVITE 200 OK: SDP m=application(SharedFloorPort) 학습.
    //   pclsRtp 에는 application 미파싱이라 SIP body 에서 직접 추출 → floor dest 설정.
    //   (SimSession 콜백이 UserAgent 보다 먼저 등록되어 200 OK 를 먼저 관찰 — return false 로 위임.)
    if (m_bPttMode && pclsMessage->m_clsCSeq.m_strMethod == "INVITE" &&
        pclsMessage->m_iStatusCode / 100 == 2 && !pclsMessage->m_strBody.empty()) {
        size_t pos = pclsMessage->m_strBody.find("m=application ");
        if (pos != std::string::npos) {
            int floorPort = atoi(pclsMessage->m_strBody.c_str() + pos + 14);
            if (floorPort > 0 && m_clsRtpThread.m_iDestFloorPort != floorPort) {
                m_clsRtpThread.m_iDestFloorPort = floorPort;
                printf("[%d] [PTT] Caller floor dest from 200 OK: %d\n", m_iId, floorPort);
            }
        }
        return false;
    }

    // affiliation PUBLISH 응답 검증 (TS 24.379 §9 / RFC 3903) — CSP 멤버십 게이트(item 1)
    //   덕에 비멤버 그룹 제휴는 403 으로 거부됨. 200 OK(SIP-ETag) vs 4xx 를 구분 기록해
    //   affiliation E2E(멤버=200 / 비멤버=403) 를 검증 가능하게 한다.
    if (pclsMessage->m_clsCSeq.m_strMethod == "PUBLISH") {
        int st = pclsMessage->m_iStatusCode;
        if (st / 100 == 2) {
            m_stats.iAffiliateOk++;
            CSipHeader* pEtag = pclsMessage->GetHeader("SIP-ETag");
            printf("[%d] AFFILIATION %d OK group=%s SIP-ETag=%s\n", m_iId, st,
                   m_strGroupId.c_str(), pEtag ? pEtag->m_strValue.c_str() : "");
        } else if (st >= 400) {
            m_stats.iAffiliateRej++;
            printf("[%d] AFFILIATION REJECTED %d group=%s (CSP 멤버십 게이트 거부 가능 — 비멤버 그룹?)\n",
                   m_iId, st, m_strGroupId.c_str());
        }
        return true;
    }

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
        } else if (strCallId == m_strRegSubCallId) {
            printf("[%d] REG-EVENT SUBSCRIBED OK\n", m_iId);
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

    const std::string& strBody = pclsMessage->m_strBody;
    if (strBody.empty()) return;

    // reg-event NOTIFY (reginfo+xml): 등록 상태만 출력 (RFC 3680)
    if (strEvent.rfind("reg", 0) == 0 && strEvent.find("xcap") == std::string::npos) {
        size_t p = strBody.find("<registration ");
        if (p != std::string::npos) {
            size_t sp = strBody.find("state=\"", p);
            std::string strRegState = "?";
            if (sp != std::string::npos) {
                size_t se = strBody.find('"', sp + 7);
                if (se != std::string::npos) strRegState = strBody.substr(sp + 7, se - sp - 7);
            }
            printf("[%d]   reginfo: registration state=%s\n", m_iId, strRegState.c_str());
        }
        return;
    }

    // xcap-root="http://{CSC}:{port}/" 추출 (Phase 3B 교정 결과)
    std::string strXcapRoot;
    {
        size_t p = strBody.find("xcap-root=\"");
        if (p != std::string::npos) {
            size_t e = strBody.find('"', p + 11);
            if (e != std::string::npos) strXcapRoot = strBody.substr(p + 11, e - p - 11);
        }
    }

    // <document new-etag="..." sel="..."/> 를 문서별로 순회
    size_t dp = strBody.find("<document");
    while (dp != std::string::npos) {
        size_t de = strBody.find('>', dp);
        std::string doc = strBody.substr(dp, (de == std::string::npos ? strBody.size() : de) - dp);
        std::string strSel, strEtag;
        {
            size_t p = doc.find("sel=\"");
            if (p != std::string::npos) { size_t e = doc.find('"', p + 5); if (e != std::string::npos) strSel = doc.substr(p + 5, e - p - 5); }
        }
        {
            size_t p = doc.find("new-etag=\"");
            if (p != std::string::npos) { size_t e = doc.find('"', p + 10); if (e != std::string::npos) strEtag = doc.substr(p + 10, e - p - 10); }
        }
        if (!strSel.empty()) {
            printf("[%d]   sel: %s (etag=%s)\n", m_iId, strSel.c_str(), strEtag.c_str());
            // xcap-diff NOTIFY 수신 → 실제 XCAP GET 으로 문서 취득 (Phase 3D)
            if (!m_bNoXcap && !strXcapRoot.empty()) FetchXcapDoc(strXcapRoot, strSel, strEtag);
        }
        dp = strBody.find("<document", dp + 1);
    }
}

// 능동 XCAP 취득 (검증용) — SUBSCRIBE 후 자신의 문서들을 직접 GET.
//   sel 형식은 CSP BuildXcapDiffBody 와 동일 (CMS user-profile/service-config + GMS group).
void SimSession::ProbeXcap(const std::string& strXcapRoot) {
    if (m_bNoXcap || strXcapRoot.empty()) return;
    std::string strUserTel = std::string("tel:") + m_strUser;  // m_strUser 는 +825.. 형식
    printf("[%d] XCAP probe (xcap-root=%s)\n", m_iId, strXcapRoot.c_str());
    FetchXcapDoc(strXcapRoot, "org.3gpp.mcptt.user-profile/users/" + strUserTel + "/user-profile", "");
    FetchXcapDoc(strXcapRoot, "org.3gpp.mcptt.service-config/users/" + strUserTel + "/service-config", "");
    if (!m_strGroupId.empty())
        FetchXcapDoc(strXcapRoot, "org.openmobilealliance.groups/users/" + strUserTel + "/tel:" + m_strGroupId, "");
}

// ─────────────────────────────────────────────
//  CSC-1 토큰 취득 (3GPP TS 33.180 / OAuth2 PKCE) — Phase 3C
//   IdMS 는 XCAP 와 동일 CSC host:port 에서 /idms/* 로 서빙.
//   세션당 1회만 취득해 m_strAccessToken 에 캐시.
// ─────────────────────────────────────────────
bool SimSession::AcquireXcapToken(const std::string& strHost, int iPort, bool bTls) {
    if (!m_strAccessToken.empty()) return true;

    // PKCE: 안정적(결정적) code_verifier → SHA256 → base64url = code_challenge.
    //   서버는 SHA256(verifier)==challenge 만 검증하므로 결정적 생성으로 충분.
    static const char* AB =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string strVerifier;
    for (int i = 0; i < 64; ++i) strVerifier += AB[(m_iId * 7 + i * 13 + 5) % 66];

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)strVerifier.data(), strVerifier.size(), hash);
    std::string strChallenge = XcapBase64Url(hash, SHA256_DIGEST_LENGTH);

    // user_name = IdMS USERS 키 형식 (tel:+<msisdn>)
    std::string strUserUri = m_strUser;
    if (strUserUri.rfind("tel:", 0) != 0)
        strUserUri = std::string("tel:") + (!strUserUri.empty() && strUserUri[0] == '+' ? strUserUri : "+" + strUserUri);

    const std::string strRedirect = "http://localhost/cb";

    // 1) GET /idms/authreq → auth code
    std::string strQuery =
        "/idms/authreq?user_name=" + XcapUrlEncode(strUserUri) +
        "&user_password=" + XcapUrlEncode(m_strPwd) +
        "&client_id=MCPTT_UE&redirect_uri=" + XcapUrlEncode(strRedirect) +
        "&scope=&code_challenge=" + strChallenge + "&code_challenge_method=S256";

    std::vector<std::pair<std::string, std::string> > noHdr;
    int iStatus = 0; std::string strRespBody, strRespEtag;
    if (!XcapHttp(strHost, iPort, bTls, "GET", strQuery, noHdr, "", "", iStatus, strRespBody, strRespEtag) || iStatus != 200) {
        printf("[%d]   XCAP token: authreq failed (status=%d)\n", m_iId, iStatus);
        m_stats.iXcapTokenFail++;
        return false;
    }
    std::string strCode = XcapJsonStr(strRespBody, "code");
    if (strCode.empty()) {
        printf("[%d]   XCAP token: no auth code in authreq response\n", m_iId);
        m_stats.iXcapTokenFail++;
        return false;
    }

    // 2) POST /idms/tokenreq (authorization_code + PKCE verifier) → access_token
    std::string strTokenBody =
        std::string("{\"grant_type\":\"authorization_code\",\"code\":\"") + strCode +
        "\",\"code_verifier\":\"" + strVerifier +
        "\",\"client_id\":\"MCPTT_UE\",\"redirect_uri\":\"" + strRedirect + "\"}";

    iStatus = 0; strRespBody.clear(); strRespEtag.clear();
    if (!XcapHttp(strHost, iPort, bTls, "POST", "/idms/tokenreq", noHdr, strTokenBody, "application/json",
                  iStatus, strRespBody, strRespEtag) || iStatus != 200) {
        printf("[%d]   XCAP token: tokenreq failed (status=%d)\n", m_iId, iStatus);
        m_stats.iXcapTokenFail++;
        return false;
    }
    m_strAccessToken = XcapJsonStr(strRespBody, "access_token");
    if (m_strAccessToken.empty()) {
        printf("[%d]   XCAP token: no access_token in tokenreq response\n", m_iId);
        m_stats.iXcapTokenFail++;
        return false;
    }
    m_stats.iXcapTokenOk++;
    printf("[%d]   XCAP token acquired (len=%zu)\n", m_iId, m_strAccessToken.size());
    return true;
}

// ─────────────────────────────────────────────
//  XCAP 문서 취득 (Phase 3D) — xcap-root + sel 을 GET.
//   200 수신 + etag 있으면 If-None-Match 재요청으로 304 동작도 검증.
// ─────────────────────────────────────────────
void SimSession::FetchXcapDoc(const std::string& strXcapRoot, const std::string& strSel,
                              const std::string& /*strDocEtag*/) {
    std::string strHost, strRootPath; int iPort = 0;
    if (!XcapParseUrl(strXcapRoot, strHost, iPort, strRootPath)) {
        printf("[%d]   XCAP: bad xcap-root '%s'\n", m_iId, strXcapRoot.c_str());
        return;
    }
    bool bTls = (strXcapRoot.rfind("https://", 0) == 0);
    if (!AcquireXcapToken(strHost, iPort, bTls)) return;

    std::string strPath = strRootPath;
    if (strPath.empty() || strPath[strPath.size() - 1] != '/') strPath += '/';
    strPath += strSel;

    std::vector<std::pair<std::string, std::string> > hdrs;
    hdrs.push_back(std::make_pair("Authorization", "Bearer " + m_strAccessToken));

    // 캐시된 ETag 있으면 If-None-Match 조건부 요청 (실제 단말 동작)
    auto itEtag = m_mapXcapEtag.find(strPath);
    if (itEtag != m_mapXcapEtag.end() && !itEtag->second.empty()) {
        hdrs.push_back(std::make_pair("If-None-Match", itEtag->second));
    }

    int iStatus = 0; std::string strBody, strEtag;
    if (!XcapHttp(strHost, iPort, bTls, "GET", strPath, hdrs, "", "", iStatus, strBody, strEtag)) {
        printf("[%d]   XCAP GET %s — connect/recv failed\n", m_iId, strSel.c_str());
        m_stats.iXcapFail++;
        return;
    }

    if (iStatus == 200) {
        m_stats.iXcapOk++;
        printf("[%d]   XCAP GET 200 %s (%zuB, etag=%s)\n", m_iId, strSel.c_str(), strBody.size(), strEtag.c_str());
        if (!strEtag.empty()) m_mapXcapEtag[strPath] = strEtag;
    } else if (iStatus == 304) {
        m_stats.iXcap304++;
        printf("[%d]   XCAP GET 304 (not modified) %s\n", m_iId, strSel.c_str());
    } else {
        m_stats.iXcapFail++;
        printf("[%d]   XCAP GET %d %s\n", m_iId, iStatus, strSel.c_str());
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

    // TS 24.379: 이미 통화 중이면 486 Busy Here (실 단말과 동일)
    if (m_pOwner->m_bInCall) {
        printf("[%d] [PTT] Already in call — reject INVITE with 486 Busy\n", m_pOwner->m_iId);
        m_pUserAgent->StopCall(pszCallId, 486);
        return;
    }

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

        // PTT 200 OK: m=application(floor 수신 포트) 광고
        if (m_pOwner->m_clsRtpThread.m_iFloorRecvPort > 0)
            clsLocalRtp.m_iApplicationPort = m_pOwner->m_clsRtpThread.m_iFloorRecvPort;

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
    // 발신자(UAC, PTT): 200 OK 의 m=application(SharedFloorPort) 을 floor dest 로 학습.
    //   (member 는 INVITE 에서 학습; caller 는 여기 200 OK 에서.) 미지정 시 audio+1 fallback.
    //   이게 없으면 caller floor REQUEST 가 잘못된 포트로 가 CMP 미매칭 → GRANT 안 됨.
    if (m_pOwner->m_bPttMode && pclsRtp) {
        int floorPort = pclsRtp->GetApplicationPort();
        if (floorPort > 0) {
            m_pOwner->m_clsRtpThread.m_iDestFloorPort = floorPort;
            printf("[%d] [PTT] Caller floor dest from 200 OK: %d\n", m_pOwner->m_iId, floorPort);
        }
    }
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
