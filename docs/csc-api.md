# CSC (Configuration & Service Controller) — 인터페이스 명세

> 작성일: 2026-03-25
> 대상 파일: `csc/bin/csc_pihttp/`

---

## 1. 아키텍처 개요

```
┌─────────────────────────────────────────────────────────────────┐
│                          CSC Process                            │
│                      (app.py · uvicorn)                         │
│                                                                 │
│  ┌─────────────────────────┐  ┌──────────────────────────────┐  │
│  │   Admin Server :4420    │  │    MCPTT Server :4430        │  │
│  │  (CIMS 관리 Web API)    │  │  (3GPP MCPTT 서비스)         │  │
│  │                         │  │                              │  │
│  │  /api/v1/auth/*         │  │  /idms/*    IdMS             │  │
│  │  /api/v1/users/*        │  │  /org.openmobilealliance...  │  │
│  │  /api/v1/ptt/groups/*   │  │             GMS              │  │
│  │  /api/v1/call/logs/*    │  │  /org.3gpp.mcptt.*          │  │
│  └─────────────────────────┘  │             CMS              │  │
│                                │  /keymanagement/*  KMS       │  │
│                                └──────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Data Layer                                              │  │
│  │  · MariaDB csc_idms  (auth_code, refresh_token)         │  │
│  │  · MariaDB cims       (users, subscriptions, groups,    │  │
│  │                        call_logs)                        │  │
│  │  · JSON files  User/*.json   Group/*.json                │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
          │ UDP JSON                    │ HTTP/WS
          ▼ :4421                       ▼ :4430 / :4420
        CSP                        cims-phone / cims-console
   (SIP 서버)
```

### 연결 클라이언트

| 클라이언트 | 접속 포트 | 용도 |
|---|---|---|
| `cims-console` (관리 Web UI) | **4420** | 가입자·그룹 관리, 통화 모니터링 |
| `cims-phone` (MCPTT UE UI) | **4430** | MCPTT IdMS/GMS/CMS/KMS |
| CSP (SIP 서버) | UDP **4421** ← CSC 송신 | 그룹 변경 이벤트 수신 |

---

## 2. 공통 규칙

### 2.1 인증 방식

| 서버 | 인증 방식 | 토큰 위치 |
|---|---|---|
| Admin (4420) | CIMS JWT (HS256) — `/api/v1/auth/login` 발급 | `Authorization: Bearer <token>` |
| MCPTT (4430) | MCPTT Access Token (HS256) — IdMS PKCE 발급 | `Authorization: Bearer <token>` |

### 2.2 토큰 Payload 비교

**CIMS Admin JWT** (`cims_auth.py`)
```json
{
  "sub":   "42",              // cims_users.id
  "email": "admin@cims.com",
  "role":  "admin",           // "admin" | "user"
  "exp":   1748000000
}
```

**MCPTT ID Token / Access Token** (`csc_service.py`)
```json
{
  "mcptt_id": "tel:+82571900001",   // MCPTT 사용자 식별자
  "iss":      "idms.mcptt.com",
  "aud":      "mcptt_client",
  "sub":      "<uuid>",
  "scope":    ["openid", "mcptt"],
  "exp":      1748000000,
  "iat":      1745000000
}
```

### 2.3 공통 오류 응답

```json
{ "error": "invalid_request",  "error_description": "..." }
{ "error": "invalid_grant" }
{ "error": "unsupported_grant_type" }
```

---

## 3. Admin Server — Port 4420

### 3.1 인증 API (`cims_auth.py`)

JWT 시크릿: `config.CimsAuth.JwtSecret` (기본값 `cims_jwt_secret_change_me`)
토큰 유효기간: 7일

---

#### `POST /api/v1/auth/login`
관리자 로그인. JWT 발급.

**Request**
```json
{ "email": "admin@cims.com", "password": "secret" }
```

**Response 200**
```json
{
  "token": "<JWT>",
  "user": {
    "id": 1,
    "name": "관리자",
    "email": "admin@cims.com",
    "role": "admin",
    "call_subscriptions": [
      { "id": "+82571900001", "auth_id": "+82571900001",
        "dnd": false, "forward_id": "",
        "register_time": "2025-01-01T00:00:00", "logout_time": null }
    ],
    "ptt_subscriptions": [ ... ]
  }
}
```

**Error 401**: 이메일·비밀번호 불일치

---

#### `POST /api/v1/auth/register`
신규 계정 등록 (role=user 고정).

**Request**
```json
{ "name": "홍길동", "email": "hong@cims.com", "password": "pass1234" }
```

**Response 201**
```json
{ "token": "<JWT>", "user": { "id": 5, ... } }
```

