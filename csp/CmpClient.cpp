#include "CmpClient.h"
#include "SipMessageLogger.h"
#include "SimpleJson.h"
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <vector>
#include "Log.h"

CCmpClient::CCmpClient() : m_iCmpPort(0), m_hSocket(-1), m_bKeepAliveRunning(false), m_bRecvRunning(false), m_iNextTransId(1), m_bConnected(false) {
}

CCmpClient::~CCmpClient() {
    m_bKeepAliveRunning = false;
    if (m_threadKeepAlive.joinable()) {
        m_threadKeepAlive.join();
    }
    
    m_bRecvRunning = false;
    if (m_hSocket != -1) {
        shutdown(m_hSocket, SHUT_RDWR);
        close(m_hSocket);
        m_hSocket = -1;
    }

    if (m_threadRecv.joinable()) {
        m_threadRecv.join();
    }
}

CCmpClient& CCmpClient::GetInstance() {
    static CCmpClient instance;
    return instance;
}

bool CCmpClient::Init(const std::string& strCmpIp, int iCmpPort, int iLocalPort) {
    // std::lock_guard<std::mutex> lock(m_mutex); // No global mutex needed for init if called once
    m_strCmpIp = strCmpIp;
    m_iCmpPort = iCmpPort;
    m_iLocalCmpPort = iLocalPort;

    if (m_hSocket == -1) {
        m_hSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_hSocket < 0) {
            CLog::Print(LOG_ERROR, "CmpClient::Init socket error");
            return false;
        }

        struct sockaddr_in localAddr;
        memset(&localAddr, 0, sizeof(localAddr));
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        localAddr.sin_port = htons(m_iLocalCmpPort);

        if (bind(m_hSocket, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0) {
            CLog::Print(LOG_ERROR, "CmpClient::Init bind error port=%d", m_iLocalCmpPort);
            close(m_hSocket);
            m_hSocket = -1;
            return false;
        }
        
        // Start Recv Thread
        m_bRecvRunning = true;
        m_threadRecv = std::thread(&CCmpClient::RecvLoop, this);
    }

    if (!m_bKeepAliveRunning) {
        m_bKeepAliveRunning = true;
        m_threadKeepAlive = std::thread(&CCmpClient::KeepAliveLoop, this);
    }

    return true;
}

// Helper to clean up transaction
void CCmpClient::OnTransactionComplete(unsigned int transId, bool success, const std::string& response) {
    if (transId == 0) return;
    
    std::shared_ptr<Transaction> pTrans = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutexTrans);
        auto it = m_mapTransactions.find(transId);
        if (it != m_mapTransactions.end()) {
            pTrans = it->second;
        }
    }

    if (pTrans) {
        std::lock_guard<std::mutex> transLock(pTrans->mutex);
        pTrans->strResponse = response;
        pTrans->bSuccess = success;
        pTrans->bCompleted = true;
        pTrans->cv.notify_one();
    }
}

