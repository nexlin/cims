#include "SimSession.h"
#include "Base64.h"
#include "SipUtility.h"
#include "SipMd5.h"
#include "SdpMedia.h"
#include "SdpAttributeCrypto.h"
#include "SipCodecTable.h"
#include "Log.h"
#include <openssl/rand.h>
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
//  협상 오디오 미디어 빌더 (psip CSipCodecTable 공유 — 서버와 동일 기본 테이블)
// ─────────────────────────────────────────────
namespace {

/** 오퍼 미디어 리스트의 audio rtpmap 에서 entry 코덱("<Name>/<Clock>")의 PT 를 찾는다 — 없으면 -1.
 *  RFC 3264: answer 는 오퍼가 선언한 PT 를 그대로 echo 해야 한다 (dynamic PT 는 번호가 임의 계약). */
int FindOfferedPt(CSipCallRtp* pclsOffer, const CSipCodecEntry& clsEntry) {
    if (pclsOffer == NULL) return -1;
    std::string strPrefix = clsEntry.GetMatchPrefix();
    for (SDP_MEDIA_LIST::iterator itM = pclsOffer->m_clsMediaList.begin();
         itM != pclsOffer->m_clsMediaList.end(); ++itM) {
        if (strcasecmp(itM->m_strMedia.c_str(), "audio")) continue;
        for (SDP_ATTRIBUTE_LIST::iterator itA = itM->m_clsAttributeList.begin();
             itA != itM->m_clsAttributeList.end(); ++itA) {
            if (strcasecmp(itA->m_strName.c_str(), "rtpmap")) continue;
            const char* pszSp = strchr(itA->m_strValue.c_str(), ' ');
            if (pszSp == NULL) continue;
            size_t iLen = strPrefix.length();
            if (strncasecmp(pszSp + 1, strPrefix.c_str(), iLen) == 0 &&
                (pszSp[1 + iLen] == '\0' || pszSp[1 + iLen] == '/'))
                return atoi(itA->m_strValue.c_str());
        }
    }
    return -1;
}

/** 협상 audio 미디어 라인 구성 — 코덱 identity(테이블 PT) 로 엔트리를 찾아 rtpmap/fmtp 생성.
 *  answer(pclsOffer 지정) 는 오퍼 PT echo, 오퍼는 테이블 PT 광고. 반환 = wire PT (RTP 스탬핑용).
 *  미디어 SRTP (media_security.md §8.1): strCryptoKey 지정 시 a=crypto 를 병기하고
 *  bSavp 면 protocol 을 RTP/SAVP 로 낸다 (answer 는 오퍼 protocol echo — 호출자 책임). */
int BuildAudioMedia(CSipCallRtp& clsRtp, int iPort, int iCodecPt, CSipCallRtp* pclsOffer,
                    bool bSavp = false, const std::string& strCryptoSuite = "",
                    const std::string& strCryptoKey = "", const std::string& strCryptoTag = "1") {
    const CSipCodecEntry* pclsEntry = CSipCodecTable::FindByPt(iCodecPt);
    if (pclsEntry == NULL) pclsEntry = CSipCodecTable::FindByPt(0);  // 미인식 오퍼 → PCMU 관용 (레거시 동작)
    if (pclsEntry == NULL) pclsEntry = &CSipCodecTable::GetTop();

    int iWirePt = FindOfferedPt(pclsOffer, *pclsEntry);
    if (iWirePt < 0) iWirePt = pclsEntry->m_iPt;

    CSdpMedia clsAudio("audio", iPort, bSavp ? "RTP/SAVP" : "RTP/AVP");
    clsAudio.AddFmt(iWirePt);
    char szVal[192];
    snprintf(szVal, sizeof(szVal), "%d %s", iWirePt, pclsEntry->GetRtpmap().c_str());
    clsAudio.AddAttribute("rtpmap", szVal);
    if (!pclsEntry->m_strFmtp.empty()) {
        snprintf(szVal, sizeof(szVal), "%d %s", iWirePt, pclsEntry->m_strFmtp.c_str());
        clsAudio.AddAttribute("fmtp", szVal);
    }
    if (!strCryptoKey.empty() && !strCryptoSuite.empty()) {
        snprintf(szVal, sizeof(szVal), "%s %s inline:%s", strCryptoTag.c_str(), strCryptoSuite.c_str(),
                 strCryptoKey.c_str());
        clsAudio.AddAttribute("crypto", szVal);
    }
    clsRtp.m_clsMediaList.push_back(clsAudio);
    return iWirePt;
}

/** 미디어 SRTP 자기 송신 키 생성 — base64(key16||salt14). 실패 시 빈 문자열. */
std::string SrtpGenInlineKeyB64() {
    unsigned char arr[30];
    if (RAND_bytes(arr, sizeof(arr)) != 1) return "";
    std::string strOut;
    if (!Base64Encode((const char*)arr, (int)sizeof(arr), strOut)) return "";
    return strOut;
}

/** video 미디어 라인 구성 (H.264 PT 96 고정). 미디어 SRTP 는 audio 와 동일 규약 — a=crypto 는
 *  m-line 단위(RFC 4568 §5)라 비디오는 자기 키를 따로 선언한다. */
void BuildVideoMedia(CSipCallRtp& clsRtp, int iPort, bool bSavp = false, const std::string& strCryptoSuite = "",
                     const std::string& strCryptoKey = "", const std::string& strCryptoTag = "1") {
    CSdpMedia clsVideo("video", iPort, bSavp ? "RTP/SAVP" : "RTP/AVP");
    clsVideo.AddFmt(96);
    clsVideo.AddAttribute("rtpmap", "96 H264/90000");
    clsVideo.AddAttribute("fmtp", "96 profile-level-id=42C016; packetization-mode=1");
    if (!strCryptoKey.empty() && !strCryptoSuite.empty()) {
        char szVal[192];
        snprintf(szVal, sizeof(szVal), "%s %s inline:%s", strCryptoTag.c_str(), strCryptoSuite.c_str(),
                 strCryptoKey.c_str());
        clsVideo.AddAttribute("crypto", szVal);
    }
    clsRtp.m_clsMediaList.push_back(clsVideo);
}

/** 상대 SDP 의 pszMedia 첫 active(port>0) m-line 포트. 없으면 0 (미디어 부재/거절). */
int FindActiveMediaPort(const SDP_MEDIA_LIST& clsList, const char* pszMedia) {
    for (SDP_MEDIA_LIST::const_iterator it = clsList.begin(); it != clsList.end(); ++it) {
        if (strcasecmp(it->m_strMedia.c_str(), pszMedia)) continue;
        if (it->m_iPort <= 0) continue;
        return it->m_iPort;
    }
    return 0;
}

/** 상대 SDP 의 pszMedia active m-line 에서 SDES crypto 를 읽는다 (psip GetSipCallRtp 는 audio 만
 *  CSipCallRtp 필드로 올리므로 video 는 media list 에서 직접). 지원 suite 의 첫 항목 채택.
 *  반환: 0=미디어 부재/비활성, 1=유효 crypto, 2=평문(crypto 없음), -1=SAVP 인데 유효 crypto 없음. */
int ReadMediaCrypto(const SDP_MEDIA_LIST& clsList, const char* pszMedia, std::string& strTag,
                    std::string& strSuite, std::string& strKey, bool& bSavp) {
    strTag.clear(); strSuite.clear(); strKey.clear(); bSavp = false;
    for (SDP_MEDIA_LIST::const_iterator it = clsList.begin(); it != clsList.end(); ++it) {
        if (strcasecmp(it->m_strMedia.c_str(), pszMedia)) continue;
        if (it->m_iPort <= 0) continue;
        bSavp = (strncasecmp(it->m_strProtocol.c_str(), "RTP/SAVP", 8) == 0);
        for (SDP_ATTRIBUTE_LIST::const_iterator itA = it->m_clsAttributeList.begin();
             itA != it->m_clsAttributeList.end(); ++itA) {
            if (strcasecmp(itA->m_strName.c_str(), "crypto")) continue;
            CSdpAttributeCrypto clsCrypto;
            if (clsCrypto.Parse(itA->m_strValue.c_str(), (int)itA->m_strValue.size()) <= 0) continue;
            if (clsCrypto.Empty()) continue;
            if (clsCrypto.m_strCryptoSuite != "AES_CM_128_HMAC_SHA1_80" &&
                clsCrypto.m_strCryptoSuite != "AES_CM_128_HMAC_SHA1_32") continue;
            strTag = clsCrypto.m_strTag;
            strSuite = clsCrypto.m_strCryptoSuite;
            strKey = clsCrypto.m_strKey;
            return 1;
        }
        return bSavp ? -1 : 2;
    }
    return 0;
}

}  // namespace

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
                       const std::string& strHa1,
                       const std::string& strServerIp,
                       int iServerPort,
                       const std::string& strLocalIp,
                       int iLocalPort,
                       bool bPttMode,
                       const std::string& strGroupId)
    : m_iId(id), m_strUser(strUser), m_strAuthId(strAuthId),
      m_strDomain(strDomain), m_strPwd(strPwd), m_strHa1(strHa1),
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
    m_clsServerInfo.m_strHa1         = m_strHa1;
    m_clsServerInfo.m_eTransport     = E_SIP_UDP;  // Start() 에서 m_eTransport 로 확정
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

    // transport 반영 — 등록 목적지와 스택 기동 모드는 한 세트다.
    //   TLS 는 서버 리스너를 열지 않는 **클라이언트 전용**으로 기동한다(m_iLocalTlsPort=0 유지):
    //   psip 이 SSLClientStart + TLS worker pool 을 초기화하고, 서버가 먼저 거는 요청도
    //   이 클라이언트 연결로 수신한다.
    m_clsServerInfo.m_eTransport = m_eTransport;
    if (m_eTransport == E_SIP_TLS) {
        m_clsSetup.m_bTlsClient = true;
    } else if (m_eTransport == E_SIP_TCP) {
        m_clsSetup.m_bTcpClient = true;   // TCP 도 클라이언트 전용 — worker pool 만 기동
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
    // floor 메시지 FF_USER_ID — CMP 가 NAT(포트변환) 환경에서 멤버를 식별하는 근거
    m_clsRtpThread.m_strUserId = m_strUser;

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
    pMsg->AddRoute(m_strServerIp.c_str(), RoutePort(), m_eTransport);

    printf("[%d] SUBSCRIBE %s Call-ID=%s\n", m_iId, strPsi.c_str(), szCallId);
    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

// ─────────────────────────────────────────────
//  conference 구독 (RFC 4575) — 그룹 AoR 로 SUBSCRIBE.
//  Req-URI/To = 그룹, Event: conference, body 없음.
//  CSP 는 200 OK 직후 현재 로스터 스냅샷 NOTIFY 를 보내고, 이후 멤버 변동마다
//  구독 경로로 NOTIFY 한다(구독자 없으면 서버가 in-dialog 폴백).
// ─────────────────────────────────────────────
void SimSession::SubscribeConference(const std::string &strGroupId) {
  const std::string &strLocalIp = m_clsSetup.m_strLocalIp;
  int iLocalPort = m_iLocalPort;

  char szCallId[128];
  snprintf(szCallId, sizeof(szCallId), "confsub_%s_%s_%d_%d",
           strGroupId.c_str(), m_strUser.c_str(), m_iId, (int)time(NULL));
  m_strConfSubGroup = strGroupId;
  m_strConfSubCallId = szCallId;
  m_iConfSubSeq = 1;

  char szTag[64];
  SipMakeTag(szTag, sizeof(szTag));
  m_strConfSubFromTag = szTag;

  CSipMessage *pMsg = new CSipMessage();
  pMsg->m_strSipMethod = "SUBSCRIBE";
  pMsg->m_clsReqUri.Set("sip", strGroupId.c_str(), m_strDomain.c_str(),
                        m_iServerPort);

  char szBranch[SIP_BRANCH_MAX_SIZE];
  SipMakeBranch(szBranch, sizeof(szBranch));
  pMsg->AddVia(strLocalIp.c_str(), iLocalPort, szBranch);

  pMsg->m_clsFrom.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(),
                               0);
  pMsg->m_clsFrom.InsertParam(SIP_TAG, szTag);
  pMsg->m_clsTo.m_clsUri.Set("sip", strGroupId.c_str(), m_strDomain.c_str(), 0);
  pMsg->m_clsCallId.Parse(szCallId, (int)strlen(szCallId));
  pMsg->m_clsCSeq.Set(m_iConfSubSeq, "SUBSCRIBE");
  pMsg->m_iMaxForwards = 70;
  pMsg->AddHeader("Expires", "3600");
  pMsg->AddHeader("Event", "conference");
  pMsg->AddHeader("Accept", "application/conference-info+xml");

  char szContact[128];
  snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>", m_strUser.c_str(),
           strLocalIp.c_str(), iLocalPort);
  pMsg->AddHeader("Contact", szContact);

  pMsg->AddRoute(m_strServerIp.c_str(), RoutePort(), m_eTransport);

  printf("[%d] SUBSCRIBE conference group=%s Call-ID=%s\n", m_iId,
         strGroupId.c_str(), szCallId);
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

    pMsg->AddRoute(m_strServerIp.c_str(), RoutePort(), m_eTransport);

    m_bRegSubscribed = true;
    printf("[%d] SUBSCRIBE reg-event Call-ID=%s\n", m_iId, szCallId);
    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

