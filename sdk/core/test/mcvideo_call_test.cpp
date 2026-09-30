// libcimsue 단위시험 — MCVideo 호 제어(C3 등록·affiliation · C4 그룹 호) (S1-UE-UNIT)
//
// 계약 K3 SIP·K4 SDP 골든(tests/fixtures/mcvideo/sip/ — CSP 생성 시험 tests/csp_mcvideo_info_test.cpp 와 같은 파일)을 기준으로:
//  - McvSip  : 경계 코덱(mcvideo/mcvideo_sip)이 골든과 같은 본문을 만들고 골든을 읽어 같은 값을 낸다.
//  - McvCall : 헤드리스 엔진 + 루프백 가짜 CSP(UDP)·CMP(제어 채널·RTP) — SDK 가 «만드는 모양»(01·02·03·05·08)을 골든과 대조하고
//              «읽는 모양»(04·06·07·09)을 골든으로 답해 호·전송 제어 결선(성립·NAT 유지 RR·송출 허가 게이트)을 본다.
// CIMS_MCVIDEO_DUMP=<dir> 이면 SDK 가 보낸 메시지를 <dir>/sdk_*.txt 로 남긴다 — `python3 tests/mcvideo_fixture_check.py <dir>/sdk_*.txt`
// 로 .48 계약 검사기(K3·K4 규칙 + 본문 XSD)를 SDK 산출물에 그대로 돌린다.
#include <gtest/gtest.h>

#include <pjlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/cimsue.h"
#include "mcvideo/mcvideo_sip.h"
#include "mcvideo/tc_codec.h"
#include "pj_scope.h"

using namespace cimsue;

namespace {

const char* kDomain = "ptt.cims.example.kr";
const char* kPsi = "sip:mcvideo_psi@ptt.cims.example.kr";
const char* kClientId = "urn:uuid:2f6b8c4e-1a2b-4c3d-9e8f-0a1b2c3d4e5f";
const char* kUeA = "+82510002001";

std::string sipFixture(const char* name) {
    std::ifstream f(std::string(CIMS_SOURCE_ROOT) + "/tests/fixtures/mcvideo/sip/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void dump(const std::string& name, const std::string& msg) {
    const char* dir = std::getenv("CIMS_MCVIDEO_DUMP");
    if (!dir || !*dir) return;
    std::ofstream f(std::string(dir) + "/sdk_" + name + ".txt", std::ios::binary);
    f << msg;
}

std::string headerOf(const std::string& msg, const std::string& name) {
    size_t p = 0;
    size_t end = msg.find("\r\n\r\n");
    while ((p = msg.find("\r\n", p)) != std::string::npos && p < end) {
        p += 2;
        if (msg.compare(p, name.size() + 1, name + ":") == 0) {
            size_t v = p + name.size() + 1, e = msg.find("\r\n", v);
            while (v < e && msg[v] == ' ') ++v;
            return msg.substr(v, e - v);
        }
    }
    return "";
}

std::vector<std::string> headersOf(const std::string& msg, const std::string& name) {
    std::vector<std::string> out;
    size_t p = 0, end = msg.find("\r\n\r\n");
    while ((p = msg.find("\r\n", p)) != std::string::npos && p < end) {
        p += 2;
        if (msg.compare(p, name.size() + 1, name + ":") == 0) {
            size_t v = p + name.size() + 1, e = msg.find("\r\n", v);
            while (v < e && msg[v] == ' ') ++v;
            out.push_back(msg.substr(v, e - v));
        }
    }
    return out;
}

std::string bodyOf(const std::string& msg) {
    size_t p = msg.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : msg.substr(p + 4);
}

/** 본문(단일 또는 multipart)에서 Content-Type 이 ctype 인 파트 본문 — 끝 줄바꿈은 뗀다. */
std::string partOf(const std::string& msg, const std::string& ctype) {
    const std::string body = bodyOf(msg);
    if (headerOf(msg, "Content-Type").rfind(ctype, 0) == 0) return body;
    const std::string key = "Content-Type: " + ctype;
    size_t k = body.find(key);
    if (k == std::string::npos) return "";
    size_t b = body.find("\r\n\r\n", k);
    if (b == std::string::npos) return "";
    b += 4;
    size_t e = body.find("\r\n--", b);
    std::string s = body.substr(b, e == std::string::npos ? std::string::npos : e - b);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::vector<std::string> sdpLines(const std::string& sdp, const std::string& prefix) {
    std::vector<std::string> out;
    std::istringstream in(sdp);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind(prefix, 0) == 0) out.push_back(l);
    }
    return out;
}

std::string replaceAll(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

/** 골든 메시지의 Content-Length 를 본문 바이트에 맞춘다(주소·포트를 바꾼 뒤). */
std::string fixLength(const std::string& msg) {
    size_t h = msg.find("\r\n\r\n");
    std::string head = msg.substr(0, h), body = msg.substr(h + 4);
    size_t c = head.find("\r\nContent-Length:");
    size_t e = head.find("\r\n", c + 2);
    head = head.substr(0, c) + "\r\nContent-Length: " + std::to_string(body.size()) + (e == std::string::npos ? "" : head.substr(e));
    return head + "\r\n\r\n" + body;
}

/** 요청의 dialog 헤더를 골든 응답에 옮긴다 — Via·From·To·Call-ID·CSeq(태그는 골든 것). */
std::string answerFrom(const std::string& golden, const std::string& req) {
    std::istringstream in(golden.substr(0, golden.find("\r\n\r\n")));
    std::string line, out, first;
    std::getline(in, first);
    if (!first.empty() && first.back() == '\r') first.pop_back();
    out = first + "\r\n";
    std::string vias;
    size_t p = 0;
    while ((p = req.find("\r\nVia:", p)) != std::string::npos) {
        size_t e = req.find("\r\n", p + 2);
        vias += req.substr(p + 2, e - p - 2) + "\r\n";
        p = e;
    }
    bool viaDone = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Via:", 0) == 0) { if (!viaDone) { out += vias; viaDone = true; } continue; }
        if (line.rfind("From:", 0) == 0) { out += "From: " + headerOf(req, "From") + "\r\n"; continue; }
        if (line.rfind("To:", 0) == 0) {
            std::string gt = line.substr(line.find(";tag="));
            out += "To: " + headerOf(req, "To") + gt + "\r\n";
            continue;
        }
        if (line.rfind("Call-ID:", 0) == 0) { out += "Call-ID: " + headerOf(req, "Call-ID") + "\r\n"; continue; }
        if (line.rfind("CSeq:", 0) == 0) { out += "CSeq: " + headerOf(req, "CSeq") + "\r\n"; continue; }
        out += line + "\r\n";
    }
    return out + "\r\n" + bodyOf(golden);
}

/** UDP 한 개 — 가짜 CSP(SIP) 또는 가짜 CMP(제어 채널·RTP). */
struct FakeUdp {
    pj_sock_t s = PJ_INVALID_SOCKET;
    int port = 0;
    pj_sockaddr_in peer;
    bool havePeer = false;
    FakeUdp() {
        pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &s);
        pj_sockaddr_in a;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&a, &ip, 0);
        pj_sock_bind(s, &a, sizeof(a));
        int l = sizeof(a);
        pj_sock_getsockname(s, &a, &l);
        port = pj_ntohs(a.sin_port);
    }
    ~FakeUdp() { if (s != PJ_INVALID_SOCKET) pj_sock_close(s); }
    bool recvRaw(std::string& out, int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pj_fd_set_t fds;
            PJ_FD_ZERO(&fds);
            PJ_FD_SET(s, &fds);
            pj_time_val tv = {0, 20};
            if (pj_sock_select((int)s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            char buf[65536];
            pj_ssize_t n = sizeof(buf);
            int al = sizeof(peer);
            if (pj_sock_recvfrom(s, buf, &n, 0, &peer, &al) != PJ_SUCCESS || n <= 0) continue;
            havePeer = true;
            out.assign(buf, (size_t)n);
            return true;
        }
        return false;
    }
    /** prefix 로 시작하는 SIP 메시지 한 건(다른 것 — keepalive CRLF 등 — 은 건너뛴다). */
    std::string recv(const std::string& prefix, int ms = 3000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::string m;
        while (std::chrono::steady_clock::now() < end) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (!recvRaw(m, left > 0 ? left : 1)) break;
            if (m.compare(0, prefix.size(), prefix) == 0) return m;
        }
        return "";
    }
    void send(const std::string& m) {
        pj_ssize_t len = (pj_ssize_t)m.size();
        pj_sock_sendto(s, m.data(), &len, 0, &peer, sizeof(peer));
    }
    void sendTo(int toPort, const std::string& m) {
        pj_sockaddr_in to;
        pj_str_t ip = pj_str(const_cast<char*>("127.0.0.1"));
        pj_sockaddr_in_init(&to, &ip, (pj_uint16_t)toPort);
        pj_ssize_t len = (pj_ssize_t)m.size();
        pj_sock_sendto(s, m.data(), &len, 0, &to, sizeof(to));
    }
    void reply(const std::string& req, int code, const char* reason, const std::string& extra = "") {
        std::string r = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
        size_t p = 0;
        while ((p = req.find("\r\nVia:", p)) != std::string::npos) {
            size_t e = req.find("\r\n", p + 2);
            r += req.substr(p + 2, e - p - 2) + "\r\n";
            p = e;
        }
        std::string to = headerOf(req, "To");
        if (to.find(";tag=") == std::string::npos) to += ";tag=srv";
        r += "From: " + headerOf(req, "From") + "\r\nTo: " + to + "\r\nCall-ID: " + headerOf(req, "Call-ID") + "\r\nCSeq: " +
             headerOf(req, "CSeq") + "\r\n" + extra + "Content-Length: 0\r\n\r\n";
        send(r);
    }
    /** 시한 동안 받은 RTP 중 payload 가 있는 것(헤더 12 바이트 초과 — 빈 RTP keep-alive 는 세지 않는다). */
    int countPayloadRtp(int ms) {
        int c = 0;
        std::string m;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (!recvRaw(m, left > 0 ? left : 1)) break;
            const unsigned b1 = m.size() > 1 ? (unsigned char)m[1] : 0;
            if (m.size() > 12 && ((unsigned char)m[0] >> 6) == 2 && !(b1 >= 192 && b1 <= 223)) ++c;   // RTCP(PT 192~223)는 뺀다
        }
        return c;
    }
    /** 전송 제어 메시지 (app, op) 하나 — RR 등 해석 안 되는 것은 건너뛴다. */
    bool expectTc(mcvideo::AppName app, uint8_t op, mcvideo::Message* out, int ms = 2000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::string raw;
        while (std::chrono::steady_clock::now() < end) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (!recvRaw(raw, left > 0 ? left : 1)) break;
            mcvideo::Message m;
            if (mcvideo::decode((const uint8_t*)raw.data(), raw.size(), m) && m.app == app && m.op == op) {
                if (out) *out = m;
                return true;
            }
        }
        return false;
    }
    /** 빈 RTCP RR(PT 201, 8 바이트) 하나 — 헤더 SSRC 를 돌려준다. */
    bool expectRr(uint32_t& ssrc, int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::string raw;
        while (std::chrono::steady_clock::now() < end) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (!recvRaw(raw, left > 0 ? left : 1)) break;
            if (raw.size() == 8 && (unsigned char)raw[1] == 201) {
                ssrc = ((uint32_t)(unsigned char)raw[4] << 24) | ((uint32_t)(unsigned char)raw[5] << 16) |
                       ((uint32_t)(unsigned char)raw[6] << 8) | (unsigned char)raw[7];
                return true;
            }
        }
        return false;
    }
    void sendTc(mcvideo::AppName app, uint8_t op, std::vector<mcvideo::Tlv> fields, bool ackRequired = false) {
        mcvideo::Message m;
        m.app = app;
        m.op = op;
        m.ackRequired = ackRequired;
        m.ssrc = 0x0C0C0C0C;
        m.fields = std::move(fields);
        std::string pkt = mcvideo::encode(m);
        ASSERT_FALSE(pkt.empty());
        send(pkt);
    }
};

