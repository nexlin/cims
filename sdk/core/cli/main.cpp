// cimsue-cli — libcimsue 위의 헤드리스 UE (ue_sdk.md §4.7·§9)
//
// 실제 단말 스택(pjsua2 + 코어)으로 등록·1:1 호·MCPTT 그룹콜(floor)·MCData SDS·관제(dialog 감시·Join 청취·픽업·전달)
// 를 구동해 S3 검증 축을 제공한다. cspsim(시뮬레이터)과 달리 코덱·지터버퍼·SRTP·TLS·floor participant 를 단말과
// 같은 경로로 처리한다.
//
//   cimsue-cli [계정 옵션] register [--hold S]
//   cimsue-cli [계정 옵션] call <번호|sip:URI> [--duration S] [--video]
//   cimsue-cli [계정 옵션] answer [--duration S] [--transfer-to X --transfer-after S]
//   cimsue-cli [계정 옵션] group-call <groupId> [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency]
//   cimsue-cli [계정 옵션] sds <groupId> <text>            (MESSAGE 최종 응답까지 대기)
//   cimsue-cli [계정 옵션] sds-recv [--duration S]        (수신 SDS 를 JSON 줄로 출력)
//   cimsue-cli [계정 옵션] dialog-watch <aor> [--duration S]      (RFC 4235 NOTIFY 를 JSON 줄로)
//   cimsue-cli [계정 옵션] join <aor> [--duration S]              (감시 → confirmed dialog 에 INVITE-Join recvonly)
//   cimsue-cli [계정 옵션] pickup [number] --code <피처코드> [--duration S]
//   cimsue-cli [계정 옵션] transfer <peer> --to <target> [--transfer-after S]   (peer 와 통화 후 REFER)
//   cimsue-cli --csc-host H [--csc-port 4430] --user U --pw P [--csc-ca FILE|--no-tls-verify] login
//   cimsue-cli [계정 옵션] drive                          (구동 모드 — stdin 명령 / stdout JSON 이벤트, 계측기 real-ue 풀이 쓴다)
//   (계정 옵션 대신 --from-profile volte|ptt 로 프로비저닝 프로파일에서 계정을 채울 수 있다)
//
// 구동 모드(drive): 엔진을 띄운 채 stdin 에서 한 줄 = 명령 하나(공백 구분 토큰)를 읽고, 진행은 stdout 에 한 줄 = JSON 이벤트 하나로 낸다
//   (test_instrument.md §3.3 — 워커가 프로세스를 가상 단말처럼 단계별로 구동한다). 등록은 자동으로 하지 않는다 — `register` 명령이 한다.
//   명령: register | unregister | dial <번호|URI> [video] | answer <call> [video] | reject <call> [code] | hangup <call> | hold <call> | resume <call>
//         dtmf <call> <digits> | transfer <call> <대상> | group_call <group> [listen] [emergency] | floor_request <call> | floor_release <call>
//         affiliate <group> on|off | pickup <code> [number] | stats [call] | quit
//   이벤트: {"event":"ready"} · reg{state,code,reason,rrd_ms} · incoming{call,from,called,video,mcptt,group} · call{call,dir,state,code,reason,media,
//         mcptt,video,by_us,srd_ms|sdd_ms,rx_pkts,tx_pkts,rx_loss,jitter_us} · floor{call,kind,subtype,t_us,cause,queue_position} · request{op,method,code,ms,on}
//         · stats{call,rx_pkts,tx_pkts,rx_loss,rx_bytes,jitter_us} · result{op,ok,call,code,reason}(명령마다 하나) · dialog · sds · exit
//
// 계정 옵션: --server IP --port N --transport udp|tcp|tls --domain D --msisdn M (--imsi I | --auth-id IMPI)
//           (--ha1 HEX32 | --password P) [--mcptt-id tel:..] [--affiliate G[,G2]] [--srtp off|optional|required]
//           [--sec tls] [--tls-ca FILE] [--no-tls-verify] [--display-name NAME] [--log-level N] [--timeout S] [--json]
// 종료 코드: 0 성공 / 2 인자 / 3 등록·로그인 실패 / 4 호 실패·시한 / 5 미디어 없음 / 6 floor 미획득 / 7 SDS 실패 / 8 관제 실패
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#else
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

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
    // 관제
    std::string code;                 // 픽업 피처코드
    std::string transferTo;
    int transferAfter = 2;
    // CSC
    std::string cscHost; int cscPort = 4430; std::string user, pw, cscCaFile, fromProfile;
    bool portSet = false, transportSet = false;
    // GMS 그룹 관리(group-put)
    std::string groupName;
    std::vector<std::string> groupMembers;
};

void usage() {
    std::fprintf(stderr,
        "usage: cimsue-cli [계정] <command> ...\n"
        "  계정: --server IP [--port N] [--transport udp|tcp|tls] --domain D --msisdn M (--imsi I | --auth-id IMPI)\n"
        "        (--ha1 HEX | --password P) [--mcptt-id tel:..] [--affiliate G,..] [--srtp off|optional|required] [--sec tls]\n"
        "        [--tls-ca FILE] [--no-tls-verify] [--display-name N] [--log-level N] [--timeout S] [--json]\n"
        "        또는 --csc-host H [--csc-port N] --user U --pw P [--csc-ca FILE] --from-profile volte|ptt\n"
        "  register [--hold S] | call TARGET [--duration S] [--video] | answer [--duration S] [--transfer-to X]\n"
        "  group-call GROUP [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency]\n"
        "  sds GROUP TEXT | sds-recv [--duration S] | login\n"
        "  dialog-watch AOR [--duration S] | join AOR [--duration S] | pickup [NUMBER] --code CODE | transfer PEER --to X\n"
        "  drive   (구동 모드 — stdin 명령 / stdout JSON 이벤트; 소스 머리 주석의 명령표)\n"
        "  groups | group-get URI | group-put URI --name N [--members tel:..,tel:..] | group-delete URI   (--csc-host --user --pw)\n");
}

