#include "CmpServer.h"
#include "CmpLog.h"
#include "SimpleJson.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <sstream>
#include <cstring>
#include <thread>
#include <algorithm>
#include <cctype>
#include "PRtpHandler.h"
#include "McpttGroup.h"
#include <fstream>

CmpServer::CmpServer(const std::string& name, const std::string& configFile)
    : PModule(name), _running(false), _udpFd(-1), _configFile(configFile), _sessionTimeout(600), _rtpWorkerCount(4)
{
    loadConfig();

    // Worker 스레드를 먼저 생성해야 initResourcePool()의 addHandler()가 동작함
    for(int i=0; i<_rtpWorkerCount; ++i) {
        std::string wname = formatStr("RtpWorker_%d", i);
        addWorker(wname, 1, 2048, true);
    }

    initResourcePool();
}

CmpServer::~CmpServer() {
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
}

bool CmpServer::startServer() {
    _udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_udpFd < 0) {
        LOG_ERROR("CmpServer", "socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_serverIp.c_str());
    addr.sin_port = htons(_serverPort);

    if (bind(_udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("CmpServer", "bind() failed on %s:%d: %s", _serverIp.c_str(), _serverPort, strerror(errno));
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
        LOG_INFO("CmpServer", "Session timeout thread started (timeout=%ds)", _sessionTimeout);
    }

    LOG_INFO("CmpServer", "Server listening on %s:%d", _serverIp.c_str(), _serverPort);
    return true;
}

void CmpServer::stopServer() {
    _running = false;
    if (_timeoutThread.joinable()) _timeoutThread.join();
    if (_udpFd >= 0) {
        ::close(_udpFd);
        _udpFd = -1;
    }
    LOG_INFO("CmpServer", "Server stopped");
}

void CmpServer::runControlLoop() {
    char buf[4096];
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    while (_running) {
        int len = recvfrom(_udpFd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&clientAddr, &addrLen);
        if (len > 0) {
            buf[len] = '\0';
            LOG_DEBUG("CmpServer", "Recv %d bytes from %s:%d", len, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
            std::string ip = inet_ntoa(clientAddr.sin_addr);
            int port = ntohs(clientAddr.sin_port);
            handlePacket(buf, len, ip, port);
        }
    }
}

// Modified to parse JSON packet
void CmpServer::handlePacket(char* buf, int len, const std::string& ip, int port) {
    if (len <= 0) return;
    std::string strPacket(buf, len);
    
    // Parse JSON Wrapper
    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(strPacket);
    
    // Check trans_id and payload
    if (root.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("CmpServer", "Invalid JSON Packet from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        return;
    }

    int transId = (int)root.GetInt("trans_id", 0);
    SimpleJson::JsonNode payload = root.Get("payload");
    
    if (payload.type != SimpleJson::JSON_OBJECT) {
        LOG_ERROR("CmpServer", "Missing Payload from %s:%d: %s", ip.c_str(), port, strPacket.c_str());
        return;
    }

    // Extract CMD
    std::string cmd = payload.GetString("cmd");
    // Normalize to uppercase for matching
    std::string cmdUpper = cmd;
    std::transform(cmdUpper.begin(), cmdUpper.end(), cmdUpper.begin(), ::toupper);

    // Dispatch
    LOG_DEBUG("CmpServer", "Dispatching cmd=%s transId=%d from %s:%d", cmd.c_str(), transId, ip.c_str(), port);
    if (cmdUpper == "ADD_SESSION" || cmdUpper == "ADD") processAdd(payload, ip, port, transId);
    else if (cmdUpper == "REMOVE_SESSION" || cmdUpper == "REMOVE") processRemove(payload, ip, port, transId);
    else if (cmdUpper == "HEARTBEAT" || cmdUpper == "ALIVE") processAlive(payload, ip, port, transId);
    else if (cmdUpper == "ADD_GROUP" || cmdUpper == "ADDGROUP") processAddGroup(payload, ip, port, transId);
    else if (cmdUpper == "JOIN_GROUP" || cmdUpper == "JOINGROUP") processJoinGroup(payload, ip, port, transId);
    else if (cmdUpper == "LEAVE_GROUP" || cmdUpper == "LEAVEGROUP") processLeaveGroup(payload, ip, port, transId);
    else if (cmdUpper == "REMOVE_GROUP" || cmdUpper == "REMOVEGROUP") processRemoveGroup(payload, ip, port, transId);
    else if (cmdUpper == "MODIFY_GROUP" || cmdUpper == "MODIFY_GROUP") processModifyGroup(payload, ip, port, transId);
    else if (cmdUpper == "MODIFY_SESSION" || cmdUpper == "MODIFY") processModify(payload, ip, port, transId);
    else if (cmdUpper == "STATS_REQUEST" || cmdUpper == "STATS") processStats(payload, ip, port, transId);
    else {
        LOG_WARN("CmpServer", "Unknown CMD: %s from %s:%d", cmd.c_str(), ip.c_str(), port);
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Unknown Command");
        sendResponse(ip, port, resp.ToString());
    }
}

void CmpServer::sendResponse(const std::string& ip, int port, const std::string& msg) {
    LOG_DEBUG("CmpServer", "Sending %lu bytes to %s:%d", msg.length(), ip.c_str(), port);
    if (_udpFd != -1) {
        struct sockaddr_in cliaddr;
        memset(&cliaddr, 0, sizeof(cliaddr));
        cliaddr.sin_family = AF_INET;
        cliaddr.sin_port = htons(port);
        cliaddr.sin_addr.s_addr = inet_addr(ip.c_str());
        int sent = sendto(_udpFd, msg.c_str(), msg.length(), 0, (struct sockaddr*)&cliaddr, sizeof(cliaddr));
        if (sent < 0) {
            LOG_ERROR("CmpServer", "sendto failed to %s:%d: %s", ip.c_str(), port, strerror(errno));
        }
    }
}

void CmpServer::processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    SimpleJson::JsonNode resp;
    resp.Set("trans_id", transId);
    resp.Set("response", "OK");
    sendResponse(ip, port, resp.ToString());
}

void CmpServer::processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    PAutoLock lock(_mutex);

    int sessionCount = (int)_sessions.size();
    int groupCount = (int)_groups.size();
    int freeCount = (int)_freeResources.size();
    int totalPorts = _rtpPoolSize;
    int usedPorts = totalPorts - freeCount;

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
        + ",\"response\":{\"status\":\"OK\""
        + ",\"sessions\":" + std::to_string(sessionCount)
        + ",\"groups\":" + std::to_string(groupCount)
        + ",\"rtp_ports_total\":" + std::to_string(totalPorts)
        + ",\"rtp_ports_used\":" + std::to_string(usedPorts)
        + ",\"rtp_ports_free\":" + std::to_string(freeCount)
        + ",\"session_timeout\":" + std::to_string(_sessionTimeout)
        + ",\"group_details\":" + groupsJson
        + "}}";

    sendResponse(ip, port, body);
    LOG_INFO("CmpServer", "STATS: sessions=%d groups=%d ports=%d/%d", sessionCount, groupCount, usedPorts, totalPorts);
}

void CmpServer::processAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string sessionId = payload.GetString("session_id");
    std::string rmtIp = payload.GetString("remote_ip");
    int rmtPort = (int)payload.GetInt("remote_port");
    int rmtVideoPort = (int)payload.GetInt("remote_video_port");
    int peerIdx = (int)payload.GetInt("peer_index");

    std::string rtpIp = _serverIp; // Resource IP
    int rtpPort = 0;
    int videoPort = 0;
    
    PRtpTrans* rtp = NULL;
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
             rtp->setRmt(rmtIp, rmtPort, rmtVideoPort, peerIdx);
        }

        // Worker thread는 initResourcePool()에서 영구 등록됨 — 여기서 추가 불필요

        // CSP가 전달한 record_dir이 있으면 해당 경로에 녹취 (없으면 녹취 안 함)
        std::string recordDir = payload.GetString("record_dir");
        if (!recordDir.empty() && !rtp->isRecording()) {
            rtp->startRecording(recordDir, sessionId);
        }

        // CSP가 전달한 log_dir이 있으면 CMP flow 로그 기록 경로로 저장
        std::string logDir = payload.GetString("log_dir");
        if (!logDir.empty()) {
            _logDirs[sessionId] = logDir;
            logFlow(sessionId, "cmp", "cmp", "INT", "SESSION_START",
                    ("port=" + std::to_string(rtpPort)).c_str());
        }

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        SimpleJson::JsonNode respBody;
        respBody.Set("status", "OK");
        respBody.Set("local_ip", rtpIp);
        respBody.Set("local_port", rtpPort);
        respBody.Set("local_video_port", videoPort);
        resp.Set("response", respBody.ToString()); 
        sendResponse(ip, port, resp.ToString());
        
        LOG_INFO("CmpServer", "ADD_SESSION session=%s remote=%s:%d -> local=%s:%d", sessionId.c_str(), rmtIp.c_str(), rmtPort, rtpIp.c_str(), rtpPort);
    } else {
         SimpleJson::JsonNode resp;
         resp.Set("trans_id", transId);
         resp.Set("response", "ERROR No Resource");
         sendResponse(ip, port, resp.ToString());
         LOG_WARN("CmpServer", "ADD_SESSION session=%s FAILED: no available resource", sessionId.c_str());
    }
}

