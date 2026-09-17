// cims-tester-worker — 계측기 워커 엔트리 (test_instrument.md §2·§8).
//   cims-tester-worker [config/cims-tester-worker.json] [--preflight] [--verbose]
// 설정 키(config_template.json 선언): Worker.Name · Server.Ip/Port · Sip.LocalIp/PortBase/Capture/DumpMax · Media.AudioFile/VideoFile/SampleDir/MaxRtpStreams
//   · Limits.EndpointsPerCore/SapsPerCore · Timers.RegisterIntervalMs/RegisterTimeoutS/InviteTimeoutMs/ByeTimeoutMs
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
        cfg.peerCertFile = c["Media"]["PeerCertFile"].asString("");
        cfg.sampleDir = c["Media"]["SampleDir"].asString("");
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
        cfg.maxEndpointsPerCore = (int)c["Limits"]["EndpointsPerCore"].asInt(cfg.maxEndpointsPerCore);
        cfg.maxSapsPerCore = c["Limits"]["SapsPerCore"].asDouble(cfg.maxSapsPerCore);
        cfg.registerIntervalMs = (int)c["Timers"]["RegisterIntervalMs"].asInt(cfg.registerIntervalMs);
        cfg.registerTimeoutS = (int)c["Timers"]["RegisterTimeoutS"].asInt(cfg.registerTimeoutS);
        cfg.inviteTimeoutMs = (int)c["Timers"]["InviteTimeoutMs"].asInt(cfg.inviteTimeoutMs);
        cfg.byeTimeoutMs = (int)c["Timers"]["ByeTimeoutMs"].asInt(cfg.byeTimeoutMs);
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