bool parse(int argc, char** argv, Opts& o) {
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 >= argc) return false; out = argv[++i]; return true; };
        std::string v;
        auto opt = [&](const char* name, std::function<void(const std::string&)> f) {
            if (a != name) return false;
            if (!next(v)) throw std::runtime_error(std::string("missing value for ") + name);
            f(v); return true;
        };
        try {
            if (opt("--server", [&](const std::string& v) { o.acc.serverHost = v; })) continue;
            if (opt("--port", [&](const std::string& v) { o.acc.serverPort = std::atoi(v.c_str()); o.portSet = true; })) continue;
            if (opt("--transport", [&](const std::string& v) { o.acc.transport = v == "tls" ? Transport::TLS : v == "tcp" ? Transport::TCP : Transport::UDP; o.transportSet = true; })) continue;
            if (opt("--domain", [&](const std::string& v) { o.acc.domain = v; })) continue;
            if (opt("--msisdn", [&](const std::string& v) { o.acc.msisdn = v; })) continue;
            if (opt("--imsi", [&](const std::string& v) { o.acc.imsi = v; })) continue;
            if (opt("--auth-id", [&](const std::string& v) { o.acc.authId = v; })) continue;
            if (opt("--ha1", [&](const std::string& v) { o.acc.ha1 = v; })) continue;
            if (opt("--password", [&](const std::string& v) { o.acc.password = v; })) continue;
            if (opt("--display-name", [&](const std::string& v) { o.acc.displayName = v; })) continue;
            if (opt("--mcptt-id", [&](const std::string& v) { o.acc.mcpttId = v; })) continue;
            if (opt("--affiliate", [&](const std::string& v) { std::stringstream ss(v); std::string g; while (std::getline(ss, g, ',')) if (!g.empty()) o.affiliate.push_back(g); })) continue;
            if (opt("--srtp", [&](const std::string& v) { o.acc.mediaSecurity = v == "required" ? MediaSecurity::Required : v == "optional" ? MediaSecurity::Optional : MediaSecurity::Off; })) continue;
            if (opt("--sec", [&](const std::string& v) { std::stringstream ss(v); std::string m; while (std::getline(ss, m, ',')) if (!m.empty()) o.acc.secMechanisms.push_back(m); })) continue;
            if (opt("--tls-ca", [&](const std::string& v) { o.tlsCaFile = v; })) continue;
            if (opt("--log-level", [&](const std::string& v) { o.logLevel = std::atoi(v.c_str()); })) continue;
            if (opt("--timeout", [&](const std::string& v) { o.timeoutSec = std::atoi(v.c_str()); })) continue;
            if (opt("--duration", [&](const std::string& v) { o.durationSec = std::atoi(v.c_str()); })) continue;
            if (opt("--hold", [&](const std::string& v) { o.holdSec = std::atoi(v.c_str()); })) continue;
            if (opt("--ptt-at", [&](const std::string& v) { o.pttAt = std::atoi(v.c_str()); })) continue;
            if (opt("--ptt-len", [&](const std::string& v) { o.pttLen = std::atoi(v.c_str()); })) continue;
            if (opt("--code", [&](const std::string& v) { o.code = v; })) continue;
            if (opt("--to", [&](const std::string& v) { o.transferTo = v; })) continue;
            if (opt("--transfer-to", [&](const std::string& v) { o.transferTo = v; })) continue;
            if (opt("--transfer-after", [&](const std::string& v) { o.transferAfter = std::atoi(v.c_str()); })) continue;
            if (opt("--csc-host", [&](const std::string& v) { o.cscHost = v; })) continue;
            if (opt("--csc-port", [&](const std::string& v) { o.cscPort = std::atoi(v.c_str()); })) continue;
            if (opt("--user", [&](const std::string& v) { o.user = v; })) continue;
            if (opt("--pw", [&](const std::string& v) { o.pw = v; })) continue;
            if (opt("--csc-ca", [&](const std::string& v) { o.cscCaFile = v; })) continue;
            if (opt("--from-profile", [&](const std::string& v) { o.fromProfile = v; })) continue;
            if (opt("--name", [&](const std::string& v) { o.groupName = v; })) continue;
            if (opt("--members", [&](const std::string& v) { std::stringstream ss(v); std::string m; while (std::getline(ss, m, ',')) if (!m.empty()) o.groupMembers.push_back(m); })) continue;
        } catch (std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return false; }
        if (a == "--no-tls-verify") o.tlsVerify = false;
        else if (a == "--json") o.json = true;
        else if (a == "--video") o.video = true;
        else if (a == "--listen-only") o.listenOnly = true;
        else if (a == "--emergency") o.emergency = true;
        else if (a == "-h" || a == "--help") return false;
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
        else pos.push_back(a);
    }
    if (pos.empty()) return false;
    o.cmd = pos[0];
    static const char* needTarget[] = {"call", "group-call", "dialog-watch", "join", "transfer", "group-get", "group-put", "group-delete"};
    for (auto n : needTarget) if (o.cmd == n) { if (pos.size() < 2) return false; o.target = pos[1]; }
    if (o.cmd == "sds") { if (pos.size() < 3) return false; o.target = pos[1]; for (size_t i = 2; i < pos.size(); ++i) o.text += (i > 2 ? " " : "") + pos[i]; }
    if (o.cmd == "pickup") { if (pos.size() >= 2) o.target = pos[1]; if (o.code.empty()) return false; }
    if (o.cmd == "drive") o.json = true;   // 구동 모드는 언제나 JSON 이벤트
    if (o.cmd == "transfer" && o.transferTo.empty()) return false;
    static const char* known[] = {"register", "call", "answer", "group-call", "sds", "sds-recv", "login", "dialog-watch", "join", "pickup", "transfer",
                                  "groups", "group-get", "group-put", "group-delete", "drive"};
    bool ok = false;
    for (auto k : known) if (o.cmd == k) ok = true;
    return ok;
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
        std::string src;
        for (auto& s : c.sources) src += std::to_string(s.ssrc) + ":" + s.label + " ";
        std::fprintf(stderr, "[cimsue-cli] call=%d media=%d state=%s sources=[%s]\n", c.callId, c.mediaActive, toString(c.state), src.c_str());
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
    void onDialogInfo(const DialogInfo& d) override {
        std::fprintf(stderr, "[cimsue-cli] dialog watched=%s id=%s state=%s call-id=%s dir=%s remote=%s\n", d.watched.c_str(),
                     d.id.c_str(), d.state.c_str(), d.callId.c_str(), d.direction.c_str(), d.remoteIdentity.c_str());
        if (json_)
            std::printf("{\"event\":\"dialog\",\"watched\":\"%s\",\"state\":\"%s\",\"call_id\":\"%s\",\"direction\":\"%s\",\"remote\":\"%s\"}\n",
                        d.watched.c_str(), d.state.c_str(), d.callId.c_str(), d.direction.c_str(), d.remoteIdentity.c_str());
        set([&] { dialogs.push_back(d); });
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
        std::fprintf(stderr, "[cimsue-cli] %s token=%lld → %d %s etag=%s\n", r.method.c_str(), (long long)r.token, r.code, r.reason.c_str(), r.etag.c_str());
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
    std::vector<DialogInfo> dialogs;
    int granted = 0, taken = 0, denied = 0, rosters = 0;
    std::vector<SdsMessage> sds;
    std::map<int64_t, RequestResult> results;

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

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

/** dispatch 블록(dispatch_center.md §8.4) — members[]/pttTargets[] 는 서버 P2 반영 확인용으로 그대로 노출한다. */
std::string dispatchJson(const DispatchProfile& d) {
    std::string mem, tgt;
    for (auto& m : d.members)
        mem += std::string(mem.empty() ? "" : ",") + "{\"user_id\":\"" + jsonEsc(m.userId) + "\",\"name\":\"" + jsonEsc(m.name) + "\",\"volte_aor\":\"" +
               jsonEsc(m.volteAor) + "\",\"ptt_id\":\"" + jsonEsc(m.pttId) + "\",\"extension\":\"" + jsonEsc(m.extension) +
               "\",\"group_id\":\"" + jsonEsc(m.groupId) + "\"}";
    for (auto& t : d.pttTargets)
        tgt += std::string(tgt.empty() ? "" : ",") + "{\"id\":\"" + jsonEsc(t.id) + "\",\"uri\":\"" + jsonEsc(t.uri) + "\",\"name\":\"" + jsonEsc(t.name) + "\"}";
    return "{\"group_id\":\"" + jsonEsc(d.groupId) + "\",\"group_name\":\"" + jsonEsc(d.groupName) + "\",\"pilot_id\":\"" + jsonEsc(d.pilotId) +
           "\",\"monitor_scope\":\"" + d.monitorScope + "\",\"ptt_listen\":\"" + d.pttListen + "\",\"listen_visibility\":\"" + d.listenVisibility +
           "\",\"directory_admin\":\"" + d.directoryAdmin + "\",\"org_code\":\"" + jsonEsc(d.orgCode) +
           "\",\"members\":[" + mem + "],\"ptt_targets\":[" + tgt + "]}";
}

/** CSC 로그인 + 프로파일. 반환 0 성공, 그 외 종료코드. */
int cscLogin(const Opts& o, Profile& prof, TokenSet& tok) {
    CscEndpoint ep;
    ep.host = o.cscHost; ep.port = o.cscPort; ep.verifyServer = o.tlsVerify;
    if (!o.cscCaFile.empty()) ep.caPem = readFile(o.cscCaFile); else if (!o.tlsCaFile.empty()) ep.caPem = readFile(o.tlsCaFile);
    CscClient csc(ep);
    Result r = csc.login(o.user, o.pw, tok);
    if (!r.ok) { std::fprintf(stderr, "[cimsue-cli] login failed: %s\n", r.reason.c_str()); return 3; }
    r = csc.fetchProfile(tok.accessToken, prof);
    if (!r.ok) { std::fprintf(stderr, "[cimsue-cli] provisioning/me failed: %s\n", r.reason.c_str()); return 3; }
    return 0;
}

// ── 구동 모드(drive) — 계측기 real-ue 풀 (test_instrument.md §3.3) ─────────────────────────────────────────────────────
// stdout 은 한 줄 = JSON 이벤트 하나. 이벤트 스레드(Listener)·stats 스레드·main(명령 응답) 이 함께 쓰므로 줄 단위로 잠근다.
// 시각은 단말 프로세스 안에서 잰다 — rrd_ms(registerAccount → Registered) · srd_ms(dial → Active) · sdd_ms(hangup → Disconnected).
std::mutex g_outMtx;
void outLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_outMtx);
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
long long nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
long long msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}
std::string statsJson(const StreamStats& st) {
    return ",\"rx_pkts\":" + std::to_string(st.rxPackets) + ",\"tx_pkts\":" + std::to_string(st.txPackets) + ",\"rx_loss\":" + std::to_string(st.rxLoss) +
           ",\"rx_bytes\":" + std::to_string(st.rxBytes) + ",\"jitter_us\":" + std::to_string(st.rxJitterUs) + ",\"stats_valid\":" + (st.valid ? "true" : "false");
}

