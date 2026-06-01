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

PCmpServer::PCmpServer(const std::string& name, const std::string& configFile)
    : PModule(name), _running(false), _udpFd(-1), _configFile(configFile), _sessionTimeout(600), _rtpWorkerCount(4),
      _pttRtpStartPort(52000), _pttRtpPoolSize(10), _pttFloorStartPort(54000), _pttVideoStartPort(56000), _segmentIntervalSec(60),
      _flowFile(nullptr), _msgFile(nullptr), _msgSeq(0), _lastRxSeq(0), _bodyFile(nullptr),
      _logFlowFloor(true), _logFlowDtmf(true), _logFlowRtcp(false)
{
    loadConfig();

    // Worker 스레드를 먼저 생성해야 initResourcePool()의 addHandler()가 동작함
    for(int i=0; i<_rtpWorkerCount; ++i) {
        std::string wname = formatStr("RtpWorker_%d", i);
        addWorker(wname, 1, 2048, true);
    }

    initResourcePool();
    initPttResourcePool();
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
    std::thread([this]() {
        this->runControlLoop();
    }).detach();

    // Session timeout 체크 스레드 시작
    if (_sessionTimeout > 0) {
        _timeoutThread = std::thread([this]() { this->timeoutLoop(); });
        LOG_INFO("PCmpServer", "Session timeout thread started (timeout=%ds)", _sessionTimeout);
    }

    LOG_INFO("PCmpServer", "Server listening on %s:%d", _serverIp.c_str(), _serverPort);
    return true;
}

