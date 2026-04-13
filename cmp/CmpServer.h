#ifndef __CMP_SERVER_H__
#define __CMP_SERVER_H__

#include <string>
#include <map>
#include <iostream>
#include <thread>
//#include "pbase.h"
#include "pmodule.h"
#include "PRtpHandler.h"
#include "McpttGroup.h"
#include "SimpleJson.h"
#include "RtpRecorder.h"

class CmpServer : public PModule {
public:
    CmpServer(const std::string& name, const std::string& configFile = "cmp.conf");
    virtual ~CmpServer();

    bool startServer();
    void stopServer();

    void runControlLoop(); // Main loop for UDP control

protected:
    void handlePacket(char* buf, int len, const std::string& ip, int port);
    void processAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    // Group Management
    void processAddGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processJoinGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    void sendResponse(const std::string& ip, int port, const std::string& msg);

    // Resource Management
    void loadConfig();
    void initResourcePool();
    void initPttResourcePool();
    PRtpTrans* allocResource(std::string& rtpIp, int& rtpPort, int& videoPort);
    void freeResource(PRtpTrans* rtp);
    PPttTrans* allocPttResource(std::string& rtpIp, int& rtpPort, int& floorPort);
    void freePttResource(PPttTrans* ptt);

private:
    int _udpFd;
    bool _running;
    
    std::map<std::string, PRtpTrans*> _sessions;
    std::map<std::string, McpttGroup*> _groups;
    std::map<std::string, std::string> _logDirs;  // session_id/group_id → log_dir
    PMutex _mutex;

    // CMP flow 로그 기록 (log_dir/cmp.jsonl)
    void logFlow(const std::string& key, const char* from, const char* to,
                 const char* proto, const char* label, const char* body = "");

    // VoIP Resource Pool
    int _rtpStartPort;
    int _rtpPoolSize;
    std::string _rtpIp;

    // PTT Resource Pool
    int _pttRtpStartPort;
    int _pttRtpPoolSize;
    int _pttFloorStartPort;

    // Server Config
    std::string _serverIp;
    int _serverPort;

    // DTMF PTT Config
    bool _dtmfPttEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;

    std::string _configFile;

    // VoIP 리소스 (PRtpTrans, 4포트 블록)
    std::vector<PRtpTrans*> _resourcePool;
    std::vector<PRtpTrans*> _freeResources;

    // PTT 리소스 (PPttTrans, audio RTP + floor control)
    std::vector<PPttTrans*> _pttPool;
    std::vector<PPttTrans*> _freePttResources;

    // Worker config
    int _rtpWorkerCount;

    // Recording config
    bool _recordEnable;
    std::string _recordDir;

    // Session timeout (seconds, 0=disabled)
    int _sessionTimeout;

    // Timeout check thread
    std::thread _timeoutThread;
    void timeoutLoop();
};

#endif // __CMP_SERVER_H__