void CmpServer::processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string sessionId = payload.GetString("session_id");
    PAutoLock lock(_mutex);
    if (_sessions.find(sessionId) != _sessions.end()) {
        PRtpTrans* rtp = _sessions[sessionId];
        logFlow(sessionId, "cmp", "cmp", "INT", "SESSION_END", "");
        // worker thread는 유지 (초기화 시 등록, 프로세스 종료까지 동작)
        rtp->reset();
        freeResource(rtp);
        _sessions.erase(sessionId);
        _logDirs.erase(sessionId);
        LOG_INFO("CmpServer", "REMOVE_SESSION session=%s", sessionId.c_str());
    } else {
        LOG_WARN("CmpServer", "REMOVE_SESSION session=%s not found", sessionId.c_str());
    }
    
    SimpleJson::JsonNode resp;
    resp.Set("trans_id", transId);
    resp.Set("response", "OK");
    sendResponse(ip, port, resp.ToString());
}

void CmpServer::processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    LOG_DEBUG("CmpServer", "MODIFY -> delegating to processAdd");
    processAdd(payload, ip, port, transId);
}

void CmpServer::processAddGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string membersStr = payload.GetString("members"); 
    
    std::string sharedIp = _serverIp;
    int sharedPort = 0;
    int sharedVideoPort = 0;
    
    PAutoLock lock(_mutex);
    PRtpTrans* sharedSession = NULL;
    McpttGroup* group = NULL;

    if (_groups.find(groupId) == _groups.end()) {
        group = new McpttGroup(groupId);
        // Floor 이벤트 로그 콜백 설정
        group->setLogCallback([this](const std::string& key, const char* from, const char* to,
                                      const char* proto, const char* label, const char* body) {
            logFlow(key, from, to, proto, label, body);
        });
        sharedSession = allocResource(sharedIp, sharedPort, sharedVideoPort);
        if (sharedSession) {
             sharedSession->setGroup(group);
             group->setDtmfConfig(_dtmfPttEnable, _dtmfPushDigit, _dtmfReleaseDigit);
             group->setSharedSession(sharedSession);
             // CSP가 전달한 record_dir이 있으면 해당 경로에 녹취
             std::string recordDir = payload.GetString("record_dir");
             if (!recordDir.empty()) {
                 group->setRecording(true, recordDir);
             }
             // CSP가 전달한 log_dir이 있으면 CMP flow 로그 기록 경로로 저장
             std::string logDir = payload.GetString("log_dir");
             if (!logDir.empty()) {
                 _logDirs[groupId] = logDir;
             }
             _groups[groupId] = group;

             static int workerIdx = 0;
             if (sharedSession->getWorkerName().empty()) {
                  std::string wname = formatStr("RtpWorker_%d", workerIdx++ % _rtpWorkerCount);
                  sharedSession->setWorkerName(wname);
                  addHandler(wname, sharedSession);
             }
             logFlow(groupId, "cmp", "cmp", "INT", "GROUP_START",
                     ("port=" + std::to_string(sharedPort)).c_str());
             LOG_INFO("CmpServer", "ADD_GROUP group=%s port=%d (new)", groupId.c_str(), sharedPort);
        } else {
             delete group;
             group = NULL;
             LOG_WARN("CmpServer", "ADD_GROUP group=%s FAILED: no available resource", groupId.c_str());
        }
    } else {
        group = _groups[groupId];
        sharedSession = group->getSharedSession();
        if (sharedSession) {
            sharedPort = sharedSession->getLocalPort();
            sharedVideoPort = sharedSession->getLocalVideoPort();
            sharedIp = _rtpIp;  // 기존 그룹도 RTP IP 사용
        }
        // 기존 그룹이더라도 log_dir/record_dir이 새로 전달되면 갱신
        std::string logDir = payload.GetString("log_dir");
        if (!logDir.empty() && _logDirs.find(groupId) == _logDirs.end()) {
            _logDirs[groupId] = logDir;
        }
        std::string recordDir = payload.GetString("record_dir");
        if (!recordDir.empty() && !group->isRecordEnabled()) {
            group->setRecording(true, recordDir);
        }
        LOG_DEBUG("CmpServer", "ADD_GROUP group=%s port=%d (existing)", groupId.c_str(), sharedPort);
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
        SimpleJson::JsonNode respBody;
        respBody.Set("status", "OK");
        respBody.Set("ip", sharedIp);
        respBody.Set("port", sharedPort);
        respBody.Set("video_port", sharedVideoPort);

        resp.Set("response", respBody.ToString());
        sendResponse(ip, port, resp.ToString());
    } else {
         SimpleJson::JsonNode resp;
         resp.Set("trans_id", transId);
         resp.Set("response", "ERROR Allocation Fail");
         sendResponse(ip, port, resp.ToString());
    }
}