void PCmpServer::stopServer() {
    _running = false;
    if (_timeoutThread.joinable()) _timeoutThread.join();
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
    
    // Parse JSON Wrapper
    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(strPacket);
    
    // Check trans_id and payload
    if (root.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("PCmpServer", "Invalid JSON Packet from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        return;
    }

    int transId = (int)root.GetInt("trans_id", 0);
    SimpleJson::JsonNode payload = root.Get("payload");
    
    if (payload.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("PCmpServer", "Missing Payload from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        return;
    }

    // Extract CMD
    std::string cmd = payload.GetString("cmd");
    // Normalize to uppercase for matching
    std::string cmdUpper = cmd;
    std::transform(cmdUpper.begin(), cmdUpper.end(), cmdUpper.begin(), ::toupper);

    // 원문 기록 (수신)
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string ts = getTimestamp();
    _lastRxSeq = writeMsgLine(ts.c_str(), "RX", peerStr.c_str(), "JSON", strPacket.c_str());

    // Dispatch
    LOG_DEBUG("PCmpServer", "Dispatching cmd=%s transId=%d from %s:%d", cmd.c_str(), transId, ip.c_str(), port);
    if (cmdUpper == "ADD_SESSION" || cmdUpper == "ADD") processAdd(payload, ip, port, transId);
    else if (cmdUpper == "REMOVE_SESSION" || cmdUpper == "REMOVE") processRemove(payload, ip, port, transId);
    else if (cmdUpper == "HEARTBEAT" || cmdUpper == "ALIVE") processAlive(payload, ip, port, transId);
    else if (cmdUpper == "ADD_PTT_GROUP" || cmdUpper == "ADD_GROUP" || cmdUpper == "ADDGROUP") processAddGroup(payload, ip, port, transId);
    else if (cmdUpper == "JOIN_PTT_GROUP" || cmdUpper == "JOIN_GROUP" || cmdUpper == "JOINGROUP") processJoinGroup(payload, ip, port, transId);
    else if (cmdUpper == "LEAVE_PTT_GROUP" || cmdUpper == "LEAVE_GROUP" || cmdUpper == "LEAVEGROUP") processLeaveGroup(payload, ip, port, transId);
    else if (cmdUpper == "REMOVE_PTT_GROUP" || cmdUpper == "REMOVE_GROUP" || cmdUpper == "REMOVEGROUP") processRemoveGroup(payload, ip, port, transId);
    else if (cmdUpper == "MODIFY_PTT_GROUP" || cmdUpper == "MODIFY_GROUP") processModifyGroup(payload, ip, port, transId);
    else if (cmdUpper == "MODIFY_SESSION" || cmdUpper == "MODIFY") processModify(payload, ip, port, transId);
    else if (cmdUpper == "STATS_REQUEST" || cmdUpper == "STATS") processStats(payload, ip, port, transId);
    else {
        LOG_WARN("PCmpServer", "Unknown CMD: %s from %s:%d", cmd.c_str(), ip.c_str(), port);
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Unknown Command");
        sendResponse(ip, port, resp.ToString());
    }
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

std::string PCmpServer::getOrIssueSesid(const std::string& key, const std::string& caller) {
    if (!key.empty()) {
        auto it = _sesidMap.find(key);
        if (it != _sesidMap.end() && !it->second.empty()) return it->second;
    }
    std::string sid = issueSesid(caller);
    if (!key.empty()) _sesidMap[key] = sid;
    return sid;
}

void PCmpServer::processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    // payload.sesid 없으면 자체 발행 (CSP가 안 보낸 경우)
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) sesid = issueSesid("");
    std::string svc = payload.GetString("service");
    if (svc.empty()) svc = "system";
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string txIdStr = std::to_string(transId);

    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    logFlow("heartbeat", "csp", "cmp", "JSON", "HEARTBEAT", "",
            txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    SimpleJson::JsonNode resp;
    resp.Set("trans_id", transId);
    resp.Set("sesid", sesid);
    resp.Set("service", svc);
    resp.Set("response", "OK");

    int txSeq = sendResponse(ip, port, resp.ToString());
    logFlow("heartbeat", "cmp", "csp", "JSON", "OK", "",
            txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
    logBody("TX", peerStr.c_str(), "JSON", "OK");
}

void PCmpServer::processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) sesid = issueSesid("");
    std::string svc = payload.GetString("service");
    if (svc.empty()) svc = "system";
    std::string peerStr = ip + ":" + std::to_string(port);
    std::string txIdStr = std::to_string(transId);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    logFlow("stats", "csp", "cmp", "JSON", "STATS_REQUEST", "",
            txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    PAutoLock lock(_mutex);

    int sessionCount = (int)_sessions.size();
    int groupCount = (int)_groups.size();
    int freeCount = (int)_freeResources.size();
    int totalPorts = _rtpPoolSize;
    int usedPorts = totalPorts - freeCount;
    // PTT(그룹통화) 전용 풀 — VoIP 풀(_resourcePool)과 분리. 대시보드 RTP 리소스 분리 표시용.
    int pttFreeCount  = (int)_freePttResources.size();
    int pttTotalPorts = (int)_pttPool.size();
    int pttUsedPorts  = pttTotalPorts - pttFreeCount;

    // 그룹별 상세 (멤버수, floor 화자)
    std::string groupsJson = "[";
    bool first = true;
    for (auto const& [gid, group] : _groups) {
        if (!first) groupsJson += ",";
        first = false;
        groupsJson += "{\"group_id\":\"" + gid + "\"";
        groupsJson += ",\"members\":" + std::to_string(group->getMemberCount());
        std::string holder = group->getFloorHolder();
        if (!holder.empty()) {
            groupsJson += ",\"floor_holder\":\"" + holder + "\"";
        }
        groupsJson += "}";
    }
    groupsJson += "]";

    std::string body = "{\"trans_id\":" + std::to_string(transId)
        + ",\"sesid\":\"" + sesid + "\""
        + ",\"service\":\"" + svc + "\""
        + ",\"response\":{\"status\":\"OK\""
        + ",\"sessions\":" + std::to_string(sessionCount)
        + ",\"groups\":" + std::to_string(groupCount)
        + ",\"rtp_ports_total\":" + std::to_string(totalPorts)
        + ",\"rtp_ports_used\":" + std::to_string(usedPorts)
        + ",\"rtp_ports_free\":" + std::to_string(freeCount)
        + ",\"ptt_rtp_ports_total\":" + std::to_string(pttTotalPorts)
        + ",\"ptt_rtp_ports_used\":" + std::to_string(pttUsedPorts)
        + ",\"ptt_rtp_ports_free\":" + std::to_string(pttFreeCount)
        + ",\"session_timeout\":" + std::to_string(_sessionTimeout)
        + ",\"group_details\":" + groupsJson
        + "}}";

    int txSeq = sendResponse(ip, port, body);
    logFlow("stats", "cmp", "csp", "JSON", "OK", "",
            txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
    logBody("TX", peerStr.c_str(), "JSON", body.c_str());
    LOG_INFO("PCmpServer", "STATS: sessions=%d groups=%d ports=%d/%d", sessionCount, groupCount, usedPorts, totalPorts);
}

void PCmpServer::processAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string cmdName = payload.GetString("cmd");
    if (cmdName.empty()) cmdName = "ADD_SESSION";
    std::string sessionId = payload.GetString("session_id");
    std::string rmtIp = payload.GetString("remote_ip");
    int rmtPort = (int)payload.GetInt("remote_port");
    int rmtVideoPort = (int)payload.GetInt("remote_video_port");
    int peerIdx = (int)payload.GetInt("peer_index", -1);
    std::string caller = payload.GetString("caller");
    std::string callee = payload.GetString("callee");

    // sesid: payload에서 받아 저장. 없으면 CMP 자체 발행 (방어적)
    std::string sesid = payload.GetString("sesid");
    if (sesid.empty()) sesid = issueSesid(caller);
    _sesidMap[sessionId] = sesid;

    // service: payload에서 받아 저장 (CSP 설정 기반), 없으면 "volte" fallback (VoLTE processAdd 특성)
    std::string svc = payload.GetString("service");
    if (svc.empty()) svc = "volte";
    _serviceMap[sessionId] = svc;

    // detail: 명령어별로 CSP 기록과 동일 포맷
    std::string detail;
    if (cmdName == "MODIFY_SESSION") {
        // MODIFY는 peer_index가 가리키는 한 쪽만 표기 (발/착 구분)
        if (peerIdx == 1 && !callee.empty()) detail = callee;
        else if (peerIdx == 0 && !caller.empty()) detail = caller;
        else if (!caller.empty() && !callee.empty()) detail = caller + "\xe2\x86\x92" + callee;
    } else {
        // ADD_SESSION: caller→callee
        if (!caller.empty() && !callee.empty()) detail = caller + "\xe2\x86\x92" + callee;
        else if (!caller.empty()) detail = caller;
        else detail = sessionId;
    }

    std::string rtpIp = _serverIp; // Resource IP
    int rtpPort = 0;
    int videoPort = 0;

    PRtpRelay* rtp = NULL;
    PAutoLock lock(_mutex);
    
    if (_sessions.find(sessionId) == _sessions.end()) {
        rtp = allocResource(rtpIp, rtpPort, videoPort);
        if (rtp) {
            rtp->setSessionId(sessionId);
            _sessions[sessionId] = rtp;
        }
    } else {
        rtp = _sessions[sessionId];
        rtpIp = _rtpIp;  // RTP IP는 항상 설정값 사용
        rtpPort = rtp->getLocalPort(); // reuse existing
        videoPort = rtp->getLocalVideoPort();
    }

    if (rtp) {
        if (rmtPort > 0) {
             rtp->setRemote(rmtIp, rmtPort, rmtVideoPort, peerIdx);
        }

        // Worker thread는 initResourcePool()에서 영구 등록됨 — 여기서 추가 불필요

        // CSP가 전달한 record_dir이 있으면 해당 경로에 녹취 (없으면 녹취 안 함)
        std::string recordDir = payload.GetString("record_dir");
        if (!recordDir.empty() && !rtp->isRecording()) {
            std::string caller = payload.GetString("caller");
            std::string callee = payload.GetString("callee");
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
        if (cmdName == "ADD_SESSION") {
            logFlow(sessionId, "cmp", "cmp", "INT", "SESSION_START",
                    ("port=" + std::to_string(rtpPort)).c_str(),
                    "", svc.c_str(), sesid.c_str(), "", 0, "",
                    caller.c_str(), callee.c_str());
        }

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);  // 응답에도 service 포함 (계승)
        SimpleJson::JsonNode respBody;
        respBody.Set("status", "OK");
        respBody.Set("local_ip", rtpIp);
        respBody.Set("local_port", rtpPort);
        respBody.Set("local_video_port", videoPort);
        resp.Set("response", respBody.ToString());

        int txSeq = sendResponse(ip, port, resp.ToString(), caller.c_str(), callee.c_str());
        // OK 응답은 detail 불필요 (요청 detail 과 중복 방지)
        logFlow(sessionId, "cmp", "csp", "JSON", "OK", "",
                txIdStr.c_str(),
                svc.c_str(), sesid.c_str(), "", txSeq, "csp",
                caller.c_str(), callee.c_str());
        logBody("TX", peerStr.c_str(), "JSON", respBody.ToString().c_str());
        LOG_INFO("PCmpServer", "ADD_SESSION session=%s remote=%s:%d -> local=%s:%d", sessionId.c_str(), rmtIp.c_str(), rmtPort, rtpIp.c_str(), rtpPort);
    } else {
         SimpleJson::JsonNode resp;
         resp.Set("trans_id", transId);
         resp.Set("response", "ERROR No Resource");
         sendResponse(ip, port, resp.ToString());
         LOG_WARN("PCmpServer", "ADD_SESSION session=%s FAILED: no available resource", sessionId.c_str());
    }
}

void PCmpServer::processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string sessionId = payload.GetString("session_id");
    std::string caller = payload.GetString("caller");
    std::string callee = payload.GetString("callee");
    std::string detail;
    if (!caller.empty() && !callee.empty()) detail = caller + "\xe2\x86\x92" + callee;
    else detail = sessionId;

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

    PAutoLock lock(_mutex);

    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(sessionId, "csp", "cmp", "JSON", "REMOVE_SESSION",
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
        LOG_INFO("PCmpServer", "REMOVE_SESSION session=%s", sessionId.c_str());
    } else {
        LOG_WARN("PCmpServer", "REMOVE_SESSION session=%s not found", sessionId.c_str());
    }

    SimpleJson::JsonNode resp;
    resp.Set("trans_id", transId);
    resp.Set("sesid", sesid);
    resp.Set("service", svc);
    resp.Set("response", "OK");
    int txSeq = sendResponse(ip, port, resp.ToString(), caller.c_str(), callee.c_str());
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
    std::string peerStr = ip + ":" + std::to_string(port);
    logBody("RX", peerStr.c_str(), "JSON", payload.ToString().c_str());
    std::string txIdStr = std::to_string(transId);
    logFlow(groupId, "csp", "cmp", "JSON", "ADD_PTT_GROUP", groupId.c_str(), txIdStr.c_str(),
            svc.c_str(), sesid.c_str(), subid.c_str(), _lastRxSeq, "csp");

    std::string sharedIp = _rtpIp;
    int sharedPort = 0;
    int sharedFloorPort = 0;
    int sharedVideoPort = 0;

    PAutoLock lock(_mutex);
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
        pttSession = allocPttResource(sharedIp, sharedPort, sharedFloorPort, sharedVideoPort);
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
                     ("rtp=" + std::to_string(sharedPort) + " floor=" + std::to_string(sharedFloorPort) +
                      " video=" + std::to_string(sharedVideoPort)).c_str(),
                     "", svc.c_str(), sesid.c_str(), subid.c_str());
             LOG_INFO("PCmpServer", "ADD_GROUP group=%s rtp=%d floor=%d video=%d (new)", groupId.c_str(), sharedPort, sharedFloorPort, sharedVideoPort);
        } else {
             delete group;
             group = NULL;
             LOG_WARN("PCmpServer", "ADD_GROUP group=%s FAILED: no available resource", groupId.c_str());
        }
    } else {
        group = _groups[groupId];
        pttSession = group->getPttSession();
        if (pttSession) {
            sharedPort = pttSession->getLocalRtpPort();
            sharedFloorPort = pttSession->getLocalFloorPort();
            sharedIp = _rtpIp;
        }
        // 기존 그룹의 비디오 포트 조회
        PRtpMulticast* pttSess = group->getPttSession();
        if (pttSess) {
            sharedVideoPort = pttSess->getLocalVideoPort();
        }
        // 기존 그룹이더라도 record_dir이 새로 전달되면 갱신
        std::string recordDir = payload.GetString("record_dir");
        if (!recordDir.empty() && !group->isRecordEnabled()) {
            group->setRecording(true, recordDir);
        }
        LOG_DEBUG("PCmpServer", "ADD_GROUP group=%s rtp=%d floor=%d video=%d (existing)", groupId.c_str(), sharedPort, sharedFloorPort, sharedVideoPort);
    }

    if (group) {
        if (!membersStr.empty()) {
            std::stringstream ss(membersStr);
            std::string segment;
            std::map<std::string, int> priorities;
            while(std::getline(ss, segment, ',')) {
                size_t colon = segment.find(':');
                if (colon != std::string::npos) {
                    std::string sid = segment.substr(0, colon);
                    int prio = 0;
                    try { prio = std::stoi(segment.substr(colon+1)); } catch(...) {}
                    priorities[sid] = prio;
                }
            }
            group->updatePriorities(priorities);
        }
        
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        SimpleJson::JsonNode respBody;
        respBody.Set("status", "OK");
        respBody.Set("ip", sharedIp);
        respBody.Set("port", sharedPort);
        respBody.Set("floor_port", sharedFloorPort);
        respBody.Set("video_port", sharedVideoPort);

        resp.Set("response", respBody.ToString());
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), subid.c_str(), txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", respBody.ToString().c_str());
    } else {
         SimpleJson::JsonNode resp;
         resp.Set("trans_id", transId);
         resp.Set("sesid", sesid);
         resp.Set("service", svc);
         resp.Set("response", "ERROR Allocation Fail");
         int txSeq = sendResponse(ip, port, resp.ToString());
         logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Allocation Fail", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
    }
}

