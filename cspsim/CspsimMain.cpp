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
#include <dirent.h>
#include <fstream>
#include <mariadb/mysql.h>

#include "SimSession.h"
#include "SimpleJson.h"
#include "Log.h"

// ─────────────────────────────────────────────
//  DB 가입자 정보
// ─────────────────────────────────────────────
struct DbSubscriber {
    std::string id;           // MSISDN (e.g. +821357007001)
    std::string authId;       // Digest auth_id
    std::string password;
    std::string serviceType;  // "volte" or "ptt"
};

static bool LoadSubscribersFromDb(const std::string& strCspJson,
                                   const std::string& strFilterMode,
                                   const std::string& strGroupId,
                                   std::vector<DbSubscriber>& vecOut,
                                   std::string& strRealm)
{
    // csp.json 파일 읽기
    std::ifstream ifs(strCspJson);
    if (!ifs.is_open()) {
        printf("[DB] csp.json 파일 열기 실패: %s\n", strCspJson.c_str());
        return false;
    }
    std::string strJson((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(strJson);
    SimpleJson::JsonNode setup = root.Get("Setup");

    // v3 (2026-04-22): Setup.Realm/AuthRealm 제거됨. domain 은 CLI -domain 인자 우선.
    //   여기서 strRealm 은 호출자 참조용으로 빈 값 두거나 Sip.LocalIp 로 fallback.
    (void)setup;

    // DB 접속 정보
    SimpleJson::JsonNode db = setup.Get("Database");
    std::string dbHost = db.Has("Host") ? db.GetString("Host") : "127.0.0.1";
    int         dbPort = db.Has("Port") ? (int)db.GetInt("Port") : 3306;
    std::string dbUser = db.Has("User") ? db.GetString("User") : "cims";
    std::string dbPass = db.Has("Password") ? db.GetString("Password") : "";
    std::string dbName = db.Has("DbName") ? db.GetString("DbName") : "cims";

    MYSQL* pMysql = mysql_init(NULL);
    if (!pMysql) { printf("[DB] mysql_init 실패\n"); return false; }

    if (!mysql_real_connect(pMysql, dbHost.c_str(), dbUser.c_str(), dbPass.c_str(),
                            dbName.c_str(), dbPort, NULL, 0)) {
        printf("[DB] 접속 실패: %s\n", mysql_error(pMysql));
        mysql_close(pMysql);
        return false;
    }
    mysql_set_character_set(pMysql, "utf8mb4");

    // v3 (2026-04-22): domain 은 CLI 인자 (-domain) 로 결정.
    //   DB 쿼리는 id / imsi / passwd 만. authId 는 SimSession 생성 시 imsi+@+strDomain 으로 조립.
    //   (sub.authId 는 imsi 만 담아 뒀다가 상위에서 완성)
    // VoIP 가입자
    if (strFilterMode.empty() || strFilterMode == "volte") {
        const char* sql =
            "SELECT cu.id, COALESCE(cu.imsi,''), cu.passwd "
            "FROM volte_subscriptions cu "
            "ORDER BY cu.id";
        if (mysql_query(pMysql, sql) == 0) {
            MYSQL_RES* res = mysql_store_result(pMysql);
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    DbSubscriber sub;
                    sub.id       = row[0] ? row[0] : "";
                    std::string imsi = row[1] ? row[1] : "";
                    sub.authId = imsi;   // domain 은 상위에서 붙임
                    sub.password    = row[2] ? row[2] : "";
                    sub.serviceType = "volte";
                    vecOut.push_back(sub);
                }
                mysql_free_result(res);
            }
        }
    }

    // PTT 가입자 (그룹 ID 지정 시 해당 그룹 멤버만 로드)
    if (strFilterMode.empty() || strFilterMode == "ptt") {
        std::string sql;
        if (!strGroupId.empty()) {
            sql = "SELECT pu.id, COALESCE(pu.imsi,''), pu.passwd "
                  "FROM ptt_subscriptions pu "
                  "JOIN ptt_group_members gm ON gm.user_id = pu.id "
                  "WHERE gm.group_id='" + strGroupId + "' "
                  "ORDER BY gm.priority, pu.id";
        } else {
            sql = "SELECT pu.id, COALESCE(pu.imsi,''), pu.passwd "
                  "FROM ptt_subscriptions pu "
                  "ORDER BY pu.id";
        }
        if (mysql_query(pMysql, sql.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(pMysql);
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    DbSubscriber sub;
                    sub.id       = row[0] ? row[0] : "";
                    std::string imsi = row[1] ? row[1] : "";
                    sub.authId = imsi;   // domain 은 상위에서 붙임
                    sub.password    = row[2] ? row[2] : "";
                    sub.serviceType = "ptt";
                    vecOut.push_back(sub);
                }
                mysql_free_result(res);
            }
        }
    }

    mysql_close(pMysql);
    printf("[DB] %d명 가입자 로드 완료 (%s)\n", (int)vecOut.size(),
           strFilterMode.empty() ? "all" : strFilterMode.c_str());
    return !vecOut.empty();
}

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
//  미디어 디렉토리에서 파일 목록 수집
// ─────────────────────────────────────────────
static std::vector<std::string> ListFilesInDir(const std::string& strDir, const std::string& strSuffix) {
    std::vector<std::string> vecFiles;
    DIR* dir = opendir(strDir.c_str());
    if (!dir) return vecFiles;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        std::string name = ent->d_name;
        if (name.size() >= strSuffix.size() &&
            name.compare(name.size() - strSuffix.size(), strSuffix.size(), strSuffix) == 0) {
            vecFiles.push_back(strDir + "/" + name);
        }
    }
    closedir(dir);
    std::sort(vecFiles.begin(), vecFiles.end());
    return vecFiles;
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
    printf("  -mode        <volte|ptt> 단말 유형 (default: volte)\n");
    printf("  -group       <group_id>  PTT 그룹 ID (default: 1000)\n");
    printf("  -scenario    <name>      자동 시나리오:\n");
    printf("                             register     - 등록만\n");
    printf("                             subscribe    - 등록 + GMS/CMS 구독 (PTT)\n");
    printf("                             call         - 등록 + 짝끼리 통화\n");
    printf("                             group-call   - 등록 + 구독 + 그룹통화 (PTT)\n");
    printf("                             full         - 전체 반복\n");
    printf("  -call_duration <secs>    통화 유지 시간 (default: 10, 버스트 모델 일괄 보유시간)\n");
    printf("  -floor_hold  <secs>      [ptt] 화자 순환 시 참여자별 발언(floor 보유) 시간 (default: 5)\n");
    printf("  -floor_loop              [ptt] 화자 순환을 종료(quit)까지 무한 반복 (장기 안정성 시험)\n");
    printf("  -floor_rounds <n>        [ptt] 화자 순환 반복 횟수 (전체 멤버 1바퀴=1round, default 1; -floor_loop 우선)\n");
    printf("  -cps         <N>         [call] 호 도착률(초당 호수). >0 이면 지속 부하 모델\n");
    printf("                             (1/cps 간격 발신 + 각 호 HT 후 개별 종료). 정상상태 동시호≈cps×ht\n");
    printf("  -ht          <secs>      [call] per-call 보유시간 (cps 모델, 미지정 시 call_duration)\n");
    printf("  -calls       <N>         [call] 총 발신 호수 (cps 모델, 미지정 시 단말쌍 수=count/2)\n");
    printf("  -callee_override <user>  call 시나리오 에서 INVITE target 을 덮음 (외부 peer 라우팅 시험용)\n");
    printf("  -media_file  <path>      AMR-WB 미디어 파일 (PT=99 전송, 생략 시 합성 RTP)\n");
    printf("  -media_dir   <dir>       미디어 디렉토리 (세션별 라운드로빈 할당)\n");
    printf("  -video_file  <path>      H.264 Annex B 비디오 파일 (PT=96 전송)\n");
    printf("  -no_video                비디오 비활성화 (음성 전용 통화)\n");
    printf("  -no_register             REGISTER 송신 skip (외부 SIP peer 모드)\n");
    printf("  -interval    <ms>        단말 기동 간격 ms (default: 100)\n");
    printf("  -db          <csp.json>  DB에서 가입자 정보 로드 (user/auth_id/password/domain 자동 설정)\n");
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
                        const std::string& strGroupId,
                        const std::string& strCalleeOverride = "",
                        int iCps = 0,
                        int iHtSec = 0,
                        int iTotalCalls = 0,
                        int iFloorHold = 5,
                        int iFloorLoop = 0,
                        int iFloorRounds = 1)
{
    // 1. 모든 단말이 등록될 때까지 대기 (최대 30초). no_register 세션은 즉시 ready.
    printf("[Scenario] Waiting for registration...\n");
    for (int retry = 0; retry < 300; ++retry) {
        int readyCount = 0;
        for (auto* s : sessions) if (s->m_bNoRegister || s->m_bRegistered) readyCount++;
        if (readyCount == (int)sessions.size()) break;
        usleep(100000);
    }
    {
        int regCount = 0, skipCount = 0;
        for (auto* s : sessions) {
            if (s->m_bNoRegister) skipCount++;
            else if (s->m_bRegistered) regCount++;
        }
        printf("[Scenario] %d/%d registered (%d no_register skip)\n",
               regCount, (int)sessions.size() - skipCount, skipCount);
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
            s->AffiliateGroup();   // MCPTT 그룹 affiliation (그룹콜 수신 조건)
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
        // 신규 (2026-06-01): 지속 호 부하 모델 — -cps (호 도착률/초) + -ht (per-call 보유시간).
        //   1/cps 간격으로 호를 발신하고 각 호를 HT 후 개별 종료 → 정상상태 동시호 ≈ cps × HT
        //   (Little's law). iCps<=0 또는 callee_override 면 기존 버스트 모델로 폴백(하위호환).
        if (iCps > 0 && strCalleeOverride.empty()) {
            int htSec = (iHtSec > 0) ? iHtSec : iCallDuration;
            int nPairs = (int)sessions.size() / 2;
            int totalCalls = nPairs;
            if (iTotalCalls > 0 && iTotalCalls < nPairs) totalCalls = iTotalCalls;
            int launchIntervalMs = (int)(1000.0 / (double)iCps);
            if (launchIntervalMs < 1) launchIntervalMs = 1;
            printf("[Scenario] Sustained call test: cps=%d ht=%ds total=%d calls "
                   "(launch interval=%dms, expected concurrent≈%d)\n",
                   iCps, htSec, totalCalls, launchIntervalMs, iCps * htSec);

            struct PendingStop { int pairIdx; long stopAtMs; };
            std::vector<PendingStop> active;
            long nowMs = 0;
            int launched = 0, stopped = 0, peak = 0;
            while (launched < totalCalls || !active.empty()) {
                // (a) 발신 예정 시각 도달한 호 발신
                while (launched < totalCalls && (long)launched * launchIntervalMs <= nowMs) {
                    int p = launched;
                    sessions[2 * p]->StartCall(sessions[2 * p + 1]->m_strUser);
                    active.push_back({ p, nowMs + (long)htSec * 1000 });
                    launched++;
                }
                if ((int)active.size() > peak) peak = (int)active.size();
                // (b) HT 경과한 호 개별 종료 (BYE)
                for (size_t k = 0; k < active.size(); ) {
                    if (nowMs >= active[k].stopAtMs) {
                        sessions[2 * active[k].pairIdx]->StopCall();
                        stopped++;
                        active.erase(active.begin() + (long)k);
                    } else {
                        ++k;
                    }
                }
                usleep(100000);
                nowMs += 100;
            }
            printf("[Scenario] Sustained call test done (launched=%d stopped=%d peak_concurrent=%d)\n",
                   launched, stopped, peak);
            return;
        }

        // 기존 버스트 모델 (cps 미지정 시 하위호환) — 전체를 200ms 간격 발신 후 일괄 보유/종료.
        if (!strCalleeOverride.empty()) {
            printf("[Scenario] Starting calls (callee_override='%s')...\n", strCalleeOverride.c_str());
            for (auto* s : sessions) {
                s->StartCall(strCalleeOverride);
                usleep(200000);
            }
        } else {
            printf("[Scenario] Starting paired calls...\n");
            for (int i = 0; i + 1 < (int)sessions.size(); i += 2) {
                std::string strTarget = sessions[i + 1]->m_strUser;
                sessions[i]->StartCall(strTarget);
                usleep(200000);
            }
        }
        for (int i = 0; i < iCallDuration * 10; i++) usleep(100000);
        printf("[Scenario] Ending calls...\n");
        for (auto* s : sessions) s->StopCall();
        return;
    }

    // 4. PTT 그룹통화 — 플로어 제어 순환 발언
    // PTT 단말은 발신하지 않음 — CSP가 CheckGroupIntegrity 로 자동 초대
    // 시뮬레이터는 등록/구독 후 CSP의 INVITE 를 기다려 응답만 함
    if (eScenario == E_SCENARIO_GROUP_CALL || eScenario == E_SCENARIO_FULL) {
        if (!sessions.empty() && sessions[0]->m_bPttMode) {
            printf("[Scenario] PTT mode: waiting for CSP to invite all members...\n");

            // Wait for all members to join (max 15s)
            bool bAllJoined = false;
            for (int retry = 0; retry < 150; ++retry) {
                int joinCount = 0;
                for (auto* s : sessions) if (s->m_bInCall) joinCount++;
                if (joinCount == (int)sessions.size()) { bAllJoined = true; break; }
                usleep(100000);
            }
            {
                int joinCount = 0;
                for (auto* s : sessions) if (s->m_bInCall) joinCount++;
                printf("[Scenario] %d/%d members in call (allJoined=%s)\n",
                       joinCount, (int)sessions.size(), bAllJoined ? "yes" : "no");
            }

            if (bAllJoined) {
                // Rotating floor control: each member speaks in order.
                // -floor_loop 이면 g_bQuit 까지 무한 반복(상주 멤버 floor 순환 — 장기 안정성 시험).
                // -floor_rounds N 이면 정확히 N 회 순환 후 종료(반복 시험). loop 가 우선.
                if (iFloorRounds < 1) iFloorRounds = 1;
                printf("[Scenario] Starting floor control rotation (%d members, %s)...\n",
                       (int)sessions.size(),
                       iFloorLoop ? "loop=infinite"
                                  : (iFloorRounds > 1 ? "rounds=" : "once") );
                if (!iFloorLoop && iFloorRounds > 1)
                    printf("[Scenario]   (floor_rounds=%d)\n", iFloorRounds);
                int iPass = 0;
                do {
                    ++iPass;
                    if (iFloorLoop || iFloorRounds > 1)
                        printf("[Scenario] ── rotation pass #%d%s ──\n",
                               iPass, iFloorLoop ? "" :
                               ("/" + std::to_string(iFloorRounds)).c_str());
                    for (int i = 0; i < (int)sessions.size(); ++i) {
                        if (g_bQuit) break;
                        if (!sessions[i]->m_bInCall) {
                            printf("[Scenario] Member %d (%s) not in call, skipping\n",
                                   i, sessions[i]->m_strUser.c_str());
                            continue;
                        }
                        printf("[Scenario] Member %d (%s): PTT Request (floor)\n",
                               i, sessions[i]->m_strUser.c_str());
                        sessions[i]->SendPttRequest();

                        // Speaking time: -floor_hold 초 (참여자별 발언 시간, default 5)
                        printf("[Scenario]   floor hold %ds...\n", iFloorHold);
                        for (int t = 0; t < iFloorHold * 10 && !g_bQuit; ++t) usleep(100000);

                        printf("[Scenario] Member %d (%s): PTT Release\n",
                               i, sessions[i]->m_strUser.c_str());
                        sessions[i]->SendPttRelease();

                        // 1 second gap between speakers
                        for (int t = 0; t < 10 && !g_bQuit; ++t) usleep(100000);
                    }
                } while ((iFloorLoop || iPass < iFloorRounds) && !g_bQuit);
                printf("[Scenario] Floor rotation complete (passes=%d)\n", iPass);
            } else {
                // Fallback: wait for call_duration if not all joined
                printf("[Scenario] Not all members joined, waiting %ds...\n", iCallDuration);
                for (int i = 0; i < iCallDuration * 10; i++) usleep(100000);
            }

            printf("[Scenario] PTT scenario done, stopping calls\n");
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
    int iLocalBasePort         = atoi(GetArg(argc, argv, "-local_port",  "0").c_str());
    int iCount                 = atoi(GetArg(argc, argv, "-count",       "1").c_str());
    std::string strStartUser  = GetArg(argc, argv, "-user",        "1000");
    std::string strExplicitAuthId = GetArg(argc, argv, "-auth_id", "");
    std::string strDomain     = GetArg(argc, argv, "-domain",     "csp");
    std::string strPassword   = GetArg(argc, argv, "-password",   "1234");
    std::string strMode       = GetArg(argc, argv, "-mode",       "volte");
    std::string strGroupId    = GetArg(argc, argv, "-group",      "1000");
    std::string strScenario   = GetArg(argc, argv, "-scenario",   "");
    int iCallDuration          = atoi(GetArg(argc, argv, "-call_duration", "10").c_str());
    // 지속 호 부하 모델 (2026-06-01): -cps 호 도착률(/초), -ht per-call 보유시간(초),
    //   -calls 총 발신 호수. -cps>0 이면 call 시나리오가 1/cps 간격 발신 + HT 후 개별 종료.
    int iCps                   = atoi(GetArg(argc, argv, "-cps",           "0").c_str());
    int iHtSec                 = atoi(GetArg(argc, argv, "-ht",            "0").c_str());
    int iTotalCalls            = atoi(GetArg(argc, argv, "-calls",         "0").c_str());
    // PTT 그룹콜 화자 순환 시 참여자별 발언(floor 보유) 시간(초). call_duration 과 별개.
    int iFloorHold             = atoi(GetArg(argc, argv, "-floor_hold",    "5").c_str());
    int iFloorLoop             = HasFlag(argc, argv, "-floor_loop") ? 1 : 0;
    // PTT 그룹콜 화자 순환 반복 횟수(전체 멤버 1바퀴 = 1 round). default 1(1바퀴).
    //   -floor_loop 가 지정되면 무한 반복이 우선한다.
    int iFloorRounds           = atoi(GetArg(argc, argv, "-floor_rounds",  "1").c_str());
    // G8 (2026-04-23): 외부 peer routing 시험용. 비우면 기존 pair 로직 유지.
    //   값이 있으면 call scenario 의 모든 outbound INVITE target 을 이 user 로 덮음.
    std::string strCalleeOverride = GetArg(argc, argv, "-callee_override", "");
    std::string strMediaFile  = GetArg(argc, argv, "-media_file",  "");
    std::string strMediaDir   = GetArg(argc, argv, "-media_dir",   "");
    std::string strVideoFile  = GetArg(argc, argv, "-video_file",  "");
    bool bNoVideo              = HasFlag(argc, argv, "-no_video");
    int iIntervalMs            = atoi(GetArg(argc, argv, "-interval",    "100").c_str());
    std::string strDbConfig   = GetArg(argc, argv, "-db",            "");
    bool bVerbose              = HasFlag(argc, argv, "-verbose");
    bool bPttMode              = (strMode == "ptt");
    // 외부 SIP peer 모드: REGISTER 송신 skip. INVITE 수신 시 SimSession 기본 핸들러가
    // 180→200 OK 자동 응답. IBCF trunk 시나리오의 mock 외부 peer 또는 register 가
    // 거부되는 ISP(IBCF role only)로 직접 INVITE 발신할 때 사용.
    bool bNoRegister           = HasFlag(argc, argv, "-no_register");

    if (strLocalIp.empty()) strLocalIp = GetLocalIp();

    // DB 모드: csp.json에서 가입자 정보 로드
    std::vector<DbSubscriber> vecDbSubs;
    bool bDbMode = false;
    if (!strDbConfig.empty()) {
        std::string strDbRealm;
        if (LoadSubscribersFromDb(strDbConfig, strMode, bPttMode ? strGroupId : "", vecDbSubs, strDbRealm)) {
            bDbMode = true;
            if (!strDbRealm.empty() && strDomain == "csp") {
                strDomain = strDbRealm;  // -domain 미지정 시 DB realm 사용
            }
            if (iCount <= 1 && !HasFlag(argc, argv, "-count")) {
                iCount = (int)vecDbSubs.size();
            }
        }
    }

    // 시나리오 선택
    ESimScenario eScenario = E_SCENARIO_NONE;
    if      (strScenario == "register")   eScenario = E_SCENARIO_REGISTER;
    else if (strScenario == "subscribe")  eScenario = E_SCENARIO_SUBSCRIBE;
    else if (strScenario == "call")       eScenario = E_SCENARIO_CALL;
    else if (strScenario == "group-call" || strScenario == "group_call") eScenario = E_SCENARIO_GROUP_CALL;
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
    if (iLocalBasePort > 0)
        printf("  로컬   : %s (시작포트 %d)\n", strLocalIp.c_str(), iLocalBasePort);
    else
        printf("  로컬   : %s (자동 포트 할당)\n", strLocalIp.c_str());
    printf("  단말수 : %d개  (시작ID: %s)\n", iCount, strStartUser.c_str());
    printf("  모드   : %s\n", bPttMode ? "PTT" : "VoIP");
    if (bPttMode) printf("  그룹ID : %s\n", strGroupId.c_str());
    if (bDbMode) printf("  DB가입자: %d명 로드 (domain: %s)\n", (int)vecDbSubs.size(), strDomain.c_str());
    if (!strScenario.empty()) printf("  시나리오: %s\n", strScenario.c_str());
    if (!strMediaDir.empty()) printf("  미디어디렉토리: %s\n", strMediaDir.c_str());
    printf("\n");

    // 미디어 디렉토리에서 파일 목록 수집
    std::vector<std::string> vecAudioFiles;
    std::vector<std::string> vecVideoFiles;
    if (!strMediaDir.empty()) {
        vecAudioFiles = ListFilesInDir(strMediaDir, "_audio.amrwb");
        if (!bNoVideo) {
            vecVideoFiles = ListFilesInDir(strMediaDir, "_video.h264");
        }
        printf("  미디어 파일 수: audio=%d, video=%d%s\n",
               (int)vecAudioFiles.size(), (int)vecVideoFiles.size(),
               bNoVideo ? " (video disabled)" : "");
    }

    // 세션 생성 및 시작
    std::vector<SimSession*> sessions;
    sessions.reserve(iCount);

    for (int i = 0; i < iCount; i++) {
        int iLocalPort = (iLocalBasePort > 0) ? iLocalBasePort + (i * 2) : 0;

        std::string strUser, strAuthId, strPwd;
        if (bDbMode && i < (int)vecDbSubs.size()) {
            // DB 모드: v3 — authId 는 DB 의 imsi + CLI -domain 으로 조립
            strUser   = vecDbSubs[i].id;
            if (!vecDbSubs[i].authId.empty() && vecDbSubs[i].authId.find('@') == std::string::npos) {
                strAuthId = vecDbSubs[i].authId + "@" + strDomain;
            } else {
                strAuthId = vecDbSubs[i].authId;  // 이미 @ 포함(레거시)이거나 빈 값
            }
            strPwd    = vecDbSubs[i].password;
        } else {
            strUser = MakeUserId(strStartUser, i);
            strPwd  = strPassword;
            if (!strExplicitAuthId.empty() && i > 0) {
                std::string base = strExplicitAuthId;
                size_t atPos = base.find('@');
                std::string suffix = (atPos != std::string::npos) ? base.substr(atPos) : "";
                std::string prefix = (atPos != std::string::npos) ? base.substr(0, atPos) : base;
                int numStart = (int)prefix.size();
                while (numStart > 0 && isdigit(prefix[numStart - 1])) numStart--;
                if (numStart < (int)prefix.size()) {
                    long long num = atoll(prefix.c_str() + numStart);
                    num += i;
                    char buf[32];
                    int digits = (int)prefix.size() - numStart;
                    snprintf(buf, sizeof(buf), "%0*lld", digits, num);
                    strAuthId = prefix.substr(0, numStart) + buf + suffix;
                } else {
                    strAuthId = strExplicitAuthId;
                }
            } else if (!strExplicitAuthId.empty()) {
                strAuthId = strExplicitAuthId;
            } else {
                if (bPttMode && !strUser.empty() && strUser[0] == '+') {
                    strAuthId = DerivePttAuthId(strUser, strDomain);
                } else {
                    strAuthId = strUser;
                }
            }
        }

        SimSession* s = new SimSession(
            i,
            strUser,
            strAuthId,
            strDomain,
            strPwd,
            strServerIp, iServerPort,
            strLocalIp,  iLocalPort,
            bPttMode,
            strGroupId
        );
        s->SetNoRegister(bNoRegister);

        // Per-session media files from directory (round-robin)
        if (!vecAudioFiles.empty()) {
            std::string audioFile = vecAudioFiles[i % vecAudioFiles.size()];
            s->m_clsRtpThread.SetMediaFile(audioFile);
            printf("[%d] Audio: %s\n", i, audioFile.c_str());
        } else if (!strMediaFile.empty()) {
            s->m_clsRtpThread.SetMediaFile(strMediaFile);
        }
        if (!bNoVideo) {
            if (!vecVideoFiles.empty()) {
                std::string videoFile = vecVideoFiles[i % vecVideoFiles.size()];
                s->m_clsRtpThread.SetVideoFile(videoFile);
                printf("[%d] Video: %s\n", i, videoFile.c_str());
            } else if (!strVideoFile.empty()) {
                s->m_clsRtpThread.SetVideoFile(strVideoFile);
            }
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
                                     strGroupId,
                                     strCalleeOverride,
                                     iCps,
                                     iHtSec,
                                     iTotalCalls,
                                     iFloorHold,
                                     iFloorLoop,
                                     iFloorRounds);
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
                usleep(30000);
                s->AffiliateGroup();
            }
            printf("SUBSCRIBE + AFFILIATE 전송\n");
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
