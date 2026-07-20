#include "PCmpServer.h"
#include "PLog.h"
#include "SimpleJson.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <sstream>
#include <cstring>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cctype>
#include "PMcpttGroup.h"
#include <fstream>
#include <tuple>
#include <unordered_map>
#include <chrono>

// 비동기 배치 writer 튜닝값 (CSP SipMessageLogger 와 동일)
static const size_t kCmpLogNotifyThreshold = 128;     // 큐가 이만큼 쌓이면 즉시 flush 깨움
static const size_t kCmpLogMaxQueue = 200000;         // 큐 상한 (NFS 장애 backlog 시 메모리 폭주 방지)
static const int kCmpLogFlushIntervalMs = 100;        // 주기 flush (버퍼 잔여분 보장)

PCmpServer::PCmpServer(const std::string& name, const std::string& configFile)
    : PModule(name), _running(false), _udpFd(-1), _configFile(configFile), _sessionTimeout(600), _orphanReclaimSec(120), _floorIdleSec(10), _rtpWorkerCount(4),
      _pttRtpStartPort(52000), _pttRtpPoolSize(10), _pttFloorStartPort(54000), _pttVideoStartPort(56000), _pttMemberPoolSize(40), _segmentIntervalSec(60),
      _msgSeq(-1), _lastRxSeq(0),
      _logFlowFloor(true), _logFlowDtmf(true), _logFlowRtcp(false),
      _leakReclaimTotal(0), _leakReclaimOrphan(0), _leakReclaimHold(0)
{
    loadConfig();

    // RTP epoll 리액터: 풀 소켓 fd 를 등록하기 전에 epoll 인스턴스를 먼저 만든다.
    //   (구: addWorker 1ms period busy-poll → 이벤트 구동 epoll 로 교체. idle CPU 0.)
    _reactors.resize(_rtpWorkerCount);
    for (int i = 0; i < _rtpWorkerCount; ++i) {
        _reactors[i].epfd = epoll_create1(0);
        if (_reactors[i].epfd < 0)
            LOG_ERROR("PCmpServer", "epoll_create1 failed for reactor %d: %s", i, strerror(errno));
    }

    initResourcePool();
    initPttResourcePool();
    initPttMemberPool();
}

PCmpServer::~PCmpServer() {
    stopServer();
    for(auto const& [name, group] : _groups) {
        delete group;
    }
    _groups.clear();

    for(auto* rtp : _resourcePool) {
        delete rtp;
    }
    _resourcePool.clear();
    _freeResources.clear();

    for(auto* ptt : _pttPool) {
        delete ptt;
    }
    _pttPool.clear();
    _freePttResources.clear();

    for(auto* mu : _pttMemberPool) {
        delete mu;
    }
    _pttMemberPool.clear();
    _freePttMembers.clear();
    _memberUnits.clear();

    // epoll fd 정리 (스레드는 stopServer 에서 이미 join 됨)
    for (auto& r : _reactors) {
        if (r.epfd >= 0) { ::close(r.epfd); r.epfd = -1; }
    }
}