// Protocol: TRANS_ID CSP_ID CSP_SESS_ID CMP_ID CMP_SESS_ID CMD PAYLOAD...
// Example: 1001 CSP_MAIN sess_1 CMP_MAIN 0 add 1.2.3.4 1000 0 0
// Response: 1001 CSP_MAIN sess_1 CMP_MAIN 0 OK ...
bool CCmpClient::SendRequestAndWait(const SimpleJson::JsonNode& payload, std::string& strResponse) {
    if (m_hSocket == -1) return false;

    unsigned int transId;
    // Use shared_ptr to prevent Use-After-Free if timeout occurs while RecvLoop is active
    std::shared_ptr<Transaction> pTrans = std::make_shared<Transaction>();
    {
        std::lock_guard<std::mutex> lock(m_mutexTrans);
        transId = m_iNextTransId++;
        pTrans->id = transId;
        m_mapTransactions[transId] = pTrans;
    }

    // Construct Packet (JSON wrapper)
    SimpleJson::JsonNode packet;
    packet.Set("trans_id", (int)transId);
    packet.Set("payload", payload);

    std::string strPacket = packet.ToString();
    CLog::Print(LOG_DEBUG, "CmpClient TX → %s:%d : %s", m_strCmpIp.c_str(), m_iCmpPort, strPacket.c_str());
    if (gclsSipLogger.IsEnabled()) {
        // Determine service from CMP command type
        std::string strCmd = payload.GetString("cmd");
        const char* pszSvc = "system";
        if (strCmd == "ADD_SESSION" || strCmd == "REMOVE_SESSION" || strCmd == "MODIFY_SESSION") {
            pszSvc = "phone";
        } else if (strCmd == "ADD_GROUP" || strCmd == "REMOVE_GROUP" || strCmd == "JOIN_GROUP" ||
                   strCmd == "LEAVE_GROUP" || strCmd == "MODIFY_GROUP") {
            pszSvc = "ptt";
        }
        gclsSipLogger.LogMessage("csp", "cmp", "JSON",
                                 strCmd.c_str(),
                                 (m_strCmpIp + ":" + std::to_string(m_iCmpPort)).c_str(),
                                 strPacket.c_str(), pszSvc);
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(m_iCmpPort);
    servaddr.sin_addr.s_addr = inet_addr(m_strCmpIp.c_str());

    if (sendto(m_hSocket, strPacket.c_str(), strPacket.length(), 0, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        CLog::Print(LOG_ERROR, "SendRequestAndWait sendto error");
        // Remove from map
        {
            std::lock_guard<std::mutex> mapLock(m_mutexTrans);
            m_mapTransactions.erase(transId);
        }
        return false;
    }
    
    // Wait
    bool bResult = false;
    // Scope for unique_lock
    {
        std::unique_lock<std::mutex> lock(pTrans->mutex);
        if (pTrans->cv.wait_for(lock, std::chrono::milliseconds(2000)) == std::cv_status::timeout) {
            CLog::Print(LOG_ERROR, "SendRequestAndWait Timeout (TransId=%d)", transId);
            bResult = false;
        } else {
            strResponse = pTrans->strResponse;
            bResult = pTrans->bSuccess;
            CLog::Print(LOG_DEBUG, "CmpClient RX ← %s", strResponse.c_str());
        }
    } 

    // Safe Cleanup: Remove from map. 
    // If RecvLoop holds a shared_ptr copy, the object won't be deleted yet.
    {
        std::lock_guard<std::mutex> mapLock(m_mutexTrans);
        m_mapTransactions.erase(transId);
    }
    
    return bResult;
}

// Overload for legacy string if needed, or remove. 
// Refactoring commands to call the new one.
// The original SendRequestAndWait is removed as per instruction to modify it.
// If a string-based SendRequestAndWait is still needed, it would be an overload.

bool CCmpClient::AddSession(const std::string& strSessionId, std::string& strLocalIp, int& iLocalPort, int& iLocalVideoPort,
                            const std::string& strRecordDir, const std::string& strLogDir,
                            const std::string& strCaller, const std::string& strCallee,
                            const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "ADD_SESSION");
    req.Set("session_id", strSessionId);
    req.Set("remote_ip", strRmtIp.empty() ? "0.0.0.0" : strRmtIp);
    req.Set("remote_port", iRmtPort);
    req.Set("remote_video_port", iRmtVideoPort);
    req.Set("peer_index", 0);
    if (!strRecordDir.empty()) req.Set("record_dir", strRecordDir);
    if (!strLogDir.empty()) req.Set("log_dir", strLogDir);
    if (!strCaller.empty()) req.Set("caller", strCaller);
    if (!strCallee.empty()) req.Set("callee", strCallee);

    // Header info (legacy args)
    req.Set("csp_id", "CSP_MAIN");
    req.Set("csp_sess_id", strSessionId);
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string strResp;
    
    CLog::Print(LOG_DEBUG, "CmpClient::AddSession: %s", req.ToString().c_str());

    std::string strReqBody = req.ToString();

    if (!SendRequestAndWait(req, strResp)) return false;


    // Response Body: CSP_MAIN <sessId> CMP_MAIN <cmpSess> OK <ip> <port> <vport>
    // The response is now expected to be JSON.
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse(strResp);
    if (respNode.type != SimpleJson::JSON_OBJECT) {
        CLog::Print(LOG_ERROR, "CmpClient::AddSession: Failed to parse JSON response: %s", strResp.c_str());
        return false;
    }

    if (respNode.Has("status") && respNode.Get("status").AsString() == "OK") {
        strLocalIp = respNode.Get("local_ip").AsString();
        iLocalPort = respNode.Get("local_port").AsInt();
        iLocalVideoPort = respNode.Get("local_video_port").AsInt();
        return true;
    }

    return false;
}

bool CCmpClient::ModifySession(const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort, int iPeerIdx) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "MODIFY_SESSION");
    req.Set("session_id", strSessionId);
    req.Set("remote_ip", strRmtIp);
    req.Set("remote_port", iRmtPort);
    req.Set("remote_video_port", iRmtVideoPort);
    req.Set("peer_index", iPeerIdx);
    req.Set("csp_id", "CSP_MAIN");
    req.Set("csp_sess_id", strSessionId);
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string strResp;
    std::string strReqBody = req.ToString();

    if (!SendRequestAndWait(req, strResp)) return false;

    return true;
}