void CmpServer::processJoinGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");
    std::string userIp = payload.GetString("user_ip");
    int userPort = (int)payload.GetInt("user_port");
    int userVideoPort = (int)payload.GetInt("user_video_port");

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        McpttGroup* group = _groups[groupId];
        group->addMember(sessionId, userIp, userPort, userVideoPort);
        logFlow(groupId, "cmp", "cmp", "RTP",
                ("JOIN(" + sessionId + ")").c_str(),
                (userIp + ":" + std::to_string(userPort)).c_str());

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
        LOG_INFO("CmpServer", "JOIN_GROUP group=%s session=%s %s:%d", groupId.c_str(), sessionId.c_str(), userIp.c_str(), userPort);
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
        LOG_WARN("CmpServer", "JOIN_GROUP group=%s not found", groupId.c_str());
    }
}

void CmpServer::processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        McpttGroup* group = _groups[groupId];
        group->removeMember(sessionId);
        logFlow(groupId, "cmp", "cmp", "RTP",
                ("LEAVE(" + sessionId + ")").c_str(), "");

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
        LOG_INFO("CmpServer", "LEAVE_GROUP group=%s session=%s", groupId.c_str(), sessionId.c_str());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
        LOG_WARN("CmpServer", "LEAVE_GROUP group=%s not found", groupId.c_str());
    }
}

