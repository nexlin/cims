// libcimsue — MCData SDS media plane(MSRP) — msrp.h 참조.
#include "mcdata/msrp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <random>

#include "mcdata/sds_codec.h"
#include "net/tls_stream.h"

namespace cimsue {
namespace msrp {

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string randHex(size_t n) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* h = "0123456789abcdef";
    std::string s;
    while (s.size() < n) { uint64_t v = rng(); for (int i = 0; i < 16 && s.size() < n; ++i) { s += h[v & 15]; v >>= 4; } }
    return s;
}

/** net::Stream 위의 프레임 수신기 — 한 스레드가 읽고 쓴다. */
class Conn {
public:
    bool open(const std::string& path, int timeoutSec, std::string& err) {
        std::string host; int port = 0;
        if (!parsePath(path, host, port)) { err = "bad path " + path; return false; }
        net::TlsOptions o; o.tls = false;
        return s_.open(host, port, timeoutSec, o, err);
    }
    bool send(const std::string& d) { return s_.writeAll(d); }
    /** 완성 프레임 하나 — 시한·취소·끊김이면 false. */
    bool recv(Frame& f, int timeoutMs, const std::atomic<bool>& cancel) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char tmp[16384];
        for (;;) {
            if (extractFrame(buf_, f)) return true;
            if (cancel) return false;
            long left = (long)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            int w = s_.waitReadable(left < 200 ? (int)left : 200);          // 취소를 200 ms 안에 본다
            if (w < 0) return false;
            if (w == 0) continue;
            int n = s_.read(tmp, sizeof(tmp));
            if (n <= 0) return false;
            buf_.append(tmp, (size_t)n);
        }
    }
    void close() { s_.close(); }

private:
    net::Stream s_;
    std::string buf_;
};

/** Byte-Range "s-e/total" 의 s(1 기준). 없으면 1. */
size_t rangeStart(const std::string& br) {
    size_t v = std::strtoul(br.c_str(), nullptr, 10);
    return v >= 1 ? v : 1;
}

}  // namespace

std::string Frame::header(const char* name) const {
    auto it = headers.find(lower(name));
    return it == headers.end() ? std::string() : it->second;
}

std::string newTransId() { return "t" + randHex(9); }
std::string newSessionId() { return randHex(12); }

bool extractFrame(std::string& buf, Frame& out) {
    size_t m = buf.find("MSRP ");
    if (m == std::string::npos) return false;
    size_t eol = buf.find("\r\n", m);
    if (eol == std::string::npos) return false;
    std::string start = buf.substr(m, eol - m);
    size_t s1 = start.find(' ', 5);                                   // "MSRP <tid> <method|code [phrase]>"
    if (s1 == std::string::npos) return false;
    std::string tid = start.substr(5, s1 - 5);
    std::string endLine = "-------" + tid;
    size_t e = buf.find(endLine, eol);
    if (e == std::string::npos || e + endLine.size() + 3 > buf.size()) return false;   // 플래그 + CRLF 까지
    out = Frame();
    out.tid = tid;
    std::string rest = start.substr(s1 + 1);
    if (!rest.empty() && std::isdigit((unsigned char)rest[0])) { out.request = false; out.status = std::atoi(rest.c_str()); }
    else { out.request = true; size_t sp = rest.find(' '); out.method = sp == std::string::npos ? rest : rest.substr(0, sp); }
    out.flag = buf[e + endLine.size()];
    size_t p = eol + 2;
    size_t blank = buf.find("\r\n\r\n", p);
    size_t hdrEnd = (blank != std::string::npos && blank < e) ? blank : e;
    while (p < hdrEnd) {
        size_t le = buf.find("\r\n", p);
        if (le == std::string::npos || le > hdrEnd) le = hdrEnd;
        std::string line = buf.substr(p, le - p);
        size_t c = line.find(':');
        if (c != std::string::npos) out.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
        p = le + 2;
    }
    if (blank != std::string::npos && blank < e) {
        size_t bs = blank + 4;
        size_t be = e >= 2 && buf.compare(e - 2, 2, "\r\n") == 0 ? e - 2 : e;   // 본문 뒤 CRLF 는 end-line 의 것
        if (be > bs) out.body = buf.substr(bs, be - bs);
    }
    buf.erase(0, e + endLine.size() + 3);
    return true;
}

std::string buildSend(const std::string& tid, const std::string& toPath, const std::string& fromPath, const std::string& msgId,
                      const std::string& contentType, const std::string& chunk, size_t start, size_t total, bool successReport,
                      char flag) {
    std::string h = "MSRP " + tid + " SEND\r\nTo-Path: " + toPath + "\r\nFrom-Path: " + fromPath + "\r\nMessage-ID: " + msgId + "\r\n";
    if (!contentType.empty())
        h += "Byte-Range: " + std::to_string(start + 1) + "-" + std::to_string(start + chunk.size()) + "/" + std::to_string(total) + "\r\n";
    if (successReport) h += "Success-Report: yes\r\n";
    h += "Failure-Report: yes\r\n";
    if (contentType.empty()) return h + "-------" + tid + flag + "\r\n";
    return h + "Content-Type: " + contentType + "\r\n\r\n" + chunk + "\r\n-------" + tid + flag + "\r\n";
}

