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
//   (계정 옵션 대신 --from-profile volte|ptt 로 프로비저닝 프로파일에서 계정을 채울 수 있다)
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
#include <map>
#include <mutex>
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
    if (o.cmd == "transfer" && o.transferTo.empty()) return false;
    static const char* known[] = {"register", "call", "answer", "group-call", "sds", "sds-recv", "login", "dialog-watch", "join", "pickup", "transfer",
                                  "groups", "group-get", "group-put", "group-delete"};
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
               jsonEsc(m.volteAor) + "\",\"ptt_id\":\"" + jsonEsc(m.pttId) + "\",\"extension\":\"" + jsonEsc(m.extension) + "\"}";
    for (auto& t : d.pttTargets)
        tgt += std::string(tgt.empty() ? "" : ",") + "{\"id\":\"" + jsonEsc(t.id) + "\",\"uri\":\"" + jsonEsc(t.uri) + "\",\"name\":\"" + jsonEsc(t.name) + "\"}";
    return "{\"group_id\":\"" + jsonEsc(d.groupId) + "\",\"group_name\":\"" + jsonEsc(d.groupName) + "\",\"pilot_id\":\"" + jsonEsc(d.pilotId) +
           "\",\"monitor_scope\":\"" + d.monitorScope + "\",\"ptt_listen\":\"" + d.pttListen + "\",\"listen_visibility\":\"" + d.listenVisibility +
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
        std::string msgId = eng.sendGroupSds(acc, o.target, o.text);
        if (msgId.empty()) { s.outcome = "sds_send_failed"; rc = 7; return finish(-1); }
        bool got = ls.waitFor([&] { for (auto& kv : ls.results) if (kv.second.method == "MESSAGE") return true; return false; }, o.timeoutSec);
        int code = 0;
        for (auto& kv : ls.results) if (kv.second.method == "MESSAGE") { code = kv.second.code; s.reason = kv.second.reason; }
        s.code = code;
        s.extra = ",\"msg_id\":\"" + msgId + "\"";
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
