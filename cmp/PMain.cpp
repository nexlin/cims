

#include "PCmpServer.h"
#include "PLog.h"

#include <csignal>

// SIGTERM = graceful stop — FM process_stopping 통지·녹취/로그 flush 후 종료.
// (종전: 핸들러 부재 → 즉사, stopServer 미실행)
static volatile sig_atomic_t g_stop = 0;
static void onTerm(int) { g_stop = 1; }

int main(int argc, char** argv) {
    std::string configFile = "../config/cmp.conf";
    if (argc > 1) {
        configFile = argv[1];
    }

    // 로그 레벨 설정 (기본: INFO, 환경변수로 DEBUG 가능)
    const char* logLevel = getenv("CMP_LOG_LEVEL");
    if (logLevel) {
        if (strcmp(logLevel, "DEBUG") == 0 || strcmp(logLevel, "debug") == 0)
            PLog::Instance().SetLevel(CMP_LOG_DEBUG);
        else if (strcmp(logLevel, "WARN") == 0 || strcmp(logLevel, "warn") == 0)
            PLog::Instance().SetLevel(CMP_LOG_WARN);
        else if (strcmp(logLevel, "ERROR") == 0 || strcmp(logLevel, "error") == 0)
            PLog::Instance().SetLevel(CMP_LOG_ERROR);
    }

    PCmpServer server("cmp", configFile);

    if (!server.startServer()) {
        LOG_ERROR("Main", "Failed to start CMP server");
        return 1;
    }

    LOG_INFO("Main", "cmp started. config=%s", configFile.c_str());

    signal(SIGTERM, onTerm);
    while (!g_stop) {
        msleep(1000);
    }
    LOG_INFO("Main", "SIGTERM — graceful stop");
    server.stopServer();
    return 0;
}
