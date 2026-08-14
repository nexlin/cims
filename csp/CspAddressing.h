#ifndef __CSP_ADDRESSING_H__
#define __CSP_ADDRESSING_H__

#include <string>

#include "SipFrom.h"
#include "SipTransport.h"

/**
 * CspAddressing — CSP 가 외부에 advertise 하는 로컬 주소 helper 레이어 (R5.a + R5.b).
 *
 * 배경:
 *   CSP 는 여러 지점에서 "우리 IP" 를 사용한다 — SIP Contact/From/Call-ID host,
 *   SDP media IP, XCAP URL. R4 이전에는 모두 `gclsSetup.m_strLocalIp` 단일
 *   값을 직접 참조했다. multi-listener / peering 환경에서는 컨텍스트별로
 *   다른 주소를 반환해야 한다.
 *
 * 진화:
 *   - R5.a: 세 semantic helper 도입. 모두 primary LocalIp 반환 (전이 버전).
 *   - R5.b: 인바운드 listener id / 아웃바운드 protocol-edge 기반 분기 추가.
 *           LocalNodeMap 에서 해당 local_node 조회 실패 시 primary fallback.
 *   - R5.c (TBD): TLS per-listener cert 연동
 *   - R6 (TBD): access_services.server_identity_uri 기반 From identity
 *
 * 호출 규약:
 *   반환값은 짧은 lifetime 로만 사용 (string copy 권장).
 */

namespace CspAddressing {

    /** 자기 Contact 를 채운다 — **(호스트, 포트, transport) 는 한 세트**다.
     *
     *  Contact 는 상대가 in-dialog 요청(BYE·re-INVITE·SUBSCRIBE 갱신)을 보낼 목적지다.
     *  포트만 그 transport 의 리스너로 적고 transport 파라미터를 빠뜨리면, 상대는
     *  RFC 3261 §19.1.1 에 따라 **UDP 로 해석**해 평문으로 그 포트에 보낸다 — TLS 리스너
     *  포트에는 UDP 소켓이 없어 그 요청은 조용히 유실된다(실측: TLS leg 의 BYE 가 사라져
     *  세션 타이머가 회수할 때까지 상대 단말 화면에 참여자가 남았다).
     *
     *  psip 의 자동 Contact(SipStackComm) 는 이 규칙을 이미 지킨다. 응용이 Contact 를 직접
     *  구성할 때(그룹 AoR user·isfocus 파라미터 등)도 같은 규칙을 쓰도록 여기로 모은다.
     *
     *  @param eTransport 그 메시지가 **실제로 나가는** transport (leg latch·요청 수신 transport).
     *  @param pszUser    Contact URI 의 user 부분 (NULL/빈값이면 host only — 실망 형태).
     */
    void FillSelfContact( CSipFrom &clsContact, ESipTransport eTransport, const char *pszUser = NULL );

    /** SIP Contact / From / Call-ID host 생성에 사용할 로컬 주소.
     *  @param inbound_listener_id psip 수신 listener 의 extId (GetCurrentInboundListenerId()
     *         반환값). 0 이면 bootstrap 또는 컨텍스트 불명 → primary fallback.
     *         >0 이면 해당 local_node 의 bind_ip 반환. bind_ip=0.0.0.0 이면 gclsSetup.m_strLocalIp. */
    std::string GetLocalSipAddress( int inbound_listener_id = 0 );

    /** inbound listener 의 bind_port 반환 (Via/Contact 자기 port 결정에 사용).
     *  @param inbound_listener_id psip 수신 listener 의 extId. 0 또는 매칭 실패 시 fallback.
     *  @param fallback_port 매칭 실패 시 반환값 (보통 gclsSetup.m_iUdpPort). */
    int GetLocalSipPort( int inbound_listener_id, int fallback_port );

    /** outbound forwarding 시 목적지 protocol/edge 에 맞는 local_node 선택.
     *  1차: enabled && protocol == proto && edge == edge_preference
     *  2차: enabled && protocol == proto (edge 무관)
     *  3차: primary LocalIp fallback (R5.a 동작).
     *  @param proto "UDP" / "TCP" / "TLS". 빈 문자열이면 protocol 체크 skip.
     *  @param edge_preference "access" / "peering" / "mgmt" 또는 빈 문자열. */
    std::string GetLocalSipAddressForOutbound( const std::string &proto = "UDP",
                                               const std::string &edge_preference = "peering" );

    /** SDP media (RTP relay) 에 advertise 할 로컬 주소.
     *  R5.c 이후 CMP interface 와 CSP interface 가 다를 때 분리 예정. */
    std::string GetLocalRtpAddress();

    /** XCAP / MCPTT 서비스 URL host.
     *  Phase 3: xcap-diff NOTIFY 의 xcap-root host. Setup.Xcap.Host(CSC XCAP 서버)
     *  가 지정되면 그 값, 비면 CSP primary LocalIp fallback. */
    std::string GetLocalXcapAddress();

    /** XCAP / MCPTT 서비스 URL port (Setup.Xcap.Port, 기본 4430 = CSC McpttServer). */
    int GetXcapPort();

    /** XCAP / MCPTT 서비스 URL scheme (Setup.Xcap.Scheme, 기본 "https" — CSC TLS). */
    std::string GetXcapScheme();

    /** R6: 해당 서비스(kind="volte"|"ptt")에 대한 CSP server identity URI.
     *  access_services.server_identity_uri 가 명시되면 그것 반환.
     *  비면 sip:cspserver@{domain} 자동 조립.
     *  서비스 매칭 실패 시 sip:cspserver@{LocalIp} primary fallback. */
    std::string GetServerIdentityForService( const std::string &kind );

}  // namespace CspAddressing

#endif  // __CSP_ADDRESSING_H__