bool CCmpClient::UpdateSession(const std::string& strSessionId, const std::string& strRmtIp, int iRmtPort, int iRmtVideoPort, int iPeerIdx, std::string& strLocalIp, int& iLocalPort) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "MODIFY_SESSION");
    req.Set("session_id", strSessionId);
    req.Set("remote_ip", strRmtIp);
    req.Set("remote_port", iRmtPort);
    req.Set("remote_video_port", iRmtVideoPort);
    req.Set("peer_index", iPeerIdx);

    // Header info (legacy args)
    req.Set("csp_id", "CSP_MAIN"); 
    req.Set("csp_sess_id", strSessionId);
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string strResp;
    
    CLog::Print(LOG_DEBUG, "CmpClient::UpdateSession: %s", req.ToString().c_str());

    if (!SendRequestAndWait(req, strResp)) return false;
    
    SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse(strResp);
    if (respNode.type != SimpleJson::JSON_OBJECT) {
        CLog::Print(LOG_ERROR, "CmpClient::UpdateSession: Failed to parse JSON response: %s", strResp.c_str());
        return false;
    }

    if (respNode.Has("status") && respNode.Get("status").AsString() == "OK") {
        strLocalIp = respNode.Get("local_ip").AsString();
        iLocalPort = respNode.Get("local_port").AsInt();
        // iLocalVideoPort is not returned by UpdateSession in the original code, but it's in AddSession.
        // Assuming it's not needed here or would be part of the response if the protocol changed.
        return true;
    }
    return false;
}

bool CCmpClient::RemoveSession(const std::string& strSessionId) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "REMOVE_SESSION");
    req.Set("session_id", strSessionId);

    // Header info (legacy args)
    req.Set("csp_id", "CSP_MAIN"); 
    req.Set("csp_sess_id", strSessionId);
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string strResp;
    CLog::Print(LOG_DEBUG, "CmpClient::RemoveSession: %s", req.ToString().c_str());
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait(req, strResp);
    return bRet;
}