**Error 409**: 이메일 중복

---

#### `GET /api/v1/auth/me`
내 정보 조회.
**Headers**: `Authorization: Bearer <JWT>`

**Response 200** — login 응답의 `user` 객체와 동일

---

#### `PUT /api/v1/auth/password`
비밀번호 변경.
**Headers**: `Authorization: Bearer <JWT>`

**Request**
```json
{ "old_password": "old", "new_password": "newpass" }
```

**Response 200**: `{ "ok": true }`
**Error 401**: 현재 비밀번호 불일치

---

### 3.2 가입자 관리 API (`cims_admin.py`)

DB: `CimsDatabase` (cims)
모든 엔드포인트 — **Admin JWT 필요** (app.py에서 `config` 주입, DB 연결)

---

#### `GET /api/v1/users`
가입자 전체 목록 (call·PTT 구독 포함).

**Response 200**
```json
{
  "users": [
    {
      "id": 1,
      "name": "홍길동",
      "email": "hong@cims.com",
      "org_id": "ORG-A",
      "details": null,
      "reject_id": ["+821000000"],
      "call_subscriptions": [
        { "id": "+82571900001", "auth_id": "+82571900001",
          "dnd": false, "forward_id": "",
          "register_time": "...", "logout_time": null }
      ],
      "ptt_subscriptions": [ ... ],
      "create_time": "2025-01-01T00:00:00",
      "update_time": "2025-01-02T00:00:00"
    }
  ]
}
```

---

#### `POST /api/v1/users`
가입자 생성.

**Request**
```json
{
  "name": "홍길동",
  "email": "hong@cims.com",
  "org_id": "ORG-A",
  "details": "비고",
  "reject_id": ["+821000000"]
}
```

**Response 201**: `{ "id": 5 }`

---

#### `GET /api/v1/users/{pid}`
가입자 단건 조회 (call·PTT 구독 포함).

**Response 200**: 위 users 배열 원소와 동일 구조
**Error 404**: 가입자 없음

---

#### `PUT /api/v1/users/{pid}`
가입자 정보 수정.

**Request** (변경할 필드만)
```json
{ "name": "김철수", "org_id": "ORG-B", "reject_id": [] }
```

**Response 200**: `{ "id": 5 }`

---

#### `DELETE /api/v1/users/{pid}`
가입자 삭제 (rejects 포함).

**Response 200**: `{ "id": 5 }`

---

#### `GET /api/v1/users/{pid}/call`
#### `GET /api/v1/users/{pid}/ptt`
Call 또는 PTT 구독 목록.

**Response 200**
```json
{
  "subscriptions": [
    { "id": "+82571900001", "auth_id": "+82571900001",
      "dnd": false, "forward_id": "",
      "register_time": null, "logout_time": null }
  ]
}
```

---

#### `POST /api/v1/users/{pid}/call`
#### `POST /api/v1/users/{pid}/ptt`
구독 추가.

**Request**
```json
{
  "id":         "+82571900001",   // MSISDN (필수)
  "auth_id":    "+82571900001",   // SIP auth_id (기본값: id와 동일)
  "passwd":     "sip_password",
  "dnd":        false,
  "forward_id": ""
}
```

**Response 201**: `{ "id": "+82571900001" }`

---

#### `PUT /api/v1/users/{pid}/call/{msisdn}`
#### `PUT /api/v1/users/{pid}/ptt/{msisdn}`
구독 수정.

**Request**: `POST`와 동일 필드
**Response 200**: `{ "id": "+82571900001" }`

---

#### `DELETE /api/v1/users/{pid}/call/{msisdn}`
#### `DELETE /api/v1/users/{pid}/ptt/{msisdn}`
구독 삭제.

**Response 200**: `{ "id": "+82571900001" }`

---

### 3.3 PTT 그룹 관리 API (`cims_admin.py`)

DB: `CimsDatabase.cims_ptt_groups`, `cims_ptt_group_members`

---

#### `GET /api/v1/ptt/groups`
그룹 전체 목록 (멤버 포함).

**Response 200**
```json
{
  "groups": [
    {
      "id": "82571900000",
      "name": "그룹 A",
      "members": [
        { "user_id": "+82571900001", "priority": 1 },
        { "user_id": "+82571900002", "priority": 2 }
      ]
    }
  ]
}
```

---

#### `POST /api/v1/ptt/groups`
그룹 생성.

**Request**
```json
{
  "id":   "82571900000",
  "name": "그룹 A",
  "members": [
    { "user_id": "+82571900001", "priority": 1 }
  ]
}
```

**Response 201**: `{ "id": "82571900000" }`

---

#### `GET /api/v1/ptt/groups/{id}`
그룹 단건 조회.