class DriveListener : public Listener {
public:
    explicit DriveListener(int logLevel) : logLevel_(logLevel) {}
    Engine* eng = nullptr;
    void onLog(int level, const std::string& msg) override { if (level <= logLevel_) std::fprintf(stderr, "%s\n", msg.c_str()); }
    void onRegState(const RegInfo& r) override {
        long long ms = -1;
        { std::lock_guard<std::mutex> lk(m_); if (regPending_) { ms = msSince(tReg_); if (r.state != RegState::Registering) regPending_ = false; } }
        const char* st = r.state == RegState::Registered ? "registered" : r.state == RegState::Failed ? "failed" : r.state == RegState::Registering ? "registering" : "unregistered";
        outLine("{\"event\":\"reg\",\"state\":\"" + std::string(st) + "\",\"code\":" + std::to_string(r.code) + ",\"reason\":\"" + jsonEsc(r.reason) +
                "\",\"expires\":" + std::to_string(r.expiresSec) + ",\"rrd_ms\":" + std::to_string(ms) + "}");
    }
    void onIncomingCall(const CallInfo& c) override {
        { std::lock_guard<std::mutex> lk(m_); active_.insert(c.callId); }
        outLine("{\"event\":\"incoming\",\"call\":" + std::to_string(c.callId) + ",\"from\":\"" + jsonEsc(c.remoteUri) + "\",\"called\":\"" + jsonEsc(c.calledParty) +
                "\",\"video\":" + (c.video ? "true" : "false") + ",\"mcptt\":" + (c.isMcptt ? "true" : "false") + ",\"group\":\"" + jsonEsc(c.groupId) + "\"}");
    }
    void onCallState(const CallInfo& c) override {
        const char* st = c.state == CallState::Outgoing ? "outgoing" : c.state == CallState::Incoming ? "incoming" : c.state == CallState::Active ? "active"
                       : c.state == CallState::Held ? "held" : c.state == CallState::Disconnected ? "disconnected" : "null";
        std::string extra;
        bool byUs = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            auto d = tDial_.find(c.callId);
            if (c.state == CallState::Active && d != tDial_.end()) { extra += ",\"srd_ms\":" + std::to_string(msSince(d->second)); tDial_.erase(d); }
            auto h = tHangup_.find(c.callId);
            if (c.state == CallState::Disconnected) {
                if (h != tHangup_.end()) { byUs = true; extra += ",\"sdd_ms\":" + std::to_string(msSince(h->second)); tHangup_.erase(h); }
                tDial_.erase(c.callId);
                active_.erase(c.callId);
            }
        }
        if (c.state == CallState::Disconnected && eng) extra += statsJson(eng->streamStats(c.callId));   // 소멸 시점의 최종 통계
        outLine("{\"event\":\"call\",\"call\":" + std::to_string(c.callId) + ",\"dir\":\"" + (c.dir == CallDir::Outgoing ? "out" : "in") + "\",\"state\":\"" + st +
                "\",\"code\":" + std::to_string(c.lastCode) + ",\"reason\":\"" + jsonEsc(c.lastReason) + "\",\"media\":" + (c.mediaActive ? "true" : "false") +
                ",\"mcptt\":" + (c.isMcptt ? "true" : "false") + ",\"video\":" + (c.video ? "true" : "false") + ",\"by_us\":" + (byUs ? "true" : "false") +
                ",\"group\":\"" + jsonEsc(c.groupId) + "\"" + extra + "}");
    }
    void onFloor(const FloorEvent& ev) override {
        // TS 24.380 §8.2 subtype 로도 낸다(워커가 가상 단말과 같은 표로 센다) — Granted 1 · Taken 2 · Deny 3 · Idle 5 · Revoke 6 · Queue Position Info 9
        const char* k = "other"; int sub = -1;
        switch (ev.kind) {
        case FloorEvent::Kind::Granted: k = "granted"; sub = 1; break;
        case FloorEvent::Kind::Taken: k = "taken"; sub = 2; break;
        case FloorEvent::Kind::Denied: k = "denied"; sub = 3; break;
        case FloorEvent::Kind::Idle: k = "idle"; sub = 5; break;
        case FloorEvent::Kind::Revoked: k = "revoked"; sub = 6; break;
        case FloorEvent::Kind::QueuePosition: k = "queue"; sub = 9; break;
        case FloorEvent::Kind::QueueCancelled: k = "queue_cancelled"; break;
        case FloorEvent::Kind::RequestTimeout: k = "request_timeout"; break;
        case FloorEvent::Kind::TalkerLeft: k = "talker_left"; break;
        case FloorEvent::Kind::TalkLimit: k = "talk_limit"; break;
        default: break;
        }
        outLine("{\"event\":\"floor\",\"call\":" + std::to_string(ev.callId) + ",\"kind\":\"" + k + "\",\"subtype\":" + std::to_string(sub) + ",\"t_us\":" + std::to_string(nowUs()) +
                ",\"cause\":" + std::to_string(ev.cause) + ",\"queue_position\":" + std::to_string(ev.queuePosition) + ",\"duration\":" + std::to_string(ev.durationSec) + "}");
    }
    void onRoster(int, const std::string& g, const std::vector<RosterEntry>& users, bool full) override {
        outLine("{\"event\":\"roster\",\"group\":\"" + jsonEsc(g) + "\",\"full\":" + (full ? "true" : "false") + ",\"users\":" + std::to_string(users.size()) + "}");
    }
    void onDialogInfo(const DialogInfo& d) override {
        outLine("{\"event\":\"dialog\",\"watched\":\"" + jsonEsc(d.watched) + "\",\"state\":\"" + d.state + "\",\"call_id\":\"" + jsonEsc(d.callId) + "\",\"direction\":\"" +
                d.direction + "\",\"remote\":\"" + jsonEsc(d.remoteIdentity) + "\"}");
    }
    void onSds(const SdsMessage& m) override {
        outLine("{\"event\":\"sds\",\"from\":\"" + jsonEsc(m.fromUri) + "\",\"group\":\"" + jsonEsc(m.groupUri) + "\",\"msg_id\":\"" + jsonEsc(m.msgId) + "\",\"notification\":" +
                (m.notification ? "true" : "false") + ",\"text\":\"" + jsonEsc(m.text) + "\"}");
    }
    void onRequestResult(const RequestResult& r) override {
        std::string op, on;
        long long ms = -1;
        {
            std::lock_guard<std::mutex> lk(m_);
            auto it = tokens_.find(r.token);
            if (it != tokens_.end()) { op = it->second.op; on = it->second.on ? "true" : "false"; ms = msSince(it->second.t); tokens_.erase(it); }
        }
        outLine("{\"event\":\"request\",\"method\":\"" + jsonEsc(r.method) + "\",\"op\":\"" + op + "\",\"on\":" + (on.empty() ? "null" : on) + ",\"code\":" + std::to_string(r.code) +
                ",\"reason\":\"" + jsonEsc(r.reason) + "\",\"ms\":" + std::to_string(ms) + ",\"token\":" + std::to_string((long long)r.token) + "}");
    }
    void onEngineStopped() override { outLine("{\"event\":\"engine_stopped\"}"); }

    // main 스레드가 명령 시각을 기록한다
    void markRegister() { std::lock_guard<std::mutex> lk(m_); tReg_ = std::chrono::steady_clock::now(); regPending_ = true; }
    void markDial(int callId) { std::lock_guard<std::mutex> lk(m_); tDial_[callId] = std::chrono::steady_clock::now(); active_.insert(callId); }
    void markHangup(int callId) { std::lock_guard<std::mutex> lk(m_); tHangup_[callId] = std::chrono::steady_clock::now(); }
    void markToken(int64_t tok, const std::string& op, bool on) { std::lock_guard<std::mutex> lk(m_); tokens_[tok] = { op, on, std::chrono::steady_clock::now() }; }
    std::vector<int> activeCalls() { std::lock_guard<std::mutex> lk(m_); return std::vector<int>(active_.begin(), active_.end()); }

