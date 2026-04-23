#ifndef __CSP_ADDRESSING_H__
#define __CSP_ADDRESSING_H__

#include <string>

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

/** SIP Contact / From / Call-ID host 생성에 사용할 로컬 주소.
 *  @param inbound_listener_id psip 수신 listener 의 extId (GetCurrentInboundListenerId()
 *         반환값). 0 이면 bootstrap 또는 컨텍스트 불명 → primary fallback.
 *         >0 이면 해당 local_node 의 bind_ip 반환. bind_ip=0.0.0.0 이면 gclsSetup.m_strLocalIp. */
std::string GetLocalSipAddress(int inbound_listener_id = 0);

/** outbound forwarding 시 목적지 protocol/edge 에 맞는 local_node 선택.
 *  1차: enabled && protocol == proto && edge == edge_preference
 *  2차: enabled && protocol == proto (edge 무관)
 *  3차: primary LocalIp fallback (R5.a 동작).
 *  @param proto "UDP" / "TCP" / "TLS". 빈 문자열이면 protocol 체크 skip.
 *  @param edge_preference "access" / "peering" / "mgmt" 또는 빈 문자열. */
std::string GetLocalSipAddressForOutbound(const std::string& proto = "UDP",
                                          const std::string& edge_preference = "peering");

/** SDP media (RTP relay) 에 advertise 할 로컬 주소.
 *  R5.c 이후 CMP interface 와 CSP interface 가 다를 때 분리 예정. */
std::string GetLocalRtpAddress();

/** XCAP / MCPTT 서비스 URL host.
 *  R6 이후 access_services 의 server_identity_uri 로 확장 예정. */
std::string GetLocalXcapAddress();

} // namespace CspAddressing

#endif // __CSP_ADDRESSING_H__
