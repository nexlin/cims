// cimsue-cli — libcimsue 위의 헤드리스 UE (ue_sdk.md §4.7·§9)
//
// 실제 단말 스택(pjsua2 + 코어)으로 등록·1:1 호·MCPTT 그룹콜(floor)·MCData SDS 를 구동해 S3 검증 축을 제공한다.
// cspsim(시뮬레이터)과 달리 코덱·지터버퍼·SRTP·TLS·floor participant 를 단말과 같은 경로로 처리한다.
//
//   cimsue-cli [계정 옵션] register [--hold S]
//   cimsue-cli [계정 옵션] call <번호|sip:URI> [--duration S] [--video]
//   cimsue-cli [계정 옵션] answer [--duration S]
//   cimsue-cli [계정 옵션] group-call <groupId> [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency]
//   cimsue-cli [계정 옵션] sds <groupId> <text>            (MESSAGE 최종 응답까지 대기)
//   cimsue-cli [계정 옵션] sds-recv [--duration S]        (수신 SDS 를 JSON 줄로 출력)
//
// 계정 옵션: --server IP --port N --transport udp|tcp|tls --domain D --msisdn M (--imsi I | --auth-id IMPI)
//           (--ha1 HEX32 | --password P) [--mcptt-id tel:..] [--affiliate G[,G2]] [--srtp off|optional|required]
//           [--sec tls] [--tls-ca FILE] [--no-tls-verify] [--display-name NAME] [--log-level N] [--timeout S] [--json]
// 종료 코드: 0 성공 / 2 인자 / 3 등록 실패·시한 / 4 호 실패·시한 / 5 미디어 없음(수신 RTP 0) / 6 floor 미획득 / 7 SDS 실패
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
    std::string text;
    std::vector<std::string> affiliate;
    int durationSec = 8;
    int holdSec = 0;
    bool video = false;
    int pttAt = -1;
    int pttLen = 3;
    bool listenOnly = false;
    bool emergency = false;
};

void usage() {
    std::fprintf(stderr,
        "usage: cimsue-cli --server IP [--port N] [--transport udp|tcp|tls] --domain D --msisdn M\n"
        "                  (--imsi I | --auth-id IMPI) (--ha1 HEX | --password P) [--mcptt-id tel:..] [--affiliate G,..]\n"
        "                  [--srtp off|optional|required] [--sec tls] [--tls-ca FILE] [--no-tls-verify] [--display-name N]\n"
        "                  [--log-level N] [--timeout S] [--json]\n"
        "       register [--hold S] | call TARGET [--duration S] [--video] | answer [--duration S]\n"
        "       group-call GROUP [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency]\n"
        "       sds GROUP TEXT | sds-recv [--duration S]\n");
}

bool parse(int argc, char** argv, Opts& o) {
    std::vector<std::string> pos;
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
        else if (a == "--mcptt-id") { if (!next(v)) return false; o.acc.mcpttId = v; }
        else if (a == "--affiliate") { if (!next(v)) return false;
            std::stringstream ss(v); std::string g; while (std::getline(ss, g, ',')) if (!g.empty()) o.affiliate.push_back(g); }
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
        else if (a == "--ptt-at") { if (!next(v)) return false; o.pttAt = std::atoi(v.c_str()); }
        else if (a == "--ptt-len") { if (!next(v)) return false; o.pttLen = std::atoi(v.c_str()); }
        else if (a == "--video") o.video = true;
        else if (a == "--listen-only") o.listenOnly = true;
        else if (a == "--emergency") o.emergency = true;
        else if (a == "-h" || a == "--help") return false;
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
        else pos.push_back(a);
    }
    if (pos.empty()) return false;
    o.cmd = pos[0];
    if (o.cmd == "call" || o.cmd == "group-call") { if (pos.size() < 2) return false; o.target = pos[1]; }
    else if (o.cmd == "sds") { if (pos.size() < 3) return false; o.target = pos[1]; for (size_t i = 2; i < pos.size(); ++i) o.text += (i > 2 ? " " : "") + pos[i]; }
    else if (o.cmd != "register" && o.cmd != "answer" && o.cmd != "sds-recv") return false;
    return true;
}