std::string buildResponse(const std::string& tid, int code, const std::string& toPath, const std::string& fromPath) {
    const char* phrase = code == 200 ? "OK" : code == 413 ? "Stop Sending Message" : code == 415 ? "Unsupported Media Type" : "Error";
    return "MSRP " + tid + " " + std::to_string(code) + " " + phrase + "\r\nTo-Path: " + toPath + "\r\nFrom-Path: " + fromPath +
           "\r\n-------" + tid + "$\r\n";
}

bool parsePath(const std::string& pathIn, std::string& host, int& port) {
    std::string path = pathIn;
    size_t sp = path.find(' ');
    if (sp != std::string::npos) path = path.substr(0, sp);
    size_t s = path.find("://");
    if (s == std::string::npos) return false;
    size_t hs = s + 3, he = path.find('/', hs);
    std::string hp = path.substr(hs, he == std::string::npos ? std::string::npos : he - hs);
    if (!hp.empty() && hp[0] == '[') {                                 // IPv6 리터럴
        size_t rb = hp.find(']');
        if (rb == std::string::npos) return false;
        host = hp.substr(1, rb - 1);
        port = rb + 2 <= hp.size() && hp[rb + 1] == ':' ? std::atoi(hp.c_str() + rb + 2) : kNominalPort;
    } else {
        size_t c = hp.rfind(':');
        if (c == std::string::npos) { host = hp; port = kNominalPort; }
        else { host = hp.substr(0, c); port = std::atoi(hp.c_str() + c + 1); }
    }
    return !host.empty() && port > 0;
}

std::string localPath(const std::string& ip, const std::string& session) {
    return "msrp://" + ip + ":" + std::to_string(kNominalPort) + "/" + session + ";tcp";
}

std::string sdpSection(const std::string& lp, const char* setup, const char* direction) {
    const char* types = std::string(direction) == "sendonly" ? kAcceptTypesOffer : kAcceptTypes;   // 발신 offer · 수신 answer
    return "m=message " + std::to_string(kNominalPort) + " TCP/MSRP *\r\na=path:" + lp + "\r\na=accept-types:" + types +
           "\r\na=setup:" + setup + "\r\na=" + direction + "\r\n";
}

std::string pathOfSdp(const std::string& sdp) {
    size_t m = sdp.find("m=message ");
    if (m == std::string::npos) return std::string();
    size_t next = sdp.find("\nm=", m + 1);
    std::string sec = sdp.substr(m, next == std::string::npos ? std::string::npos : next - m);
    size_t a = sec.find("a=path:");
    if (a == std::string::npos) return std::string();
    size_t e = sec.find_first_of(" \r\n", a + 7);
    return sec.substr(a + 7, e == std::string::npos ? std::string::npos : e - a - 7);
}

std::string connAddrOf(const std::string& sdp) {
    size_t c = sdp.find("c=IN IP4 ");
    if (c == std::string::npos) return std::string();
    size_t e = sdp.find_first_of("\r\n", c);
    return sdp.substr(c + 9, e == std::string::npos ? std::string::npos : e - c - 9);
}

std::string audioInactive(const std::string& sdp) {
    size_t m = sdp.find("m=audio ");
    if (m == std::string::npos) return sdp;
    size_t next = sdp.find("\nm=", m + 1);
    size_t end = next == std::string::npos ? sdp.size() : next + 1;
    std::string sec = sdp.substr(m, end - m);
    for (const char* d : {"a=sendrecv", "a=sendonly", "a=recvonly"}) {
        size_t p = sec.find(d);
        if (p != std::string::npos) { sec.replace(p, std::string(d).size(), "a=inactive"); return sdp.substr(0, m) + sec + sdp.substr(end); }
    }
    if (sec.find("a=inactive") != std::string::npos) return sdp;
    size_t eol = sec.find("\r\n");
    if (eol == std::string::npos) return sdp;
    sec.insert(eol + 2, "a=inactive\r\n");
    return sdp.substr(0, m) + sec + sdp.substr(end);
}

std::string mcdataInfoUri(const std::string& body, const char* elem) {
    size_t p = body.find(std::string("<") + elem);
    if (p == std::string::npos) {
        p = body.find(std::string(":") + elem);                       // 접두사가 붙은 경우
        if (p == std::string::npos) return std::string();
    }
    size_t u = body.find("mcdataURI>", p);
    if (u == std::string::npos) return std::string();
    u += 10;
    size_t e = body.find("</", u);
    return e == std::string::npos ? std::string() : trim(body.substr(u, e - u));
}