// ─────────────────────────────────────────────
//  dialog-event SUBSCRIBE (RFC 4235 — 관제 BLF, volte_supplementary_services.md §6.2)
//   Req-URI/To = 감시 대상 AoR, Event: dialog. 서버가 대상의 호 상태를 dialog-info NOTIFY 로 통지.
// ─────────────────────────────────────────────
void SimSession::SubscribeDialog(const std::string& strWatchedAor)
{
    m_strDlgWatchedAor = strWatchedAor;
    m_iDlgSubStatus = 0;
    printf("[%d] SUBSCRIBE dialog watched=%s\n", m_iId, strWatchedAor.c_str());
    SendEventSubscribe("dialog", "application/dialog-info+xml", strWatchedAor,
                       m_strDlgSubCallId, m_iDlgSubSeq, m_strDlgSubFromTag);
}

// 이벤트 패키지 프로브 — Event 토큰 임의 지정. 자원은 보통 자기 AoR (인가 축 무관하게 분류만 본다).
void SimSession::SubscribeEvent(const std::string& strEvent, const std::string& strResourceAor)
{
    m_iEventSubStatus = 0;
    printf("[%d] SUBSCRIBE Event=%s resource=%s\n", m_iId, strEvent.c_str(), strResourceAor.c_str());
    SendEventSubscribe(strEvent, "", strResourceAor, m_strEventSubCallId, m_iEventSubSeq, m_strEventSubFromTag);
}

