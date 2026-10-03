#include "sds_codec.h"

#include "csc/xml_scan.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace cimsue {
namespace mcdata {

// ── 유틸 ──

static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    size_t i = 0;
    const unsigned char* d = (const unsigned char*)in.data();
    while (i + 2 < in.size()) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = d[i] << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        if (std::isspace((unsigned char)c) || c == '=') continue;
        const char* p = std::strchr(kB64, c);
        if (!p) continue;
        v = (v << 6) | (uint32_t)(p - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((v >> bits) & 0xFF); }
    }
    return out;
}

std::string hexEncode(const std::string& raw) {
    std::string o;
    char b[3];
    for (unsigned char c : raw) { std::snprintf(b, sizeof b, "%02x", c); o += b; }
    return o;
}

std::string hexDecode(const std::string& hex) {
    std::string o;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) o += (char)std::stoi(hex.substr(i, 2), nullptr, 16);
    return o;
}

std::string conversationIdOf(const std::string& groupId) {
    // Java UUID.nameUUIDFromBytes: MD5 → version 3 · IETF variant 비트 세팅.
    std::string name = "cims-mcdata:" + groupId;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(name.data(), name.size(), md, &len, EVP_md5(), nullptr);
    md[6] = (md[6] & 0x0f) | 0x30;
    md[8] = (md[8] & 0x3f) | 0x80;
    return hexEncode(std::string((const char*)md, 16));
}

std::string conversationIdOneToOne(const std::string& a, const std::string& b) {
    // 쌍을 정렬해서 넣는다 — 그러지 않으면 보낸 쪽과 받은 쪽이 다른 conversation ID 를 만들어
    //   같은 대화가 단말마다 둘로 갈라진다(mcdata_messaging.md §4).
    const std::string& lo = a < b ? a : b;
    const std::string& hi = a < b ? b : a;
    std::string name = "cims-mcdata:1to1:" + lo + ":" + hi;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(name.data(), name.size(), md, &len, EVP_md5(), nullptr);
    md[6] = (md[6] & 0x0f) | 0x30;
    md[8] = (md[8] & 0x3f) | 0x80;
    return hexEncode(std::string((const char*)md, 16));
}

std::string newMessageId() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    unsigned char b[16];
    uint64_t a = rng(), c = rng();
    std::memcpy(b, &a, 8); std::memcpy(b + 8, &c, 8);
    b[6] = (b[6] & 0x0f) | 0x40;      // v4
    b[8] = (b[8] & 0x3f) | 0x80;
    return hexEncode(std::string((const char*)b, 16));
}

static void putDateTime(std::string& s, int64_t sec) {           // 5옥텟 big-endian (Kotlin putDateTime 과 동일)
    for (int i = 4; i >= 0; --i) s += (char)((sec >> (8 * i)) & 0xFF);
}
static int64_t readDateTime(const std::string& b, size_t off) {   // 40비트 — long 은 Windows 에서 32비트
    int64_t v = 0;
    for (size_t i = 0; i < 5; ++i) v = (v << 8) | (unsigned char)b[off + i];
    return v;
}

std::string sdsSignallingTlv(const std::string& convId, const std::string& msgId, bool requestDelivery, int64_t timeSec) {
    std::string s;
    s += (char)kMsgSdsSignalling;
    putDateTime(s, timeSec);
    s += hexDecode(convId);
    s += hexDecode(msgId);
    if (requestDelivery) s += (char)(0x80 | kDispReqDelivery);   // TV type1, IEI=8-
    return s;
}

std::string sdsPayloadTlv(const std::string& text) {
    std::string s;
    s += (char)kMsgDataPayload;
    s += (char)1;                                   // Number of payloads
    s += (char)0x78;                                // Payload IEI (TLV-E)
    size_t l = 1 + text.size();
    s += (char)((l >> 8) & 0xFF); s += (char)(l & 0xFF);
    s += (char)0x01;                                // TEXT
    s += text;
    return s;
}