struct McvListener : Listener {
    void onLog(int lv, const std::string& m) override { if (std::getenv("MCV_LOG")) std::fprintf(stderr, "[%d] %s\n", lv, m.c_str()); }
    std::mutex m;
    std::condition_variable cv;
    std::vector<CallInfo> incoming, states;
    std::vector<TransmissionEvent> tx;
    std::vector<ReceptionEvent> rx;
    std::vector<RequestResult> results;
    std::vector<RegInfo> regs;
    void onRegState(const RegInfo& i) override { { std::lock_guard<std::mutex> lk(m); regs.push_back(i); } cv.notify_all(); }
    void onIncomingCall(const CallInfo& i) override { { std::lock_guard<std::mutex> lk(m); incoming.push_back(i); } cv.notify_all(); }
    void onCallState(const CallInfo& i) override { { std::lock_guard<std::mutex> lk(m); states.push_back(i); } cv.notify_all(); }
    void onTransmission(const TransmissionEvent& e) override { { std::lock_guard<std::mutex> lk(m); tx.push_back(e); } cv.notify_all(); }
    void onReception(const ReceptionEvent& e) override { { std::lock_guard<std::mutex> lk(m); rx.push_back(e); } cv.notify_all(); }
    void onRequestResult(const RequestResult& r) override { { std::lock_guard<std::mutex> lk(m); results.push_back(r); } cv.notify_all(); }
    template <class P> bool wait(P pred, int ms = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pred(); });
    }
    bool hasState(int callId, CallState s) {
        for (auto& c : states) if (c.callId == callId && c.state == s) return true;
        return false;
    }
    bool hasTx(TransmissionEvent::Kind k) {
        for (auto& e : tx) if (e.kind == k) return true;
        return false;
    }
};

/** 헤드리스 엔진 + 계정 하나(MCVideo 켬). */
struct Rig {
    Engine eng;
    McvListener l;
    FakeUdp csp;
    int acc = -1;
    std::unique_ptr<cimsue_test::PjScope> pj;
    explicit Rig(bool startEngine = true) {
        if (!startEngine) return;
        EngineConfig cfg;
        cfg.logLevel = std::getenv("MCV_LOG") ? 5 : 0;
        cfg.nullAudioDevice = true;
        cfg.udpNoTcpSwitch = true;                       // MCVideo INVITE(3 m-line + multipart)는 1300 B 를 넘는다 — UDP 가짜 서버로
        EXPECT_TRUE(eng.start(cfg, &l).ok);
        // 엔진이 pjlib 을 먼저 초기화한 뒤(ue-ctl 등록) 이 스레드를 등록한다(pj_scope.h)
        pj.reset(new cimsue_test::PjScope("mcv-test"));
    }
    AccountConfig account() const {
        AccountConfig ac;
        ac.serverHost = "127.0.0.1";
        ac.serverPort = csp.port;
        ac.transport = Transport::UDP;
        ac.domain = kDomain;
        ac.msisdn = kUeA;
        ac.authId = "450081000002001@ptt.cims.example.kr";
        ac.password = "x";
        ac.instanceId = kClientId;                       // MCVideo client ID = instance URN(effectiveMcpttClientId)
        ac.mcvideoEnabled = true;
        ac.mcvideoServerUri = kPsi;
        return ac;
    }
    void addAccount(const AccountConfig& ac) { acc = eng.addAccount(ac); ASSERT_GE(acc, 0); }
    ~Rig() {
        pj.reset();
        eng.stop();
    }
};