/** 상태를 모아 조건 대기하는 리스너 — 이벤트 스레드가 쓰고 main 이 기다린다. */
class CliListener : public Listener {
public:
    CliListener(int logLevel, bool json) : logLevel_(logLevel), json_(json) {}
    void onLog(int level, const std::string& msg) override {
        if (level <= logLevel_) std::fprintf(stderr, "%s\n", msg.c_str());
    }
    void onRegState(const RegInfo& r) override {
        std::fprintf(stderr, "[cimsue-cli] reg acc=%d %s code=%d %s expires=%d\n", r.accountId, toString(r.state),
                     r.code, r.reason.c_str(), r.expiresSec);
        set([&] { reg = r; });
    }
    void onIncomingCall(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] incoming call=%d from=%s called=%s video=%d mcptt=%d group=%s\n", c.callId,
                     c.remoteUri.c_str(), c.calledParty.c_str(), c.video, c.isMcptt, c.groupId.c_str());
        set([&] { incoming = c; haveIncoming = true; calls[c.callId] = c; });
    }
    void onCallState(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] call=%d %s code=%d %s\n", c.callId, toString(c.state), c.lastCode, c.lastReason.c_str());
        set([&] { calls[c.callId] = c; });
    }
    void onCallMedia(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] call=%d media=%d state=%s\n", c.callId, c.mediaActive, toString(c.state));
        set([&] { calls[c.callId] = c; });
    }
    void onFloor(const FloorEvent& ev) override {
        std::string tk;
        for (auto& t : ev.talkers) tk += (t.self ? "*" : "") + t.id + " ";
        std::fprintf(stderr, "[cimsue-cli] floor call=%d %s state=%s dur=%d cause=%d(%s) perm=%d ind=0x%x talkers=[%s]\n", ev.callId,
                     toString(ev.kind), toString(ev.state), ev.durationSec, ev.cause, ev.causeText.c_str(), ev.permission,
                     ev.indicator, tk.c_str());
        set([&] {
            floorEvents.push_back(ev);
            if (ev.kind == FloorEvent::Kind::Granted) granted++;
            if (ev.kind == FloorEvent::Kind::Taken) taken++;
            if (ev.kind == FloorEvent::Kind::Denied) denied++;
        });
    }
    void onRoster(int, const std::string& g, const std::vector<RosterEntry>& users, bool full) override {
        std::string s;
        for (auto& u : users) s += u.uri + "=" + u.status + " ";
        std::fprintf(stderr, "[cimsue-cli] roster %s full=%d [%s]\n", g.c_str(), full, s.c_str());
        set([&] { rosters++; });
    }
    void onSds(const SdsMessage& m) override {
        std::fprintf(stderr, "[cimsue-cli] sds from=%s group=%s msg=%s notif=%d/%d text=%s\n", m.fromUri.c_str(),
                     m.groupUri.c_str(), m.msgId.c_str(), m.notification, m.notifType, m.text.c_str());
        if (json_)
            std::printf("{\"event\":\"sds\",\"from\":\"%s\",\"group\":\"%s\",\"conv_id\":\"%s\",\"msg_id\":\"%s\",\"notification\":%s,"
                        "\"notif_type\":%d,\"text\":\"%s\"}\n", m.fromUri.c_str(), m.groupUri.c_str(), m.convId.c_str(),
                        m.msgId.c_str(), m.notification ? "true" : "false", m.notifType, m.text.c_str());
        set([&] { sds.push_back(m); });
    }
    void onRequestResult(const RequestResult& r) override {
        std::fprintf(stderr, "[cimsue-cli] %s token=%ld → %d %s etag=%s\n", r.method.c_str(), r.token, r.code, r.reason.c_str(), r.etag.c_str());
        set([&] { results[r.token] = r; });
    }
    void onMessage(int, const std::string& from, const std::string& ct, const std::string& body) override {
        std::fprintf(stderr, "[cimsue-cli] message from=%s ct=%s len=%zu\n", from.c_str(), ct.c_str(), body.size());
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
    std::vector<FloorEvent> floorEvents;
    int granted = 0, taken = 0, denied = 0, rosters = 0;
    std::vector<SdsMessage> sds;
    std::map<long, RequestResult> results;

private:
    template <typename F> void set(F f) { { std::lock_guard<std::mutex> lk(m_); f(); } cv_.notify_all(); }
    int logLevel_;
    bool json_;
    std::mutex m_;
    std::condition_variable cv_;
};

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

struct Summary {
    std::string outcome = "ok";
    int callId = -1;
    StreamStats st;
    int code = 0;
    std::string reason;
    int granted = 0, taken = 0, denied = 0;
    std::string extra;               // 추가 JSON 필드 ("," 로 시작)
};

void print(const Opts& o, const Summary& s) {
    if (!o.json) {
        std::printf("%s: %s call=%d rx_pkts=%u tx_pkts=%u rx_loss=%u granted=%d taken=%d denied=%d code=%d %s\n", o.cmd.c_str(),
                    s.outcome.c_str(), s.callId, s.st.rxPackets, s.st.txPackets, s.st.rxLoss, s.granted, s.taken, s.denied,
                    s.code, s.reason.c_str());
        return;
    }
    std::printf("{\"cmd\":\"%s\",\"outcome\":\"%s\",\"msisdn\":\"%s\",\"call_id\":%d,\"rx_pkts\":%u,\"tx_pkts\":%u,"
                "\"rx_bytes\":%u,\"rx_loss\":%u,\"granted\":%d,\"taken\":%d,\"denied\":%d,\"code\":%d,\"reason\":\"%s\"%s}\n",
                o.cmd.c_str(), s.outcome.c_str(), o.acc.msisdn.c_str(), s.callId, s.st.rxPackets, s.st.txPackets, s.st.rxBytes,
                s.st.rxLoss, s.granted, s.taken, s.denied, s.code, s.reason.c_str(), s.extra.c_str());
}