int sendSds(const std::string& serverPath, const std::string& lp, const std::string& signallingTlv,
            const std::string& payloadTlv, int timeoutSec, const std::atomic<bool>& cancel,
            const std::function<void(size_t, size_t)>& progress, std::string& err) {
    Conn c;
    if (!c.open(serverPath, timeoutSec, err)) return 503;
    const int waitMs = timeoutSec * 1000;
    Frame f;
    // 응답 하나를 기다린다 — 그 사이 온 요청(REPORT 등)은 건너뛴다.
    auto awaitResponse = [&](const std::string& tid) -> int {
        for (;;) {
            if (!c.recv(f, waitMs, cancel)) return cancel ? 487 : 408;
            if (!f.request && f.tid == tid) return f.status;
        }
    };
    // ① SDS SIGNALLING PAYLOAD — 작은 본문 한 청크
    std::string tid = newTransId();
    if (!c.send(buildSend(tid, serverPath, lp, "m1", mcdata::kCtSignalling, signallingTlv, 0, signallingTlv.size(), false, '$')))
        { err = "send failed"; return 503; }
    int code = awaitResponse(tid);
    if (code != 200) { err = "signalling → " + std::to_string(code); return code; }
    // ② DATA PAYLOAD — 청크 stop-and-wait(원천 앱과 같이 16 KB), 모든 청크에 Success-Report
    const size_t total = payloadTlv.size();
    for (size_t off = 0; off < total || (total == 0 && off == 0);) {
        const size_t n = std::min(kChunkBytes, total - off);
        const bool last = off + n >= total;
        tid = newTransId();
        if (!c.send(buildSend(tid, serverPath, lp, "m2", mcdata::kCtPayload, payloadTlv.substr(off, n), off, total, true, last ? '$' : '+')))
            { err = "send failed"; return 503; }
        code = awaitResponse(tid);
        if (code != 200) { err = "payload → " + std::to_string(code); return code; }
        off += n;
        if (progress) progress(off, total);
        if (total == 0) break;
    }
    // ③ 저장 확인 REPORT(Success-Report) — 3 초만 기다린다(없어도 200 이 받았다는 뜻이다)
    for (;;) {
        if (!c.recv(f, 3000, cancel)) break;
        if (f.request && f.method == "REPORT") break;
    }
    c.close();
    return 200;
}

bool receiveSds(const std::string& serverPath, const std::string& lp, int timeoutSec, const std::atomic<bool>& cancel,
                std::string& contentType, std::string& body, std::string& err) {
    Conn c;
    if (!c.open(serverPath, timeoutSec, err)) return false;
    if (!c.send(buildSend(newTransId(), serverPath, lp, "b0", "", "", 0, 0, false, '$'))) { err = "bind send failed"; return false; }
    struct Msg { std::string ct, body; };
    std::map<std::string, Msg> msgs;                                   // Message-ID → 조립 중 본문
    std::string sig, pay;
    Frame f;
    for (int guard = 0; guard < 4096; ++guard) {
        if (!c.recv(f, timeoutSec * 1000, cancel)) { err = cancel ? "cancelled" : "timeout"; return false; }
        if (!f.request || f.method != "SEND") continue;
        c.send(buildResponse(f.tid, 200, f.header("From-Path"), lp));
        const std::string ct = f.header("Content-Type");
        const std::string id = f.header("Message-ID");
        Msg& m = msgs[id];
        if (!ct.empty()) m.ct = ct;
        if (m.ct.empty()) { msgs.erase(id); continue; }                  // bodiless
        if (!f.body.empty()) {
            size_t at = rangeStart(f.header("Byte-Range")) - 1;          // 청크 자리(순서가 바뀌어 와도)
            if (m.body.size() < at + f.body.size()) m.body.resize(at + f.body.size());
            m.body.replace(at, f.body.size(), f.body);
        }
        if (f.flag == '#') { msgs.erase(id); continue; }               // 중단된 메시지
        if (f.flag != '$') continue;
        Msg done = m;
        msgs.erase(id);
        const std::string lct = lower(done.ct);
        if (lct.find("multipart/mixed") != std::string::npos) {
            contentType = done.ct; body = done.body;
            c.close();
            return true;
        }
        if (lct.find("mcdata-signalling") != std::string::npos) sig = done.body;
        else if (lct.find("mcdata-payload") != std::string::npos) pay = done.body;
        if (!sig.empty() && !pay.empty()) {                              // 파트별 SEND 두 건 — multipart 로 합성(cmdp 재전달 형식)
            const std::string b = "cims" + randHex(16);
            body = "--" + b + "\r\nContent-Type: " + mcdata::kCtSignalling + "\r\n\r\n" + sig + "\r\n--" + b +
                   "\r\nContent-Type: " + mcdata::kCtPayload + "\r\n\r\n" + pay + "\r\n--" + b + "--\r\n";
            contentType = "multipart/mixed;boundary=" + b;
            c.close();
            return true;
        }
    }
    err = "too many frames";
    return false;
}

}  // namespace msrp
}  // namespace cimsue