/** 골든 answer·초대의 CMP 주소(10.10.0.20)·포트를 가짜 CMP 로, 제어 기능 Contact 를 가짜 CSP 로 바꾼다. */
std::string localize(std::string msg, int audioFrom, int audioTo, int videoFrom, int videoTo, int ctrlFrom, int ctrlTo, int cspPort) {
    msg = replaceAll(msg, "10.10.0.20", "127.0.0.1");
    msg = replaceAll(msg, "m=audio " + std::to_string(audioFrom) + " ", "m=audio " + std::to_string(audioTo) + " ");
    msg = replaceAll(msg, "m=video " + std::to_string(videoFrom) + " ", "m=video " + std::to_string(videoTo) + " ");
    msg = replaceAll(msg, "m=application " + std::to_string(ctrlFrom) + " ", "m=application " + std::to_string(ctrlTo) + " ");
    msg = replaceAll(msg, "@csp.ptt.cims.example.kr:5061;transport=tls", "@127.0.0.1:" + std::to_string(cspPort) + ";transport=udp");
    return fixLength(msg);
}

/** multipart 파트마다 Content-Length 가 있으면 파트 본문 바이트와 같은가. */
bool partLengthsMatch(const std::string& msg) {
    const std::string ct = headerOf(msg, "Content-Type");
    size_t b = ct.find("boundary=");
    if (b == std::string::npos) return true;
    std::string boundary = ct.substr(b + 9);
    if (!boundary.empty() && boundary[0] == '"') boundary = boundary.substr(1, boundary.find('"', 1) - 1);
    const std::string body = bodyOf(msg), delim = "--" + boundary;
    size_t p = body.find(delim);
    while (p != std::string::npos && body.compare(p, delim.size() + 2, delim + "--") != 0) {
        size_t hs = body.find("\r\n", p) + 2, he = body.find("\r\n\r\n", hs), next = body.find("\r\n" + delim, he);
        if (he == std::string::npos || next == std::string::npos) return false;
        const std::string hdrs = body.substr(hs, he - hs);
        size_t cl = hdrs.find("Content-Length:");
        if (cl != std::string::npos && (size_t)std::atoi(hdrs.c_str() + cl + 15) != next - (he + 4)) return false;
        p = next + 2;
    }
    return true;
}

/** offer 의 media 섹션(m=prefix … 다음 m= 전) 줄들. */
std::vector<std::string> sectionOf(const std::string& sdp, const std::string& mprefix) {
    std::vector<std::string> out;
    std::istringstream in(sdp);
    std::string l;
    bool on = false;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind("m=", 0) == 0) on = l.rfind(mprefix, 0) == 0;
        if (on) out.push_back(l);
    }
    return out;
}

/**
 * 제어 기능의 200 OK — 헤더는 골든(Contact 세션 식별자·Require timer·PAI …), SDP 는 SDK offer 에 대한 answer(RFC 3264 — 실제 CSP 처럼
 * offer 의 음성 형식을 되돌리고, port 0 영상은 port 0 으로). 제어 채널 fmtp 는 골든 answer 의 것(서버가 정하는 값).
 */
std::string serverAnswer(const char* golden, const std::string& invite, int audioPort, int videoPort, int ctrlPort, int cspPort) {
    const std::string g = sipFixture(golden);
    const std::string offer = partOf(invite, "application/sdp");
    std::string sdp = "v=0\r\no=CSS 4 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n";
    for (const auto& l : sectionOf(offer, "m=audio ")) {
        if (l.rfind("a=rtcp:", 0) == 0 || l.rfind("a=ssrc", 0) == 0 || l.rfind("c=", 0) == 0) continue;
        if (l.rfind("m=audio ", 0) == 0) {
            std::string rest = l.substr(8);
            sdp += "m=audio " + std::to_string(audioPort) + rest.substr(rest.find(' ')) + "\r\n";
        } else {
            sdp += l + "\r\n";
        }
    }
    std::vector<std::string> ov = sectionOf(offer, "m=video ");
    const bool videoOffered = !ov.empty() && std::atoi(ov[0].c_str() + 8) != 0;
    for (const auto& l : sectionOf(bodyOf(g), "m=video ")) {
        if (l.rfind("m=video ", 0) == 0) sdp += "m=video " + std::to_string(videoOffered ? videoPort : 0) + " RTP/AVP 97\r\n";
        else sdp += l + "\r\n";
    }
    sdp += "m=application " + std::to_string(ctrlPort) + " udp MCVideo\r\n" + sdpLines(bodyOf(g), "a=fmtp:MCVideo ")[0] + "\r\n";
    std::string head = answerFrom(g, invite);
    head = head.substr(0, head.find("\r\n\r\n") + 4);
    return localize(head + sdp, 0, 0, 0, 0, 0, 0, cspPort);
}

std::string ackFor(const std::string& invite, const std::string& ok, int cspPort) {
    const std::string contact = headerOf(ok, "Contact");
    std::string target = contact.substr(contact.find('<') + 1, contact.find('>') - contact.find('<') - 1);
    std::string cseq = headerOf(invite, "CSeq");
    return "ACK " + target + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(cspPort) + ";branch=z9hG4bK-ack\r\n" +
           "From: " + headerOf(ok, "From") + "\r\nTo: " + headerOf(ok, "To") + "\r\nCall-ID: " + headerOf(ok, "Call-ID") +
           "\r\nCSeq: " + cseq.substr(0, cseq.find(' ')) + " ACK\r\nContent-Length: 0\r\n\r\n";
}

/** 제어 기능이 보내는 다이얼로그 안 BYE(해제 — prearranged 참가자 ≤1·chat 0명·T1·TNG3, TS 24.281 §9.2.1.4.2). */
std::string byeFor(const std::string& invite, const std::string& ok, int cspPort) {
    const std::string contact = headerOf(ok, "Contact");
    std::string target = contact.substr(contact.find('<') + 1, contact.find('>') - contact.find('<') - 1);
    return "BYE " + target + " SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:" + std::to_string(cspPort) + ";branch=z9hG4bK-bye\r\n" +
           "Max-Forwards: 70\r\nFrom: " + headerOf(ok, "From") + "\r\nTo: " + headerOf(ok, "To") + "\r\nCall-ID: " + headerOf(ok, "Call-ID") +
           "\r\nCSeq: " + std::to_string(std::atoi(headerOf(invite, "CSeq").c_str()) + 1) + " BYE\r\nContent-Length: 0\r\n\r\n";
}

}  // namespace

// ── 경계 코덱 ↔ 골든 ─────────────────────────────────────────────────────────────

// mcvideo-info·pidf 는 골든 파트와 바이트까지 같다(요소 순서 = Annex F.1 mcvideo-ParamsType, 표기 = 골든)
TEST(McvSip, BodiesMatchGolden) {
    mcvideo::InfoParams chat;
    chat.sessionType = "chat";
    chat.requestUri = "tel:g101";
    chat.clientId = kClientId;
    EXPECT_EQ(mcvideo::info(chat), partOf(sipFixture("03_chat_join_invite.txt"), mcvideo::kCtInfo));
    mcvideo::InfoParams pre = chat;
    pre.sessionType = "prearranged";
    pre.requestUri = "tel:g103";
    EXPECT_EQ(mcvideo::info(pre), partOf(sipFixture("05_prearranged_initiate_invite.txt"), mcvideo::kCtInfo));
    EXPECT_EQ(mcvideo::info(pre), partOf(sipFixture("08_prearranged_rejoin_invite.txt"), mcvideo::kCtInfo));
    mcvideo::InfoParams aff;
    aff.requestUri = std::string("tel:") + kUeA;
    const std::string g02 = sipFixture("02_publish_affiliation.txt");
    EXPECT_EQ(mcvideo::info(aff), partOf(g02, mcvideo::kCtInfo));
    EXPECT_EQ(mcvideo::affiliationPidf(std::string("tel:") + kUeA, kClientId, {"tel:g101", "tel:g103"}, "a1-mcv-aff-0001"),
              partOf(g02, mcvideo::kCtPidf));
}

