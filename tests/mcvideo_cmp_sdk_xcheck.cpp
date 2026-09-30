// MCVideo 전송 제어 교차 스모크 — 실제 CMP(PMcvControl, TS 24.581 §6.3 — mcvideo.md §5.3.1) ↔ SDK 참여자(mcvideo/tc_participant, §6.2.4·§6.2.5).
//
// 시험용 CMP 를 임시 설정으로 빈 포트 창에 직접 띄우고, CSP 자리에서 UDP JSON(cmp_media_api.md §7.9)으로 ADD·JOIN 을 부른 뒤 SDK 참여자
// 둘이 CMP 와 실제 제어 채널(RTCP APP)로 주고받는다. 코덱 수준 대조(McvXCheck)·양쪽 단위시험(McvParticipant·cmp_mcvideo_control_test)이
// 각자 가짜 상대를 쓰는 것과 달리 두 상태 머신을 맞붙인다 — M2(서버 e2e)의 미디어 평면 절반을 CSP·DB 없이 본다.
//   ① chat: 합류 Idle · 송출 요청 Granted(송출 SSRC = offer a=ssrc) · 알림 · [받기]/[그만 보기]/다시 [받기] · 상한 거절 · End Notify 로 수신 끝 · 교대
//   ② 대기열(양쪽 mc_queueing): 점유 중 요청 → Queue Position → 앞 송출 끝 → Granted
//   ③ 암묵 요청(prearranged): 다른 참가자가 있으면 JOIN 에서 곧바로 허가 · 개시자 혼자면 첫 참가자가 붙을 때 Transmission Granted(늦은 허가)
// 빌드·실행: tests/mcvideo_cmp_sdk_xcheck.sh [build/bin/cmp]. **라이브 CMP 에 쓰지 않는다**(이벤트 회신처를 가로챈다).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <pjlib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "mcvideo/tc_participant.h"

using namespace cimsue;
using namespace cimsue::mcvideo;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c, ...) do { bool _ok = (c); std::printf("%s  %s", _ok ? "PASS" : "FAIL", #c); if (!_ok) { ++g_fail; std::printf("  -- "); std::printf(__VA_ARGS__); } std::printf("\n"); std::fflush(stdout); } while (0)

static int udpBind(int port = 0) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) { close(s); return -1; }
    return s;
}
static int portOf(int s) { sockaddr_in a{}; socklen_t l = sizeof a; getsockname(s, (sockaddr*)&a, &l); return ntohs(a.sin_port); }