private:
    struct Tok { std::string op; bool on; std::chrono::steady_clock::time_point t; };
    int logLevel_;
    std::mutex m_;
    bool regPending_ = false;
    std::chrono::steady_clock::time_point tReg_;
    std::map<int, std::chrono::steady_clock::time_point> tDial_, tHangup_;
    std::map<int64_t, Tok> tokens_;
    std::set<int> active_;
};

/** 구동 루프 — stdin 한 줄 = 명령 하나. 명령마다 result 이벤트 하나(동기 결과 — dial/group_call 은 call id). EOF 또는 quit 에 엔진을 내린다. */
int driveLoop(Engine& eng, DriveListener& ls, int acc, const Opts& o) {
    std::atomic<bool> stop{false};
    // stats 스레드 — 활성 호마다 1 초 간격으로 RTP/RTCP 통계(워커의 media_hold 표본 원천)
    std::thread stats([&] {
        while (!stop) {
            for (int i = 0; i < 10 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop) break;
            for (int id : ls.activeCalls()) {
                StreamStats st = eng.streamStats(id);
                if (st.valid) outLine("{\"event\":\"stats\",\"call\":" + std::to_string(id) + statsJson(st) + "}");
            }
        }
    });
    auto result = [&](const std::string& op, bool ok, int call, int code, const std::string& reason) {
        outLine("{\"event\":\"result\",\"op\":\"" + op + "\",\"ok\":" + (ok ? "true" : "false") + ",\"call\":" + std::to_string(call) + ",\"code\":" + std::to_string(code) +
                ",\"reason\":\"" + jsonEsc(reason) + "\"}");
    };
    auto res = [&](const std::string& op, const Result& r, int call = -1) { result(op, r.ok, call, r.code, r.reason); };
    outLine("{\"event\":\"ready\",\"version\":\"" + jsonEsc(Engine::version()) + "\",\"aor\":\"" + jsonEsc(o.acc.aor()) + "\"}");
    std::string line;
    bool quit = false;
    while (!quit && std::getline(std::cin, line)) {
        std::vector<std::string> tk;
        { std::stringstream ss(line); std::string t; while (ss >> t) tk.push_back(t); }
        if (tk.empty()) continue;
        const std::string& op = tk[0];
        auto arg = [&](size_t i) { return i < tk.size() ? tk[i] : std::string(); };
        auto argi = [&](size_t i, int def) { return i < tk.size() ? std::atoi(tk[i].c_str()) : def; };
        auto has = [&](const char* flag) { for (size_t i = 1; i < tk.size(); ++i) if (tk[i] == flag) return true; return false; };
        if (op == "quit") { quit = true; result(op, true, -1, 0, ""); }
        else if (op == "register") { ls.markRegister(); res(op, eng.registerAccount(acc)); }
        else if (op == "unregister") { res(op, eng.unregisterAccount(acc)); }
        else if (op == "dial") {
            CallOptions co; co.video = has("video");
            int id = eng.dial(acc, arg(1), co);
            if (id >= 0) ls.markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "dial refused");
        } else if (op == "answer") { CallOptions co; co.video = has("video"); res(op, eng.answer(argi(1, -1), co), argi(1, -1)); }
        else if (op == "reject") { res(op, eng.reject(argi(1, -1), argi(2, 486)), argi(1, -1)); }
        else if (op == "hangup") { ls.markHangup(argi(1, -1)); res(op, eng.hangup(argi(1, -1)), argi(1, -1)); }
        else if (op == "hold") { res(op, eng.hold(argi(1, -1)), argi(1, -1)); }
        else if (op == "resume") { res(op, eng.resume(argi(1, -1)), argi(1, -1)); }
        else if (op == "dtmf") { res(op, eng.sendDtmf(argi(1, -1), arg(2)), argi(1, -1)); }
        else if (op == "transfer") { res(op, eng.transfer(argi(1, -1), arg(2)), argi(1, -1)); }
        else if (op == "group_call") {
            GroupCallOptions go; go.listenOnly = has("listen"); go.emergency = has("emergency");
            int id = eng.joinGroupCall(acc, arg(1), go);
            if (id >= 0) ls.markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "group call refused");
        } else if (op == "floor_request") { res(op, eng.floorRequest(argi(1, -1)), argi(1, -1)); }
        else if (op == "floor_release") { res(op, eng.floorRelease(argi(1, -1)), argi(1, -1)); }
        else if (op == "affiliate") {
            bool on = arg(2) != "off";
            int64_t tok = eng.affiliate(acc, arg(1), on);
            if (tok >= 0) ls.markToken(tok, op, on);
            result(op, tok >= 0, -1, 0, tok >= 0 ? "" : "affiliate refused");
        } else if (op == "pickup") {
            int id = eng.pickup(acc, arg(1), arg(2));
            if (id >= 0) ls.markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "pickup refused");
        } else if (op == "stats") {
            std::vector<int> ids = tk.size() > 1 ? std::vector<int>{ argi(1, -1) } : ls.activeCalls();
            for (int id : ids) outLine("{\"event\":\"stats\",\"call\":" + std::to_string(id) + statsJson(eng.streamStats(id)) + "}");
            result(op, true, -1, 0, "");
        } else result(op, false, -1, 0, "unknown command");
    }
    stop = true;
    stats.join();
    for (int id : ls.activeCalls()) eng.hangup(id);
    eng.unregisterAccount(acc);
    eng.stop();
    outLine("{\"event\":\"exit\"}");
    return 0;
}

}  // namespace

