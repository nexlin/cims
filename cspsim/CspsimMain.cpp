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
#include <map>
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
    std::string password;     // 평문 — CLI/-creds 전용 (DB 에는 없다: passwd 컬럼 DROP, sip_access_security.md §4.7 ⑥)
    std::string ha1;          // SIP Digest H(A1) (sip_access_security.md §4) — DB 모드의 유일한 Digest 자격
    std::string serviceType;  // "volte" or "ptt"
    std::string sipTransport; // 채널 정책 (sip_access_security.md §3) — ''(무정책)|UDP|TCP|TLS
    std::string authScheme;   // digest|aka — aka 는 TLS 위 AKAv1-MD5 만 성립 (Annex X)
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
    //   DB 쿼리는 id / imsi / ha1. authId 는 SimSession 생성 시 imsi+@+strDomain 으로 조립.
    //   인증 자료는 ha1 (원문 없이 response 계산) — passwd 컬럼은 없다(§4.7 ⑥). ha1 이 비면 등록 불가.
    //   (sub.authId 는 imsi 만 담아 뒀다가 상위에서 완성)
    // VoIP 가입자
    if (strFilterMode.empty() || strFilterMode == "volte") {
        const char* sql =
            "SELECT cu.id, COALESCE(cu.imsi,''), COALESCE(cu.ha1,''), "
            "COALESCE(cu.sip_transport,''), COALESCE(cu.auth_scheme,'digest') "
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
                    sub.ha1          = row[2] ? row[2] : "";
                    sub.sipTransport = row[3] ? row[3] : "";
                    sub.authScheme   = row[4] ? row[4] : "digest";
                    sub.serviceType  = "volte";
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
            // group_id(멤버) 는 surrogate ptt_groups.id → mcptt_group_id 식별자로 JOIN
            sql = "SELECT pu.id, COALESCE(pu.imsi,''), COALESCE(pu.ha1,''), "
                  "COALESCE(pu.sip_transport,''), COALESCE(pu.auth_scheme,'digest') "
                  "FROM ptt_subscriptions pu "
                  "JOIN ptt_group_members gm ON gm.user_id = pu.id "
                  "JOIN ptt_groups g ON g.id = gm.group_id "
                  "WHERE g.mcptt_group_id='" + strGroupId + "' "
                  "ORDER BY gm.priority, pu.id";
        } else {
            sql = "SELECT pu.id, COALESCE(pu.imsi,''), COALESCE(pu.ha1,''), "
                  "COALESCE(pu.sip_transport,''), COALESCE(pu.auth_scheme,'digest') "
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
                    sub.ha1          = row[2] ? row[2] : "";
                    sub.sipTransport = row[3] ? row[3] : "";
                    sub.authScheme   = row[4] ? row[4] : "digest";
                    sub.serviceType  = "ptt";
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

// ─────────────────────────────────────────────
//  단말별 자격 파일 (-creds, sip_access_security.md §4.7)
// ─────────────────────────────────────────────
// JSONL — 한 줄 = 한 단말:
// {"user":"+82...","ha1":"<hex32>","authId":"...","password":"...","k":"<hex32>","opc":"<hex32>","sqn":"0",
//  "login":"test001","loginPw":"1234"}
//   · user (필수)         : 전개된 사용자 ID 와 정확 일치하는 매칭 키
//   · ha1 / password / k  : 셋 중 하나 필수 — ha1 우선(원문 없이 response 계산)
//                           k+opc 는 IMS AKA(sip_access_security.md §8.2)
//   · authId (선택)       : '@' 없으면 -domain 을 붙여 조립(DB 모드와 같다)
//   · login/loginPw (선택): IdMS 로그인 자격(users.login_id/passwd) — XCAP 토큰(authreq) 용.
//                           SIP 자격과 별개(§4.7). 없으면 구식 tel:+msisdn/-password 로그인 폴백.
// "같은 비밀번호 구간" 없이 -count 전개 단말 각각에 자기 자격을 준다.
struct UserCred {
    std::string authId;
    std::string password;
    std::string ha1;
    std::string
        k; // IMS AKA K (hex32) — 있으면 AKAv1-MD5 챌린지에 Milenage 로 답한다
    std::string opc; // IMS AKA OPc (hex32)
    std::string sqn; // 단말 초기 SQN_MS (10진, 선택)
    std::string login;   // IdMS 로그인 ID (XCAP 토큰) — 선택
    std::string loginPw; // IdMS 로그인 비밀번호 — 선택
};

static bool LoadCredsFile(const std::string& strPath, std::map<std::string, UserCred>& mapOut,
                          std::vector<std::string>& vecOrder) {
    std::ifstream ifs(strPath);
    if (!ifs.is_open()) {
        printf("[CREDS] 자격 파일 열기 실패: %s\n", strPath.c_str());
        return false;
    }
    std::string line;
    int iLine = 0;
    while (std::getline(ifs, line)) {
        iLine++;
        size_t p = line.find_first_not_of(" \t\r\n");
        if (p == std::string::npos || line[p] == '#') continue;   // 공백/주석
        SimpleJson::JsonNode node = SimpleJson::JsonNode::Parse(line);
        std::string strUser = node.GetString("user");
        UserCred cred;
        cred.authId   = node.GetString("authId");
        cred.password = node.GetString("password");
        cred.ha1      = node.GetString("ha1");
        cred.k = node.GetString("k");
        cred.opc = node.GetString("opc");
        cred.sqn = node.GetString("sqn");
        cred.login = node.GetString("login");
        cred.loginPw = node.GetString("loginPw");
        if (strUser.empty() ||
            (cred.ha1.empty() && cred.password.empty() && cred.k.empty())) {
          printf("[CREDS] %s:%d — user 또는 자격(ha1/password/k) 누락\n",
                 strPath.c_str(), iLine);
          return false;
        }
        if (mapOut.find(strUser) == mapOut.end()) vecOrder.push_back(strUser);
        mapOut[strUser] = cred;
    }
    if (mapOut.empty()) {
        printf("[CREDS] 자격 파일에 항목이 없음: %s\n", strPath.c_str());
        return false;
    }
    return true;
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
    int affiliateOk = 0, affiliateRej = 0;
    int xcapTokenOk = 0, xcapTokenFail = 0, xcapOk = 0, xcap304 = 0, xcapFail = 0;
    long long totalRegMs = 0, totalCallMs = 0;
    int registered = 0, inCall = 0;

    for (auto* s : sessions) {
        totalReg   += s->m_stats.iRegOk.load();
        failReg    += s->m_stats.iRegFail.load();
        gmsOk      += s->m_stats.iGmsOk.load();
        cmsOk      += s->m_stats.iCmsOk.load();
        notifyRecv += s->m_stats.iNotifyRecv.load();
        confNotify += s->m_stats.iConfNotify.load();
        affiliateOk  += s->m_stats.iAffiliateOk.load();
        affiliateRej += s->m_stats.iAffiliateRej.load();
        xcapTokenOk   += s->m_stats.iXcapTokenOk.load();
        xcapTokenFail += s->m_stats.iXcapTokenFail.load();
        xcapOk        += s->m_stats.iXcapOk.load();
        xcap304       += s->m_stats.iXcap304.load();
        xcapFail      += s->m_stats.iXcapFail.load();
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
    if (affiliateOk || affiliateRej)
        printf("  Affiliation   : ok=%d rej=%d  (rej=CSP 멤버십 게이트 403 — 비멤버 그룹)\n",
               affiliateOk, affiliateRej);
    if (xcapTokenOk || xcapTokenFail || xcapOk || xcap304 || xcapFail) {
        printf("  XCAP Token    : %d  (fail=%d)\n", xcapTokenOk, xcapTokenFail);
        printf("  XCAP GET      : 200=%d 304=%d fail=%d\n", xcapOk, xcap304, xcapFail);
    }
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
    printf("  -ha1         <hex32>     SIP Digest H(A1) — 지정 시 -password 대신 이 값으로 response 계산\n");
    printf("                             (-db 모드는 DB 의 ha1 을 사용 — 비어 있으면 등록 불가)\n");
    printf("  -creds       <file>      단말별 자격 파일(JSONL: "
           "user/ha1/authId/password/k/opc/sqn/login/loginPw)\n");
    printf("  -users_from_creds        전개 로스터 = -creds 파일의 user 순서 (-no-db 전용, -user 무시,"
           " 번호 비연속 창 가능)\n");
    printf("  -aka_k       <hex32>     IMS AKA K — 지정 시 AKAv1-MD5 챌린지에 "
           "Milenage 로 답한다 (TLS 필수)\n");
    printf("  -aka_opc     <hex32>     IMS AKA OPc\n");
    printf("  -aka_sqn     <n>         단말 초기 SQN_MS (기본 0. 큰 값이면 SQN "
           "이탈 → AUTS 재동기 경로 재현)\n");
    printf(
        "  -sec_agree               RFC 3329 sec-agree 협상 (Security-Client: "
        "tls → 401 Security-Server → Security-Verify echo)\n");
    printf("  -sec_verify  <값>        [시험] Security-Verify 를 echo 대신 이 "
           "값으로 전송 (강등 변조 → 서버 494)\n");
    printf("                             -count 전개 단말 각각의 자격 — 파일에 없는 user 는 즉시 중단\n");
    printf(
        "  -srtp off|optional|required  미디어 SRTP (SDES, media_security.md §8) — optional=AVP+a=crypto\n"
        "                           best-effort 오퍼, required=RTP/SAVP 오퍼. -sec_agree 동반 시\n"
        "                           REGISTER 에 sdes-srtp;mediasec 능력 선언(§4.1)\n");
    printf("  -ipsec                   IMS AKA + IPsec (TS 33.203 §7) — Security-Client 에 "
           "ipsec-3gpp 제안, 401 뒤 커널 SA 설치 (CAP_NET_ADMIN, -aka_k 필수)\n");
    printf("  -ipsec_alg   <alg>       hmac-sha-1-96(기본)|hmac-md5-96\n");
    printf("  -ipsec_ealg  <ealg>      aes-cbc(기본)|null\n");
    printf("  -mode        <volte|ptt> 단말 유형 (default: volte)\n");
    printf("  -transport   <udp|tcp|tls> 시그널링 transport (default: udp)\n");
    printf("                             tls 는 서버 인증서를 검증하지 않는다(랩 자가서명 수용)\n");
    printf("  -group       <group_id>  PTT 그룹 ID (default: 1000)\n");
    printf("  -scenario    <name>      자동 시나리오:\n");
    printf("                             register     - 등록만 (-hold <secs> 로 등록 유지 후 해제)\n");
    printf("                             subscribe    - 등록 + GMS/CMS 구독 (PTT)\n");
    printf("                             call         - 등록 + 짝끼리 통화\n");
    printf("                             group-call   - 등록 + 구독 + 그룹통화 (PTT)\n");
    printf("                             full         - 전체 반복\n");
    printf("                             transfer     - [volte] A→B 후 A blind REFER(→C): 전달 후 B–C (-count 3)\n");
    printf("                             transfer_attended - [volte] A→B + A→C 상담 후 attended REFER: B–C (-count 3)\n");
    printf("                             pickup       - [volte] A→B 링잉 중 C 가 당겨받기 코드(-pickup_code) 다이얼: A–C (-count 3)\n");
    printf("                             dialog_pickup- [volte] C 가 B 를 dialog 구독(BLF) → A→B 링잉 NOTIFY → C INVITE-Replaces: A–C (-count 3)\n");
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
    printf("  -no_xcap                 xcap-diff NOTIFY 수신 시 XCAP HTTP GET skip (기본: 자동 GET)\n");
    printf("  -no_conf_sub             [ptt] conference 구독 skip — 구독 미구현 단말(구 APK) 재현용\n");
    printf("  -csc_ip <IP>             [ptt] CSC 서버 IP — REGISTER 전 IdMS auth 수행\n");
    printf("  -csc_port <N>            [ptt] CSC McpttServer 포트 (기본: 4530)\n");
    printf("  -csc_tls                 [ptt] CSC TLS 사용 (기본: off, 테스트 환경)\n");
    printf("  -xcap_root <url>         [ptt] SUBSCRIBE 후 XCAP 문서 능동 GET (예: https://121.161.164.47:4430/)\n");
    printf("  -interval    <ms>        단말 기동 간격 ms (default: 100)\n");
    printf("  -db          <csp.json>  DB에서 가입자 정보 로드 (user/auth_id/password/domain 자동 설정)\n");
    printf("  -db_offset   <N>         [db] 로드된 가입자 앞쪽 N명 건너뜀 (인스턴스별 disjoint 풀 할당)\n");
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
static bool g_bPreempt = false;   // -preempt: floor 선점(preemption) 검증 시퀀스
static int  g_iPreemptBy = 0;     // -preempt_by N: 선점자 세션 인덱스 (default 0; chair 검증 시 chair 인덱스)
static bool g_bNoConfSub = false;  // -no_conf_sub: conference 구독 skip (구 APK 재현)
static std::string g_strXcapRoot; // -xcap_root: SUBSCRIBE 후 XCAP 문서 능동 GET (Phase 3D 검증)
static std::string g_strPickupCode = "**";  // -pickup_code: 당겨받기 코드 (pickup 시나리오, 서버 pickup_feature_code/CallPickupId 와 일치)

// -hold <secs>: register 시나리오가 등록을 유지하는 시간 — 등록 유지 중 외부 프로브(비보호 요청 403 등)를
//   받기 위한 창. 0 이면 종전대로 등록 직후 해제. (RunScenario 시그니처를 늘리지 않으려 파일 정적)
static int g_iRegisterHoldSec = 0;

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

    // 실제 UE 플로우: REGISTER 200 OK 직후 자신의 등록 상태를 Event: reg 로 구독 (RFC 3680)
    for (auto* s : sessions) {
        if (s->m_bNoRegister || !s->m_bRegistered) continue;
        s->SubscribeReg();
        usleep(30000);
    }

    if (eScenario == E_SCENARIO_REGISTER) {
        if (g_iRegisterHoldSec > 0) {
            // 마커 "[Scenario] registration held Ns" 는 verify(S3-SCN-IPSEC-LIVE V25)가 프로브 시점으로 읽는다
            printf("[Scenario] registration held %ds — probes may run now\n", g_iRegisterHoldSec);
            fflush(stdout);
            sleep(g_iRegisterHoldSec);
            printf("[Scenario] registration hold done\n");
        }
        return;
    }

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
            // 참가자 정보 구독 (RFC 4575) — 그룹콜 참가자 NOTIFY 를 구독 경로로
            // 수신
            //   -no_conf_sub 면 구독을 걸지 않는다: 구독 미구현 단말(구 APK)이 통화 dialog
            //   in-dialog NOTIFY 폴백으로만 명단을 받는 혼재 상황을 재현하기 위한 스위치.
            if (!s->m_strGroupId.empty() && !g_bNoConfSub) {
              s->SubscribeConference(s->m_strGroupId);
              usleep(50000);
            }
        }
        // 구독 완료 대기 (최대 10초)
        for (int retry = 0; retry < 100; ++retry) {
            int subCount = 0;
            for (auto* s : sessions) if (s->m_bGmsSubscribed) subCount++;
            if (subCount == (int)sessions.size()) break;
            usleep(100000);
        }
        printf("[Scenario] Subscriptions complete\n");

        // Phase 3D 검증: xcap-root 지정 시 SUBSCRIBE 후 XCAP 문서 능동 취득
        // (token PKCE → TLS GET 200 → If-None-Match 304). 정식 흐름은 NOTIFY 트리거.
        if (!g_strXcapRoot.empty()) {
            usleep(300000);
            for (auto* s : sessions) {
                if (s->m_bPttMode) s->ProbeXcap(g_strXcapRoot);
            }
        }
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
            // 총 발신 호수: -calls 지정 시 그 값(pair 수 무관 — pair 재사용으로 장시간 지속).
            //   미지정 시 pair 수(각 pair 1회). 동시호 ≈ cps×ht 이므로 nPairs > cps×ht 필요.
            int totalCalls = (iTotalCalls > 0) ? iTotalCalls : nPairs;
            int launchIntervalMs = (int)(1000.0 / (double)iCps);
            if (launchIntervalMs < 1) launchIntervalMs = 1;
            printf("[Scenario] Sustained call test: cps=%d ht=%ds total=%d calls pairs=%d "
                   "(launch interval=%dms, expected concurrent≈%d)\n",
                   iCps, htSec, totalCalls, nPairs, launchIntervalMs, iCps * htSec);

            struct PendingStop { int pairIdx; long stopAtMs; };
            std::vector<PendingStop> active;
            // 가용 pair 풀 — 호 종료(HT 경과) 시 반환하여 재사용 → 적은 단말로 장시간 지속.
            std::vector<int> freePairs;
            for (int i = 0; i < nPairs; i++) freePairs.push_back(i);
            long nowMs = 0, nextLaunchMs = 0;
            int launched = 0, stopped = 0, peak = 0, skipped = 0;
            while (launched < totalCalls || !active.empty()) {
                // (a) 발신 예정 시각 도달 — 가용 pair 있으면 발신, 없으면(과부하) skip
                while (launched < totalCalls && nextLaunchMs <= nowMs) {
                    if (!freePairs.empty()) {
                        int p = freePairs.back(); freePairs.pop_back();
                        sessions[2 * p]->StartCall(sessions[2 * p + 1]->m_strUser);
                        active.push_back({ p, nowMs + (long)htSec * 1000 });
                        launched++;
                    } else {
                        skipped++;   // 가용 단말 없음 — 슬롯 건너뜀(무음 누락 방지 위해 카운트/로그)
                    }
                    nextLaunchMs += launchIntervalMs;
                }
                if ((int)active.size() > peak) peak = (int)active.size();
                // (b) HT 경과한 호 개별 종료(BYE) → pair 반환
                for (size_t k = 0; k < active.size(); ) {
                    if (nowMs >= active[k].stopAtMs) {
                        sessions[2 * active[k].pairIdx]->StopCall();
                        freePairs.push_back(active[k].pairIdx);
                        stopped++;
                        active.erase(active.begin() + (long)k);
                    } else {
                        ++k;
                    }
                }
                usleep(100000);
                nowMs += 100;
            }
            printf("[Scenario] Sustained call test done (launched=%d stopped=%d peak_concurrent=%d skipped=%d)\n",
                   launched, stopped, peak, skipped);
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

    // 3b. 호 전달·당겨받기 (volte_supplementary_services.md §5·§6) — 3 단말(A,B,C) 필요.
    //   서버(B2BUA)가 REFER/픽업을 종단하며, 재고정된 leg 로 미디어가 이어지는지(원 relay 유지)를
    //   각 단말의 누적 수신 RTP 로 검증한다. (검증 아이템 S3-SCN-XFER/PICKUP 이 결과를 판정.)
    if (eScenario == E_SCENARIO_TRANSFER || eScenario == E_SCENARIO_TRANSFER_ATTENDED ||
        eScenario == E_SCENARIO_PICKUP || eScenario == E_SCENARIO_DIALOG_PICKUP) {
        if (sessions.size() < 3) {
            printf("[Scenario] transfer/pickup 는 -count 3 (A,B,C) 필요 — 현재 %d\n", (int)sessions.size());
            g_bScenarioDone = true;
            return;
        }
        SimSession* A = sessions[0];
        SimSession* B = sessions[1];
        SimSession* C = sessions[2];
        auto waitInCall = [&](SimSession* s, int tenths) {
            for (int t = 0; t < tenths && !g_bQuit; ++t) {
                if (s->m_bInCall) return true;
                usleep(100000);
            }
            return s->m_bInCall;
        };

        if (eScenario == E_SCENARIO_DIALOG_PICKUP) {
            // 관제 BLF: C 가 B 를 dialog 구독 → A→B 링잉 시 dialog NOTIFY 로 leg Call-ID 학습 →
            //   C 가 INVITE-Replaces 로 그 링잉 호를 가져간다(§6.2). 서버가 A–C 연결.
            B->SetRingHold(true);
            printf("[Scenario] BLF: C(%s) SUBSCRIBE dialog watched=B(%s)\n", C->m_strUser.c_str(),
                   B->m_strUser.c_str());
            C->SubscribeDialog(B->m_strUser);
            for (int t = 0; t < 15 && !g_bQuit; ++t) usleep(100000);  // 구독 확립 대기
            printf("[Scenario] BLF: A(%s) -> B(%s) [ring-hold]\n", A->m_strUser.c_str(), B->m_strUser.c_str());
            A->StartCall(B->m_strUser);
            // C 가 dialog NOTIFY(early)로 링잉 leg Call-ID 를 받을 때까지 대기 (최대 6s)
            for (int t = 0; t < 60 && !g_bQuit; ++t) {
                if (!C->m_strWatchedDlgCallId.empty() && C->m_strWatchedDlgState == "early") break;
                usleep(100000);
            }
            if (C->m_strWatchedDlgCallId.empty()) {
                printf("[Scenario] BLF: dialog NOTIFY 미수신 — INVITE-Replaces 생략(실패)\n");
            } else {
                printf("[Scenario] BLF: C INVITE-Replaces(call-id=%s) 당겨받기\n",
                       C->m_strWatchedDlgCallId.c_str());
                // RFC 3891 태그 방향: picker to-tag=원격(remote)·from-tag=로컬(local) — 서버가 양방향 대조.
                C->StartCallWithReplaces(B->m_strUser, C->m_strWatchedDlgCallId, C->m_strWatchedDlgRemoteTag,
                                         C->m_strWatchedDlgLocalTag);
            }
            bool aOk = waitInCall(A, 40), cOk = waitInCall(C, 40);
            printf("[Scenario] BLF result: dialog_notify=%d A_inCall=%d C_inCall=%d B_inCall=%d\n",
                   C->m_iDialogNotifyCount.load(), aOk, cOk, B->m_bInCall);
        } else if (eScenario == E_SCENARIO_PICKUP) {
            // A→B 링잉(B ring-hold: 180만) 중 C 가 당겨받기 코드 다이얼 → 서버 PickUp 이 A–C 연결.
            B->SetRingHold(true);
            printf("[Scenario] PICKUP: A(%s) -> B(%s) [ring-hold]\n", A->m_strUser.c_str(), B->m_strUser.c_str());
            A->StartCall(B->m_strUser);
            for (int t = 0; t < 20 && !g_bQuit; ++t) usleep(100000);  // 2s — B 링잉 확립 대기
            printf("[Scenario] PICKUP: C(%s) dials pickup code '%s'\n", C->m_strUser.c_str(), g_strPickupCode.c_str());
            C->StartCall(g_strPickupCode);
            bool aOk = waitInCall(A, 40), cOk = waitInCall(C, 40);
            printf("[Scenario] PICKUP result: A_inCall=%d C_inCall=%d B_inCall=%d\n", aOk, cOk, B->m_bInCall);
        } else if (eScenario == E_SCENARIO_TRANSFER) {
            // A→B 확립 후 A 가 blind REFER(→C): 전달 후 B–C.
            printf("[Scenario] BLIND XFER: A(%s) -> B(%s)\n", A->m_strUser.c_str(), B->m_strUser.c_str());
            A->StartCall(B->m_strUser);
            waitInCall(A, 60);
            waitInCall(B, 60);
            for (int t = 0; t < 20 && !g_bQuit; ++t) usleep(100000);  // 2s 통화 유지(기준 미디어)
            printf("[Scenario] BLIND XFER: A REFER(blind) -> C(%s)\n", C->m_strUser.c_str());
            A->BlindTransfer(C->m_strUser);
            bool cOk = waitInCall(C, 60);
            printf("[Scenario] BLIND XFER result: C_inCall=%d B_inCall=%d A_inCall=%d\n", cOk, B->m_bInCall,
                   A->m_bInCall);
        } else {
            // attended: A→B + A→C(상담) 후 attended REFER → B–C.
            printf("[Scenario] ATT XFER: A(%s) -> B(%s)\n", A->m_strUser.c_str(), B->m_strUser.c_str());
            A->StartCall(B->m_strUser);
            waitInCall(A, 60);
            waitInCall(B, 60);
            for (int t = 0; t < 15 && !g_bQuit; ++t) usleep(100000);
            printf("[Scenario] ATT XFER: A -> C(%s) [consult]\n", C->m_strUser.c_str());
            A->StartConsultCall(C->m_strUser);
            waitInCall(C, 60);
            for (int t = 0; t < 15 && !g_bQuit; ++t) usleep(100000);
            printf("[Scenario] ATT XFER: A REFER(attended, Replaces consult)\n");
            A->AttendedTransfer();
            for (int t = 0; t < 30 && !g_bQuit; ++t) usleep(100000);  // 전달 완결·재-INVITE 대기
            printf("[Scenario] ATT XFER result: B_inCall=%d C_inCall=%d A_inCall=%d\n", B->m_bInCall, C->m_bInCall,
                   A->m_bInCall);
        }

        // 재고정 후 미디어 흐름 관찰 창 — 이후 검증 아이템이 각 단말 누적 수신 RTP 로 판정한다.
        unsigned long long a0 = A->RecvPackets(), b0 = B->RecvPackets(), c0 = C->RecvPackets();
        int obs = iCallDuration > 0 ? iCallDuration : 4;
        for (int t = 0; t < obs * 10 && !g_bQuit; ++t) usleep(100000);
        printf("[Scenario] RTP recv delta over %ds: A=+%llu B=+%llu C=+%llu\n", obs, A->RecvPackets() - a0,
               B->RecvPackets() - b0, C->RecvPackets() - c0);
        printf("[Scenario] transfer/pickup done, stopping calls\n");
        for (auto* s : sessions) s->StopCall();
        return;
    }

    // 4. PTT 그룹통화 — 플로어 제어 순환 발언 (MCPTT 규격: on-demand)
    //   규격 모델: 발신 UE 1명이 그룹 INVITE(키업)로 세션을 개시(ProcessGroupCall) →
    //   CSP 가 affiliate+등록된 나머지 멤버에게 fan-out → 멤버는 auto-answer.
    //   (chat 그룹은 affiliation 만으로도 CheckGroupIntegrity 가 합류시킴.)
    if (eScenario == E_SCENARIO_GROUP_CALL || eScenario == E_SCENARIO_FULL) {
        if (!sessions.empty() && sessions[0]->m_bPttMode) {
            // 발신자(session[0]) 가 그룹콜 개시 (on-demand)
            printf("[Scenario] PTT on-demand: session[0] (%s) originating group INVITE -> %s\n",
                   sessions[0]->m_strUser.c_str(), strGroupId.c_str());
            sessions[0]->StartGroupCall(strGroupId);
            usleep(200000);
            printf("[Scenario] waiting for members to join (originator fan-out)...\n");

            // Wait for all members to join (max 45s — 40명 affiliation+자동초대 여유)
            bool bAllJoined = false;
            for (int retry = 0; retry < 450; ++retry) {
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

            if (bAllJoined && g_bPreempt) {
                // Floor 선점(preemption) 검증: holder 점유 중 preemptor 가 요청 → CMP REVOKE+GRANT.
                //   preemptor=session[g_iPreemptBy](기본0). chair 검증 시 chair 멤버의 세션 인덱스 지정.
                int pi = g_iPreemptBy;
                if (pi < 0 || pi >= (int)sessions.size()) pi = 0;
                int hi = (pi == 1) ? 2 : 1;
                if (hi >= (int)sessions.size()) hi = 0;
                printf("[Scenario] === Floor 선점 검증: holder=session[%d](%s) <- preemptor=session[%d](%s) ===\n",
                       hi, sessions[hi]->m_strUser.c_str(), pi, sessions[pi]->m_strUser.c_str());
                printf("[Scenario] holder(%s) PTT Request — floor 점유\n", sessions[hi]->m_strUser.c_str());
                sessions[hi]->SendPttRequest();
                for (int t = 0; t < 25 && !g_bQuit; ++t) usleep(100000);  // 2.5s 점유
                printf("[Scenario] preemptor(%s) PTT Request — 선점 시도\n", sessions[pi]->m_strUser.c_str());
                sessions[pi]->SendPttRequest();
                for (int t = 0; t < 30 && !g_bQuit; ++t) usleep(100000);  // 3s
                printf("[Scenario] preemptor(%s) PTT Release\n", sessions[pi]->m_strUser.c_str());
                sessions[pi]->SendPttRelease();
                for (int t = 0; t < 10 && !g_bQuit; ++t) usleep(100000);
                sessions[hi]->SendPttRelease();  // holder 정리 (이미 REVOKE 됐을 수 있음)
                printf("[Scenario] 선점 검증 시퀀스 완료 — floor.jsonl 의 REVOKE/preempt 확인\n");
            } else if (bAllJoined) {
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
                        sessions[i]->m_clsRtpThread.m_iLastFloorOp.store(0);
                        sessions[i]->m_clsRtpThread.m_bGrantReceived.store(false);
                        sessions[i]->SendPttRequest();

                        // GRANT 대기 (최대 3초)
                        // m_bGrantReceived 사용: GRANT(2) 직후 TAKEN(6) broadcast가 와서
                        // m_iLastFloorOp을 덮어써도 GRANT 수신 사실을 잃지 않는다.
                        bool bGranted = false;
                        for (int t = 0; t < 30 && !g_bQuit; ++t) {
                            if (sessions[i]->m_clsRtpThread.m_bGrantReceived.load()) {
                                bGranted = true;
                                break;
                            }
                            usleep(100000);
                        }
                        if (!bGranted) {
                            printf("[Scenario]   GRANT timeout — skipping (DENY/QUEUE?)\n");
                            continue;
                        }

                        // Speaking time: -floor_hold 초 (참여자별 발언 시간, default 5)
                        printf("[Scenario]   GRANT received, speaking %ds...\n", iFloorHold);
                        for (int t = 0; t < iFloorHold * 10 && !g_bQuit; ++t) usleep(100000);

                        printf("[Scenario] Member %d (%s): PTT Release\n",
                               i, sessions[i]->m_strUser.c_str());
                        sessions[i]->SendPttRelease();

                        // IDLE 대기 (최대 2초) — 다음 멤버 REQUEST 전 floor 정리 확인
                        for (int t = 0; t < 20 && !g_bQuit; ++t) {
                            if (sessions[i]->m_clsRtpThread.m_iLastFloorOp.load() == 5) break;
                            usleep(100000);
                        }

                        // 화자 전환 간격 0.3s
                        for (int t = 0; t < 3 && !g_bQuit; ++t) usleep(100000);
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
    std::string strHa1        = GetArg(argc, argv, "-ha1",        "");
    std::string strCredsFile  = GetArg(argc, argv, "-creds",      "");
    // IMS AKA 소프트-USIM (sip_access_security.md §8.2) — -creds 의 k/opc/sqn
    // 이 우선한다.
    std::string strAkaK = GetArg(argc, argv, "-aka_k", "");
    std::string strAkaOpc = GetArg(argc, argv, "-aka_opc", "");
    unsigned long long ullAkaSqn =
        strtoull(GetArg(argc, argv, "-aka_sqn", "0").c_str(), NULL, 10);
    std::string strMode       = GetArg(argc, argv, "-mode",       "volte");
    // 시그널링 transport — udp(기본)|tcp|tls. TLS 는 스택을 TLS 클라이언트로 기동한다.
    std::string strTransport  = GetArg(argc, argv, "-transport",  "udp");
    // RFC 3329 sec-agree 협상 (sip_access_security.md P2). -sec_verify <값> 은
    // Security-Verify 를
    //   서버 목록 echo 대신 그 값으로 보내 강등 변조를 재현한다(서버 494
    //   확인용).
    bool bSecAgree = HasFlag(argc, argv, "-sec_agree");
    std::string strSecVerify = GetArg(argc, argv, "-sec_verify", "");
    // IMS AKA + IPsec (sip_access_security.md §8.3) — Security-Client 에 ipsec-3gpp 만
    //   제안하고 401 의 서버 파라미터 + AKA CK/IK 로 커널 SA 4개를 설치한다. CAP_NET_ADMIN 필요.
    bool bIpsec = HasFlag(argc, argv, "-ipsec");
    std::string strIpsecAlg = GetArg(argc, argv, "-ipsec_alg", "hmac-sha-1-96");
    std::string strIpsecEalg = GetArg(argc, argv, "-ipsec_ealg", "aes-cbc");
    // 미디어 SRTP (SDES — media_security.md §8.3): off(기본)|optional|required.
    //   optional=AVP+a=crypto best-effort 오퍼, required=RTP/SAVP 오퍼(상대 무crypto 시 실패).
    //   -sec_agree 동반 시 REGISTER Security-Client 에 sdes-srtp;mediasec 능력을 선언한다(§4.1).
    std::string strSrtp = GetArg(argc, argv, "-srtp", "off");
    int iSrtpMode = (strSrtp == "required") ? 2 : (strSrtp == "optional" ? 1 : 0);
    if (strSrtp != "off" && strSrtp != "optional" && strSrtp != "required") {
        printf("invalid -srtp '%s' (off|optional|required) — using off\n", strSrtp.c_str());
    }
    std::string strGroupId    = GetArg(argc, argv, "-group",      "1000");
    std::string strScenario   = GetArg(argc, argv, "-scenario",   "");
    int iCallDuration          = atoi(GetArg(argc, argv, "-call_duration", "10").c_str());
    g_iRegisterHoldSec         = atoi(GetArg(argc, argv, "-hold", "0").c_str());
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
    // floor 선점(preemption) 검증: holder 점유 중 preemptor 요청 → CMP REVOKE+GRANT 확인.
    //   -preempt_by N = 선점자 세션 인덱스(default 0). chair 검증 시 chair 멤버 세션 인덱스 지정.
    g_bPreempt                 = HasFlag(argc, argv, "-preempt");
    g_strXcapRoot              = GetArg(argc, argv, "-xcap_root", "");
    g_iPreemptBy               = atoi(GetArg(argc, argv, "-preempt_by", "0").c_str());
    // G8 (2026-04-23): 외부 peer routing 시험용. 비우면 기존 pair 로직 유지.
    //   값이 있으면 call scenario 의 모든 outbound INVITE target 을 이 user 로 덮음.
    std::string strCalleeOverride = GetArg(argc, argv, "-callee_override", "");
    std::string strMediaFile  = GetArg(argc, argv, "-media_file",  "");
    std::string strMediaDir   = GetArg(argc, argv, "-media_dir",   "");
    std::string strVideoFile  = GetArg(argc, argv, "-video_file",  "");
    bool bNoVideo              = HasFlag(argc, argv, "-no_video");
    // -creds 의 user 순서를 전개 로스터로(번호 비연속 창 허용, -user 무시) — verify 하네스 전개 방식.
    bool bUsersFromCreds       = HasFlag(argc, argv, "-users_from_creds");
    int iIntervalMs            = atoi(GetArg(argc, argv, "-interval",    "100").c_str());
    std::string strDbConfig   = GetArg(argc, argv, "-db",            "");
    int iDbOffset             = atoi(GetArg(argc, argv, "-db_offset",    "0").c_str());
    bool bVerbose              = HasFlag(argc, argv, "-verbose");
    bool bPttMode              = (strMode == "ptt");
    // 외부 SIP peer 모드: REGISTER 송신 skip. INVITE 수신 시 SimSession 기본 핸들러가
    // 180→200 OK 자동 응답. IBCF trunk 시나리오의 mock 외부 peer 또는 register 가
    // 거부되는 ISP(IBCF role only)로 직접 INVITE 발신할 때 사용.
    bool bNoRegister           = HasFlag(argc, argv, "-no_register");
    // XCAP HTTP GET 비활성화 (Phase 3D). 기본은 xcap-diff NOTIFY 수신 시 자동 GET.
    bool bNoXcap               = HasFlag(argc, argv, "-no_xcap");
    g_bNoConfSub               = HasFlag(argc, argv, "-no_conf_sub");
    g_strPickupCode            = GetArg(argc, argv, "-pickup_code", "**");
    // CSC 연동: REGISTER 전 IdMS auth 수행 (올바른 순서)
    std::string strCscIp       = GetArg(argc, argv, "-csc_ip",   "");
    int iCscPort               = atoi(GetArg(argc, argv, "-csc_port", "4530").c_str());
    bool bCscTls               = HasFlag(argc, argv, "-csc_tls");

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
            // -db_offset: 로드된 가입자 앞쪽 N명 건너뜀 (여러 인스턴스에 disjoint 풀 할당 — 중복 등록 회피)
            if (iDbOffset > 0 && iDbOffset < (int)vecDbSubs.size()) {
                vecDbSubs.erase(vecDbSubs.begin(), vecDbSubs.begin() + iDbOffset);
                printf("[DB] db_offset=%d 적용 → %d명 사용\n", iDbOffset, (int)vecDbSubs.size());
            }
            if (iCount <= 1 && !HasFlag(argc, argv, "-count")) {
                iCount = (int)vecDbSubs.size();
            }
            // 채널 정책 사전 경고 — CSP 게이트(sip_access_security.md §3)는 정책 위반 REGISTER 를
            //   인증보다 먼저 403 처리하므로, 해당 가입자는 조용히 미등록 상태로 남아 시나리오의
            //   등록 대기(30s)만 꽉 채운다. 시험 전에 원인을 드러낸다.
            {
                std::string strCliTransport = strTransport;
                for (auto& c : strCliTransport) c = toupper(c);
                int iUsed = iCount < (int)vecDbSubs.size() ? iCount : (int)vecDbSubs.size();
                for (int k = 0; k < iUsed; k++) {
                    const DbSubscriber& sub = vecDbSubs[k];
                    if (!sub.sipTransport.empty() && sub.sipTransport != strCliTransport)
                        printf("[DB] ⚠ %s: sip_transport=%s 정책 vs CLI -transport %s — 채널 정책 게이트가 REGISTER 를 403 처리 (미등록 예상)\n",
                               sub.id.c_str(), sub.sipTransport.c_str(), strCliTransport.c_str());
                    if (sub.authScheme == "aka" && strAkaK.empty())
                        printf("[DB] ⚠ %s: auth_scheme=aka 인데 -aka_k 미지정 — Digest 등록이 거부된다 (미등록 예상)\n",
                               sub.id.c_str());
                }
            }
        } else {
            // -db 명시 요청의 조용한 fallback 금지 (-creds 선검증과 동일 원칙) —
            //   빈 결과로 합성 계정(-user)에 넘어가면 미가입 REGISTER 가 침묵 속에
            //   반복돼 "송신 전 hang" 으로 오인된다. ptt 는 -group 필터(기본 1000)가
            //   DB 에 없는 그룹이면 0명이 되는 함정이 대표 사례.
            printf("[DB] 가입자 로드 실패/0명 — 중단 (mode=%s", strMode.c_str());
            if (bPttMode) printf(", group=%s — ptt 는 그룹 멤버만 로드, -group 확인", strGroupId.c_str());
            printf(")\n");
            return 1;
        }
    }

    // 단말별 자격 파일 (-creds) — 전개될 user 전원이 파일에 있는지 세션 기동 전 일괄 선검증.
    //   조용한 fallback 은 "같은 비밀번호 구간" 함정을 침묵 속에 되살리므로 누락은 즉시 중단.
    std::map<std::string, UserCred> mapCreds;
    std::vector<std::string> vecCredsOrder;   // 파일 순서(-users_from_creds 로스터)
    if (bUsersFromCreds && (strCredsFile.empty() || bDbMode)) {
        printf("[CREDS] -users_from_creds 는 -no-db -creds <file> 와 함께 써야 함 — 중단\n");
        return 1;
    }
    if (!strCredsFile.empty()) {
        if (!LoadCredsFile(strCredsFile, mapCreds, vecCredsOrder)) return 1;
        if (bUsersFromCreds && iCount > (int)vecCredsOrder.size()) {
            printf("[CREDS] -count %d > 자격 파일 %d명 — 파일 인원으로 축소\n", iCount, (int)vecCredsOrder.size());
            iCount = (int)vecCredsOrder.size();
        }
        for (int i = 0; i < iCount; i++) {
            std::string u = bUsersFromCreds ? vecCredsOrder[i]
                          : (bDbMode && i < (int)vecDbSubs.size())
                          ? vecDbSubs[i].id : MakeUserId(strStartUser, i);
            if (mapCreds.find(u) == mapCreds.end()) {
                printf("[CREDS] 자격 파일에 user=%s 항목이 없음 — 중단\n", u.c_str());
                return 1;
            }
        }
        printf("[CREDS] 자격 파일 로드: %s (%d항목, 전개 %d명 커버)\n",
               strCredsFile.c_str(), (int)mapCreds.size(), iCount);
    }

    // 시나리오 선택
    ESimScenario eScenario = E_SCENARIO_NONE;
    if      (strScenario == "register")   eScenario = E_SCENARIO_REGISTER;
    else if (strScenario == "subscribe")  eScenario = E_SCENARIO_SUBSCRIBE;
    else if (strScenario == "call")       eScenario = E_SCENARIO_CALL;
    else if (strScenario == "group-call" || strScenario == "group_call") eScenario = E_SCENARIO_GROUP_CALL;
    else if (strScenario == "full")       eScenario = E_SCENARIO_FULL;
    else if (strScenario == "transfer" || strScenario == "transfer_blind") eScenario = E_SCENARIO_TRANSFER;
    else if (strScenario == "transfer_attended" || strScenario == "attended") eScenario = E_SCENARIO_TRANSFER_ATTENDED;
    else if (strScenario == "pickup")     eScenario = E_SCENARIO_PICKUP;
    else if (strScenario == "dialog_pickup" || strScenario == "blf") eScenario = E_SCENARIO_DIALOG_PICKUP;

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

        std::string strUser, strAuthId, strPwd, strSesHa1;
        if (bDbMode && i < (int)vecDbSubs.size()) {
            // DB 모드: v3 — authId 는 DB 의 imsi + CLI -domain 으로 조립
            strUser   = vecDbSubs[i].id;
            if (!vecDbSubs[i].authId.empty() && vecDbSubs[i].authId.find('@') == std::string::npos) {
                strAuthId = vecDbSubs[i].authId + "@" + strDomain;
            } else {
                strAuthId = vecDbSubs[i].authId;  // 이미 @ 포함(레거시)이거나 빈 값
            }
            strPwd    = vecDbSubs[i].password;
            strSesHa1 = vecDbSubs[i].ha1;
        } else {
            strUser = bUsersFromCreds ? vecCredsOrder[i] : MakeUserId(strStartUser, i);
            strPwd  = strPassword;
            strSesHa1 = strHa1;
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

        // 단말별 자격 파일 override — 전원 커버는 위에서 선검증됨.
        std::string strSesAkaK = strAkaK, strSesAkaOpc = strAkaOpc;
        unsigned long long ullSesAkaSqn = ullAkaSqn;
        if (!mapCreds.empty()) {
            const UserCred& cred = mapCreds[strUser];
            if (!cred.authId.empty()) {
                strAuthId = (cred.authId.find('@') == std::string::npos)
                          ? cred.authId + "@" + strDomain : cred.authId;
            }
            if (!cred.password.empty()) strPwd = cred.password;
            if (!cred.ha1.empty())      strSesHa1 = cred.ha1;
            if (!cred.k.empty()) {
              strSesAkaK = cred.k;
              strSesAkaOpc = cred.opc;
            }
            if (!cred.sqn.empty())
              ullSesAkaSqn = strtoull(cred.sqn.c_str(), NULL, 10);
        }

        SimSession* s = new SimSession(
            i,
            strUser,
            strAuthId,
            strDomain,
            strPwd,
            strSesHa1,
            strServerIp, iServerPort,
            strLocalIp,  iLocalPort,
            bPttMode,
            strGroupId
        );
        if (!mapCreds.empty()) {
            const UserCred& cred = mapCreds[strUser];
            if (!cred.login.empty()) s->SetIdmsLogin(cred.login, cred.loginPw);
        }
        if (strTransport == "tls" || strTransport == "TLS") {
            s->SetTransport(E_SIP_TLS);
        } else if (strTransport == "tcp" || strTransport == "TCP") {
            s->SetTransport(E_SIP_TCP);
        }
        if (bSecAgree || bIpsec)
          s->SetSecAgree(true, strSecVerify);
        if (bIpsec)
          s->SetIpsec(true, strIpsecAlg, strIpsecEalg);
        s->SetSrtpMode(iSrtpMode);  // SetSecAgree 이후 — mediasec 능력 선언 결합(§4.1)
        if (!strSesAkaK.empty())
          s->SetAka(strSesAkaK, strSesAkaOpc, (uint64_t)ullSesAkaSqn);
        s->SetNoRegister(bNoRegister);
        s->SetNoXcap(bNoXcap);
        if (!strCscIp.empty()) s->SetCscHost(strCscIp, iCscPort, bCscTls);

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

    // MCPTT 긴급/임박 개시 (TS 24.379): 발신자(session[0]) 의 그룹 INVITE 에 emergency-ind 주입.
    //   -emergency → emergency, -imminent → imminent peril. (둘 다 없으면 일반.)
    {
        int iEmergCond = HasFlag(argc, argv, "-emergency") ? 2
                       : (HasFlag(argc, argv, "-imminent") ? 1 : 0);
        if (iEmergCond > 0 && !sessions.empty()) {
            sessions[0]->SetEmergency(iEmergCond);
            printf("[MCPTT] session[0] 긴급개시 모드: %s\n", iEmergCond >= 2 ? "emergency" : "imminent");
        }
        // ad hoc (Rel-18): -adhoc → session[0] 이 나머지 세션 사용자를 동적 멤버로 그룹 개시
        if (HasFlag(argc, argv, "-adhoc") && sessions.size() > 1) {
            std::vector<std::string> mem;
            for (size_t j = 1; j < sessions.size(); ++j) mem.push_back(sessions[j]->m_strUser);
            sessions[0]->SetAdhocMembers(mem);
            printf("[MCPTT] session[0] ad-hoc 개시: %zu 동적 멤버\n", mem.size());
        }
    }

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

        // 표준 로그아웃: de-affiliate PUBLISH + SUBSCRIBE Expires=0 (gms/cms)
        for (auto* s : sessions) s->Logout();
        // SUBSCRIBE Expires=0 전송 완료 대기
        usleep(500000);

        // SIP 스택 종료 (UA.Stop() 내부에서 REGISTER Expires=0 전송)
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
