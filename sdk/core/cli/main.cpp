// cimsue-cli — libcimsue 위의 헤드리스 UE (ue_sdk.md §4.7·§9)
//
// 실제 단말 스택(pjsua2 + 코어)으로 등록·1:1 호를 구동해 S3 검증 축을 제공한다. cspsim(시뮬레이터)과
// 달리 코덱·지터버퍼·SRTP·TLS 를 단말과 같은 경로로 처리한다.
//
//   cimsue-cli [계정 옵션] register [--hold S]
//   cimsue-cli [계정 옵션] call <번호|sip:URI> [--duration S] [--video]
//   cimsue-cli [계정 옵션] answer [--duration S]
//
// 계정 옵션: --server IP --port N --transport udp|tcp|tls --domain D --msisdn M (--imsi I | --auth-id IMPI)
//           (--ha1 HEX32 | --password P) [--srtp off|optional|required] [--sec tls,ipsec-3gpp]
//           [--tls-ca FILE] [--no-tls-verify] [--display-name NAME] [--log-level N] [--timeout S] [--json]
// 종료 코드: 0 성공 / 2 인자 / 3 등록 실패·시한 / 4 호 실패·시한 / 5 미디어 없음(수신 RTP 0)
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cimsue/cimsue.h"

using namespace cimsue;
using Clock = std::chrono::steady_clock;

namespace {

struct Opts {
    AccountConfig acc;
    std::string tlsCaFile;
    bool tlsVerify = true;
    int logLevel = 3;
    int timeoutSec = 20;
    bool json = false;
    std::string cmd;
    std::string target;
    int durationSec = 8;
    int holdSec = 0;
    bool video = false;
};

void usage() {
    std::fprintf(stderr,
        "usage: cimsue-cli --server IP [--port N] [--transport udp|tcp|tls] --domain D --msisdn M\n"
        "                  (--imsi I | --auth-id IMPI) (--ha1 HEX | --password P) [--srtp off|optional|required]\n"
        "                  [--sec tls] [--tls-ca FILE] [--no-tls-verify] [--display-name N] [--log-level N]\n"
        "                  [--timeout S] [--json]  register [--hold S] | call TARGET [--duration S] [--video] | answer [--duration S]\n");
}

bool parse(int argc, char** argv, Opts& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 >= argc) return false; out = argv[++i]; return true; };
        std::string v;
        if (a == "--server") { if (!next(v)) return false; o.acc.serverHost = v; }
        else if (a == "--port") { if (!next(v)) return false; o.acc.serverPort = std::atoi(v.c_str()); }
        else if (a == "--transport") { if (!next(v)) return false;
            o.acc.transport = v == "tls" ? Transport::TLS : v == "tcp" ? Transport::TCP : Transport::UDP; }
        else if (a == "--domain") { if (!next(v)) return false; o.acc.domain = v; }
        else if (a == "--msisdn") { if (!next(v)) return false; o.acc.msisdn = v; }
        else if (a == "--imsi") { if (!next(v)) return false; o.acc.imsi = v; }
        else if (a == "--auth-id") { if (!next(v)) return false; o.acc.authId = v; }
        else if (a == "--ha1") { if (!next(v)) return false; o.acc.ha1 = v; }
        else if (a == "--password") { if (!next(v)) return false; o.acc.password = v; }
        else if (a == "--display-name") { if (!next(v)) return false; o.acc.displayName = v; }
        else if (a == "--srtp") { if (!next(v)) return false;
            o.acc.mediaSecurity = v == "required" ? MediaSecurity::Required : v == "optional" ? MediaSecurity::Optional : MediaSecurity::Off; }
        else if (a == "--sec") { if (!next(v)) return false;
            std::stringstream ss(v); std::string m; while (std::getline(ss, m, ',')) if (!m.empty()) o.acc.secMechanisms.push_back(m); }
        else if (a == "--tls-ca") { if (!next(v)) return false; o.tlsCaFile = v; }
        else if (a == "--no-tls-verify") o.tlsVerify = false;
        else if (a == "--log-level") { if (!next(v)) return false; o.logLevel = std::atoi(v.c_str()); }
        else if (a == "--timeout") { if (!next(v)) return false; o.timeoutSec = std::atoi(v.c_str()); }
        else if (a == "--json") o.json = true;
        else if (a == "--duration") { if (!next(v)) return false; o.durationSec = std::atoi(v.c_str()); }
        else if (a == "--hold") { if (!next(v)) return false; o.holdSec = std::atoi(v.c_str()); }
        else if (a == "--video") o.video = true;
        else if (a == "-h" || a == "--help") return false;
        else if (o.cmd.empty() && a.rfind("--", 0) != 0) o.cmd = a;
        else if (o.cmd == "call" && o.target.empty() && a.rfind("--", 0) != 0) o.target = a;
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
    }
    if (o.cmd.empty()) return false;
    if (o.cmd == "call" && o.target.empty()) return false;
    if (o.cmd != "register" && o.cmd != "call" && o.cmd != "answer") return false;
    return true;
}

