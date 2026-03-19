#include "CmpServer.h"
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
        // delete group; // PModule/destructor might be an issue. McpttGroup should be deleted clearly.
        // Actually, _groups stores McpttGroup* which we new'd.
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
        perror("socket");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_serverIp.c_str());
    addr.sin_port = htons(_serverPort);

    if (bind(_udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(_udpFd);
        return false;
    }

    _running = true;
    std::thread([this]() {
        this->runControlLoop();
    }).detach();

    return true;
}

void CmpServer::stopServer() {
    _running = false;
    if (_udpFd >= 0) {
        ::close(_udpFd);
        _udpFd = -1;
    }
}

void CmpServer::runControlLoop() {
    char buf[4096];
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    while (_running) {
        int len = recvfrom(_udpFd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&clientAddr, &addrLen);
        if (len > 0) {
            buf[len] = '\0';
            //printf("[CmpServer] Recv %d bytes from %s:%d: %s\n", len, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port), buf);
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
        printf("[CmpServer] Invalid JSON Packet: %s\n", strPacket.c_str());
        return;
    }

    int transId = (int)root.GetInt("trans_id", 0);
    SimpleJson::JsonNode payload = root.Get("payload");
    
    if (payload.type != SimpleJson::JSON_OBJECT) {
        printf("[CmpServer] Missing Payload: %s\n", strPacket.c_str());
        return;
    }

    // Extract CMD
    std::string cmd = payload.GetString("cmd");
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower); // normalize

    // Dispatch
    //printf("[CmpServer] Dispatching cmd=%s transId=%d\n", cmd.c_str(), transId);
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
        printf("[CmpServer] Unknown CMD: %s\n", cmd.c_str());
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Unknown Command");
        sendResponse(ip, port, resp.ToString());
    }
}

void CmpServer::sendResponse(const std::string& ip, int port, const std::string& msg) {
    //printf("[CmpServer] Sending %d bytes to %s:%d: %s\n", msg.length(), ip.c_str(), port, msg.c_str());
    if (_udpFd != -1) {
        struct sockaddr_in cliaddr;
        memset(&cliaddr, 0, sizeof(cliaddr));
        cliaddr.sin_family = AF_INET;
        cliaddr.sin_port = htons(port);
        cliaddr.sin_addr.s_addr = inet_addr(ip.c_str());
        int sent = sendto(_udpFd, msg.c_str(), msg.length(), 0, (struct sockaddr*)&cliaddr, sizeof(cliaddr));
        //printf("[CmpServer] Sent %d bytes to %s:%d: %s\n", sent, ip.c_str(), port, msg.c_str());
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
        
        printf("[ADD] ID=%s Rmt=%s:%d -> Loc=%s:%d\n", sessionId.c_str(), rmtIp.c_str(), rmtPort, rtpIp.c_str(), rtpPort);
    } else {
         SimpleJson::JsonNode resp;
         resp.Set("trans_id", transId);
         resp.Set("response", "ERROR No Resource");
         sendResponse(ip, port, resp.ToString());
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
    }
    
    SimpleJson::JsonNode resp;
    resp.Set("trans_id", transId);
    resp.Set("response", "OK");
    sendResponse(ip, port, resp.ToString());
}

void CmpServer::processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
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
        } else {
             delete group;
             group = NULL;
        }
    } else {
        group = _groups[groupId];
        sharedSession = group->getSharedSession();
        if (sharedSession) {
            sharedPort = sharedSession->getLocalPort();
        }
    }

    printf("[CmpServer] processAddGroup Group=%p SharedSession=%p Port=%d\n", group, sharedSession, sharedPort);

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

    PAutoLock lock(_mutex);
    if (_groups.find(groupId) != _groups.end()) {
        McpttGroup* group = _groups[groupId];
        group->addMember(sessionId, userIp, userPort);
        
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "OK");
        sendResponse(ip, port, resp.ToString());
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
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
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
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
    } else {
        SimpleJson::JsonNode resp;
        resp.Set("trans_id", transId);
        resp.Set("response", "ERROR Group Not Found");
        sendResponse(ip, port, resp.ToString());
    }
}

void CmpServer::processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId) {
     processAddGroup(payload, ip, port, transId);
}

void CmpServer::loadConfig() {
    std::ifstream t(_configFile);
    if (!t.is_open()) {
        if (_configFile.find(".json") != std::string::npos) {
             printf("Failed to open config file: %s\n", _configFile.c_str());
             return;
        }
        // Fallback to old logic if file not json or failed? 
        // For now, assume if extension is .json, we use JSON loader.
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
             // Supports boolean or int in JSON
             SimpleJson::JsonNode val = root.Get("EnableDtmfPtt");
             // Minimal parser stores bool as ??? 
             // Int check:
             // Our parser doesn't have explicit BOOL type, likely stores "true"/"false" as string or 0/1 as int.
             // Let's check string, then int.
             std::string sVal = root.GetString("EnableDtmfPtt");
             if (sVal == "true") _dtmfPttEnable = true;
             else if (sVal == "false") _dtmfPttEnable = false;
             else _dtmfPttEnable = (root.GetInt("EnableDtmfPtt") != 0);
        }
        
        if (root.Has("DtmfPushDigit")) _dtmfPushDigit = root.GetString("DtmfPushDigit");
        if (root.Has("DtmfReleaseDigit")) _dtmfReleaseDigit = root.GetString("DtmfReleaseDigit");
        
        // CspIp/Port if needed
        
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

    printf("Config: RtpStartPort=%d, RtpPoolSize=%d, RtpIp=%s, ServerIp=%s, ServerPort=%d, DtmfPtt=%d Push=%s Rel=%s\n", 
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
             printf("Failed to init resource on port %d\n", currentPort);
             delete rtp;
        }
        currentPort += 4;
    }
    printf("Initialized %lu resources\n", _resourcePool.size());
}

PRtpTrans* CmpServer::allocResource(std::string& rtpIp, int& rtpPort, int& videoPort) {
    if (_freeResources.empty()) return NULL;
    
    PRtpTrans* rtp = _freeResources.back();
    _freeResources.pop_back();
    
    rtpIp = _rtpIp; 
    rtpPort = rtp->getLocalPort(); 
    videoPort = rtp->getLocalVideoPort();
    
    printf("[CmpServer] allocResource: Returning port %d (Remaining %lu)\n", rtpPort, _freeResources.size());
    return rtp;
}

void CmpServer::freeResource(PRtpTrans* rtp) {
    if (rtp) {
        printf("[CmpServer] freeResource: Freeing port %d\n", rtp->getLocalPort());
        _freeResources.push_back(rtp);
    }
}