void SimSession::SendEventSubscribe(const std::string& strEvent, const std::string& strAccept,
                                    const std::string& strResourceAor,
                                    std::string& strCallIdOut, int& iSeqOut, std::string& strFromTagOut)
{
    const std::string& strLocalIp = m_clsSetup.m_strLocalIp;

    char szCallId[128];
    snprintf(szCallId, sizeof(szCallId), "evsub_%s_%s_%d_%d", strEvent.c_str(), m_strUser.c_str(), m_iId,
             (int)time(NULL));
    strCallIdOut = szCallId;
    iSeqOut = 1;

    char szTag[64];
    SipMakeTag(szTag, sizeof(szTag));
    strFromTagOut = szTag;

    CSipMessage* pMsg = new CSipMessage();
    pMsg->m_strSipMethod = "SUBSCRIBE";
    pMsg->m_clsReqUri.Set("sip", strResourceAor.c_str(), m_strDomain.c_str(), m_iServerPort);

    char szBranch[SIP_BRANCH_MAX_SIZE];
    SipMakeBranch(szBranch, sizeof(szBranch));
    pMsg->AddVia(strLocalIp.c_str(), m_iLocalPort, szBranch);

    pMsg->m_clsFrom.m_clsUri.Set("sip", m_strUser.c_str(), m_strDomain.c_str(), 0);
    pMsg->m_clsFrom.InsertParam(SIP_TAG, szTag);
    pMsg->m_clsTo.m_clsUri.Set("sip", strResourceAor.c_str(), m_strDomain.c_str(), 0);

    pMsg->m_clsCallId.Parse(szCallId, (int)strlen(szCallId));
    pMsg->m_clsCSeq.Set(iSeqOut, "SUBSCRIBE");
    pMsg->m_iMaxForwards = 70;
    pMsg->AddHeader("Expires", "3600");
    pMsg->AddHeader("Event", strEvent.c_str());
    if (!strAccept.empty()) pMsg->AddHeader("Accept", strAccept.c_str());

    char szContact[128];
    snprintf(szContact, sizeof(szContact), "<sip:%s@%s:%d>", m_strUser.c_str(), strLocalIp.c_str(), m_iLocalPort);
    pMsg->AddHeader("Contact", szContact);

    pMsg->AddRoute(m_strServerIp.c_str(), RoutePort(), m_eTransport);

    m_clsUserAgent.m_clsSipStack.SendSipMessage(pMsg);
}

