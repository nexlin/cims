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

XCAP(RFC 4825) 리소스 기반 — TS 24.481 Ut. 인증 = `Authorization: Bearer <IdMS PKCE access token>`,
경로의 `{xui}` 는 토큰 `mcptt_id` 본인 트리만(타인 트리 403). 그룹 URI 는 시스템 관례대로 `tel:<id>`
(예 `tel:g001`, 클라이언트 생성 `tel:g-0a1b2c3d`); `sip:<id>@<PTT 도메인>` 도 같은 그룹으로 받는다.

| Method | Path | 인가 | 응답 |
|---|---|---|---|
| GET  | `/org.openmobilealliance.groups/users/{xui}` | 본인 트리 | JSON 배열 — 멤버인 그룹 + **소유(`authorized_user_id`) 그룹**(비멤버라도). 항목 `{uri, display_name, etag, member_count, is_owner}` — `is_owner` = 편집·삭제 가능 |
| GET  | `…/users/{xui}/{group_uri}` | 멤버 또는 소유자 | `application/vnd.oma.poc.groups+xml` + `ETag`; `If-None-Match` → 304 |
| PUT  | `…/users/{xui}/{group_uri}` | **신규** = 프로파일 `allow_create_group`(OAM 부여, 프로비저닝 `ptt.allowCreateGroup`) · **기존** = 소유자 | 201(신규)/200(갱신) + 문서 + `ETag`. 403 `group_creation_not_allowed` / `not_group_owner`(소유자 없는 콘솔 그룹 포함), 409 `uri_taken`(타인 소유 id — 다른 id 로), 400 `invalid_group_id`·`reserved_prefix`·`invalid_group_document`·`unknown_member`, 412 `etag_mismatch`(`If-Match` 사용 시) |
| DELETE | `…/users/{xui}/{group_uri}` | 소유자 | 200. 403 `not_group_owner`, 404 |

- **신규 그룹 식별자는 클라이언트가 정한다**(XCAP 관습): `g-` + 소문자 hex 8자리(`tel:g-0a1b2c3d`). `adhoc-`/`priv-` 는
  즉석 세션 예약 접두사라 거부. 콘솔이 만든 `g001` 류는 형식이 달라도 소유자면 PUT/DELETE 가능.
- 처리 = DB(`ptt_groups`·`ptt_group_members`, 소유자 = 토큰 가입자 `users.id`) → in-memory GROUPS 동기화 →
  CSP `GROUP_CHANGED` 통지(CSP 가 xcap-diff NOTIFY 로 단말에 전파). 관리 API(4421, 콘솔 토큰)의 그룹 CRUD 와
  같은 정본·같은 동기화를 쓴다.
- PUT 본문 = **GET 이 돌려주는 문서와 같은 포맷**(아래). 없는 요소는 갱신 시 기존값 유지, 생성 시 기본값
  (session-type prearranged, priority 5, SDS 허용, FD 불허, 긴급통화 불허, 긴급경보 허용). `<list>` 가 있으면
  멤버 전체 교체(없으면 유지) — entry uri 는 PTT 가입 번호(`tel:+E.164`, `sip:` 형 가능), 미가입 번호는 400.
  `<mcpttgi:authorized-user>` 는 서버가 정한다(본문의 값 무시). floor 정책(`floor_policy`/`max_talkers`)은 관리 API 전용.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">
  <list-service uri="tel:g-0a1b2c3d">
    <display-name xml:lang="en-us">관제채널</display-name>
    <list>
      <entry uri="tel:+82510001001">
        <rl:display-name>관제1석</rl:display-name>
        <mcpttgi:participant-type>chair</mcpttgi:participant-type>
        <mcpttgi:user-priority>1</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+82500000001">
        <rl:display-name>테스트001</rl:display-name>
        <mcpttgi:participant-type>participant</mcpttgi:participant-type>
        <mcpttgi:user-priority>5</mcpttgi:user-priority>
      </entry>
    </list>
    <mcpttgi:session-type>prearranged</mcpttgi:session-type>
    <mcpttgi:mcdata-allow-short-data-service>true</mcpttgi:mcdata-allow-short-data-service>
    <mcpttgi:mcdata-allow-file-distribution>false</mcpttgi:mcdata-allow-file-distribution>
    <mcpttgi:mcptt-video>false</mcpttgi:mcptt-video>
    <mcpttgi:on-network-max-participant-count>10</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-require-affiliation>true</mcpttgi:on-network-require-affiliation>
    <mcpttgi:on-network-group-priority>5</mcpttgi:on-network-group-priority>
    <mcpttgi:on-network-encryption>false</mcpttgi:on-network-encryption>
    <cp:ruleset><cp:rule id="a7c"><cp:actions>
      <mcpttgi:allow-MCPTT-emergency-call>false</mcpttgi:allow-MCPTT-emergency-call>
      <mcpttgi:allow-MCPTT-emergency-alert>true</mcpttgi:allow-MCPTT-emergency-alert>
    </cp:actions></cp:rule></cp:ruleset>
  </list-service>
</group>
```

GET 응답은 여기에 `<mcpttgi:authorized-user>tel:+82510001001</mcpttgi:authorized-user>`(소유자)·`<cims:user-title>`
(직함, CIMS 확장)·MCData 크기 요소·`<oxe:supported-services>` 가 더 실린다. 클라이언트가 변경 구독 시
SIP `SUBSCRIBE Event: xcap-diff` 이용.

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
