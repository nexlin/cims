# MCPTT API (3GPP TS 24.482/483/484)

CSC 의 MCPTT 서버(포트 4430)가 제공하는 3GPP 표준 MCPTT 서비스 엔드포인트.

**Base URL**: `https://<CSC>:4430`
**인증**: OAuth 2.0 (IdMS) → Access Token → 각 서비스 호출 시 `Authorization: Bearer`

---

## 1. IdMS (Identity Management Server)

MCPTT 단말 로그인 + 토큰 발급 (OAuth 2.0 Authorization Code + PKCE S256 필수).

| Method | Path | 용도 |
|---|---|---|
| GET  | `/.well-known/openid-configuration` | OIDC 디스커버리 (authorization/token/introspection endpoint 광고) |
| GET  | `/idms/authreq` | **두 말투 병행** — ① `user_name`+`user_password` 쿼리 동반: 자체 단말 간이형 → `200 JSON {code,state,Location}` ② 자격 없음(규격 OIDC Authentication Request): `client_id`·`redirect_uri`(필수)·`code_challenge`(필수)·`code_challenge_method=S256`·`scope`·`state`·`nonce`·`response_type=code` → `200 text/html` 로그인 폼 |
| POST | `/idms/authreq` | 규격 로그인 폼 제출 (`application/x-www-form-urlencoded`: 입력칸 `username`/`password` — 이름은 `IdMs.FormLoginField`/`FormPasswordField` 설정 + hidden 문맥) → 성공 **`302 Location: redirect_uri?code=…&state=…`** / 실패 `200` 폼 재표시+오류 |
| POST | `/idms/tokenreq` | `grant_type=authorization_code`(`code`·`code_verifier`·`client_id`·`redirect_uri`) 또는 `refresh_token` → JSON(access/id/refresh token). JSON·form-urlencoded 모두 수용 |
| GET  | `/idms/introspect` | 토큰 introspection |

검증 공통: PKCE 누락/plain → 400, `redirect_uri` 허용목록 `IdMs.RedirectUriAllow`(비면 전부 허용,
정확 일치) 위반 → 400, 인증 실패(간이형) → 401 `access_denied`. 흐름 상세는 3GPP TS 24.482 §6.3.1 과
[mcptt_standard_conformance.md §3 IdMS](../design/features/mcptt_standard_conformance.md).

---

## 2. GMS (Group Management Server)

`XCAP-style` 리소스 기반. 그룹 문서 조회/수정.

| Method | Path | 용도 |
|---|---|---|
| GET  | `/org.openmobilealliance.groups/users/{user}/oma_groups/...` | 그룹 문서 |
| PUT  | (위와 동일) | 그룹 문서 갱신 |

클라이언트가 변경 구독 시 SIP `SUBSCRIBE Event: xcap-diff` 이용.

---

## 3. CMS (Configuration Management Server)

MCPTT 설정 문서 (TS 24.484). ue-init-config 만 **익명 GET**(로그인 전 부트스트랩 — XUI 는 UE
인스턴스 ID, 무검증), 나머지는 Bearer 토큰 + 본인 문서만(403). 전부 ETag/If-None-Match 지원.

| Method | Path | 인증 |
|---|---|---|
| GET  | `/org.3gpp.mcptt.ue-init-config/users/{instance}/{doc}` | 없음 (익명) |
| GET  | `/org.3gpp.mcptt.user-profile/users/{user}/user-profile` | Bearer + 본인 |
| GET  | `/org.3gpp.mcptt.service-config/users/{user}/service-config` | Bearer + 본인 |

ue-init-config 의 주소류(IdMS/CMS/GMS/KMS/XCAP 루트)의 base 는 CSC 설정 `McpttServer.PublicUrl`
이 정본이다(비면 요청 Host 유도 — 올인원 전용). CSP 가 xcap-diff NOTIFY 로 광고하는 `xcap-root`
도 같은 값이며, CSP 는 이를 내부 API 로 취득한다:

| Method | Path | 인증 | 응답 |
|---|---|---|---|
| GET | `/internal/mcptt/endpoint` (admin 4421) | `Bearer {InternalApi.Token}` | `{"xcap_root","mcptt_port","public_url_configured"}` |

`/api/v1` 밖이라 OAM 게이트웨이가 프록시하지 않는다(CSP 직접 호출 전용, `/internal/aka/av` 와 동일).

나머지 주소류(domain·PLMN·GMS-URI)는 토폴로지에서 유도되고,
규격 파라미터값(Timers·con-ref·http-proxy·보호 플래그·group-creation-XUI·name)과 확장 요소
(`MCPTT/MCData-Service-Details`) 는 csc 설정 `UeInitConfig.*` 로 사용자지정한다 — 값이 바뀌면 ETag 도
바뀐다([mcptt_standard_conformance.md §R4-1](../design/features/mcptt_standard_conformance.md)).

---

## 4. KMS (Key Management Server)

미디어 암호화 키 배포.

| Method | Path |
|---|---|
| POST | `/keymanagement/identity/v1/init` |
| GET  | `/keymanagement/identity/v1/certificate` |

---

## 5. 관련 파일

- 소스: `csc/src/handlers/idms.py`, `mcptt.py` 계열
- 저장: `csc_idms` DB (auth_code, refresh_token)
- SIP 단말 인증은 `api/admin_api.md` 의 CSCF 참조

상세 3GPP 스펙 추종 확인은 공식 스펙 문서 참조. 이 문서는 CIMS 구현된 엔드포인트 일람만 제공합니다.