void CmpServer::processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        McpttGroup* group = _groups[groupId];
        logFlow(groupId, "cmp", "cmp", "INT", "GROUP_END", "");
        delete group;
        _groups.erase(groupId);
        _logDirs.erase(groupId);

        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
        LOG_INFO("CmpServer", "REMOVE_GROUP group=%s", groupId.c_str());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
        LOG_WARN("CmpServer", "REMOVE_GROUP group=%s not found", groupId.c_str());
    }
}

void CmpServer::processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
     LOG_DEBUG("CmpServer", "MODIFY_GROUP -> delegating to processAddGroup");
     processAddGroup(payload, ip, port, transId);
}

void CmpServer::loadConfig() {
    std::ifstream t(_configFile);
    if (!t.is_open()) {
        if (_configFile.find(".json") != std::string::npos) {
             LOG_ERROR("CmpServer", "Failed to open config file: %s", _configFile.c_str());
             return;
        }
    }
    
    // Check extension
    if (_configFile.substr(_configFile.find_last_of(".") + 1) == "json") {
        std::stringstream buffer;
        buffer << t.rdbuf();
        std::string jsonContent = buffer.str();
        
        SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(jsonContent);
        
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
        
        // Log configuration
        std::string logDir = root.Has("LogDir") ? root.GetString("LogDir") : "";
        int logMaxSizeMB = root.Has("LogMaxSizeMB") ? (int)root.GetInt("LogMaxSizeMB") : 10;
        int logMaxFiles = root.Has("LogMaxFiles") ? (int)root.GetInt("LogMaxFiles") : 5;
        
        if (!logDir.empty()) {
            CmpLog::Instance().InitFile(logDir, "cmp", logMaxSizeMB, logMaxFiles);
        }
        
        if (root.Has("LogLevel")) {
            std::string lvl = root.GetString("LogLevel");
            if (lvl == "DEBUG" || lvl == "debug") CmpLog::Instance().SetLevel(CMP_LOG_DEBUG);
            else if (lvl == "WARN" || lvl == "warn") CmpLog::Instance().SetLevel(CMP_LOG_WARN);
            else if (lvl == "ERROR" || lvl == "error") CmpLog::Instance().SetLevel(CMP_LOG_ERROR);
            else CmpLog::Instance().SetLevel(CMP_LOG_INFO);
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
        }
    }

    if (_recordEnable) {
        std::string mkdirCmd = "mkdir -p " + _recordDir;
        system(mkdirCmd.c_str());
    }

    LOG_INFO("CmpServer", "Config: RtpStartPort=%d RtpPoolSize=%d RtpWorkerCount=%d RtpIp=%s ServerIp=%s ServerPort=%d DtmfPtt=%d Push=%s Rel=%s Record=%d RecordDir=%s SessionTimeout=%d",
           _rtpStartPort, _rtpPoolSize, _rtpWorkerCount, _rtpIp.c_str(), _serverIp.c_str(), _serverPort,
           _dtmfPttEnable, _dtmfPushDigit.c_str(), _dtmfReleaseDigit.c_str(),
           _recordEnable, _recordDir.c_str(), _sessionTimeout);
}

