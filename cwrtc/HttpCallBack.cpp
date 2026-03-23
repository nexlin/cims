#include "HttpCallBack.h"
#include "HttpStatusCode.h"
#include "FileUtility.h"
#include "Directory.h"
#include "Log.h"
#include "SessionMap.h"
#include "SipAgent.h"
#include "SimpleJson.h"
#include "MemoryDebug.h"
#include <cstring>

CHttpStack    gclsHttpStack;
CHttpCallBack gclsHttpCallBack;

CHttpCallBack::CHttpCallBack() : m_bStop(false)
{
}

// ─── HTTP 정적 파일 서빙 ──────────────────────────────────────────────────────

bool CHttpCallBack::RecvHttpRequest(CHttpMessage* pclsRequest, CHttpMessage* pclsResponse)
{
    // 보안: '..' 경로 차단
    if (strstr(pclsRequest->m_strReqUri.c_str(), "..")) {
        pclsResponse->m_iStatusCode = HTTP_NOT_FOUND;
        return true;
    }

    std::string strPath = m_strDocumentRoot;
    if (pclsRequest->m_strReqUri == "/") {
        CDirectory::AppendName(strPath, "index.html");
    } else {
        strPath.append(pclsRequest->m_strReqUri);
    }

    if (!IsExistFile(strPath.c_str())) {
        pclsResponse->m_iStatusCode = HTTP_NOT_FOUND;
        return true;
    }

    // MIME type
    std::string strExt;
    GetFileExt(strPath.c_str(), strExt);
    const char* e = strExt.c_str();
    if      (!strcmp(e,"html")||!strcmp(e,"htm")) pclsResponse->m_strContentType = "text/html";
    else if (!strcmp(e,"css"))  pclsResponse->m_strContentType = "text/css";
    else if (!strcmp(e,"js"))   pclsResponse->m_strContentType = "text/javascript";
    else if (!strcmp(e,"png"))  pclsResponse->m_strContentType = "image/png";
    else if (!strcmp(e,"gif"))  pclsResponse->m_strContentType = "image/gif";
    else if (!strcmp(e,"jpg")||!strcmp(e,"jpeg")) pclsResponse->m_strContentType = "image/jpeg";
    else { pclsResponse->m_iStatusCode = HTTP_NOT_FOUND; return true; }

    FILE* fd = fopen(strPath.c_str(), "rb");
    if (!fd) { pclsResponse->m_iStatusCode = HTTP_NOT_FOUND; return true; }
    char buf[8192]; int n;
    while ((n = fread(buf, 1, sizeof(buf), fd)) > 0)
        pclsResponse->m_strBody.append(buf, n);
    fclose(fd);
    pclsResponse->m_iStatusCode = HTTP_OK;
    return true;
}

// ─── WebSocket 연결 이벤트 ────────────────────────────────────────────────────

void CHttpCallBack::WebSocketConnected(const char* pszClientIp, int iClientPort)
{
    CLog::Print(LOG_INFO, "WS connected [%s:%d]", pszClientIp, iClientPort);
}

void CHttpCallBack::WebSocketClosed(const char* pszClientIp, int iClientPort)
{
    CLog::Print(LOG_INFO, "WS closed [%s:%d]", pszClientIp, iClientPort);

    // 사용자 통화 종료 처리
    std::string strUserId = gclsSessionMap.GetUserIdByWs(pszClientIp, iClientPort);
    if (!strUserId.empty()) {
        CWsClient cli;
        if (gclsSessionMap.GetClientByWs(pszClientIp, iClientPort, cli)) {
            if (!cli.strActiveCallId.empty()) {
                gclsSipAgent.HangupCall(cli.strActiveCallId);
            }
            gclsSipAgent.UnregisterUser(strUserId, cli.strDomain);
        }
        gclsSessionMap.DeleteClient(pszClientIp, iClientPort);
    }
}

// ─── WebSocket 메시지 처리 ────────────────────────────────────────────────────

bool CHttpCallBack::WebSocketData(const char* pszClientIp, int iClientPort,
                                  std::string& strData, CHttpStackSession* pclsSession)
{
    CLog::Print(LOG_NETWORK, "WS[%s:%d] recv: %s", pszClientIp, iClientPort, strData.c_str());

    SimpleJson::JsonNode msg = SimpleJson::JsonNode::Parse(strData);
    if (msg.type != SimpleJson::JSON_OBJECT) return false;

    std::string strType = msg.GetString("type");

    // ── register ──────────────────────────────────────────────────────────────
    if (strType == "register") {
        std::string strUser     = msg.GetString("user");
        std::string strPassword = msg.GetString("password");
        std::string strDomain   = msg.GetString("domain");
        std::string strAuthId   = msg.GetString("auth_id");

        if (strUser.empty() || strPassword.empty()) {
            SendText(pszClientIp, iClientPort,
                R"({"type":"register_failed","reason":"missing_fields"})");
            return true;
        }

        gclsSessionMap.InsertClient(strUser, pszClientIp, iClientPort,
                                    strDomain, strPassword, strAuthId);
        gclsSipAgent.RegisterUser(strUser, strPassword, strDomain, strAuthId);
        // 실제 응답은 EventRegister 콜백에서 전송
        return true;
    }

    // ── call (발신) ───────────────────────────────────────────────────────────
    if (strType == "call") {
        std::string strTo  = msg.GetString("to");
        std::string strSdp = msg.GetString("sdp");

        std::string strUserId = gclsSessionMap.GetUserIdByWs(pszClientIp, iClientPort);
        if (strUserId.empty() || strTo.empty() || strSdp.empty()) {
            SendText(pszClientIp, iClientPort, R"({"type":"ended","reason":"invalid"})");
            return true;
        }

        std::string strCallId;
        if (!gclsSipAgent.StartOutgoingCall(strUserId, strTo, strSdp,
                                            pszClientIp, iClientPort, strCallId)) {
            SendText(pszClientIp, iClientPort, R"({"type":"ended","reason":"error"})");
        }
        return true;
    }

    // ── answer (착신 수락) ────────────────────────────────────────────────────
    if (strType == "answer") {
        std::string strCallId = msg.GetString("call_id");
        std::string strSdp    = msg.GetString("sdp");

        if (strCallId.empty() || strSdp.empty()) return true;
        gclsSipAgent.AcceptIncomingCall(strCallId, strSdp);
        return true;
    }

    // ── hangup ────────────────────────────────────────────────────────────────
    if (strType == "hangup") {
        std::string strCallId = msg.GetString("call_id");
        if (!strCallId.empty()) gclsSipAgent.HangupCall(strCallId);
        return true;
    }

    CLog::Print(LOG_INFO, "WS unknown type: %s", strType.c_str());
    return true;
}

// ─── 전송 헬퍼 ────────────────────────────────────────────────────────────────

bool CHttpCallBack::SendText(const char* pszClientIp, int iClientPort, const char* pszText)
{
    int iLen = (int)strlen(pszText);
    CLog::Print(LOG_NETWORK, "WS[%s:%d] send: %s", pszClientIp, iClientPort, pszText);
    return gclsHttpStack.SendWebSocketPacket(pszClientIp, iClientPort,
                                              const_cast<char*>(pszText), iLen);
}
