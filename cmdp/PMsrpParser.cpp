#include "PMsrpParser.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

// ── 유틸 ────────────────────────────────────────────────────────────────────

static bool _iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

static std::string _trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

static bool _isTransIdChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '.' || c == '-' || c == '+' || c == '%' || c == '=';
}

std::string PMsrpMessage::GetHeader(const std::string& name) const {
    for (const auto& kv : headers) {
        if (_iequals(kv.first, name)) return kv.second;
    }
    return "";
}

PMsrpByteRange MsrpParseByteRange(const std::string& value) {
    PMsrpByteRange r;
    // "start-end/total", end 또는 total 은 '*'
    size_t dash = value.find('-');
    size_t slash = value.find('/', dash == std::string::npos ? 0 : dash);
    if (dash == std::string::npos || slash == std::string::npos || dash > slash) return r;
    std::string s1 = _trim(value.substr(0, dash));
    std::string s2 = _trim(value.substr(dash + 1, slash - dash - 1));
    std::string s3 = _trim(value.substr(slash + 1));
    if (s1.empty() || s2.empty() || s3.empty()) return r;
    r.start = atoll(s1.c_str());
    if (r.start <= 0) return r;
    if (s2 == "*") r.endStar = true;
    else {
        r.end = atoll(s2.c_str());
        if (r.end < r.start) return r;
    }
    if (s3 == "*") r.totalStar = true;
    else r.total = atoll(s3.c_str());
    r.valid = true;
    return r;
}

// ── 증분 파서 ───────────────────────────────────────────────────────────────

void PMsrpParser::setError(const std::string& reason) {
    _error = true;
    _errorReason = reason;
}

bool PMsrpParser::next(PMsrpMessage& out) {
    if (_error) return false;

    // 1) 시작줄: "MSRP <trans-id> <method|code [phrase]>\r\n"
    size_t lineEnd = _buf.find("\r\n");
    if (lineEnd == std::string::npos) {
        if (_buf.size() > 4096) setError("start line too long");
        return false;
    }
    std::string startLine = _buf.substr(0, lineEnd);
    if (startLine.compare(0, 5, "MSRP ") != 0) {
        setError("not an MSRP frame");
        return false;
    }
    size_t tidBegin = 5;
    size_t tidEnd = startLine.find(' ', tidBegin);
    if (tidEnd == std::string::npos || tidEnd == tidBegin) {
        setError("missing transaction id");
        return false;
    }
    std::string transId = startLine.substr(tidBegin, tidEnd - tidBegin);
    for (char c : transId) {
        if (!_isTransIdChar(c)) { setError("bad transaction id"); return false; }
    }

    // 2) end-line 탐색: "\n-------<tid><flag>\r\n" (본문 내 다른 tid 의 유사열은 무시)
    std::string marker = "\n-------" + transId;
    size_t markerPos = std::string::npos;
    char flag = 0;
    size_t searchFrom = lineEnd;  // 시작줄 이후부터
    for (;;) {
        size_t p = _buf.find(marker, searchFrom);
        if (p == std::string::npos) break;
        size_t flagPos = p + marker.size();
        if (flagPos + 3 > _buf.size()) return false;  // <flag>\r\n 미도착 — 수신 대기
        char f = _buf[flagPos];
        if ((f == '$' || f == '+' || f == '#') &&
            _buf[flagPos + 1] == '\r' && _buf[flagPos + 2] == '\n' &&
            p > 0 && _buf[p - 1] == '\r') {  // end-line 은 항상 CRLF 뒤에 온다
            markerPos = p;
            flag = f;
            break;
        }
        searchFrom = p + 1;  // 본문 내 유사열 — 다음 후보 탐색
    }
    if (markerPos == std::string::npos) {
        // 프레임 미완 — 폭주 방지 상한 (MaxMessageBytes 는 세션 계층에서 별도 적용)
        if (_buf.size() > (64u << 20)) setError("frame too large");
        return false;
    }

    // 3) 프레임 확정 — [시작줄 이후 .. end-line 직전 CRLF 전) 가 헤더(+본문).
    //    markerPos 는 end-line 직전 CRLF 의 '\n' 위치 — 그 CRLF 는 프레이밍 소속이므로 제외.
    size_t frameEnd = markerPos + marker.size() + 3;  // <flag>\r\n 포함
    size_t innerEnd = markerPos - 1;                  // '\r' 제외 (탐색 시 '\r' 선행 보장)
    std::string inner = _buf.substr(lineEnd + 2, innerEnd - (lineEnd + 2));

    out = PMsrpMessage();
    out.transId = transId;
    out.contFlag = flag;

    // 시작줄 3번째 토큰: 메서드 또는 상태코드
    std::string rest = startLine.substr(tidEnd + 1);
    if (!rest.empty() && rest[0] >= '0' && rest[0] <= '9') {
        out.kind = PMsrpMessage::MSRP_RESPONSE;
        size_t sp = rest.find(' ');
        out.statusCode = atoi(rest.substr(0, sp).c_str());
        if (sp != std::string::npos) out.reason = _trim(rest.substr(sp + 1));
    } else {
        out.kind = PMsrpMessage::MSRP_REQUEST;
        out.method = _trim(rest);
        if (out.method.empty()) { setError("missing method"); return false; }
    }

    // 4) 헤더/본문 분리 — blank line 이 있으면 본문 존재 (framing CRLF 는 3에서 이미 제외됨)
    size_t blank = inner.find("\r\n\r\n");
    std::string headerPart;
    if (blank != std::string::npos) {
        headerPart = inner.substr(0, blank + 2);   // 마지막 헤더의 CRLF 포함
        out.body = inner.substr(blank + 4);
    } else {
        headerPart = inner;  // 본문 없음 — 전부 헤더
    }

    // 5) 헤더 파싱
    size_t pos = 0;
    while (pos < headerPart.size()) {
        size_t eol = headerPart.find("\r\n", pos);
        std::string line = (eol == std::string::npos)
                               ? headerPart.substr(pos)
                               : headerPart.substr(pos, eol - pos);
        pos = (eol == std::string::npos) ? headerPart.size() : eol + 2;
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) { setError("bad header line"); return false; }
        std::string name = _trim(line.substr(0, colon));
        std::string value = _trim(line.substr(colon + 1));
        if (name.empty() || name.size() > 256 || line.size() > 8192) {
            setError("bad header");
            return false;
        }
        out.headers.emplace_back(name, value);
    }

    _buf.erase(0, frameEnd);
    return true;
}

