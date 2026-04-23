#include "CspAddressing.h"
#include "CspLocalNodeMap.h"
#include "SipServerSetup.h"

namespace CspAddressing {

/** bind_ip=0.0.0.0/공백 이면 CSP 의 advertised primary IP(gclsSetup.m_strLocalIp) 반환.
 *  그 외에는 로컬 노드의 구체 bind_ip 그대로. */
static std::string _resolveBindIp(const LocalNodeInfo& n) {
    if (n.bind_ip.empty() || n.bind_ip == "0.0.0.0") return gclsSetup.m_strLocalIp;
    return n.bind_ip;
}

std::string GetLocalSipAddress(int inbound_listener_id) {
    if (inbound_listener_id > 0) {
        LocalNodeInfo n = gclsLocalNodeMap.GetByIntId(inbound_listener_id);
        if (n.IsValid()) return _resolveBindIp(n);
    }
    return gclsSetup.m_strLocalIp;
}

std::string GetLocalSipAddressForOutbound(const std::string& proto,
                                          const std::string& edge_preference) {
    std::vector<LocalNodeInfo> all = gclsLocalNodeMap.GetAll();

    // 1차: protocol + edge 일치
    if (!edge_preference.empty()) {
        for (const auto& n : all) {
            if (!n.enabled) continue;
            if (!proto.empty() && n.protocol != proto) continue;
            if (n.edge != edge_preference) continue;
            return _resolveBindIp(n);
        }
    }
    // 2차: protocol 만 일치
    for (const auto& n : all) {
        if (!n.enabled) continue;
        if (!proto.empty() && n.protocol != proto) continue;
        return _resolveBindIp(n);
    }
    // 3차: primary fallback
    return gclsSetup.m_strLocalIp;
}

std::string GetLocalRtpAddress() {
    return gclsSetup.m_strLocalIp;
}

std::string GetLocalXcapAddress() {
    return gclsSetup.m_strLocalIp;
}

} // namespace CspAddressing
