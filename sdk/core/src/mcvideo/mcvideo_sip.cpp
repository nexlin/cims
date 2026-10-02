#include "mcvideo_sip.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include "../csc/xml_scan.h"
#include "tc_defs.h"

namespace cimsue {
namespace mcvideo {

std::string contactFeatureParams() { return std::string(";") + kFeatureTag + ";+g.3gpp.icsi-ref=\"" + kIcsiEnc + "\""; }
std::string acceptContactFeature() { return std::string("*;") + kFeatureTag + ";require;explicit"; }
std::string acceptContactIcsi() { return std::string("*;+g.3gpp.icsi-ref=\"") + kIcsiEnc + "\";require;explicit"; }

// ── mcvideo-info (Annex F.1) ──

static std::string uriElem(const char* tag, const std::string& v) {
    return std::string("    <") + tag + " type=\"Normal\"><mcvideoURI>" + xmlscan::esc(v) + "</mcvideoURI></" + tag + ">\r\n";
}
static std::string strElem(const char* tag, const std::string& v) {
    return std::string("    <") + tag + " type=\"Normal\"><mcvideoString>" + xmlscan::esc(v) + "</mcvideoString></" + tag + ">\r\n";
}

std::string info(const InfoParams& p) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    s += std::string("<mcvideoinfo xmlns=\"") + kNsInfo + "\">\r\n  <mcvideo-Params>\r\n";
    if (!p.accessToken.empty()) s += strElem("mcvideo-access-token", p.accessToken);
    if (!p.sessionType.empty()) s += "    <session-type>" + xmlscan::esc(p.sessionType) + "</session-type>\r\n";
    if (!p.requestUri.empty()) s += uriElem("mcvideo-request-uri", p.requestUri);
    if (!p.callingUserId.empty()) s += uriElem("mcvideo-calling-user-id", p.callingUserId);
    if (!p.calledPartyId.empty()) s += uriElem("mcvideo-called-party-id", p.calledPartyId);
    if (!p.callingGroupId.empty()) s += uriElem("mcvideo-calling-group-id", p.callingGroupId);
    if (!p.clientId.empty()) s += strElem("mcvideo-client-id", p.clientId);
    s += "  </mcvideo-Params>\r\n</mcvideoinfo>";
    return s;
}

/** mcvideo-info 문서 부분 — 루트 <mcvideoinfo>(스키마 이름)·<mcvideo-info>(본문 설명 표기 — mcvideo.md §9) 둘 다. */
static std::string infoDoc(const std::string& s) {
    for (const char* root : {"mcvideoinfo", "mcvideo-info"}) {
        xmlscan::Elem e = xmlscan::elem(s, root);
        if (e.found) return e.inner;
    }
    return std::string();
}

static int indicatorOf(const std::string& doc, const char* local) {
    bool found = false;
    std::string v = xmlscan::elemText(doc, local, &found);
    if (!found) return 0;
    // contentType 요소면 <mcvideoBoolean> 자식 — elemText 는 자식 태그까지 돌려주므로 한 번 더 벗긴다
    if (v.find('<') != std::string::npos) v = xmlscan::elemText(v, "mcvideoBoolean");
    return xmlscan::isTrue(v) ? 1 : -1;
}

/** contentType 요소(type 속성 + mcvideoURI/String 자식) 또는 평문 요소의 값. */
static std::string valueOf(const std::string& doc, const char* local) {
    std::string v = xmlscan::elemText(doc, local);
    if (v.find('<') == std::string::npos) return v;
    for (const char* child : {"mcvideoURI", "mcvideoString"}) {
        bool found = false;
        std::string c = xmlscan::elemText(v, child, &found);
        if (found) return c;
    }
    return std::string();
}

InfoRx parseInfo(const std::string& s) {
    InfoRx r;
    std::string doc = infoDoc(s);
    if (doc.empty()) return r;
    r.present = true;
    r.sessionType = xmlscan::elemText(doc, "session-type");
    r.requestUri = valueOf(doc, "mcvideo-request-uri");
    r.callingUserId = valueOf(doc, "mcvideo-calling-user-id");
    r.calledPartyId = valueOf(doc, "mcvideo-called-party-id");
    r.callingGroupId = valueOf(doc, "mcvideo-calling-group-id");
    r.clientId = valueOf(doc, "mcvideo-client-id");
    r.emergency = indicatorOf(doc, "emergency-ind");
    r.imminentPeril = indicatorOf(doc, "imminentperil-ind");
    return r;
}

// ── affiliation pidf (§8.3.1) ──

std::string pocSettings(const std::string& entityId, bool autoAnswer, int userProfileIndex, bool multiplexSupport) {
    // 확장 요소는 XML 스키마(표 7.4.1.2.2-2 — targetNamespace urn:3gpp:mcsSettings:1.0, qualified)의 이름공간에 둔다.
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") +
           "<poc-settings xmlns=\"urn:oma:params:xml:ns:poc:poc-settings\" xmlns:mcs10Set=\"urn:3gpp:mcsSettings:1.0\">" +
           "<entity id=\"" + xmlscan::esc(entityId) + "\">" +
           "<am-settings><answer-mode>" + (autoAnswer ? "automatic" : "manual") + "</answer-mode></am-settings>" +
           "<mcs10Set:selected-user-profile-index><mcs10Set:user-profile-index>" + std::to_string(userProfileIndex) +
           "</mcs10Set:user-profile-index></mcs10Set:selected-user-profile-index>" +
           "<mcs10Set:multiplex-support>" + (multiplexSupport ? "true" : "false") + "</mcs10Set:multiplex-support>" +
           "</entity></poc-settings>";
}

std::string affiliationPidf(const std::string& entity, const std::string& clientId, const std::vector<std::string>& groupUris,
                            const std::string& pid) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    s += std::string("<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" xmlns:mcvideoPI10=\"") + kNsPresInfo + "\"\r\n";
    s += "  entity=\"" + xmlscan::esc(entity) + "\">\r\n";
    s += "  <tuple id=\"" + xmlscan::esc(clientId) + "\">\r\n    <status>\r\n";
    for (const auto& g : groupUris) s += "      <mcvideoPI10:affiliation group=\"" + xmlscan::esc(g) + "\"/>\r\n";
    s += "    </status>\r\n  </tuple>\r\n";
    s += "  <mcvideoPI10:p-id>" + xmlscan::esc(pid) + "</mcvideoPI10:p-id>\r\n</presence>";
    return s;
}

