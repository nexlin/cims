// libcimsue 내부 — cimsue::AccountConfig → pj::AccountConfig 매핑 (ue_sdk.md §4.1 sip).
// 단위시험이 직접 검증하므로 엔진 부팅 없이 호출 가능해야 한다(pj::AccountConfig 는 순수 값 객체).
#pragma once

#include <pjsua2.hpp>
#include <string>

#include "cimsue/types.h"

namespace cimsue {
namespace detail {

/** 승계 규칙(android_ue_m1_pjsip_integration.md §3.2·§3.3, SipController.buildAccountConfig):
 *  - idUri = "이름" <sip:msisdn@domain>, registrar = sip:domain:port;transport=X
 *  - Digest username = IMPI(imsi@domain 또는 authId), realm="*"(challenge realm echo — 불일치 무한 401 회피)
 *  - 자료 우선순위: AKA(K/OPc, EXT_AKA) > H(A1)(DIGEST) > 평문
 *  - proxies = sip:host:port;transport=X;lr (도메인 DNS 미해석 회피)
 *  - NAT: udp keepalive 15s, contact/via rewrite, STUN 없음
 *  - SRTP(SDES)는 TLS 접속에서만(TS 33.328 e2ae 전제 — 비-TLS 에서 켜면 pjsua 가 호를 거부)
 *  - sec-agree(RFC 3329): TLS 접속 + 서버 메커니즘에 tls 가 있을 때 Security-Client/Require 제안,
 *    미디어 SRTP 정책이 켜져 있으면 sdes-srtp;mediasec 병기 */
pj::AccountConfig buildPjAccountConfig(const AccountConfig& c, std::string* note = nullptr);

/** SIP transport 이름(udp/tcp/tls). */
const char* transportParam(Transport t);

/** 발신 대상 정규화 — 번호면 sip:번호@domain, URI(sip:/sips:/tel:)면 그대로. */
std::string normalizeTarget(const std::string& target, const std::string& domain);

/** SIP 원문에서 헤더 값 1개(대소문자 무시, 첫 매치). 없으면 빈 문자열. */
std::string headerValue(const std::string& wholeMsg, const std::string& name);

/** 헤더 값의 URI 사용자부(예: <sip:+8210@d>;x → +8210). */
std::string uriUser(const std::string& headerVal);

}  // namespace detail
}  // namespace cimsue
