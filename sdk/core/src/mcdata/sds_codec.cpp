#include "sds_codec.h"

#include <openssl/evp.h>

#include <cctype>
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

static std::string infoXml(const std::string& groupUri) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\n"
           "  <mcdata-Params>\n"
           "    <request-type>group-sds</request-type>\n"
           "    <mcdata-request-uri type=\"Normal\"><mcdataURI>" + groupUri + "</mcdataURI></mcdata-request-uri>\n"
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

Body buildGroupSds(const std::string& groupUri, const std::string& text, const std::string& convId,
                   const std::string& msgId, bool requestDelivery, int64_t timeSec) {
    std::string boundary = "mcdata-" + msgId.substr(0, 16);
    std::string body;
    appendPart(body, boundary, kCtInfo, nullptr, infoXml(groupUri));
    appendPart(body, boundary, kCtSignalling, "base64", base64Encode(sdsSignallingTlv(convId, msgId, requestDelivery, timeSec)));
    appendPart(body, boundary, kCtPayload, "base64", base64Encode(sdsPayloadTlv(text)));
    body += "--" + boundary + "--\r\n";
    return Body{"multipart/mixed;boundary=" + boundary, body};
}

Body buildNotification(const std::string& convId, const std::string& msgId, int notifType, int64_t timeSec) {
    std::string tlv;
    tlv += (char)kMsgSdsNotification;
    tlv += (char)notifType;
    putDateTime(tlv, timeSec);
    tlv += hexDecode(convId);
    tlv += hexDecode(msgId);
    std::string boundary = "mcdata-ntf-" + msgId.substr(0, 12);
    std::string body;
    appendPart(body, boundary, kCtSignalling, "base64", base64Encode(tlv));
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

bool parse(const std::string& contentType, const std::string& body, SdsMessage& out) {
    std::string boundary = boundaryOf(contentType);
    if (boundary.empty()) {
        size_t nl = body.find_first_of("\r\n");
        std::string first = body.substr(0, nl);
        if (first.rfind("--", 0) == 0) boundary = first.substr(2);
    }
    if (boundary.empty()) return false;
    bool haveSig = false;
    for (auto& p : splitParts(body, boundary)) {
        std::string raw = p.b64 ? base64Decode(p.content) : p.content;
        if (p.ct == kCtInfo) {
            out.groupUri = mcdataUri(raw, "mcdata-request-uri");
        } else if (p.ct == kCtSignalling) {
            if (raw.size() < 38) continue;
            haveSig = true;
            int t = (unsigned char)raw[0] & 0x3F;
            if (t == kMsgSdsSignalling) {
                out.timeSec = readDateTime(raw, 1);
                out.convId = hexEncode(raw.substr(6, 16));
                out.msgId = hexEncode(raw.substr(22, 16));
                size_t i = 38;
                while (i < raw.size()) {
                    int iei = (unsigned char)raw[i];
                    if ((iei & 0xF0) == 0x80) { out.dispositionReq = iei & 0x0F; i += 1; }
                    else if (iei == 0x21) i += 17;                      // InReplyTo message ID
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
                size_t i = 38;
                while (i + 3 <= raw.size()) {
                    int iei = (unsigned char)raw[i];
                    size_t l = ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
                    if (i + 3 + l > raw.size()) break;
                    std::string v = raw.substr(i + 3, l);
                    if (iei == 0x78 && !v.empty()) out.fileUrl = v.substr(1);
                    else if (iei == 0x79) {
                        // name:"x" size:N type:mime
                        size_t n = v.find("name:\""); if (n != std::string::npos) { size_t q = v.find('"', n + 6); if (q != std::string::npos) out.fileName = v.substr(n + 6, q - n - 6); }
                        size_t sz = v.find("size:"); if (sz != std::string::npos) out.fileSize = std::atol(v.c_str() + sz + 5);
                        size_t ty = v.find("type:"); if (ty != std::string::npos) out.fileType = v.substr(ty + 5);
                    }
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
                    int ctype = (unsigned char)raw[i + 3];
                    std::string data = raw.substr(i + 4, l - 1);
                    if (ctype == 0x01) out.text = data;
                    else if (ctype == 0x04) { out.fd = true; out.fileUrl = data; }
                }
                i += 3 + l;
            }
        }
    }
    return haveSig;
}

}  // namespace mcdata
}  // namespace cimsue