void PCmpServer::processJoinGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");
    std::string userIp = payload.GetString("user_ip");
    int userPort = (int)payload.GetInt("user_port");
    int userFloorPort = (int)payload.GetInt("user_floor_port");
    int userVideoPort = (int)payload.GetInt("user_video_port");

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
    logFlow(groupId, "csp", "cmp", "JSON", "JOIN_PTT_GROUP", sessionId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        PMcpttGroup* group = _groups[groupId];
        group->addMember(sessionId, userIp, userPort, userFloorPort, userVideoPort);

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        resp.Set("response", "OK");
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", "OK");
        LOG_INFO("PCmpServer", "JOIN_GROUP group=%s session=%s %s:%d", groupId.c_str(), sessionId.c_str(), userIp.c_str(), userPort);
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        resp.Set("response", "ERROR Group Not Found");
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "JOIN_GROUP group=%s not found", groupId.c_str());
    }
}

void PCmpServer::processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");

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
    logFlow(groupId, "csp", "cmp", "JSON", "LEAVE_PTT_GROUP", sessionId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        PMcpttGroup* group = _groups[groupId];
        group->removeMember(sessionId);

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        resp.Set("response", "OK");
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", "OK");
        LOG_INFO("PCmpServer", "LEAVE_GROUP group=%s session=%s", groupId.c_str(), sessionId.c_str());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        resp.Set("response", "ERROR Group Not Found");
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "LEAVE_GROUP group=%s not found", groupId.c_str());
    }
}