std::string fileSelector(const FdFile& file) {
    // file-selector-attr = "file-selector" ":" selector *(SP selector)(RFC 5547 §6) — 이름은 filename-string: NUL·CR·LF·'"'·'%' 만
    //   퍼센트 인코딩(그 밖 바이트는 그대로 — UTF-8 이름 포함). type = MIME, hash = sha-1(FdFile.hash 가 있을 때).
    static const char* hx = "0123456789ABCDEF";
    std::string name;
    for (unsigned char c : file.name) {
        if (c == 0 || c == '\r' || c == '\n' || c == '"' || c == '%') { name += '%'; name += hx[c >> 4]; name += hx[c & 0xF]; }
        else name += (char)c;
    }
    std::string s = "file-selector:name:\"" + name + "\" size:" + std::to_string(file.size) + " type:" +
                    (file.type.empty() ? std::string("application/octet-stream") : file.type);
    if (!file.hash.empty()) s += " hash:sha-1:" + file.hash;
    return s;
}

void parseFileSelector(const std::string& meta, std::string& name, int64_t& size, std::string& type) {
    // Metadata = file-selector · file-date · file-availability · file-description 를 이은 것(§15.2.17) — file-selector 의 선택자만 읽는다.
    //   접두가 없는 «선택자만» 형식도 같은 규칙으로 읽는다(선택자 이름이 같다).
    size_t i = meta.find("file-selector:");
    i = i == std::string::npos ? 0 : i + 14;
    auto hexv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; };
    while (i < meta.size()) {
        while (i < meta.size() && meta[i] == ' ') ++i;
        if (meta.compare(i, 6, "name:\"") == 0) {
            const size_t q = meta.find('"', i + 6);
            if (q == std::string::npos) return;
            name.clear();
            for (size_t k = i + 6; k < q; ++k) {
                if (meta[k] == '%' && k + 2 < q && hexv(meta[k + 1]) >= 0 && hexv(meta[k + 2]) >= 0) {
                    name += (char)(hexv(meta[k + 1]) * 16 + hexv(meta[k + 2])); k += 2;
                } else name += meta[k];
            }
            i = q + 1;
            continue;
        }
        size_t e = meta.find(' ', i);
        if (e == std::string::npos) e = meta.size();
        const std::string tok = meta.substr(i, e - i);
        if (tok.compare(0, 5, "size:") == 0) size = std::atoll(tok.c_str() + 5);
        else if (tok.compare(0, 5, "type:") == 0) type = tok.substr(5);
        else if (tok.compare(0, 5, "hash:") != 0) return;          // 다음 성분(file-date: 등) — file-selector 끝
        i = e;
    }
}

std::string fdSignallingTlv(const std::string& convId, const std::string& msgId, const FdFile& file, int64_t timeSec) {
    // FD SIGNALLING PAYLOAD(§15.1.3) = 유형·Date-time·ConvID·MsgID + Payload IE 0x78(TLV-E, content-type FILEURL 0x04 + URL)
    //   + Metadata IE 0x79(TLV-E, §15.2.17 — RFC 5547 file-selector-attr).
    const std::string meta = fileSelector(file);
    std::string s;
    s += (char)kMsgFdSignalling;
    putDateTime(s, timeSec);
    s += hexDecode(convId);
    s += hexDecode(msgId);
    size_t l = 1 + file.url.size();
    s += (char)0x78;
    s += (char)((l >> 8) & 0xFF); s += (char)(l & 0xFF);
    s += (char)0x04;                                // FILEURL
    s += file.url;
    s += (char)0x79;
    s += (char)((meta.size() >> 8) & 0xFF); s += (char)(meta.size() & 0xFF);
    s += meta;
    return s;
}

// mcdata-info 발신 본문(Annex D) — request-type · <mcdata-request-uri>(그룹 요청만 — 1:1 대상은 resource-lists) · <mcdata-client-id>(그룹 요청만)
static std::string infoXml(const std::string& requestType, const std::string& uri, const std::string& clientId = std::string()) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\n"
           "  <mcdata-Params>\n"
           "    <request-type>" + requestType + "</request-type>\n" +
           (uri.empty() ? std::string()
                        : "    <mcdata-request-uri type=\"Normal\"><mcdataURI>" + xmlscan::esc(uri) + "</mcdataURI></mcdata-request-uri>\n") +
           (clientId.empty() ? std::string()
                             : "    <mcdata-client-id type=\"Normal\"><mcdataString>" + xmlscan::esc(clientId) +
                                   "</mcdataString></mcdata-client-id>\n") +
           "  </mcdata-Params>\n"
           "</mcdatainfo>";
}