bool PCmpServer::startServer() {
    _udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_udpFd < 0) {
        LOG_ERROR("PCmpServer", "socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_serverIp.c_str());
    addr.sin_port = htons(_serverPort);

    if (bind(_udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("PCmpServer", "bind() failed on %s:%d: %s", _serverIp.c_str(), _serverPort, strerror(errno));
        close(_udpFd);
        return false;
    }

    _running = true;
    startLogWriter();  // 비동기 배치 로그 writer 기동 (control 스레드 NFS HOL 블로킹 제거)
    std::thread([this]() {
        this->runControlLoop();
    }).detach();

    // Session timeout 체크 스레드 시작
    if (_sessionTimeout > 0) {
        _timeoutThread = std::thread([this]() { this->timeoutLoop(); });
        LOG_INFO("PCmpServer", "Session timeout thread started (timeout=%ds)", _sessionTimeout);
    }

    // RTP epoll 리액터 스레드 기동 (epoll fd 는 생성자에서 만들고 풀 fd 는 init 때 등록 완료)
    _reactorRunning = true;
    for (int i = 0; i < (int)_reactors.size(); ++i) {
        int w = i;
        _reactors[i].thread = std::thread([this, w]() { this->reactorLoop(w); });
    }
    LOG_INFO("PCmpServer", "RTP epoll reactors started (%d workers, event-driven)", (int)_reactors.size());

    LOG_INFO("PCmpServer", "Server listening on %s:%d", _serverIp.c_str(), _serverPort);
    return true;
}

void PCmpServer::stopServer() {
    _running = false;
    // 리액터 스레드 정지 (epoll_wait 1s timeout 내 종료)
    _reactorRunning = false;
    for (auto& r : _reactors) {
        if (r.thread.joinable()) r.thread.join();
    }
    if (_timeoutThread.joinable()) _timeoutThread.join();
    stopLogWriter();  // timeout 스레드 정지 후 잔여 로그 전량 flush
    if (_udpFd >= 0) {
        ::close(_udpFd);
        _udpFd = -1;
    }
    LOG_INFO("PCmpServer", "Server stopped");
}

void PCmpServer::runControlLoop() {
    char buf[4096];
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    while (_running) {
        int len = recvfrom(_udpFd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&clientAddr, &addrLen);
        if (len > 0) {
            buf[len] = '\0';
            LOG_DEBUG("PCmpServer", "Recv %d bytes from %s:%d", len, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
            std::string ip = inet_ntoa(clientAddr.sin_addr);
            int port = ntohs(clientAddr.sin_port);
            handlePacket(buf, len, ip, port);
        }
    }
}

// Modified to parse JSON packet
void PCmpServer::handlePacket(char* buf, int len, const std::string& ip, int port) {
    if (len <= 0) return;
    std::string strPacket(buf, len);
    
    // Parse JSON Wrapper (envelope v2: {hdr, payload} — docs/api/cmp_media_api.md)
    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(strPacket);

    if (root.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("PCmpServer", "Invalid JSON Packet from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        return;
    }

    // 원문 기록 (수신)
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string ts = getTimestamp();
    _lastRxSeq = writeMsgLine(ts.c_str(), "RX", peerStr.c_str(), "JSON", strPacket.c_str());

    SimpleJson::JsonNode hdr = root.Get("hdr");
    if (hdr.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("PCmpServer", "Missing hdr from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        sendErr(ip, port, (int)root.GetInt("trans_id", 0), "", "", "",
                "BAD_REQUEST", "hdr required (protocol v2)");
        return;
    }

    int ver = (int)hdr.GetInt("ver", 0);
    int transId = (int)hdr.GetInt("trans_id", 0);
    std::string type = hdr.GetString("type");
    std::string cmdUpper = hdr.GetString("cmd");
    std::transform(cmdUpper.begin(), cmdUpper.end(), cmdUpper.begin(), ::toupper);

    if (ver != 2) {
        sendErr(ip, port, transId, cmdUpper, hdr.GetString("sesid"), hdr.GetString("service"),
                "UNSUPPORTED_VER", "protocol ver 2 required");
        return;
    }
    if (type != "request") {
        // 응답/이벤트 방향 오수신 — 응답하면 루프가 되므로 기록만 하고 버린다
        LOG_WARN("PCmpServer", "Drop non-request type=%s cmd=%s from %s:%d", type.c_str(), cmdUpper.c_str(), ip.c_str(), port);
        return;
    }

    // payload 준비 — hdr 의 상관 필드(sesid/service)와 cmd 를 계승해 핸들러 공통 로직에 전달
    SimpleJson::JsonNode payload = root.Get("payload");
    if (payload.type != SimpleJson::JSON_OBJECT) {
        payload = SimpleJson::JsonNode();
        payload.type = SimpleJson::JSON_OBJECT;
    }
    if (hdr.Has("sesid")) payload.Set("sesid", hdr.GetString("sesid"));
    if (hdr.Has("service")) payload.Set("service", hdr.GetString("service"));
    payload.Set("cmd", cmdUpper);

    // Dispatch
    LOG_DEBUG("PCmpServer", "Dispatching cmd=%s transId=%d from %s:%d", cmdUpper.c_str(), transId, ip.c_str(), port);
    if (cmdUpper == "RELAY_ADD") processAdd(payload, ip, port, transId);
    else if (cmdUpper == "RELAY_MODIFY") processModify(payload, ip, port, transId);
    else if (cmdUpper == "RELAY_REMOVE") processRemove(payload, ip, port, transId);
    else if (cmdUpper == "HEARTBEAT") processAlive(payload, ip, port, transId);
    else if (cmdUpper == "PTT_GROUP_ADD") processAddGroup(payload, ip, port, transId);
    else if (cmdUpper == "PTT_GROUP_MODIFY") processModifyGroup(payload, ip, port, transId);
    else if (cmdUpper == "PTT_GROUP_REMOVE") processRemoveGroup(payload, ip, port, transId);
    else if (cmdUpper == "PTT_JOIN") processJoinGroup(payload, ip, port, transId);
    else if (cmdUpper == "PTT_LEAVE") processLeaveGroup(payload, ip, port, transId);
    else if (cmdUpper == "PTT_FLOOR_TIER") processSetFloorTier(payload, ip, port, transId);
    else if (cmdUpper == "STATS") processStats(payload, ip, port, transId);
    else {
        LOG_WARN("PCmpServer", "Unknown CMD: %s from %s:%d", cmdUpper.c_str(), ip.c_str(), port);
        sendErr(ip, port, transId, cmdUpper, payload.GetString("sesid"), payload.GetString("service"),
                "UNKNOWN_CMD", ("unknown cmd: " + cmdUpper).c_str());
    }
}

// ── v2 응답 빌더 ────────────────────────────────────────────────────────────
// 응답 = {hdr:{ver,trans_id,node,cmd,type,status[,code,reason][,sesid,service]}[,payload]}
// sesid/service 는 호 문맥 명령에서만(빈 값이면 생략), CORE(HEARTBEAT/STATS)는 빈 값 전달.
int PCmpServer::sendOk(const std::string& ip, int port, int transId, const std::string& cmd,
                       const std::string& sesid, const std::string& svc,
                       const SimpleJson::JsonNode* body,
                       const char* caller, const char* callee) {
    SimpleJson::JsonNode hdr;
    hdr.Set("ver", 2);
    hdr.Set("trans_id", transId);
    hdr.Set("node", _systemId);
    hdr.Set("cmd", cmd);
    hdr.Set("type", "response");
    hdr.Set("status", "OK");
    if (!sesid.empty()) hdr.Set("sesid", sesid);
    if (!svc.empty()) hdr.Set("service", svc);
    SimpleJson::JsonNode env;
    env.Set("hdr", hdr);
    if (body) env.Set("payload", *body);
    return sendResponse(ip, port, env.ToString(), caller, callee);
}

int PCmpServer::sendErr(const std::string& ip, int port, int transId, const std::string& cmd,
                        const std::string& sesid, const std::string& svc,
                        const char* code, const char* reason) {
    SimpleJson::JsonNode hdr;
    hdr.Set("ver", 2);
    hdr.Set("trans_id", transId);
    hdr.Set("node", _systemId);
    hdr.Set("cmd", cmd);
    hdr.Set("type", "response");
    hdr.Set("status", "ERROR");
    hdr.Set("code", code);
    hdr.Set("reason", reason);
    if (!sesid.empty()) hdr.Set("sesid", sesid);
    if (!svc.empty()) hdr.Set("service", svc);
    SimpleJson::JsonNode env;
    env.Set("hdr", hdr);
    return sendResponse(ip, port, env.ToString());
}

// HEARTBEAT/STATS 공통 자원 요약 — resource 키 목록이 곧 기능 광고 (호출측이 _mutex 보유)
SimpleJson::JsonNode PCmpServer::buildResourceSummary() {
    int freeCount = (int)_freeResources.size();
    int pttFreeCount = (int)_freePttResources.size();
    int pttTotalPorts = (int)_pttPool.size();
    int joined = 0;
    for (auto const& [gid, group] : _groups) joined += group->getMemberCount();

    SimpleJson::JsonNode relay;
    relay.Set("total", _rtpPoolSize);
    relay.Set("used", _rtpPoolSize - freeCount);
    relay.Set("sessions", (int)_sessions.size());

    SimpleJson::JsonNode ptt;
    ptt.Set("total", pttTotalPorts);
    ptt.Set("used", pttTotalPorts - pttFreeCount);
    ptt.Set("groups", (int)_groups.size());
    ptt.Set("joined", joined);
    ptt.Set("member_total", (int)_pttMemberPool.size());
    ptt.Set("member_used", (int)(_pttMemberPool.size() - _freePttMembers.size()));

    SimpleJson::JsonNode resource;
    resource.Set("relay", relay);
    resource.Set("ptt", ptt);
    return resource;
}

int PCmpServer::sendResponse(const std::string& ip, int port, const std::string& msg,
                              const char* caller, const char* callee) {
    // 원문 기록 (송신)
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string ts = getTimestamp();
    int txSeq = writeMsgLine(ts.c_str(), "TX", peerStr.c_str(), "JSON", msg.c_str(),
                              caller, callee);
    LOG_DEBUG("PCmpServer", "Sending %lu bytes to %s:%d", msg.length(), ip.c_str(), port);
    if (_udpFd != -1) {
        struct sockaddr_in cliaddr;
        memset(&cliaddr, 0, sizeof(cliaddr));
        cliaddr.sin_family = AF_INET;
        cliaddr.sin_port = htons(port);
        cliaddr.sin_addr.s_addr = inet_addr(ip.c_str());
        int sent = sendto(_udpFd, msg.c_str(), msg.length(), 0, (struct sockaddr*)&cliaddr, sizeof(cliaddr));
        if (sent < 0) {
            LOG_ERROR("PCmpServer", "sendto failed to %s:%d: %s", ip.c_str(), port, strerror(errno));
        }
    }
    return txSeq;
}

// sesid 발행: {caller}::cmp::{us_ts}::{counter}
std::string PCmpServer::issueSesid(const std::string& caller) {
    static std::mutex sMtx;
    static std::string sLastTs;
    static unsigned int sCounter = 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    char tsBuf[32];
    snprintf(tsBuf, sizeof(tsBuf), "%04d%02d%02d%02d%02d%02d%06ld",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (long)tv.tv_usec);
    unsigned int counter;
    std::string ts(tsBuf);
    {
        std::lock_guard<std::mutex> lock(sMtx);
        if (ts == sLastTs) ++sCounter;
        else { sLastTs = ts; sCounter = 1; }
        counter = sCounter;
    }
    std::string r = caller;
    r += "::cmp::";
    r += ts;
    r += "::";
    r += std::to_string(counter);
    return r;
}

void PCmpServer::processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    // CORE 명령 — wire 에는 sesid/service 미포함. flow 로그 분류용 sesid 만 내부 발행.
    std::string sesid = issueSesid("");
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string txIdStr = std::to_string(transId);

    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    logFlow("heartbeat", "csp", "cmp", "JSON", "HEARTBEAT", "",
            txIdStr.c_str(), "system", sesid.c_str(), "", _lastRxSeq, "csp");

    SimpleJson::JsonNode body;
    {
        PAutoLock lock(_mutex);
        body.Set("resource", buildResourceSummary());
    }
    int txSeq = sendOk(ip, port, transId, "HEARTBEAT", "", "", &body);
    logFlow("heartbeat", "cmp", "csp", "JSON", "OK", "",
            txIdStr.c_str(), "system", sesid.c_str(), "", txSeq, "csp");
    logBody("TX", peerStr.c_str(), "JSON", body.ToString().c_str());
}

void PCmpServer::processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    // CORE 명령 — wire 에는 sesid/service 미포함. flow 로그 분류용 sesid 만 내부 발행.
    std::string sesid = issueSesid("");
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string txIdStr = std::to_string(transId);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    logFlow("stats", "csp", "cmp", "JSON", "STATS", "",
            txIdStr.c_str(), "system", sesid.c_str(), "", _lastRxSeq, "csp");

    PAutoLock lock(_mutex);

    // 요약(resource) + 상세(detail) — HEARTBEAT 요약과 동일 구조에 진단 정보를 더한다
    SimpleJson::JsonNode body;
    body.Set("resource", buildResourceSummary());

    SimpleJson::JsonNode leak;
    leak.Set("total", (long long)_leakReclaimTotal);
    leak.Set("orphan", (long long)_leakReclaimOrphan);
    leak.Set("hold", (long long)_leakReclaimHold);

    // 그룹별 상세 (멤버수, floor 화자)
    SimpleJson::JsonNode groupsArr;
    groupsArr.type = SimpleJson::JSON_ARRAY;
    for (auto const& [gid, group] : _groups) {
        SimpleJson::JsonNode g;
        g.Set("group_id", gid);
        g.Set("members", group->getMemberCount());
        std::string holder = group->getFloorHolder();
        if (!holder.empty()) g.Set("floor_holder", holder);
        groupsArr.Add(g);
    }

    // 미협상 소스 드롭 누적 (활성 세션/그룹 합산 — 진단용)
    long srcDrop = 0;
    for (auto const& [sid, rtp] : _sessions) if (rtp) srcDrop += rtp->getSrcDrop();
    for (auto const& [gid, group] : _groups) if (group) srcDrop += group->getSrcDrop();

    // NAT latch 완료 leg 목록 — 학습된 실주소 노출 (ue_nat_traversal.md §5 관측)
    SimpleJson::JsonNode natArr;
    natArr.type = SimpleJson::JSON_ARRAY;
    for (auto const& [sid, rtp] : _sessions) {
        if (!rtp) continue;
        for (int i = 0; i < 2; ++i) {
            std::string learnedIp; int learnedPort = 0;
            if (rtp->getNatLatched(i, learnedIp, learnedPort)) {
                SimpleJson::JsonNode n;
                n.Set("key", sid);
                n.Set("leg", i == 0 ? "a" : "b");
                n.Set("learned_ip", learnedIp);
                n.Set("learned_port", learnedPort);
                natArr.Add(n);
            }
        }
    }
    for (auto const& [gid, group] : _groups) {
        if (!group) continue;
        std::vector<std::tuple<std::string, std::string, int>> latched;
        group->collectNatLatched(latched);
        for (auto const& [sid, learnedIp, learnedPort] : latched) {
            SimpleJson::JsonNode n;
            n.Set("key", gid + ":" + sid);
            n.Set("leg", sid);
            n.Set("learned_ip", learnedIp);
            n.Set("learned_port", learnedPort);
            natArr.Add(n);
        }
    }

    SimpleJson::JsonNode detail;
    detail.Set("session_timeout", _sessionTimeout);
    detail.Set("orphan_reclaim_sec", _orphanReclaimSec);
    detail.Set("leak_reclaim", leak);
    detail.Set("rtp_src_drop", (long long)srcDrop);
    detail.Set("nat", natArr);
    detail.Set("groups", groupsArr);
    body.Set("detail", detail);

    int txSeq = sendOk(ip, port, transId, "STATS", "", "", &body);
    logFlow("stats", "cmp", "csp", "JSON", "OK", "",
            txIdStr.c_str(), "system", sesid.c_str(), "", txSeq, "csp");
    logBody("TX", peerStr.c_str(), "JSON", body.ToString().c_str());
    LOG_INFO("PCmpServer", "STATS: sessions=%d groups=%d ports_free=%d/%d", (int)_sessions.size(),
             (int)_groups.size(), (int)_freeResources.size(), _rtpPoolSize);
}

void PCmpServer::processAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string cmdName = payload.GetString("cmd");
    if (cmdName.empty()) cmdName = "RELAY_ADD";
    std::string sessionId = payload.GetString("session_id");
    std::string rmtIp = payload.GetString("remote_ip");
    int rmtPort = (int)payload.GetInt("remote_port");
    int rmtVideoPort = (int)payload.GetInt("remote_video_port");
    int peerIdx = (int)payload.GetInt("peer_index", -1);
    // NAT 목적지 latch 허용 지시 (제어평면 승인형 — ue_nat_traversal.md §4-5)
    int rmtNat = (int)payload.GetInt("remote_nat", 0);
    std::string rmtSigIp = payload.GetString("remote_sig_ip");
    std::string caller = payload.GetString("caller");
    std::string callee = payload.GetString("callee");

    // sesid: payload에서 받아 저장. 없으면 CMP 자체 발행 (방어적)
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) sesid = issueSesid(caller);

    // service: payload에서 받아 저장 (CSP 설정 기반), 없으면 "volte" fallback (VoLTE processAdd 특성)
    std::string svc = payload.GetString("service");
    if (svc.empty()) svc = "volte";

    // detail: 명령어별로 CSP 기록과 동일 포맷
    std::string detail;
    if (cmdName == "RELAY_MODIFY") {
        // MODIFY는 peer_index가 가리키는 한 쪽만 표기 (발/착 구분)
        if (peerIdx == 1 && !callee.empty()) detail = callee;
        else if (peerIdx == 0 && !caller.empty()) detail = caller;
        else if (!caller.empty() && !callee.empty()) detail = caller + "\xe2\x86\x92" + callee;
    } else {
        // RELAY_ADD: caller→callee
        if (!caller.empty() && !callee.empty()) detail = caller + "\xe2\x86\x92" + callee;
        else if (!caller.empty()) detail = caller;
        else detail = sessionId;
    }

    std::string rtpIp = _serverIp; // Resource IP
    int rtpPort = 0;
    int videoPort = 0;

    PRtpRelay* rtp = NULL;
    // _sesidMap/_serviceMap 은 sweeper(timeoutLoop)가 _mutex 하에 erase 하므로 쓰기도 lock 안에서.
    PAutoLock lock(_mutex);
    _sesidMap[sessionId] = sesid;
    _serviceMap[sessionId] = svc;

    if (_sessions.find(sessionId) == _sessions.end()) {
        // MODIFY 는 기존 세션 전제 (cmp_media_api.md §6.2) — 소실 세션을 부활시키지 않는다.
        //   부활 시 포트가 재할당·녹취 미개시인데 client 는 구 포트를 이미 광고 중이라 정합 불가.
        if (cmdName == "RELAY_MODIFY") {
            std::string peerStr = ip + ":" + std::to_string(port);
            logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
            std::string txIdStr = std::to_string(transId);
            logFlow(sessionId, "csp", "cmp", "JSON", cmdName.c_str(), detail.c_str(), txIdStr.c_str(),
                    svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp", caller.c_str(), callee.c_str());
            int txSeq = sendErr(ip, port, transId, cmdName, sesid, svc,
                                "NOT_FOUND", "session not found");
            logFlow(sessionId, "cmp", "csp", "JSON", "ERROR", "Session Not Found",
                    txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp",
                    caller.c_str(), callee.c_str());
            LOG_WARN("PCmpServer", "RELAY_MODIFY session=%s not found", sessionId.c_str());
            _sesidMap.erase(sessionId);  // 세션 미존재 — 캐시 잔존 방지
            _serviceMap.erase(sessionId);
            return;
        }
        rtp = allocResource(rtpIp, rtpPort, videoPort);
        if (rtp) {
            rtp->setSessionId(sessionId);
            // 풀 재사용 relay 의 잔존 활동시각 초기화 — 없으면 idle 이 풀 생성시각 기준으로
            // 계산되어 새 세션이 다음 sweep(60s)에 orphan_no_rtp 로 즉시 회수된다.
            rtp->resetActivity();
            _sessions[sessionId] = rtp;
        }
    } else {
        rtp = _sessions[sessionId];
        rtpIp = _rtpIp;  // RTP IP는 항상 설정값 사용
        rtpPort = rtp->getLocalPort(0); // reuse existing
        videoPort = rtp->getLocalVideoPort(0);
    }

    if (rtp) {
        if (rmtPort > 0) {
             rtp->setRemote(rmtIp, rmtPort, rmtVideoPort, peerIdx, rmtNat != 0, rmtSigIp);
        }

        // Worker thread는 initResourcePool()에서 영구 등록됨 — 여기서 추가 불필요

        // CSP가 전달한 record_dir이 있으면 해당 경로에 녹취 (없으면 녹취 안 함)
        std::string recordDir = payload.GetString("record_dir");
        if (!recordDir.empty() && !rtp->isRecording()) {
            rtp->startRecording(recordDir, sessionId, caller, callee, _segmentIntervalSec);
        }

        // CMP flow + body 로그 (sesid 적용)
        std::string peerStr = ip + ":" + std::to_string(port);
        logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
        std::string txIdStr = std::to_string(transId);
        logFlow(sessionId, "csp", "cmp", "JSON", cmdName.c_str(),
                detail.c_str(), txIdStr.c_str(),
                svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp",
                caller.c_str(), callee.c_str());
        if (cmdName == "RELAY_ADD") {
            logFlow(sessionId, "cmp", "cmp", "INT", "SESSION_START",
                    ("port=" + std::to_string(rtpPort)).c_str(),
                    "", svc.c_str(), sesid.c_str(), "", 0, "",
                    caller.c_str(), callee.c_str());
        }

        // leg 별 전용 포트: peer0(발신 A) = local_port*, peer1(착신 B) = local_port_b*
        SimpleJson::JsonNode respBody;
        respBody.Set("local_ip", rtpIp);
        respBody.Set("local_port", rtpPort);
        respBody.Set("local_video_port", videoPort);
        respBody.Set("local_port_b", (int)rtp->getLocalPort(1));
        respBody.Set("local_video_port_b", (int)rtp->getLocalVideoPort(1));

        int txSeq = sendOk(ip, port, transId, cmdName, sesid, svc, &respBody,
                           caller.c_str(), callee.c_str());
        // OK 응답은 detail 불필요 (요청 detail 과 중복 방지)
        logFlow(sessionId, "cmp", "csp", "JSON", "OK", "",
                txIdStr.c_str(),
                svc.c_str(), sesid.c_str(), "", txSeq, "csp",
                caller.c_str(), callee.c_str());
        logBody("TX", peerStr.c_str(), "JSON", respBody.ToString().c_str());
        LOG_INFO("PCmpServer", "%s session=%s remote=%s:%d -> local=%s:%d", cmdName.c_str(), sessionId.c_str(),
                 rmtIp.c_str(), rmtPort, rtpIp.c_str(), rtpPort);
    } else {
         std::string txIdStr2 = std::to_string(transId);
         int txSeq = sendErr(ip, port, transId, cmdName, sesid, svc,
                             "NO_RESOURCE", "rtp pool exhausted");
         logFlow(sessionId, "cmp", "csp", "JSON", "ERROR", "No Resource",
                 txIdStr2.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp",
                 caller.c_str(), callee.c_str());
         LOG_WARN("PCmpServer", "%s session=%s FAILED: no available resource", cmdName.c_str(), sessionId.c_str());
         _sesidMap.erase(sessionId);  // 할당 실패 — 캐시 잔존 방지
         _serviceMap.erase(sessionId);
    }
}

void PCmpServer::processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string sessionId = payload.GetString("session_id");
    std::string caller = payload.GetString("caller");
    std::string callee = payload.GetString("callee");
    std::string detail;
    if (!caller.empty() && !callee.empty()) detail = caller + "\xe2\x86\x92" + callee;
    else detail = sessionId;

    // sweeper 와의 _sesidMap/_serviceMap 동시 접근 방지 — map 읽기 전에 lock
    PAutoLock lock(_mutex);

    // sesid: payload > 기존 저장된 값 > 발행
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto it = _sesidMap.find(sessionId);
        if (it != _sesidMap.end()) sesid = it->second;
    }
    if (sesid.empty()) sesid = issueSesid(caller);

    // service: payload > 캐시 > volte fallback
    std::string svc = payload.GetString("service");
    if (svc.empty()) {
        auto it = _serviceMap.find(sessionId);
        if (it != _serviceMap.end()) svc = it->second;
    }
    if (svc.empty()) svc = "volte";

    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(sessionId, "csp", "cmp", "JSON", "RELAY_REMOVE",
            detail.c_str(), txIdStr.c_str(),
            svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp",
            caller.c_str(), callee.c_str());

    if (_sessions.find(sessionId) != _sessions.end()) {
        PRtpRelay* rtp = _sessions[sessionId];
        logFlow(sessionId, "cmp", "cmp", "INT", "SESSION_END", "",
                "", svc.c_str(), sesid.c_str(), "", 0, "",
                caller.c_str(), callee.c_str());
        rtp->reset();
        freeResource(rtp);
        _sessions.erase(sessionId);
        LOG_INFO("PCmpServer", "RELAY_REMOVE session=%s", sessionId.c_str());
    } else {
        LOG_WARN("PCmpServer", "RELAY_REMOVE session=%s not found", sessionId.c_str());
    }

    int txSeq = sendOk(ip, port, transId, "RELAY_REMOVE", sesid, svc, NULL,
                       caller.c_str(), callee.c_str());
    logFlow(sessionId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(),
            svc.c_str(), sesid.c_str(), "", txSeq, "csp",
            caller.c_str(), callee.c_str());
    logBody("TX", peerStr.c_str(), "JSON", "OK");

    // 세션 종료 후 캐시 정리
    _sesidMap.erase(sessionId);
    _serviceMap.erase(sessionId);
}