void PCmpServer::processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");

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
    logFlow(groupId, "csp", "cmp", "JSON", "REMOVE_PTT_GROUP", groupId.c_str(), txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", _lastRxSeq, "csp");

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        PMcpttGroup* group = _groups[groupId];
        logFlow(groupId, "cmp", "cmp", "INT", "GROUP_END", "", "", svc.c_str(), sesid.c_str());
        // PTT 리소스 반환
        PRtpMulticast* ptt = group->getPttSession();
        if (ptt) { ptt->reset(); freePttResource(ptt); }
        delete group;
        _groups.erase(groupId);

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        resp.Set("response", "OK");
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "OK", "", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        logBody("TX", peerStr.c_str(), "JSON", "OK");
        LOG_INFO("PCmpServer", "REMOVE_GROUP group=%s", groupId.c_str());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("sesid", sesid);
        resp.Set("service", svc);
        resp.Set("response", "ERROR Group Not Found");
        int txSeq = sendResponse(ip, port, resp.ToString());
        logFlow(groupId, "cmp", "csp", "JSON", "ERROR", "Group Not Found", txIdStr.c_str(), svc.c_str(), sesid.c_str(), "", txSeq, "csp");
        LOG_WARN("PCmpServer", "REMOVE_GROUP group=%s not found", groupId.c_str());
    }

    // 그룹 종료 후 캐시 정리
    _sesidMap.erase(groupId);
    _serviceMap.erase(groupId);
    _groupSubId.erase(groupId);
}