bool CCmpClient::AddGroup(const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers, std::string& strIp, int& iPort, int& iFloorPort, int& iVideoPort,
                          const std::string& strRecordDir, const std::string& strLogDir, bool bVideoEnabled) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "ADD_GROUP");
    req.Set("group_id", strGroupId);
    req.Set("count", (int)vecMembers.size());
    if (!strRecordDir.empty()) req.Set("record_dir", strRecordDir);
    if (!strLogDir.empty()) req.Set("log_dir", strLogDir);
    if (bVideoEnabled) req.Set("video_enabled", 1);

    std::stringstream ssMembers;
    for(size_t i=0; i<vecMembers.size(); ++i) {
        if (!vecMembers[i]) continue;
        if(i>0) ssMembers << ",";
        ssMembers << vecMembers[i]->_id << ":" << vecMembers[i]->_priority;
    }
    req.Set("members", ssMembers.str());

    // Header info (legacy args)
    req.Set("csp_id", "CSP_MAIN"); 
    req.Set("csp_sess_id", "0");
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string strResp;
    std::string strReqBody = req.ToString();

    if (SendRequestAndWait(req, strResp)) {
        SimpleJson::JsonNode respNode = SimpleJson::JsonNode::Parse(strResp);
        if (respNode.type != SimpleJson::JSON_OBJECT) {
            CLog::Print(LOG_ERROR, "CmpClient::AddGroup: Failed to parse JSON response: %s", strResp.c_str());
            return false;
        }

        if (respNode.Has("status") && respNode.Get("status").AsString() == "OK") {
            strIp = respNode.Get("ip").AsString();
            iPort = respNode.Get("port").AsInt();
            iFloorPort = respNode.Has("floor_port") ? respNode.Get("floor_port").AsInt() : 0;
            iVideoPort = respNode.Has("video_port") ? respNode.Get("video_port").AsInt() : 0;
            CLog::Print(LOG_INFO, "CmpClient::AddGroup Success: %s:%d floor=%d video=%d Members: %d", strIp.c_str(), iPort, iFloorPort, iVideoPort, (int)vecMembers.size());
            return true;
        }
        CLog::Print(LOG_ERROR, "CmpClient::AddGroup Fail: Status not OK. Resp: %s", strResp.c_str());
    } else {
         CLog::Print(LOG_ERROR, "CmpClient::AddGroup SendRequest Failed");
    }
    return false;
}

bool CCmpClient::ModifyGroup(const std::string& strGroupId, const std::vector<std::shared_ptr<CspPttUser>>& vecMembers) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "MODIFY_GROUP");
    req.Set("group_id", strGroupId);
    
    std::stringstream ssMembers;
    for(size_t i=0; i<vecMembers.size(); ++i) {
        if (!vecMembers[i]) continue;
        if(i>0) ssMembers << ",";
        ssMembers << vecMembers[i]->_id << ":" << vecMembers[i]->_priority;
    }
    req.Set("members", ssMembers.str());

    // Header info (legacy args)
    req.Set("csp_id", "CSP_MAIN"); 
    req.Set("csp_sess_id", "0");
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");
    
    std::string strResp;
    return SendRequestAndWait(req, strResp);
}

bool CCmpClient::JoinGroup(const std::string& strGroupId, const std::string& strSessionId, const std::string& strUserIp, int iUserPort, int iFloorPort, int iVideoPort) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "JOIN_GROUP");
    req.Set("group_id", strGroupId);
    req.Set("session_id", strSessionId);
    req.Set("user_ip", strUserIp);
    req.Set("user_port", iUserPort);
    if (iFloorPort > 0) req.Set("user_floor_port", iFloorPort);
    if (iVideoPort > 0) req.Set("user_video_port", iVideoPort);
    
    req.Set("csp_id", "CSP_MAIN");
    req.Set("csp_sess_id", strSessionId); 
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string resp;
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait(req, resp);
    return bRet;
}

bool CCmpClient::LeaveGroup(const std::string& strGroupId, const std::string& strSessionId) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "LEAVE_GROUP");
    req.Set("group_id", strGroupId);
    req.Set("session_id", strSessionId);

    req.Set("csp_id", "CSP_MAIN");
    req.Set("csp_sess_id", strSessionId); 
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string resp;
    std::string strReqBody = req.ToString();
    bool bRet = SendRequestAndWait(req, resp);
    return bRet;
}