std::string groupSdsInfo(const std::string& groupUri, const std::string& clientId) { return infoXml("group-sds", groupUri, clientId); }

std::string fdUploadInfo(const std::string& groupUri, const std::string& callingUserId) {
    // TS 24.282 §10.2.2.1 5)·6) — request-type(1:1 one-to-one-fd · 그룹 group-fd) · <mcdata-request-uri>(그룹만) · <mcdata-calling-user-id>.
    //   요소 순서 = mcdata-ParamsType(Annex D.1): request-type → mcdata-request-uri → mcdata-calling-user-id
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\n"
           "  <mcdata-Params>\n"
           "    <request-type>" + std::string(groupUri.empty() ? "one-to-one-fd" : "group-fd") + "</request-type>\n" +
           (groupUri.empty() ? std::string()
                             : "    <mcdata-request-uri type=\"Normal\"><mcdataURI>" + xmlscan::esc(groupUri) + "</mcdataURI></mcdata-request-uri>\n") +
           (callingUserId.empty() ? std::string()
                                  : "    <mcdata-calling-user-id type=\"Normal\"><mcdataURI>" + xmlscan::esc(callingUserId) +
                                        "</mcdataURI></mcdata-calling-user-id>\n") +
           "  </mcdata-Params>\n"
           "</mcdatainfo>";
}

static void appendPart(std::string& b, const std::string& boundary, const std::string& ct, const char* cte,
                       const std::string& content) {
    b += "--" + boundary + "\r\n";
    b += "Content-Type: " + ct + "\r\n";
    if (cte) b += std::string("Content-Transfer-Encoding: ") + cte + "\r\n";
    b += "\r\n";
    b += content;
    b += "\r\n";
}

// 대상 MCData ID 하나 — resource-lists entry(RFC 4826 · RFC 5366 recipient-list). 1:1 SDS·FD 의 대상(§9.2.2.2.1 2)a) · §10.2.4.2.1 2)a))와
//   통지 대상(§12.2.1.1 3))이 같은 모양이다.
static void appendRecipient(std::string& b, const std::string& boundary, const std::string& uri) {
    b += "--" + boundary + "\r\n";
    b += std::string("Content-Type: ") + kCtResourceLists + "\r\n";
    b += "Content-Disposition: recipient-list\r\n\r\n";
    b += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\">\n"
         "  <list>\n"
         "    <entry uri=\"" + xmlscan::esc(uri) + "\"/>\n"
         "  </list>\n"
         "</resource-lists>\r\n";
}

// 그룹과 1:1 은 **대상 표기만** 다르다 — 그룹 = mcdata-info <mcdata-request-uri>·<mcdata-client-id>(TS 24.282 §9.2.2.2.1 3)b) ·
//   §10.2.4.2.1 3)b)), 1:1 = resource-lists entry(2)a)) 와 request-type 만 실은 mcdata-info(2)b)). 서명·payload 파트는 같다.
static Body buildSds(const char* requestType, const std::string& groupUri, const std::string& peerUri, const std::string& text,
                     const std::string& convId, const std::string& msgId, bool requestDelivery, int64_t timeSec,
                     const std::string& clientId = std::string()) {
    std::string boundary = "mcdata-" + msgId.substr(0, 16);
    std::string body;
    appendPart(body, boundary, kCtInfo, nullptr, infoXml(requestType, groupUri, clientId));
    appendPart(body, boundary, kCtSignalling, "base64", base64Encode(sdsSignallingTlv(convId, msgId, requestDelivery, timeSec)));
    appendPart(body, boundary, kCtPayload, "base64", base64Encode(sdsPayloadTlv(text)));
    if (!peerUri.empty()) appendRecipient(body, boundary, peerUri);
    body += "--" + boundary + "--\r\n";
    return Body{"multipart/mixed;boundary=" + boundary, body};
}

Body buildGroupSds(const std::string& groupUri, const std::string& text, const std::string& convId,
                   const std::string& msgId, bool requestDelivery, int64_t timeSec, const std::string& clientId) {
    return buildSds("group-sds", groupUri, std::string(), text, convId, msgId, requestDelivery, timeSec, clientId);
}

