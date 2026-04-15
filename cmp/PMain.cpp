

#include "PCmpServer.h"
#include "PLog.h"

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
    
    // Keep main thread alive
    while(true) {
        msleep(1000);
    }
    return 0;
}
