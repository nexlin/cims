// cimsue-cli — libcimsue 위의 헤드리스 UE (ue_sdk.md §4.7·§9)
//
// 실제 단말 스택(pjsua2 + 코어)으로 등록·1:1 호·MCPTT 그룹콜(floor)·MCData SDS·관제(dialog 감시·Join 청취·픽업·전달)
// 를 구동해 S3 검증 축을 제공한다. cspsim(시뮬레이터)과 달리 코덱·지터버퍼·SRTP·TLS·floor participant 를 단말과
// 같은 경로로 처리한다.
//
//   cimsue-cli [계정 옵션] register [--hold S]
//   cimsue-cli [계정 옵션] call <번호|sip:URI> [--duration S] [--video]
//   cimsue-cli [계정 옵션] answer [--duration S] [--transfer-to X --transfer-after S]
//   cimsue-cli [계정 옵션] group-call <groupId> [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency] [--broadcast] [--implicit]
//              (MCPTT 그룹콜은 음성만 — 그룹 영상은 video-call(MCVideo 호, mcvideo.md §8))
//              (--broadcast = 일제 통화 개시 — 발언을 놓은 뒤 서버 Floor Idle(B-bit)이면 코어가 호를 해제, outcome 에 broadcast_released)
//              (--implicit = 개시 INVITE 가 암묵적 발언 요청 — mc_implicit_request+mc_granted, TS 24.380 §14.2.4·§14.2.5. --ptt-at 0 과 함께)
//              [--upgrade-at S] [--cancel-at S]  (진행 중 긴급 상향·하향 re-INVITE, TS 24.379 §10.1.1.2.1.3·§10.1.1.2.1.4 — outcome 에 conditions)
//   cimsue-cli [계정 옵션] video-call <groupId> [--prearranged] [--queueing] [--priority N] [--implicit] [--rejoin SESSION_URI]
//              [--transmit-at S --transmit-len S] [--accept] [--duration S]
//              (MCVideo 그룹 호 — TS 24.281 §9.2.1 prearranged · §9.2.2 chat, 전송 제어 TS 24.581. --transmit-at = 그 시각에 [영상 보내기]
//               (Transmission Request) → --transmit-len 뒤 [보내기 끝], --accept = 알림 온 송출마다 [받기](Receive Media Request),
//               --implicit = 개시 INVITE 가 암묵적 송출 요청. 계정 옵션 --mcvideo · --mcvideo-psi 필요. chat 합류는 곧 affiliation(§8.1)이고,
//               prearranged 팬아웃을 받을 멤버는 계정 옵션 --affiliate-mcvideo G 로 명시 affiliation(§8.2 PUBLISH — MCVideo 제휴를 가르는
//               CSP(A9) 가 배포된 서버에만))
//   cimsue-cli [계정 옵션] video-answer [--transmit-at S --transmit-len S] [--accept] [--duration S]
//              (제어 기능의 MCVideo 멤버 초대(§9.2.1.3)를 기다린다 — 코어가 자동 수락(autoAnswerMcvideo), 뒤는 video-call 과 같다)
//   cimsue-cli [계정 옵션] alert <groupId> [--cancel] [--originated-by ID] [--cancel-group-emergency]   (긴급 경보 MESSAGE, §12.1.1.1·§12.1.1.2)
//   cimsue-cli [계정 옵션] sds <groupId> <text> [--wait-disposition S]   (MESSAGE 최종 응답까지 대기 — --cplane-max N 을 넘으면 MSRP, 결과
//              plane=media. --wait-disposition = 그 메시지의 전달 확인 통지(TS 24.282 §12.2.1.2)를 S 초까지 기다린다)
//   cimsue-cli [계정 옵션] sds-recv [--duration S] [--notify-delivered]   (수신 SDS 를 JSON 줄로 출력 — --notify-delivered = 전달 확인을
//              요청한 SDS 에 DELIVERED 통지, §12.2.1.1. 계정에 MCData PSI 가 있으면 규격형)
//   cimsue-cli [계정 옵션] dialog-watch <aor> [--duration S]      (RFC 4235 NOTIFY 를 JSON 줄로)
//   cimsue-cli [계정 옵션] join <aor> [--duration S]              (감시 → confirmed dialog 에 INVITE-Join recvonly)
//   cimsue-cli [계정 옵션] pickup [number] --code <피처코드> [--duration S]
//   cimsue-cli [계정 옵션] transfer <peer> --to <target> [--transfer-after S]   (peer 와 통화 후 REFER)
//   cimsue-cli --csc-host H [--csc-port 4430] --user U --pw P [--csc-ca FILE|--no-tls-verify] login
//   cimsue-cli [계정 옵션] drive                          (구동 모드 — stdin 명령 / stdout JSON 이벤트, 계측기 real-ue 풀이 쓴다)
//   cimsue-cli [계정 옵션] link HOST[:PORT] [--pair-key K] [--link-ca PEM|--link-pin FILE] [--sample-file WAV]   (계측 링크 — device 풀)
//   (계정 옵션 대신 --from-profile volte|ptt 로 프로비저닝 프로파일에서 계정을 채울 수 있다)
//
// 구동 모드(drive): 엔진을 띄운 채 stdin 에서 한 줄 = 명령 하나를 읽고, 진행은 stdout 에 한 줄 = JSON 이벤트 하나로 낸다
//   (test_instrument.md §3.3 — 워커가 프로세스를 가상 단말처럼 단계별로 구동한다). 등록은 자동으로 하지 않는다 — `register` 명령이 한다.
//   명령·이벤트 정의는 코어 cimsue/drive.h(DriveSession) 하나다 — 계측 링크(link)와 같다.
// 계측 링크(link HOST[:PORT]): 등록한 뒤 계측기 워커(Device.Port)에 TLS 로 먼저 붙어 hello 를 보내고, 워커가 보낸 명령을 같은
//   DriveSession 으로 실행한다(ue_voice_quality.md §5 — 앱 시험 모드와 같은 경로, 등록은 cli 소유라 register 명령은 app_owned 로 거절).
//   stdout = link{state,detail} 줄. --duration 이 없으면 SIGINT/SIGTERM 까지.
//
// 계정 옵션: --server IP --port N --transport udp|tcp|tls --domain D --msisdn M (--imsi I | --auth-id IMPI)
//           (--ha1 HEX32 | --password P) [--mcptt-id tel:..] [--affiliate G[,G2]] [--srtp off|optional|required]
//           [--sec tls] [--tls-ca FILE] [--no-tls-verify] [--display-name NAME] [--log-level N] [--timeout S] [--json]
//           [--cplane-max N] (그룹 SDS 시그널링 평면 상한 — 넘으면 MSRP, TS 24.282 §9.2.3) [--msrp] (서버발 MSRP 배포 수신 광고)
//           [--mcptt-psi URI] [--mcdata-psi URI] (참여 기능 PSI — --from-profile 이면 ue-init-config(TS 24.484 §7.2)에서 채우고 명시값이 덮는다)
//           [--instance-id URN] (+sip.instance · ue-init-config 의 MCS UE ID)
//           [--mcvideo] (REGISTER 에 MCVideo 태그 — TS 24.281 §7.2.1AA) [--mcvideo-psi URI] (참여 MCVideo 기능 PSI — --from-profile ptt 면
//           ue-init-config 의 MCVideo-Service-Details 에서 채운다) [--affiliate-mcvideo G[,G2]] (MCVideo affiliation — TS 24.281 §8.2)
// 종료 코드: 0 성공 / 2 인자 / 3 등록·로그인 실패 / 4 호 실패·시한 / 5 미디어 없음 / 6 floor·송출 미획득 / 7 SDS 실패 / 8 관제 실패
#include <chrono>
#include <csignal>
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
    std::vector<std::string> affiliateMcvideo;                          // --affiliate-mcvideo — MCVideo 서비스 affiliation(TS 24.281 §8.2)
    int durationSec = 8;
    int holdSec = 0;
    bool video = false;
    int pttAt = -1;
    int pttLen = 3;
    bool listenOnly = false;
    bool emergency = false;
    bool broadcast = false;           // 일제 통화 개시(TS 24.379 §4.12)
    bool implicit = false;            // 암묵적 발언 요청(TS 24.380 §14.2.5) · MCVideo 암묵적 송출 요청(TS 24.581 §14.2.5)
    // MCVideo 그룹 호(video-call · video-answer)
    bool prearranged = false, queueing = false, accept = false;
    int priority = -1;
    int transmitAt = -1, transmitLen = 3;
    std::string rejoinUri;            // 재합류 — 앞 호의 세션 식별자(TS 24.281 §9.2.1.2.4)
    int upgradeAt = -1, cancelAt = -1;  // 진행 중 긴급 상향·하향 시각(TS 24.379 §10.1.1.2.1.3·§10.1.1.2.1.4)
    bool alertCancel = false, cancelGroupEmergency = false;
    bool notifyDelivered = false;     // sds-recv — 전달 확인 요청에 DELIVERED 통지(TS 24.282 §12.2.1.1)
    int waitDispositionSec = 0;       // sds — 전달 확인 통지 대기
    std::string originatedBy;
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
    // 구동·계측 링크(ue_voice_quality.md §5)
    std::string service;              // 회선 서비스 이름(volte·voip·ptt) — 비면 --mcptt-id 유무로
    std::string sampleFile;           // `media sample` 기본 WAV
    std::string pairKey, linkCaFile, linkPinFile, deviceId;
    bool durationSet = false;
};