void PCmpServer::processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
     LOG_DEBUG("PCmpServer", "MODIFY_GROUP -> delegating to processAddGroup");
     processAddGroup(payload, ip, port, transId);
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
        if (root.Has("RtpWorkerCount")) {
            int w = (int)root.GetInt("RtpWorkerCount");
            if (w >= 1 && w <= 32) _rtpWorkerCount = w;
        }
        // PTT 리소스 풀 설정
        if (root.Has("PttRtpStartPort")) _pttRtpStartPort = (int)root.GetInt("PttRtpStartPort");
        if (root.Has("PttRtpPoolSize")) _pttRtpPoolSize = (int)root.GetInt("PttRtpPoolSize");
        if (root.Has("PttFloorStartPort")) _pttFloorStartPort = (int)root.GetInt("PttFloorStartPort");
        if (root.Has("PttVideoStartPort")) _pttVideoStartPort = (int)root.GetInt("PttVideoStartPort");
        
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

    LOG_INFO("PCmpServer", "Config: VoIP(port=%d pool=%d) PTT(rtp=%d floor=%d video=%d pool=%d) Workers=%d RtpIp=%s ServerIp=%s:%d DtmfPtt=%d SessionTimeout=%d",
           _rtpStartPort, _rtpPoolSize, _pttRtpStartPort, _pttFloorStartPort, _pttVideoStartPort, _pttRtpPoolSize,
           _rtpWorkerCount, _rtpIp.c_str(), _serverIp.c_str(), _serverPort,
           _dtmfPttEnable, _sessionTimeout);
}