void PCmpServer::processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    LOG_DEBUG("PCmpServer", "MODIFY -> delegating to processAdd");
    processAdd(payload, ip, port, transId);
}

void PCmpServer::processAddGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string membersStr = payload.GetString("members");
    // sweeper 와의 _sesidMap/_serviceMap/_groupSubId 동시 접근 방지 — map 쓰기 전에 lock
    PAutoLock lock(_mutex);
    // sesid: payload 수신 값 > 자체 발행 (CSP가 안 보낸 경우 방어)
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) sesid = issueSesid("");
    _sesidMap[groupId] = sesid;
    // service: payload 계승, 없으면 mcptt fallback (PTT 그룹 특성)
    std::string svc = payload.GetString("service");
    if (svc.empty()) svc = "mcptt";
    _serviceMap[groupId] = svc;
    std::string subid = payload.GetString("subid");
    if (!subid.empty()) _groupSubId[groupId] = subid;
    // PTT_GROUP_MODIFY 위임 경로 포함 — flow 라벨은 실제 수신 cmd 를 따른다
    std::string cmdName = payload.GetString("cmd");
    if (cmdName.empty()) cmdName = "PTT_GROUP_ADD";
    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(groupId, "csp", "cmp", "JSON", cmdName.c_str(), groupId.c_str(), txIdStr.c_str(),
            svc.c_str(), sesid.c_str(), subid.c_str(), _lastRxSeq, "csp");

    std::string sharedIp = _rtpIp;
    int sharedFloorPort = 0;

    PRtpMulticast* pttSession = NULL;
    PMcpttGroup* group = NULL;

    if (_groups.find(groupId) == _groups.end()) {
        group = new PMcpttGroup(groupId);
        // Floor/DTMF 콜백: Flow enable flag 적용하여 조건부 기록
        bool logFloor = _logFlowFloor;
        bool logDtmf  = _logFlowDtmf;
        group->setLogCallback([this, logFloor, logDtmf](const std::string& key, const char* from, const char* to,
                                                         const char* proto, const char* label, const char* body) {
            if (proto && strcmp(proto, "MCPTT") == 0 && !logFloor) return;
            if (proto && strcmp(proto, "DTMF")  == 0 && !logDtmf)  return;
            logFlow(key, from, to, proto, label, body);
        });
        group->setRtcpLogEnable(_logFlowRtcp);
        pttSession = allocPttResource(sharedIp, sharedFloorPort);
        if (pttSession) {
             pttSession->setGroup(group);
             group->setDtmfConfig(_dtmfPttEnable, _dtmfPushDigit, _dtmfReleaseDigit);
             group->setPttSession(pttSession);

             // CSP가 전달한 record_dir이 있으면 해당 경로에 녹취
             std::string recordDir = payload.GetString("record_dir");
             if (!recordDir.empty()) {
                 group->setRecording(true, recordDir);
             }
             _groups[groupId] = group;

             logFlow(groupId, "cmp", "cmp", "INT", "GROUP_START",
                     ("floor=" + std::to_string(sharedFloorPort)).c_str(),
                     "", svc.c_str(), sesid.c_str(), subid.c_str());
             LOG_INFO("PCmpServer", "ADD_GROUP group=%s floor=%d (new)", groupId.c_str(), sharedFloorPort);
        } else {
             delete group;
             group = NULL;
             LOG_WARN("PCmpServer", "ADD_GROUP group=%s FAILED: no available resource", groupId.c_str());
        }
    } else {
        group = _groups[groupId];
        pttSession = group->getPttSession();
        if (pttSession) {
            sharedFloorPort = pttSession->getLocalFloorPort();
            sharedIp = _rtpIp;
        }
        // 기존 그룹이더라도 record_dir이 새로 전달되면 갱신
        std::string recordDir = payload.GetString("record_dir");
        if (!recordDir.empty() && !group->isRecordEnabled()) {
            group->setRecording(true, recordDir);
        }
        LOG_DEBUG("PCmpServer", "ADD_GROUP group=%s floor=%d (existing)", groupId.c_str(), sharedFloorPort);
    }

    std::vector<std::string> memberIds;
    if (group) {
        if (!membersStr.empty()) {
            std::stringstream ss(membersStr);
            std::string segment;
            std::map<std::string, int> priorities;
            std::map<std::string, std::string> roles;
            std::map<std::string, int> tiers;
            // 형식: id:priority[:role[:tier]]  (role/tier 미지정 시 participant/normal — 하위호환)
            while(std::getline(ss, segment, ',')) {
                size_t c1 = segment.find(':');
                if (c1 == std::string::npos) continue;
                std::string sid = segment.substr(0, c1);
                size_t c2 = segment.find(':', c1+1);
                int prio = 0;
                std::string role = "participant";
                try { prio = std::stoi(segment.substr(c1+1)); } catch(...) {}
                if (c2 != std::string::npos) {
                    size_t c3 = segment.find(':', c2+1);
                    role = segment.substr(c2+1, (c3 == std::string::npos) ? std::string::npos : c3 - (c2+1));
                    if (c3 != std::string::npos) {
                        int t = ParseFloorTier(segment.substr(c3+1));
                        if (t > TIER_NORMAL) tiers[sid] = t;
                    }
                }
                priorities[sid] = prio;
                roles[sid] = role;
                memberIds.push_back(sid);
            }
            group->updatePriorities(priorities);
            group->updateRoles(roles);
            group->updateTiers(tiers);
        }

        // broadcast 그룹 floor 독점 (TS 24.380 §10.3): 개시자만 floor.
        std::string groupType = payload.GetString("group_type");
        std::string initiator = payload.GetString("initiator_id");
        if (!groupType.empty() || !initiator.empty()) {
            group->setBroadcast(groupType, initiator);
            if (groupType == "broadcast")
                LOG_INFO("PCmpServer", "ADD_GROUP group=%s type=broadcast initiator=%s (floor 독점)",
                         groupId.c_str(), initiator.c_str());
        }

        // 초기 로스터의 멤버별 전용 포트 할당 (멱등 — 기존 유닛 재사용).
        //   client 는 각 멤버의 SDP offer 에 이 포트를 광고한다.
        SimpleJson::JsonNode memberPorts;
        memberPorts.type = SimpleJson::JSON_OBJECT;
        bool memberAllocFail = false;
        for (const auto& sid : memberIds) {
            PPttMemberPort* mu = ensureMemberUnit(groupId, sid, group);
            if (!mu) { memberAllocFail = true; break; }
            SimpleJson::JsonNode mp;
            mp.Set("port", (int)mu->getAudioPort());
            mp.Set("video_port", (int)mu->getVideoPort());
            memberPorts.Set(sid, mp);
        }
        if (memberAllocFail) {
            int txSeq = sendErr(ip, port, transId, cmdName, sesid, svc,
                                "NO_RESOURCE", "ptt member pool exhausted");
            logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "No Resource", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
            return;
        }

        SimpleJson::JsonNode respBody;
        respBody.Set("ip", sharedIp);
        respBody.Set("floor_port", sharedFloorPort);
        respBody.Set("member_ports", memberPorts);

        int txSeq = sendOk(ip, port, transId, cmdName, sesid, svc, &respBody);
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), subid.c_str(), txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", respBody.ToString().c_str());
    } else {
         int txSeq = sendErr(ip, port, transId, cmdName, sesid, svc,
                             "NO_RESOURCE", "ptt pool exhausted");
         logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "No Resource", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
    }
}

