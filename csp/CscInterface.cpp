#include "CscInterface.h"
#include "Log.h"
#include "SipServer.h"
#include "GroupCallService.h"
#include "CspUser.h"
#include "DbManager.h"
#include "ModuleDispatcher.h"
#include "CallMap.h"
#include "UserMap.h"
#include "RtpMap.h"
#include "SipServerSetup.h"
#include "CallDir.h"

#include <sstream>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

#include <iostream>
#include <vector>

CCscInterface gclsCscInterface;

CCscInterface::CCscInterface() : m_iPort(0), m_iServerSock(-1), m_bRunning(false) {
}

CCscInterface::~CCscInterface() {
    Stop();
}

bool CCscInterface::Start(int iPort) {
    m_iPort = iPort;
    m_bRunning = true;
    m_threadListener = std::thread(&CCscInterface::ListenerLoop, this);
    CLog::Print(LOG_INFO, "CscInterface Started on Port %d", m_iPort);
    return true;
}

void CCscInterface::Stop() {
    m_bRunning = false;
    if (m_iServerSock != -1) {
#ifdef WIN32
        closesocket(m_iServerSock);
#else
        close(m_iServerSock);
#endif
        m_iServerSock = -1;
    }
    if (m_threadListener.joinable()) {
        m_threadListener.join();
    }
}

void CCscInterface::ListenerLoop() {
    struct sockaddr_in serverAddr;
    
#ifdef WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // UDP Socket
    m_iServerSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_iServerSock == -1) {
        CLog::Print(LOG_ERROR, "CscInterface: Socket creation failed");
        return;
    }

    // Reuse Address
    int opt = 1;
    setsockopt(m_iServerSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(m_iPort);

    if (bind(m_iServerSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        CLog::Print(LOG_ERROR, "CscInterface: Bind failed port %d", m_iPort);
        return;
    }

    // No Listen for UDP

    CLog::Print(LOG_INFO, "CscInterface: UDP Listener Started...");

    char buffer[4096];
    struct sockaddr_in clientAddr;
#ifdef WIN32
    int clientLen = sizeof(clientAddr);
#else
    socklen_t clientLen = sizeof(clientAddr);
#endif

    while (m_bRunning) {
        int bytesRead = recvfrom(m_iServerSock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&clientAddr, &clientLen);
        
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            std::string strMsg(buffer);
            ProcessMessage(strMsg, clientAddr);
        } else if (bytesRead < 0) {
            // Error or Timeout
            // CLog::Print(LOG_ERROR, "CscInterface: Recvfrom failed");
            // Break if socket closed?
            if (!m_bRunning) break;
        }
    }
}

// Simple JSON Parser
// Expected: {"event": "group_change", "uri": "tel:+...", "action": "PUT", "etag": "..."}
void CCscInterface::ProcessMessage(const std::string& strMsg, const struct sockaddr_in& clientAddr) {
    // Helper lambda to get value by key
    auto getVal = [&](const std::string& key) -> std::string {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = strMsg.find(searchKey);
        if (pos == std::string::npos) return "";
        
        pos = strMsg.find(":", pos);
        if (pos == std::string::npos) return "";
        
        size_t startQuote = strMsg.find("\"", pos);
        if (startQuote == std::string::npos) return "";
        
        size_t endQuote = strMsg.find("\"", startQuote + 1);
        if (endQuote == std::string::npos) return "";
        
        return strMsg.substr(startQuote + 1, endQuote - startQuote - 1);
    };

    std::string strEvent = getVal("event");
    std::string strUri = getVal("uri");
    std::string strAction = getVal("action");
    std::string strEtag = getVal("etag");

    CLog::Print(LOG_INFO, "CscInterface Event: %s, URI: %s, Action: %s, ETag: %s", 
        strEvent.c_str(), strUri.c_str(), strAction.c_str(), strEtag.c_str());

    if (strEvent == "group_change") {
        extern void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);
        SendSipNotify(strUri, strEtag, strAction);
        // Log config_change event to active PTT session history
        {
            // Extract group ID from URI (strip "tel:" prefix if present)
            std::string strGroupId = strUri;
            if (strGroupId.substr(0, 4) == "tel:") strGroupId = strGroupId.substr(4);
            if (gclsCallDir.IsEnabled()) {
                gclsCallDir.PttLogEvent(strGroupId, "config_change", "{\"action\":\"" + strAction + "\"}");
            }
        }
        // Reload group config and re-sync CMP sessions / re-invite members
        gclsGroupCallService.OnGroupConfigChanged();
    } else if (strEvent == "stats") {
        // stats 요청 → 현재 CSP 상태를 JSON으로 응답
        USER_ID_LIST regList;
        gclsUserMap.GetRegisteredUsers(regList);
        int regUsers = (int)regList.size();
        // active_calls: DB 기반 정확한 수 (B2BUA + Proxy 모두 포함)
        int activeCalls = 0;
        bool dbConnected = gclsDbManager.IsConnected();
        if (dbConnected) {
            activeCalls = gclsDbManager.GetActiveVoipCallCount();
        } else {
            activeCalls = gclsCallMap.GetCount();
        }

        std::ostringstream oss;
        oss << "{\"status\":\"OK\""
            << ",\"registered_users\":" << regUsers
            << ",\"active_calls\":" << activeCalls
            << ",\"db_connected\":" << (dbConnected ? "true" : "false")
            << ",\"roles\":{\"CSCF\":" << (gclsSetup.m_bRoleCscf ? "true" : "false")
            << ",\"TAS\":" << (gclsSetup.m_bRoleTas ? "true" : "false")
            << ",\"PTT_AS\":" << (gclsSetup.m_bRolePttAs ? "true" : "false")
            << ",\"IBCF\":" << (gclsSetup.m_bRoleIbcf ? "true" : "false")
            << "}"
            << ",\"timeouts\":{\"user_timeout\":" << gclsSetup.m_iUserTimeout
            << ",\"stale_call_timeout\":" << gclsSetup.m_iStaleCallTimeout
            << ",\"send_options_period\":" << gclsSetup.m_iSendOptionsPeriod
            << "}"
            << ",\"record_enable\":" << (gclsSetup.m_bRecordEnable ? "true" : "false")
            << "}";

        std::string resp = oss.str();
        sendto(m_iServerSock, resp.c_str(), resp.size(), 0,
               (const struct sockaddr*)&clientAddr, sizeof(clientAddr));

        CLog::Print(LOG_INFO, "CscInterface: Stats response sent (reg=%d calls=%d)", regUsers, activeCalls);
    } else if (strEvent == "user_change") {
        extern void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);
        SendSipNotify(strUri, strEtag, strAction);

        // 가입자 캐시 즉시 갱신
        std::string strUserId = strUri;
        // tel:+821001 → 821001
        if (strUserId.substr(0, 5) == "tel:+") {
            strUserId = strUserId.substr(5);
        } else if (strUserId.substr(0, 4) == "tel:") {
            strUserId = strUserId.substr(4);
        }

        if (strAction == "DELETE") {
            gclsCspUserMap.Remove(strUserId);
            CLog::Print(LOG_INFO, "CscInterface: User cache removed [%s]", strUserId.c_str());
        } else {
            // POST (신규) 또는 PUT (수정) — DB에서 다시 읽어 캐시 갱신
            if (gclsCspUserMap.ReloadFromDb(strUserId)) {
                CLog::Print(LOG_INFO, "CscInterface: User cache updated [%s]", strUserId.c_str());
            } else {
                CLog::Print(LOG_ERROR, "CscInterface: User not found in DB [%s]", strUserId.c_str());
            }
        }
    }
}