void PCmpServer::initResourcePool() {
    int currentPort = _rtpStartPort;
    for (int i = 0; i < _rtpPoolSize; ++i) {
        std::string name = formatStr("InActiveRtp_%d", i);
        PRtpRelay* rtp = new PRtpRelay(name);
        
        if (rtp->init(_rtpIp, currentPort, currentPort + 2)) {
             // Worker thread 영구 등록 (프로세스 종료까지 proc() 상시 동작)
             std::string wname = formatStr("RtpWorker_%d", i % _rtpWorkerCount);
             rtp->setWorkerName(wname);
             addHandler(wname, rtp);
             _resourcePool.push_back(rtp);
             _freeResources.push_back(rtp);
        } else {
             LOG_ERROR("PCmpServer", "Failed to init resource on port %d", currentPort);
             delete rtp;
        }
        currentPort += 4;
    }
    LOG_INFO("PCmpServer", "VoIP pool: %lu resources (port %d-%d)", _resourcePool.size(), _rtpStartPort, currentPort - 4);
}

void PCmpServer::initPttResourcePool() {
    int rtpPort = _pttRtpStartPort;
    int floorPort = _pttFloorStartPort;
    int videoPort = _pttVideoStartPort;
    for (int i = 0; i < _pttRtpPoolSize; ++i) {
        std::string name = formatStr("PttRtp_%d", i);
        PRtpMulticast* ptt = new PRtpMulticast(name);

        if (ptt->init(_rtpIp, rtpPort, floorPort, videoPort)) {
            std::string wname = formatStr("RtpWorker_%d", i % _rtpWorkerCount);
            ptt->setWorkerName(wname);
            addHandler(wname, ptt);
            _pttPool.push_back(ptt);
            _freePttResources.push_back(ptt);
        } else {
            LOG_ERROR("PCmpServer", "Failed to init PTT resource rtp=%d floor=%d video=%d", rtpPort, floorPort, videoPort);
            delete ptt;
        }
        rtpPort += 2;
        floorPort += 2;
        videoPort += 2;
    }
    LOG_INFO("PCmpServer", "PTT pool: %lu resources (rtp %d-%d, floor %d-%d, video %d-%d)",
             _pttPool.size(), _pttRtpStartPort, rtpPort - 2, _pttFloorStartPort, floorPort - 2, _pttVideoStartPort, videoPort - 2);
}