bool CCmpClient::RemoveGroup(const std::string& strGroupId) {
    SimpleJson::JsonNode req;
    req.Set("cmd", "REMOVE_GROUP");
    req.Set("group_id", strGroupId);

    // Header info (legacy args)
    req.Set("csp_id", "CSP_MAIN"); 
    req.Set("csp_sess_id", "0");
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");

    std::string strResp;
    return SendRequestAndWait(req, strResp);
}

bool CCmpClient::Alive() {
    SimpleJson::JsonNode req;
    req.Set("cmd", "HEARTBEAT");
    req.Set("csp_id", "CSP_MAIN"); 
    req.Set("csp_sess_id", "0");
    req.Set("cmp_id", "CMP_MAIN");
    req.Set("cmp_sess_id", "0");
    
    std::string resp;
    return SendRequestAndWait(req, resp);
}

void CCmpClient::RecvLoop() {
    char buffer[4096];
    struct sockaddr_in cliaddr;
    socklen_t len;

    while (m_bRecvRunning) {
        len = sizeof(cliaddr);
        int n = recvfrom(m_hSocket, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&cliaddr, &len);
        if (n > 0) {
            buffer[n] = '\0';
            std::string strPacket = buffer;
            // printf("RX: %s\n", strPacket.c_str());

            // New JSON Parsing
             SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(strPacket);
             if (root.type == SimpleJson::JSON_OBJECT) {
                 int transId = (int)root.GetInt("trans_id", 0);
                 std::string respBody;
                 if (root.Has("response")) {
                      SimpleJson::JsonNode r = root.Get("response");
                      if (r.type == SimpleJson::JSON_STRING) respBody = r.strValue; 
                      else respBody = r.ToString(); 
                 }
                 
                 if (gclsSipLogger.IsEnabled()) {
                     // 응답에서 원래 명령 추출 (trans_id로 매칭)
                     std::string strCmd;
                     {
                         std::lock_guard<std::mutex> lk(m_mutexTrans);
                         auto it = m_mapTransactions.find(transId);
                         if (it != m_mapTransactions.end()) {
                             // 요청 payload에서 cmd 추출
                             // trans_id → 원래 요청 매핑은 없으므로, 응답 body에서 status 확인
                         }
                     }
                     // 응답 body에서 상태 추출
                     bool bOk = (respBody.find("OK") != std::string::npos);
                     std::string strStatus = bOk ? "OK" : "ERROR";

                     // 서비스 결정: 응답에 SESSION/GROUP 포함 여부
                     const char* pszSvc = "system";
                     if (respBody.find("local_port") != std::string::npos ||
                         respBody.find("session") != std::string::npos) {
                         pszSvc = "phone";
                     } else if (respBody.find("floor") != std::string::npos ||
                                respBody.find("group") != std::string::npos) {
                         pszSvc = "ptt";
                     }
                     gclsSipLogger.LogMessage("cmp", "csp", "JSON",
                                              strStatus.c_str(),
                                              (m_strCmpIp + ":" + std::to_string(m_iCmpPort)).c_str(),
                                              respBody.c_str(), pszSvc);
                 }
                 // Notify Transaction matches
                 OnTransactionComplete(transId, true, respBody);
             } else {
                  CLog::Print(LOG_INFO, "CmpClient Non-JSON RX: %s", strPacket.c_str());
             }
        }
    }
}

// OnPacketReceived is deprecated/unused with new RecvLoop logic.

void CCmpClient::KeepAliveLoop() {
    while (m_bKeepAliveRunning) {
        // Use JSON Alive()
        bool bSuccess = Alive();
        
        if (bSuccess) {
            if (!m_bConnected) {
                m_bConnected = true;
                if (m_fnConnectionCallback) {
                    m_fnConnectionCallback(true);
                }
                CLog::Print(LOG_INFO, "CMP Connected");
            }
        } else {
            if (m_bConnected) {
                m_bConnected = false;
                if (m_fnConnectionCallback) {
                    m_fnConnectionCallback(false);
                }
                CLog::Print(LOG_INFO, "CMP Disconnected");
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}