void PCmpServer::processJoinGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");
    std::string userIp = payload.GetString("user_ip");
    int userPort = (int)payload.GetInt("user_port");
    int userFloorPort = (int)payload.GetInt("user_floor_port");
    int userVideoPort = (int)payload.GetInt("user_video_port");
    std::string role = payload.GetString("role");
    if (role.empty()) role = "participant";

    // sweeper 와의 _sesidMap/_serviceMap 동시 접근 방지 — map 접근 전에 lock
    PAutoLock lock(_mutex);

    // sesid: payload > 캐시 > 신규 발행
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto it = _sesidMap.find(groupId);
        if (it != _sesidMap.end()) sesid = it->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    _sesidMap[groupId] = sesid;
    // service: payload > 캐시 > mcptt fallback
    std::string svc = payload.GetString("service");
    if (svc.empty()) {
        auto it = _serviceMap.find(groupId);
        if (it != _serviceMap.end()) svc = it->second;
    }
    if (svc.empty()) svc = "mcptt";
    _serviceMap[groupId] = svc;

    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(groupId, "csp", "cmp", "JSON", "PTT_JOIN", sessionId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    if (_groups.find(groupId) != _groups.end()) {
        PMcpttGroup* group = _groups[groupId];
        // 멤버 전용 포트 유닛 (멱등) — ① user_ip 없는 선할당 호출은 포트만 응답,
        //   ② SDP answer 후 user_ip/port 동반 호출로 멤버 등록/주소 갱신.
        PPttMemberPort* mu = ensureMemberUnit(groupId, sessionId, group);
        if (!mu) {
            int txSeq = sendErr(ip, port, transId, "PTT_JOIN", sesid, svc,
                                "NO_RESOURCE", "ptt member pool exhausted");
            logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "No Resource", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
            return;
        }
        if (!userIp.empty() && userPort > 0) {
            int userNat = (int)payload.GetInt("user_nat", 0);
            std::string userSigIp = payload.GetString("user_sig_ip");
            group->addMember(sessionId, userIp, userPort, userFloorPort, userVideoPort, role, mu,
                             userNat != 0, userSigIp);
            // condition tier(emergency/imminent) 동반 시 반영 (CSP 가 긴급 멤버 join 시 전달)
            std::string tierStr = payload.GetString("tier");
            if (!tierStr.empty()) group->setTier(sessionId, ParseFloorTier(tierStr));
        }

        SimpleJson::JsonNode respBody;
        respBody.Set("ip", _rtpIp);
        respBody.Set("port", (int)mu->getAudioPort());
        respBody.Set("video_port", (int)mu->getVideoPort());

        int txSeq = sendOk(ip, port, transId, "PTT_JOIN", sesid, svc, &respBody);
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", respBody.ToString().c_str());
        LOG_INFO("PCmpServer", "PTT_JOIN group=%s session=%s %s:%d local=%d", groupId.c_str(), sessionId.c_str(), userIp.c_str(), userPort, mu->getAudioPort());
    } else {
        int txSeq = sendErr(ip, port, transId, "PTT_JOIN", sesid, svc,
                            "NOT_FOUND", "group not found");
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "PTT_JOIN group=%s not found", groupId.c_str());
    }
}

void PCmpServer::processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");

    // sweeper 와의 _sesidMap/_serviceMap 동시 접근 방지 — map 접근 전에 lock
    PAutoLock lock(_mutex);

    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto it = _sesidMap.find(groupId);
        if (it != _sesidMap.end()) sesid = it->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    std::string svc = payload.GetString("service");
    if (svc.empty()) {
        auto it = _serviceMap.find(groupId);
        if (it != _serviceMap.end()) svc = it->second;
    }
    if (svc.empty()) svc = "mcptt";

    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(groupId, "csp", "cmp", "JSON", "PTT_LEAVE", sessionId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    if (_groups.find(groupId) != _groups.end()) {
        PMcpttGroup* group = _groups[groupId];
        group->removeMember(sessionId);
        freeMemberUnit(groupId, sessionId);

        int txSeq = sendOk(ip, port, transId, "PTT_LEAVE", sesid, svc);
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", "OK");
        LOG_INFO("PCmpServer", "PTT_LEAVE group=%s session=%s", groupId.c_str(), sessionId.c_str());
    } else {
        int txSeq = sendErr(ip, port, transId, "PTT_LEAVE", sesid, svc,
                            "NOT_FOUND", "group not found");
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "PTT_LEAVE group=%s not found", groupId.c_str());
    }
}