PRtpRelay* PCmpServer::allocResource(std::string& rtpIp, int& rtpPort, int& videoPort) {
    if (_freeResources.empty()) {
        LOG_WARN("PCmpServer", "allocResource: no free resources");
        return NULL;
    }
    
    PRtpRelay* rtp = _freeResources.back();
    _freeResources.pop_back();
    
    rtpIp = _rtpIp; 
    rtpPort = rtp->getLocalPort(); 
    videoPort = rtp->getLocalVideoPort();
    
    LOG_INFO("PCmpServer", "allocResource: port=%d (remaining %lu)", rtpPort, _freeResources.size());
    return rtp;
}

void PCmpServer::freeResource(PRtpRelay* rtp) {
    if (rtp) {
        LOG_INFO("PCmpServer", "freeResource: port=%d", rtp->getLocalPort());
        _freeResources.push_back(rtp);
    }
}

PRtpMulticast* PCmpServer::allocPttResource(std::string& rtpIp, int& rtpPort, int& floorPort, int& videoPort) {
    if (_freePttResources.empty()) {
        LOG_WARN("PCmpServer", "allocPttResource: no free PTT resources");
        return NULL;
    }
    PRtpMulticast* ptt = _freePttResources.back();
    _freePttResources.pop_back();
    rtpIp = _rtpIp;
    rtpPort = ptt->getLocalRtpPort();
    floorPort = ptt->getLocalFloorPort();
    videoPort = ptt->getLocalVideoPort();
    LOG_INFO("PCmpServer", "allocPttResource: rtp=%d floor=%d video=%d (remaining %lu)", rtpPort, floorPort, videoPort, _freePttResources.size());
    return ptt;
}

void PCmpServer::freePttResource(PRtpMulticast* ptt) {
    if (ptt) {
        LOG_INFO("PCmpServer", "freePttResource: rtp=%d floor=%d", ptt->getLocalRtpPort(), ptt->getLocalFloorPort());
        _freePttResources.push_back(ptt);
    }
}