// ── CMP 제어(UDP JSON) — 응답은 trans_id 로 짝짓고, 이벤트(TRANSMITTERS 등)는 받는 대로 ack ──
struct Ctl {
    int s = -1; sockaddr_in cmp{};
    std::thread th; std::atomic<bool> run{true};
    std::mutex m; std::condition_variable cv; std::map<int, std::string> resp; std::vector<std::string> events;
    int tid = 8810000;
    void start(int cmpPort) {
        s = udpBind(); cmp.sin_family = AF_INET; cmp.sin_addr.s_addr = htonl(INADDR_LOOPBACK); cmp.sin_port = htons(cmpPort);
        th = std::thread([this] {
            char buf[16384];
            while (run) {
                timeval tv{0, 100000}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                ssize_t n = recv(s, buf, sizeof buf - 1, 0);
                if (n <= 0) continue;
                std::string j(buf, (size_t)n);
                std::smatch mm; int t = 0;
                if (std::regex_search(j, mm, std::regex("\"trans_id\"\\s*:\\s*(\\d+)"))) t = std::stoi(mm[1]);
                if (j.find("\"type\":\"event\"") != std::string::npos || j.find("\"type\": \"event\"") != std::string::npos) {
                    std::smatch cm; std::string cmd;
                    if (std::regex_search(j, cm, std::regex("\"cmd\"\\s*:\\s*\"([A-Z_]+)\""))) cmd = cm[1];
                    std::string ack = "{\"hdr\":{\"ver\":2,\"trans_id\":" + std::to_string(t) + ",\"node\":\"cmpx\",\"cmd\":\"" + cmd +
                                      "\",\"type\":\"response\",\"status\":\"OK\"}}";
                    sendto(s, ack.data(), ack.size(), 0, (sockaddr*)&cmp, sizeof cmp);
                    std::lock_guard<std::mutex> lk(m); events.push_back(j); cv.notify_all();
                    continue;
                }
                std::lock_guard<std::mutex> lk(m); resp[t] = j; cv.notify_all();
            }
        });
    }
    std::string req(const std::string& cmd, const std::string& payload, const char* service = "mcvideo", const char* sesid = "mcvx::1",
                    int ms = 2000) {
        int t;
        { std::lock_guard<std::mutex> lk(m); t = ++tid; }
        std::string j = "{\"hdr\":{\"ver\":2,\"trans_id\":" + std::to_string(t) + ",\"node\":\"cmpx\",\"cmd\":\"" + cmd +
                        "\",\"type\":\"request\",\"sesid\":\"" + sesid + "\",\"service\":\"" + service + "\"},\"payload\":" + payload + "}";
        sendto(s, j.data(), j.size(), 0, (sockaddr*)&cmp, sizeof cmp);
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return resp.count(t) > 0; })) return "";
        return resp[t];
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (s >= 0) close(s); }
};
static long num(const std::string& j, const std::string& key) {
    std::smatch m;
    if (std::regex_search(j, m, std::regex("\"" + key + "\"\\s*:\\s*(\\d+)"))) return std::stol(m[1]);
    return -1;
}
static std::string memberBlock(const std::string& j, const std::string& user) {
    size_t p = j.find("\"" + user + "\"");
    if (p == std::string::npos) return "";
    size_t b = j.find('{', p), e = j.find('}', b);
    return j.substr(b, e - b + 1);
}

// ── SDK 참여자 쪽 — 이벤트 기록 ──
struct Side {
    std::string uri;
    std::mutex m; std::condition_variable cv;
    std::vector<TransmissionEvent> tx; std::vector<ReceptionEvent> rx; std::vector<std::pair<bool, uint32_t>> send; std::vector<bool> recvOn;
    std::unique_ptr<Participant> p;
    int audio = -1, video = -1;
    void make(uint32_t ssrc) {
        Participant::Callbacks cb;
        cb.onTransmission = [this](const TransmissionEvent& e) { std::lock_guard<std::mutex> lk(m); tx.push_back(e); cv.notify_all(); };
        cb.onReception = [this](const ReceptionEvent& e) { std::lock_guard<std::mutex> lk(m); rx.push_back(e); cv.notify_all(); };
        cb.onSend = [this](bool on, uint32_t a, uint32_t) { std::lock_guard<std::mutex> lk(m); send.push_back({on, a}); cv.notify_all(); };
        cb.onReceive = [this](const VideoTransmitter&, bool on) { std::lock_guard<std::mutex> lk(m); recvOn.push_back(on); cv.notify_all(); };
        cb.log = [this](int lv, const std::string& s) { if (std::getenv("MCV_XCHECK_LOG")) std::fprintf(stderr, "[%s %d] %s\n", uri.c_str(), lv, s.c_str()); };
        p.reset(new Participant(-1, ssrc, uri, cb));
        audio = udpBind(); video = udpBind();
    }
    bool waitTx(TransmissionEvent::Kind k, int ms = 3000, TransmissionEvent* out = nullptr) {
        std::unique_lock<std::mutex> lk(m);
        bool ok = cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { for (auto& e : tx) if (e.kind == k) return true; return false; });
        if (ok && out) for (auto& e : tx) if (e.kind == k) *out = e;
        return ok;
    }
    bool waitRx(ReceptionEvent::Kind k, int ms = 3000, ReceptionEvent* out = nullptr) {
        std::unique_lock<std::mutex> lk(m);
        bool ok = cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { for (auto& e : rx) if (e.kind == k) return true; return false; });
        if (ok && out) for (auto& e : rx) if (e.kind == k) *out = e;
        return ok;
    }
    void clear() { std::lock_guard<std::mutex> lk(m); tx.clear(); rx.clear(); send.clear(); recvOn.clear(); }
};