// INVITE-with-Replaces(RFC 3891) — 대상 다이얼로그(strReplacesCallId+태그)를 교체한다.
void SimSession::StartCallWithReplaces(const std::string& strTarget, const std::string& strReplacesCallId,
                                       const std::string& strToTag, const std::string& strFromTag)
{
    if (!m_strInviteId.empty() || strReplacesCallId.empty()) return;

    CSipCallRtp clsRtp;
    CSipCallRoute clsRoute;
    clsRtp.m_strIp  = m_clsSetup.m_strLocalIp;
    clsRtp.m_iPort  = m_clsRtpThread.m_iPort;
    clsRtp.m_iCodec = m_clsRtpThread.m_strMediaFile.empty() ? 0 : CSipCodecTable::GetTop().m_iPt;
#ifdef USE_MEDIA_LIST
    m_clsRtpThread.m_iAudioPt = BuildAudioMedia(clsRtp, m_clsRtpThread.m_iPort, clsRtp.m_iCodec, NULL, false, "", "");
#endif
    clsRoute.m_strDestIp  = m_strServerIp;
    clsRoute.m_iDestPort  = RoutePort();
    clsRoute.m_eTransport = m_eTransport;

    CSipMessage* pInvite = NULL;
    if (m_clsUserAgent.CreateCall(m_strUser.c_str(), strTarget.c_str(), &clsRtp, &clsRoute, m_strInviteId, &pInvite,
                                  NULL) && pInvite) {
        // Replaces: <call-id>;to-tag=..;from-tag=.. (RFC 3891). 태그 없으면 Call-ID 만.
        std::string strReplaces = strReplacesCallId;
        if (!strToTag.empty()) strReplaces += ";to-tag=" + strToTag;
        if (!strFromTag.empty()) strReplaces += ";from-tag=" + strFromTag;
        pInvite->AddHeader("Replaces", strReplaces.c_str());
        printf("[%d] INVITE (Replaces=%s) -> %s\n", m_iId, strReplaces.c_str(), strTarget.c_str());
        m_clsUserAgent.StartCall(m_strInviteId.c_str(), pInvite);
        return;
    }
    printf("[%d] StartCallWithReplaces: CreateCall 실패\n", m_iId);
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

    pMsg->AddRoute(m_strServerIp.c_str(), RoutePort(), m_eTransport);

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

    pMsg->AddRoute(m_strServerIp.c_str(), RoutePort(), m_eTransport);

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
    // 미디어 파일 지정 시 서비스 코덱(테이블 최우선 — 기본 AMR-WB 96), 아니면 합성 PCMU(0)
    clsRtp.m_iCodec = m_clsRtpThread.m_strMediaFile.empty() ? 0 : CSipCodecTable::GetTop().m_iPt;
    // PTT: SDP에 m=application(floor 수신 포트) 광고
    if (m_bPttMode && m_clsRtpThread.m_iFloorRecvPort > 0)
        clsRtp.m_iApplicationPort = m_clsRtpThread.m_iFloorRecvPort;

#ifdef USE_MEDIA_LIST
    // 미디어 SRTP 오퍼 (media_security.md §8.1) — required=RTP/SAVP, optional=AVP+a=crypto(best-effort)
    bool bSrtpOffer = (m_iSrtpMode > 0);
    if (bSrtpOffer) {
        m_strSrtpLocalKey = SrtpGenInlineKeyB64();
        if (m_strSrtpLocalKey.empty()) {
            printf("[%d] [SRTP] key generation failed — abort call\n", m_iId);
            return;
        }
    } else {
        m_strSrtpLocalKey.clear();
    }
    // Audio media line — 오퍼러이므로 테이블 PT 로 광고, RTP 송신 PT 도 동일 값으로
    m_clsRtpThread.m_iAudioPt =
        BuildAudioMedia(clsRtp, m_clsRtpThread.m_iPort, clsRtp.m_iCodec, NULL, m_iSrtpMode >= 2,
                        bSrtpOffer ? "AES_CM_128_HMAC_SHA1_80" : "", m_strSrtpLocalKey);
    // Video media line (if video file set) — SRTP 오퍼 시 비디오도 자기 키로 a=crypto (m-line 단위)
    m_strSrtpVideoLocalKey.clear();
    if (m_clsRtpThread.m_iVideoPort > 0) {
        if (bSrtpOffer) {
            m_strSrtpVideoLocalKey = SrtpGenInlineKeyB64();
            if (m_strSrtpVideoLocalKey.empty()) {
                printf("[%d] [SRTP] video key generation failed — abort call\n", m_iId);
                return;
            }
        }
        BuildVideoMedia(clsRtp, m_clsRtpThread.m_iVideoPort, m_iSrtpMode >= 2,
                        bSrtpOffer ? "AES_CM_128_HMAC_SHA1_80" : "", m_strSrtpVideoLocalKey);
    }
#endif

    clsRoute.m_strDestIp  = m_strServerIp;
    clsRoute.m_iDestPort  = RoutePort();  // IPsec 등록 뒤에는 port_ps
    clsRoute.m_eTransport = m_eTransport;  // 등록과 같은 transport 로 발신 (TCP/TLS 호 회귀)

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
//  호 전달·당겨받기 (volte_supplementary_services.md §5·§6)
// ─────────────────────────────────────────────
void SimSession::BlindTransfer(const std::string& strTarget) {
    if (m_strInviteId.empty()) {
        printf("[%d] [XFER] BlindTransfer NOOP — no active call\n", m_iId);
        return;
    }
    printf("[%d] [XFER] REFER (blind) callid=%s -> %s\n", m_iId, m_strInviteId.c_str(), strTarget.c_str());
    m_clsUserAgent.TransferCallBlind(m_strInviteId.c_str(), strTarget.c_str());
}

void SimSession::StartConsultCall(const std::string& strTarget) {
    if (!m_strConsultId.empty()) {
        printf("[%d] [XFER] StartConsultCall NOOP — consult already active\n", m_iId);
        return;
    }
    // 상담 통화는 두 번째 다이얼로그 — 첫 통화(m_strInviteId)와 RtpThread 를 공유한다(발신자는
    //   전달 후 빠지므로 상담 구간의 미디어 방향은 검증 대상이 아니다). VoIP 전용(PTT/긴급 없음).
    CSipCallRtp clsRtp;
    CSipCallRoute clsRoute;
    clsRtp.m_strIp  = m_clsSetup.m_strLocalIp;
    clsRtp.m_iPort  = m_clsRtpThread.m_iPort;
    clsRtp.m_iCodec = m_clsRtpThread.m_strMediaFile.empty() ? 0 : CSipCodecTable::GetTop().m_iPt;
#ifdef USE_MEDIA_LIST
    m_clsRtpThread.m_iAudioPt = BuildAudioMedia(clsRtp, m_clsRtpThread.m_iPort, clsRtp.m_iCodec, NULL, false, "", "");
#endif
    clsRoute.m_strDestIp  = m_strServerIp;
    clsRoute.m_iDestPort  = RoutePort();
    clsRoute.m_eTransport = m_eTransport;
    printf("[%d] INVITE (consult) -> %s\n", m_iId, strTarget.c_str());
    m_clsUserAgent.StartCall(m_strUser.c_str(), strTarget.c_str(), &clsRtp, &clsRoute, m_strConsultId);
}

void SimSession::AttendedTransfer() {
    if (m_strInviteId.empty() || m_strConsultId.empty()) {
        printf("[%d] [XFER] AttendedTransfer NOOP — need both call(%s) + consult(%s)\n", m_iId,
               m_strInviteId.c_str(), m_strConsultId.c_str());
        return;
    }
    printf("[%d] [XFER] REFER (attended) call=%s replaces consult=%s\n", m_iId, m_strInviteId.c_str(),
           m_strConsultId.c_str());
    m_clsUserAgent.TransferCall(m_strInviteId.c_str(), m_strConsultId.c_str());
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

    // dialog-event NOTIFY (RFC 4235, 관제 BLF) — dialog-info+xml 에서 활성 dialog Call-ID/태그 학습.
    if (strEvt.find("dialog") != std::string::npos) {
        m_iDialogNotifyCount++;
        const std::string& body = pclsMessage->m_strBody;
        auto attr = [&](const char* key) -> std::string {
            size_t p = body.find(key);
            if (p == std::string::npos) return "";
            p += strlen(key);
            size_t e = body.find('"', p);
            return (e != std::string::npos) ? body.substr(p, e - p) : "";
        };
        std::string cid = attr("call-id=\"");
        std::string st;
        {
            size_t sp = body.find("<state>");
            if (sp != std::string::npos) {
                size_t se = body.find("</state>", sp);
                if (se != std::string::npos) st = body.substr(sp + 7, se - sp - 7);
            }
        }
        if (!cid.empty()) {
            m_strWatchedDlgCallId = cid;
            m_strWatchedDlgState = st;
            m_strWatchedDlgLocalTag = attr("local-tag=\"");
            m_strWatchedDlgRemoteTag = attr("remote-tag=\"");
        }
        printf("[%d] [BLF] dialog NOTIFY watched=%s state=%s call-id=%s\n", m_iId,
               pclsMessage->m_clsTo.m_clsUri.m_strUser.c_str(), st.c_str(), cid.c_str());
        CSipMessage* pRes = pclsMessage->CreateResponseWithToTag(200);
        if (pRes) m_clsUserAgent.m_clsSipStack.SendSipMessage(pRes);
        return true;
    }

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
        } else if (strCallId == m_strDlgSubCallId) {
            m_iDlgSubStatus = 200;
            printf("[%d] [BLF] dialog SUBSCRIBED OK watched=%s\n", m_iId, m_strDlgWatchedAor.c_str());
        } else if (strCallId == m_strEventSubCallId) {
            m_iEventSubStatus = 200;
            printf("[%d] EVENT-PROBE SUBSCRIBED OK\n", m_iId);
        }
    } else if (iStatus >= 400) {
        // dialog 구독·이벤트 프로브의 최종 응답은 검증 판정값 (403 그룹 밖 감시 / 489 Bad Event)
        if (strCallId == m_strDlgSubCallId) m_iDlgSubStatus = iStatus;
        else if (strCallId == m_strEventSubCallId) m_iEventSubStatus = iStatus;
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

    // IdMS 로그인 자격 — 정본은 users.login_id/passwd(-creds 의 login/loginPw, CSC LOGIN_ACCOUNTS 우선).
    //   없으면 구식 tel:+<msisdn> / SIP 비밀번호 폴백(CSC legacy USERS 경로 — SIP passwd 소거 후엔
    //   "login_id not found" 로 실패하는 게 정상, sip_access_security.md §4.7 ⑤).
    std::string strUserUri = m_strIdmsLogin;
    std::string strLoginPw = m_strIdmsLoginPw;
    if (strUserUri.empty()) {
        strUserUri = m_strUser;
        if (strUserUri.rfind("tel:", 0) != 0)
            strUserUri = std::string("tel:") + (!strUserUri.empty() && strUserUri[0] == '+' ? strUserUri : "+" + strUserUri);
        strLoginPw = m_strPwd;
    }

    const std::string strRedirect = "http://localhost/cb";

    // 1) GET /idms/authreq → auth code
    std::string strQuery =
        "/idms/authreq?user_name=" + XcapUrlEncode(strUserUri) +
        "&user_password=" + XcapUrlEncode(strLoginPw) +
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
        // REGISTER Expires=0 (로그아웃) 의 200 OK — 등록 성공과 별개 이벤트라 통계·경과시간에서 제외
        //   (경과시간은 tRegStart 기준이라 시나리오 길이만큼 커져 "등록 지연"으로 오독된다)
        if (pclsInfo->m_iLoginTimeout == 0) {
            m_pOwner->m_bRegistered = false;
            printf("[%d] DEREGISTERED User=%s\n", m_pOwner->m_iId, pclsInfo->m_strUserId.c_str());
            return;
        }
        m_pOwner->m_bRegistered = true;
        m_pOwner->m_iRoutePort = pclsInfo->m_clsIpsec.ServerPort();  // IPsec 이면 port_ps, 아니면 0
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
    m_pOwner->m_iIncomingInvites++;
    if (pclsMessage) {
        CSipHeader* pclsPcp = pclsMessage->GetHeader("P-Called-Party-ID");
        m_pOwner->m_strLastPCalledParty = pclsPcp ? pclsPcp->m_strValue : "";
    }

    // TS 24.379: 이미 통화 중이면 486 Busy Here (실 단말과 동일)
    if (m_pOwner->m_bInCall) {
        printf("[%d] [PTT] Already in call — reject INVITE with 486 Busy\n", m_pOwner->m_iId);
        m_pUserAgent->StopCall(pszCallId, 486);
        return;
    }

    if (m_pInviteId) *m_pInviteId = pszCallId;

    // 당겨받기 대상(ring-hold): 180 만 보내고 200 은 보류한다 — 다른 단말이 당겨받기 코드로 이
    //   링잉 호를 가져간다(서버 PickUp 이 이 leg 를 StopCall→회수, volte_supplementary_services.md §5).
    if (m_pOwner->m_bRingHold) {
        printf("[%d] [PICKUP] ring-hold — 180 Ringing, 200 보류\n", m_pOwner->m_iId);
        m_pUserAgent->RingCall(pszCallId, 180, NULL);
        return;
    }

    // 미디어 SRTP answer 협상 (media_security.md §8.1) — 오퍼 crypto 존재 && 모드>0 이면
    //   수락(suite/tag echo + 자기 키 선언). SAVP 오퍼인데 수락 불가면 평문 answer 가
    //   성립하지 않으므로 488. answer protocol 은 오퍼 echo (SAVP/AVP+crypto).
    bool bSrtpAnswer = false;
    std::string strSrtpSuite, strSrtpTag = "1";
    m_pOwner->m_strSrtpLocalKey.clear();
    if (pclsRtp && m_pOwner->m_iSrtpMode > 0 && !pclsRtp->m_strRemoteCryptoSuite.empty() &&
        !pclsRtp->m_strRemoteCryptoKey.empty()) {
        m_pOwner->m_strSrtpLocalKey = SrtpGenInlineKeyB64();
        if (!m_pOwner->m_strSrtpLocalKey.empty()) {
            bSrtpAnswer = true;
            strSrtpSuite = pclsRtp->m_strRemoteCryptoSuite;
            if (!pclsRtp->m_strRemoteCryptoTag.empty()) strSrtpTag = pclsRtp->m_strRemoteCryptoTag;
        }
    }
    if (pclsRtp && pclsRtp->m_bRemoteSavp && !bSrtpAnswer) {
        printf("[%d] [SRTP] SAVP offer but srtp mode=off/unusable — 488\n", m_pOwner->m_iId);
        m_pUserAgent->StopCall(pszCallId, 488);
        return;
    }
    if (bSrtpAnswer &&
        !m_pOwner->m_clsRtpThread.SetSrtpKeys(strSrtpSuite, m_pOwner->m_strSrtpLocalKey,
                                              pclsRtp->m_strRemoteCryptoKey)) {
        printf("[%d] [SRTP] session setup failed — 488\n", m_pOwner->m_iId);
        m_pUserAgent->StopCall(pszCallId, 488);
        return;
    }
    if (!bSrtpAnswer) m_pOwner->m_clsRtpThread.ClearSrtp();

    // 비디오 m-line SDES (RFC 4568 §5 — 미디어 단위 키). 오퍼 video 에 crypto 가 있고 모드>0 이면
    //   수락(suite/tag echo + 자기 키), SAVP 인데 수락 불가면 488. 오퍼에 video 가 없거나 평문이면
    //   기존대로 평문 m=video (CSP PTT 는 video 를 X-Video-Port 로만 다룬다).
    bool bVideoSrtpAnswer = false;
    std::string strVideoSuite, strVideoTag = "1", strVideoRemoteKey;
    bool bVideoSavp = false;
    m_pOwner->m_strSrtpVideoLocalKey.clear();
    m_pOwner->m_clsRtpThread.ClearVideoSrtp();
    if (pclsRtp) {
        int iVc = ReadMediaCrypto(pclsRtp->m_clsMediaList, "video", strVideoTag, strVideoSuite,
                                  strVideoRemoteKey, bVideoSavp);
        if (iVc == 1 && m_pOwner->m_iSrtpMode > 0) {
            m_pOwner->m_strSrtpVideoLocalKey = SrtpGenInlineKeyB64();
            bVideoSrtpAnswer = !m_pOwner->m_strSrtpVideoLocalKey.empty();
            if (strVideoTag.empty()) strVideoTag = "1";
        }
        if (bVideoSavp && !bVideoSrtpAnswer) {
            printf("[%d] [SRTP] video SAVP offer but srtp mode=off/unusable — 488\n", m_pOwner->m_iId);
            m_pUserAgent->StopCall(pszCallId, 488);
            return;
        }
        if (bVideoSrtpAnswer && m_pOwner->m_clsRtpThread.m_iVideoPort > 0 &&
            !m_pOwner->m_clsRtpThread.SetVideoSrtpKeys(strVideoSuite, m_pOwner->m_strSrtpVideoLocalKey,
                                                       strVideoRemoteKey)) {
            printf("[%d] [SRTP] video session setup failed — 488\n", m_pOwner->m_iId);
            m_pUserAgent->StopCall(pszCallId, 488);
            return;
        }
        // 비디오 송신 목적지 = 오퍼 m=video 포트 (RFC 3264) — 없으면 PTT X-Video-Port 헤더 폴백(아래)
        m_pOwner->m_clsRtpThread.m_iDestVideoPort = FindActiveMediaPort(pclsRtp->m_clsMediaList, "video");
    }

    // PTT 모드: 180 Ringing → 200 OK 자동응답 (실 단말 동작과 동일)
    if (m_pOwner->m_bPttMode) {
        printf("[%d] [PTT] Group INVITE - sending 180 Ringing\n", m_pOwner->m_iId);
        m_pUserAgent->RingCall(pszCallId, 180, NULL);
        usleep(200000); // 200ms

        CSipCallRtp clsLocalRtp;
        clsLocalRtp.m_strIp  = m_pOwner->m_clsSetup.m_strLocalIp;
        clsLocalRtp.m_iPort  = m_pOwner->m_clsRtpThread.m_iPort;
        clsLocalRtp.m_iCodec = pclsRtp ? pclsRtp->m_iCodec : 0;  // GetSipCallRtp 가 테이블 PT 로 정규화한 identity

#ifdef USE_MEDIA_LIST
        // PTT 200 OK SDP: audio(오퍼 PT echo) + video (비디오 파일이 있는 경우)
        m_pOwner->m_clsRtpThread.m_iAudioPt =
            BuildAudioMedia(clsLocalRtp, m_pOwner->m_clsRtpThread.m_iPort, clsLocalRtp.m_iCodec, pclsRtp,
                            bSrtpAnswer && pclsRtp->m_bRemoteSavp, bSrtpAnswer ? strSrtpSuite : "",
                            m_pOwner->m_strSrtpLocalKey, strSrtpTag);
        if (m_pOwner->m_clsRtpThread.m_iVideoPort > 0)
            BuildVideoMedia(clsLocalRtp, m_pOwner->m_clsRtpThread.m_iVideoPort, bVideoSrtpAnswer && bVideoSavp,
                            bVideoSrtpAnswer ? strVideoSuite : "", m_pOwner->m_strSrtpVideoLocalKey, strVideoTag);
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
            // X-Video-Port 헤더에서 비디오 포트 추출 (SDP m=video 가 없을 때의 PTT 폴백)
            if (pclsMessage && m_pOwner->m_clsRtpThread.m_iDestVideoPort <= 0) {
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
        clsLocalRtp.m_iCodec = pclsRtp ? pclsRtp->m_iCodec : 0;  // GetSipCallRtp 가 테이블 PT 로 정규화한 identity

#ifdef USE_MEDIA_LIST
        // 200 OK SDP에 audio(오퍼 PT echo) + video 미디어 포함
        m_pOwner->m_clsRtpThread.m_iAudioPt =
            BuildAudioMedia(clsLocalRtp, m_pOwner->m_clsRtpThread.m_iPort, clsLocalRtp.m_iCodec, pclsRtp,
                            bSrtpAnswer && pclsRtp->m_bRemoteSavp, bSrtpAnswer ? strSrtpSuite : "",
                            m_pOwner->m_strSrtpLocalKey, strSrtpTag);
        if (m_pOwner->m_clsRtpThread.m_iVideoPort > 0)
            BuildVideoMedia(clsLocalRtp, m_pOwner->m_clsRtpThread.m_iVideoPort, bVideoSrtpAnswer && bVideoSavp,
                            bVideoSrtpAnswer ? strVideoSuite : "", m_pOwner->m_strSrtpVideoLocalKey, strVideoTag);
#endif

        m_pUserAgent->AcceptCall(pszCallId, &clsLocalRtp);
        // 200 OK 후 150ms 대기 → RTP 송출 시작 (CMP 녹취 세그먼트 초반에 SPS/PPS 포함 보장)
        usleep(150000);
        if (pclsRtp) m_pOwner->m_clsRtpThread.Start(pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort);
    }
}

void SessionSipClient::EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) {
    // 미디어 SRTP (media_security.md §8.1) — 오퍼에 키를 실었으면 answer 의 a=crypto 로 세션을
    //   확정한다. RTP 송신은 부모(EventCallStart→RtpThread.Start)에서 시작되므로 그 전에 처리.
    if (!m_pOwner->m_strSrtpLocalKey.empty()) {
        bool bOk = pclsRtp && !pclsRtp->m_strRemoteCryptoKey.empty() &&
                   m_pOwner->m_clsRtpThread.SetSrtpKeys(pclsRtp->m_strRemoteCryptoSuite,
                                                        m_pOwner->m_strSrtpLocalKey,
                                                        pclsRtp->m_strRemoteCryptoKey);
        if (!bOk) {
            if (m_pOwner->m_iSrtpMode >= 2) {
                // required — 평문 폴백 금지: answer 가 crypto 를 안 실었으면 협상 실패로 종료
                printf("[%d] [SRTP] answer without usable crypto (required) — drop call\n", m_pOwner->m_iId);
                m_pUserAgent->StopCall(pszCallId);
                return;
            }
            printf("[%d] [SRTP] answer without crypto — plaintext call (optional)\n", m_pOwner->m_iId);
            m_pOwner->m_clsRtpThread.ClearSrtp();
        }
    } else if (m_pOwner->m_iSrtpMode == 0) {
        m_pOwner->m_clsRtpThread.ClearSrtp();
    }
    // 비디오 m-line answer — 오퍼에 비디오 키를 실었으면 answer video 의 a=crypto 로 확정.
    //   answer 가 video 를 거절/생략(port 0·m-line 없음)하면 비디오 미송신(SRTP 무관). 활성 video 가
    //   crypto 없이 오면 required 는 협상 실패(호 종료), optional 은 평문 비디오.
    if (pclsRtp) {
        int iVideoDest = FindActiveMediaPort(pclsRtp->m_clsMediaList, "video");
        m_pOwner->m_clsRtpThread.m_iDestVideoPort = iVideoDest;
        if (!m_pOwner->m_strSrtpVideoLocalKey.empty() && iVideoDest > 0) {
            std::string strTag, strSuite, strKey;
            bool bSavp = false;
            bool bOk = ReadMediaCrypto(pclsRtp->m_clsMediaList, "video", strTag, strSuite, strKey, bSavp) == 1 &&
                       m_pOwner->m_clsRtpThread.SetVideoSrtpKeys(strSuite, m_pOwner->m_strSrtpVideoLocalKey, strKey);
            if (!bOk) {
                if (m_pOwner->m_iSrtpMode >= 2) {
                    printf("[%d] [SRTP] video answer without usable crypto (required) — drop call\n", m_pOwner->m_iId);
                    m_pUserAgent->StopCall(pszCallId);
                    return;
                }
                printf("[%d] [SRTP] video answer without crypto — plaintext video (optional)\n", m_pOwner->m_iId);
                m_pOwner->m_clsRtpThread.ClearVideoSrtp();
            }
        } else {
            m_pOwner->m_clsRtpThread.ClearVideoSrtp();
        }
    }
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
    m_pOwner->m_iLastCallEndStatus = iSipStatus;
    printf("[%d] CALL ENDED CallId=%s status=%d\n", m_pOwner->m_iId, pszCallId, iSipStatus);
}

void SessionSipClient::EventTransferResponse(const char* pszCallId, int iSipStatus) {
    m_pOwner->m_iReferStatus = iSipStatus;
    printf("[%d] [XFER] REFER response status=%d CallId=%s\n", m_pOwner->m_iId, iSipStatus, pszCallId);
}
