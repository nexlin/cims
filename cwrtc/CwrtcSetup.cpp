#include "CwrtcSetup.h"
#include "SimpleJson.h"
#include "Log.h"
#include <fstream>
#include <sstream>

CCwrtcSetup gclsCwrtcSetup;

CCwrtcSetup::CCwrtcSetup()
    : m_iWsPort(3000)
    , m_bWss(false)
    , m_strSipIp("127.0.0.1")
    , m_iSipPort(5060)
    , m_strSipDomain("ims.nex-cims.co.kr")
    , m_strPttDomain("ptt.nex-cims.co.kr")
    , m_iSipLocalPort(5062)
    , m_iRtpPortBase(50100)
    , m_iRtpPortCount(50)
    , m_strLogDir("log")
    , m_strDocRoot("html")
{
}

bool CCwrtcSetup::Load(const char* pszConfigFile)
{
    std::ifstream ifs(pszConfigFile);
    if (!ifs.is_open()) {
        printf("CCwrtcSetup: cannot open [%s]\n", pszConfigFile);
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();

    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse(oss.str());
    if (root.type != SimpleJson::JSON_OBJECT) return false;

    SimpleJson::JsonNode setup = root.Get("Setup");
    if (setup.type != SimpleJson::JSON_OBJECT) return false;

    if (setup.Has("LocalIp"))    m_strLocalIp    = setup.GetString("LocalIp");
    if (setup.Has("WsPort"))     m_iWsPort       = (int)setup.GetInt("WsPort");
    if (setup.Has("Wss"))               m_bWss               = ((int)setup.GetInt("Wss") != 0);
    if (setup.Has("CertFile"))          m_strCertFile        = setup.GetString("CertFile");
    if (setup.Has("ApiToken"))          m_strApiToken        = setup.GetString("ApiToken");
    if (setup.Has("UserAgent"))         m_strUserAgent       = setup.GetString("UserAgent");
    if (setup.Has("AccessNetworkInfo")) m_strPAccessNetworkInfo = setup.GetString("AccessNetworkInfo");
    if (setup.Has("DocRoot"))           m_strDocRoot         = setup.GetString("DocRoot");

    SimpleJson::JsonNode sip = setup.Get("Sip");
    if (sip.type == SimpleJson::JSON_OBJECT) {
        if (sip.Has("ServerIp"))    m_strSipIp       = sip.GetString("ServerIp");
        if (sip.Has("ServerPort"))  m_iSipPort       = (int)sip.GetInt("ServerPort");
        if (sip.Has("Domain"))      m_strSipDomain   = sip.GetString("Domain");
        if (sip.Has("PttDomain"))   m_strPttDomain   = sip.GetString("PttDomain");
        if (sip.Has("LocalPort"))   m_iSipLocalPort  = (int)sip.GetInt("LocalPort");
    }

    SimpleJson::JsonNode rtp = setup.Get("Rtp");
    if (rtp.type == SimpleJson::JSON_OBJECT) {
        if (rtp.Has("PortBase"))  m_iRtpPortBase  = (int)rtp.GetInt("PortBase");
        if (rtp.Has("PortCount")) m_iRtpPortCount = (int)rtp.GetInt("PortCount");
    }

    SimpleJson::JsonNode log = setup.Get("Log");
    if (log.type == SimpleJson::JSON_OBJECT) {
        if (log.Has("Dir")) m_strLogDir = log.GetString("Dir");
    }

    CLog::Print(LOG_INFO, "CwrtcSetup: LocalIp=%s WsPort=%d SipIp=%s:%d Domain=%s SipLocalPort=%d RtpBase=%d",
        m_strLocalIp.c_str(), m_iWsPort,
        m_strSipIp.c_str(), m_iSipPort, m_strSipDomain.c_str(), m_iSipLocalPort,
        m_iRtpPortBase);

    return !m_strLocalIp.empty();
}