Body buildOneToOneSds(const std::string& peerUri, const std::string& text, const std::string& convId,
                      const std::string& msgId, bool requestDelivery, int64_t timeSec) {
    return buildSds("one-to-one-sds", std::string(), peerUri, text, convId, msgId, requestDelivery, timeSec);
}

static Body buildFd(const char* requestType, const std::string& groupUri, const std::string& peerUri, const FdFile& file,
                    const std::string& convId, const std::string& msgId, int64_t timeSec,
                    const std::string& clientId = std::string()) {
    std::string boundary = "mcdata-fd-" + msgId.substr(0, 14);
    std::string body;
    appendPart(body, boundary, kCtInfo, nullptr, infoXml(requestType, groupUri, clientId));
    appendPart(body, boundary, kCtSignalling, "base64", base64Encode(fdSignallingTlv(convId, msgId, file, timeSec)));
    if (!peerUri.empty()) appendRecipient(body, boundary, peerUri);
    body += "--" + boundary + "--\r\n";
    return Body{"multipart/mixed;boundary=" + boundary, body};
}

Body buildGroupFd(const std::string& groupUri, const FdFile& file, const std::string& convId,
                  const std::string& msgId, int64_t timeSec, const std::string& clientId) {
    return buildFd("group-fd", groupUri, std::string(), file, convId, msgId, timeSec, clientId);
}

Body buildOneToOneFd(const std::string& peerUri, const FdFile& file, const std::string& convId,
                     const std::string& msgId, int64_t timeSec) {
    return buildFd("one-to-one-fd", std::string(), peerUri, file, convId, msgId, timeSec);
}

Body buildNotification(const std::string& convId, const std::string& msgId, int notifType, int64_t timeSec,
                       const std::string& targetUri, const std::string& groupUri) {
    std::string tlv;
    tlv += (char)kMsgSdsNotification;
    tlv += (char)notifType;
    putDateTime(tlv, timeSec);
    tlv += hexDecode(convId);
    tlv += hexDecode(msgId);
    std::string boundary = "mcdata-ntf-" + msgId.substr(0, 12);
    std::string body;
    // §12.2.1.1 5) 그룹 데이터 요청에 대한 통지면 mcdata-info <mcdata-calling-group-id>
    if (!targetUri.empty() && !groupUri.empty())
        appendPart(body, boundary, kCtInfo, nullptr,
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\n"
                   "  <mcdata-Params>\n"
                   "    <mcdata-calling-group-id type=\"Normal\"><mcdataURI>" + xmlscan::esc(groupUri) +
                   "</mcdataURI></mcdata-calling-group-id>\n"
                   "  </mcdata-Params>\n"
                   "</mcdatainfo>");
    appendPart(body, boundary, kCtSignalling, "base64", base64Encode(tlv));                          // 6) SDS NOTIFICATION
    if (!targetUri.empty()) appendRecipient(body, boundary, targetUri);                              // 3) 통지 대상 MCData ID
    body += "--" + boundary + "--\r\n";
    return Body{"multipart/mixed;boundary=" + boundary, body};
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

static std::string boundaryOf(const std::string& ct) {
    std::string l = lower(ct);
    size_t p = l.find("boundary=");
    if (p == std::string::npos) return std::string();
    std::string v = ct.substr(p + 9);
    if (!v.empty() && v[0] == '"') { size_t q = v.find('"', 1); return q == std::string::npos ? std::string() : v.substr(1, q - 1); }
    size_t e = v.find_first_of(";\r\n ");
    return e == std::string::npos ? v : v.substr(0, e);
}

struct Part { std::string ct; bool b64; std::string content; };

static std::vector<Part> splitParts(const std::string& body, const std::string& boundary) {
    std::vector<Part> out;
    std::string delim = "--" + boundary;
    size_t pos = body.find(delim);
    while (pos != std::string::npos) {
        size_t start = pos + delim.size();
        if (body.compare(start, 2, "--") == 0) break;
        size_t next = body.find(delim, start);
        std::string chunk = body.substr(start, next == std::string::npos ? std::string::npos : next - start);
        if (chunk.rfind("\r\n", 0) == 0) chunk = chunk.substr(2); else if (chunk.rfind("\n", 0) == 0) chunk = chunk.substr(1);
        size_t sep = chunk.find("\r\n\r\n"); size_t sepLen = 4;
        if (sep == std::string::npos) { sep = chunk.find("\n\n"); sepLen = 2; }
        if (sep != std::string::npos) {
            std::string hdrs = lower(chunk.substr(0, sep));
            Part p;
            size_t c = hdrs.find("content-type:");
            if (c != std::string::npos) {
                std::string v = hdrs.substr(c + 13);
                size_t b = v.find_first_not_of(" \t"); size_t e = v.find_first_of(";\r\n");
                p.ct = v.substr(b, e == std::string::npos ? std::string::npos : e - b);
                p.b64 = hdrs.find("content-transfer-encoding: base64") != std::string::npos ||
                        hdrs.find("content-transfer-encoding:base64") != std::string::npos;
                p.content = chunk.substr(sep + sepLen);
                while (!p.content.empty() && (p.content.back() == '\r' || p.content.back() == '\n')) p.content.pop_back();
                out.push_back(p);
            }
        }
        pos = next;
    }
    return out;
}

static std::string mcdataUri(const std::string& xml, const std::string& elem) {
    size_t p = xml.find("<" + elem);
    if (p == std::string::npos) return std::string();
    size_t u = xml.find("<mcdataURI>", p);
    if (u == std::string::npos) return std::string();
    size_t e = xml.find("</mcdataURI>", u);
    if (e == std::string::npos) return std::string();
    std::string v = xml.substr(u + 11, e - u - 11);
    size_t b = v.find_first_not_of(" \t\r\n"), t = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, t - b + 1);
}