**Response 200**: groups 배열 원소와 동일
**Error 404**

---

#### `PUT /api/v1/ptt/groups/{id}`
그룹 수정 (members 지정 시 전체 교체).

**Request**
```json
{ "name": "그룹 B", "members": [ { "user_id": "+82571900003", "priority": 1 } ] }
```

**Response 200**: `{ "id": "82571900000" }`

---

#### `DELETE /api/v1/ptt/groups/{id}`
그룹 삭제 (멤버 포함).

**Response 200**: `{ "id": "82571900000" }`

---

#### `GET /api/v1/ptt/groups/{id}/members`
멤버 목록.

**Response 200**
```json
{ "group_id": "82571900000", "members": [ { "user_id": "...", "priority": 1 } ] }
```

---

#### `POST /api/v1/ptt/groups/{id}/members`
멤버 추가 (중복 시 priority 업데이트).

**Request**: `{ "user_id": "+82571900004", "priority": 3 }`
**Response 201**: `{ "group_id": "82571900000", "user_id": "+82571900004" }`

---

#### `DELETE /api/v1/ptt/groups/{id}/members/{uid}`
멤버 제거.

**Response 200**: `{ "group_id": "...", "user_id": "..." }`

---

### 3.4 통화 이력 API (`cims_admin.py`)

DB: `CimsDatabase.cims_call_logs`, `cims_call_participants`

---

#### `GET /api/v1/call/logs`
통화 이력 조회.

**Query Parameters**

| 파라미터 | 타입 | 설명 |
|---|---|---|
| `state` | string | `ringing` \| `active` \| `ended` |
| `caller` | string | 발신자 MSISDN |
| `callee` | string | 수신자 MSISDN |
| `msisdn` | string | caller OR callee OR participant 포함 |
| `group_id` | string | PTT 그룹 ID |
| `call_type` | string | `voip` \| `ptt` |
| `from_dt` | string | 시작일 `YYYY-MM-DD` |
| `to_dt` | string | 종료일 `YYYY-MM-DD` |
| `limit` | int | 최대 건수 (기본 200, 최대 1000) |
| `offset` | int | 페이지 오프셋 (기본 0) |

**Response 200**
```json
{
  "total": 150,
  "limit": 50,
  "offset": 0,
  "logs": [
    {
      "id": 1001,
      "call_id": "abc-123",
      "call_type": "ptt",
      "group_id": "82571900000",
      "initiator": "+82571900001",
      "callee": "+82571900000",
      "state": "ended",
      "invite_time":  "2025-03-01T10:00:00",
      "answer_time":  "2025-03-01T10:00:02",
      "end_time":     "2025-03-01T10:05:00",
      "duration": 298,
      "sip_status": 200,
      "end_reason": "normal",
      "end_reason_ko": "정상종료",
      "participants": [
        { "msisdn": "+82571900001", "role": "caller",
          "join_time": "2025-03-01T10:00:02", "leave_time": "2025-03-01T10:05:00" }
      ]
    }
  ]
}
```

---

#### `GET /api/v1/call/logs/active`
진행 중인 통화만 조회 (`state IN ('ringing','active')` 단축).

**Response**: 위와 동일 구조

---

## 4. MCPTT Server — Port 4430

설정: `config.IdMs.*`
데이터: `Data.User/*.json`, `Data.Group/*.json` (메모리 캐시)
토큰 검증: MCPTT Access Token (HS256, `aud=mcptt_client`)

---

### 4.1 IdMS — Identity Management Service
참조 규격: 3GPP TS 33.180, OAuth 2.0 (RFC 6749), PKCE (RFC 7636)

#### 인증 흐름

```
UE                          CSC IdMS
 │                               │
 │── GET /idms/authreq ─────────→│  user_name, user_password,
 │   ?code_challenge=...         │  code_challenge (S256)
 │   ?code_challenge_method=S256 │
 │                               │  사용자 인증
 │                               │  auth_code 생성·저장
 │←── 200 { code, state } ───────│
 │                               │
 │── POST /idms/tokenreq ───────→│  grant_type=authorization_code
 │   { code, code_verifier, ... }│  PKCE 검증
 │                               │  access_token, id_token 발급
 │←── 200 { access_token, ... } ─│
 │                               │
 │── POST /idms/tokenreq ───────→│  grant_type=refresh_token
 │   { refresh_token }           │  Refresh Token Rotation
 │←── 200 { new tokens } ────────│
```

---

#### `GET /idms/authreq`
인증 코드 요청 (PKCE 필수).

**Query Parameters**

