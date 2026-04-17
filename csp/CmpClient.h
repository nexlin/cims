#ifndef __CMP_CLIENT_H__
#define __CMP_CLIENT_H__

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include "SipStackDefine.h"
#include "CspPttGroup.h"
#include "SimpleJson.h"

struct CmpSocket {
    int iSocket;
    bool bInUse;
};

class CCmpClient {
public:
    static CCmpClient& GetInstance();

    bool Init(const std::string& strCmpIp, int iCmpPort, int iLocalPort);
    
    // Returns assigned local IP/Port from CMP
    bool AddSession(const std::string& strSessionId, std::string& strLocalIp, int& iLocalPort, int& iLocalVideoPort,
                    const std::string& strRecordDir = "", const std::string& strLogDir = "",
                    const std::string& strCaller = "", const std::string& strCallee = "",
                    const std::string& strRmtIp = "", int iRmtPort = 0, int iRmtVideoPort = 0,
                    const std::string& strSesId = "");
    bool ModifySession(const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort, int iPeerIdx,
                       const std::string& strCaller = "", const std::string& strCallee = "",
                       const std::string& strSesId = "");
    bool UpdateSession(const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort, int iPeerIdx,
                       const std::string& strCaller, const std::string& strCallee,
                       std::string& strLocalIp, int& iLocalPort,
                       const std::string& strSesId = "");
    bool RemoveSession(const std::string& strSessionId,
                       const std::string& strCaller = "", const std::string& strCallee = "",
                       const std::string& strSesId = "");
    bool Alive();


    bool AddGroup(const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers, std::string& strIp, int& iPort, int& iFloorPort, int& iVideoPort,
                  const std::string& strRecordDir = "", const std::string& strLogDir = "", bool bVideoEnabled = false,
                  int iSessionSeq = 0,
                  const std::string& strSesId = "");
    bool ModifyGroup(const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers,
                     const std::string& strSesId = "");
    bool JoinGroup(const std::string& strGroupId, const std::string& strSessionId, const std::string& strIp, int iPort, int iFloorPort = 0, int iVideoPort = 0,
                   const std::string& strSesId = "");
    bool LeaveGroup(const std::string& strGroupId, const std::string& strSessionId,
                    const std::string& strSesId = "");
    bool RemoveGroup(const std::string& strGroupId,
                     const std::string& strSesId = "");

    /** 세션/그룹별 기 발행된 sesid 조회 (없으면 빈문자열) */
    std::string GetSesIdByKey(const std::string& strKey);

private:
    CCmpClient();
    ~CCmpClient();

    // Async Request/Response
    struct Transaction {
        unsigned int id;
        std::string strResponse;
        std::string strSesId;    // flow sesid (응답 기록용)
        std::string strService;  // flow service (응답 기록용)
        std::string strCaller;   // 발신 MSISDN (응답 기록용)
        std::string strCallee;   // 착신 MSISDN (응답 기록용)
        std::condition_variable cv;
        std::mutex mutex;
        bool bCompleted;
        bool bSuccess;
        Transaction() : id(0), bCompleted(false), bSuccess(false) {}
    };

    bool SendRequestAndWait(const SimpleJson::JsonNode& payload, std::string& strResponse);

    // Threads
    void KeepAliveLoop();
    void RecvLoop();
    void OnTransactionComplete(unsigned int transId, bool success, const std::string& response);
    // void OnPacketReceived(const std::string& strPacket, const std::string& strIp, int iPort); // Deprecated

    std::string m_strCmpIp;
    int m_iCmpPort;
    int m_iLocalCmpPort;
    
    // Single Socket
    int m_hSocket;

    // Transaction Map
    std::mutex m_mutexTrans;
    std::map<unsigned int, std::shared_ptr<Transaction>> m_mapTransactions;
    unsigned int m_iNextTransId;

    // session_id/group_id → sesid 캐시 (Modify/Remove 시 재사용)
    std::mutex m_mutexSesid;
    std::map<std::string, std::string> m_mapKeyToSesid;

    // Threads
    std::atomic<bool> m_bKeepAliveRunning;
    std::thread m_threadKeepAlive;
    
    std::atomic<bool> m_bRecvRunning;
    std::thread m_threadRecv;

    // Connection State
    bool m_bConnected;
    std::function<void(bool)> m_fnConnectionCallback;

public:
    void SetConnectionCallback(std::function<void(bool)> fnCallback) {
        m_fnConnectionCallback = fnCallback;
    }
    bool IsConnected() const { return m_bConnected; }
};

#define gclsCmpClient CCmpClient::GetInstance()

#endif
