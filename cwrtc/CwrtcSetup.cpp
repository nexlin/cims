#include "CwrtcSetup.h"
#include "SimpleJson.h"
#include "Log.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

CCwrtcSetup gclsCwrtcSetup;

// config.json overlay: flat 키 ("Setup.Sip.ServerIp": "1.2.3.4") 를 root 의 중첩 경로에 set.
// 같은 경로가 이미 있으면 덮어씀. SipServerSetup.cpp 의 overlay 계약과 동일.
static void _setByDotPath(SimpleJson::JsonNode& parent, const std::string& dotPath,
                          const SimpleJson::JsonNode& value)
{
    size_t pos = dotPath.find('.');
    if (pos == std::string::npos) {
        parent.Set(dotPath, value);
        return;
    }
    std::string head = dotPath.substr(0, pos);
    std::string rest = dotPath.substr(pos + 1);
    SimpleJson::JsonNode sub = parent.Has(head) ? parent.Get(head) : SimpleJson::JsonNode();
    if (sub.type != SimpleJson::JSON_OBJECT) {
        sub = SimpleJson::JsonNode();
        sub.type = SimpleJson::JSON_OBJECT;
    }
    _setByDotPath(sub, rest, value);
    parent.Set(head, sub);
}

// install_path 기준으로 overlay 파일 경로 탐색. 시도 순서:
//   1) CIMS_DEPLOYMENT_CONFIG 환경변수
//   2) <cwrtc.json 디렉토리>/../../config.json     (install_path/config.json, 배포 배치)
//   3) (없음) — overlay 생략
static std::string _findDeploymentConfig(const std::string& cwrtcJsonPath)
{
    if (const char* env = getenv("CIMS_DEPLOYMENT_CONFIG")) {
        if (*env) {
            std::ifstream f(env);
            if (f) return env;
        }
    }
    std::string dir = cwrtcJsonPath;
    size_t s = dir.find_last_of('/');
    if (s != std::string::npos) dir = dir.substr(0, s);
    // dir = install_path/cwrtc/config  →  ../.. = install_path
    std::string cand = dir + "/../../config.json";
    std::ifstream f(cand);
    if (f) return cand;
    return "";
}

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

    // Deployment overlay: install_path/config.json 을 flat key → nested 로 merge.
    std::string strOverlayPath = _findDeploymentConfig(pszConfigFile);
    if (!strOverlayPath.empty()) {
        std::ifstream of(strOverlayPath);
        std::ostringstream ob;
        ob << of.rdbuf();
        SimpleJson::JsonNode over = SimpleJson::JsonNode::Parse(ob.str());
        if (over.type == SimpleJson::JSON_OBJECT) {
            int applied = 0;
            for (const auto& kv : over.objects) {
                _setByDotPath(root, kv.first, kv.second);
                ++applied;
            }
            CLog::Print(LOG_INFO, "CwrtcSetup: overlay %s applied (%d keys)",
                strOverlayPath.c_str(), applied);
        }
    }

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

    SimpleJson::JsonNode msglog = setup.Get("MsgLog");
    if (msglog.type == SimpleJson::JSON_OBJECT) {
        if (msglog.Has("Dir")) m_strMsgLogDir = msglog.GetString("Dir");
    }

    CLog::Print(LOG_INFO, "CwrtcSetup: LocalIp=%s WsPort=%d SipIp=%s:%d Domain=%s SipLocalPort=%d RtpBase=%d",
        m_strLocalIp.c_str(), m_iWsPort,
        m_strSipIp.c_str(), m_iSipPort, m_strSipDomain.c_str(), m_iSipLocalPort,
        m_iRtpPortBase);

    return !m_strLocalIp.empty();
}