void PCmpServer::processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");

    // sweeper 와의 _sesidMap/_serviceMap 동시 접근 방지 — map 접근 전에 lock
    PAutoLock lock(_mutex);

    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto it = _sesidMap.find(groupId);
        if (it != _sesidMap.end()) sesid = it->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    std::string svc = payload.GetString("service");
    if (svc.empty()) {
        auto it = _serviceMap.find(groupId);
        if (it != _serviceMap.end()) svc = it->second;
    }
    if (svc.empty()) svc = "mcptt";

    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(groupId, "csp", "cmp", "JSON", "PTT_GROUP_REMOVE", groupId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    if (_groups.find(groupId) != _groups.end()) {
        PMcpttGroup* group = _groups[groupId];
        logFlow(groupId, "cmp", "cmp", "INT", "GROUP_END", "", "", svc.c_str(), sesid.c_str());
        // PTT 리소스 반환 (그룹 floor + 멤버 유닛 전체)
        PRtpMulticast* ptt = group->getPttSession();
        if (ptt) { ptt->reset(); freePttResource(ptt); }
        freeGroupMemberUnits(groupId);
        delete group;
        _groups.erase(groupId);

        int txSeq = sendOk(ip, port, transId, "PTT_GROUP_REMOVE", sesid, svc);
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", "OK");
        LOG_INFO("PCmpServer", "PTT_GROUP_REMOVE group=%s", groupId.c_str());
    } else {
        int txSeq = sendErr(ip, port, transId, "PTT_GROUP_REMOVE", sesid, svc,
                            "NOT_FOUND", "group not found");
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "PTT_GROUP_REMOVE group=%s not found", groupId.c_str());
    }

    // 그룹 종료 후 캐시 정리
    _sesidMap.erase(groupId);
    _serviceMap.erase(groupId);
    _groupSubId.erase(groupId);
}

void PCmpServer::processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
     LOG_DEBUG("PCmpServer", "PTT_GROUP_MODIFY -> delegating to processAddGroup");
     processAddGroup(payload, ip, port, transId);
}

// PTT_FLOOR_TIER {group_id, session_id, tier} — 멤버의 condition tier(emergency/imminent/normal)
// 런타임 갱신. Phase 2 CSP 가 긴급 개시/업그레이드/취소 시 호출. 미디어 재협상 불필요(floor 만).
void PCmpServer::processSetFloorTier(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId   = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");
    std::string tierStr   = payload.GetString("tier");
    std::string txIdStr   = std::to_string(transId);
    std::string peerStr   = ip + ":" + std::to_string(port);

    // sweeper 와의 _sesidMap/_serviceMap 동시 접근 방지 — map 접근 전에 lock
    PAutoLock lock(_mutex);

    // sesid/service: payload > 그룹 캐시 > 발행/mcptt (다른 그룹 명령과 동일 규칙)
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) {
        auto its = _sesidMap.find(groupId);
        if (its != _sesidMap.end()) sesid = its->second;
    }
    if (sesid.empty()) sesid = issueSesid("");
    std::string svc = payload.GetString("service");
    if (svc.empty()) {
        auto itv = _serviceMap.find(groupId);
        if (itv != _serviceMap.end()) svc = itv->second;
    }
    if (svc.empty()) svc = "mcptt";

    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    logFlow(groupId, "csp", "cmp", "JSON", "PTT_FLOOR_TIER",
            (sessionId + " tier=" + tierStr).c_str(), txIdStr.c_str(),
            svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    auto it = _groups.find(groupId);
    if (it != _groups.end() && !sessionId.empty()) {
        it->second->setTier(sessionId, ParseFloorTier(tierStr));
        int txSeq = sendOk(ip, port, transId, "PTT_FLOOR_TIER", sesid, svc);
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_INFO("PCmpServer", "PTT_FLOOR_TIER group=%s session=%s tier=%s", groupId.c_str(), sessionId.c_str(), tierStr.c_str());
    } else {
        int txSeq = sendErr(ip, port, transId, "PTT_FLOOR_TIER", sesid, svc,
                            "NOT_FOUND", "group not found");
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "PTT_FLOOR_TIER group=%s not found", groupId.c_str());
    }
}

// flat dot-path key → root 중첩 경로에 set (CSP 와 동일 overlay 규칙).
static void _cmpSetByDotPath(SimpleJson::JsonNode& parent, const std::string& dotPath,
                             const SimpleJson::JsonNode& value) {
    size_t pos = dotPath.find('.');
    if (pos == std::string::npos) {
        parent.Set(dotPath, value);
        return;
    }
    std::string head = dotPath.substr(0, pos);
    std::string rest = dotPath.substr(pos + 1);
    SimpleJson::JsonNode sub = parent.Has(head) ? parent.Get(head) : SimpleJson::JsonNode();
    if (sub.type != SimpleJson::JSON_OBJECT) {
        sub = SimpleJson::JsonNode();
        sub.type = SimpleJson::JSON_OBJECT;
    }
    _cmpSetByDotPath(sub, rest, value);
    parent.Set(head, sub);
}

void PCmpServer::loadConfig() {
    std::ifstream t(_configFile);
    if (!t.is_open()) {
        if (_configFile.find(".json") != std::string::npos) {
             LOG_ERROR("PCmpServer", "Failed to open config file: %s", _configFile.c_str());
             return;
        }
    }

    // Check extension
    if (_configFile.substr(_configFile.find_last_of(".") + 1) == "json") {
        std::stringstream buffer;
        buffer << t.rdbuf();
        std::string jsonContent = buffer.str();

        SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(jsonContent);

        // Deployment overlay: <cmp.json 디렉토리>/../../config.json (install_path/config.json).
        // flat key → nested 로 merge. CIMS_DEPLOYMENT_CONFIG 환경변수가 우선.
        do {
            std::string overlayPath;
            if (const char* env = getenv("CIMS_DEPLOYMENT_CONFIG")) {
                if (*env) overlayPath = env;
            }
            if (overlayPath.empty()) {
                std::string dir = _configFile;
                size_t s = dir.find_last_of('/');
                if (s != std::string::npos) dir = dir.substr(0, s);
                std::string cand = dir + "/../../config.json";
                std::ifstream f(cand);
                if (f) overlayPath = cand;
            }
            if (overlayPath.empty()) break;
            std::ifstream of(overlayPath);
            if (!of) break;
            std::stringstream ob; ob << of.rdbuf();
            SimpleJson::JsonNode over = SimpleJson::JsonNode::Parse(ob.str());
            if (over.type != SimpleJson::JSON_OBJECT) break;
            int applied = 0;
            for (const auto& kv : over.objects) {
                _cmpSetByDotPath(root, kv.first, kv.second);
                ++applied;
            }
            LOG_INFO("PCmpServer", "overlay applied: %s (%d keys)",
                     overlayPath.c_str(), applied);
        } while (false);
        
        if (root.Has("RtpStartPort")) _rtpStartPort = (int)root.GetInt("RtpStartPort");
        if (root.Has("RtpPoolSize")) _rtpPoolSize = (int)root.GetInt("RtpPoolSize");
        if (root.Has("RtpIp")) _rtpIp = root.GetString("RtpIp");
        if (root.Has("ServerIp")) _serverIp = root.GetString("ServerIp");
        if (root.Has("ServerPort")) _serverPort = (int)root.GetInt("ServerPort");
        
        if (root.Has("EnableDtmfPtt")) {
             SimpleJson::JsonNode val = root.Get("EnableDtmfPtt");
             std::string sVal = root.GetString("EnableDtmfPtt");
             if (sVal == "true") _dtmfPttEnable = true;
             else if (sVal == "false") _dtmfPttEnable = false;
             else _dtmfPttEnable = (root.GetInt("EnableDtmfPtt") != 0);
        }
        
        if (root.Has("DtmfPushDigit")) _dtmfPushDigit = root.GetString("DtmfPushDigit");
        if (root.Has("DtmfReleaseDigit")) _dtmfReleaseDigit = root.GetString("DtmfReleaseDigit");
        if (root.Has("SessionTimeout")) _sessionTimeout = (int)root.GetInt("SessionTimeout");
        if (root.Has("OrphanReclaimSec")) _orphanReclaimSec = (int)root.GetInt("OrphanReclaimSec");
        if (root.Has("FloorIdleSec")) _floorIdleSec = (int)root.GetInt("FloorIdleSec");
        if (root.Has("RtpWorkerCount")) {
            int w = (int)root.GetInt("RtpWorkerCount");
            if (w >= 1 && w <= 32) _rtpWorkerCount = w;
        }
        // PTT 리소스 풀 설정
        if (root.Has("PttRtpStartPort")) _pttRtpStartPort = (int)root.GetInt("PttRtpStartPort");
        if (root.Has("PttRtpPoolSize")) _pttRtpPoolSize = (int)root.GetInt("PttRtpPoolSize");
        if (root.Has("PttFloorStartPort")) _pttFloorStartPort = (int)root.GetInt("PttFloorStartPort");
        if (root.Has("PttVideoStartPort")) _pttVideoStartPort = (int)root.GetInt("PttVideoStartPort");
        if (root.Has("PttMemberPoolSize")) _pttMemberPoolSize = (int)root.GetInt("PttMemberPoolSize");
        
        // Log configuration
        std::string logDir = root.Has("LogDir") ? root.GetString("LogDir") : "";
        int logMaxSizeMB = root.Has("LogMaxSizeMB") ? (int)root.GetInt("LogMaxSizeMB") : 10;
        int logMaxFiles = root.Has("LogMaxFiles") ? (int)root.GetInt("LogMaxFiles") : 5;
        
        if (!logDir.empty()) {
            PLog::Instance().InitFile(logDir, "cmp", logMaxSizeMB, logMaxFiles);
        }
        
        if (root.Has("LogLevel")) {
            std::string lvl = root.GetString("LogLevel");
            if (lvl == "DEBUG" || lvl == "debug") PLog::Instance().SetLevel(CMP_LOG_DEBUG);
            else if (lvl == "WARN" || lvl == "warn") PLog::Instance().SetLevel(CMP_LOG_WARN);
            else if (lvl == "ERROR" || lvl == "error") PLog::Instance().SetLevel(CMP_LOG_ERROR);
            else PLog::Instance().SetLevel(CMP_LOG_INFO);
        }

    } else {
        // Legacy .conf loader
        FILE* fp = fopen(_configFile.c_str(), "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                char key[128], val[128];
                if (sscanf(line, "%[^=]=%s", key, val) == 2) {
                    if (strcmp(key, "RtpStartPort") == 0) _rtpStartPort = atoi(val);
                    if (strcmp(key, "RtpPoolSize") == 0) _rtpPoolSize = atoi(val);
                    if (strcmp(key, "RtpIp") == 0) _rtpIp = val;
                    if (strcmp(key, "ServerIp") == 0) _serverIp = val;
                    if (strcmp(key, "ServerPort") == 0) _serverPort = atoi(val);
                    if (strcmp(key, "EnableDtmfPtt") == 0) _dtmfPttEnable = strcmp(val, "true") == 0;
                    if (strcmp(key, "DtmfPushDigit") == 0) _dtmfPushDigit = val;
                    if (strcmp(key, "DtmfReleaseDigit") == 0) _dtmfReleaseDigit = val;
                }
            }
            fclose(fp);
        }
    }

    // Recording config
    _recordEnable = false;
    _recordDir = "recordings/raw";
    if (_configFile.substr(_configFile.find_last_of(".") + 1) == "json") {
        std::ifstream t2(_configFile);
        if (t2.is_open()) {
            std::stringstream buf2;
            buf2 << t2.rdbuf();
            SimpleJson::JsonNode root2 = SimpleJson::JsonNode::Parse(buf2.str());
            if (root2.Has("RecordEnable")) {
                std::string rv = root2.GetString("RecordEnable");
                _recordEnable = (rv == "true");
            }
            if (root2.Has("RecordDir")) _recordDir = root2.GetString("RecordDir");
            if (root2.Has("SegmentIntervalSec")) _segmentIntervalSec = root2.Get("SegmentIntervalSec").AsInt();
            // ServiceLogging 설정 (신규)
            if (root2.Has("ServiceLogging")) {
                SimpleJson::JsonNode sl = root2.Get("ServiceLogging");
                if (sl.Has("Dir")) _serviceLogDir = sl.GetString("Dir");
                // Flow 로깅 세부 flag: { "Flow": { "Floor": true, "Dtmf": true, "Rtcp": false } }
                if (sl.Has("Flow")) {
                    SimpleJson::JsonNode fl = sl.Get("Flow");
                    if (fl.Has("Floor")) _logFlowFloor = (fl.GetString("Floor") == "true" || fl.GetString("Floor") == "1");
                    if (fl.Has("Dtmf"))  _logFlowDtmf  = (fl.GetString("Dtmf")  == "true" || fl.GetString("Dtmf")  == "1");
                    if (fl.Has("Rtcp"))  _logFlowRtcp  = (fl.GetString("Rtcp")  == "true" || fl.GetString("Rtcp")  == "1");
                }
            }
            // 레거시 호환
            if (_serviceLogDir.empty() && root2.Has("ServiceLogDir"))
                _serviceLogDir = root2.GetString("ServiceLogDir");
            _msgLogDir = _serviceLogDir; // 통합 디렉토리
            if (root2.Has("SystemId")) _systemId = root2.GetString("SystemId");
            else _systemId = "cmp_01";
            // node 필드용: "cmp_01" → "cmp"
            _nodeName = _systemId;
            auto upos = _nodeName.find('_');
            if (upos != std::string::npos) _nodeName = _nodeName.substr(0, upos);
        }
    }

    if (_recordEnable) {
        std::string mkdirCmd = "mkdir -p " + _recordDir;
        system(mkdirCmd.c_str());
    }

    LOG_INFO("PCmpServer", "Config: VoIP(port=%d pool=%d 8/call) PTT(member rtp=%d video=%d pool=%d, group floor=%d pool=%d) Workers=%d RtpIp=%s ServerIp=%s:%d DtmfPtt=%d SessionTimeout=%d FloorIdleSec=%d",
           _rtpStartPort, _rtpPoolSize, _pttRtpStartPort, _pttVideoStartPort, _pttMemberPoolSize,
           _pttFloorStartPort, _pttRtpPoolSize,
           _rtpWorkerCount, _rtpIp.c_str(), _serverIp.c_str(), _serverPort,
           _dtmfPttEnable, _sessionTimeout, _floorIdleSec);
}