// fmtp 순서·구분자(K4) — 골든 03(chat, 대기열·우선순위) · 05(prearranged 암묵 요청)
TEST(McvSip, FmtpMatchesGolden) {
    mcvideo::TcFmtp a;
    a.queueing = true;
    a.priority = 5;
    a.hasTcSsrc = true;
    a.tcSsrc = 305419896;
    EXPECT_EQ("a=fmtp:MCVideo " + mcvideo::fmtpString(a), sdpLines(sipFixture("03_chat_join_invite.txt"), "a=fmtp:MCVideo")[0]);
    mcvideo::TcFmtp b;
    b.priority = 5;
    b.granted = true;
    b.implicitRequest = true;
    b.hasTcSsrc = true;
    b.tcSsrc = 305419897;
    EXPECT_EQ("a=fmtp:MCVideo " + mcvideo::fmtpString(b), sdpLines(sipFixture("05_prearranged_initiate_invite.txt"), "a=fmtp:MCVideo")[0]);
    EXPECT_EQ(mcvideo::controlSdp(40004, a), "m=application 40004 udp MCVideo\r\na=fmtp:MCVideo mc_queueing;mc_priority=5;mc_transmission_ssrc=305419896");
}

// 이어지는 offer(세션 갱신 re-INVITE) — 개시 offer(골든 05)의 mc_granted·mc_implicit_request 를 뺀다(TS 24.581 §14.5), 나머지·다른 줄은 그대로
TEST(McvSip, SubsequentOfferDropsInitialOnlyFmtp) {
    const std::string sdp = partOf(sipFixture("05_prearranged_initiate_invite.txt"), "application/sdp") + "\r\n";   // 본문은 CRLF 로 끝난다
    ASSERT_NE(sdp.find("mc_granted;mc_implicit_request"), std::string::npos);
    const std::string sub = mcvideo::forSubsequentOffer(sdp);
    std::vector<std::string> fm = sdpLines(sub, "a=fmtp:MCVideo ");
    ASSERT_EQ(fm.size(), 1u);
    EXPECT_EQ(fm[0], "a=fmtp:MCVideo mc_priority=5;mc_transmission_ssrc=305419897") << fm[0];
    EXPECT_EQ(replaceAll(sdp, "mc_granted;mc_implicit_request;", ""), sub);    // 그 둘만 빠진다
    EXPECT_EQ(mcvideo::forSubsequentOffer(sub), sub);
    const std::string plain = "v=0\r\nm=audio 4000 RTP/AVP 96\r\na=fmtp:96 mc_granted\r\n";
    EXPECT_EQ(mcvideo::forSubsequentOffer(plain), plain);                      // MCVideo SDP 가 아니면 그대로
}

// «읽는 모양» — answer(04·06)·멤버 초대(07)의 제어 채널·fmtp·mcvideo-info
TEST(McvSip, ParsesGoldenAnswersAndInvitation) {
    std::string ip;
    int port = 0;
    mcvideo::TcFmtp f;
    ASSERT_TRUE(mcvideo::parseControl(sipFixture("04_chat_join_200.txt"), ip, port, f));
    EXPECT_EQ(ip, "10.10.0.20");
    EXPECT_EQ(port, 58000);
    EXPECT_TRUE(f.present);
    EXPECT_TRUE(f.queueing);                                  // offer 의 mc_queueing 을 서버가 되돌린다(송출 큐 — TS 24.581 §14.3.2)
    EXPECT_EQ(f.priority, 5);
    EXPECT_TRUE(f.hasTcSsrc);
    EXPECT_EQ(f.tcSsrc, 2863311530u);
    EXPECT_FALSE(f.implicitRequest);

    ASSERT_TRUE(mcvideo::parseControl(sipFixture("06_prearranged_initiate_200.txt"), ip, port, f));
    EXPECT_EQ(port, 58010);
    EXPECT_TRUE(f.granted);
    EXPECT_TRUE(f.implicitRequest);
    EXPECT_EQ(f.audioSsrc, 1111638594u);
    EXPECT_EQ(f.videoSsrc, 1111638595u);
    EXPECT_EQ(f.tcSsrc, 2863311531u);

    const std::string g07 = sipFixture("07_prearranged_member_invite.txt");
    ASSERT_TRUE(mcvideo::parseControl(g07, ip, port, f));
    EXPECT_EQ(port, 58012);
    EXPECT_EQ(f.priority, 5);
    EXPECT_EQ(f.tcSsrc, 2863311532u);
    EXPECT_FALSE(f.granted);
    mcvideo::InfoRx vi = mcvideo::parseInfo(g07);
    ASSERT_TRUE(vi.present);
    EXPECT_EQ(vi.sessionType, "prearranged");
    EXPECT_EQ(vi.requestUri, "tel:+82510002002");
    EXPECT_EQ(vi.callingUserId, "tel:+82510002001");
    EXPECT_EQ(vi.callingGroupId, "tel:g103");
    EXPECT_TRUE(mcvideo::isMcVideoSdp(g07));
    EXPECT_FALSE(mcvideo::parseInfo(sipFixture("09_reject_404_117.txt")).present);
}

// 미디어 i= 성분 표시(§6.2.1 2)c)·3)d)) — 없는 곳에만 m= 바로 뒤에, MCVideo SDP 에만
TEST(McvSip, MediaInfoInserted) {
    const std::string sdp = "v=0\r\ns=-\r\nc=IN IP4 1.2.3.4\r\nt=0 0\r\nm=audio 4000 RTP/AVP 96\r\na=rtpmap:96 AMR-WB/16000\r\n"
                            "m=video 4002 RTP/AVP 97\r\na=rtpmap:97 H264/90000\r\nm=application 4004 udp MCVideo\r\n";
    const std::string fixed = mcvideo::withMediaInfo(sdp);
    EXPECT_NE(fixed.find("m=audio 4000 RTP/AVP 96\r\ni=audio component of MCVideo\r\na=rtpmap"), std::string::npos);
    EXPECT_NE(fixed.find("m=video 4002 RTP/AVP 97\r\ni=video component of MCVideo\r\na=rtpmap"), std::string::npos);
    EXPECT_EQ(mcvideo::withMediaInfo(fixed), fixed);          // 이미 있으면 그대로(재송신·인증 재전송)
    const std::string mcptt = "v=0\r\nm=audio 4000 RTP/AVP 96\r\nm=application 4004 udp MCPTT\r\n";
    EXPECT_EQ(mcvideo::withMediaInfo(mcptt), mcptt);
    // multipart 인쇄본(pjsip — 파트마다 Content-Length) — SDP 파트만 고치고 그 Content-Length 를 새 길이로
    const std::string info = "<mcvideoinfo/>";
    const std::string mp = "\r\n--b1\r\nContent-Type: application/vnd.3gpp.mcvideo-info+xml\r\nContent-Length:   " + std::to_string(info.size()) +
                           "\r\n\r\n" + info + "\r\n--b1\r\nContent-Type: application/sdp\r\nContent-Length:   " + std::to_string(sdp.size()) +
                           "\r\n\r\n" + sdp + "\r\n--b1--\r\n";
    const std::string mfixed = mcvideo::withMediaInfoMultipart(mp);
    EXPECT_NE(mfixed.find("Content-Length:   " + std::to_string(info.size()) + "\r\n\r\n" + info + "\r\n--b1\r\n"), std::string::npos) << mfixed;
    EXPECT_NE(mfixed.find("Content-Type: application/sdp\r\nContent-Length: " + std::to_string(fixed.size()) + "\r\n\r\n" + fixed + "\r\n--b1--\r\n"),
              std::string::npos) << mfixed;
    EXPECT_EQ(mcvideo::withMediaInfoMultipart(mfixed), mfixed);
    EXPECT_EQ(mcvideo::contactUriPart("\"A\" <sip:a@1.2.3.4:5060;transport=udp>;+g.3gpp.mcptt"), "<sip:a@1.2.3.4:5060;transport=udp>");
    std::vector<std::string> icsis;
    EXPECT_EQ(mcvideo::withoutIcsiRef(";video;+g.3gpp.icsi-ref=\"urn%3Aa,urn%3Ab\";x", &icsis), ";video;x");
    ASSERT_EQ(icsis.size(), 2u);
    EXPECT_EQ(icsis[1], "urn%3Ab");
}