/** 상태를 모아 조건 대기하는 리스너 — 이벤트 스레드가 쓰고 main 이 기다린다. */
class CliListener : public Listener {
public:
    explicit CliListener(int logLevel) : logLevel_(logLevel) {}
    void onLog(int level, const std::string& msg) override {
        if (level <= logLevel_) std::fprintf(stderr, "%s\n", msg.c_str());
    }
    void onRegState(const RegInfo& r) override {
        std::fprintf(stderr, "[cimsue-cli] reg acc=%d %s code=%d %s expires=%d\n", r.accountId, toString(r.state),
                     r.code, r.reason.c_str(), r.expiresSec);
        set([&] { reg = r; });
    }
    void onIncomingCall(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] incoming call=%d from=%s called=%s video=%d\n", c.callId,
                     c.remoteUri.c_str(), c.calledParty.c_str(), c.video);
        set([&] { incoming = c; haveIncoming = true; });
    }
    void onCallState(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] call=%d %s code=%d %s\n", c.callId, toString(c.state), c.lastCode,
                     c.lastReason.c_str());
        set([&] { calls[c.callId] = c; });
    }
    void onCallMedia(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] call=%d media=%d state=%s\n", c.callId, c.mediaActive, toString(c.state));
        set([&] { calls[c.callId] = c; });
    }

    template <typename Pred>
    bool waitFor(Pred p, int timeoutSec) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::seconds(timeoutSec), [&] { return p(); });
    }
    RegInfo reg;
    CallInfo incoming;
    bool haveIncoming = false;
    std::map<int, CallInfo> calls;

private:
    template <typename F> void set(F f) { { std::lock_guard<std::mutex> lk(m_); f(); } cv_.notify_all(); }
    int logLevel_;
    std::mutex m_;
    std::condition_variable cv_;
};

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