void usage() {
    std::fprintf(stderr,
        "usage: cimsue-cli [계정] <command> ...\n"
        "  계정: --server IP [--port N] [--transport udp|tcp|tls] --domain D --msisdn M (--imsi I | --auth-id IMPI)\n"
        "        (--ha1 HEX | --password P) [--mcptt-id tel:..] [--affiliate G,..] [--srtp off|optional|required] [--sec tls]\n"
        "        [--tls-ca FILE] [--no-tls-verify] [--display-name N] [--log-level N] [--timeout S] [--json]\n"
        "        [--cplane-max N] [--msrp]   (MCData media plane — 큰 그룹 SDS 발신·서버발 배포 수신)\n"
        "        [--mcptt-psi URI]   (참여 기능 PSI — 긴급 경보 Request-URI, TS 24.379 §12.1.1.1 8))\n"
        "        [--mcdata-psi URI]  (참여 MCData 기능 PSI — disposition 통지 Request-URI, TS 24.282 §12.2.1.1)\n"
        "        [--instance-id URN] (+sip.instance · ue-init-config 의 MCS UE ID. --from-profile ptt 면 ue-init-config 로 PSI 를 채운다)\n"
        "        [--mcvideo] [--mcvideo-psi URI]   (MCVideo 등록 태그 · 참여 MCVideo 기능 PSI — TS 24.281 §7.2.1AA·§9.2.1.2.1.1)\n"
        "        [--mcvideo-service-settings]   (등록 뒤 MCVideo 서비스 설정 PUBLISH — Event: poc-settings, TS 24.281 §7.2.3)\n"
        "        [--affiliate-mcvideo G,..]   (MCVideo affiliation — 관심 그룹 전부를 한 PUBLISH 로, TS 24.281 §8.2.1.2)\n"
        "        또는 --csc-host H [--csc-port N] --user U (--pw P | --pw-env VAR) [--csc-ca FILE] --from-profile volte|ptt\n"
        "  register [--hold S] | call TARGET [--duration S] [--video] | answer [--duration S] [--transfer-to X]\n"
        "  group-call GROUP [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency] [--broadcast] [--implicit]\n"
        "             [--upgrade-at S] [--cancel-at S]\n"
        "  video-call GROUP [--prearranged] [--queueing] [--priority N] [--implicit] [--rejoin URI] [--transmit-at S --transmit-len S]\n"
        "             [--accept] [--duration S]   (MCVideo 그룹 호 — 계정 --mcvideo --mcvideo-psi URI)\n"
        "  video-answer [--transmit-at S --transmit-len S] [--accept] [--duration S]   (MCVideo 멤버 초대 대기 — 코어가 자동 수락)\n"
        "  alert GROUP [--cancel] [--originated-by ID] [--cancel-group-emergency]\n"
        "  sds GROUP TEXT [--wait-disposition S] | sds-recv [--duration S] [--notify-delivered] | login\n"
        "  dialog-watch AOR [--duration S] | join AOR [--duration S] | pickup [NUMBER] --code CODE | transfer PEER --to X\n"
        "  drive [--sample-file WAV] [--service volte|voip|ptt]   (구동 모드 — stdin 명령 / stdout JSON 이벤트; cimsue/drive.h 명령표)\n"
        "  link HOST[:PORT] [--pair-key K] [--link-ca PEM | --link-pin FILE] [--sample-file WAV] [--service S] [--duration S]\n"
        "          (계측 링크 — 등록 뒤 계측기 워커에 TLS 로 붙어 워커 명령을 실행, 앱 시험 모드와 같은 경로)\n"
        "  groups | group-get URI | group-info URI(멤버 제외) | group-put URI --name N [--members tel:..,tel:..] | group-delete URI   (--csc-host --user --pw)\n");
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
            if (opt("--affiliate-mcvideo", [&](const std::string& v) { std::stringstream ss(v); std::string g; while (std::getline(ss, g, ',')) if (!g.empty()) o.affiliateMcvideo.push_back(g); })) continue;
            if (opt("--srtp", [&](const std::string& v) { o.acc.mediaSecurity = v == "required" ? MediaSecurity::Required : v == "optional" ? MediaSecurity::Optional : MediaSecurity::Off; })) continue;
            if (opt("--sec", [&](const std::string& v) { std::stringstream ss(v); std::string m; while (std::getline(ss, m, ',')) if (!m.empty()) o.acc.secMechanisms.push_back(m); })) continue;
            if (opt("--tls-ca", [&](const std::string& v) { o.tlsCaFile = v; })) continue;
            if (opt("--log-level", [&](const std::string& v) { o.logLevel = std::atoi(v.c_str()); })) continue;
            if (opt("--timeout", [&](const std::string& v) { o.timeoutSec = std::atoi(v.c_str()); })) continue;
            if (opt("--duration", [&](const std::string& v) { o.durationSec = std::atoi(v.c_str()); o.durationSet = true; })) continue;
            if (opt("--service", [&](const std::string& v) { o.service = v; })) continue;
            if (opt("--sample-file", [&](const std::string& v) { o.sampleFile = v; })) continue;
            if (opt("--pair-key", [&](const std::string& v) { o.pairKey = v; })) continue;
            if (opt("--link-ca", [&](const std::string& v) { o.linkCaFile = v; })) continue;
            if (opt("--link-pin", [&](const std::string& v) { o.linkPinFile = v; })) continue;
            if (opt("--device-id", [&](const std::string& v) { o.deviceId = v; })) continue;
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
            // 비밀을 명령행(프로세스 목록)에 두지 않는 경로 — 값은 환경변수에서(계측기 비밀 규약 *_env 와 같은 방식)
            if (opt("--pw-env", [&](const std::string& v) { const char* e = std::getenv(v.c_str()); o.pw = e ? e : ""; })) continue;
            if (opt("--csc-ca", [&](const std::string& v) { o.cscCaFile = v; })) continue;
            if (opt("--from-profile", [&](const std::string& v) { o.fromProfile = v; })) continue;
            if (opt("--name", [&](const std::string& v) { o.groupName = v; })) continue;
            if (opt("--upgrade-at", [&](const std::string& v) { o.upgradeAt = std::stoi(v); })) continue;
            if (opt("--cplane-max", [&](const std::string& v) { o.acc.maxSdsCplaneBytes = std::stoi(v); })) continue;
            if (opt("--mcptt-psi", [&](const std::string& v) { o.acc.mcpttServerUri = v; })) continue;
            if (opt("--mcdata-psi", [&](const std::string& v) { o.acc.mcdataServerUri = v; })) continue;
            if (opt("--instance-id", [&](const std::string& v) { o.acc.instanceId = v; })) continue;
            if (opt("--wait-disposition", [&](const std::string& v) { o.waitDispositionSec = std::stoi(v); })) continue;
            if (opt("--cancel-at", [&](const std::string& v) { o.cancelAt = std::stoi(v); })) continue;
            if (opt("--originated-by", [&](const std::string& v) { o.originatedBy = v; })) continue;
            if (opt("--mcvideo-psi", [&](const std::string& v) { o.acc.mcvideoServerUri = v; o.acc.mcvideoEnabled = true; })) continue;
            if (opt("--priority", [&](const std::string& v) { o.priority = std::stoi(v); })) continue;
            if (opt("--transmit-at", [&](const std::string& v) { o.transmitAt = std::stoi(v); })) continue;
            if (opt("--transmit-len", [&](const std::string& v) { o.transmitLen = std::stoi(v); })) continue;
            if (opt("--rejoin", [&](const std::string& v) { o.rejoinUri = v; })) continue;
            if (opt("--members", [&](const std::string& v) { std::stringstream ss(v); std::string m; while (std::getline(ss, m, ',')) if (!m.empty()) o.groupMembers.push_back(m); })) continue;
        } catch (std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return false; }
        if (a == "--no-tls-verify") o.tlsVerify = false;
        else if (a == "--json") o.json = true;
        else if (a == "--video") o.video = true;
        else if (a == "--listen-only") o.listenOnly = true;
        else if (a == "--emergency") o.emergency = true;
        else if (a == "--broadcast") o.broadcast = true;
        else if (a == "--implicit") o.implicit = true;
        else if (a == "--cancel") o.alertCancel = true;
        else if (a == "--msrp") o.acc.mcdataMsrp = true;
        else if (a == "--notify-delivered") o.notifyDelivered = true;
        else if (a == "--cancel-group-emergency") o.cancelGroupEmergency = true;
        else if (a == "--mcvideo") o.acc.mcvideoEnabled = true;
        else if (a == "--mcvideo-service-settings") o.acc.mcvideoServiceSettings = true;   // 서비스 설정 PUBLISH(TS 24.281 §7.2.3)
        else if (a == "--prearranged") o.prearranged = true;
        else if (a == "--queueing") o.queueing = true;
        else if (a == "--accept") o.accept = true;
        else if (a == "-h" || a == "--help") return false;
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
        else pos.push_back(a);
    }
    if (pos.empty()) return false;
    o.cmd = pos[0];
    static const char* needTarget[] = {"call", "group-call", "video-call", "alert", "dialog-watch", "join", "transfer", "group-get", "group-info",
                                       "group-put", "group-delete", "link"};
    for (auto n : needTarget) if (o.cmd == n) { if (pos.size() < 2) return false; o.target = pos[1]; }
    if (o.cmd == "sds") { if (pos.size() < 3) return false; o.target = pos[1]; for (size_t i = 2; i < pos.size(); ++i) o.text += (i > 2 ? " " : "") + pos[i]; }
    if (o.cmd == "pickup") { if (pos.size() >= 2) o.target = pos[1]; if (o.code.empty()) return false; }
    if (o.cmd == "drive" || o.cmd == "link") o.json = true;   // 구동·링크 모드는 언제나 JSON 이벤트
    if (o.cmd == "transfer" && o.transferTo.empty()) return false;
    static const char* known[] = {"register", "call", "answer", "group-call", "video-call", "video-answer", "alert", "sds", "sds-recv", "login",
                                  "dialog-watch", "join", "pickup", "transfer", "groups", "group-get", "group-info", "group-put", "group-delete", "drive",
                                  "link"};
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
        std::fprintf(stderr, "[cimsue-cli] incoming call=%d from=%s called=%s video=%d mcptt=%d service=%s group=%s\n", c.callId,
                     c.remoteUri.c_str(), c.calledParty.c_str(), c.video, c.isMcptt, toString(c.service), c.groupId.c_str());
        set([&] { incoming = c; haveIncoming = true; calls[c.callId] = c; });
    }
    void onCallState(const CallInfo& c) override {
        std::fprintf(stderr, "[cimsue-cli] call=%d %s code=%d %s%s%s\n", c.callId, toString(c.state), c.lastCode, c.lastReason.c_str(),
                     c.answerState.empty() ? "" : " answer-state=", c.answerState.c_str());
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
    void onTransmission(const TransmissionEvent& ev) override {
        std::fprintf(stderr, "[cimsue-cli] transmission call=%d %s state=%s cause=%d(%s) dur=%d prio=%d queue=%d audio_ssrc=%u video_ssrc=%u\n",
                     ev.callId, toString(ev.kind), toString(ev.state), ev.cause, ev.causeText.c_str(), ev.durationSec, ev.priority,
                     ev.queuePosition, ev.audioSsrc, ev.videoSsrc);
        if (json_)
            std::printf("{\"event\":\"transmission\",\"call_id\":%d,\"kind\":\"%s\",\"state\":\"%s\",\"cause\":%d}\n", ev.callId,
                        toString(ev.kind), toString(ev.state), ev.cause);
        set([&] { transmissions.push_back(ev); });
    }
    void onReception(const ReceptionEvent& ev) override {
        std::fprintf(stderr, "[cimsue-cli] reception call=%d %s from=%s state=%s auto=%d cause=%d(%s) audio_ssrc=%u video_ssrc=%u\n", ev.callId,
                     toString(ev.kind), ev.transmitter.userId.c_str(), toString(ev.transmitter.state), ev.transmitter.automatic, ev.cause,
                     ev.causeText.c_str(), ev.transmitter.audioSsrc, ev.transmitter.videoSsrc);
        if (json_)
            std::printf("{\"event\":\"reception\",\"call_id\":%d,\"kind\":\"%s\",\"from\":\"%s\",\"state\":\"%s\",\"cause\":%d}\n",
                        ev.callId, toString(ev.kind), ev.transmitter.userId.c_str(), toString(ev.transmitter.state), ev.cause);
        set([&] { receptions.push_back(ev); });
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
        std::fprintf(stderr, "[cimsue-cli] sds from=%s group=%s msg=%s notif=%d/%d media=%d bytes=%zu text=%.80s\n", m.fromUri.c_str(),
                     m.groupUri.c_str(), m.msgId.c_str(), m.notification, m.notifType, m.mediaPlane, m.text.size(), m.text.c_str());
        if (json_)
            std::printf("{\"event\":\"sds\",\"from\":\"%s\",\"group\":\"%s\",\"conv_id\":\"%s\",\"msg_id\":\"%s\",\"notification\":%s,"
                        "\"notif_type\":%d,\"plane\":\"%s\",\"bytes\":%zu,\"text\":\"%s\"}\n", m.fromUri.c_str(), m.groupUri.c_str(),
                        m.convId.c_str(), m.msgId.c_str(), m.notification ? "true" : "false", m.notifType,
                        m.mediaPlane ? "media" : "signalling", m.text.size(), m.text.size() > 200 ? "(long)" : m.text.c_str());
        set([&] { sds.push_back(m); });
    }
    void onMcpttCondition(const CallInfo& c, ConditionCause cause) override {
        std::fprintf(stderr, "[cimsue-cli] condition call=%d %s emergency=%d imminent=%d mine=%d pending=%d code=%d\n", c.callId,
                     toString(cause), c.condition.emergency, c.condition.imminentPeril, c.condition.mine, c.condition.pending,
                     c.condition.lastCode);
        if (json_)
            std::printf("{\"event\":\"condition\",\"call_id\":%d,\"cause\":\"%s\",\"emergency\":%s,\"imminent_peril\":%s,\"code\":%d}\n",
                        c.callId, toString(cause), c.condition.emergency ? "true" : "false",
                        c.condition.imminentPeril ? "true" : "false", c.condition.lastCode);
        set([&] { conditions.emplace_back(c, cause); calls[c.callId] = c; });
    }
    void onNonAcknowledgedUsers(const CallInfo& c) override {
        std::string u;
        for (auto& x : c.nonAcknowledgedUsers) u += x + " ";
        std::fprintf(stderr, "[cimsue-cli] non-acknowledged call=%d users=[%s]\n", c.callId, u.c_str());
        if (json_) std::printf("{\"event\":\"non_acknowledged\",\"call_id\":%d,\"count\":%zu}\n", c.callId, c.nonAcknowledgedUsers.size());
        set([&] { calls[c.callId] = c; });
    }
    void onEmergencyAlert(const EmergencyAlert& a) override {
        std::fprintf(stderr, "[cimsue-cli] alert group=%s user=%s alert=%d emergency=%d imminent=%d originated-by=%s self=%d\n",
                     a.groupId.c_str(), a.userId.c_str(), a.alertInd, a.emergencyInd, a.imminentPerilInd, a.originatedBy.c_str(), a.self);
        if (json_)
            std::printf("{\"event\":\"alert\",\"group\":\"%s\",\"user\":\"%s\",\"alert\":%d,\"emergency\":%d,\"imminent_peril\":%d,"
                        "\"originated_by\":\"%s\"}\n", a.groupId.c_str(), a.userId.c_str(), a.alertInd, a.emergencyInd,
                        a.imminentPerilInd, a.originatedBy.c_str());
        set([&] { alerts.push_back(a); });
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
    std::vector<std::pair<CallInfo, ConditionCause>> conditions;
    std::vector<EmergencyAlert> alerts;
    std::vector<TransmissionEvent> transmissions;
    std::vector<ReceptionEvent> receptions;

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
/** 호 품질(ue_voice_quality.md §3 — Engine::callQuality) 필드 — 줄 프로토콜과 같은 정의(cimsue/drive.h). */
std::string qualityJson(const CallQuality& q) { return drive::qualityFields(q); }

/** 구동 모드 줄 출력 = stdout (줄 단위로 잠근다). */
class StdoutSink : public LineSink {
public:
    void writeLine(const std::string& line) override { outLine(line); }
};

/** 구동·링크 모드의 주 리스너 — 로그만 stderr 로. 프로토콜 이벤트는 DriveSession(관찰자)이 낸다. */
class LogListener : public Listener {
public:
    explicit LogListener(int logLevel) : logLevel_(logLevel) {}
    void onLog(int level, const std::string& msg) override { if (level <= logLevel_) std::fprintf(stderr, "%s\n", msg.c_str()); }
    void onRegState(const RegInfo& r) override {
        std::lock_guard<std::mutex> lk(m_); reg_ = r; cv_.notify_all();
    }
    bool waitRegistered(int timeoutSec) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(timeoutSec), [&] { return reg_.state == RegState::Registered || reg_.state == RegState::Failed; });
        return reg_.state == RegState::Registered;
    }
    RegInfo reg() { std::lock_guard<std::mutex> lk(m_); return reg_; }

private:
    int logLevel_;
    std::mutex m_;
    std::condition_variable cv_;
    RegInfo reg_;
};

