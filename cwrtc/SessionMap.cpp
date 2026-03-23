#include "SessionMap.h"
#include <sstream>

CSessionMap gclsSessionMap;

std::string CSessionMap::WsKey(const std::string& ip, int port)
{
    return ip + "_" + std::to_string(port);
}

bool CSessionMap::InsertClient(const std::string& userId, const std::string& wsIp, int wsPort,
                               const std::string& domain, const std::string& password, const std::string& authId)
{
    m_clsMutex.acquire();
    CWsClient c;
    c.strUserId  = userId;
    c.strWsIp    = wsIp;
    c.iWsPort    = wsPort;
    c.strDomain  = domain;
    c.strPassword = password;
    c.strAuthId  = authId;
    m_clsClientMap[userId] = c;
    m_clsWsKeyMap[WsKey(wsIp, wsPort)] = userId;
    m_clsMutex.release();
    return true;
}

bool CSessionMap::DeleteClient(const std::string& wsIp, int wsPort)
{
    m_clsMutex.acquire();
    auto it = m_clsWsKeyMap.find(WsKey(wsIp, wsPort));
    if (it != m_clsWsKeyMap.end()) {
        m_clsClientMap.erase(it->second);
        m_clsWsKeyMap.erase(it);
    }
    m_clsMutex.release();
    return true;
}

bool CSessionMap::GetClientByUser(const std::string& userId, CWsClient& out)
{
    m_clsMutex.acquire();
    auto it = m_clsClientMap.find(userId);
    bool found = (it != m_clsClientMap.end());
    if (found) out = it->second;
    m_clsMutex.release();
    return found;
}

bool CSessionMap::GetClientByWs(const std::string& wsIp, int wsPort, CWsClient& out)
{
    m_clsMutex.acquire();
    auto itKey = m_clsWsKeyMap.find(WsKey(wsIp, wsPort));
    bool found = false;
    if (itKey != m_clsWsKeyMap.end()) {
        auto itClient = m_clsClientMap.find(itKey->second);
        if (itClient != m_clsClientMap.end()) {
            out = itClient->second;
            found = true;
        }
    }
    m_clsMutex.release();
    return found;
}

std::string CSessionMap::GetUserIdByWs(const std::string& wsIp, int wsPort)
{
    m_clsMutex.acquire();
    auto it = m_clsWsKeyMap.find(WsKey(wsIp, wsPort));
    std::string uid = (it != m_clsWsKeyMap.end()) ? it->second : "";
    m_clsMutex.release();
    return uid;
}

bool CSessionMap::InsertCall(const CCallSession& sess)
{
    m_clsMutex.acquire();
    m_clsCallMap[sess.strCallId] = sess;
    // 사용자의 활성 통화 callId 업데이트
    auto itC = m_clsClientMap.find(sess.strUserId);
    if (itC != m_clsClientMap.end()) itC->second.strActiveCallId = sess.strCallId;
    m_clsMutex.release();
    return true;
}

bool CSessionMap::GetCall(const std::string& callId, CCallSession& out)
{
    m_clsMutex.acquire();
    auto it = m_clsCallMap.find(callId);
    bool found = (it != m_clsCallMap.end());
    if (found) out = it->second;
    m_clsMutex.release();
    return found;
}

bool CSessionMap::UpdateCallCmp(const std::string& callId, const std::string& cmpIp, int cmpPort)
{
    m_clsMutex.acquire();
    auto it = m_clsCallMap.find(callId);
    bool found = (it != m_clsCallMap.end());
    if (found) { it->second.strCmpIp = cmpIp; it->second.iCmpPort = cmpPort; }
    m_clsMutex.release();
    return found;
}

bool CSessionMap::UpdateCallRtpArg(const std::string& callId, CRtpThreadArg* pArg)
{
    m_clsMutex.acquire();
    auto it = m_clsCallMap.find(callId);
    bool found = (it != m_clsCallMap.end());
    if (found) it->second.pclsRtpArg = pArg;
    m_clsMutex.release();
    return found;
}

bool CSessionMap::UpdateCallBrowserSdp(const std::string& callId, const std::string& sdp)
{
    m_clsMutex.acquire();
    auto it = m_clsCallMap.find(callId);
    bool found = (it != m_clsCallMap.end());
    if (found) it->second.strBrowserSdp = sdp;
    m_clsMutex.release();
    return found;
}

bool CSessionMap::DeleteCall(const std::string& callId)
{
    m_clsMutex.acquire();
    auto it = m_clsCallMap.find(callId);
    if (it != m_clsCallMap.end()) {
        // 사용자의 활성 통화 초기화
        auto itC = m_clsClientMap.find(it->second.strUserId);
        if (itC != m_clsClientMap.end() && itC->second.strActiveCallId == callId)
            itC->second.strActiveCallId = "";
        m_clsCallMap.erase(it);
    }
    m_clsMutex.release();
    return true;
}

void CSessionMap::ClearUserActiveCall(const std::string& userId)
{
    m_clsMutex.acquire();
    auto it = m_clsClientMap.find(userId);
    if (it != m_clsClientMap.end()) it->second.strActiveCallId = "";
    m_clsMutex.release();
}