// 진단 — SIGSEGV/SIGABRT 시 백트레이스(-rdynamic 심볼)를 stderr 로. 실기기 없는 개발 서버에 gdb 가 없어 필요하다.
// glibc 전용(execinfo) — Windows 빌드는 디버거/WER 에 맡긴다.
#ifndef _WIN32
static void crashHandler(int sig) {
    void* frames[64];
    int n = backtrace(frames, 64);
    std::fprintf(stderr, "\n[cimsue-cli] fatal signal %d — backtrace:\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    _exit(128 + sig);
}
#endif

int main(int argc, char** argv) {
#ifndef _WIN32
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
#else
    // Windows 콘솔은 argv 를 ANSI(CP949)로 넘긴다 — 한글 그룹명·표시명이 서버에 깨져 저장되지 않도록 UTF-8 로 다시 받는다. 출력도 UTF-8.
    SetConsoleOutputCP(CP_UTF8);
    static std::vector<std::string> utf8Args;
    static std::vector<char*> utf8Argv;
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
            utf8Args.push_back(std::move(s));
        }
        LocalFree(wargv);
        for (auto& s : utf8Args) utf8Argv.push_back(s.data());
        utf8Argv.push_back(nullptr);
        argc = wargc;
        argv = utf8Argv.data();
    }
