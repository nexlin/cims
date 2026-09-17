#include "SipCapture.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/time.h>

void SipCapture::install(Mode mode) {
    m_mode = mode;
    if (mode == OFF) return;
    CLog::SetLevel(LOG_NETWORK);
    CLog::SetCallBack(this);
}

/** 헤더 이름(대소문자 무시, 축약형 포함)으로 첫 값을 찾는다 — 줄 시작에서만 */
static bool headerValue(const std::string& text, const char* name, const char* shortName, std::string& out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        size_t len = eol - pos;
        if (len <= 1) break;   // 빈 줄 = 헤더 끝
        const char* p = text.c_str() + pos;
        const char* colon = (const char*)memchr(p, ':', len);
        if (colon) {
            size_t n = (size_t)(colon - p);
            while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) --n;
            if ((n == strlen(name) && strncasecmp(p, name, n) == 0) || (n == strlen(shortName) && strncasecmp(p, shortName, n) == 0)) {
                const char* v = colon + 1;
                const char* e = p + len;
                while (v < e && (*v == ' ' || *v == '\t')) ++v;
                while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) --e;
                out.assign(v, (size_t)(e - v));
                return !out.empty();
            }
        }
        pos = eol + 1;
    }
    return false;
}

bool SipCapture::Parse(const char* line, SipCapturedMessage& out, std::string& callId) {
    // "[NETWORK] UdpSend(1.2.3.4:5060) \n[<메시지>]" — 앞의 레벨 머리는 있을 수도 없을 수도
    const char* p = line;
    if (*p == '[') { const char* q = strchr(p, ']'); if (q) { p = q + 1; while (*p == ' ') ++p; } }
    static const struct { const char* tag; const char* transport; bool tx; } kTags[] = {
        { "UdpSend(", "udp", true }, { "UdpRecv(", "udp", false }, { "TcpSend(", "tcp", true },
        { "TcpRecv(", "tcp", false }, { "TlsSend(", "tls", true }, { "TlsRecv(", "tls", false } };
    const char* rest = nullptr;
    for (auto& k : kTags) {
        size_t n = strlen(k.tag);
        if (strncmp(p, k.tag, n) == 0) { out.transport = k.transport; out.tx = k.tx; rest = p + n; break; }
    }
    if (!rest) return false;
    const char* close = strchr(rest, ')');
    if (!close) return false;
    out.peer.assign(rest, (size_t)(close - rest));
    const char* open = strchr(close, '[');
    if (!open) return false;
    const char* body = open + 1;
    const char* end = strrchr(body, ']');
    if (!end || end <= body) end = body + strlen(body);   // 8 KB 에서 잘린 메시지 — 닫는 괄호가 없다
    out.text.assign(body, (size_t)(end - body));
    if (out.text.size() < 12) return false;               // keepalive(CRLF) 등
    return headerValue(out.text, "Call-ID", "i", callId);
}

void SipCapture::Print(EnumLogLevel eLevel, const char* fmt, ...) {
    if (m_mode == OFF || eLevel != LOG_NETWORK) return;
    // CLog 는 완성된 한 줄을 "%s" 로 넘긴다 — 그래도 형식 문자열로 다룬다
    char buf[LOG_MAX_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SipCapturedMessage m;
    std::string callId;
    if (!Parse(buf, m, callId)) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    m.t = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end()) {
        if (m_calls.size() >= kMaxCalls && !m_order.empty()) {
            m_calls.erase(m_order.front());
            m_order.pop_front();
        }
        it = m_calls.emplace(callId, Entry()).first;
        m_order.push_back(callId);
        it->second.lru = std::prev(m_order.end());
    } else {
        m_order.splice(m_order.end(), m_order, it->second.lru);
    }
    Entry& e = it->second;
    if (e.msgs.size() >= kMaxMessagesPerCall || e.bytes + m.text.size() > kMaxBytesPerCall) return;
    e.bytes += m.text.size();
    e.msgs.push_back(std::move(m));
}

std::vector<SipCapturedMessage> SipCapture::take(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end()) return {};
    std::vector<SipCapturedMessage> out = std::move(it->second.msgs);
    m_order.erase(it->second.lru);
    m_calls.erase(it);
    return out;
}

void SipCapture::drop(const std::string& callId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_calls.find(callId);
    if (it == m_calls.end()) return;
    m_order.erase(it->second.lru);
    m_calls.erase(it);
}

size_t SipCapture::calls() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_calls.size();
}