void PCmpServer::timeoutLoop() {
    while (_running) {
        // 60초마다 체크
        for (int i = 0; i < 60 && _running; ++i) {
            msleep(1000);
        }
        if (!_running) break;

        time_t now;
        time(&now);

        // Stale 개별 세션 정리
        std::vector<std::string> staleSessionIds;
        {
            PAutoLock lock(_mutex);
            for (auto const& [sid, rtp] : _sessions) {
                if (rtp && (now - rtp->getLastActivityTime()) >= _sessionTimeout) {
                    staleSessionIds.push_back(sid);
                }
            }
        }
        for (const auto& sid : staleSessionIds) {
            LOG_INFO("PCmpServer", "Session timeout: session=%s — auto cleanup", sid.c_str());
            PAutoLock lock(_mutex);
            auto it = _sessions.find(sid);
            if (it != _sessions.end()) {
                PRtpRelay* rtp = it->second;
                std::string sesid = _sesidMap.count(sid) ? _sesidMap[sid] : issueSesid("");
                std::string svc = _serviceMap.count(sid) ? _serviceMap[sid] : "volte";
                logFlow(sid, "cmp", "cmp", "INT", "SESSION_TIMEOUT", "",
                        "", svc.c_str(), sesid.c_str());
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

void PCmpServer::ensureFlowHourDir() {
    std::string hourDir = getFlowHourDir();
    if (hourDir == _currentFlowHourDir) return;

    if (_flowFile) { fclose(_flowFile); _flowFile = nullptr; }
    if (_msgFile) { fclose(_msgFile); _msgFile = nullptr; }

    mkdirP(hourDir);
    _currentFlowHourDir = hourDir;

    _flowFile = fopen((hourDir + "/" + _systemId + ".flow.jsonl").c_str(), "a");
    // "a+" 로 열어야 fgets 로 기존 라인 수 카운트 가능 ("a" 단독은 읽기 불가 → _msgSeq 부정확)
    _msgFile = fopen((hourDir + "/" + _systemId + "_csp.msg.jsonl").c_str(), "a+");
    _msgSeq = 0;
    // 기존 라인 수 카운트 (seq 연속성)
    if (_msgFile) {
        fseek(_msgFile, 0, SEEK_SET);
        char buf[4096];
        while (fgets(buf, sizeof(buf), _msgFile)) _msgSeq++;
        fseek(_msgFile, 0, SEEK_END);
    }
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
    ensureFlowHourDir();
    if (!_msgFile) return 0;
    _msgSeq++;
    // 순서: ts, dir, peer, caller, callee, proto, msg (빈값 key 생략)
    fprintf(_msgFile, "{\"ts\":\"%s\",\"dir\":\"%s\",\"peer\":\"%s\"",
            ts ? ts : "", dir ? dir : "", peer ? peer : "");
    if (caller && caller[0])
        fprintf(_msgFile, ",\"caller\":\"%s\"", _jsonEsc(caller).c_str());
    if (callee && callee[0])
        fprintf(_msgFile, ",\"callee\":\"%s\"", _jsonEsc(callee).c_str());
    fprintf(_msgFile, ",\"proto\":\"%s\",\"msg\":\"%s\"}\n",
            proto ? proto : "", _jsonEsc(msg).c_str());
    fflush(_msgFile);
    return _msgSeq;
}

void PCmpServer::logFlow(const std::string& key, const char* from, const char* to,
                         const char* proto, const char* label, const char* detail,
                         const char* txId, const char* service,
                         const char* sesid, const char* subid,
                         int seq, const char* iface,
                         const char* caller, const char* callee) {
    if (_serviceLogDir.empty()) return;

    ensureFlowHourDir();

    FILE* f = _flowFile;
    if (!f) return;

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
    bool bFirst = true;
    auto emit = [&](const char* kname, const std::string& val, bool isNum = false, int iNum = 0) {
        if (!isNum && val.empty()) return;
        if (!bFirst) fprintf(f, ",");
        bFirst = false;
        if (isNum) fprintf(f, "\"%s\":%d", kname, iNum);
        else fprintf(f, "\"%s\":\"%s\"", kname, val.c_str());
    };

    fprintf(f, "{");
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
    fprintf(f, "}\n");
    fflush(f);
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

    std::string hourDir = getMsgHourDir();
    if (hourDir != _currentMsgHourDir) {
        if (_bodyFile) { fclose(_bodyFile); _bodyFile = nullptr; }
        mkdirP(hourDir);
        _currentMsgHourDir = hourDir;
        std::string path = hourDir + "/" + _systemId + "_csp.jsonl";
        _bodyFile = fopen(path.c_str(), "a");
    }
    if (!_bodyFile) return;

    std::string ts = getTimestamp();

    // JSON escape msg
    std::string escaped;
    if (msg) {
        for (const char* p = msg; *p; ++p) {
            switch (*p) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n";  break;
                case '\r': escaped += "\\r";  break;
                default:   escaped += *p;
            }
        }
    }

    fprintf(_bodyFile,
        "{\"ts\":\"%s\",\"dir\":\"%s\",\"peer\":\"%s\",\"proto\":\"%s\",\"msg\":\"%s\"}\n",
        ts.c_str(), dir ? dir : "", peer ? peer : "",
        proto ? proto : "", escaped.c_str());
    fflush(_bodyFile);
}
