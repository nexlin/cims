#pragma once
#include "HttpStackCallBack.h"
#include "HttpStack.h"
#include <string>

/**
 * WebSocket JSON 핸들러
 *
 * JSON 프로토콜:
 *  register   : {"type":"register","user":"...","password":"...","domain":"...","auth_id":"..."}
 *  call       : {"type":"call","to":"...","sdp":"..."}
 *  answer     : {"type":"answer","call_id":"...","sdp":"..."}
 *  hangup     : {"type":"hangup","call_id":"..."}
 *  ptt_push   : {"type":"ptt_push","call_id":"..."}
 *  ptt_release: {"type":"ptt_release","call_id":"..."}
 */
class CHttpCallBack : public IHttpStackCallBack
{
public:
    CHttpCallBack();
    ~CHttpCallBack() override = default;

    bool RecvHttpRequest(CHttpMessage* pclsRequest, CHttpMessage* pclsResponse) override;
    void WebSocketConnected(const char* pszClientIp, int iClientPort) override;
    void WebSocketClosed(const char* pszClientIp, int iClientPort) override;
    bool WebSocketData(const char* pszClientIp, int iClientPort,
                       std::string& strData, CHttpStackSession* pclsSession) override;

    bool SendText(const char* pszClientIp, int iClientPort, const char* pszText);

    std::string m_strDocumentRoot;
    bool        m_bStop;
};

extern CHttpStack    gclsHttpStack;
extern CHttpCallBack gclsHttpCallBack;