// ── 엔진 — 등록·affiliation (C3) ─────────────────────────────────────────────────

// REGISTER(§7.2.1AA · 골든 01 의 Contact 태그) → affiliation PUBLISH(§8.2.1.2 · 골든 02) — 관심 그룹 전부를 한 게시로, ETag 조건부 갱신, 0 개면 Expires 0
TEST(McvCall, RegisterAndAffiliation) {
    Rig r;
    r.addAccount(r.account());
    ASSERT_TRUE(r.eng.registerAccount(r.acc).ok);
    std::string reg = r.csp.recv("REGISTER ");
    ASSERT_FALSE(reg.empty());
    dump("01_register", reg);
    const std::string c = headerOf(reg, "Contact");
    EXPECT_NE(c.find(";+g.3gpp.mcvideo"), std::string::npos) << c;
    EXPECT_NE(c.find("+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcvideo\""), std::string::npos) << c;
    EXPECT_EQ(c.find("icsi-ref", c.find("icsi-ref") + 1), std::string::npos) << "icsi-ref 는 한 번(RFC 3840)";
    EXPECT_NE(c.find(std::string("+sip.instance=\"<") + kClientId + ">\""), std::string::npos);
    EXPECT_EQ(headerOf(reg, "Content-Length"), "0");          // §7.2.1AA — 서비스 인가 본문 없음
    r.csp.reply(reg, 200, "OK", "Contact: " + c + ";expires=3600\r\nExpires: 3600\r\n");
    ASSERT_TRUE(r.l.wait([&] { for (auto& i : r.l.regs) if (i.state == RegState::Registered) return true; return false; }));

    const std::string g02 = sipFixture("02_publish_affiliation.txt");
    auto publish = [&](const std::string& group, bool on) {
        EXPECT_GE(r.eng.affiliate(r.acc, group, on, McService::McVideo), 0);
        return r.csp.recv("PUBLISH ");
    };
    std::string p1 = publish("g101", true);
    ASSERT_FALSE(p1.empty());
    EXPECT_EQ(p1.substr(0, p1.find("\r\n")), std::string("PUBLISH ") + kPsi + " SIP/2.0");   // §8.2.1.2 1) R-URI = PSI
    EXPECT_EQ(headerOf(p1, "P-Preferred-Service"), headerOf(g02, "P-Preferred-Service"));
    EXPECT_EQ(headerOf(p1, "Event"), "presence");
    EXPECT_EQ(headerOf(p1, "Expires"), "4294967295");
    EXPECT_EQ(headerOf(p1, "SIP-If-Match"), "");
    EXPECT_EQ(partOf(p1, mcvideo::kCtInfo), partOf(g02, mcvideo::kCtInfo));
    std::string pidf = partOf(p1, mcvideo::kCtPidf);
    EXPECT_NE(pidf.find("<mcvideoPI10:affiliation group=\"tel:g101\"/>"), std::string::npos);
    EXPECT_EQ(pidf.find("g103"), std::string::npos);
    r.csp.reply(p1, 200, "OK", "SIP-ETag: e1\r\nExpires: 4294967295\r\n");
    ASSERT_TRUE(r.l.wait([&] { return r.l.results.size() >= 1; }));
    EXPECT_EQ(r.l.results[0].code, 200);

    std::string p2 = publish("g103", true);                   // 두 그룹 = 골든 02 의 pidf(p-id 만 다르다)
    ASSERT_FALSE(p2.empty());
    dump("02_publish_affiliation", p2);
    EXPECT_EQ(headerOf(p2, "SIP-If-Match"), "e1");
    pidf = partOf(p2, mcvideo::kCtPidf);
    std::string golden = partOf(g02, mcvideo::kCtPidf);
    auto pidOf = [](const std::string& x) {
        size_t a = x.find("<mcvideoPI10:p-id>") + 18, b = x.find("</mcvideoPI10:p-id>");
        return x.substr(a, b - a);
    };
    const std::string pid1 = pidOf(partOf(p1, mcvideo::kCtPidf)), pid2 = pidOf(pidf);
    EXPECT_NE(pid1, pid2);                                    // §8.2.1.2 6)d) — 게시마다 유일
    EXPECT_EQ(replaceAll(pidf, pid2, "a1-mcv-aff-0001"), golden);
    r.csp.reply(p2, 200, "OK", "SIP-ETag: e2\r\n");

    std::string p3 = publish("g101", false);                  // 관심 그룹 하나 남음
    ASSERT_FALSE(p3.empty());
    EXPECT_EQ(headerOf(p3, "Expires"), "4294967295");
    pidf = partOf(p3, mcvideo::kCtPidf);
    EXPECT_EQ(pidf.find("g101"), std::string::npos);
    EXPECT_NE(pidf.find("tel:g103"), std::string::npos);
    r.csp.reply(p3, 200, "OK", "SIP-ETag: e3\r\n");

    std::string p4 = publish("g103", false);                  // §8.2.1.2 5) — 더 관심 그룹이 없다
    ASSERT_FALSE(p4.empty());
    EXPECT_EQ(headerOf(p4, "Expires"), "0");
    EXPECT_EQ(partOf(p4, mcvideo::kCtPidf).find("mcvideoPI10:affiliation"), std::string::npos);
    r.csp.reply(p4, 200, "OK");
    ASSERT_TRUE(r.l.wait([&] { return r.l.results.size() >= 4; }));
}

// ── 엔진 — 그룹 호 (C4) ─────────────────────────────────────────────────────────