void CmpServer::initResourcePool() {
    int currentPort = _rtpStartPort;
    for (int i = 0; i < _rtpPoolSize; ++i) {
        std::string name = formatStr("InActiveRtp_%d", i);
        PRtpTrans* rtp = new PRtpTrans(name);
        
        if (rtp->init(_rtpIp, currentPort, currentPort + 2)) {
             // Worker thread 영구 등록 (프로세스 종료까지 proc() 상시 동작)
             std::string wname = formatStr("RtpWorker_%d", i % _rtpWorkerCount);
             rtp->setWorkerName(wname);
             addHandler(wname, rtp);
             _resourcePool.push_back(rtp);
             _freeResources.push_back(rtp);
        } else {
             LOG_ERROR("CmpServer", "Failed to init resource on port %d", currentPort);
             delete rtp;
        }
        currentPort += 4;
    }
    LOG_INFO("CmpServer", "Initialized %lu resources (port %d-%d)", _resourcePool.size(), _rtpStartPort, currentPort - 4);
}

PRtpTrans* CmpServer::allocResource(std::string& rtpIp, int& rtpPort, int& videoPort) {
    if (_freeResources.empty()) {
        LOG_WARN("CmpServer", "allocResource: no free resources");
        return NULL;
    }
    
    PRtpTrans* rtp = _freeResources.back();
    _freeResources.pop_back();
    
    rtpIp = _rtpIp; 
    rtpPort = rtp->getLocalPort(); 
    videoPort = rtp->getLocalVideoPort();
    
    LOG_INFO("CmpServer", "allocResource: port=%d (remaining %lu)", rtpPort, _freeResources.size());
    return rtp;
}

