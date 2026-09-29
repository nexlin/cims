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
    // 인스턴스 ID — TCP/TLS 는 pjsua outbound(RFC 5626) 경로가 reg-id 와 함께 싣고, 그 경로를 타지 않는 UDP 는
    //   REGISTER Contact 에 직접 싣는다(두 경로가 겹치지 않게 transport 로 가른다 — 중복 파라미터 방지).
    if (!c.instanceId.empty()) {
        const std::string inst = "<" + c.instanceId + ">";
        ac.natConfig.sipOutboundInstanceId = inst;
        if (c.transport == Transport::UDP) ac.regConfig.contactParams = ";+sip.instance=\"" + inst + "\"";
        if (note) *note += "instance ";
    }
    ac.sipConfig.proxies.push_back("sip:" + c.serverHost + ":" + std::to_string(c.serverPort) +
                                   ";transport=" + tp + ";lr");
    ac.videoConfig.autoTransmitOutgoing = c.videoAutoTransmit;
    ac.videoConfig.autoShowIncoming = false;                // 수신 렌더는 앱이 프레임/Surface 로 결선
    return ac;
}

}  // namespace detail

std::string imeiUrn(const std::string& imei) {
    if (imei.size() != 15 || !std::all_of(imei.begin(), imei.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return std::string();
    int sum = 0;                                            // Luhn — 오른쪽에서 두 번째 자리부터 두 배
    for (int i = 0; i < 14; ++i) {
        int d = imei[13 - i] - '0';
        if (i % 2 == 0) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
    }
    if ((10 - sum % 10) % 10 != imei[14] - '0') return std::string();
    return "urn:gsma:imei:" + imei.substr(0, 8) + "-" + imei.substr(8, 6) + "-" + imei.substr(14, 1);
}

// User-Agent comment 에 넣을 값 정리(RFC 3261 §25.1 — comment 안 ctext 는 괄호·역슬래시·제어 문자를 못 쓴다). 괄호·역슬래시는 빼고,
// 공백·제어 문자는 공백 하나로 접는다. dropSemicolon = OS 칸 — `;` 는 이 형식에서 OS·모델 구분자다(수집 쪽 파서와 짝).
static std::string commentPart(const std::string& s, bool dropSemicolon) {
    std::string out;
    bool pendingSpace = false;
    for (unsigned char ch : s) {
        if (ch == 0x28 || ch == 0x29 || ch == 0x5C || (dropSemicolon && ch == ';')) continue;   // ( ) 역슬래시
        if (ch <= 0x20 || ch == 0x7F) { pendingSpace = !out.empty(); continue; }
        if (pendingSpace) { out += ' '; pendingSpace = false; }
        out += (char)ch;
    }
    return out;
}

std::string userAgentOf(const std::string& product, const std::string& version, const std::string& os,
                        const std::string& model) {
    std::string ua = product + (version.empty() ? std::string() : "/" + version);
    std::string cm = commentPart(os, true);
    std::string md = commentPart(model, false);
    if (!md.empty()) cm += (cm.empty() ? "" : "; ") + md;
    if (!cm.empty()) ua += " (" + cm + ")";
    return ua;
}

namespace detail {

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