| 파라미터 | 필수 | 설명 |
|---|---|---|
| `user_name` | ✓ | MCPTT ID (`tel:+82571900001`) |
| `user_password` | ✓ | 사용자 비밀번호 |
| `client_id` | ✓ | 클라이언트 ID (예: `MCPTT_UE`) |
| `redirect_uri` | ✓ | 리다이렉트 URI |
| `code_challenge` | ✓ | SHA-256(code_verifier) Base64URL |
| `code_challenge_method` | ✓ | 반드시 `S256` |
| `scope` | - | `openid mcptt` |
| `state` | - | CSRF 방어용 임의값 |

**Response 200**
```json
{ "code": "550e8400-e29b-...", "state": "abc123", "Location": "<redirect_uri>" }
```

**Response 302** (인증 실패)
```
Location: <redirect_uri>?error=auth_fail&state=<state>
```

**Response 400**
```json
{ "error": "invalid_request", "error_description": "code_challenge is required (PKCE mandatory)" }
```

**code 유효기간**: `IdMs.AuthCodeTtl` (기본 60초), 1회 사용 후 즉시 무효화

---

#### `POST /idms/tokenreq`
토큰 발급 (Authorization Code) 또는 갱신 (Refresh Token).

**Content-Type**: `application/json`

**Case 1: authorization_code**

```json
{
  "grant_type":    "authorization_code",
  "code":          "550e8400-...",
  "code_verifier": "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1...",
  "client_id":     "MCPTT_UE",
  "redirect_uri":  "https://example.com/callback"
}
```

검증 순서: ① code 존재, ② 만료, ③ 1회성, ④ client_id 일치, ⑤ redirect_uri 일치, ⑥ PKCE SHA-256 검증

**Case 2: refresh_token**

```json
{
  "grant_type":    "refresh_token",
  "refresh_token": "a3f...",
  "client_id":     "MCPTT_UE"
}
```

Refresh Token Rotation 적용 — 기존 토큰 revoke, 새 토큰 발급

**Response 200**
```json
{
  "access_token":  "<JWT>",
  "refresh_token": "<UUID>",
  "id_token":      "<JWT>",
  "token_type":    "Bearer",
  "expires_in":    3600
}
```

**ID Token Payload**
```json
{
  "mcptt_id": "tel:+82571900001",
  "iss":      "idms.mcptt.com",
  "sub":      "<uuid>",
  "aud":      "mcptt_client",
  "exp":      1748000000,
  "iat":      1745000000
}
```

**Access Token Payload**
```json
{
  "mcptt_id": "tel:+82571900001",
  "aud":      "mcptt_client",
  "scope":    ["openid", "mcptt"],
  "exp":      1748000000
}
```

유효기간: `IdMs.AccessTokenTtl` (기본 3600초)
Refresh Token 유효기간: `IdMs.RefreshTokenTtl` (기본 604800초 = 7일)

---

#### `POST /idms/introspect`
토큰 유효성 검사 (RFC 7662).

**Request** (`application/json` 또는 form)
```json
{ "token": "<access_token>" }
```

**Response 200 — 유효**
```json
{
  "active":    true,
  "mcptt_id":  "tel:+82571900001",
  "aud":       "mcptt_client",
  "exp":       1748000000,
  "iat":       1745000000,
  "scope":     "openid mcptt"
}
```

**Response 200 — 무효/만료**
```json
{ "active": false }
```

---

### 4.2 GMS — Group Management Service
참조 규격: OMA POC XDM, 3GPP TS 24.481
데이터: `Data.Group/*.json` (메모리 캐시 + 파일 영속화)
인증: MCPTT Access Token 필수

---

#### `GET /org.openmobilealliance.groups/users/{user_uri}`
사용자가 속한 그룹 목록 조회 (JSON).

**Path**: `user_uri` = tel: URI (URL 인코딩, 예: `tel%3A%2B82571900001`)
**Headers**: `Authorization: Bearer <access_token>`

**Response 200** (`application/json`)
```json
[
  {
    "uri":          "tel:+82571900000",
    "display_name": "MCPTT 그룹 A",
    "etag":         "etag_1711234567",
    "member_count": 5
  }
]
```

---

#### `GET /org.openmobilealliance.groups/users/{user_uri}/{group_uri}`
그룹 상세 조회 (OMA POC XML).

**Headers**: `Authorization: Bearer <access_token>`
**Conditional**: `If-None-Match: <etag>` → 변경 없으면 `304 Not Modified`

