#include "CspAddressing.h"

#include "CspLocalNodeMap.h"
#include "CspServiceMap.h"
#include "SipServerSetup.h"

namespace CspAddressing {

    /** bind_ip=0.0.0.0/공백 이면 CSP 의 advertised primary IP(gclsSetup.m_strLocalIp) 반환.
     *  그 외에는 로컬 노드의 구체 bind_ip 그대로. */
    static std::string _resolveBindIp( const LocalNodeInfo &n ) {
        if ( n.bind_ip.empty() || n.bind_ip == "0.0.0.0" ) return gclsSetup.m_strLocalIp;
        return n.bind_ip;
    }

    void FillSelfContact( CSipFrom &clsContact, ESipTransport eTransport, const char *pszUser ) {
        // 포트는 그 transport 의 리스너 포트여야 한다 — 평문 포트에 TLS 를, TLS 포트에 평문을
        //   광고하면 상대의 in-dialog 요청이 도달하지 못한다.
        int iPort = gclsSetup.m_iUdpPort;
        if ( eTransport == E_SIP_TCP && gclsSetup.m_iTcpPort > 0 )
            iPort = gclsSetup.m_iTcpPort;
        else if ( eTransport == E_SIP_TLS && gclsSetup.m_iTlsPort > 0 )
            iPort = gclsSetup.m_iTlsPort;

        clsContact.m_clsUri.m_strProtocol = "sip";
        if ( pszUser && *pszUser ) clsContact.m_clsUri.m_strUser = pszUser;
        clsContact.m_clsUri.m_strHost = gclsSetup.m_strLocalIp;
        clsContact.m_clsUri.m_iPort = iPort;
        // UDP 는 기본값이라 생략해도 무해하지만, 명시하면 상대 스택이 추측하지 않는다.
        clsContact.m_clsUri.InsertTransport( eTransport );
    }

    std::string GetLocalSipAddress( int inbound_listener_id ) {
        if ( inbound_listener_id > 0 ) {
            LocalNodeInfo n = gclsLocalNodeMap.GetByIntId( inbound_listener_id );
            if ( n.IsValid() ) return _resolveBindIp( n );
        }
        return gclsSetup.m_strLocalIp;
    }

    int GetLocalSipPort( int inbound_listener_id, int fallback_port ) {
        if ( inbound_listener_id > 0 ) {
            LocalNodeInfo n = gclsLocalNodeMap.GetByIntId( inbound_listener_id );
            if ( n.IsValid() && n.bind_port > 0 ) return n.bind_port;
        }
        return fallback_port;
    }

    std::string GetLocalSipAddressForOutbound( const std::string &proto, const std::string &edge_preference ) {
        std::vector<LocalNodeInfo> all = gclsLocalNodeMap.GetAll();

        // 1차: protocol + edge 일치
        if ( !edge_preference.empty() ) {
            for ( const auto &n : all ) {
                if ( !n.enabled ) continue;
                if ( !proto.empty() && n.protocol != proto ) continue;
                if ( n.edge != edge_preference ) continue;
                return _resolveBindIp( n );
            }
        }
        // 2차: protocol 만 일치
        for ( const auto &n : all ) {
            if ( !n.enabled ) continue;
            if ( !proto.empty() && n.protocol != proto ) continue;
            return _resolveBindIp( n );
        }
        // 3차: primary fallback
        return gclsSetup.m_strLocalIp;
    }

    std::string GetLocalRtpAddress() {
        return gclsSetup.m_strLocalIp;
    }

    std::string GetLocalXcapAddress() {
        // Phase 3: xcap-root host 는 CSC XCAP(MCPTT) 서버. 미설정 시 CSP 자기 IP fallback.
        if ( !gclsSetup.m_strXcapHost.empty() ) return gclsSetup.m_strXcapHost;
        return gclsSetup.m_strLocalIp;
    }

    int GetXcapPort() {
        return gclsSetup.m_iXcapPort > 0 ? gclsSetup.m_iXcapPort : 4430;
    }

    std::string GetXcapScheme() {
        return gclsSetup.m_strXcapScheme.empty() ? "https" : gclsSetup.m_strXcapScheme;
    }

    std::string GetServerIdentityForService( const std::string &kind ) {
        // 1) access_services 에서 kind 매칭되는 첫 enabled 서비스 조회
        ServiceInfo svc = gclsServiceMap.GetByKind( kind );
        if ( svc.id > 0 ) {
            // 1a) server_identity_uri 명시 → 그대로 반환
            if ( !svc.server_identity_uri.empty() ) return svc.server_identity_uri;
            // 1b) domain 기반 자동 조립
            if ( !svc.domain.empty() ) return "sip:cspserver@" + svc.domain;
        }
        // 2) 서비스 매칭 실패 → primary LocalIp fallback (R5.a 동작)
        return "sip:cspserver@" + gclsSetup.m_strLocalIp;
    }

}  // namespace CspAddressing
