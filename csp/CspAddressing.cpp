#include "CspAddressing.h"
#include "CspLocalNodeMap.h"
#include "CspServiceMap.h"
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

std::string GetServerIdentityForService(const std::string& kind) {
    // 1) access_services 에서 kind 매칭되는 첫 enabled 서비스 조회
    ServiceInfo svc = gclsServiceMap.GetByKind(kind);
    if (svc.id > 0) {
        // 1a) server_identity_uri 명시 → 그대로 반환
        if (!svc.server_identity_uri.empty()) return svc.server_identity_uri;
        // 1b) domain 기반 자동 조립
        if (!svc.domain.empty()) return "sip:cspserver@" + svc.domain;
    }
    // 2) 서비스 매칭 실패 → primary LocalIp fallback (R5.a 동작)
    return "sip:cspserver@" + gclsSetup.m_strLocalIp;
}

} // namespace CspAddressing
