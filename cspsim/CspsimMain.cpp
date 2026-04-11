#include <vector>
#include <string>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <atomic>
#include <algorithm>

#include "SimSession.h"
#include "Log.h"

// ─────────────────────────────────────────────
//  콘솔 로거
// ─────────────────────────────────────────────
class CConsoleLog : public ILogCallBack {
public:
    void Print(EnumLogLevel eLevel, const char* fmt, ...) {
        if (!(eLevel & (LOG_INFO | LOG_ERROR | LOG_SYSTEM))) return;
        va_list ap;
        char szBuf[LOG_MAX_SIZE];
        va_start(ap, fmt);
        vsnprintf(szBuf, sizeof(szBuf), fmt, ap);
        va_end(ap);
        printf("%s\n", szBuf);
    }
};
CConsoleLog gclsConsoleLog;

// ─────────────────────────────────────────────
//  유틸
// ─────────────────────────────────────────────
static std::string GetArg(int argc, char* argv[], const char* pszKey,
                           const char* pszDefault = "") {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], pszKey) == 0) return argv[i + 1];
    }
    return pszDefault;
}

static bool HasFlag(int argc, char* argv[], const char* pszKey) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], pszKey) == 0) return true;
    }
    return false;
}

// 순번 i 만큼 증가한 사용자 ID 생성
// E.164(+로 시작) 또는 순수 숫자 모두 지원
static std::string MakeUserId(const std::string& strBase, int iOffset) {
    if (!strBase.empty() && strBase[0] == '+') {
        long long llNum = atoll(strBase.c_str() + 1);
        llNum += iOffset;
        char buf[32];
        snprintf(buf, sizeof(buf), "+%lld", llNum);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)atoll(strBase.c_str()) + iOffset);
    return buf;
}

// PTT 모드 E.164 → Digest auth_id 자동 유도
// +82571900001 → 4503382571900001@{domain}
static std::string DerivePttAuthId(const std::string& strUser, const std::string& strDomain) {
    if (strUser.empty() || strUser[0] != '+') return "";
    return std::string("45033") + strUser.substr(1) + "@" + strDomain;
}

static std::string GetLocalIp() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1";
    struct sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    if (connect(sock, (const struct sockaddr*)&serv, sizeof(serv)) < 0) {
        close(sock); return "127.0.0.1";
    }
    struct sockaddr_in name{};
    socklen_t namelen = sizeof(name);
    getsockname(sock, (struct sockaddr*)&name, &namelen);
    char buf[INET_ADDRSTRLEN];
    const char* p = inet_ntop(AF_INET, &name.sin_addr, buf, sizeof(buf));
    close(sock);
    return p ? std::string(buf) : "127.0.0.1";
}

// ─────────────────────────────────────────────
//  집계 통계 출력
// ─────────────────────────────────────────────
static void PrintStats(const std::vector<SimSession*>& sessions) {
    int totalReg = 0, failReg = 0, gmsOk = 0, cmsOk = 0;
    int notifyRecv = 0, confNotify = 0, callOk = 0, callFail = 0, callEnd = 0;
    long long totalRegMs = 0, totalCallMs = 0;
    int registered = 0, inCall = 0;

    for (auto* s : sessions) {
        totalReg   += s->m_stats.iRegOk.load();
        failReg    += s->m_stats.iRegFail.load();
        gmsOk      += s->m_stats.iGmsOk.load();
        cmsOk      += s->m_stats.iCmsOk.load();
        notifyRecv += s->m_stats.iNotifyRecv.load();
        confNotify += s->m_stats.iConfNotify.load();
        callOk     += s->m_stats.iCallOk.load();
        callFail   += s->m_stats.iCallFail.load();
        callEnd    += s->m_stats.iCallEnd.load();
        totalRegMs += s->m_stats.llTotalRegMs.load();
        totalCallMs+= s->m_stats.llTotalCallMs.load();
        if (s->m_bRegistered) registered++;
        if (s->m_bInCall)     inCall++;
    }

    int n = (int)sessions.size();
    printf("\n===== STATISTICS (%d sessions) =====\n", n);
    printf("  Registered   : %d / %d  (fail=%d)\n", registered, n, failReg);
    printf("  Avg Reg Time : %lldms\n", totalReg ? totalRegMs / totalReg : 0LL);
    printf("  GMS Subscribed: %d\n", gmsOk);
    printf("  CMS Subscribed: %d\n", cmsOk);
    printf("  NOTIFY Recv   : %d\n", notifyRecv);
    printf("  Conf NOTIFY   : %d\n", confNotify);
    printf("  Active Calls  : %d\n", inCall);
    printf("  Call OK/End   : %d / %d  (fail=%d)\n", callOk, callEnd, callFail);
    printf("  Avg Call Setup: %lldms\n", callOk ? totalCallMs / callOk : 0LL);
    printf("=====================================\n\n");
}

