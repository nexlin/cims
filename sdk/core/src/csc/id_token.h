// libcimsue 내부 — ID token 검증(TS 33.180 B.11.1 → OpenID Connect Core §3.1.3.7).
//   단말은 토큰 응답의 ID token 을 검증한다: iss 가 발급자(discovery `issuer`)와 정확히 같고, aud 에 자기 client_id 가 있고,
//   지금이 exp 전이고, 인증 요청에 nonce 를 보냈으면 같은 nonce 가 실려 있어야 한다. 서명은 검사하지 않는다 — 인가 코드
//   흐름의 ID token 은 토큰 엔드포인트와의 TLS 로 직접 받으므로 TLS 서버 검증이 서명 검증을 갈음할 수 있다(§3.1.3.7 6)).
#pragma once

#include <cstdint>
#include <string>

namespace cimsue {
namespace idtoken {

/** 시계 차 허용(초) — TS 33.180 표 B.2.1.2-1 `exp`: «not to exceed 30 seconds». */
constexpr int64_t kClockLeewaySec = 30;

/** base64url(패딩 없음) → 원문. */
std::string base64UrlDecode(const std::string& s);

/** ID token(JWS compact)의 claim 검증. nonce 가 비면 nonce 는 보지 않는다(refresh 응답 — OIDC Core §12.2).
 *  통과하면 true, 아니면 false 와 why(사람이 읽는 사유). */
bool validate(const std::string& idToken, const std::string& issuer, const std::string& clientId,
              const std::string& nonce, int64_t nowEpoch, std::string* why);

}  // namespace idtoken
}  // namespace cimsue