// ── 제어 채널 SDP (TS 24.581 §4.3.3.1 · §14) ──

std::string fmtpString(const TcFmtp& f) {
    std::string s;
    auto add = [&s](const std::string& p) { if (!s.empty()) s += kFmtpSeparator; s += p; };
    if (f.queueing) add(fmtp::QUEUEING);
    if (f.priority >= 0) add(std::string(fmtp::PRIORITY) + "=" + std::to_string(f.priority));
    if (f.receptionPriority >= 0) add(std::string(fmtp::RECEPTION_PRIORITY) + "=" + std::to_string(f.receptionPriority));
    if (f.granted) add(fmtp::GRANTED);
    if (f.implicitRequest) add(fmtp::IMPLICIT_REQUEST);
    if (f.hasAudioSsrc) add(std::string(fmtp::AUDIO_SSRC) + "=" + std::to_string(f.audioSsrc));
    if (f.hasVideoSsrc) add(std::string(fmtp::VIDEO_SSRC) + "=" + std::to_string(f.videoSsrc));
    if (f.hasTcSsrc) add(std::string(fmtp::TRANSMISSION_SSRC) + "=" + std::to_string(f.tcSsrc));
    return s;
}

TcFmtp answerFmtp(const TcFmtp& offer, uint32_t localTcSsrc, bool queueingSupported) {
    TcFmtp a;
    a.queueing = offer.queueing && queueingSupported;
    a.priority = offer.priority;
    a.receptionPriority = offer.receptionPriority;
    a.hasTcSsrc = offer.hasTcSsrc;
    a.tcSsrc = localTcSsrc;
    return a;
}

std::string controlSdp(int port, const TcFmtp& f) {
    std::string s = "m=application " + std::to_string(port) + " " + kSdpProto + " " + kSdpFmt;
    std::string p = fmtpString(f);
    if (!p.empty()) s += std::string("\r\na=fmtp:") + kSdpFmt + " " + p;
    return s;
}

/** SDP 줄 목록(CRLF·LF 모두). */
static std::vector<std::string> lines(const std::string& sdp) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p < sdp.size()) {
        size_t e = sdp.find('\n', p);
        std::string ln = sdp.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        v.push_back(ln);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return v;
}

