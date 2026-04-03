#ifndef __CMP_SERVER_H__
#define __CMP_SERVER_H__

#include <string>
#include <map>
#include <iostream>
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
    PRtpTrans* allocResource(std::string& rtpIp, int& rtpPort, int& videoPort);
    void freeResource(PRtpTrans* rtp);

private:
    int _udpFd;
    bool _running;
    
    std::map<std::string, PRtpTrans*> _sessions;
    std::map<std::string, McpttGroup*> _groups;
    PMutex _mutex;

    // Resource Pool
    int _rtpStartPort;
    int _rtpPoolSize;
    std::string _rtpIp;
    
    // Server Config
    std::string _serverIp;
    int _serverPort;
    
    // DTMF PTT Config
    bool _dtmfPttEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;

    std::string _configFile;

    std::vector<PRtpTrans*> _resourcePool; // Pre-allocated list
    std::vector<PRtpTrans*> _freeResources; // Available for use

    // Recording config
    bool _recordEnable;
    std::string _recordDir;
};

#endif // __CMP_SERVER_H__
