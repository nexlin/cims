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
                    const std::string& strRmtIp = "", int iRmtPort = 0, int iRmtVideoPort = 0);
    bool ModifySession(const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort, int iPeerIdx);
    bool UpdateSession(const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort, int iPeerIdx, std::string& strLocalIp, int& iLocalPort);
    bool RemoveSession(const std::string& strSessionId);
    bool Alive();


    bool AddGroup(const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers, std::string& strIp, int& iPort, int& iFloorPort, int& iVideoPort,
                  const std::string& strRecordDir = "", const std::string& strLogDir = "", bool bVideoEnabled = false);
    bool ModifyGroup(const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers);
    bool JoinGroup(const std::string& strGroupId, const std::string& strSessionId, const std::string& strIp, int iPort, int iFloorPort = 0, int iVideoPort = 0);
    bool LeaveGroup(const std::string& strGroupId, const std::string& strSessionId);
    bool RemoveGroup(const std::string& strGroupId);

private:
    CCmpClient();
    ~CCmpClient();

    // Async Request/Response
    struct Transaction {
        unsigned int id;
        std::string strResponse; 
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