// ═══════════════════════════════════════════════════════════════
//  RTP epoll 리액터
// ═══════════════════════════════════════════════════════════════

// relay 의 소켓 fd 들을 워커 widx 의 epoll 에 등록. data.ptr=handler 로 역참조.
void PCmpServer::epollAddHandler(int widx, PHandler* h, const std::vector<int>& fds) {
    if (widx < 0 || widx >= (int)_reactors.size()) return;
    int epfd = _reactors[widx].epfd;
    if (epfd < 0) return;
    for (int fd : fds) {
        if (fd < 0) continue;
        struct epoll_event ev{};
        ev.events = EPOLLIN;           // level-triggered: proc() 가 미처리분 남겨도 다음 wait 가 재통지
        ev.data.ptr = h;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
            LOG_ERROR("PCmpServer", "epoll_ctl ADD fd=%d (reactor %d) failed: %s", fd, widx, strerror(errno));
    }
}

// 워커 스레드 본체: 트래픽 없으면 epoll_wait 블록(idle CPU 0). 패킷 도착 시 해당 relay 의 proc() 호출.
void PCmpServer::reactorLoop(int widx) {
    int epfd = _reactors[widx].epfd;
    if (epfd < 0) return;
    const int MAXEV = 256;
    struct epoll_event evs[MAXEV];
    while (_reactorRunning.load()) {
        int n = epoll_wait(epfd, evs, MAXEV, 1000);   // 1s timeout → 종료 플래그 재확인(클린 join)
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("PCmpServer", "epoll_wait reactor %d failed: %s", widx, strerror(errno));
            break;
        }
        if (n == 0) continue;
        // 한 relay 가 여러 fd 로 깨어날 수 있음 → proc() 가 relay 의 모든 소켓을 drain 하므로
        //   relay 당 1회만 호출(중복 제거). relay→워커 매핑이 고정이라 동일 relay 가 다른 스레드와 경합 없음.
        std::unordered_set<PHandler*> handled;
        for (int i = 0; i < n; ++i) {
            PHandler* h = static_cast<PHandler*>(evs[i].data.ptr);
            if (h && handled.insert(h).second) h->proc();
        }
    }
}

void PCmpServer::initResourcePool() {
    // leg 별 포트셋: 호당 8포트 (peer 별 audio/video RTP+RTCP 블록 × 2) — ue_nat_traversal.md §3.1
    int currentPort = _rtpStartPort;
    for (int i = 0; i < _rtpPoolSize; ++i) {
        std::string name = formatStr("InActiveRtp_%d", i);
        PRtpRelay* rtp = new PRtpRelay(name);

        if (rtp->init(_rtpIp, currentPort)) {
             // epoll 리액터에 소켓 fd 영구 등록 (소켓은 프로세스 종료까지 유지 → 1회 등록).
             int widx = i % _rtpWorkerCount;
             rtp->setWorkerName(formatStr("RtpWorker_%d", widx));
             std::vector<int> fds; rtp->collectFds(fds);
             epollAddHandler(widx, rtp, fds);
             _resourcePool.push_back(rtp);
             _freeResources.push_back(rtp);
        } else {
             LOG_ERROR("PCmpServer", "Failed to init resource on port %d", currentPort);
             delete rtp;
        }
        currentPort += 8;
    }
    LOG_INFO("PCmpServer", "VoIP pool: %lu resources (port %d-%d, 8/call)", _resourcePool.size(), _rtpStartPort, currentPort - 1);
}

void PCmpServer::initPttResourcePool() {
    // 그룹 자원 = 공유 floor 포트만. audio/video 는 멤버 유닛(initPttMemberPool)이 담당.
    int floorPort = _pttFloorStartPort;
    for (int i = 0; i < _pttRtpPoolSize; ++i) {
        std::string name = formatStr("PttFloor_%d", i);
        PRtpMulticast* ptt = new PRtpMulticast(name);

        if (ptt->init(_rtpIp, floorPort)) {
            int widx = i % _rtpWorkerCount;
            ptt->setWorkerName(formatStr("RtpWorker_%d", widx));
            std::vector<int> fds; ptt->collectFds(fds);
            epollAddHandler(widx, ptt, fds);
            _pttPool.push_back(ptt);
            _freePttResources.push_back(ptt);
        } else {
            LOG_ERROR("PCmpServer", "Failed to init PTT group resource floor=%d", floorPort);
            delete ptt;
        }
        floorPort += 2;
    }
    LOG_INFO("PCmpServer", "PTT group pool: %lu resources (floor %d-%d)",
             _pttPool.size(), _pttFloorStartPort, floorPort - 2);
}

void PCmpServer::initPttMemberPool() {
    int audioPort = _pttRtpStartPort;
    int videoPort = _pttVideoStartPort;
    for (int i = 0; i < _pttMemberPoolSize; ++i) {
        std::string name = formatStr("PttMember_%d", i);
        PPttMemberPort* mu = new PPttMemberPort(name);

        if (mu->init(_rtpIp, audioPort, videoPort)) {
            int widx = i % _rtpWorkerCount;
            mu->setWorkerName(formatStr("RtpWorker_%d", widx));
            std::vector<int> fds; mu->collectFds(fds);
            epollAddHandler(widx, mu, fds);
            _pttMemberPool.push_back(mu);
            _freePttMembers.push_back(mu);
        } else {
            LOG_ERROR("PCmpServer", "Failed to init PTT member unit audio=%d video=%d", audioPort, videoPort);
            delete mu;
        }
        audioPort += 2;
        videoPort += 2;
    }
    LOG_INFO("PCmpServer", "PTT member pool: %lu units (audio %d-%d, video %d-%d)",
             _pttMemberPool.size(), _pttRtpStartPort, audioPort - 2, _pttVideoStartPort, videoPort - 2);
}

PRtpRelay* PCmpServer::allocResource(std::string& rtpIp, int& rtpPort, int& videoPort) {
    if (_freeResources.empty()) {
        LOG_WARN("PCmpServer", "allocResource: no free resources");
        return NULL;
    }

    PRtpRelay* rtp = _freeResources.back();
    _freeResources.pop_back();

    rtpIp = _rtpIp;
    rtpPort = rtp->getLocalPort(0);
    videoPort = rtp->getLocalVideoPort(0);

    LOG_INFO("PCmpServer", "allocResource: peer0=%d peer1=%d (remaining %lu)", rtpPort, rtp->getLocalPort(1), _freeResources.size());
    return rtp;
}

void PCmpServer::freeResource(PRtpRelay* rtp) {
    if (rtp) {
        LOG_INFO("PCmpServer", "freeResource: port=%d", rtp->getLocalPort(0));
        _freeResources.push_back(rtp);
    }
}