// chat 합류(골든 03) → 200 OK(골든 04) → 제어 채널 NAT 유지 RR → [영상 보내기] 허가 전·후의 오디오 RTP(허가 밖에서는 payload 없음) → [보내기 끝]
TEST(McvCall, ChatJoinTransmitAndRelease) {
    Rig r;
    r.addAccount(r.account());
    FakeUdp ctrl, audio, video;
    VideoGroupCallOptions o;
    o.queueing = true;
    o.maxPriority = 5;
    int id = r.eng.joinVideoGroupCall(r.acc, "g101", o);
    ASSERT_GE(id, 0);
    std::string inv = r.csp.recv("INVITE ");
    ASSERT_FALSE(inv.empty());
    dump("03_chat_join_invite", inv);
    const std::string g03 = sipFixture("03_chat_join_invite.txt");
    EXPECT_EQ(inv.substr(0, inv.find("\r\n")), std::string("INVITE ") + kPsi + " SIP/2.0");   // R-URI = 참여 MCVideo 기능 PSI
    EXPECT_EQ(headerOf(inv, "To"), std::string("<") + kPsi + ">");     // 골든 03 모양(name-addr)
    EXPECT_EQ(headersOf(inv, "Accept-Contact"), headersOf(g03, "Accept-Contact"));
    EXPECT_EQ(headerOf(inv, "P-Preferred-Service"), headerOf(g03, "P-Preferred-Service"));
    const std::string c = headerOf(inv, "Contact");
    EXPECT_EQ(c.substr(c.find('>') + 1), mcvideo::contactFeatureParams()) << c;   // 자기 서비스 태그만(골든 03)
    EXPECT_EQ(partOf(inv, mcvideo::kCtInfo), partOf(g03, mcvideo::kCtInfo));
    const std::string sdp = partOf(inv, "application/sdp");
    std::vector<std::string> m = sdpLines(sdp, "m=");
    ASSERT_EQ(m.size(), 3u) << sdp;
    EXPECT_EQ(m[0].rfind("m=audio ", 0), 0u);
    EXPECT_EQ(m[1].rfind("m=video ", 0), 0u);
    EXPECT_NE(m[2].find(" udp MCVideo"), std::string::npos) << m[2];
    EXPECT_EQ(sdpLines(sdp, "i=").size(), 2u) << sdp;         // 미디어 i= 둘(송신 직전 보정)
    EXPECT_TRUE(partLengthsMatch(inv)) << inv;               // 보정 뒤에도 파트 Content-Length = 파트 본문 바이트
    EXPECT_NE(sdp.find("m=audio"), std::string::npos);
    std::vector<std::string> fm = sdpLines(sdp, "a=fmtp:MCVideo ");
    ASSERT_EQ(fm.size(), 1u);
    EXPECT_EQ(fm[0].rfind("a=fmtp:MCVideo mc_queueing;mc_priority=5;mc_transmission_ssrc=", 0), 0u) << fm[0];
    const int ueCtrl = std::atoi(m[2].c_str() + 14);
    EXPECT_GT(ueCtrl, 0);
    CallInfo ci = r.eng.callInfo(id);
    EXPECT_EQ(ci.service, McService::McVideo);
    EXPECT_FALSE(ci.isMcptt);
    EXPECT_EQ(ci.groupId, "g101");

    std::string ok = serverAnswer("04_chat_join_200.txt", inv, audio.port, video.port, ctrl.port, r.csp.port);
    r.csp.send(ok);
    ASSERT_FALSE(r.csp.recv("ACK ").empty());
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasState(id, CallState::Active); }));
    ci = r.eng.callInfo(id);
    EXPECT_EQ(ci.sessionUri, "sip:g101@127.0.0.1:" + std::to_string(r.csp.port) + ";transport=udp;gr=1790775600123456-3");
    TransmissionInfo ti = r.eng.transmissionInfo(id);
    EXPECT_EQ(ti.localPort, ueCtrl);
    EXPECT_EQ(ti.remotePort, ctrl.port);
    EXPECT_EQ(ti.state, TransmissionState::NoPermission);    // chat 합류·암묵 요청 없음(§6.2.4.2.2 3)

    uint32_t rrSsrc = 0;
    ASSERT_TRUE(ctrl.expectRr(rrSsrc, 1000));                 // 성립 즉시 NAT 유지 RR — 헤더 SSRC = answer mc_transmission_ssrc
    EXPECT_EQ(rrSsrc, 2863311530u);
    EXPECT_EQ(audio.countPayloadRtp(700), 0);                 // 허가 밖 — 무음 프레임도 보내지 않는다(Revoked #3 방지)

    ASSERT_TRUE(r.eng.requestTransmission(id, 3).ok);
    mcvideo::Message req;
    ASSERT_TRUE(ctrl.expectTc(mcvideo::AppName::MCV0, (uint8_t)mcvideo::Mcv0::TRANSMISSION_REQUEST, &req));
    EXPECT_EQ(req.ssrc, 2863311530u);
    EXPECT_EQ(req.priority(), 3);
    ctrl.sendTc(mcvideo::AppName::MCV1, (uint8_t)mcvideo::Mcv1::TRANSMISSION_GRANTED,
                {mcvideo::ssrcField(mcvideo::Field::AUDIO_SSRC, 0xA1A1A1A1), mcvideo::ssrcField(mcvideo::Field::VIDEO_SSRC, 0xB1B1B1B1)}, true);
    mcvideo::Message ack;
    ASSERT_TRUE(ctrl.expectTc(mcvideo::AppName::MCV2, (uint8_t)mcvideo::Mcv2::TRANSMISSION_CONTROL_ACK, &ack));
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasTx(TransmissionEvent::Kind::Granted); }));
    {
        std::lock_guard<std::mutex> lk(r.l.m);
        EXPECT_EQ(r.l.tx.back().callId, id);
        EXPECT_EQ(r.l.tx.back().audioSsrc, 0xA1A1A1A1u);
    }
    EXPECT_EQ(r.eng.transmissionInfo(id).state, TransmissionState::Permitted);
    EXPECT_GT(audio.countPayloadRtp(600), 5);                 // 허가 — 오디오 송출

    ASSERT_TRUE(r.eng.releaseTransmission(id).ok);
    ASSERT_TRUE(ctrl.expectTc(mcvideo::AppName::MCV2, (uint8_t)mcvideo::Mcv2::TRANSMISSION_END_REQUEST, nullptr));
    ctrl.sendTc(mcvideo::AppName::MCV2, (uint8_t)mcvideo::Mcv2::TRANSMISSION_END_RESPONSE, {});
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasTx(TransmissionEvent::Kind::Ended); }));
    audio.countPayloadRtp(200);                                // 멈추기 전 이미 보낸 것 흘려보내기
    EXPECT_EQ(audio.countPayloadRtp(600), 0);

    ASSERT_TRUE(r.eng.hangup(id).ok);
    std::string bye = r.csp.recv("BYE ");
    ASSERT_FALSE(bye.empty());
    EXPECT_EQ(bye.substr(4, bye.find(' ', 4) - 4), ci.sessionUri);   // in-dialog 목적지 = 제어 기능 Contact(세션 식별자)
    r.csp.reply(bye, 200, "OK");
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasState(id, CallState::Disconnected); }));
}

// prearranged 개시 + 암묵적 송출 요청(골든 05) → 200 OK 허가(골든 06 — mc_granted·mc_audio_ssrc·mc_video_ssrc) = 곧바로 송출
TEST(McvCall, PrearrangedImplicitGranted) {
    Rig r;
    r.addAccount(r.account());
    FakeUdp ctrl, audio, video;
    VideoGroupCallOptions o;
    o.prearranged = true;
    o.maxPriority = 5;
    o.implicitTransmissionRequest = true;
    int id = r.eng.joinVideoGroupCall(r.acc, "g103", o);
    ASSERT_GE(id, 0);
    std::string inv = r.csp.recv("INVITE ");
    ASSERT_FALSE(inv.empty());
    dump("05_prearranged_initiate_invite", inv);
    EXPECT_EQ(partOf(inv, mcvideo::kCtInfo), partOf(sipFixture("05_prearranged_initiate_invite.txt"), mcvideo::kCtInfo));
    std::vector<std::string> fm = sdpLines(partOf(inv, "application/sdp"), "a=fmtp:MCVideo ");
    ASSERT_EQ(fm.size(), 1u);
    EXPECT_EQ(fm[0].rfind("a=fmtp:MCVideo mc_priority=5;mc_granted;mc_implicit_request;mc_transmission_ssrc=", 0), 0u) << fm[0];
    EXPECT_EQ(r.eng.transmissionInfo(id).state, TransmissionState::PendingRequest);   // §6.2.4.2.2 4
    r.csp.send(serverAnswer("06_prearranged_initiate_200.txt", inv, audio.port, video.port, ctrl.port, r.csp.port));
    ASSERT_FALSE(r.csp.recv("ACK ").empty());
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasTx(TransmissionEvent::Kind::Granted); }));
    {
        std::lock_guard<std::mutex> lk(r.l.m);
        EXPECT_EQ(r.l.tx.back().audioSsrc, 1111638594u);      // §14.4 — answer 의 값을 쓴다
        EXPECT_EQ(r.l.tx.back().videoSsrc, 1111638595u);
    }
    EXPECT_EQ(r.eng.transmissionInfo(id).state, TransmissionState::Permitted);
    EXPECT_FALSE(ctrl.expectTc(mcvideo::AppName::MCV0, (uint8_t)mcvideo::Mcv0::TRANSMISSION_REQUEST, nullptr, 500));   // answer 로 허가
    EXPECT_GT(audio.countPayloadRtp(600), 5);
    ASSERT_TRUE(r.eng.hangup(id).ok);
    std::string bye = r.csp.recv("BYE ");
    ASSERT_FALSE(bye.empty());
    r.csp.reply(bye, 200, "OK");
}

