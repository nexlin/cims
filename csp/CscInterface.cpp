#include "CscInterface.h"
#include "SipMessageLogger.h"
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
#include "CspConfigCache.h"
#include "CspListenerManager.h"
#include "CspTrunkManager.h"

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
    std::string strTransId = getVal("trans_id");
    std::string strSesId = getVal("sesid");
    std::string strService = getVal("service");
    // CSC가 sesid/service를 안 보낸 경우 보수적 기본값 적용
    if (strSesId.empty()) {
        strSesId = CSipMessageLogger::IssueSesId("", "csp");
    }
    if (strService.empty()) strService = "system";

    // caller 파생: uri 에서 추출 (tel:+82... 또는 sip:user@domain)
    std::string strCaller;
    if (!strUri.empty()) {
        if (strUri.compare(0, 4, "tel:") == 0) strCaller = strUri.substr(4);
        else if (strUri.compare(0, 4, "sip:") == 0) {
            std::string tail = strUri.substr(4);
            size_t at = tail.find('@');
            strCaller = (at != std::string::npos) ? tail.substr(0, at) : tail;
        }
    }

    CLog::Print(LOG_INFO, "CscInterface Event: %s, URI: %s, Action: %s, TransId: %s, SesId: %s, Service: %s",
        strEvent.c_str(), strUri.c_str(), strAction.c_str(),
        strTransId.c_str(), strSesId.c_str(), strService.c_str());

    // CSC admin 메시지를 SIP 로그에 기록 (sesid/service/caller 포함)
    {
        char peerBuf[64];
        snprintf(peerBuf, sizeof(peerBuf), "%s:%d",
                 inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
        std::string strLabel = strEvent + "(" + strAction + ")";
        gclsSipLogger.LogMessage("csc", "csp", "CSC", strLabel.c_str(), peerBuf, strMsg.c_str(),
                                 strService.c_str(),
                                 strTransId.c_str(), strSesId.c_str(),
                                 "", strCaller.c_str(), "");
    }

    if (strEvent == "GROUP_CHANGED") {
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
    } else if (strEvent == "STATS_REQUEST") {
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
            << ",\"record_enable\":" << (gclsSetup.m_bRecordEnable ? "true" : "false");

        // 트렁크 상태
        {
            std::vector<CCspTrunkManager::StatusEntry> trunks;
            gclsTrunkManager.GetStatus(trunks);
            oss << ",\"trunks\":[";
            for (size_t i = 0; i < trunks.size(); ++i) {
                const auto& t = trunks[i];
                if (i) oss << ",";
                oss << "{\"id\":" << t.id
                    << ",\"name\":\"" << t.name << "\""
                    << ",\"remote\":\"" << t.remote << "\""
                    << ",\"enabled\":" << (t.enabled ? "true" : "false")
                    << ",\"alive\":" << (t.alive ? "true" : "false")
                    << ",\"last_rtt_ms\":" << t.last_rtt_ms
                    << ",\"last_ping\":" << (long long)t.last_ping
                    << ",\"last_reply\":" << (long long)t.last_reply
                    << ",\"fail_count\":" << t.fail_count
                    << "}";
            }
            oss << "]";
        }
        oss << "}";

        std::string resp = oss.str();

        // TX 로그: CSC에 응답 전송 기록 (요청의 sesid/service 계승)
        {
            char peerBuf[64];
            snprintf(peerBuf, sizeof(peerBuf), "%s:%d",
                     inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
            gclsSipLogger.LogMessage("csp", "csc", "CSC", "STATS_RESPONSE", peerBuf, resp.c_str(),
                                     strService.c_str(),
                                     strTransId.c_str(), strSesId.c_str());
        }

        sendto(m_iServerSock, resp.c_str(), resp.size(), 0,
               (const struct sockaddr*)&clientAddr, sizeof(clientAddr));

        CLog::Print(LOG_INFO, "CscInterface: Stats response sent (reg=%d calls=%d)", regUsers, activeCalls);
    } else if (strEvent == "CSC_RESTART") {
        CLog::Print(LOG_INFO, "CscInterface: CSC_RESTART received — resyncing all group and user state from DB");

        // Resync user map from DB
        gclsCspUserMap.LoadFromDb();

        // Trigger full group resync (SyncGroupsState)
        gclsGroupCallService.OnGroupConfigChanged();

        // 런타임 설정도 전체 재로드
        gclsCspConfigCache.RefreshAll();
        gclsListenerManager.Sync();
        gclsTrunkManager.Sync();
    } else if (strEvent == "LISTENER_CHANGED") {
        CLog::Print(LOG_INFO, "CscInterface: LISTENER_CHANGED uri=%s action=%s", strUri.c_str(), strAction.c_str());
        gclsCspConfigCache.RefreshEntity(CACHE_LISTENER);
        gclsListenerManager.Sync();
    } else if (strEvent == "TRUNK_CHANGED") {
        CLog::Print(LOG_INFO, "CscInterface: TRUNK_CHANGED uri=%s action=%s", strUri.c_str(), strAction.c_str());
        gclsCspConfigCache.RefreshEntity(CACHE_TRUNK);
        gclsTrunkManager.Sync();
    } else if (strEvent == "ROUTE_RULE_CHANGED") {
        CLog::Print(LOG_INFO, "CscInterface: ROUTE_RULE_CHANGED uri=%s action=%s", strUri.c_str(), strAction.c_str());
        gclsCspConfigCache.RefreshEntity(CACHE_ROUTE);
        // TODO(P4): 라우팅 규칙 적용
    } else if (strEvent == "ACCESS_LIST_CHANGED") {
        CLog::Print(LOG_INFO, "CscInterface: ACCESS_LIST_CHANGED uri=%s action=%s", strUri.c_str(), strAction.c_str());
        gclsCspConfigCache.RefreshEntity(CACHE_ACCESS);
        // TODO(P5): 접근제어 적용
    } else if (strEvent == "USER_CHANGED") {
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