PRtpMulticast* PCmpServer::allocPttResource(std::string& rtpIp, int& floorPort) {
    if (_freePttResources.empty()) {
        LOG_WARN("PCmpServer", "allocPttResource: no free PTT resources");
        return NULL;
    }
    PRtpMulticast* ptt = _freePttResources.back();
    _freePttResources.pop_back();
    rtpIp = _rtpIp;
    floorPort = ptt->getLocalFloorPort();
    LOG_INFO("PCmpServer", "allocPttResource: floor=%d (remaining %lu)", floorPort, _freePttResources.size());
    return ptt;
}

void PCmpServer::freePttResource(PRtpMulticast* ptt) {
    if (ptt) {
        LOG_INFO("PCmpServer", "freePttResource: floor=%d", ptt->getLocalFloorPort());
        _freePttResources.push_back(ptt);
    }
}

// 멤버 유닛 할당/해제 — (groupId, sessionId) 멱등 키. 호출자가 _mutex 보유.
PPttMemberPort* PCmpServer::ensureMemberUnit(const std::string& groupId, const std::string& sessionId, PMcpttGroup* group) {
    std::string key = groupId + "|" + sessionId;
    auto it = _memberUnits.find(key);
    if (it != _memberUnits.end()) return it->second;
    if (_freePttMembers.empty()) {
        LOG_WARN("PCmpServer", "ensureMemberUnit: member pool exhausted (group=%s session=%s)", groupId.c_str(), sessionId.c_str());
        return NULL;
    }
    PPttMemberPort* mu = _freePttMembers.back();
    _freePttMembers.pop_back();
    mu->bind(group, sessionId);
    _memberUnits[key] = mu;
    LOG_INFO("PCmpServer", "ensureMemberUnit: group=%s session=%s audio=%d video=%d (remaining %lu)",
             groupId.c_str(), sessionId.c_str(), mu->getAudioPort(), mu->getVideoPort(), _freePttMembers.size());
    return mu;
}

void PCmpServer::freeMemberUnit(const std::string& groupId, const std::string& sessionId) {
    auto it = _memberUnits.find(groupId + "|" + sessionId);
    if (it == _memberUnits.end()) return;
    it->second->reset();
    _freePttMembers.push_back(it->second);
    _memberUnits.erase(it);
}

void PCmpServer::freeGroupMemberUnits(const std::string& groupId) {
    std::string prefix = groupId + "|";
    for (auto it = _memberUnits.begin(); it != _memberUnits.end(); ) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it->second->reset();
            _freePttMembers.push_back(it->second);
            it = _memberUnits.erase(it);
        } else {
            ++it;
        }
    }
}

void PCmpServer::timeoutLoop() {
    int tick = 0;
    while (_running) {
        msleep(1000);
        if (!_running) break;
        ++tick;

        // PTT floor 무활동 회수 — 매 초 점검(idleSec 해상도). owner 가 RELEASE 없이 RTP 를
        //   멈추면(검증 마지막 발언자 등) floor 강제 해제. _mutex 보유 중 호출 → 그룹 삭제 방지.
        //   checkFloorInactivity 는 그룹 자체 mutex 만 잡고 서버 _mutex 를 역으로 잡지 않으므로 안전.
        if (_floorIdleSec > 0) {
            PAutoLock lock(_mutex);
            for (auto const& [gid, group] : _groups) {
                if (group) group->checkFloorInactivity(_floorIdleSec);
            }
        }

        // 무거운 세션/그룹 sweep 은 60초마다
        if (tick % 60 != 0) continue;

        time_t now;
        time(&now);

        // Stale 개별 세션 정리. (sid, bGotRtp, heldSec) — bGotRtp=RTP 받은 적 있음(hold_timeout) vs 무RTP(orphan).
        std::vector<std::tuple<std::string, bool, int>> staleSessions;
        {
            PAutoLock lock(_mutex);
            for (auto const& [sid, rtp] : _sessions) {
                if (!rtp) continue;
                // 고아(RTP 무수신 = setup 실패/실패호) relay 는 짧게(_orphanReclaimSec) 회수,
                // RTP 받은 적 있는 호(활성/홀드)는 기존 _sessionTimeout 유지 → 홀드 오회수 방지.
                // touchActivity 가 RTP 수신 시에만 호출되므로 (now - lastActivity) = RTP 무수신 경과.
                bool bGotRtp = rtp->everReceivedRtp();
                int to = bGotRtp ? _sessionTimeout : _orphanReclaimSec;
                int idle = (int)(now - rtp->getLastActivityTime());
                if (idle >= to) {
                    staleSessions.emplace_back(sid, bGotRtp, idle);
                }
            }
        }
        for (const auto& [sid, bGotRtp, heldSec] : staleSessions) {
            // 누수 회수: owner(CSP)가 REMOVE 를 안 보낸 relay 를 sweeper 가 회수. RtpMap fix 후 이 경로 발동은
            //   비정상 신호(CSP crash/BYE 누락=hold_timeout, setup 실패=orphan_no_rtp). 카운터+상세기록으로 관측.
            const char* reason = bGotRtp ? "hold_timeout" : "orphan_no_rtp";
            LOG_INFO("PCmpServer", "Leak reclaim: session=%s reason=%s held=%ds — sweeper auto cleanup", sid.c_str(),
                     reason, heldSec);
            PAutoLock lock(_mutex);
            auto it = _sessions.find(sid);
            if (it != _sessions.end()) {
                PRtpRelay* rtp = it->second;
                std::string sesid = _sesidMap.count(sid) ? _sesidMap[sid] : issueSesid("");
                std::string svc = _serviceMap.count(sid) ? _serviceMap[sid] : "volte";
                logFlow(sid, "cmp", "cmp", "INT", "SESSION_TIMEOUT", reason, "", svc.c_str(), sesid.c_str());
                _leakReclaimTotal++;
                if (bGotRtp) _leakReclaimHold++; else _leakReclaimOrphan++;
                writeLeakReclaim(sid, sesid, svc, reason, heldSec);
                rtp->reset();
                freeResource(rtp);
                _sessions.erase(it);
                _sesidMap.erase(sid);
                _serviceMap.erase(sid);
            }
        }

        // Stale 그룹 세션 정리 (공유 RTP에 패킷이 없는 그룹)
        std::vector<std::string> staleGroupIds;
        {
            PAutoLock lock(_mutex);
            for (auto const& [gid, group] : _groups) {
                PRtpMulticast* pttSess = group->getPttSession();
                if (pttSess && group->getMemberCount() == 0 &&
                    (now - pttSess->getLastActivityTime()) >= _sessionTimeout) {
                    staleGroupIds.push_back(gid);
                }
            }
        }
        for (const auto& gid : staleGroupIds) {
            LOG_INFO("PCmpServer", "Group timeout: group=%s (no members, no activity) — auto cleanup", gid.c_str());
            PAutoLock lock(_mutex);
            auto it = _groups.find(gid);
            if (it != _groups.end()) {
                std::string sesid = _sesidMap.count(gid) ? _sesidMap[gid] : issueSesid("");
                std::string svc = _serviceMap.count(gid) ? _serviceMap[gid] : "mcptt";
                logFlow(gid, "cmp", "cmp", "INT", "GROUP_TIMEOUT", "",
                        "", svc.c_str(), sesid.c_str());
                // PTT 리소스 free pool 반환 (removeGroup 와 동일 패턴) — 누락 시 누적 leak
                PRtpMulticast* ptt = it->second->getPttSession();
                if (ptt) { ptt->reset(); freePttResource(ptt); }
                freeGroupMemberUnits(gid);
                delete it->second;
                _groups.erase(it);
                _sesidMap.erase(gid);
                _serviceMap.erase(gid);
                _groupSubId.erase(gid);
            }
        }
    }
}

// ── 통합 Flow 로그 ──────────────────────────────────────────────

std::string PCmpServer::getTimestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06ld",
             t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000);
    return buf;
}

std::string PCmpServer::getFlowHourDir() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d",
             _serviceLogDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    return buf;
}

bool PCmpServer::mkdirP(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return true;
    size_t pos = p.rfind('/');
    if (pos != std::string::npos && pos > 0) mkdirP(p.substr(0, pos));
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string PCmpServer::bucketSuffix() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", (t.tm_min / 5) * 5);  // 00,05,10,...,55
    return buf;
}

std::string PCmpServer::flowFilePath() {
    if (_currentFlowHourDir.empty()) return "";
    return _currentFlowHourDir + "/" + _systemId + ".flow." + bucketSuffix() + ".jsonl";
}

std::string PCmpServer::msgFilePath() {
    if (_currentMsgHourDir.empty()) return "";
    return _currentMsgHourDir + "/" + _systemId + "_csp.msg." + bucketSuffix() + ".jsonl";
}

std::string PCmpServer::bodyFilePath() {
    if (_currentMsgHourDir.empty()) return "";
    return _currentMsgHourDir + "/" + _systemId + "_csp." + bucketSuffix() + ".jsonl";
}

int PCmpServer::countFileLines(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int n = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) n++;
    fclose(f);
    return n;
}

// 5분 버킷 회전 — 파일 핸들을 유지하지 않고(open-per-write) 디렉터리만 보장하고,
//   버킷이 바뀌면 _msgSeq 를 '미계수(-1)' 로 리셋(다음 write 가 기존 줄 수를 lazy 계수해 이어붙임).
//   (구버전: 시간당 파일을 내내 열어둠 → 운영 중 로그삭제 시 .nfs 고아·데이터유실 + 대용량검색. CSP SipMessageLogger 와 동일 전환.)
void PCmpServer::ensureBucket() {
    std::string hourDir = getFlowHourDir();
    std::string bucketKey = hourDir + "/" + bucketSuffix();
    if (bucketKey == _currentBucketKey) return;
    _currentBucketKey = bucketKey;
    mkdirP(hourDir);
    _currentFlowHourDir = hourDir;
    _currentMsgHourDir  = hourDir;   // flow/msg 통합 디렉터리
    _msgSeq = -1;                    // 새 버킷 → 다음 write 가 lazy 계수
}

static std::string _jsonEsc(const char* s) {
    if (!s) return "";
    std::string r;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            default:   r += *p;
        }
    }
    return r;
}