**Response 200** (`application/vnd.oma.poc.groups+xml`)
```xml
<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:ocp="urn:oma:xml:xdm:common-policy"
  xmlns:oxe="urn:oma:xml:xdm:extensions"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">
  <list-service uri="tel:+82571900000">
    <display-name xml:lang="en-us">MCPTT 그룹 A</display-name>
    <list>
      <entry uri="tel:+82571900001">
        <rl:display-name>홍길동</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:user-priority>1</mcpttgi:user-priority>
      </entry>
    </list>
    <mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>
    <mcpttgi:on-network-max-participant-count>10</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-hang-time>3</mcpttgi:on-network-hang-time>
    <mcpttgi:on-network-max-duration>3600</mcpttgi:on-network-max-duration>
    <mcpttgi:on-network-require-talker-id>false</mcpttgi:on-network-require-talker-id>
    <cp:ruleset>
      <cp:rule id="a7c">
        <cp:actions>
          <mcpttgi:allow-MCPTT-emergency-call>true</mcpttgi:allow-MCPTT-emergency-call>
          <mcpttgi:allow-imminent-peril-call>true</mcpttgi:allow-imminent-peril-call>
          <mcpttgi:allow-MCPTT-emergency-alert>true</mcpttgi:allow-MCPTT-emergency-alert>
        </cp:actions>
      </cp:rule>
    </cp:ruleset>
    <oxe:supported-services>
      <oxe:service enabler="example.mcptt">
        <oxe:group-media>
          <mcpttgi:mcptt-speech/>
        </oxe:group-media>
      </oxe:service>
    </oxe:supported-services>
    <mcpttgi:on-network-group-priority>5</mcpttgi:on-network-group-priority>
  </list-service>
</group>
```

**Headers**: `ETag: etag_1711234567`

---

#### `PUT /org.openmobilealliance.groups/users/{user_uri}/{group_uri}`
그룹 생성/갱신.

현재 구현: 요청자를 owner로 하는 기본 그룹 생성 (XML 파싱 미지원).
생성 후 CSP에 UDP 알림 전송.

**Response 200** — 생성된 그룹 XML
**Side effect**: `Group/{id}.json` 파일 갱신, CSP UDP 알림

---

#### `DELETE /org.openmobilealliance.groups/users/{user_uri}/{group_uri}`
그룹 삭제.

**Response 200**
**Side effect**: `Group/{id}.json` 파일 삭제, CSP UDP 알림

---

### 4.3 CMS — Configuration Management Service
참조 규격: 3GPP TS 24.484
데이터: `Data.User/*.json` (메모리 캐시)
인증: MCPTT Access Token 필수

---

#### `GET /org.3gpp.mcptt.user-profile/users/{user_uri}/user-profile`
사용자 프로파일 조회.

**Headers**: `Authorization: Bearer <access_token>`
**Conditional**: `If-None-Match: <etag>` 지원

**Response 200** (`application/vnd.3gpp.mcptt-user-profile+xml`)
```xml
<?xml version="1.0" encoding="UTF-8"?>
<mcptt-user-profile xmlns="urn:3gpp:ns:mcpttUserProfile:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
  user-profile-index="1">
  <Name>
    <display-name xml:lang="en">홍길동</display-name>
  </Name>
  <Common>
    <MCPTTUserID>tel:+82571900001</MCPTTUserID>
    <PrivateCall>
      <MaxSimultaneousCallsN6>1</MaxSimultaneousCallsN6>
      <MaxCallsN7>1</MaxCallsN7>
      <EmergencyCall>
        <MCPTTUserID>tel:+82571900001</MCPTTUserID>
      </EmergencyCall>
    </PrivateCall>
    <EmergencyAlert>
      <MCPTTUserID>tel:+82571900001</MCPTTUserID>
    </EmergencyAlert>
  </Common>
  <OnNetwork>
    <MCPTTUserID>tel:+82571900001</MCPTTUserID>
  </OnNetwork>
</mcptt-user-profile>
```

**Headers**: `ETag: etag_tel:+82571900001`

---

#### `GET /org.3gpp.mcptt.service-config/users/{user_uri}/service-config`
서비스 설정 조회.

**Headers**: `Authorization: Bearer <access_token>`
**Conditional**: `If-None-Match` 지원

**Response 200** (`application/vnd.3gpp.mcptt-service-config+xml`)
```xml
<?xml version="1.0" encoding="UTF-8"?>
<mcptt-service-config xmlns="urn:3gpp:ns:mcpttServiceConfig:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <num-levels-group-hierarchy>3</num-levels-group-hierarchy>
  <num-levels-user-hierarchy>3</num-levels-user-hierarchy>
  <max-affiliations-N2>10</max-affiliations-N2>
  <allow-create-delete-group>true</allow-create-delete-group>
  <allow-private-call>true</allow-private-call>
  <allow-emergency-call>true</allow-emergency-call>
  <allow-alert>true</allow-alert>
  <on-network>
    <allow-transmit-request>true</allow-transmit-request>
    <max-on-network-affiliations-N2>10</max-on-network-affiliations-N2>
  </on-network>
</mcptt-service-config>
```