#endif
    Opts o;
    if (!parse(argc, argv, o)) { usage(); return 2; }

    // ── CSC 로그인 / 프로파일 (login 명령 또는 --from-profile) ──
    const bool groupCmd = o.cmd == "groups" || o.cmd == "group-get" || o.cmd == "group-put" || o.cmd == "group-delete";
    if (o.cmd == "login" || groupCmd || !o.fromProfile.empty()) {
        if (o.cscHost.empty() || o.user.empty()) { std::fprintf(stderr, "need --csc-host --user --pw\n"); return 2; }
        Profile prof; TokenSet tok;
        int rc = cscLogin(o, prof, tok);
        if (rc) return rc;
        if (groupCmd) {
            // GMS 그룹 관리(TS 24.481 XCAP) — 자기 트리(ptt 서비스 mcptt_id)에서 목록/문서/PUT/DELETE. 출력은 JSON 한 줄.
            const ServiceProfile* ptt = prof.service("ptt");
            std::string me = ptt ? (ptt->mcpttId.empty() ? "tel:" + ptt->msisdn : ptt->mcpttId) : "";
            if (me.empty()) { std::fprintf(stderr, "[cimsue-cli] profile has no ptt service\n"); return 3; }
            CscEndpoint ep; ep.host = o.cscHost; ep.port = o.cscPort; ep.verifyServer = o.tlsVerify;
            if (!o.cscCaFile.empty()) ep.caPem = readFile(o.cscCaFile); else if (!o.tlsCaFile.empty()) ep.caPem = readFile(o.tlsCaFile);
            CscClient csc(ep);
            auto printDoc = [&](const char* cmd, const GroupDoc& d) {
                std::string mem;
                for (auto& m : d.members) mem += std::string(mem.empty() ? "" : ",") + "{\"uri\":\"" + jsonEsc(m.uri) + "\",\"name\":\"" + jsonEsc(m.name) + "\",\"role\":\"" + m.role + "\"}";
                std::printf("{\"cmd\":\"%s\",\"outcome\":\"ok\",\"uri\":\"%s\",\"name\":\"%s\",\"etag\":\"%s\",\"session_type\":\"%s\",\"authorized_user\":\"%s\",\"members\":[%s]}\n",
                            cmd, jsonEsc(d.uri).c_str(), jsonEsc(d.displayName).c_str(), jsonEsc(d.etag).c_str(), d.sessionType.c_str(),
                            jsonEsc(d.authorizedUser).c_str(), mem.c_str());
            };
            Result r;
            if (o.cmd == "groups") {
                std::vector<GroupSummary> gs;
                r = csc.listGroups(tok.accessToken, me, gs);
                if (r.ok) {
                    std::string items;
                    for (auto& g : gs) items += std::string(items.empty() ? "" : ",") + "{\"uri\":\"" + jsonEsc(g.uri) + "\",\"name\":\"" + jsonEsc(g.displayName) +
                                                "\",\"members\":" + std::to_string(g.memberCount) + ",\"owner\":" + (g.isOwner ? "true" : "false") + "}";
                    std::printf("{\"cmd\":\"groups\",\"outcome\":\"ok\",\"user\":\"%s\",\"allow_group_creation\":%s,\"groups\":[%s]}\n",
                                jsonEsc(me).c_str(), prof.allowGroupCreation ? "true" : "false", items.c_str());
                }
            } else if (o.cmd == "group-get") {
                GroupDoc d;
                r = csc.getGroup(tok.accessToken, me, o.target, d);
                if (r.ok) printDoc("group-get", d);
            } else if (o.cmd == "group-put") {
                GroupDoc d, out;
                r = csc.getGroup(tok.accessToken, me, o.target, d);           // 기존 문서면 수정(etag 조건부), 없으면 신규
                std::string ifMatch = r.ok ? d.etag : std::string();
                if (!r.ok) { d = GroupDoc(); d.uri = o.target; }
                if (!o.groupName.empty()) d.displayName = o.groupName;
                if (d.displayName.empty()) d.displayName = o.target;
                if (!o.groupMembers.empty()) {
                    d.members.clear();
                    for (auto& m : o.groupMembers) { GroupMember gm; gm.uri = m; d.members.push_back(gm); }
                }
                if (d.members.empty()) { GroupMember gm; gm.uri = me; gm.role = "chair"; d.members.push_back(gm); }
                r = csc.putGroup(tok.accessToken, me, d, ifMatch, out);
                if (r.ok) printDoc("group-put", out);
            } else {
                r = csc.deleteGroup(tok.accessToken, me, o.target);
                if (r.ok) std::printf("{\"cmd\":\"group-delete\",\"outcome\":\"ok\",\"uri\":\"%s\"}\n", jsonEsc(o.target).c_str());
            }
            if (!r.ok) {
                std::printf("{\"cmd\":\"%s\",\"outcome\":\"failed\",\"code\":%d,\"reason\":\"%s\"}\n", o.cmd.c_str(), r.code, jsonEsc(r.reason).c_str());
                return r.code == 403 ? 8 : 3;
            }
            return 0;
        }
        if (o.cmd == "login") {
            std::string svcs;
            for (auto& s : prof.services)
                svcs += std::string(svcs.empty() ? "" : ",") + "{\"kind\":\"" + s.kind + "\",\"sip\":\"" + s.sipHost + ":" + std::to_string(s.sipPort) +
                        "/" + toString(s.transport) + "\",\"domain\":\"" + s.domain + "\",\"msisdn\":\"" + s.msisdn + "\",\"imsi\":\"" + s.imsi +
                        "\",\"ha1\":" + (s.sipHa1.empty() ? "false" : "true") + ",\"mcptt_id\":\"" + s.mcpttId + "\",\"media_security\":" +
                        std::to_string((int)s.mediaSecurity) + ",\"enforced\":" + (s.enforced ? "true" : "false") + "}";
            std::printf("{\"cmd\":\"login\",\"outcome\":\"ok\",\"login_id\":\"%s\",\"display_name\":\"%s\",\"country\":\"%s\",\"services\":[%s],"
                        "\"dispatch\":%s}\n", jsonEsc(prof.loginId).c_str(), jsonEsc(prof.displayName).c_str(), prof.countryCode.c_str(), svcs.c_str(),
                        prof.dispatch.present ? dispatchJson(prof.dispatch).c_str() : "null");
            return 0;
        }
        const ServiceProfile* sp = prof.service(o.fromProfile);
        if (!sp) { std::fprintf(stderr, "[cimsue-cli] profile has no service '%s'\n", o.fromProfile.c_str()); return 3; }
        AccountConfig a = sp->toAccount(o.pw);
        if (!o.acc.serverHost.empty()) a.serverHost = o.acc.serverHost;      // 명시 인자가 프로파일을 덮는다
        if (o.portSet) a.serverPort = o.acc.serverPort;
        if (o.transportSet) a.transport = o.acc.transport;
        if (o.acc.mediaSecurity != MediaSecurity::Off) a.mediaSecurity = o.acc.mediaSecurity;
        o.acc = a;
        std::fprintf(stderr, "[cimsue-cli] provisioned %s: %s via %s:%d/%s ha1=%d dispatch=%s\n", sp->kind.c_str(), a.aor().c_str(),
                     a.serverHost.c_str(), a.serverPort, toString(a.transport), !a.ha1.empty(), prof.dispatch.groupId.c_str());
    }
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

    if (o.cmd == "drive") {
        DriveListener dl(o.logLevel);
        Engine eng;
        dl.eng = &eng;
        Result r = eng.start(ec, &dl);
        if (!r.ok) { outLine("{\"event\":\"exit\",\"error\":\"engine start failed: " + jsonEsc(r.reason) + "\"}"); return 3; }
        int acc = eng.addAccount(o.acc);
        if (acc < 0) { outLine("{\"event\":\"exit\",\"error\":\"addAccount failed\"}"); eng.stop(); return 3; }
        return driveLoop(eng, dl, acc, o);
    }

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

    for (auto& g : o.affiliate) {
        int64_t tok = eng.affiliate(acc, g, true);
        bool got = ls.waitFor([&] { return ls.results.count(tok) > 0; }, 10);
        if (!got || ls.results[tok].code / 100 != 2)
            std::fprintf(stderr, "[cimsue-cli] affiliate %s failed (code=%d)\n", g.c_str(), got ? ls.results[tok].code : 0);
    }

    int rc = 0;
    auto disconnected = [&](int callId) { auto it = ls.calls.find(callId); return it != ls.calls.end() && it->second.state == CallState::Disconnected; };
    auto finish = [&](int callId) {
        if (callId >= 0) {
            s.st = eng.streamStats(callId);
            CallInfo ci = ls.calls.count(callId) ? ls.calls[callId] : CallInfo{};
            s.code = ci.lastCode; s.reason = ci.lastReason;
            if (ci.state != CallState::Disconnected) {
                eng.hangup(callId);
                ls.waitFor([&] { return disconnected(callId); }, 5);
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
    auto mediaCheck = [&](int callId) {
        s.st = eng.streamStats(callId);
        if (!s.st.valid || s.st.rxPackets == 0) { s.outcome = "no_media"; rc = 5; }
    };

    if (o.cmd == "register") {
        if (o.holdSec > 0) std::this_thread::sleep_for(std::chrono::seconds(o.holdSec));
        s.outcome = "registered";
        return finish(-1);
    }

    if (o.cmd == "call" || o.cmd == "pickup") {
        CallOptions co; co.video = o.video;
        s.callId = o.cmd == "call" ? eng.dial(acc, o.target, co) : eng.pickup(acc, o.code, o.target);
        if (s.callId < 0) { s.outcome = "dial_failed"; rc = 4; return finish(-1); }
        bool up = waitActive(ls, s.callId, o.timeoutSec);
        CallInfo ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) {
            s.outcome = up ? (o.cmd == "pickup" ? "pickup_rejected" : "call_failed") : "call_timeout"; rc = o.cmd == "pickup" ? 8 : 4;
            return finish(s.callId);
        }
        ls.waitFor([&] { return disconnected(s.callId); }, o.durationSec);
        mediaCheck(s.callId);
        return finish(s.callId);
    }

    if (o.cmd == "answer") {
        bool got = ls.waitFor([&] { return ls.haveIncoming; }, o.timeoutSec);
        if (!got) { s.outcome = "no_incoming"; rc = 4; return finish(-1); }
        s.callId = ls.incoming.callId;
        if (!ls.incoming.isMcptt) {
            CallOptions co; co.video = ls.incoming.video && o.video;
            r = eng.answer(s.callId, co);
            if (!r.ok) { s.outcome = "answer_failed"; s.code = r.code; s.reason = r.reason; rc = 4; return finish(s.callId); }
        }
        if (!waitActive(ls, s.callId, o.timeoutSec)) { s.outcome = "call_timeout"; rc = 4; return finish(s.callId); }
        if (!o.transferTo.empty()) {                                      // 착신 후 blind transfer
            ls.waitFor([&] { return disconnected(s.callId); }, o.transferAfter);
            r = eng.transfer(s.callId, o.transferTo);
            std::fprintf(stderr, "[cimsue-cli] REFER → %s: %s\n", o.transferTo.c_str(), r.ok ? "sent" : r.reason.c_str());
            if (!r.ok) { s.outcome = "transfer_failed"; rc = 8; return finish(s.callId); }
            bool ended = ls.waitFor([&] { return disconnected(s.callId); }, o.durationSec);
            s.extra = std::string(",\"transferred\":") + (ended ? "true" : "false");
            if (!ended) { s.outcome = "transfer_not_completed"; rc = 8; }
            return finish(s.callId);
        }
        ls.waitFor([&] { return disconnected(s.callId); }, o.durationSec);
        mediaCheck(s.callId);
        return finish(s.callId);
    }

    if (o.cmd == "transfer") {                                            // peer 와 통화 후 REFER --to
        s.callId = eng.dial(acc, o.target);
        if (s.callId < 0) { s.outcome = "dial_failed"; rc = 4; return finish(-1); }
        if (!waitActive(ls, s.callId, o.timeoutSec) || ls.calls[s.callId].state != CallState::Active) { s.outcome = "call_failed"; rc = 4; return finish(s.callId); }
        ls.waitFor([&] { return disconnected(s.callId); }, o.transferAfter);
        r = eng.transfer(s.callId, o.transferTo);
        std::fprintf(stderr, "[cimsue-cli] REFER → %s: %s\n", o.transferTo.c_str(), r.ok ? "sent" : r.reason.c_str());
        if (!r.ok) { s.outcome = "transfer_failed"; rc = 8; return finish(s.callId); }
        bool ended = ls.waitFor([&] { return disconnected(s.callId); }, o.durationSec);
        s.extra = std::string(",\"transferred\":") + (ended ? "true" : "false");
        if (!ended) { s.outcome = "transfer_not_completed"; rc = 8; }
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
        bool gone = false;
        while (elapsed() < o.durationSec && !gone) {
            if (!pttDone && elapsed() >= o.pttAt) {
                pttDone = true;
                r = eng.floorRequest(s.callId);
                if (!r.ok) std::fprintf(stderr, "[cimsue-cli] floorRequest: %s\n", r.reason.c_str());
                ls.waitFor([&] {
                    for (auto& e : ls.floorEvents)
                        if (e.callId == s.callId && (e.kind == FloorEvent::Kind::Granted || e.kind == FloorEvent::Kind::Denied ||
                                                     e.kind == FloorEvent::Kind::RequestTimeout || e.kind == FloorEvent::Kind::QueuePosition)) return true;
                    return false;
                }, 5);
                ls.waitFor([&] { return disconnected(s.callId); }, o.pttLen);
                eng.floorRelease(s.callId);
            }
            gone = ls.waitFor([&] { return disconnected(s.callId); }, 1);
        }
        FloorInfo fi = eng.floorInfo(s.callId);
        s.st = eng.streamStats(s.callId);
        s.extra = ",\"floor_local_port\":" + std::to_string(fi.localPort) + ",\"floor_remote\":\"" + fi.remoteIp + ":" +
                  std::to_string(fi.remotePort) + "\",\"rosters\":" + std::to_string(ls.rosters);
        if (o.pttAt >= 0 && ls.granted == 0) { s.outcome = "floor_not_granted"; rc = 6; }
        return finish(s.callId);
    }

    if (o.cmd == "sds") {
        SdsSend sds = eng.sendGroupSds(acc, o.target, o.text);
        if (!sds.ok) { s.outcome = "sds_send_failed"; rc = 7; return finish(-1); }
        bool got = ls.waitFor([&] { for (auto& kv : ls.results) if (kv.second.method == "MESSAGE") return true; return false; }, o.timeoutSec);
        int code = 0;
        for (auto& kv : ls.results) if (kv.second.method == "MESSAGE") { code = kv.second.code; s.reason = kv.second.reason; }
        s.code = code;
        s.extra = ",\"msg_id\":\"" + sds.msgId + "\"";
        if (!got || code / 100 != 2) { s.outcome = got ? "sds_rejected" : "sds_timeout"; rc = 7; }
        return finish(-1);
    }

    if (o.cmd == "sds-recv") {
        ls.waitFor([&] { return false; }, o.durationSec);
        s.extra = ",\"sds_received\":" + std::to_string(ls.sds.size());
        if (ls.sds.empty()) { s.outcome = "no_sds"; rc = 7; }
        return finish(-1);
    }

    if (o.cmd == "dialog-watch" || o.cmd == "join") {
        r = eng.dialogWatch(acc, o.target, true);
        if (!r.ok) { s.outcome = "subscribe_failed"; s.reason = r.reason; rc = 8; return finish(-1); }
        bool anyNotify = ls.waitFor([&] { return !ls.dialogs.empty(); }, o.timeoutSec);
        // SUBSCRIBE 거절(403 등)은 onRequestResult 가 아니라 evsub 종료로 온다 — NOTIFY 부재로 판정
        if (o.cmd == "dialog-watch") {
            ls.waitFor([&] { return false; }, o.durationSec);
            s.extra = ",\"dialogs\":" + std::to_string(ls.dialogs.size());
            if (!anyNotify) { s.outcome = "no_dialog_notify"; rc = 8; }
            eng.dialogWatch(acc, o.target, false);
            return finish(-1);
        }
        // join: confirmed dialog 를 기다린다
        bool got = ls.waitFor([&] { for (auto& d : ls.dialogs) if (d.state == "confirmed" && !d.callId.empty()) return true; return false; }, o.timeoutSec);
        if (!got) { s.outcome = "no_confirmed_dialog"; rc = 8; eng.dialogWatch(acc, o.target, false); return finish(-1); }
        DialogInfo target;
        for (auto& d : ls.dialogs) if (d.state == "confirmed" && !d.callId.empty()) target = d;
        s.callId = eng.join(acc, o.target, target);
        if (s.callId < 0) { s.outcome = "join_failed"; rc = 8; eng.dialogWatch(acc, o.target, false); return finish(-1); }
        bool up = waitActive(ls, s.callId, o.timeoutSec);
        CallInfo ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) { s.outcome = up ? "join_rejected" : "join_timeout"; rc = 8; eng.dialogWatch(acc, o.target, false); return finish(s.callId); }
        ls.waitFor([&] { return disconnected(s.callId); }, o.durationSec);
        mediaCheck(s.callId);
        ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        std::string src;
        for (auto& m : ci.sources) src += std::string(src.empty() ? "" : ",") + "{\"ssrc\":" + std::to_string(m.ssrc) + ",\"label\":\"" + m.label + "\"}";
        s.extra = ",\"join_call_id\":\"" + target.callId + "\",\"sources\":[" + src + "]";
        eng.dialogWatch(acc, o.target, false);
        return finish(s.callId);
    }
    return finish(-1);
}
