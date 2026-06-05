#ifndef __CMP_SERVER_H__
#define __CMP_SERVER_H__

#include <string>
#include <map>
#include <iostream>
#include <thread>
//#include "pbase.h"
#include "pmodule.h"
#include "PRtpRelay.h"
#include "PRtpMulticast.h"
#include "PMcpttGroup.h"
#include "SimpleJson.h"

class PCmpServer : public PModule {
public:
    PCmpServer(const std::string& name, const std::string& configFile = "cmp.conf");
    virtual ~PCmpServer();

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

    int sendResponse(const std::string& ip, int port, const std::string& msg,
                     const char* caller = "", const char* callee = "");

    // Resource Management
    void loadConfig();
    void initResourcePool();
    void initPttResourcePool();
    PRtpRelay* allocResource(std::string& rtpIp, int& rtpPort, int& videoPort);
    void freeResource(PRtpRelay* rtp);
    PRtpMulticast* allocPttResource(std::string& rtpIp, int& rtpPort, int& floorPort, int& videoPort);
    void freePttResource(PRtpMulticast* ptt);

private:
    int _udpFd;
    bool _running;
    
    std::map<std::string, PRtpRelay*> _sessions;
    std::map<std::string, PMcpttGroup*> _groups;
    std::map<std::string, std::string> _groupSubId;  // groupId → subid(session_seq)
    std::map<std::string, std::string> _sesidMap;    // sessionId 또는 groupId → sesid (flow 상관용)
    std::map<std::string, std::string> _serviceMap;  // sessionId 또는 groupId → service (payload 계승)
    PMutex _mutex;

    // sesid 발행 유틸: {caller}::cmp::{us_ts}::{counter}
    static std::string issueSesid(const std::string& caller);
    // key에 매핑된 sesid 조회, 없으면 발행하여 저장
    std::string getOrIssueSesid(const std::string& key, const std::string& caller);

    // CMP flow 로그 (통합 디렉터리: {ServiceLogDir}/YYYY/MM/DD/HH/cmp_01_{service}.flow.jsonl)
    std::string _serviceLogDir;
    std::string _systemId;      // 파일명용 (cmp_01)
    std::string _nodeName;      // flow node 필드용 (cmp)
    std::string _currentFlowHourDir;
    FILE* _flowFile;
    FILE* _msgFile;       // cmp_01_csp.jsonl (CSP↔CMP JSON 원문)
    int _msgSeq;
    int _lastRxSeq;       // 현재 요청의 원문 seq (logFlow에서 사용)

    void logFlow(const std::string& key, const char* from, const char* to,
                 const char* proto, const char* label, const char* detail = "",
                 const char* txId = "", const char* service = "",
                 const char* sesid = "", const char* subid = "",
                 int seq = 0, const char* iface = "",
                 const char* caller = "", const char* callee = "");
    int writeMsgLine(const char* ts, const char* dir, const char* peer, const char* proto, const char* msg,
                     const char* caller = "", const char* callee = "");
    void logBody(const char* dir, const char* peer, const char* proto, const char* msg);
    void ensureFlowHourDir();
    std::string getFlowHourDir();
    std::string getMsgHourDir();
    static std::string getTimestamp();
    static bool mkdirP(const std::string& path);

    // msg_log body
    std::string _msgLogDir;
    std::string _currentMsgHourDir;
    FILE* _bodyFile;

    // VoIP Resource Pool
    int _rtpStartPort;
    int _rtpPoolSize;
    std::string _rtpIp;

    // PTT Resource Pool
    int _pttRtpStartPort;
    int _pttRtpPoolSize;
    int _pttFloorStartPort;
    int _pttVideoStartPort;

    // Server Config
    std::string _serverIp;
    int _serverPort;

    // DTMF PTT Config
    bool _dtmfPttEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;

    std::string _configFile;

    // VoIP 리소스 (PRtpRelay, 4포트 블록)
    std::vector<PRtpRelay*> _resourcePool;
    std::vector<PRtpRelay*> _freeResources;

    // PTT 리소스 (PRtpMulticast, audio RTP + floor control)
    std::vector<PRtpMulticast*> _pttPool;
    std::vector<PRtpMulticast*> _freePttResources;

    // Worker config
    int _rtpWorkerCount;

    // Recording config
    bool _recordEnable;
    std::string _recordDir;
    int _segmentIntervalSec;  // VoLTE 세그먼트 회전 간격 (초, 기본 60)

    // Session timeout (seconds, 0=disabled)
    int _sessionTimeout;
    // 고아(RTP 무수신) relay 회수 시간(초) — setup 실패/실패호 누수 방지. 활성/홀드 호는 _sessionTimeout 적용.
    int _orphanReclaimSec;

    // Flow 로깅 enable flags (cmp.json: Logging.Flow.{floor,dtmf,rtcp})
    bool _logFlowFloor;   // MCPTT floor control RTCP APP 메시지 기록 여부
    bool _logFlowDtmf;    // RFC 2833/4733 DTMF 이벤트 기록 여부
    bool _logFlowRtcp;    // 일반 RTCP(SR/RR/SDES/BYE) 기록 여부
public:
    bool getLogFlowFloor() const { return _logFlowFloor; }
    bool getLogFlowDtmf()  const { return _logFlowDtmf; }
    bool getLogFlowRtcp()  const { return _logFlowRtcp; }

    // Timeout check thread
    std::thread _timeoutThread;
    void timeoutLoop();
};

#endif // __CMP_SERVER_H__
