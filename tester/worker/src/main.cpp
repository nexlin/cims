// cims-tester-worker — 계측기 워커 엔트리 (test_instrument.md §2·§8).
//   cims-tester-worker [config/cims-tester-worker.json] [--preflight] [--verbose]
// 설정 키(config_template.json 선언): Worker.Name · Server.Ip/Port · Sip.LocalIp/PortBase/Capture/DumpMax · Media.AudioFile/VideoFile/SampleDir/MaxRtpStreams
//   · Tls.CaFile/ClientCertFile/ClientKeyFile/PeerCertFile/PeerKeyFile(구 Media.PeerCertFile 승계) · Nat.NetnsDir(NAT 풀 netns 디렉터리)
//   · RealUe.CliPath/MaxProcesses/LogLevel/StartTimeoutS/TlsCaFile · Limits.EndpointsPerCore/SapsPerCore · Timers.RegisterIntervalMs/RegisterTimeoutS/InviteTimeoutMs/ByeTimeoutMs
// libcsim(SimSession) 의 printf 진단은 부하 중 초당 수천 줄이라 stdout 을 /dev/null 로 돌린다(--verbose 면 유지).
// 워커 자기 로그는 stderr — agent lifecycle 이 로그 파일로 모은다.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "Json.h"
#include "Worker.h"

static volatile sig_atomic_t g_stop = 0;
static void onSig(int) { g_stop = 1; }

static Json loadJson(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "open failed: " + path; return Json(); }
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    if (!Json::parse(ss.str(), j, err)) return Json();
    return j;
}

