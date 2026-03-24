#include "CscInterface.h"
#include "Log.h"
#include "SipServer.h"
#include "GroupCallService.h"

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
            ProcessMessage(strMsg);
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
void CCscInterface::ProcessMessage(const std::string& strMsg) {
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
        // Reload group config and re-sync CMP sessions / re-invite members
        gclsGroupCallService.OnGroupConfigChanged();
    } else if (strEvent == "user_change") {
        extern void SendSipNotify(const std::string& uri, const std::string& etag, const std::string& action);
        SendSipNotify(strUri, strEtag, strAction);
    }
}