// 재합류(골든 08, §9.2.1.2.4) — R-URI·To = 세션 식별자, prearranged, 암묵 요청 없음 · 그룹 종류 거절(골든 09 — 404 Warning 117)
TEST(McvCall, RejoinAndWrongGroupType) {
    Rig r;
    r.addAccount(r.account());
    const std::string session = "sip:g103@127.0.0.1:" + std::to_string(r.csp.port) + ";transport=udp;gr=1790775900654321-1";
    VideoGroupCallOptions o;
    o.sessionUri = session;
    o.implicitTransmissionRequest = true;                     // 재합류에는 싣지 않는다(§14.3.5 진행 중 세션)
    o.maxPriority = 5;
    int id = r.eng.joinVideoGroupCall(r.acc, "g103", o);
    ASSERT_GE(id, 0);
    std::string inv = r.csp.recv("INVITE ");
    ASSERT_FALSE(inv.empty());
    dump("08_prearranged_rejoin_invite", replaceAll(inv, "127.0.0.1:" + std::to_string(r.csp.port) + ";transport=udp",
                                                    "csp.ptt.cims.example.kr:5061;transport=tls"));
    EXPECT_EQ(inv.substr(0, inv.find("\r\n")), "INVITE " + session + " SIP/2.0");
    // To = 세션 식별자(골든 08 모양) — port·transport 는 To 에 둘 수 없어(RFC 3261 §19.1.1 표 1) pjsip 이 빼고, gr 은 URI 파라미터라
    //   꺾쇠 안에 남는다(RFC 3261 §20 — 꺾쇠가 없으면 To 헤더 파라미터로 읽힌다). 세션은 사용자부·gr 로 가른다
    EXPECT_EQ(headerOf(inv, "To"), "<sip:g103@127.0.0.1;gr=1790775900654321-1>");
    EXPECT_EQ(partOf(inv, mcvideo::kCtInfo), partOf(sipFixture("08_prearranged_rejoin_invite.txt"), mcvideo::kCtInfo));
    std::vector<std::string> fm = sdpLines(partOf(inv, "application/sdp"), "a=fmtp:MCVideo ");
    ASSERT_EQ(fm.size(), 1u);
    EXPECT_EQ(fm[0].find("mc_implicit_request"), std::string::npos);
    EXPECT_EQ(fm[0].find("mc_granted"), std::string::npos);
    r.csp.reply(inv, 486, "Busy Here");
    ASSERT_FALSE(r.csp.recv("ACK ").empty());
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasState(id, CallState::Disconnected); }));

    // chat INVITE 가 prearranged 그룹에 — 404 + Warning 117(§6.3.5.2 5)c)): 호가 끝나고 최종 코드가 남는다
    int id2 = r.eng.joinVideoGroupCall(r.acc, "g103", VideoGroupCallOptions());
    ASSERT_GE(id2, 0);
    std::string inv2 = r.csp.recv("INVITE ");
    ASSERT_FALSE(inv2.empty());
    r.csp.send(fixLength(answerFrom(sipFixture("09_reject_404_117.txt"), inv2)));
    ASSERT_FALSE(r.csp.recv("ACK ").empty());
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasState(id2, CallState::Disconnected); }));
    EXPECT_EQ(r.eng.callInfo(id2).lastCode, 404);
}