int main(int argc, char** argv) {
    std::string cfgPath = "config/cims-tester-worker.json";
    bool preflight = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--preflight")) preflight = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [config.json] [--preflight] [--verbose]\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        } else cfgPath = argv[i];
    }
    WorkerConfig cfg;
    std::string err;
    Json c = loadJson(cfgPath, err);
    if (!err.empty()) {
        fprintf(stderr, "config: %s — 기본값으로 기동\n", err.c_str());
    } else {
        // 배포 overlay(config.json, 평면 키) 가 있으면 덮는다 — 다른 모듈과 같은 계층(02_deployment 설정 계층)
        Json ov;
        std::string e2;
        for (const char* p : { "config.json", "../config.json" }) {
            std::ifstream t(p);
            if (!t) continue;
            std::stringstream ss; ss << t.rdbuf();
            if (Json::parse(ss.str(), ov, e2) && ov.isObject()) {
                for (auto& kv : ov.items()) {
                    size_t dot = kv.first.find('.');
                    if (dot == std::string::npos) c[kv.first] = kv.second;
                    else c[kv.first.substr(0, dot)][kv.first.substr(dot + 1)] = kv.second;
                }
                break;
            }
        }
        cfg.name = c["Worker"]["Name"].asString(cfg.name);
        cfg.bindIp = c["Server"]["Ip"].asString(cfg.bindIp);
        cfg.port = (int)c["Server"]["Port"].asInt(cfg.port);
        cfg.localIp = c["Sip"]["LocalIp"].asString("");
        cfg.sipPortBase = (int)c["Sip"]["PortBase"].asInt(0);
        cfg.sipCapture = c["Sip"]["Capture"].asString(cfg.sipCapture);
        cfg.sipDumpMax = (int)c["Sip"]["DumpMax"].asInt(cfg.sipDumpMax);
        cfg.mediaFile = c["Media"]["AudioFile"].asString("");
        cfg.videoFile = c["Media"]["VideoFile"].asString("");
        cfg.peerCertFile = c["Tls"]["PeerCertFile"].asString(c["Media"]["PeerCertFile"].asString(""));
        cfg.peerKeyFile = c["Tls"]["PeerKeyFile"].asString("");
        cfg.tlsCaFile = c["Tls"]["CaFile"].asString("");
        cfg.tlsClientCertFile = c["Tls"]["ClientCertFile"].asString("");
        cfg.tlsClientKeyFile = c["Tls"]["ClientKeyFile"].asString("");
        cfg.sampleDir = c["Media"]["SampleDir"].asString("");
        cfg.natNetnsDir = c["Nat"]["NetnsDir"].asString(cfg.natNetnsDir);
        cfg.maxRtpStreams = (int)c["Media"]["MaxRtpStreams"].asInt(0);
        // 상대 경로 SampleDir — 모듈 디렉터리(<모듈>/bin/cims-tester-worker 의 위) 기준, 거기 없으면 작업 디렉터리 기준
        if (!cfg.sampleDir.empty() && cfg.sampleDir[0] != '/') {
            char exe[4096] = { 0 };
            ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (n > 0) {
                std::string dir(exe, (size_t)n);
                dir = dir.substr(0, dir.rfind('/'));
                dir = dir.substr(0, dir.rfind('/')) + "/" + cfg.sampleDir;
                if (access(dir.c_str(), R_OK | X_OK) == 0) cfg.sampleDir = dir;
            }
        }
        // real-ue 풀(§3.3) — cimsue-cli 경로(상대 = 모듈 디렉터리 기준, 그다음 작업 디렉터리)·프로세스 상한·로그
        cfg.realUeCli = c["RealUe"]["CliPath"].asString("bin/cimsue-cli");
        cfg.realUeMax = (int)c["RealUe"]["MaxProcesses"].asInt(cfg.realUeMax);
        cfg.realUeLogLevel = (int)c["RealUe"]["LogLevel"].asInt(cfg.realUeLogLevel);
        cfg.realUeStartTimeoutS = (int)c["RealUe"]["StartTimeoutS"].asInt(cfg.realUeStartTimeoutS);
        cfg.realUeTlsCaFile = c["RealUe"]["TlsCaFile"].asString("");
        {
            char exe[4096] = { 0 };
            ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            std::string moduleDir;
            if (n > 0) { moduleDir = std::string(exe, (size_t)n); moduleDir = moduleDir.substr(0, moduleDir.rfind('/')); moduleDir = moduleDir.substr(0, moduleDir.rfind('/')); }
            if (!cfg.realUeCli.empty() && cfg.realUeCli[0] != '/') {
                std::string cand = moduleDir + "/" + cfg.realUeCli;
                if (!moduleDir.empty() && access(cand.c_str(), X_OK) == 0) cfg.realUeCli = cand;
                else if (access(cfg.realUeCli.c_str(), X_OK) != 0 && !moduleDir.empty()) cfg.realUeCli = cand;   // 없어도 모듈 경로로 — 오류 메시지가 그 경로를 가리킨다
            }
            // TLS 파일 상대 경로 — 모듈 디렉터리 기준(있을 때만 바꾼다 — 없으면 오류 메시지가 원래 값을 가리킨다)
            for (std::string* p : { &cfg.peerCertFile, &cfg.peerKeyFile, &cfg.tlsCaFile, &cfg.tlsClientCertFile, &cfg.tlsClientKeyFile }) {
                if (p->empty() || (*p)[0] == '/' || moduleDir.empty()) continue;
                std::string cand = moduleDir + "/" + *p;
                if (access(cand.c_str(), R_OK) == 0) *p = cand;
            }
            if (cfg.realUeTlsCaFile.empty()) cfg.realUeTlsCaFile = cfg.tlsCaFile;   // 실단말 앵커 기본 = 같은 CA
            cfg.realUeLogDir = c["RealUe"]["LogDir"].asString("");
            if (cfg.realUeLogDir.empty()) cfg.realUeLogDir = (moduleDir.empty() ? std::string("log") : moduleDir + "/log") + "/real-ue";
            else if (cfg.realUeLogDir[0] != '/' && !moduleDir.empty()) cfg.realUeLogDir = moduleDir + "/" + cfg.realUeLogDir;
        }
        cfg.maxEndpointsPerCore = (int)c["Limits"]["EndpointsPerCore"].asInt(cfg.maxEndpointsPerCore);
        cfg.maxSapsPerCore = c["Limits"]["SapsPerCore"].asDouble(cfg.maxSapsPerCore);
        cfg.registerIntervalMs = (int)c["Timers"]["RegisterIntervalMs"].asInt(cfg.registerIntervalMs);
        cfg.registerTimeoutS = (int)c["Timers"]["RegisterTimeoutS"].asInt(cfg.registerTimeoutS);
        cfg.inviteTimeoutMs = (int)c["Timers"]["InviteTimeoutMs"].asInt(cfg.inviteTimeoutMs);
        cfg.byeTimeoutMs = (int)c["Timers"]["ByeTimeoutMs"].asInt(cfg.byeTimeoutMs);
        cfg.floorTimeoutMs = (int)c["Timers"]["FloorTimeoutMs"].asInt(cfg.floorTimeoutMs);
        cfg.groupReuseGapMs = (int)c["Timers"]["GroupReuseGapMs"].asInt(cfg.groupReuseGapMs);
    }
    if (cfg.name.empty() || cfg.name == "worker") {
        char host[128] = { 0 };
        if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) cfg.name = host;
    }
    // pkg.json 의 version — health 에 싣는다
    {
        std::string e3;
        Json pk = loadJson("pkg.json", e3);
        if (e3.empty()) cfg.version = pk["version"].asString(cfg.version);
        else { pk = loadJson("../pkg.json", e3); if (e3.empty()) cfg.version = pk["version"].asString(cfg.version); }
    }
    if (preflight) {
        printf("WORKER_PREFLIGHT_OK name=%s port=%d local_ip=%s\n", cfg.name.c_str(), cfg.port, cfg.localIp.c_str());
        return 0;
    }
    if (!verbose) { if (!freopen("/dev/null", "w", stdout)) {} }
    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);
    signal(SIGPIPE, SIG_IGN);

    Worker w(cfg);
    if (!w.start(err)) {
        fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fprintf(stderr, "stopping\n");
    w.stop();
    return 0;
}