static std::string elemText(const std::string& xml, const std::string& elem) {
    size_t p = xml.find("<" + elem + ">");
    if (p == std::string::npos) return std::string();
    p += elem.size() + 2;
    size_t e = xml.find("</" + elem + ">", p);
    if (e == std::string::npos) return std::string();
    std::string v = xml.substr(p, e - p);
    size_t b = v.find_first_not_of(" \t\r\n"), t = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, t - b + 1);
}

namespace {

void appendUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

/** UTF-16 → UTF-8. bigEndian 은 BOM 이 없을 때의 바이트 순서(RFC 2781 §4.3 — 없으면 big-endian). */
std::string utf16ToUtf8(const std::string& in, bool bigEndian, bool allowBom) {
    size_t i = 0;
    if (allowBom && in.size() >= 2) {
        const unsigned char a = (unsigned char)in[0], b = (unsigned char)in[1];
        if (a == 0xFE && b == 0xFF) { bigEndian = true; i = 2; }
        else if (a == 0xFF && b == 0xFE) { bigEndian = false; i = 2; }
    }
    std::string o;
    auto unit = [&](size_t k) -> uint32_t {
        const unsigned char a = (unsigned char)in[k], b = (unsigned char)in[k + 1];
        return bigEndian ? (uint32_t)(a << 8 | b) : (uint32_t)(b << 8 | a);
    };
    for (; i + 1 < in.size(); i += 2) {
        uint32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < in.size()) {
            const uint32_t lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { appendUtf8(o, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)); i += 2; continue; }
        }
        appendUtf8(o, u);
    }
    return o;
}

/** CODED TEXT 의 글 — IANA MIBenum(§15.2.13 표 15.2.13-3)으로 UTF-8 로. 모르는 집합이면 빈 값(원문은 SdsPayload.data). */
std::string decodeCharset(int mib, const std::string& in) {
    switch (mib) {
        case 106: case 3: return in;                                  // UTF-8 · US-ASCII
        case 4: { std::string o; for (unsigned char c : in) appendUtf8(o, c); return o; }   // ISO-8859-1
        case 1013: return utf16ToUtf8(in, true, false);               // UTF-16BE
        case 1014: return utf16ToUtf8(in, false, false);              // UTF-16LE
        case 1015: return utf16ToUtf8(in, true, true);                // UTF-16(BOM, 없으면 BE)
        default: return std::string();
    }
}

}  // namespace

