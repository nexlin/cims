# MCPTT API (3GPP TS 24.482/483/484)

CSC 의 MCPTT 서버(포트 4430)가 제공하는 3GPP 표준 MCPTT 서비스 엔드포인트.

**Base URL**: `https://<CSC>:4430`
**인증**: OAuth 2.0 (IdMS) → Access Token → 각 서비스 호출 시 `Authorization: Bearer`

---

## 1. IdMS (Identity Management Server)

MCPTT 단말 로그인 + 토큰 발급.

| Method | Path | 용도 |
|---|---|---|
| GET  | `/idms/oauth2/authorize` | OAuth 2.0 Authorization Code 시작 |
| POST | `/idms/oauth2/token`     | Authorization Code → Access Token 교환 |

(Flow 상세는 3GPP TS 24.482)

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

MCPTT 단말별 설정 (user profile, service config).

| Method | Path |
|---|---|
| GET  | `/org.3gpp.mcptt.ue-init-config/users/{user}/ue-init-config/` |
| GET  | `/org.3gpp.mcptt.service-config/users/{user}/service-config/` |

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