std::string serviceOf(const Opts& o) {
    if (!o.service.empty()) return o.service;
    return o.acc.mcpttId.empty() ? "volte" : "ptt";
}

/** 구동 루프 — stdin 한 줄 = 명령 하나를 DriveSession 에 넘긴다(명령·이벤트 정의 = cimsue/drive.h). EOF 또는 quit 에 엔진을 내린다. */
int driveLoop(Engine& eng, int acc, const Opts& o) {
    StdoutSink sink;
    DriveOptions opt;
    opt.accounts.push_back({ serviceOf(o), acc, o.acc.aor(), o.acc.msisdn });
    opt.sampleFile = o.sampleFile;
    opt.hangupOnStop = DriveOptions::Hangup::All;
    DriveSession ds(eng, sink, opt);
    ds.start(true);
    std::string line;
    while (!ds.quitRequested() && std::getline(std::cin, line)) ds.handleLine(line);
    ds.stop();
    eng.unregisterAccount(acc);
    eng.stop();
    outLine("{\"event\":\"exit\"}");
    return 0;
}

std::atomic<bool> g_interrupted{false};

/** 링크 모드 — 앱과 같은 계측 링크(DeviceLink)로 계측기 워커에 붙는다(ue_voice_quality.md §5). 등록은 cli 가 먼저 해 둔다(앱 소유 규칙). */
class LinkPrinter : public DeviceLinkListener {
public:
    void onLinkState(LinkState st, const std::string& detail) override {
        const char* s = st == LinkState::Connecting ? "connecting" : st == LinkState::Connected ? "connected" : st == LinkState::Disconnected ? "disconnected"
                      : st == LinkState::Refused ? "refused" : "idle";
        outLine("{\"event\":\"link\",\"state\":\"" + std::string(s) + "\",\"detail\":\"" + drive::jsonEscape(detail) + "\"}");
        if (st == LinkState::Refused) refused = true;
    }
    std::atomic<bool> refused{false};
};