void CmpServer::freeResource(PRtpTrans* rtp) {
    if (rtp) {
        LOG_INFO("CmpServer", "freeResource: port=%d", rtp->getLocalPort());
        _freeResources.push_back(rtp);
    }
}

void CmpServer::timeoutLoop() {
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
            LOG_INFO("CmpServer", "Session timeout: session=%s — auto cleanup", sid.c_str());
            PAutoLock lock(_mutex);
            auto it = _sessions.find(sid);
            if (it != _sessions.end()) {
                PRtpTrans* rtp = it->second;
                logFlow(sid, "cmp", "cmp", "INT", "SESSION_TIMEOUT", "");
                rtp->reset();
                freeResource(rtp);
                _sessions.erase(it);
                _logDirs.erase(sid);
            }
        }

        // Stale 그룹 세션 정리 (공유 RTP에 패킷이 없는 그룹)
        std::vector<std::string> staleGroupIds;
        {
            PAutoLock lock(_mutex);
            for (auto const& [gid, group] : _groups) {
                PRtpTrans* shared = group->getSharedSession();
                if (shared && group->getMemberCount() == 0 &&
                    (now - shared->getLastActivityTime()) >= _sessionTimeout) {
                    staleGroupIds.push_back(gid);
                }
            }
        }
        for (const auto& gid : staleGroupIds) {
            LOG_INFO("CmpServer", "Group timeout: group=%s (no members, no activity) — auto cleanup", gid.c_str());
            PAutoLock lock(_mutex);
            auto it = _groups.find(gid);
            if (it != _groups.end()) {
                logFlow(gid, "cmp", "cmp", "INT", "GROUP_TIMEOUT", "");
                delete it->second;
                _groups.erase(it);
                _logDirs.erase(gid);
            }
        }
    }
}

void CmpServer::logFlow(const std::string& key, const char* from, const char* to,
                         const char* proto, const char* label, const char* body) {
    auto it = _logDirs.find(key);
    if (it == _logDirs.end() || it->second.empty()) return;

    // timestamp (HH:MM:SS.microsec)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    char tsBuf[32];
    snprintf(tsBuf, sizeof(tsBuf), "%02d:%02d:%02d.%06ld",
             t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000);

    // JSON escape helper (simple)
    auto esc = [](const char* s) -> std::string {
        if (!s) return "";
        std::string r;
        r.reserve(strlen(s) + 16);
        for (const char* p = s; *p; ++p) {
            switch (*p) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                case '\r': r += "\\r";  break;
                case '\t': r += "\\t";  break;
                default:
                    if ((unsigned char)*p < 0x20) {
                        char h[8]; snprintf(h, 8, "\\u%04x", (unsigned char)*p); r += h;
                    } else {
                        r += *p;
                    }
            }
        }
        return r;
    };

    std::string line =
        std::string("{\"ts\":\"") + tsBuf + "\","
        "\"from\":\"" + (from ? from : "") + "\","
        "\"to\":\"" + (to ? to : "") + "\","
        "\"proto\":\"" + (proto ? proto : "") + "\","
        "\"label\":\"" + esc(label) + "\","
        "\"body\":\"" + esc(body) + "\"}";

    std::string path = it->second + "/cmp.jsonl";
    FILE* f = fopen(path.c_str(), "a");
    if (f) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}