**Headers**: `ETag: svcfg_etag_v1`

---

### 4.4 KMS — Key Management Service
참조 규격: 3GPP TS 33.180 (MIKEY-SAKKE)
인증: MCPTT Access Token 필수
설정: `IdMs.KmsUri`, `IdMs.KmsClientReqUrl`, `IdMs.Domain`

---

#### `GET /keymanagement/identity/v1/init`
KMS 초기화 — 루트 인증서 및 공개키 수신.

**Headers**: `Authorization: Bearer <access_token>`

**Response 200** (`application/xml`)
```xml
<?xml version="1.0" encoding="UTF-8"?>
<KmsResponse Version="1.1.0" xmlns="http://org.csc.kms"
  xmlns:ds="http://www.w3.org/2000/09/xmldsig#">
  <KmsUri>kms.mcptt.com</KmsUri>
  <UserUri>tel:+82571900001@mcptt.com</UserUri>
  <Time>2025-03-25T10:00:00+00:00</Time>
  <KmsId>kmsprovider12345</KmsId>
  <ClientReqUrl>http://localhost:4420/keymanagement/identity/v1/init</ClientReqUrl>
  <KmsMessage>
    <KmsInit Version="1.0.0">
      <KmsCertificate Version="1.0.0" Role="Root">
        <KmsUri>kms.mcptt.com</KmsUri>
        <Issuer>www.mcptt.com</Issuer>
        <ValidFrom>2025-03-25T10:00:00+00:00</ValidFrom>
        <ValidTo>2045-03-25T10:00:00+00:00</ValidTo>
        <UserIdFormat>2</UserIdFormat>
        <UserKeyPeriod>2419200</UserKeyPeriod>
        <UserKeyOffset>0</UserKeyOffset>
        <PubEncKey>041C7B84...</PubEncKey>
        <PubAuthKey>0467EF33...</PubAuthKey>
        <ParameterSet>1</ParameterSet>
      </KmsCertificate>
    </KmsInit>
  </KmsMessage>
</KmsResponse>
```

---

#### `GET /keymanagement/identity/v1/keyprov`
사용자 키 프로비저닝.

**Headers**: `Authorization: Bearer <access_token>`

**Response 200** (`application/xml`) — init과 동일 구조 (ValidFrom/ValidTo 고정값)

---

## 5. 내부 인터페이스

### 5.1 CSC → CSP 이벤트 알림 (UDP)

그룹 정보가 변경될 때 CSC가 CSP로 UDP JSON 메시지를 송신합니다.

**대상**: `127.0.0.1:4421`
**프로토콜**: UDP
**인코딩**: UTF-8 JSON

**메시지 형식**
```json
{
  "event":  "group_change",
  "uri":    "tel:+82571900000",
  "action": "PUT",            // "PUT" | "DELETE"
  "etag":   "etag_1711234567"
}
```

**발생 시점**

| 액션 | 발생 조건 |
|---|---|
| `PUT` | GMS `PUT /.../{group_uri}` — 그룹 생성 또는 갱신 |
| `DELETE` | GMS `DELETE /.../{group_uri}` — 그룹 삭제 |

---

### 5.2 데이터 파일

#### User JSON (`Data.User/**/*.json`)

파일명 = MSISDN (예: `+82571900001.json`)

```json
{
  "passwd": "sip_password",
  "name":   "홍길동"
}
```

메모리 로드 키: `tel:+82571900001`
MCPTT IdMS 인증 및 CMS 프로파일 생성에 사용

---

#### Group JSON (`Data.Group/*.json`)

파일명 = 그룹 번호 (예: `82571900000.json`)

```json
{
  "name":       "MCPTT 그룹 A",
  "created_by": "tel:+82571900001",
  "created_at": "2025-01-01T00:00:00",
  "etag":       "etag_1711234567",
  "users": [
    { "id": "82571900001", "priority": 1, "role": "owner",       "joined_at": "" },
    { "id": "82571900002", "priority": 2, "role": "participant",  "joined_at": "" }
  ]
}
```

메모리 로드 키: `tel:+82571900000`

---

### 5.3 MariaDB 스키마

#### DB: `csc_idms` — IdMS 영속성 (`Database` config)

**테이블: idms_auth_codes**

| 컬럼 | 타입 | 설명 |
|---|---|---|
| code | VARCHAR PK | UUID auth code |
| data | JSON | auth_data (user_id, client_id, redirect_uri, scope, state, issued_at, expires_at, used, code_challenge, code_challenge_method) |
| created_at | DATETIME | 생성 시각 |