void summary(const Opts& o, const std::string& outcome, int callId, const StreamStats& st, int code, const std::string& reason) {
    if (!o.json) {
        std::printf("%s: %s call=%d rx_pkts=%u tx_pkts=%u rx_loss=%u code=%d %s\n", o.cmd.c_str(), outcome.c_str(),
                    callId, st.rxPackets, st.txPackets, st.rxLoss, code, reason.c_str());
        return;
    }
    std::printf("{\"cmd\":\"%s\",\"outcome\":\"%s\",\"msisdn\":\"%s\",\"call_id\":%d,\"rx_pkts\":%u,\"tx_pkts\":%u,"
                "\"rx_bytes\":%u,\"rx_loss\":%u,\"code\":%d,\"reason\":\"%s\"}\n",
                o.cmd.c_str(), outcome.c_str(), o.acc.msisdn.c_str(), callId, st.rxPackets, st.txPackets, st.rxBytes,
                st.rxLoss, code, reason.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    if (!parse(argc, argv, o)) { usage(); return 2; }
    if (!o.acc.isComplete()) {
        std::fprintf(stderr, "account incomplete: need --server --domain --msisdn (--imsi|--auth-id) (--ha1|--password)\n");
        return 2;
    }

    EngineConfig ec;
    ec.userAgent = "CIMS-UE/cimsue-cli";
    ec.logLevel = o.logLevel;
    ec.nullAudioDevice = true;
    ec.tlsVerifyServer = o.tlsVerify;
    if (!o.tlsCaFile.empty()) ec.tlsCaPem = readFile(o.tlsCaFile);

    CliListener ls(o.logLevel);
    Engine eng;
    Result r = eng.start(ec, &ls);
    if (!r.ok) { std::fprintf(stderr, "engine start failed: %d %s\n", r.code, r.reason.c_str()); return 3; }
    std::fprintf(stderr, "[cimsue-cli] %s\n", Engine::version().c_str());

    int acc = eng.addAccount(o.acc);
    if (acc < 0) { std::fprintf(stderr, "addAccount failed\n"); eng.stop(); return 3; }
    r = eng.registerAccount(acc);
    if (!r.ok) { std::fprintf(stderr, "register failed: %s\n", r.reason.c_str()); eng.stop(); return 3; }
    bool regOk = ls.waitFor([&] { return ls.reg.state == RegState::Registered || ls.reg.state == RegState::Failed; }, o.timeoutSec);
    if (!regOk || ls.reg.state != RegState::Registered) {
        summary(o, "register_failed", -1, StreamStats{}, ls.reg.code, ls.reg.reason);
        eng.stop();
        return 3;
    }

    int rc = 0;
    StreamStats st;
    int callId = -1;
    std::string outcome = "ok";
    int code = ls.reg.code; std::string reason = ls.reg.reason;

    if (o.cmd == "register") {
        if (o.holdSec > 0) std::this_thread::sleep_for(std::chrono::seconds(o.holdSec));
        outcome = "registered";
    } else if (o.cmd == "call") {
        CallOptions co; co.video = o.video;
        callId = eng.dial(acc, o.target, co);
        if (callId < 0) { summary(o, "dial_failed", -1, st, 0, "dial"); eng.stop(); return 4; }
        bool up = ls.waitFor([&] {
            auto it = ls.calls.find(callId);
            return it != ls.calls.end() && (it->second.state == CallState::Disconnected ||
                                            (it->second.state == CallState::Active && it->second.mediaActive));
        }, o.timeoutSec);
        CallInfo ci = ls.calls.count(callId) ? ls.calls[callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) {
            summary(o, up ? "call_failed" : "call_timeout", callId, st, ci.lastCode, ci.lastReason);
            eng.hangup(callId); eng.stop(); return 4;
        }
        // 통화 유지 — 상대가 먼저 끊으면 조기 종료
        ls.waitFor([&] { return ls.calls[callId].state == CallState::Disconnected; }, o.durationSec);
        st = eng.streamStats(callId);
        ci = ls.calls[callId];
        code = ci.lastCode; reason = ci.lastReason;
        if (ci.state != CallState::Disconnected) {
            eng.hangup(callId);
            ls.waitFor([&] { return ls.calls[callId].state == CallState::Disconnected; }, 5);
        }
        if (!st.valid || st.rxPackets == 0) { outcome = "no_media"; rc = 5; }
    } else if (o.cmd == "answer") {
        bool got = ls.waitFor([&] { return ls.haveIncoming; }, o.timeoutSec);
        if (!got) { summary(o, "no_incoming", -1, st, 0, "timeout"); eng.stop(); return 4; }
        callId = ls.incoming.callId;
        CallOptions co; co.video = ls.incoming.video && o.video;
        r = eng.answer(callId, co);
        if (!r.ok) { summary(o, "answer_failed", callId, st, r.code, r.reason); eng.stop(); return 4; }
        bool up = ls.waitFor([&] {
            auto it = ls.calls.find(callId);
            return it != ls.calls.end() && (it->second.state == CallState::Disconnected ||
                                            (it->second.state == CallState::Active && it->second.mediaActive));
        }, o.timeoutSec);
        if (!up) { summary(o, "call_timeout", callId, st, 0, "no media"); eng.hangup(callId); eng.stop(); return 4; }
        ls.waitFor([&] { return ls.calls[callId].state == CallState::Disconnected; }, o.durationSec);
        st = eng.streamStats(callId);
        CallInfo ci = ls.calls[callId];
        code = ci.lastCode; reason = ci.lastReason;
        if (ci.state != CallState::Disconnected) {
            eng.hangup(callId);
            ls.waitFor([&] { return ls.calls[callId].state == CallState::Disconnected; }, 5);
        }
        if (!st.valid || st.rxPackets == 0) { outcome = "no_media"; rc = 5; }
    }

    eng.unregisterAccount(acc);
    ls.waitFor([&] { return ls.reg.state == RegState::Unregistered || ls.reg.state == RegState::Failed; }, 5);
    eng.stop();
    summary(o, outcome, callId, st, code, reason);
    return rc;
}