// ─────────────────────────────────────────────
//  사용법 출력
// ─────────────────────────────────────────────
static void PrintUsage(const char* pszBin) {
    printf("Usage: %s [options]\n\n", pszBin);
    printf("Options:\n");
    printf("  -server_ip   <ip>        CSP 서버 IP (default: 127.0.0.1)\n");
    printf("  -server_port <port>      CSP 서버 SIP 포트 (default: 5060)\n");
    printf("  -local_ip    <ip>        로컬 IP (default: auto-detect)\n");
    printf("  -local_port  <port>      로컬 SIP 시작 포트 (default: 6000)\n");
    printf("  -count       <N>         단말 수 (default: 1)\n");
    printf("  -user        <start_id>  시작 사용자 ID (예: 1000 또는 +82571900001)\n");
    printf("  -auth_id     <auth_id>   Digest 인증 ID (PTT E.164는 자동 유도)\n");
    printf("  -domain      <domain>    SIP 도메인 (default: csp)\n");
    printf("  -password    <pwd>       패스워드 (default: 1234)\n");
    printf("  -mode        <voip|ptt>  단말 유형 (default: voip)\n");
    printf("  -group       <group_id>  PTT 그룹 ID (default: 1000)\n");
    printf("  -scenario    <name>      자동 시나리오:\n");
    printf("                             register     - 등록만\n");
    printf("                             subscribe    - 등록 + GMS/CMS 구독 (PTT)\n");
    printf("                             call         - 등록 + 짝끼리 통화\n");
    printf("                             group-call   - 등록 + 구독 + 그룹통화 (PTT)\n");
    printf("                             full         - 전체 반복\n");
    printf("  -call_duration <secs>    통화 유지 시간 (default: 10)\n");
    printf("  -media_file  <path>      AMR-WB 미디어 파일 (PT=99 전송, 생략 시 합성 RTP)\n");
    printf("  -video_file  <path>      H.264 Annex B 비디오 파일 (PT=96 전송)\n");
    printf("  -interval    <ms>        단말 기동 간격 ms (default: 100)\n");
    printf("  -verbose                 SIP 메시지 상세 로그\n\n");
    printf("Commands (실행 중):\n");
    printf("  s           - 통계 출력\n");
    printf("  c [N] [dst] - 통화 시작 (N번 세션 → dst, 생략시 전체)\n");
    printf("  e           - 통화 종료\n");
    printf("  g [group]   - PTT 그룹통화 시작\n");
    printf("  t           - PTT 발언권 요청\n");
    printf("  r           - PTT 발언권 해제\n");
    printf("  sub         - GMS/CMS SUBSCRIBE 전송\n");
    printf("  q           - 종료\n\n");
}

// ─────────────────────────────────────────────
//  자동 시나리오 실행 스레드
// ─────────────────────────────────────────────
static std::atomic<bool> g_bScenarioDone(false);
static std::atomic<bool> g_bQuit(false);