**테이블: idms_refresh_tokens**

| 컬럼 | 타입 | 설명 |
|---|---|---|
| token | VARCHAR PK | UUID refresh token |
| data | JSON | {user_id, client_id, scope, issued_at, expires_at, revoked, rotated_to} |
| created_at | DATETIME | 생성 시각 |

---

#### DB: `cims` — 관리 데이터 (`CimsDatabase` config)

**cims_users**

| 컬럼 | 타입 | 설명 |
|---|---|---|
| id | INT AUTO_INCREMENT PK | 가입자 ID |
| name | VARCHAR | 이름 |
| email | VARCHAR | 이메일 |
| password | VARCHAR | SHA-256 해시 |
| role | ENUM | `admin` \| `user` |
| org_id | VARCHAR | 소속 조직 |
| details | TEXT | 비고 |
| create_time | DATETIME | |
| update_time | DATETIME | |

**cims_user_rejects**

| 컬럼 | 설명 |
|---|---|
| user_id | → cims_users.id |
| reject_id | 착신 거부 MSISDN |

**cims_call_users** / **cims_ptt_users**

| 컬럼 | 타입 | 설명 |
|---|---|---|
| id | VARCHAR PK | MSISDN |
| user_id | INT | → cims_users.id |
| auth_id | VARCHAR | SIP auth_id |
| passwd | VARCHAR | SIP 비밀번호 |
| dnd | TINYINT | 방해금지 |
| forward_id | VARCHAR | 착신 전환 번호 |
| register_time | DATETIME | 최근 REGISTER |
| logout_time | DATETIME | 최근 로그아웃 |

**cims_ptt_groups**

| 컬럼 | 설명 |
|---|---|
| id | VARCHAR PK — 그룹 번호 |
| name | 그룹 이름 |

**cims_ptt_group_members**

| 컬럼 | 설명 |
|---|---|
| group_id | → cims_ptt_groups.id |
| user_id | MSISDN |
| priority | 숫자가 낮을수록 우선순위 높음 |

**cims_call_logs**

| 컬럼 | 타입 | 설명 |
|---|---|---|
| id | INT AUTO_INCREMENT PK | |
| call_id | VARCHAR | SIP Call-ID |
| call_type | ENUM | `voip` \| `ptt` |
| group_id | VARCHAR | PTT 그룹 ID |
| initiator | VARCHAR | 발신자 MSISDN |
| callee | VARCHAR | 수신자 MSISDN |
| state | ENUM | `ringing` \| `active` \| `ended` |
| invite_time | DATETIME | |
| answer_time | DATETIME | |
| end_time | DATETIME | |
| duration | INT | 통화 시간(초) |
| sip_status | INT | 최종 SIP 응답 코드 |
| end_reason | VARCHAR | `normal` \| `busy` \| `cancel` \| `timeout` \| `error` |

**cims_call_participants**

| 컬럼 | 설명 |
|---|---|
| log_id | → cims_call_logs.id |
| msisdn | 참가자 MSISDN |
| role | `caller` \| `callee` \| `participant` |
| join_time | DATETIME |
| leave_time | DATETIME |

---

## 6. 설정 파일 (`config/csc.json`)

```json
{
  "Server": {
    "Ip":       "0.0.0.0",
    "Port":     4420,
    "CertFile": "../cert/cert.pem",
    "KeyFile":  "../cert/key.pem"
  },
  "McpttServer": {
    "Ip":       "0.0.0.0",
    "Port":     4430,
    "CertFile": "../cert/cert.pem",
    "KeyFile":  "../cert/key.pem"
  },
  "Log": {
    "File":  "../log/csc.log",
    "Level": "DEBUG"
  },
  "Data": {
    "User":  "/path/to/csp/dist/User",
    "Group": "/path/to/csp/dist/Group"
  },
  "Database": {
    "Host":     "localhost",
    "User":     "db_user",
    "Password": "db_pass",
    "Db":       "csc_idms"
  },
  "CimsDatabase": {
    "Host":     "127.0.0.1",
    "Port":     3306,
    "User":     "cims",
    "Password": "cims1234",
    "Db":       "cims"
  },
  "IdMs": {
    "JwtSecret":      "mcptt_jwt_secret_change_me",
    "Issuer":         "idms.mcptt.com",
    "AuthCodeTtl":    60,
    "AccessTokenTtl": 3600,
    "RefreshTokenTtl": 604800,
    "KmsUri":         "kms.mcptt.com",
    "KmsClientReqUrl": "http://localhost:4430/keymanagement/identity/v1/init",
    "Domain":         "mcptt.com"
  },
  "CimsAuth": {
    "JwtSecret": "cims_jwt_secret_change_me"
  }
}
```