SdsPayload decodePayload(int contentType, const std::string& data) {
    SdsPayload p;
    p.type = contentType;
    p.data = data;
    switch (contentType) {
        case kSdsPayloadText: case kSdsPayloadHyperlinks: case kSdsPayloadFileUrl: p.text = data; break;
        case kSdsPayloadCodedText:
            // 앞 2 octet = 문자 집합 MIBenum(양의 정수, 망 바이트 순서) — §6.2.2.1 3)a)ii)
            if (data.size() >= 2) {
                p.charset = ((unsigned char)data[0] << 8) | (unsigned char)data[1];
                p.data = data.substr(2);
                p.text = decodeCharset(p.charset, p.data);
            }
            break;
        case kSdsPayloadLocation:
            // 6 octet = 위도 3 · 경도 3(TS 23.032 §6.1)
            if (data.size() == 6) {
                auto u = [&](size_t k) { return (uint32_t)(unsigned char)data[k]; };
                const uint32_t latN = ((u(0) & 0x7F) << 16) | (u(1) << 8) | u(2);
                int32_t lonN = (int32_t)((u(3) << 16) | (u(4) << 8) | u(5));
                if (lonN & 0x800000) lonN -= 0x1000000;                // 24비트 2의 보수
                p.latitude = latN * 90.0 / 8388608.0 * ((u(0) & 0x80) ? -1 : 1);
                p.longitude = lonN * 360.0 / 16777216.0;
                p.hasLocation = true;
            }
            break;
        case kSdsPayloadLocationTimestamp:
            // 첫 octet = 길이, 나머지 = "yyyy-mm-dd hh:mm:ss.fffff"(ISO 8601)
            if (!data.empty()) p.text = data.substr(1, std::min<size_t>((unsigned char)data[0], data.size() - 1));
            break;
        default: break;
    }
    return p;
}

bool parse(const std::string& contentType, const std::string& body, SdsMessage& out) {
    bool forApplication = false;
    return parse(contentType, body, out, forApplication);
}

