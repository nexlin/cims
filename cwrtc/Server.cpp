#include "HttpCallBack.h"
#include "CwrtcSetup.h"
#include "SipAgent.h"
#include "RtpThread.h"
#include "Log.h"
#include "Directory.h"
#include "TcpStackSetup.h"
#include "MemoryDebug.h"

bool StartServer(const char* pszConfigFile)
{
    if (!gclsCwrtcSetup.Load(pszConfigFile)) return false;

    // 로그 초기화
    CLog::SetPrefix("cwrtc");
    CLog::SetDirectory(gclsCwrtcSetup.m_strLogDir.c_str());
    CLog::SetLevel(LOG_INFO | LOG_DEBUG | LOG_NETWORK);

    // 네트워크 + DTLS 초기화
    InitNetwork();
    InitDtls();

    // WebSocket HTTP 서버 시작
    CTcpStackSetup clsHttpSetup;
    clsHttpSetup.m_iListenPort        = gclsCwrtcSetup.m_iWsPort;
    clsHttpSetup.m_iMaxSocketPerThread = 100;
    clsHttpSetup.m_iThreadMaxCount     = 0;
    clsHttpSetup.m_bUseThreadPipe      = false;

    // WSS(TLS WebSocket) 설정
    if (gclsCwrtcSetup.m_bWss && !gclsCwrtcSetup.m_strCertFile.empty()) {
        clsHttpSetup.m_bUseTls    = true;
        clsHttpSetup.m_strCertFile = gclsCwrtcSetup.m_strCertFile;
        CLog::Print(LOG_INFO, "WSS enabled: cert=%s", gclsCwrtcSetup.m_strCertFile.c_str());
    }

    gclsHttpCallBack.m_strDocumentRoot = gclsCwrtcSetup.m_strDocRoot;
    if (!CDirectory::IsDirectory(gclsHttpCallBack.m_strDocumentRoot.c_str())) {
        printf("[%s] doc root not found — WS only mode\n",
               gclsHttpCallBack.m_strDocumentRoot.c_str());
    }

    if (!gclsHttpStack.Start(&clsHttpSetup, &gclsHttpCallBack)) {
        printf("HttpStack.Start failed (port %d)\n", gclsCwrtcSetup.m_iWsPort);
        return false;
    }
    CLog::Print(LOG_INFO, "WebSocket server started (port %d)", gclsCwrtcSetup.m_iWsPort);

    // SIP UA 시작
    if (!gclsSipAgent.Start()) return false;

    return true;
}

bool StopServer()
{
    gclsSipAgent.Stop();
    FinalDtls();
    gclsHttpStack.Stop();
    SSLFinal();
    sleep(1);
    return true;
}