static void RunScenario(std::vector<SimSession*>& sessions,
                        ESimScenario eScenario,
                        int iCallDuration,
                        const std::string& strGroupId)
{
    // 1. 모든 단말이 등록될 때까지 대기 (최대 30초)
    printf("[Scenario] Waiting for registration...\n");
    for (int retry = 0; retry < 300; ++retry) {
        int regCount = 0;
        for (auto* s : sessions) if (s->m_bRegistered) regCount++;
        if (regCount == (int)sessions.size()) break;
        usleep(100000);
    }
    {
        int regCount = 0;
        for (auto* s : sessions) if (s->m_bRegistered) regCount++;
        printf("[Scenario] %d/%d registered\n", regCount, (int)sessions.size());
    }

    if (eScenario == E_SCENARIO_REGISTER) return;

    // 2. PTT: GMS/CMS SUBSCRIBE
    if (eScenario == E_SCENARIO_SUBSCRIBE ||
        eScenario == E_SCENARIO_GROUP_CALL ||
        eScenario == E_SCENARIO_FULL) {
        printf("[Scenario] Sending GMS/CMS SUBSCRIBE...\n");
        for (auto* s : sessions) {
            if (!s->m_bPttMode) continue;
            s->SubscribeGms();
            usleep(50000);
            s->SubscribeCms();
            usleep(50000);
        }
        // 구독 완료 대기 (최대 10초)
        for (int retry = 0; retry < 100; ++retry) {
            int subCount = 0;
            for (auto* s : sessions) if (s->m_bGmsSubscribed) subCount++;
            if (subCount == (int)sessions.size()) break;
            usleep(100000);
        }
        printf("[Scenario] Subscriptions complete\n");
    }

    if (eScenario == E_SCENARIO_SUBSCRIBE) return;

    // 3. 통화 시나리오
    if (eScenario == E_SCENARIO_CALL) {
        printf("[Scenario] Starting paired calls...\n");
        for (int i = 0; i + 1 < (int)sessions.size(); i += 2) {
            std::string strTarget = sessions[i + 1]->m_strUser;
            sessions[i]->StartCall(strTarget);
            usleep(200000);
        }
        for (int i = 0; i < iCallDuration * 10; i++) usleep(100000);
        printf("[Scenario] Ending calls...\n");
        for (auto* s : sessions) s->StopCall();
        return;
    }

    // 4. PTT 그룹통화
    // PTT 단말은 발신하지 않음 — CSP가 CheckGroupIntegrity 로 자동 초대
    // 시뮬레이터는 등록/구독 후 CSP의 INVITE 를 기다려 응답만 함
    if (eScenario == E_SCENARIO_GROUP_CALL || eScenario == E_SCENARIO_FULL) {
        if (!sessions.empty() && sessions[0]->m_bPttMode) {
            printf("[Scenario] PTT mode: waiting for CSP to invite (call_duration=%ds)...\n", iCallDuration);
            for (int i = 0; i < iCallDuration * 10; i++) usleep(100000);
            printf("[Scenario] PTT wait done, stopping\n");
            for (auto* s : sessions) s->StopCall();
        } else {
            // VoIP group call (legacy)
            printf("[Scenario] Starting group call → %s\n", strGroupId.c_str());
            if (!sessions.empty()) sessions[0]->StartGroupCall(strGroupId);
            for (int i = 0; i < iCallDuration * 10; i++) usleep(100000);
            printf("[Scenario] Ending group call\n");
            for (auto* s : sessions) s->StopCall();
        }
    }

    g_bScenarioDone = true;
}

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // 파이프 출력 시 버퍼링 비활성화 (자동화 테스트 호환)
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (HasFlag(argc, argv, "-help") || HasFlag(argc, argv, "--help") || argc < 2) {
        PrintUsage(argv[0]);
        return 0;
    }

    // 인자 파싱
    std::string strServerIp   = GetArg(argc, argv, "-server_ip",   "127.0.0.1");
    int iServerPort            = atoi(GetArg(argc, argv, "-server_port", "5060").c_str());
    std::string strLocalIp    = GetArg(argc, argv, "-local_ip",    "");
    int iLocalBasePort         = atoi(GetArg(argc, argv, "-local_port",  "6000").c_str());
    int iCount                 = atoi(GetArg(argc, argv, "-count",       "1").c_str());
    std::string strStartUser  = GetArg(argc, argv, "-user",        "1000");
    std::string strExplicitAuthId = GetArg(argc, argv, "-auth_id", "");
    std::string strDomain     = GetArg(argc, argv, "-domain",     "csp");
    std::string strPassword   = GetArg(argc, argv, "-password",   "1234");
    std::string strMode       = GetArg(argc, argv, "-mode",       "voip");
    std::string strGroupId    = GetArg(argc, argv, "-group",      "1000");
    std::string strScenario   = GetArg(argc, argv, "-scenario",   "");
    int iCallDuration          = atoi(GetArg(argc, argv, "-call_duration", "10").c_str());
    std::string strMediaFile  = GetArg(argc, argv, "-media_file",  "");
    std::string strVideoFile  = GetArg(argc, argv, "-video_file",  "");
    int iIntervalMs            = atoi(GetArg(argc, argv, "-interval",    "100").c_str());
    bool bVerbose              = HasFlag(argc, argv, "-verbose");
    bool bPttMode              = (strMode == "ptt");

    if (strLocalIp.empty()) strLocalIp = GetLocalIp();

    // 시나리오 선택
    ESimScenario eScenario = E_SCENARIO_NONE;
    if      (strScenario == "register")   eScenario = E_SCENARIO_REGISTER;
    else if (strScenario == "subscribe")  eScenario = E_SCENARIO_SUBSCRIBE;
    else if (strScenario == "call")       eScenario = E_SCENARIO_CALL;
    else if (strScenario == "group-call") eScenario = E_SCENARIO_GROUP_CALL;
    else if (strScenario == "full")       eScenario = E_SCENARIO_FULL;

    // 로깅 설정
    CLog::SetPrefix("cspsim");
    CLog::SetDirectory("log");
    EnumLogLevel eLevel = (EnumLogLevel)(LOG_INFO | LOG_ERROR | LOG_SYSTEM);
    if (bVerbose) eLevel = (EnumLogLevel)(eLevel | LOG_NETWORK | LOG_DEBUG);
    CLog::SetLevel(eLevel);
    CLog::SetCallBack(&gclsConsoleLog);

    printf("╔══════════════════════════════════════════╗\n");
    printf("║           CSP SIM - 단말 시뮬레이터       ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("  서버   : %s:%d\n", strServerIp.c_str(), iServerPort);
    printf("  로컬   : %s (시작포트 %d)\n", strLocalIp.c_str(), iLocalBasePort);
    printf("  단말수 : %d개  (시작ID: %s)\n", iCount, strStartUser.c_str());
    printf("  모드   : %s\n", bPttMode ? "PTT" : "VoIP");
    if (bPttMode) printf("  그룹ID : %s\n", strGroupId.c_str());
    if (!strScenario.empty()) printf("  시나리오: %s\n", strScenario.c_str());
    printf("\n");

    // 세션 생성 및 시작
    std::vector<SimSession*> sessions;
    sessions.reserve(iCount);

    for (int i = 0; i < iCount; i++) {
        int iLocalPort = iLocalBasePort + (i * 2); // SIP + 여유

        std::string strUser   = MakeUserId(strStartUser, i);
        std::string strAuthId;
        if (!strExplicitAuthId.empty() && i > 0) {
            // 명시적 auth_id의 숫자 부분을 offset만큼 증가
            // 예: 450033100000002@domain → 450033100000003@domain (i=1)
            std::string base = strExplicitAuthId;
            size_t atPos = base.find('@');
            std::string suffix = (atPos != std::string::npos) ? base.substr(atPos) : "";
            std::string prefix = (atPos != std::string::npos) ? base.substr(0, atPos) : base;
            // 끝에서 연속 숫자 찾기
            int numStart = (int)prefix.size();
            while (numStart > 0 && isdigit(prefix[numStart - 1])) numStart--;
            if (numStart < (int)prefix.size()) {
                long long num = atoll(prefix.c_str() + numStart);
                num += i;
                char buf[32];
                // 원본 자릿수 유지
                int digits = (int)prefix.size() - numStart;
                snprintf(buf, sizeof(buf), "%0*lld", digits, num);
                strAuthId = prefix.substr(0, numStart) + buf + suffix;
            } else {
                strAuthId = strExplicitAuthId; // 숫자 없으면 그대로
            }
        } else if (!strExplicitAuthId.empty()) {
            strAuthId = strExplicitAuthId;
        } else {
            if (bPttMode && !strUser.empty() && strUser[0] == '+') {
                // PTT + E.164: 자동 유도 (45033 + MSISDN숫자 + @domain)
                strAuthId = DerivePttAuthId(strUser, strDomain);
            } else {
                strAuthId = strUser;
            }
        }

        SimSession* s = new SimSession(
            i,
            strUser,
            strAuthId,
            strDomain,
            strPassword,
            strServerIp, iServerPort,
            strLocalIp,  iLocalPort,
            bPttMode,
            strGroupId
        );

        if (!strMediaFile.empty()) {
            s->m_clsRtpThread.SetMediaFile(strMediaFile);
        }
        if (!strVideoFile.empty()) {
            s->m_clsRtpThread.SetVideoFile(strVideoFile);
        }
        if (s->Start()) {
            sessions.push_back(s);
        } else {
            printf("[%d] Failed to start session (User %s)\n", i, strUser.c_str());
            delete s;
        }

        if (iIntervalMs > 0) usleep(iIntervalMs * 1000);
    }

    printf("\n%d개 세션 시작 완료. 명령어는 -help 참조\n\n", (int)sessions.size());

    // 자동 시나리오 실행 (별도 스레드)
    std::thread scenarioThread;
    if (eScenario != E_SCENARIO_NONE && !sessions.empty()) {
        scenarioThread = std::thread(RunScenario,
                                     std::ref(sessions),
                                     eScenario,
                                     iCallDuration,
                                     strGroupId);
    }

    // 시나리오 모드: 완료 대기 → 세션 정리 → 종료 (stdin 루프 진입 안함)
    if (eScenario != E_SCENARIO_NONE) {
        if (scenarioThread.joinable()) scenarioThread.join();
        printf("\n최종 통계:\n");
        PrintStats(sessions);

        // 세션 정리: BYE + 등록 해제 전송
        printf("세션 종료 중...\n");
        // 먼저 모든 활성 통화 종료 (BYE 전송)
        for (auto* s : sessions) s->StopCall();
        // BYE 처리 대기 (서버측 OnCallTerminated + DB 갱신 시간 확보)
        usleep(1500000 + iCount * 300000);

        // SIP 스택 종료 (REGISTER Expires=0 전송 → 등록 해제)
        for (auto* s : sessions) {
            s->m_clsRtpThread.Stop();
            s->m_clsUserAgent.Stop();
        }
        // 등록 해제 처리 대기 (401 challenge + re-REGISTER)
        usleep(1000000 + iCount * 200000);

        // 타임아웃 보호: 네트워크 지연으로 정리가 오래 걸리면 강제 종료
        _exit(0);
    }

    // 대화형 명령 루프 (시나리오 없는 대화형 모드)
    char szCommand[256];
    while (true) {
        if (!fgets(szCommand, sizeof(szCommand), stdin)) break;
        // 개행 제거
        char* nl = strchr(szCommand, '\n');
        if (nl) *nl = '\0';

        if (szCommand[0] == 'q' || szCommand[0] == 'Q') {
            break;
        } else if (szCommand[0] == 's') {
            PrintStats(sessions);
        } else if (szCommand[0] == 't') {
            for (auto* s : sessions) s->SendPttRequest();
            printf("PTT 발언권 요청 전송\n");
        } else if (szCommand[0] == 'r') {
            for (auto* s : sessions) s->SendPttRelease();
            printf("PTT 발언권 해제 전송\n");
        } else if (szCommand[0] == 'e') {
            for (auto* s : sessions) s->StopCall();
            printf("통화 종료\n");
        } else if (strncmp(szCommand, "sub", 3) == 0) {
            for (auto* s : sessions) {
                if (!s->m_bPttMode) continue;
                s->SubscribeGms();
                usleep(30000);
                s->SubscribeCms();
            }
            printf("SUBSCRIBE 전송\n");
        } else if (szCommand[0] == 'g') {
            // g [group_id]
            char szGroup[64] = "";
            sscanf(szCommand + 1, " %63s", szGroup);
            std::string grp = strlen(szGroup) > 0 ? szGroup : strGroupId;
            for (auto* s : sessions) {
                if (s->m_bPttMode) s->StartGroupCall(grp);
            }
            printf("그룹통화 시작 → %s\n", grp.c_str());
        } else if (szCommand[0] == 'c') {
            // c [session_idx] [target]
            int idx = -1;
            char szTarget[64] = "";
            sscanf(szCommand + 1, " %d %63s", &idx, szTarget);
            if (idx >= 0 && idx < (int)sessions.size()) {
                sessions[idx]->StartCall(strlen(szTarget) > 0 ? szTarget : "");
            } else {
                // 짝끼리 통화
                for (int i = 0; i + 1 < (int)sessions.size(); i += 2) {
                    sessions[i]->StartCall(sessions[i + 1]->m_strUser);
                    usleep(50000);
                }
                printf("페어 통화 시작\n");
            }
        } else if (szCommand[0] != '\0') {
            printf("? 알 수 없는 명령 (h: 도움말)\n");
        }
    }

    // 대화형 모드 종료 후 시나리오 스레드 정리 (시나리오 모드는 위에서 이미 종료)
    if (scenarioThread.joinable()) scenarioThread.join();
    printf("\n최종 통계:\n");
    PrintStats(sessions);
    fflush(stdout);

    printf("세션 종료 중...\n");
    for (auto* s : sessions) delete s;
    sessions.clear();

    return 0;
}