bool waitActive(CliListener& ls, int callId, int timeoutSec) {
    return ls.waitFor([&] {
        auto it = ls.calls.find(callId);
        return it != ls.calls.end() && (it->second.state == CallState::Disconnected ||
                                        (it->second.state == CallState::Active && it->second.mediaActive));
    }, timeoutSec);
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

    CliListener ls(o.logLevel, o.json);
    Engine eng;
    Result r = eng.start(ec, &ls);
    if (!r.ok) { std::fprintf(stderr, "engine start failed: %d %s\n", r.code, r.reason.c_str()); return 3; }
    std::fprintf(stderr, "[cimsue-cli] %s\n", Engine::version().c_str());

    int acc = eng.addAccount(o.acc);
    if (acc < 0) { std::fprintf(stderr, "addAccount failed\n"); eng.stop(); return 3; }
    r = eng.registerAccount(acc);
    if (!r.ok) { std::fprintf(stderr, "register failed: %s\n", r.reason.c_str()); eng.stop(); return 3; }
    bool regOk = ls.waitFor([&] { return ls.reg.state == RegState::Registered || ls.reg.state == RegState::Failed; }, o.timeoutSec);
    Summary s;
    if (!regOk || ls.reg.state != RegState::Registered) {
        s.outcome = "register_failed"; s.code = ls.reg.code; s.reason = ls.reg.reason;
        print(o, s); eng.stop(); return 3;
    }
    s.code = ls.reg.code; s.reason = ls.reg.reason;

    // affiliation (PUBLISH Event: mcptt) — 그룹콜·SDS 수신 전제
    for (auto& g : o.affiliate) {
        long tok = eng.affiliate(acc, g, true);
        bool got = ls.waitFor([&] { return ls.results.count(tok) > 0; }, 10);
        if (!got || ls.results[tok].code / 100 != 2) {
            std::fprintf(stderr, "[cimsue-cli] affiliate %s failed (code=%d)\n", g.c_str(), got ? ls.results[tok].code : 0);
        }
    }

    int rc = 0;
    auto finish = [&](int callId) {
        if (callId >= 0) {
            s.st = eng.streamStats(callId);
            CallInfo ci = ls.calls.count(callId) ? ls.calls[callId] : CallInfo{};
            s.code = ci.lastCode; s.reason = ci.lastReason;
            if (ci.state != CallState::Disconnected) {
                eng.hangup(callId);
                ls.waitFor([&] { return ls.calls[callId].state == CallState::Disconnected; }, 5);
            }
        }
        for (auto& g : o.affiliate) eng.affiliate(acc, g, false);
        eng.unregisterAccount(acc);
        ls.waitFor([&] { return ls.reg.state == RegState::Unregistered || ls.reg.state == RegState::Failed; }, 5);
        eng.stop();
        s.granted = ls.granted; s.taken = ls.taken; s.denied = ls.denied;
        print(o, s);
        return rc;
    };

    if (o.cmd == "register") {
        if (o.holdSec > 0) std::this_thread::sleep_for(std::chrono::seconds(o.holdSec));
        s.outcome = "registered";
        return finish(-1);
    }

    if (o.cmd == "call") {
        CallOptions co; co.video = o.video;
        s.callId = eng.dial(acc, o.target, co);
        if (s.callId < 0) { s.outcome = "dial_failed"; rc = 4; return finish(-1); }
        bool up = waitActive(ls, s.callId, o.timeoutSec);
        CallInfo ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) { s.outcome = up ? "call_failed" : "call_timeout"; rc = 4; return finish(s.callId); }
        ls.waitFor([&] { return ls.calls[s.callId].state == CallState::Disconnected; }, o.durationSec);
        s.st = eng.streamStats(s.callId);
        if (!s.st.valid || s.st.rxPackets == 0) { s.outcome = "no_media"; rc = 5; }
        return finish(s.callId);
    }

    if (o.cmd == "answer") {
        bool got = ls.waitFor([&] { return ls.haveIncoming; }, o.timeoutSec);
        if (!got) { s.outcome = "no_incoming"; rc = 4; return finish(-1); }
        s.callId = ls.incoming.callId;
        if (!ls.incoming.isMcptt) {                                   // MCPTT 착신은 코어가 자동 수락
            CallOptions co; co.video = ls.incoming.video && o.video;
            r = eng.answer(s.callId, co);
            if (!r.ok) { s.outcome = "answer_failed"; s.code = r.code; s.reason = r.reason; rc = 4; return finish(s.callId); }
        }
        if (!waitActive(ls, s.callId, o.timeoutSec)) { s.outcome = "call_timeout"; rc = 4; return finish(s.callId); }
        ls.waitFor([&] { return ls.calls[s.callId].state == CallState::Disconnected; }, o.durationSec);
        s.st = eng.streamStats(s.callId);
        if (!s.st.valid || s.st.rxPackets == 0) { s.outcome = "no_media"; rc = 5; }
        return finish(s.callId);
    }

    if (o.cmd == "group-call") {
        GroupCallOptions go; go.listenOnly = o.listenOnly; go.emergency = o.emergency;
        s.callId = eng.joinGroupCall(acc, o.target, go);
        if (s.callId < 0) { s.outcome = "invite_failed"; rc = 4; return finish(-1); }
        bool up = waitActive(ls, s.callId, o.timeoutSec);
        CallInfo ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) { s.outcome = up ? "call_failed" : "call_timeout"; rc = 4; return finish(s.callId); }
        auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count(); };
        bool pttDone = o.pttAt < 0;
        bool disconnected = false;
        while (elapsed() < o.durationSec && !disconnected) {
            if (!pttDone && elapsed() >= o.pttAt) {
                pttDone = true;
                FloorInfo fi = eng.floorInfo(s.callId);
                if (fi.remotePort == 0) std::fprintf(stderr, "[cimsue-cli] floor remote not learned yet — request anyway\n");
                r = eng.floorRequest(s.callId);
                if (!r.ok) std::fprintf(stderr, "[cimsue-cli] floorRequest: %s\n", r.reason.c_str());
                bool decided = ls.waitFor([&] {
                    for (auto& e : ls.floorEvents)
                        if (e.callId == s.callId && (e.kind == FloorEvent::Kind::Granted || e.kind == FloorEvent::Kind::Denied ||
                                                     e.kind == FloorEvent::Kind::RequestTimeout || e.kind == FloorEvent::Kind::QueuePosition)) return true;
                    return false;
                }, 5);
                FloorInfo after = eng.floorInfo(s.callId);
                std::fprintf(stderr, "[cimsue-cli] floor after request: decided=%d state=%s granted=%u denied=%u\n", decided,
                             toString(after.state), after.grantedCount, after.denyCount);
                ls.waitFor([&] { return ls.calls[s.callId].state == CallState::Disconnected; }, o.pttLen);
                eng.floorRelease(s.callId);
            }
            disconnected = ls.waitFor([&] { return ls.calls[s.callId].state == CallState::Disconnected; }, 1);
        }
        FloorInfo fi = eng.floorInfo(s.callId);
        s.st = eng.streamStats(s.callId);
        s.extra = ",\"floor_local_port\":" + std::to_string(fi.localPort) + ",\"floor_remote\":\"" + fi.remoteIp + ":" +
                  std::to_string(fi.remotePort) + "\",\"rosters\":" + std::to_string(ls.rosters);
        if (o.pttAt >= 0 && ls.granted == 0) { s.outcome = "floor_not_granted"; rc = 6; }
        return finish(s.callId);
    }

    if (o.cmd == "sds") {
        std::string msgId = eng.sendGroupSds(acc, o.target, o.text);
        if (msgId.empty()) { s.outcome = "sds_send_failed"; rc = 7; return finish(-1); }
        // MESSAGE 최종 응답 — 토큰을 모르므로 마지막 MESSAGE 결과를 기다린다
        bool got = ls.waitFor([&] { for (auto& kv : ls.results) if (kv.second.method == "MESSAGE") return true; return false; }, o.timeoutSec);
        int code = 0;
        for (auto& kv : ls.results) if (kv.second.method == "MESSAGE") { code = kv.second.code; s.reason = kv.second.reason; }
        s.code = code;
        s.extra = ",\"msg_id\":\"" + msgId + "\"";
        if (!got || code / 100 != 2) { s.outcome = got ? "sds_rejected" : "sds_timeout"; rc = 7; }
        return finish(-1);
    }

    if (o.cmd == "sds-recv") {
        ls.waitFor([&] { return !ls.sds.empty() && (int)ls.sds.size() >= 1 && false; }, o.durationSec);   // duration 동안 수신
        s.extra = ",\"sds_received\":" + std::to_string(ls.sds.size());
        if (ls.sds.empty()) { s.outcome = "no_sds"; rc = 7; }
        return finish(-1);
    }
    return finish(-1);
}