// 제어 기능의 멤버 초대(골든 07, §9.2.1.3) → 자동 수락: 180·200 Contact = MCVideo 태그, answer = 제어 채널 + fmtp(mc_priority 되돌림 ·
//   이 단말의 mc_transmission_ssrc — §14.3.3·§14.3.9) · ACK 뒤 RR(헤더 SSRC = offer mc_transmission_ssrc)
TEST(McvCall, MemberInvitationAutoAnswer) {
    Rig r;
    AccountConfig ac = r.account();
    ac.msisdn = "+82510002002";
    ac.authId = "450081000002002@ptt.cims.example.kr";
    r.addAccount(ac);
    ASSERT_TRUE(r.eng.registerAccount(r.acc).ok);
    std::string reg = r.csp.recv("REGISTER ");
    ASSERT_FALSE(reg.empty());
    const std::string ue = headerOf(reg, "Contact");
    const std::string ueUri = ue.substr(ue.find('<') + 1, ue.find('>') - ue.find('<') - 1);
    r.csp.reply(reg, 200, "OK", "Contact: " + ue + ";expires=3600\r\n");
    ASSERT_TRUE(r.l.wait([&] { for (auto& i : r.l.regs) if (i.state == RegState::Registered) return true; return false; }));

    FakeUdp ctrl, audio, video;
    std::string inv = sipFixture("07_prearranged_member_invite.txt");
    inv = "INVITE " + ueUri + " SIP/2.0" + inv.substr(inv.find("\r\n"));
    inv = replaceAll(inv, "Via: SIP/2.0/TLS csp.ptt.cims.example.kr:5061;branch=z9hG4bK-mcv-fan1",
                     "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(r.csp.port) + ";branch=z9hG4bK-mcv-fan1");
    inv = localize(inv, 52012, audio.port, 56012, video.port, 58012, ctrl.port, r.csp.port);
    r.csp.send(inv);
    std::string ringing = r.csp.recv("SIP/2.0 180");
    ASSERT_FALSE(ringing.empty());
    std::string okr = r.csp.recv("SIP/2.0 200");
    ASSERT_FALSE(okr.empty());
    dump("07r_member_answer_200", okr);
    // 세션 갱신 주체 = 단말(TS 24.281 §6.2.3.1.1 5) — 제어 기능 초대는 refresher 를 싣지 않는다(골든 07, §6.3.3.1.2 6))
    EXPECT_EQ(headerOf(inv, "Session-Expires"), "1800");
    EXPECT_EQ(headerOf(okr, "Session-Expires"), "1800;refresher=uas");
    EXPECT_NE(headerOf(okr, "Require").find("timer"), std::string::npos);
    EXPECT_EQ(headersOf(okr, "Require").size(), 1u) << okr;           // 180 의 Require 가 200 에 남아도 한 줄
    ASSERT_TRUE(r.l.wait([&] { return !r.l.incoming.empty(); }));
    CallInfo in;
    { std::lock_guard<std::mutex> lk(r.l.m); in = r.l.incoming[0]; }
    EXPECT_EQ(in.service, McService::McVideo);
    EXPECT_EQ(in.groupId, "g103");
    EXPECT_FALSE(in.isMcptt);
    EXPECT_EQ(in.sessionUri, "sip:g103@127.0.0.1:" + std::to_string(r.csp.port) + ";transport=udp;gr=1790775900654321-1");
    for (const std::string* resp : {&ringing, &okr}) {
        const std::string c = headerOf(*resp, "Contact");
        EXPECT_EQ(c.substr(c.find('>') + 1), mcvideo::contactFeatureParams()) << c;   // §6.2.3.1.1 3)·4) · §6.2.3.2.1 3)·4)
        EXPECT_NE(headerOf(*resp, "Require").find("timer"), std::string::npos);        // §6.2.3.1.1 2) · §6.2.3.2.1 2)
    }
    const std::string sdp = partOf(okr, "application/sdp");
    std::vector<std::string> m = sdpLines(sdp, "m=");
    ASSERT_EQ(m.size(), 3u) << sdp;
    EXPECT_NE(m[2].find(" udp MCVideo"), std::string::npos) << m[2];
    EXPECT_NE(std::atoi(m[2].c_str() + 14), 0);
    EXPECT_EQ(sdpLines(sdp, "i=").size(), 2u) << sdp;
    std::vector<std::string> fm = sdpLines(sdp, "a=fmtp:MCVideo ");
    ASSERT_EQ(fm.size(), 1u);
    EXPECT_EQ(fm[0].rfind("a=fmtp:MCVideo mc_priority=5;mc_transmission_ssrc=", 0), 0u) << fm[0];   // offer 에 없던 것은 싣지 않는다(§14.3.1)
    r.csp.send(ackFor(inv, okr, r.csp.port));
    uint32_t rrSsrc = 0;
    ASSERT_TRUE(ctrl.expectRr(rrSsrc, 1000));
    EXPECT_EQ(rrSsrc, 2863311532u);
    EXPECT_EQ(audio.countPayloadRtp(500), 0);
    // 서버 알림 → 수신 목록(manual 수신 — [받기] 대기)
    ctrl.sendTc(mcvideo::AppName::MCV1, (uint8_t)mcvideo::Mcv1::MEDIA_TRANSMISSION_NOTIFICATION,
                {mcvideo::strField(mcvideo::Field::TRANSMITTING_USER_ID, "tel:+82510002001"),
                 mcvideo::ssrcField(mcvideo::Field::AUDIO_SSRC, 1111638594), mcvideo::ssrcField(mcvideo::Field::VIDEO_SSRC, 1111638595),
                 mcvideo::u16Field(mcvideo::Field::RECEPTION_MODE, (int)mcvideo::ReceptionMode::MANUAL)});
    ASSERT_TRUE(r.l.wait([&] { return !r.l.rx.empty(); }));
    {
        std::lock_guard<std::mutex> lk(r.l.m);
        EXPECT_EQ(r.l.rx[0].kind, ReceptionEvent::Kind::Notified);
        EXPECT_EQ(r.l.rx[0].callId, in.callId);
        EXPECT_EQ(r.l.rx[0].transmitter.userId, "tel:+82510002001");
    }
    ASSERT_TRUE(r.eng.acceptReception(in.callId, "tel:+82510002001").ok);
    mcvideo::Message rq;
    ASSERT_TRUE(ctrl.expectTc(mcvideo::AppName::MCV0, (uint8_t)mcvideo::Mcv0::RECEIVE_MEDIA_REQUEST, &rq));
    EXPECT_EQ(rq.transmittingUserId(), "tel:+82510002001");
    EXPECT_EQ(rq.ssrc, 2863311532u);
    ASSERT_TRUE(r.eng.hangup(in.callId).ok);
    std::string bye = r.csp.recv("BYE ");
    ASSERT_FALSE(bye.empty());
    r.csp.reply(bye, 200, "OK");
}

// 수동 개시(TS 24.281 §6.2.3.2.1 — autoAnswerMcvideo 끔): 180 만 보내고 앱의 answer() 를 기다린다. 앱이 영상 옵션 없이 받아도 MCVideo answer
//   (audio + video + 제어 채널, §6.2.2)다. 이어서 제어 기능의 해제 BYE(§9.2.1.4.2) — 호가 끝나고 제어 채널 RR 이 멈춘다.
TEST(McvCall, MemberInvitationManualAnswerAndServerRelease) {
    Rig r;
    AccountConfig ac = r.account();
    ac.msisdn = "+82510002002";
    ac.authId = "450081000002002@ptt.cims.example.kr";
    ac.autoAnswerMcvideo = false;
    r.addAccount(ac);
    ASSERT_TRUE(r.eng.registerAccount(r.acc).ok);
    std::string reg = r.csp.recv("REGISTER ");
    ASSERT_FALSE(reg.empty());
    const std::string ue = headerOf(reg, "Contact");
    const std::string ueUri = ue.substr(ue.find('<') + 1, ue.find('>') - ue.find('<') - 1);
    r.csp.reply(reg, 200, "OK", "Contact: " + ue + ";expires=3600\r\n");
    ASSERT_TRUE(r.l.wait([&] { for (auto& i : r.l.regs) if (i.state == RegState::Registered) return true; return false; }));

    FakeUdp ctrl, audio, video;
    std::string inv = sipFixture("07_prearranged_member_invite.txt");
    inv = "INVITE " + ueUri + " SIP/2.0" + inv.substr(inv.find("\r\n"));
    inv = replaceAll(inv, "Via: SIP/2.0/TLS csp.ptt.cims.example.kr:5061;branch=z9hG4bK-mcv-fan1",
                     "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(r.csp.port) + ";branch=z9hG4bK-mcv-fan1");
    inv = localize(inv, 52012, audio.port, 56012, video.port, 58012, ctrl.port, r.csp.port);
    r.csp.send(inv);
    std::string ringing = r.csp.recv("SIP/2.0 180");
    ASSERT_FALSE(ringing.empty());
    EXPECT_NE(headerOf(ringing, "Require").find("timer"), std::string::npos);        // §6.2.3.2.1 2)
    EXPECT_TRUE(r.csp.recv("SIP/2.0 200", 800).empty());                          // 앱이 받기 전에는 200 이 없다
    ASSERT_TRUE(r.l.wait([&] { return !r.l.incoming.empty(); }));
    CallInfo in;
    { std::lock_guard<std::mutex> lk(r.l.m); in = r.l.incoming[0]; }
    EXPECT_EQ(in.service, McService::McVideo);
    ASSERT_TRUE(r.eng.answer(in.callId, CallOptions{}).ok);                       // 영상 옵션 없이
    std::string okr = r.csp.recv("SIP/2.0 200");
    ASSERT_FALSE(okr.empty());
    EXPECT_EQ(headerOf(okr, "Session-Expires"), "1800;refresher=uas");
    EXPECT_NE(headerOf(okr, "Require").find("timer"), std::string::npos);
    const std::string sdp = partOf(okr, "application/sdp");
    std::vector<std::string> m = sdpLines(sdp, "m=");
    ASSERT_EQ(m.size(), 3u) << sdp;
    EXPECT_EQ(m[1].rfind("m=video ", 0), 0u);
    EXPECT_NE(m[2].find(" udp MCVideo"), std::string::npos) << m[2];
    EXPECT_NE(std::atoi(m[2].c_str() + 14), 0);
    std::vector<std::string> fm = sdpLines(sdp, "a=fmtp:MCVideo ");
    ASSERT_EQ(fm.size(), 1u);
    EXPECT_EQ(fm[0].rfind("a=fmtp:MCVideo mc_priority=5;mc_transmission_ssrc=", 0), 0u) << fm[0];
    r.csp.send(ackFor(inv, okr, r.csp.port));
    uint32_t rrSsrc = 0;
    ASSERT_TRUE(ctrl.expectRr(rrSsrc, 1000));                                     // 참여자 성립 — 제어 채널 유지 RR

    // 제어 기능의 해제 — 200 OK 로 답하고 호가 끝나며, 제어 채널 유지 RR 이 멈춘다(이 호의 참여자 인스턴스가 닫혔다)
    r.csp.send(byeFor(inv, okr, r.csp.port));
    std::string byeOk = r.csp.recv("SIP/2.0 200");
    ASSERT_FALSE(byeOk.empty());
    EXPECT_NE(headerOf(byeOk, "CSeq").find("BYE"), std::string::npos);
    ASSERT_TRUE(r.l.wait([&] { return r.l.hasState(in.callId, CallState::Disconnected); }));
    EXPECT_FALSE(ctrl.expectRr(rrSsrc, 2500));                                    // 1 s 간격 유지 RR 둘이 남아 있었다면 여기서 보인다
    EXPECT_EQ(r.eng.transmissionInfo(in.callId).localPort, 0);                     // 끝난 호 — 참여자 없음(기본값)
}