bool parse(const std::string& contentType, const std::string& body, SdsMessage& out, bool& forApplication) {
    forApplication = false;
    out.text.clear();                                    // payload 를 이어 붙이므로 받은 구조체의 앞 값을 지운다
    out.payloads.clear();
    std::string boundary = boundaryOf(contentType);
    if (boundary.empty()) {
        size_t nl = body.find_first_of("\r\n");
        std::string first = body.substr(0, nl);
        if (first.rfind("--", 0) == 0) boundary = first.substr(2);
    }
    if (boundary.empty()) return false;
    bool haveSig = false;
    std::string callingGroup;
    for (auto& p : splitParts(body, boundary)) {
        std::string raw = p.b64 ? base64Decode(p.content) : p.content;
        if (p.ct == kCtInfo) {
            // 1:1(one-to-one-sds/-fd)의 request-uri 는 받는 사람(나)이다 — 그룹으로 오인하면 내 번호 스레드가 생긴다.
            //   request-type 이 없으면(옛 발신자) request-uri 를 그룹으로 본다.
            if (elemText(raw, "request-type").rfind("one-to-one", 0) != 0) out.groupUri = mcdataUri(raw, "mcdata-request-uri");
            // 보낸 사용자·그룹의 정본(TS 24.282 §12.2.1.1 — disposition 통지가 이 둘로 대상을 정한다)
            callingGroup = mcdataUri(raw, "mcdata-calling-group-id");
            if (!callingGroup.empty()) out.groupUri = callingGroup;
            out.fromUri = mcdataUri(raw, "mcdata-calling-user-id");                // 없으면 호출자가 From 으로 채운다
        } else if (p.ct == kCtSignalling) {
            if (raw.size() < 38) continue;
            haveSig = true;
            int t = (unsigned char)raw[0] & 0x3F;
            if (t == kMsgSdsSignalling) {
                out.timeSec = readDateTime(raw, 1);
                out.convId = hexEncode(raw.substr(6, 16));
                out.msgId = hexEncode(raw.substr(22, 16));
                // 선택 IE(표 15.1.2.1-1 순서): InReplyTo 0x21(TV 17) · Application ID 0x22(TV 2) · disposition 요청 0x8-(TV 1) ·
                //   Extended application ID 0x7D · User location 0x7E · Sender MCData user ID 0x51 · Application metadata container 0x53
                //   (TLV-E — 길이 2 octet). 모르는 IE 에서 멈춘다(길이를 알 수 없다).
                size_t i = 38;
                while (i < raw.size()) {
                    int iei = (unsigned char)raw[i];
                    if ((iei & 0xF0) == 0x80) { out.dispositionReq = iei & 0x0F; i += 1; }
                    else if (iei == 0x21) i += 17;                      // InReplyTo message ID
                    else if (iei == 0x22) { forApplication = true; i += 2; }   // Application ID — 응용 대상(§9.2.1.2 7))
                    else if (iei == 0x7D || iei == 0x7E || iei == 0x51 || iei == 0x53) {
                        if (i + 3 > raw.size()) break;
                        size_t l = ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
                        if (i + 3 + l > raw.size()) break;
                        if (iei == 0x7D) forApplication = true;         // Extended application ID — 응용 대상(§9.2.1.2 8))
                        i += 3 + l;
                    }
                    else break;
                }
            } else if (t == kMsgSdsNotification) {
                if (raw.size() < 39) continue;
                out.notification = true;
                out.notifType = (unsigned char)raw[1];
                out.timeSec = readDateTime(raw, 2);
                out.convId = hexEncode(raw.substr(7, 16));
                out.msgId = hexEncode(raw.substr(23, 16));
            } else if (t == kMsgFdSignalling) {
                out.fd = true;
                out.timeSec = readDateTime(raw, 1);
                out.convId = hexEncode(raw.substr(6, 16));
                out.msgId = hexEncode(raw.substr(22, 16));
                // 선택 IE(§15.1.3): FD disposition 요청 0x9x·mandatory download 0xAx(TV 1) · InReplyTo 0x21(TV 17) ·
                //   Application ID 0x22(TV 2) · Payload 0x78 / Metadata 0x79(TLV-E). 모르는 IE 에서 멈춘다(길이를 알 수 없다).
                size_t i = 38;
                while (i < raw.size()) {
                    int iei = (unsigned char)raw[i];
                    if ((iei & 0xF0) == 0x90 || (iei & 0xF0) == 0xA0) { i += 1; continue; }
                    if (iei == 0x21) { i += 17; continue; }
                    if (iei == 0x22) { forApplication = true; i += 2; continue; }   // 응용 대상 파일(§10.2.1.2 — SDS 와 같은 규칙)
                    if ((iei != 0x78 && iei != 0x79) || i + 3 > raw.size()) break;
                    size_t l = ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
                    if (i + 3 + l > raw.size()) break;
                    std::string v = raw.substr(i + 3, l);
                    if (iei == 0x78 && !v.empty() && (unsigned char)v[0] == 0x04) out.fileUrl = v.substr(1);   // FILEURL
                    else if (iei == 0x79) parseFileSelector(v, out.fileName, out.fileSize, out.fileType);   // §15.2.17
                    i += 3 + l;
                }
            }
        } else if (p.ct == kCtPayload) {
            if (raw.size() < 6 || ((unsigned char)raw[0] & 0x3F) != kMsgDataPayload) continue;
            size_t i = 2;
            while (i + 3 <= raw.size()) {
                int iei = (unsigned char)raw[i];
                size_t l = ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
                if (i + 3 + l > raw.size()) break;
                if (iei == 0x78 && l >= 1) {
                    // Payload content type(표 15.2.13-2)마다 해석해 차례대로 싣는다. 글(TEXT·HYPERLINKS·CODED TEXT)은 text 에도 줄을 바꿔
                    //   잇는다(§9.2.1.2 6)d) «render the contents of the Payload IE(s)»). FILEURL = 파일.
                    SdsPayload pl = decodePayload((unsigned char)raw[i + 3], raw.substr(i + 4, l - 1));
                    if ((pl.type == kSdsPayloadText || pl.type == kSdsPayloadHyperlinks || pl.type == kSdsPayloadCodedText) && !pl.text.empty())
                        out.text += (out.text.empty() ? "" : "\n") + pl.text;
                    else if (pl.type == kSdsPayloadFileUrl) { out.fd = true; out.fileUrl = pl.data; }
                    out.payloads.push_back(std::move(pl));
                }
                i += 3 + l;
            }
        }
    }
    // 중계된 disposition 통지의 request-uri 는 통지 대상(나)이다(§12.2.3 14)) — 그룹은 <mcdata-calling-group-id> 뿐이다
    if (out.notification) out.groupUri = callingGroup;
    return haveSig;
}

}  // namespace mcdata
}  // namespace cimsue