// ── 빌더 ────────────────────────────────────────────────────────────────────

static void _appendEndLine(std::string& s, const std::string& transId, char flag) {
    s += "-------";
    s += transId;
    s += flag;
    s += "\r\n";
}

std::string MsrpBuildResponse(const std::string& transId, int code, const std::string& reason,
                              const std::string& toPath, const std::string& fromPath) {
    std::string s = "MSRP " + transId + " " + std::to_string(code);
    if (!reason.empty()) s += " " + reason;
    s += "\r\nTo-Path: " + toPath + "\r\nFrom-Path: " + fromPath + "\r\n";
    _appendEndLine(s, transId, '$');
    return s;
}

std::string MsrpBuildSendChunk(const std::string& transId, const std::string& toPath,
                               const std::string& fromPath, const std::string& messageId,
                               const std::string& contentType, const std::string& payloadSlice,
                               long long rangeStart, long long rangeEnd, long long total,
                               bool successReport, char flag) {
    std::string s = "MSRP " + transId + " SEND\r\n";
    s += "To-Path: " + toPath + "\r\n";
    s += "From-Path: " + fromPath + "\r\n";
    s += "Message-ID: " + messageId + "\r\n";
    s += "Byte-Range: " + std::to_string(rangeStart) + "-" + std::to_string(rangeEnd) + "/" +
         (total < 0 ? std::string("*") : std::to_string(total)) + "\r\n";
    if (successReport) s += "Success-Report: yes\r\n";
    s += "Failure-Report: yes\r\n";
    if (!contentType.empty()) {
        s += "Content-Type: " + contentType + "\r\n\r\n";
        s += payloadSlice;
        s += "\r\n";
    }
    _appendEndLine(s, transId, flag);
    return s;
}

std::string MsrpBuildReport(const std::string& transId, const std::string& toPath,
                            const std::string& fromPath, const std::string& messageId,
                            long long rangeStart, long long rangeEnd, long long total,
                            int code, const std::string& reason) {
    std::string s = "MSRP " + transId + " REPORT\r\n";
    s += "To-Path: " + toPath + "\r\n";
    s += "From-Path: " + fromPath + "\r\n";
    s += "Message-ID: " + messageId + "\r\n";
    s += "Byte-Range: " + std::to_string(rangeStart) + "-" + std::to_string(rangeEnd) + "/" +
         (total < 0 ? std::string("*") : std::to_string(total)) + "\r\n";
    s += "Status: 000 " + std::to_string(code) + (reason.empty() ? "" : " " + reason) + "\r\n";
    _appendEndLine(s, transId, '$');
    return s;
}

std::string MsrpNewTransId() {
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static bool seeded = false;
    if (!seeded) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        srandom((unsigned)(ts.tv_nsec ^ ts.tv_sec ^ getpid()));
        seeded = true;
    }
    std::string s;
    s.reserve(12);
    for (int i = 0; i < 12; ++i) s += kChars[random() % (sizeof(kChars) - 1)];
    return s;
}

bool MsrpParseUri(const std::string& uri, std::string& host, int& port, std::string& session) {
    // msrp://host:port/session;tcp  (msrps 는 미지원 — TLS 후속)
    static const char kPrefix[] = "msrp://";
    if (uri.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
    size_t hostBegin = sizeof(kPrefix) - 1;
    size_t slash = uri.find('/', hostBegin);
    if (slash == std::string::npos) return false;
    std::string hostPort = uri.substr(hostBegin, slash - hostBegin);
    size_t colon = hostPort.rfind(':');
    if (colon == std::string::npos) {
        host = hostPort;
        port = 2855;
    } else {
        host = hostPort.substr(0, colon);
        port = atoi(hostPort.substr(colon + 1).c_str());
    }
    if (host.empty() || port <= 0 || port > 65535) return false;
    std::string rest = uri.substr(slash + 1);
    size_t semi = rest.find(';');
    session = (semi == std::string::npos) ? rest : rest.substr(0, semi);
    return !session.empty();
}