static bool isControlMline(const std::string& ln) {
    if (ln.rfind("m=application ", 0) != 0) return false;
    // m=application <port> <proto> <fmt> — proto 는 대소문자 무시(수신 관대), fmt = MCVideo
    size_t a = ln.find(' ', 14);
    if (a == std::string::npos) return false;
    size_t b = ln.find(' ', a + 1);
    if (b == std::string::npos) return false;
    std::string proto = ln.substr(a + 1, b - a - 1), fmt = ln.substr(b + 1);
    for (auto& ch : proto) ch = (char)std::tolower((unsigned char)ch);
    while (!fmt.empty() && fmt.back() == ' ') fmt.pop_back();
    return proto == kSdpProto && fmt == kSdpFmt;
}

bool isMcVideoSdp(const std::string& sdp) {
    for (const auto& ln : lines(sdp)) if (isControlMline(ln)) return true;
    return false;
}

static void parseFmtpParams(const std::string& params, TcFmtp& f) {
    // 구분자 ';'(K4), ABNF 의 ':' 도 받는다(수신 관대 — 값에 ':' 가 없다)
    size_t p = 0;
    while (p <= params.size()) {
        size_t e = params.find_first_of(";:", p);
        if (e == std::string::npos) e = params.size();
        std::string item = xmlscan::trim(params.substr(p, e - p));
        p = e + 1;
        if (item.empty()) continue;
        size_t eq = item.find('=');
        std::string name = item.substr(0, eq), val = eq == std::string::npos ? std::string() : item.substr(eq + 1);
        const unsigned long ul = std::strtoul(val.c_str(), nullptr, 10);
        if (name == fmtp::QUEUEING) f.queueing = true;
        else if (name == fmtp::GRANTED) f.granted = true;
        else if (name == fmtp::IMPLICIT_REQUEST) f.implicitRequest = true;
        else if (name == fmtp::PRIORITY && !val.empty()) f.priority = (int)ul;
        else if (name == fmtp::RECEPTION_PRIORITY && !val.empty()) f.receptionPriority = (int)ul;
        else if (name == fmtp::AUDIO_SSRC && !val.empty()) { f.hasAudioSsrc = true; f.audioSsrc = (uint32_t)ul; }
        else if (name == fmtp::VIDEO_SSRC && !val.empty()) { f.hasVideoSsrc = true; f.videoSsrc = (uint32_t)ul; }
        else if (name == fmtp::TRANSMISSION_SSRC && !val.empty()) { f.hasTcSsrc = true; f.tcSsrc = (uint32_t)ul; }
    }
}

bool parseControl(const std::string& sdp, std::string& ip, int& port, TcFmtp& f) {
    f = TcFmtp{};
    std::string sessionConn, mediaConn;
    bool anyMedia = false, inControl = false, found = false;
    auto connOf = [](const std::string& ln) { return ln.rfind("c=IN IP4 ", 0) == 0 ? xmlscan::trim(ln.substr(9)) : std::string(); };
    for (const auto& ln : lines(sdp)) {
        if (ln.rfind("m=", 0) == 0) {
            if (found) break;                                // 제어 섹션이 끝났다
            anyMedia = true;
            inControl = isControlMline(ln);
            if (inControl) { found = true; port = std::atoi(ln.c_str() + 14); }
            continue;
        }
        if (!anyMedia && ln.rfind("c=", 0) == 0) { sessionConn = connOf(ln); continue; }
        if (!inControl) continue;
        if (ln.rfind("c=", 0) == 0) mediaConn = connOf(ln);
        std::string pre = std::string("a=fmtp:") + kSdpFmt;
        if (ln.rfind(pre, 0) == 0) { f.present = true; parseFmtpParams(ln.substr(pre.size()), f); }
    }
    if (!found) return false;
    ip = mediaConn.empty() ? sessionConn : mediaConn;
    return port > 0 && !ip.empty();
}

std::string withMediaInfo(const std::string& sdp) {
    if (!isMcVideoSdp(sdp)) return sdp;
    const std::string eol = sdp.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> in = lines(sdp);
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].empty() && i + 1 == in.size()) break;      // 끝 줄바꿈이 만든 빈 줄
        out += in[i] + eol;
        const char* info = in[i].rfind("m=audio ", 0) == 0 ? kAudioInfo : in[i].rfind("m=video ", 0) == 0 ? kVideoInfo : nullptr;
        if (info && !(i + 1 < in.size() && in[i + 1].rfind("i=", 0) == 0)) out += std::string("i=") + info + eol;
    }
    return out;
}