int linkLoop(Engine& eng, LogListener& ls, int acc, const Opts& o) {
    Result r = eng.registerAccount(acc);
    if (!r.ok || !ls.waitRegistered(o.timeoutSec)) {
        outLine("{\"event\":\"exit\",\"error\":\"register failed\",\"code\":" + std::to_string(ls.reg().code) + "}");
        eng.stop();
        return 3;
    }
    DeviceLinkConfig lc;
    std::string hp = o.target;
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos && hp.find(']') == std::string::npos) { lc.host = hp.substr(0, colon); lc.port = std::atoi(hp.c_str() + colon + 1); }
    else lc.host = hp;
    lc.pairKey = o.pairKey;
    lc.verifyServer = !o.linkCaFile.empty();
    if (lc.verifyServer) lc.caPem = readFile(o.linkCaFile);
    lc.pinFile = o.linkPinFile;
    lc.deviceId = o.deviceId.empty() ? "cimsue-cli:" + o.acc.msisdn : o.deviceId;
    lc.app = "cimsue-cli"; lc.appVersion = Engine::version(); lc.platform = "linux"; lc.model = "headless";
    DriveOptions opt;
    opt.accounts.push_back({ serviceOf(o), acc, o.acc.aor(), o.acc.msisdn });
    opt.sampleFile = o.sampleFile;
    LinkPrinter lp;
    DeviceLink link(eng);
    r = link.start(lc, opt, &lp);
    if (!r.ok) { outLine("{\"event\":\"exit\",\"error\":\"" + drive::jsonEscape(r.reason) + "\"}"); eng.stop(); return 2; }
    auto t0 = std::chrono::steady_clock::now();
    while (!g_interrupted && !lp.refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (o.durationSet && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(o.durationSec)) break;
    }
    link.stop();
    eng.unregisterAccount(acc);
    eng.stop();
    outLine("{\"event\":\"exit\"}");
    return lp.refused ? 3 : 0;
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
    const bool groupCmd = o.cmd == "groups" || o.cmd == "group-get" || o.cmd == "group-info" || o.cmd == "group-put" ||
                          o.cmd == "group-delete";
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
            } else if (o.cmd == "group-info") {                               // 멤버를 뺀 문서(TS 24.481 §6.3.16 — 기본 조회)
                GroupDoc d;
                r = csc.getGroupExcludingMembers(tok.accessToken, o.target, d);
                if (r.ok) printDoc("group-info", d);
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
        if (o.acc.maxSdsCplaneBytes > 0) a.maxSdsCplaneBytes = o.acc.maxSdsCplaneBytes;
        a.mcdataMsrp = o.acc.mcdataMsrp;
        a.instanceId = o.acc.instanceId;
        // 참여 기능 PSI = UE initial configuration(TS 24.484 §7.2.2.1 10)·14)) — 명시 인자가 문서를 덮는다
        if (sp->kind == "ptt") {
            CscEndpoint ep; ep.host = o.cscHost; ep.port = o.cscPort; ep.verifyServer = o.tlsVerify;
            if (!o.cscCaFile.empty()) ep.caPem = readFile(o.cscCaFile); else if (!o.tlsCaFile.empty()) ep.caPem = readFile(o.tlsCaFile);
            CscClient csc(ep);
            UeInitConfigDoc ui;
            // MCS UE ID = instance ID. 없으면 Nil UUID(RFC 4122 §4.1.7) — 이 CMS 는 모든 UE 에 같은 문서를 준다.
            Result ur = csc.fetchUeInitConfig(a.instanceId.empty() ? "urn:uuid:00000000-0000-0000-0000-000000000000" : a.instanceId, "", ui);
            if (ur.ok) {
                a.mcpttServerUri = ui.mcpttServerUri; a.mcdataServerUri = ui.mcdataServerUri; a.mcvideoServerUri = ui.mcvideoServerUri;
                a.floorTimers = ui.floorTimers;                   // 발언권 참여자 타이머(<Timers>, TS 24.484 §7.2.2.7)
            }
            else std::fprintf(stderr, "[cimsue-cli] ue-init-config: %s\n", ur.reason.c_str());
        }
        if (!o.acc.mcpttServerUri.empty()) a.mcpttServerUri = o.acc.mcpttServerUri;
        if (!o.acc.mcdataServerUri.empty()) a.mcdataServerUri = o.acc.mcdataServerUri;
        if (!o.acc.mcvideoServerUri.empty()) a.mcvideoServerUri = o.acc.mcvideoServerUri;
        a.mcvideoEnabled = o.acc.mcvideoEnabled;                          // MCVideo 등록은 명시할 때만(--mcvideo)
        a.mcvideoServiceSettings = o.acc.mcvideoServiceSettings;
        if (!a.mcpttServerUri.empty() || !a.mcdataServerUri.empty() || !a.mcvideoServerUri.empty())
            std::fprintf(stderr, "[cimsue-cli] psi mcptt=%s mcdata=%s mcvideo=%s\n", a.mcpttServerUri.c_str(), a.mcdataServerUri.c_str(),
                         a.mcvideoServerUri.c_str());
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

    if (o.cmd == "drive" || o.cmd == "link") {
        LogListener ll(o.logLevel);
        Engine eng;
        Result r = eng.start(ec, &ll);
        if (!r.ok) { outLine("{\"event\":\"exit\",\"error\":\"engine start failed: " + jsonEsc(r.reason) + "\"}"); return 3; }
        int acc = eng.addAccount(o.acc);
        if (acc < 0) { outLine("{\"event\":\"exit\",\"error\":\"addAccount failed\"}"); eng.stop(); return 3; }
        if (o.cmd == "link") {
            std::signal(SIGINT, [](int) { g_interrupted = true; });
            std::signal(SIGTERM, [](int) { g_interrupted = true; });
            return linkLoop(eng, ll, acc, o);
        }
        return driveLoop(eng, acc, o);
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

    // affiliation — MCPTT(--affiliate) · MCVideo(--affiliate-mcvideo, 관심 그룹 전부를 한 PUBLISH 로 — TS 24.281 §8.2.1.2)
    auto affiliateAll = [&](const std::vector<std::string>& groups, McService svc, const char* name) {
        for (auto& g : groups) {
            int64_t tok = eng.affiliate(acc, g, true, svc);
            bool got = ls.waitFor([&] { return ls.results.count(tok) > 0; }, 10);
            if (!got || ls.results[tok].code / 100 != 2)
                std::fprintf(stderr, "[cimsue-cli] affiliate(%s) %s failed (code=%d)\n", name, g.c_str(), got ? ls.results[tok].code : 0);
        }
    };
    affiliateAll(o.affiliate, McService::Mcptt, "mcptt");
    affiliateAll(o.affiliateMcvideo, McService::McVideo, "mcvideo");

    int rc = 0;
    auto disconnected = [&](int callId) { auto it = ls.calls.find(callId); return it != ls.calls.end() && it->second.state == CallState::Disconnected; };
    auto finish = [&](int callId) {
        if (callId >= 0) {
            s.st = eng.streamStats(callId);
            s.extra += qualityJson(eng.callQuality(callId));
            CallInfo ci = ls.calls.count(callId) ? ls.calls[callId] : CallInfo{};
            s.code = ci.lastCode; s.reason = ci.lastReason;
            if (ci.state != CallState::Disconnected) {
                eng.hangup(callId);
                ls.waitFor([&] { return disconnected(callId); }, 5);
            }
        }
        // affiliation 해제는 응답을 받은 뒤 등록을 해제한다 — 응답 전에 계정이 사라지면 늦은 응답이 무효 계정으로 올라온다
        //   (pjsua2 on_acc_send_request 의 계정 조회 assert)
        std::vector<int64_t> deaff;
        for (auto& g : o.affiliate) deaff.push_back(eng.affiliate(acc, g, false));
        for (auto& g : o.affiliateMcvideo) deaff.push_back(eng.affiliate(acc, g, false, McService::McVideo));
        ls.waitFor([&] { for (int64_t t : deaff) if (t >= 0 && !ls.results.count(t)) return false; return true; }, 5);
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
        GroupCallOptions go; go.listenOnly = o.listenOnly; go.emergency = o.emergency; go.broadcast = o.broadcast;
        go.implicitFloorRequest = o.implicit;             // --ptt-at 의 floorRequest 는 이미 요청 중이라 무시된다
        s.callId = eng.joinGroupCall(acc, o.target, go);
        if (s.callId < 0) { s.outcome = "invite_failed"; rc = 4; return finish(-1); }
        bool up = waitActive(ls, s.callId, o.timeoutSec);
        CallInfo ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) { s.outcome = up ? "call_failed" : "call_timeout"; rc = 4; return finish(s.callId); }
        auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count(); };
        bool pttDone = o.pttAt < 0;
        bool upDone = o.upgradeAt < 0, cancelDone = o.cancelAt < 0;
        bool gone = false;
        // 조건 re-INVITE 의 최종 결과(Confirmed·Denied)를 기다린다 — 서버 판정이 시험의 관측 대상이다.
        auto conditionStep = [&](bool emergency) {
            if (eng.callInfo(s.callId).condition.emergency == emergency) return;   // 바뀐 것 없음 — 코어가 보내지 않는다
            size_t before;
            { before = ls.conditions.size(); }
            r = eng.setCallCondition(s.callId, emergency, false);
            if (!r.ok) { std::fprintf(stderr, "[cimsue-cli] setCallCondition: %s\n", r.reason.c_str()); return; }
            ls.waitFor([&] {
                for (size_t k = before; k < ls.conditions.size(); ++k)
                    if (ls.conditions[k].second == ConditionCause::Confirmed || ls.conditions[k].second == ConditionCause::Denied) return true;
                return false;
            }, o.timeoutSec);
        };
        while (elapsed() < o.durationSec && !gone) {
            if (!upDone && elapsed() >= o.upgradeAt) { upDone = true; conditionStep(true); }
            if (!cancelDone && upDone && elapsed() >= o.cancelAt) { cancelDone = true; conditionStep(false); }
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
        {
            std::string cs;
            for (auto& c : ls.conditions)
                if (c.first.callId == s.callId)
                    cs += std::string(cs.empty() ? "" : ",") + "{\"cause\":\"" + toString(c.second) + "\",\"emergency\":" +
                          (c.first.condition.emergency ? "true" : "false") + ",\"code\":" + std::to_string(c.first.condition.lastCode) + "}";
            s.extra += ",\"conditions\":[" + cs + "],\"alerts\":" + std::to_string(ls.alerts.size());
        }
        if (o.pttAt >= 0 && ls.granted == 0) { s.outcome = "floor_not_granted"; rc = 6; }
        // 일제 통화 개시자: 발언을 놓은 뒤 코어가 호를 해제했으면(TS 24.380 §6.2.4.6.4) 시한 전에 끝난 것이 정상이다.
        else if (o.broadcast && gone && o.pttAt >= 0) s.extra += ",\"broadcast_released\":true";
        return finish(s.callId);
    }

    if (o.cmd == "video-call" || o.cmd == "video-answer") {
        if (o.cmd == "video-call") {
            VideoGroupCallOptions vo;
            vo.prearranged = o.prearranged;
            vo.queueing = o.queueing;
            vo.maxPriority = o.priority;
            vo.implicitTransmissionRequest = o.implicit;
            vo.sessionUri = o.rejoinUri;
            s.callId = eng.joinVideoGroupCall(acc, o.target, vo);
            if (s.callId < 0) { s.outcome = "invite_failed"; rc = 4; return finish(-1); }
        } else {
            bool got = ls.waitFor([&] { return ls.haveIncoming && ls.incoming.service == McService::McVideo; }, o.durationSec);
            if (!got) { s.outcome = "no_invitation"; rc = 4; return finish(-1); }
            s.callId = ls.incoming.callId;                                   // 코어가 자동 수락한다(autoAnswerMcvideo)
        }
        bool up = waitActive(ls, s.callId, o.timeoutSec);
        CallInfo ci = ls.calls.count(s.callId) ? ls.calls[s.callId] : CallInfo{};
        if (!up || ci.state != CallState::Active) { s.outcome = up ? "call_failed" : "call_timeout"; rc = 4; return finish(s.callId); }
        auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count(); };
        auto txSeen = [&](std::initializer_list<TransmissionEvent::Kind> kinds, size_t from) {
            for (size_t k = from; k < ls.transmissions.size(); ++k)
                for (auto kd : kinds) if (ls.transmissions[k].callId == s.callId && ls.transmissions[k].kind == kd) return true;
            return false;
        };
        bool txDone = o.transmitAt < 0, gone = false;
        size_t accepted = 0;                                             // 이미 [받기] 한 알림 수(receptions 색인)
        std::set<std::string> acceptedIds;
        while (elapsed() < o.durationSec && !gone) {
            if (!txDone && elapsed() >= o.transmitAt) {
                txDone = true;
                size_t before = ls.transmissions.size();
                Result tr = eng.requestTransmission(s.callId, o.priority);
                if (!tr.ok) std::fprintf(stderr, "[cimsue-cli] requestTransmission: %s\n", tr.reason.c_str());
                using K = TransmissionEvent::Kind;
                ls.waitFor([&] { return txSeen({K::Granted, K::Rejected, K::RequestTimeout, K::QueuePosition}, before); }, 5);
                ls.waitFor([&] { return disconnected(s.callId); }, o.transmitLen);
                before = ls.transmissions.size();
                Result rr = eng.releaseTransmission(s.callId);
                if (rr.ok) ls.waitFor([&] { return txSeen({K::Ended, K::RequestTimeout}, before); }, 5);
            }
            if (o.accept) {
                std::vector<std::string> todo;
                ls.waitFor([&] {
                    for (; accepted < ls.receptions.size(); ++accepted) {
                        const ReceptionEvent& e = ls.receptions[accepted];
                        if (e.callId == s.callId && e.kind == ReceptionEvent::Kind::Notified && !e.transmitter.automatic &&
                            !acceptedIds.count(e.transmitter.userId))
                            todo.push_back(e.transmitter.userId);
                    }
                    return !todo.empty();
                }, 0);
                for (auto& id : todo) {                                      // 엔진 명령은 main 에서(리스너 스레드가 엔진을 다시 부르지 않게)
                    acceptedIds.insert(id);
                    Result ar = eng.acceptReception(s.callId, id);
                    std::fprintf(stderr, "[cimsue-cli] acceptReception %s: %s\n", id.c_str(), ar.ok ? "sent" : ar.reason.c_str());
                }
            }
            gone = ls.waitFor([&] { return disconnected(s.callId); }, 1);
        }
        TransmissionInfo ti = eng.transmissionInfo(s.callId);
        ci = eng.callInfo(s.callId);
        auto count = [&](auto& v, auto kind) { int n = 0; for (auto& e : v) if (e.callId == s.callId && e.kind == kind) ++n; return n; };
        using K = TransmissionEvent::Kind;
        using R = ReceptionEvent::Kind;
        const int granted = count(ls.transmissions, K::Granted);
        s.extra = ",\"session_uri\":\"" + jsonEsc(ci.sessionUri) + "\",\"tc_local_port\":" + std::to_string(ti.localPort) +
                  ",\"tc_remote\":\"" + ti.remoteIp + ":" + std::to_string(ti.remotePort) + "\",\"tx_granted\":" + std::to_string(granted) +
                  ",\"tx_rejected\":" + std::to_string(count(ls.transmissions, K::Rejected)) +
                  ",\"tx_revoked\":" + std::to_string(count(ls.transmissions, K::Revoked)) +
                  ",\"tx_ended\":" + std::to_string(count(ls.transmissions, K::Ended)) +
                  ",\"rx_notified\":" + std::to_string(count(ls.receptions, R::Notified)) +
                  ",\"rx_granted\":" + std::to_string(count(ls.receptions, R::Granted)) +
                  ",\"rx_rejected\":" + std::to_string(count(ls.receptions, R::Rejected));
        if (o.transmitAt >= 0 && granted == 0) { s.outcome = "transmission_not_granted"; rc = 6; }
        return finish(s.callId);
    }

    if (o.cmd == "alert") {
        int64_t tok = eng.sendEmergencyAlert(acc, o.target, !o.alertCancel, o.originatedBy, o.cancelGroupEmergency);
        if (tok < 0) { s.outcome = "alert_send_failed"; rc = 7; return finish(-1); }
        bool got = ls.waitFor([&] { return ls.results.count(tok) > 0; }, o.timeoutSec);
        if (got) { s.code = ls.results[tok].code; s.reason = ls.results[tok].reason; }
        if (!got || s.code / 100 != 2) { s.outcome = got ? "alert_rejected" : "alert_timeout"; rc = 7; }
        return finish(-1);
    }

    if (o.cmd == "sds") {
        SdsSend sds = eng.sendGroupSds(acc, o.target, o.text);
        if (!sds.ok) { s.outcome = "sds_send_failed"; rc = 7; return finish(-1); }
        // 최종 결과는 발신 token 으로 — 시그널링 평면 = MESSAGE 응답, media plane = MSRP(저장소 200/REPORT)
        bool got = ls.waitFor([&] { return ls.results.count(sds.token) > 0; }, o.timeoutSec);
        int code = 0;
        std::string plane = "signalling";
        if (got) { const RequestResult& rr = ls.results[sds.token]; code = rr.code; s.reason = rr.reason; if (rr.method == "MSRP") plane = "media"; }
        // media plane 은 서버가 저장·배포 뒤 발신 leg 를 BYE 한다 — 그 전에 엔진을 내리면 배포가 끊긴다. 코어의 자체 해제(5 s)까지 기다린다.
        if (plane == "media") std::this_thread::sleep_for(std::chrono::seconds(6));
        s.code = code;
        s.extra = ",\"msg_id\":\"" + sds.msgId + "\",\"plane\":\"" + plane + "\",\"bytes\":" + std::to_string(o.text.size());
        if (!got || code / 100 != 2) { s.outcome = got ? "sds_rejected" : "sds_timeout"; rc = 7; }
        else if (o.waitDispositionSec > 0) {
            // 전달 확인 통지(§12.2.1.2) — 같은 message ID 의 SDS NOTIFICATION. 그룹이면 멤버마다 하나씩 온다(집계 없음).
            int notif = 0;
            ls.waitFor([&] {
                for (auto& m : ls.sds) if (m.notification && m.msgId == sds.msgId) { notif = m.notifType; return true; }
                return false;
            }, o.waitDispositionSec);
            s.extra += ",\"disposition\":" + std::to_string(notif);
            if (!notif) { s.outcome = "no_disposition"; rc = 7; }
        }
        return finish(-1);
    }

    if (o.cmd == "sds-recv") {
        // 전달 확인 회신은 main 에서 — 리스너 스레드가 엔진을 다시 부르지 않게
        size_t handled = 0;
        int notified = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(o.durationSec);
        while (std::chrono::steady_clock::now() < until) {
            std::vector<SdsMessage> fresh;
            ls.waitFor([&] {
                if (ls.sds.size() <= handled) return false;
                fresh.assign(ls.sds.begin() + (long)handled, ls.sds.end());
                handled = ls.sds.size();
                return true;
            }, 1);
            if (!o.notifyDelivered) continue;
            for (auto& m : fresh) {
                if (m.notification || !(m.dispositionReq & 1)) continue;                 // 1 = delivery 요청(§15.2.3)
                SdsSend n = eng.sendSdsNotification(acc, m.fromUri, m.convId, m.msgId, 2, m.groupUri);   // 2 = DELIVERED
                std::fprintf(stderr, "[cimsue-cli] disposition delivered → %s group=%s msg=%s %s\n", m.fromUri.c_str(),
                             m.groupUri.c_str(), m.msgId.c_str(), n.ok ? "sent" : n.reason.c_str());
                if (n.ok) notified++;
            }
        }
        s.extra = ",\"sds_received\":" + std::to_string(ls.sds.size()) + ",\"alerts\":" + std::to_string(ls.alerts.size()) +
                  ",\"notified\":" + std::to_string(notified);
        if (ls.sds.empty() && ls.alerts.empty()) { s.outcome = "no_sds"; rc = 7; }   // 경보만 받은 경우도 수신이다
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