---

## 7. 엔드포인트 빠른 참조

### Admin Server (Port 4420)

| Method | Path | 설명 | Auth |
|---|---|---|---|
| POST | `/api/v1/auth/login` | 관리자 로그인 | - |
| POST | `/api/v1/auth/register` | 계정 등록 | - |
| GET | `/api/v1/auth/me` | 내 정보 | CIMS JWT |
| PUT | `/api/v1/auth/password` | 비밀번호 변경 | CIMS JWT |
| GET | `/api/v1/users` | 가입자 목록 | CIMS JWT |
| POST | `/api/v1/users` | 가입자 생성 | CIMS JWT |
| GET | `/api/v1/users/{pid}` | 가입자 조회 | CIMS JWT |
| PUT | `/api/v1/users/{pid}` | 가입자 수정 | CIMS JWT |
| DELETE | `/api/v1/users/{pid}` | 가입자 삭제 | CIMS JWT |
| GET | `/api/v1/users/{pid}/call` | Call 구독 목록 | CIMS JWT |
| POST | `/api/v1/users/{pid}/call` | Call 구독 추가 | CIMS JWT |
| PUT | `/api/v1/users/{pid}/call/{msisdn}` | Call 구독 수정 | CIMS JWT |
| DELETE | `/api/v1/users/{pid}/call/{msisdn}` | Call 구독 삭제 | CIMS JWT |
| GET | `/api/v1/users/{pid}/ptt` | PTT 구독 목록 | CIMS JWT |
| POST | `/api/v1/users/{pid}/ptt` | PTT 구독 추가 | CIMS JWT |
| PUT | `/api/v1/users/{pid}/ptt/{msisdn}` | PTT 구독 수정 | CIMS JWT |
| DELETE | `/api/v1/users/{pid}/ptt/{msisdn}` | PTT 구독 삭제 | CIMS JWT |
| GET | `/api/v1/ptt/groups` | PTT 그룹 목록 | CIMS JWT |
| POST | `/api/v1/ptt/groups` | PTT 그룹 생성 | CIMS JWT |
| GET | `/api/v1/ptt/groups/{id}` | PTT 그룹 조회 | CIMS JWT |
| PUT | `/api/v1/ptt/groups/{id}` | PTT 그룹 수정 | CIMS JWT |
| DELETE | `/api/v1/ptt/groups/{id}` | PTT 그룹 삭제 | CIMS JWT |
| GET | `/api/v1/ptt/groups/{id}/members` | 멤버 목록 | CIMS JWT |
| POST | `/api/v1/ptt/groups/{id}/members` | 멤버 추가 | CIMS JWT |
| DELETE | `/api/v1/ptt/groups/{id}/members/{uid}` | 멤버 삭제 | CIMS JWT |
| GET | `/api/v1/call/logs` | 통화 이력 조회 | CIMS JWT |
| GET | `/api/v1/call/logs/active` | 진행 중 통화 | CIMS JWT |

### MCPTT Server (Port 4430)

| Method | Path | 설명 | Auth |
|---|---|---|---|
| GET | `/idms/authreq` | 인증 코드 요청 (PKCE) | - |
| POST | `/idms/tokenreq` | 토큰 발급/갱신 | - |
| POST | `/idms/introspect` | 토큰 검증 (RFC 7662) | - |
| GET | `/org.openmobilealliance.groups/users/{user_uri}` | 내 그룹 목록 | MCPTT Token |
| GET | `/org.openmobilealliance.groups/users/{user_uri}/{group_uri}` | 그룹 XML 조회 | MCPTT Token |
| PUT | `/org.openmobilealliance.groups/users/{user_uri}/{group_uri}` | 그룹 생성/갱신 | MCPTT Token |
| DELETE | `/org.openmobilealliance.groups/users/{user_uri}/{group_uri}` | 그룹 삭제 | MCPTT Token |
| GET | `/org.3gpp.mcptt.user-profile/users/{user_uri}/user-profile` | 사용자 프로파일 XML | MCPTT Token |
| GET | `/org.3gpp.mcptt.service-config/users/{user_uri}/service-config` | 서비스 설정 XML | MCPTT Token |
| GET | `/keymanagement/identity/v1/init` | KMS 초기화 | MCPTT Token |
| GET | `/keymanagement/identity/v1/keyprov` | KMS 키 프로비저닝 | MCPTT Token |

### 내부 인터페이스

| 방향 | 프로토콜 | 주소 | 설명 |
|---|---|---|---|
| CSC → CSP | UDP JSON | 127.0.0.1:4421 | 그룹 변경 이벤트 |