std::string forSubsequentOffer(const std::string& sdp) {
    if (!isMcVideoSdp(sdp)) return sdp;
    const std::string eol = sdp.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string pre = std::string("a=fmtp:") + kSdpFmt + " ";
    std::vector<std::string> in = lines(sdp);
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].empty() && i + 1 == in.size()) break;      // 끝 줄바꿈이 만든 빈 줄
        std::string ln = in[i];
        if (ln.rfind(pre, 0) == 0) {
            std::string kept;
            const std::string params = ln.substr(pre.size());
            for (size_t b = 0; b <= params.size();) {                // 구분자 `;`(K4) — 해석 쪽처럼 `:` 도 받는다
                size_t e = params.find_first_of(";:", b);
                if (e == std::string::npos) e = params.size();
                const std::string p = params.substr(b, e - b);
                if (!p.empty() && p != fmtp::GRANTED && p != fmtp::IMPLICIT_REQUEST) kept += (kept.empty() ? "" : kFmtpSeparator) + p;
                b = e + 1;
            }
            ln = pre + kept;
        }
        out += ln + eol;
    }
    return out;
}

static bool ieqPrefix(const std::string& line, const char* name) {
    size_t n = std::char_traits<char>::length(name);
    if (line.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)name[i])) return false;
    return true;
}

std::string withMediaInfoMultipart(const std::string& text) {
    size_t d0 = text.rfind("--", 0) == 0 ? 0 : text.find("\n--");
    if (d0 == std::string::npos) return text;
    if (d0 != 0) ++d0;
    const size_t e0 = text.find_first_of("\r\n", d0);
    if (e0 == std::string::npos) return text;
    const std::string delim = text.substr(d0, e0 - d0);                 // "--<boundary>"
    std::string out = text.substr(0, d0);
    size_t pos = d0;
    for (;;) {
        if (text.compare(pos, delim.size() + 2, delim + "--") == 0) { out += text.substr(pos); break; }   // 끝 구분자
        const size_t lineEnd = text.find('\n', pos);
        const size_t next = lineEnd == std::string::npos ? std::string::npos : text.find("\r\n" + delim, lineEnd + 1);
        if (next == std::string::npos) { out += text.substr(pos); break; }
        out += text.substr(pos, lineEnd + 1 - pos);                      // 구분자 줄
        std::string part = text.substr(lineEnd + 1, next - lineEnd - 1);   // 파트 헤더 CRLFCRLF 본문(구분자 앞 CRLF 제외)
        const size_t hb = part.find("\r\n\r\n");
        if (hb != std::string::npos) {
            std::vector<std::string> hdrs = lines(part.substr(0, hb));
            bool isSdp = false;
            for (const auto& h : hdrs)
                if (ieqPrefix(h, "Content-Type:") && xmlscan::trim(h.substr(13)).rfind("application/sdp", 0) == 0) isSdp = true;
            if (isSdp) {
                const std::string body = mcvideo::withMediaInfo(part.substr(hb + 4));
                std::string h2;
                for (const auto& h : hdrs)
                    h2 += (ieqPrefix(h, "Content-Length:") ? "Content-Length: " + std::to_string(body.size()) : h) + "\r\n";
                part = h2 + "\r\n" + body;
            }
        }
        out += part;
        pos = next + 2;                                                   // 구분자 앞 CRLF 는 다음 반복이 붙인다
        out += "\r\n";
    }
    return out;
}

std::string contactUriPart(const std::string& contact) {
    size_t a = contact.find('<'), b = contact.find('>', a == std::string::npos ? 0 : a);
    if (a == std::string::npos || b == std::string::npos) return contact;
    return contact.substr(a, b - a + 1);
}

std::string withoutIcsiRef(const std::string& params, std::vector<std::string>* icsis) {
    const std::string tag = "+g.3gpp.icsi-ref=\"";
    size_t p = params.find(tag);
    if (p == std::string::npos) return params;
    size_t e = params.find('"', p + tag.size());
    if (e == std::string::npos) return params;
    const size_t cut = p > 0 && params[p - 1] == ';' ? p - 1 : p;      // 앞의 ';' 도 함께 뺀다
    if (icsis) {
        std::string list = params.substr(p + tag.size(), e - p - tag.size());
        size_t q = 0;
        while (q <= list.size()) {
            size_t c = list.find(',', q);
            if (c == std::string::npos) c = list.size();
            std::string v = xmlscan::trim(list.substr(q, c - q));
            if (!v.empty()) icsis->push_back(v);
            q = c + 1;
        }
    }
    return params.substr(0, cut) + params.substr(e + 1);
}

}  // namespace mcvideo
}  // namespace cimsue
