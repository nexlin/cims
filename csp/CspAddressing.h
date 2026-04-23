#ifndef __CSP_ADDRESSING_H__
#define __CSP_ADDRESSING_H__

#include <string>

/**
 * CspAddressing — CSP 가 외부에 advertise 하는 로컬 주소 helper 레이어 (R5.a).
 *
 * 배경:
 *   CSP 는 여러 지점에서 "우리 IP" 를 사용한다 — SIP Contact/From/Call-ID host,
 *   SDP media IP, XCAP URL. R4 이전에는 모두 `gclsSetup.m_strLocalIp` 단일
 *   값을 직접 참조했다. multi-listener / peering 환경에서는 컨텍스트별로
 *   다른 주소를 반환해야 한다:
 *     - SIP signaling: 수신/발신 listener 의 bind_ip (R5.b 에서 per-route 분기)
 *     - SDP media: RTP 가 advertise 할 주소 (CMP 가 별도 호스트에 있을 수 있음)
 *     - XCAP URL: MCPTT/CSC 서비스 URL host (보통 CSC 호스트)
 *
 * 현재 (R5.a):
 *   세 함수 모두 `gclsSetup.m_strLocalIp` 를 반환. semantic label 로 호출부를
 *   분리해두어 R5.b (per-route listener 선택) / R5.c (TLS per-listener cert) /
 *   R6 (From identity per Access Service) 확장 시 해당 함수만 수정하면 되도록 한다.
 *
 * 호출 규약:
 *   반환값은 짧은 lifetime 로만 사용 (string copy 권장). 내부 구현이 전역 값
 *   참조이므로 현재는 안전하나 향후 per-call resolution 시 temporary 반환 가능.
 */

namespace CspAddressing {

/** SIP Contact / From / Call-ID host 생성에 사용할 로컬 주소.
 *  R5.b 에서 inbound listener 또는 outbound Route 에 따라 분기 예정. */
std::string GetLocalSipAddress();

/** SDP media (RTP relay) 에 advertise 할 로컬 주소.
 *  R5.b 이후 CMP interface 와 CSP interface 가 다를 때 분리 예정. */
std::string GetLocalRtpAddress();

/** XCAP / MCPTT 서비스 URL host.
 *  R6 이후 access_services 의 server_identity_uri 로 확장 예정. */
std::string GetLocalXcapAddress();

} // namespace CspAddressing

#endif // __CSP_ADDRESSING_H__
