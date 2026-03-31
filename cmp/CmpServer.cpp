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
    : PModule(name), _running(false), _udpFd(-1), _configFile(configFile)
{
    loadConfig();
    initResourcePool();

    // Initialize a few workers for RTP processing
    for(int i=0; i<4; ++i) {
        std::string wname = formatStr("RtpWorker_%d", i);
        addWorker(wname, 1, 2048, true);
    }
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

    LOG_INFO("CmpServer", "Server listening on %s:%d", _serverIp.c_str(), _serverPort);
    return true;
}

void CmpServer::stopServer() {
    _running = false;
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
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower); // normalize

    // Dispatch
    LOG_DEBUG("CmpServer", "Dispatching cmd=%s transId=%d from %s:%d", cmd.c_str(), transId, ip.c_str(), port);
    if (cmd == "add") processAdd(payload, ip, port, transId);
    else if (cmd == "remove") processRemove(payload, ip, port, transId);
    else if (cmd == "alive") processAlive(payload, ip, port, transId);
    else if (cmd == "addgroup") processAddGroup(payload, ip, port, transId);
    else if (cmd == "joingroup") processJoinGroup(payload, ip, port, transId);
    else if (cmd == "leavegroup") processLeaveGroup(payload, ip, port, transId);
    else if (cmd == "removegroup") processRemoveGroup(payload, ip, port, transId);
    else if (cmd == "modifygroup") processModifyGroup(payload, ip, port, transId);
    else if (cmd == "modify") processModify(payload, ip, port, transId);
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
        rtpPort = rtp->getLocalPort(); // reuse existing
        videoPort = rtp->getLocalVideoPort();
    }

    if (rtp) {
        if (rmtPort > 0) {
             rtp->setRmt(rmtIp, rmtPort, rmtVideoPort, peerIdx);
        }
        
        static int workerIdx = 0;
        if (rtp->getWorkerName().empty()) {
             std::string wname = formatStr("RtpWorker_%d", workerIdx++ % 4);
             rtp->setWorkerName(wname);
             addHandler(wname, rtp);
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
        
        LOG_INFO("CmpServer", "ADD session=%s remote=%s:%d -> local=%s:%d", sessionId.c_str(), rmtIp.c_str(), rmtPort, rtpIp.c_str(), rtpPort);
    } else {
         SimpleJson::JsonNode resp;
         resp.Set("trans_id", transId);
         resp.Set("response", "ERROR No Resource");
         sendResponse(ip, port, resp.ToString());
         LOG_WARN("CmpServer", "ADD session=%s FAILED: no available resource", sessionId.c_str());
    }
}

void CmpServer::processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string sessionId = payload.GetString("session_id");
    PAutoLock lock(_mutex);
    if (_sessions.find(sessionId) != _sessions.end()) {
        PRtpTrans* rtp = _sessions[sessionId];
        delHandler(rtp->getWorkerName(), rtp);
        rtp->reset();
        freeResource(rtp);
        _sessions.erase(sessionId);
        LOG_INFO("CmpServer", "REMOVE session=%s", sessionId.c_str());
    } else {
        LOG_WARN("CmpServer", "REMOVE session=%s not found", sessionId.c_str());
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
        sharedSession = allocResource(sharedIp, sharedPort, sharedVideoPort);
        if (sharedSession) {
             sharedSession->setGroup(group);
             group->setDtmfConfig(_dtmfPttEnable, _dtmfPushDigit, _dtmfReleaseDigit);
             group->setSharedSession(sharedSession);
             _groups[groupId] = group;

             static int workerIdx = 0;
             if (sharedSession->getWorkerName().empty()) {
                  std::string wname = formatStr("RtpWorker_%d", workerIdx++ % 4);
                  sharedSession->setWorkerName(wname);
                  addHandler(wname, sharedSession);
             }
             LOG_INFO("CmpServer", "ADDGROUP group=%s port=%d (new)", groupId.c_str(), sharedPort);
        } else {
             delete group;
             group = NULL;
             LOG_WARN("CmpServer", "ADDGROUP group=%s FAILED: no available resource", groupId.c_str());
        }
    } else {
        group = _groups[groupId];
        sharedSession = group->getSharedSession();
        if (sharedSession) {
            sharedPort = sharedSession->getLocalPort();
            sharedVideoPort = sharedSession->getLocalVideoPort();
            sharedIp = _rtpIp;  // 기존 그룹도 RTP IP 사용
        }
        LOG_DEBUG("CmpServer", "ADDGROUP group=%s port=%d (existing)", groupId.c_str(), sharedPort);
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
        
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
        LOG_INFO("CmpServer", "JOINGROUP group=%s session=%s %s:%d", groupId.c_str(), sessionId.c_str(), userIp.c_str(), userPort);
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
        LOG_WARN("CmpServer", "JOINGROUP group=%s not found", groupId.c_str());
    }
}

void CmpServer::processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    std::string sessionId = payload.GetString("session_id");

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        McpttGroup* group = _groups[groupId];
        group->removeMember(sessionId);
        
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
        LOG_INFO("CmpServer", "LEAVEGROUP group=%s session=%s", groupId.c_str(), sessionId.c_str());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
        LOG_WARN("CmpServer", "LEAVEGROUP group=%s not found", groupId.c_str());
    }
}

void CmpServer::processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
    std::string groupId = payload.GetString("group_id");
    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        McpttGroup* group = _groups[groupId];
        delete group;
        _groups.erase(groupId);
        
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
        LOG_INFO("CmpServer", "REMOVEGROUP group=%s", groupId.c_str());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
        LOG_WARN("CmpServer", "REMOVEGROUP group=%s not found", groupId.c_str());
    }
}

void CmpServer::processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
     LOG_DEBUG("CmpServer", "MODIFYGROUP -> delegating to processAddGroup");
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

    LOG_INFO("CmpServer", "Config: RtpStartPort=%d RtpPoolSize=%d RtpIp=%s ServerIp=%s ServerPort=%d DtmfPtt=%d Push=%s Rel=%s", 
           _rtpStartPort, _rtpPoolSize, _rtpIp.c_str(), _serverIp.c_str(), _serverPort, 
           _dtmfPttEnable, _dtmfPushDigit.c_str(), _dtmfReleaseDigit.c_str());
}

void CmpServer::initResourcePool() {
    int currentPort = _rtpStartPort;
    for (int i = 0; i < _rtpPoolSize; ++i) {
        std::string name = formatStr("InActiveRtp_%d", i);
        PRtpTrans* rtp = new PRtpTrans(name);
        
        if (rtp->init(_rtpIp, currentPort, currentPort + 2)) {
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
