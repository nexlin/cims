#include "account_map.h"

#include <algorithm>
#include <cctype>

namespace cimsue {
namespace detail {

const char* transportParam(Transport t) {
    switch (t) {
        case Transport::TCP: return "tcp";
        case Transport::TLS: return "tls";
        default: return "udp";
    }
}

static bool ieq(const std::string& a, const char* b) {
    std::string lb(b);
    if (a.size() != lb.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)lb[i])) return false;
    return true;
}

pj::AccountConfig buildPjAccountConfig(const AccountConfig& c, std::string* note) {
    pj::AccountConfig ac;
    const std::string tp = transportParam(c.transport);
    ac.idUri = c.displayName.empty() ? c.aor() : "\"" + c.displayName + "\" <" + c.aor() + ">";
    ac.regConfig.registrarUri = "sip:" + c.domain + ":" + std::to_string(c.serverPort) + ";transport=" + tp;
    ac.regConfig.timeoutSec = c.expiresSec;                 // 희망값 — 서버 200 OK Expires 추종
    ac.regConfig.registerOnAdd = false;                     // 등록은 registerAccount() 가 명시적으로
    ac.regConfig.retryIntervalSec = 30;
    ac.regConfig.firstRetryIntervalSec = 5;
    ac.regConfig.randomRetryIntervalSec = 5;
    ac.regConfig.delayBeforeRefreshSec = 10;
    ac.regConfig.dropCallsOnFail = false;
    ac.natConfig.udpKaIntervalSec = 15;
    ac.natConfig.contactRewriteUse = 1;
    ac.natConfig.viaRewriteUse = 1;
    ac.natConfig.sipStunUse = PJSUA_STUN_USE_DISABLED;
    ac.natConfig.mediaStunUse = PJSUA_STUN_USE_DISABLED;

    const bool isAka = c.authScheme == AuthScheme::Aka && !c.akaK.empty();
    const bool hasHa1 = !c.ha1.empty();
    const std::string user = c.digestUsername();
    if (isAka) {
        pj::AuthCredInfo cred("digest", "*", user, PJSIP_CRED_DATA_EXT_AKA, "");
        cred.akaK = c.akaK;
        cred.akaOp = c.akaOpc;                              // pjsip 패치 PJSIP_AKA_OP_IS_OPC: OPc 직접 소비
        cred.akaAmf = c.akaAmf;
        ac.sipConfig.authCreds.push_back(cred);
        if (note) *note += "auth=aka(K/OPc) ";
    } else if (hasHa1) {
        ac.sipConfig.authCreds.push_back(pj::AuthCredInfo("digest", "*", user, PJSIP_CRED_DATA_DIGEST, c.ha1));
        if (note) *note += "auth=ha1 ";
    } else {
        ac.sipConfig.authCreds.push_back(
            pj::AuthCredInfo("digest", "*", user, PJSIP_CRED_DATA_PLAIN_PASSWD, c.password));
        if (note) *note += "auth=plain ";
    }

    const bool tls = c.transport == Transport::TLS;
    const bool mediaSrtp = tls && c.mediaSecurity != MediaSecurity::Off;
    ac.mediaConfig.srtpUse = !mediaSrtp ? PJMEDIA_SRTP_DISABLED
                             : c.mediaSecurity == MediaSecurity::Required ? PJMEDIA_SRTP_MANDATORY
                                                                           : PJMEDIA_SRTP_OPTIONAL;
    ac.mediaConfig.srtpSecureSignaling = 1;                 // SDES 키는 기밀 채널에서만(TLS)
    if (note && mediaSrtp) *note += (c.mediaSecurity == MediaSecurity::Required ? "srtp=required " : "srtp=optional ");

    bool secAgree = tls && std::any_of(c.secMechanisms.begin(), c.secMechanisms.end(),
                                       [](const std::string& m) { return ieq(m, "tls"); });
    if (secAgree) {
        pj::SipHeader h1; h1.hName = "Security-Client"; h1.hValue = mediaSrtp ? "tls, sdes-srtp;mediasec" : "tls";
        pj::SipHeader h2; h2.hName = "Require"; h2.hValue = "sec-agree";
        pj::SipHeader h3; h3.hName = "Proxy-Require"; h3.hValue = "sec-agree";
        ac.regConfig.headers.push_back(h1);
        ac.regConfig.headers.push_back(h2);
        ac.regConfig.headers.push_back(h3);
        if (note) *note += "sec-agree ";
    }
    if (!c.contactParams.empty()) ac.sipConfig.contactParams = c.contactParams;
    ac.sipConfig.proxies.push_back("sip:" + c.serverHost + ":" + std::to_string(c.serverPort) +
                                   ";transport=" + tp + ";lr");
    ac.videoConfig.autoTransmitOutgoing = c.videoAutoTransmit;
    ac.videoConfig.autoShowIncoming = false;                // 수신 렌더는 앱이 프레임/Surface 로 결선
    return ac;
}

std::string normalizeTarget(const std::string& target, const std::string& domain) {
    auto starts = [&](const char* p) { return target.rfind(p, 0) == 0; };
    if (starts("sip:") || starts("sips:") || starts("tel:")) return target;
    if (target.find('@') != std::string::npos) return "sip:" + target;
    return "sip:" + target + "@" + domain;
}

std::string headerValue(const std::string& whole, const std::string& name) {
    size_t end = whole.find("\r\n\r\n");
    std::string hdrs = end == std::string::npos ? whole : whole.substr(0, end);
    size_t pos = 0;
    while (pos < hdrs.size()) {
        size_t eol = hdrs.find("\r\n", pos);
        if (eol == std::string::npos) eol = hdrs.size();
        std::string line = hdrs.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string hn = line.substr(0, colon);
            while (!hn.empty() && std::isspace((unsigned char)hn.back())) hn.pop_back();
            if (ieq(hn, name.c_str())) {
                std::string v = line.substr(colon + 1);
                size_t b = v.find_first_not_of(" \t");
                return b == std::string::npos ? std::string() : v.substr(b);
            }
        }
        pos = eol + 2;
    }
    return std::string();
}

std::string uriUser(const std::string& hv) {
    size_t s = hv.find(':');
    if (s == std::string::npos) return std::string();
    size_t lt = hv.find('<');
    if (lt != std::string::npos && lt < s) { /* <scheme:user@host> */ }
    std::string rest = hv.substr(s + 1);
    size_t e = rest.find_first_of("@>;");
    return e == std::string::npos ? rest : rest.substr(0, e);
}

}  // namespace detail
}  // namespace cimsue