int main(int argc, char** argv) {
    char absBuf[4096];
    const std::string cmpBin = realpath(argc > 1 ? argv[1] : "build/bin/cmp", absBuf) ? absBuf : "";   // 자식이 chdir 한 뒤 exec 한다
    if (cmpBin.empty()) { std::fprintf(stderr, "cmp binary not found (make cmp)\n"); return 2; }
    pj_init();
    // 빈 포트 창(60)
    int base = 0;
    for (int b = 34000; b < 60000 && !base; b += 100) {
        std::vector<int> ss; bool ok = true;
        for (int p = b; p < b + 60; ++p) { int s = udpBind(p); if (s < 0) { ok = false; break; } ss.push_back(s); }
        for (int s : ss) close(s);
        if (ok) base = b;
    }
    char tmpl[] = "/tmp/mcv_cmp_sdk_xcheck_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    {
        std::ofstream f(dir + "/cmp.json");
        f << "{\"ServerIp\":\"127.0.0.1\",\"ServerPort\":" << base + 50 << ",\"RtpIp\":\"127.0.0.1\",\"RtpStartPort\":" << base
          << ",\"RtpPoolSize\":1,\"TranscodeSlots\":0,\"TapPoolSize\":0,\"AnnPlayers\":0,\"PttRtpStartPort\":" << base + 10
          << ",\"PttMemberPoolSize\":1,\"PttVideoStartPort\":" << base + 12 << ",\"PttFloorStartPort\":" << base + 14
          << ",\"PttRtpPoolSize\":1,\"PttMediaBufferMs\":0,\"McVideoStartPort\":" << base + 20 << ",\"McVideoMemberPoolSize\":4,"
          << "\"RtpWorkerCount\":1,\"LogDir\":\"" << dir << "\",\"SystemId\":\"cmpx\",\"ServiceLogging\":{\"Dir\":\"\"},\"Fm\":{\"Enable\":false}}";
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(dir.c_str()) != 0) _exit(127);
        freopen((dir + "/stdout.log").c_str(), "w", stdout); dup2(fileno(stdout), 2);
        execl(cmpBin.c_str(), cmpBin.c_str(), (dir + "/cmp.json").c_str(), (char*)nullptr);
        _exit(127);
    }
    Ctl ctl; ctl.start(base + 50);
    std::string hb;
    for (int i = 0; i < 50 && hb.empty(); ++i) hb = ctl.req("HEARTBEAT", "{}", "system", "mcvx::0", 200);
    CHECK(!hb.empty() && hb.find("mcvideo") != std::string::npos, "no HEARTBEAT");

    const std::string A = "+82510004001", B = "+82510004002";
    Side sa, sb; sa.uri = "sip:" + A + "@ptt.cims.example.kr"; sb.uri = "sip:" + B + "@ptt.cims.example.kr";
    sa.make(0x0D000001); sb.make(0x0D000002);
    CHECK(sa.p->open(0) && sb.p->open(0), "open");

    std::string add = ctl.req("PTT_GROUP_ADD", "{\"group_id\":\"gx1\",\"members\":\"" + A + ":5:participant," + B +
                               ":3:participant\",\"max_transmitters\":1,\"group_type\":\"chat\",\"reception_mode\":\"manual\"}");
    CHECK(add.find("\"status\":\"OK\"") != std::string::npos, "%s", add.c_str());
    std::string ma = memberBlock(add, A), mb = memberBlock(add, B);
    long ctlA = num(ma, "control_port"), ctlB = num(mb, "control_port");
    std::string j1a = ctl.req("PTT_JOIN", "{\"group_id\":\"gx1\",\"session_id\":\"" + A + "\"}");
    std::string j1b = ctl.req("PTT_JOIN", "{\"group_id\":\"gx1\",\"session_id\":\"" + B + "\"}");
    long tcA = num(j1a, "tc_ssrc"), tcB = num(j1b, "tc_ssrc");
    CHECK(ctlA > 0 && ctlB > 0 && tcA > 0 && tcB > 0, "ctl %ld %ld tc %ld %ld", ctlA, ctlB, tcA, tcB);
    auto join2 = [&](Side& s, const std::string& id, bool audioSsrc, const std::string& gid = "gx1", const std::string& extra = "") {
        std::string body = "{\"group_id\":\"" + gid + "\",\"session_id\":\"" + id + "\",\"user_ip\":\"127.0.0.1\",\"user_port\":" +
                           std::to_string(portOf(s.audio)) + ",\"user_video_port\":" + std::to_string(portOf(s.video)) +
                           ",\"user_control_port\":" + std::to_string(s.p->localPort()) + ",\"user_uri\":\"" + s.uri +
                           "\",\"user_tc_ssrc\":" + std::to_string(s.p->localSsrc()) + ",\"user_pt\":96,\"user_video_pt\":97" +
                           (audioSsrc ? ",\"user_audio_ssrc\":1510000001,\"user_video_ssrc\":1510000002" : "") + extra + "}";
        return ctl.req("PTT_JOIN", body);
    };
    // 호 성립 순서 = SDK 와 같다 — answer(=JOIN ②) 뒤 setRemote·onEstablished
    std::string j2a = join2(sa, A, true), j2b = join2(sb, B, false);
    CHECK(j2a.find("\"status\":\"OK\"") != std::string::npos && j2b.find("\"status\":\"OK\"") != std::string::npos, "%s | %s", j2a.c_str(), j2b.c_str());
    sa.p->setRemote("127.0.0.1", (int)ctlA, (uint32_t)tcA); sa.p->onEstablished();
    sb.p->setRemote("127.0.0.1", (int)ctlB, (uint32_t)tcB); sb.p->onEstablished();
    CHECK(sa.waitTx(TransmissionEvent::Kind::Idle) && sb.waitTx(TransmissionEvent::Kind::Idle), "join → Transmission Idle 이 SDK 에 도달");

    // ① A 송출 요청 → Granted(offer a=ssrc 쌍) · B 알림
    sa.clear(); sb.clear();
    CHECK(sa.p->requestTransmission().ok, "requestTransmission");
    TransmissionEvent g;
    CHECK(sa.waitTx(TransmissionEvent::Kind::Granted, 3000, &g), "A Granted");
    CHECK(g.audioSsrc == 1510000001u && g.videoSsrc == 1510000002u, "ssrc %u %u", g.audioSsrc, g.videoSsrc);
    CHECK(sa.p->info().state == TransmissionState::Permitted, "A state %d", (int)sa.p->info().state);
    ReceptionEvent nb;
    CHECK(sb.waitRx(ReceptionEvent::Kind::Notified, 3000, &nb) && nb.transmitter.userId == sa.uri, "B Notified %s", nb.transmitter.userId.c_str());
    // ② B [받기] → 허가 · [그만 보기] · 다시 [받기]
    CHECK(sb.p->acceptReception(sa.uri).ok, "acceptReception");
    CHECK(sb.waitRx(ReceptionEvent::Kind::Granted), "B reception Granted");
    sb.clear();
    CHECK(sb.p->endReception(sa.uri).ok, "endReception");
    CHECK(sb.waitRx(ReceptionEvent::Kind::Released) || sb.waitRx(ReceptionEvent::Kind::Ended, 500), "B reception released");
    sb.clear();
    CHECK(sb.p->acceptReception(sa.uri).ok && sb.waitRx(ReceptionEvent::Kind::Granted), "B 다시 받기 허가");
    // ③ 상한 1 — B 송출 요청은 거절(대기열 없음)
    sb.clear();
    CHECK(sb.p->requestTransmission().ok, "B request");
    TransmissionEvent rj;
    CHECK(sb.waitTx(TransmissionEvent::Kind::Rejected, 3000, &rj), "B Rejected (max_transmitters 1) cause=%d", rj.cause);
    // ④ A [보내기 끝] → A 끝 · B 수신 끝(End Notify) · 둘 다 Idle
    sa.clear(); sb.clear();
    CHECK(sa.p->releaseTransmission().ok, "releaseTransmission");
    CHECK(sa.waitTx(TransmissionEvent::Kind::Ended) || sa.waitTx(TransmissionEvent::Kind::Idle, 500), "A ended");
    CHECK(sb.waitRx(ReceptionEvent::Kind::Ended), "B reception ended by End Notify");
    CHECK(sa.p->info().state == TransmissionState::NoPermission, "A state %d", (int)sa.p->info().state);
    // ⑤ 이제 B 가 받는다
    sb.clear(); sa.clear();
    CHECK(sb.p->requestTransmission().ok && sb.waitTx(TransmissionEvent::Kind::Granted), "B Granted after A ended");
    CHECK(sa.waitRx(ReceptionEvent::Kind::Notified), "A Notified of B");
    CHECK(sb.p->releaseTransmission().ok && (sb.waitTx(TransmissionEvent::Kind::Ended) || sb.waitTx(TransmissionEvent::Kind::Idle, 500)), "B ended");
    // 제어 채널 유지 RR 이 CMP 에서 해석 오류를 내지 않는다(STATS)
    std::string stats = ctl.req("STATS", "{}", "system", "mcvx::0");
    std::smatch sm; std::string grp;
    if (std::regex_search(stats, sm, std::regex("\"mcvideo_groups\"\\s*:\\s*\\[(\\{[^\\]]*)\\]"))) grp = sm[1];
    std::printf("INFO  STATS mcvideo_groups: %s\n", grp.substr(0, 400).c_str());

    sa.p->close(); sb.p->close();
    ctl.req("PTT_GROUP_REMOVE", "{\"group_id\":\"gx1\"}");

    // ── 대기열(양쪽 mc_queueing 협상 — JOIN queueing 1) ──
    auto fresh = [&](Side& s, uint32_t ssrc) { if (s.p) s.p->close(); s.p.reset(); s.clear(); s.make(ssrc); s.p->open(0); };
    auto setup = [&](const std::string& gid, const char* type, const std::string& extraA, const std::string& extraB, bool bFirst,
                     std::string* outJ2a = nullptr) {
        std::string ad = ctl.req("PTT_GROUP_ADD", "{\"group_id\":\"" + gid + "\",\"members\":\"" + A + ":5:participant," + B +
                                  ":3:participant\",\"max_transmitters\":1,\"group_type\":\"" + type + "\",\"reception_mode\":\"automatic\"}");
        std::string a1 = ctl.req("PTT_JOIN", "{\"group_id\":\"" + gid + "\",\"session_id\":\"" + A + "\"}");
        std::string b1 = ctl.req("PTT_JOIN", "{\"group_id\":\"" + gid + "\",\"session_id\":\"" + B + "\"}");
        long cA = num(memberBlock(ad, A), "control_port"), cB = num(memberBlock(ad, B), "control_port");
        long tA = num(a1, "tc_ssrc"), tB = num(b1, "tc_ssrc");
        std::string ja, jb;
        if (bFirst) { jb = join2(sb, B, false, gid, extraB); ja = join2(sa, A, true, gid, extraA); }
        else { ja = join2(sa, A, true, gid, extraA); }
        if (outJ2a) *outJ2a = ja;
        sa.p->setRemote("127.0.0.1", (int)cA, (uint32_t)tA);
        sb.p->setRemote("127.0.0.1", (int)cB, (uint32_t)tB);
        return std::make_pair(ad.find("\"status\":\"OK\"") != std::string::npos, std::to_string(cB) + ":" + std::to_string(tB));
    };
    fresh(sa, 0x0D000011); fresh(sb, 0x0D000012);
    auto q = setup("gx2", "chat", ",\"queueing\":1", ",\"queueing\":1", true);
    CHECK(q.first, "ADD gx2");
    sa.p->onEstablished(); sb.p->onEstablished();
    CHECK(sa.p->requestTransmission().ok && sa.waitTx(TransmissionEvent::Kind::Granted), "Q: A Granted");
    CHECK(sb.p->requestTransmission().ok && sb.waitTx(TransmissionEvent::Kind::QueuePosition), "Q: B queued (Queue Position Info)");
    CHECK(sb.p->info().state == TransmissionState::Queued, "Q: B state %d", (int)sb.p->info().state);
    CHECK(sa.p->releaseTransmission().ok && sb.waitTx(TransmissionEvent::Kind::Granted), "Q: A 끝 → B 대기열에서 Granted");
    CHECK(sb.p->releaseTransmission().ok, "Q: B release");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ctl.req("PTT_GROUP_REMOVE", "{\"group_id\":\"gx2\"}");

    // ── 암묵 요청 — 다른 참가자가 이미 있으면 JOIN 때 곧바로 허가(answer mc_granted·SSRC 쌍) ──
    fresh(sa, 0x0D000021); fresh(sb, 0x0D000022);
    std::string ja;
    sb.p->onEstablished();
    sa.p->armImplicitRequest();
    auto im = setup("gx3", "prearranged", ",\"implicit_request\":1", "", true, &ja);
    long granted = num(ja, "granted"), as = num(ja, "audio_ssrc"), vs = num(ja, "video_ssrc");
    CHECK(im.first && granted == 1 && as > 0 && vs > 0, "I: JOIN granted=%ld ssrc %ld %ld (%s)", granted, as, vs, ja.c_str());
    sa.p->onEstablished(true, granted == 1, (uint32_t)as, (uint32_t)vs);
    CHECK(sa.p->info().state == TransmissionState::Permitted, "I: A Permitted at establishment (%d)", (int)sa.p->info().state);
    CHECK(sb.waitRx(ReceptionEvent::Kind::Notified), "I: B Notified");
    CHECK(sa.p->releaseTransmission().ok && (sa.waitTx(TransmissionEvent::Kind::Ended) || sa.waitTx(TransmissionEvent::Kind::Idle, 500)), "I: A ended");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ctl.req("PTT_GROUP_REMOVE", "{\"group_id\":\"gx3\"}");

    // ── 암묵 요청 — 개시자 혼자면 기다렸다가 첫 참가자가 붙을 때 Transmission Granted(늦은 허가, T100×C100 안) ──
    fresh(sa, 0x0D000031); fresh(sb, 0x0D000032);
    sa.p->armImplicitRequest();
    auto lt = setup("gx4", "prearranged", ",\"implicit_request\":1", "", false, &ja);
    granted = num(ja, "granted");
    CHECK(lt.first && granted == 0, "L: JOIN alone granted=%ld (%s)", granted, ja.c_str());
    sa.p->onEstablished(true, false, (uint32_t)std::max(0L, num(ja, "audio_ssrc")), (uint32_t)std::max(0L, num(ja, "video_ssrc")));
    CHECK(sa.p->info().state == TransmissionState::PendingRequest, "L: A pending (%d)", (int)sa.p->info().state);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));             // 초대 멤버가 0.8 s 뒤에 받는다
    std::string jb = join2(sb, B, false, "gx4");
    sb.p->onEstablished();
    CHECK(jb.find("\"status\":\"OK\"") != std::string::npos, "L: B join");
    CHECK(sa.waitTx(TransmissionEvent::Kind::Granted, 3000), "L: A Granted when first invitee joins");
    CHECK(sa.p->info().state == TransmissionState::Permitted, "L: A Permitted (%d)", (int)sa.p->info().state);
    CHECK(sa.p->releaseTransmission().ok, "L: A release");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sa.p->close(); sb.p->close();
    ctl.req("PTT_GROUP_REMOVE", "{\"group_id\":\"gx4\"}");
    ctl.stop();
    kill(pid, SIGTERM);
    int st = 0; waitpid(pid, &st, 0);
    if (!g_fail) { std::string rm = "rm -rf '" + dir + "'"; if (std::system(rm.c_str()) != 0) {} }   // 실패면 CMP 로그를 남긴다
    else std::printf("CMP log: %s/stdout.log\n", dir.c_str());
    std::printf(g_fail ? "FAIL %d\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