int PCmpServer::writeMsgLine(const char* ts, const char* dir, const char* peer, const char* proto, const char* msg,
                              const char* caller, const char* callee) {
    if (!msg || !msg[0]) return 0;
    std::lock_guard<std::mutex> lk(_logMtx);  // ensureBucket+seq+format+enqueue 직렬화
    ensureBucket();
    std::string path = msgFilePath();
    if (path.empty()) return 0;
    if (_msgSeq < 0) _msgSeq = countFileLines(path);  // 버킷 첫 write: 기존 줄 수 계수(재기동 연속성)
    _msgSeq++;
    int seq = _msgSeq;
    // 순서: ts, dir, peer, caller, callee, proto, msg (빈값 key 생략)
    std::string line = "{\"ts\":\"";
    line += ts ? ts : "";
    line += "\",\"dir\":\"";
    line += dir ? dir : "";
    line += "\",\"peer\":\"";
    line += peer ? peer : "";
    line += "\"";
    if (caller && caller[0]) { line += ",\"caller\":\""; line += _jsonEsc(caller); line += "\""; }
    if (callee && callee[0]) { line += ",\"callee\":\""; line += _jsonEsc(callee); line += "\""; }
    line += ",\"proto\":\"";
    line += proto ? proto : "";
    line += "\",\"msg\":\"";
    line += _jsonEsc(msg);
    line += "\"}\n";
    enqueueLine(path, std::move(line));  // NFS I/O 없이 즉시 반환
    return seq;
}

void PCmpServer::logFlow(const std::string& key, const char* from, const char* to,
                         const char* proto, const char* label, const char* detail,
                         const char* txId, const char* service,
                         const char* sesid, const char* subid,
                         int seq, const char* iface,
                         const char* caller, const char* callee) {
    if (_serviceLogDir.empty()) return;

    std::lock_guard<std::mutex> lk(_logMtx);  // ensureBucket+format+enqueue 직렬화
    ensureBucket();
    std::string flowPath = flowFilePath();
    if (flowPath.empty()) return;

    // service: 파라미터 > _serviceMap 저장값 (추론 코드 제거)
    std::string svc = (service && service[0]) ? service : "";
    if (svc.empty()) {
        auto it = _serviceMap.find(key);
        if (it != _serviceMap.end()) svc = it->second;
    }

    std::string ts = getTimestamp();

    // sesid: 파라미터 > _sesidMap > 공백
    std::string actualSesid = (sesid && sesid[0]) ? sesid : "";
    if (actualSesid.empty()) {
        auto itSes = _sesidMap.find(key);
        if (itSes != _sesidMap.end()) actualSesid = itSes->second;
    }

    // subid: 파라미터 > _groupSubId 캐시 fallback
    std::string actualSubid = (subid && subid[0]) ? subid : "";
    if (actualSubid.empty()) {
        auto itSub = _groupSubId.find(key);
        if (itSub != _groupSubId.end()) actualSubid = itSub->second;
    }

    // 순서: ts, service, caller, callee, sesid, subid, node, from, to, proto, method, detail, mid, seq, iface
    std::string line = "{";
    bool bFirst = true;
    auto emit = [&](const char* kname, const std::string& val, bool isNum = false, int iNum = 0) {
        if (!isNum && val.empty()) return;
        if (!bFirst) line += ",";
        bFirst = false;
        if (isNum) {
            char nb[64];
            snprintf(nb, sizeof(nb), "\"%s\":%d", kname, iNum);
            line += nb;
        } else {
            line += "\"";
            line += kname;
            line += "\":\"";
            line += val;
            line += "\"";
        }
    };

    emit("ts",      ts);
    emit("service", svc);
    emit("caller",  caller ? _jsonEsc(caller) : "");
    emit("callee",  callee ? _jsonEsc(callee) : "");
    emit("sesid",   _jsonEsc(actualSesid.c_str()));
    emit("subid",   _jsonEsc(actualSubid.c_str()));
    emit("node",    _nodeName);
    emit("from",    from ? std::string(from) : "");
    emit("to",      to ? std::string(to) : "");
    emit("proto",   proto ? std::string(proto) : "");
    emit("method",  label ? _jsonEsc(label) : "");
    emit("detail",  detail ? _jsonEsc(detail) : "");
    emit("mid",     txId ? _jsonEsc(txId) : "");
    if (seq > 0) emit("seq", "", true, seq);
    emit("iface",   iface ? std::string(iface) : "");
    line += "}\n";
    enqueueLine(flowPath, std::move(line));  // NFS I/O 없이 즉시 반환
}

// 누수 회수 세션 상세를 {ServiceLogDir}/leak_reclaim/YYYY/MM/DD/reclaim.jsonl 에 한 줄 기록(open-append-close).
//   발동 빈도가 낮아(정상 환경 0) 매 회수마다 open/close 비용은 무시 가능. 콘솔/OAM 이 이 파일을 조회.
void PCmpServer::writeLeakReclaim(const std::string& sessionId, const std::string& sesid, const std::string& service,
                                  const char* reason, int heldSec) {
    if (_serviceLogDir.empty()) return;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/leak_reclaim/%04d/%02d/%02d", _serviceLogDir.c_str(), t.tm_year + 1900,
             t.tm_mon + 1, t.tm_mday);
    mkdirP(dir);  // 디렉터리만 동기 생성(드물게 발동) — 실제 기록은 비동기 writer
    std::string path = std::string(dir) + "/reclaim.jsonl";
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"ts\":\"%s\",\"node\":\"%s\",\"session_id\":\"%s\",\"sesid\":\"%s\",\"service\":\"%s\","
             "\"reason\":\"%s\",\"held_sec\":%d}\n",
             getTimestamp().c_str(), _nodeName.c_str(), _jsonEsc(sessionId.c_str()).c_str(),
             _jsonEsc(sesid.c_str()).c_str(), _jsonEsc(service.c_str()).c_str(), reason, heldSec);
    enqueueLine(path, std::string(buf));
}

std::string PCmpServer::getMsgHourDir() {
    if (_msgLogDir.empty()) return "";
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/%04d/%02d/%02d/%02d",
             _msgLogDir.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    return buf;
}

void PCmpServer::logBody(const char* dir, const char* peer, const char* proto, const char* msg) {
    if (_msgLogDir.empty()) return;

    std::lock_guard<std::mutex> lk(_logMtx);  // ensureBucket+format+enqueue 직렬화
    ensureBucket();
    std::string path = bodyFilePath();   // {systemId}_csp.{mm5}.jsonl
    if (path.empty()) return;

    std::string ts = getTimestamp();
    std::string escaped = _jsonEsc(msg);

    std::string line = "{\"ts\":\"";
    line += ts;
    line += "\",\"dir\":\"";
    line += dir ? dir : "";
    line += "\",\"peer\":\"";
    line += peer ? peer : "";
    line += "\",\"proto\":\"";
    line += proto ? proto : "";
    line += "\",\"msg\":\"";
    line += escaped;
    line += "\"}\n";
    enqueueLine(path, std::move(line));  // NFS I/O 없이 즉시 반환
}

// ─────────────────────────────────────────────────────────────────────────────
// 비동기 배치 writer (CSP SipMessageLogger 와 동일 패턴)
//   생산자(writeMsgLine/logFlow/logBody/writeLeakReclaim)는 _logMtx 보유 중 포맷한 한 줄을
//   enqueueLine 으로 적재만 하고 즉시 반환(NFS I/O 없음). 단일 control 스레드가 매 control
//   패킷마다 NFS open/append/close 로 막히던 HOL 블로킹 제거. 단일 writer 스레드가 주기/임계마다
//   큐를 비워 파일경로별로 라인을 합쳐 경로당 1회 open→append→close. 단일 writer + FIFO 라
//   파일 내 줄 순서 = enqueue(=seq) 순서와 일치 → flow.seq ↔ msg 줄번호 cross-reference 정합 유지.
// ─────────────────────────────────────────────────────────────────────────────
void PCmpServer::enqueueLine(const std::string& path, std::string&& line) {
    if (path.empty() || line.empty()) return;
    bool bNotify = false;
    {
        std::lock_guard<std::mutex> lk(_logQMtx);
        if (_logQueue.size() >= kCmpLogMaxQueue) {
            // backlog 상한 초과(예: NFS 무응답) — 가장 오래된 줄을 버려 메모리 폭주 방지.
            _logQueue.pop_front();
            _logDropped.fetch_add(1);
        }
        _logQueue.push_back(LogItem{path, std::move(line)});
        if (_logQueue.size() >= kCmpLogNotifyThreshold) bNotify = true;
    }
    if (bNotify) _logQCv.notify_one();
}

void PCmpServer::flushLogBatch(std::deque<LogItem>& batch) {
    if (batch.empty()) return;
    // 파일경로별로 라인을 이어붙인다. 같은 경로의 줄은 batch 순서(=enqueue/seq 순서)대로 누적.
    std::unordered_map<std::string, std::string> groups;
    for (auto& item : batch) {
        auto it = groups.find(item.path);
        if (it == groups.end())
            groups.emplace(std::move(item.path), std::move(item.line));
        else
            it->second += item.line;
    }
    batch.clear();
    // 경로당 1회 open→append→close (서로 다른 파일끼리는 순서 무관).
    for (auto& kv : groups) {
        FILE* f = fopen(kv.first.c_str(), "a");
        if (!f) continue;
        fwrite(kv.second.data(), 1, kv.second.size(), f);
        fclose(f);
    }
}

void PCmpServer::logWriterLoop() {
    const auto interval = std::chrono::milliseconds(kCmpLogFlushIntervalMs);
    while (_logWriterRunning.load()) {
        std::deque<LogItem> batch;
        {
            std::unique_lock<std::mutex> lk(_logQMtx);
            _logQCv.wait_for(lk, interval, [this] { return !_logQueue.empty() || !_logWriterRunning.load(); });
            batch.swap(_logQueue);
        }
        flushLogBatch(batch);  // 락 밖에서 NFS I/O
    }
    // 종료 — 잔여 큐 전량 flush
    for (;;) {
        std::deque<LogItem> batch;
        {
            std::lock_guard<std::mutex> lk(_logQMtx);
            batch.swap(_logQueue);
        }
        if (batch.empty()) break;
        flushLogBatch(batch);
    }
}

void PCmpServer::startLogWriter() {
    bool bExpected = false;
    if (_logWriterRunning.compare_exchange_strong(bExpected, true)) {
        _logWriterThread = std::thread(&PCmpServer::logWriterLoop, this);
    }
}

void PCmpServer::stopLogWriter() {
    if (_logWriterRunning.exchange(false)) {
        _logQCv.notify_all();
        if (_logWriterThread.joinable()) _logWriterThread.join();
    }
}
