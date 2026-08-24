# CIMS 가입자 관리 콘솔 API 명세 및 설명서

## 1. 개요

CIMS 관리 콘솔은 REST API 기반으로 사용자, 구독(Call/PTT 번호), PTT 그룹을 관리합니다.
API 서버는 CSC (`csc/src/csc_app.py`) 가 HTTPS 포트 4421에서 제공합니다.

**기본 URL:** `https://<서버IP>:4421/api/v1`
**인증:** JWT Bearer Token (`Authorization: Bearer <token>`)
**콘텐츠 타입:** `application/json`

**공통 오류 응답:**
| HTTP 코드 | 의미 | 응답 예시 |
|-----------|------|----------|
| 400 | 잘못된 요청 (필수 항목 누락, 형식 오류) | `{"error": "필수 항목이 누락되었습니다"}` |
| 401 | 인증 실패 (토큰 만료 또는 미포함) | `{"error": "인증이 필요합니다"}` |
| 403 | 권한 없음 (admin 전용 API에 user 접근) | `{"error": "관리자 권한이 필요합니다"}` |
| 404 | 리소스 없음 | `{"error": "사용자를 찾을 수 없습니다"}` |
| 409 | 중복 (아이디 또는 번호) | `{"error": "이미 사용 중인 아이디입니다"}` |
| 500 | 서버 내부 오류 | `{"error": "내부 오류가 발생했습니다"}` |

---

## 2. 인증 API (`/api/v1/auth`)

### 2.1 로그인

```
POST /api/v1/auth/login
Content-Type: application/json
```

**요청:**
```json
{
  "login_id": "admin",
  "password": "1234"
}
```

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4421/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"login_id":"admin","password":"1234"}'
```

**성공 응답 (200):**
```json
{
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpZCI6MzMsInJvbGUiOiJhZG1pbiIsImlhdCI6MTcxMTkwMDAwMCwiZXhwIjoxNzExOTg2NDAwfQ.abc123def456",
  "user": {
    "id": 33,
    "name": "관리자",
    "login_id": "admin",
    "role": "admin",
    "org_id": "",
    "details": null,
    "reject_id": [],
    "call_subscriptions": [],
    "ptt_subscriptions": [
      {
        "id": "+821030432632",
        "auth_id": "4503382103043263@ptt.mnc033.mcc450.3gppnetwork.org",
        "passwd": "1234",
        "dnd": false,
        "forward_id": "",
        "register_time": "2026-03-31T19:35:38",
        "logout_time": null
      }
    ],
    "create_time": "2026-03-25T10:00:00",
    "update_time": "2026-03-31T15:30:00"
  }
}
```

**실패 응답 (400 - 필수 항목 누락):**
```json
{
  "error": "아이디와 비밀번호를 입력하세요"
}
```

**실패 응답 (401 - 인증 실패):**
```json
{
  "error": "아이디 또는 비밀번호가 잘못되었습니다"
}
```

> **JWT 토큰 구조:**
> - Header: `{"alg":"HS256","typ":"JWT"}`
> - Payload: `{"id":<user_id>, "role":"admin|user", "iat":<발급시각>, "exp":<만료시각>}`
> - 만료 시간: 기본 24시간 (86400초)

### 2.2 회원가입

```
POST /api/v1/auth/register
Content-Type: application/json
```

**요청:**
```json
{
  "name": "홍길동",
  "login_id": "hong001",
  "password": "mypassword"
}
```

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4421/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{"name":"홍길동","login_id":"hong001","password":"mypassword"}'
```

**성공 응답 (201):**
```json
{
  "token": "eyJhbGciOiJIUzI1NiIs...",
  "user": {
    "id": 34,
    "name": "홍길동",
    "login_id": "hong001",
    "role": "user",
    "org_id": "",
    "details": null,
    "reject_id": [],
    "call_subscriptions": [],
    "ptt_subscriptions": [],
    "create_time": "2026-03-31T20:00:00",
    "update_time": "2026-03-31T20:00:00"
  }
}
```
> 회원가입 시 role은 항상 `"user"`로 고정됩니다. admin 계정은 직접 DB에서 변경해야 합니다.

**실패 응답 (400 - 유효성 검증):**
```json
{
  "error": "비밀번호는 4자 이상이어야 합니다"
}
```

**실패 응답 (409 - 중복):**
```json
{
  "error": "이미 사용 중인 아이디입니다"
}
```

### 2.3 내 정보 조회

```
GET /api/v1/auth/me
Authorization: Bearer <token>
```

**curl 예시:**
```bash
curl -k -X GET https://192.168.0.2:4421/api/v1/auth/me \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**응답 (200):**
```json
{
  "id": 33,
  "name": "관리자",
  "login_id": "admin",
  "role": "admin",
  "org_id": "",
  "details": null,
  "reject_id": [],
  "call_subscriptions": [
    {
      "id": "+821012345678",
      "auth_id": "+821012345678",
      "passwd": "1234",
      "dnd": false,
      "forward_id": "",
      "register_time": "2026-03-31T10:00:00",
      "logout_time": null
    }
  ],
  "ptt_subscriptions": [
    {
      "id": "+821030432632",
      "auth_id": "4503382103043263@ptt.mnc033.mcc450.3gppnetwork.org",
      "passwd": "1234",
      "dnd": false,
      "forward_id": "",
      "register_time": "2026-03-31T19:35:38",
      "logout_time": null
    }
  ],
  "create_time": "2026-03-25T10:00:00",
  "update_time": "2026-03-31T15:30:00"
}
```

### 2.4 비밀번호 변경

```
PUT /api/v1/auth/password
Authorization: Bearer <token>
Content-Type: application/json
```

**요청:**
```json
{
  "old_password": "현재비밀번호",
  "new_password": "새비밀번호"
}
```

**성공 응답 (200):**
```json
{
  "ok": true
}
```

**실패 응답 (401):**
```json
{
  "error": "현재 비밀번호가 잘못되었습니다"
}
```

---

## 3. 사용자 관리 API (`/api/v1/users`)

> 관리자(role=admin) 전용 API입니다.

### 3.1 사용자 목록 조회

```
GET /api/v1/users
Authorization: Bearer <admin_token>
```

**curl 예시:**
```bash
curl -k -X GET https://192.168.0.2:4421/api/v1/users \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**응답 (200):**
```json
[
  {
    "id": 1,
    "name": "테스트001",
    "login_id": "test001",
    "role": "user",
    "org_id": "본부1팀",
    "details": "테스트 사용자",
    "reject_id": ["+821099990001"],
    "call_subscriptions": [
      {
        "id": "+821012345678",
        "auth_id": "+821012345678",
        "passwd": "1234",
        "dnd": false,
        "forward_id": "",
        "register_time": "2026-03-31T10:00:00",
        "logout_time": null
      }
    ],
    "ptt_subscriptions": [
      {
        "id": "+82571900001",
        "auth_id": "4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org",
        "passwd": "123456",
        "dnd": false,
        "forward_id": "",
        "register_time": "2026-03-31T12:00:00",
        "logout_time": null
      }
    ],
    "create_time": "2026-03-25T10:00:00",
    "update_time": "2026-03-31T15:30:00"
  },
  {
    "id": 2,
    "name": "테스트002",
    "login_id": "test002",
    "role": "user",
    "org_id": "본부2팀",
    "details": null,
    "reject_id": [],
    "call_subscriptions": [],
    "ptt_subscriptions": [],
    "create_time": "2026-03-26T09:00:00",
    "update_time": "2026-03-26T09:00:00"
  }
]
```

### 3.2 사용자 추가

```
POST /api/v1/users
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "name": "신규사용자",
  "login_id": "user001",
  "password": "initpwd1234",
  "org_id": "본부1팀",
  "details": "비고 내용",
  "reject_id": ["+821099990001", "+821099990002"]
}
```

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `name` | string | Y | 표시 이름 (1~128자) |
| `login_id` | string | Y | 로그인 아이디 (중복 불가) |
| `password` | string | Y | 초기 비밀번호 (4자 이상) |
| `org_id` | string | N | 소속 조직 |
| `details` | string | N | 비고 |
| `reject_id` | string[] | N | 착신거부 번호 목록 (E.164 형식) |

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4421/api/v1/users \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{
    "name": "신규사용자",
    "login_id": "user001",
    "password": "initpwd1234",
    "org_id": "본부1팀"
  }'
```

**성공 응답 (201):**
```json
{
  "id": 35,
  "name": "신규사용자",
  "login_id": "user001",
  "role": "user",
  "org_id": "본부1팀",
  "details": "비고 내용",
  "reject_id": ["+821099990001", "+821099990002"],
  "call_subscriptions": [],
  "ptt_subscriptions": [],
  "create_time": "2026-03-31T20:30:00",
  "update_time": "2026-03-31T20:30:00"
}
```

### 3.3 사용자 수정

```
PUT /api/v1/users/{user_id}
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:** (변경할 필드만 포함)
```json
{
  "name": "변경이름",
  "org_id": "본부2팀",
  "reject_id": ["+821099990003"]
}
```

**curl 예시:**
```bash
curl -k -X PUT https://192.168.0.2:4421/api/v1/users/35 \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{"name":"변경이름","org_id":"본부2팀"}'
```

**성공 응답 (200):**
```json
{
  "id": 35,
  "name": "변경이름",
  "login_id": "user001",
  "role": "user",
  "org_id": "본부2팀",
  "details": "비고 내용",
  "reject_id": ["+821099990003"],
  "call_subscriptions": [],
  "ptt_subscriptions": [],
  "create_time": "2026-03-31T20:30:00",
  "update_time": "2026-03-31T21:00:00"
}
```

### 3.4 사용자 삭제

```
DELETE /api/v1/users/{user_id}
Authorization: Bearer <admin_token>
```

**curl 예시:**
```bash
curl -k -X DELETE https://192.168.0.2:4421/api/v1/users/35 \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**성공 응답 (200):**
```json
{
  "ok": true
}
```

> 연결된 Call/PTT 구독 및 PTT 그룹 멤버십도 함께 삭제됩니다 (CASCADE).

**실패 응답 (404):**
```json
{
  "error": "사용자를 찾을 수 없습니다"
}
```

---

## 4. Call 구독 관리 (`/api/v1/users/{pid}/call`)

Call 구독은 VoIP 전화번호를 사용자에게 할당하는 기능입니다.

### 4.1 Call 번호 추가

```
POST /api/v1/users/{pid}/call
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "id": "+821012345678",
  "auth_id": "+821012345678",
  "passwd": "1234",
  "dnd": false,
  "forward_id": ""
}
```

| 필드 | 타입 | 필수 | 기본값 | 설명 |
|------|------|------|--------|------|
| `id` | string | Y | - | MSISDN (E.164 형식, `+` 국가코드 포함) |
| `auth_id` | string | N | id와 동일 | SIP Digest 인증 ID |
| `passwd` | string | Y | - | SIP Digest 비밀번호 — **저장되지 않는다.** `ha1=MD5(imsi@domain:realm:passwd)` 로 변환해 저장(realm = 서비스 `auth_realm ?? domain`). 따라서 `service_ref` 가 해석되어야 한다(400) |
| `sip_transport` | string | N | null | 채널 정책 `UDP`/`TCP`/`TLS`. **`TLS` 는 서버가 집행** — 이 번호의 비-TLS 채널 요청은 REGISTER 포함 403. `UDP`/`TCP` 는 단말 프로비저닝 힌트, null 은 단말 선택 |
| `auth_scheme` | string | N | `digest` | 인증 체계 `digest`(SIP Digest, `ha1`) / `aka`(IMS AKA over TLS — `sip_transport` 와 무관하게 TLS 채널만 허용). 마이그레이션(`migrate_subscription_aka.sql`) 전 DB 에서는 400 |
| `k` / `opc` / `op` / `amf` | string | aka 면 Y | - / `8000` | IMS AKA 자료(hex32 / hex32 / hex32 → OPc 유도 / hex4). **저장 형식은 CSC `AuC.Kek` 암호화**이며 어떤 응답에도 원문이 나가지 않는다(조회는 `auth_scheme`·`aka_provisioned`). 키를 넣으면 SQN 이 0 으로 리셋. `AuC.Kek` 미설정이면 503 |
| `dnd` | boolean | N | false | 방해금지 모드 |
| `forward_id` | string | N | "" | 착신전환 번호 (E.164 형식) |

> 변경(PUT) 시 `passwd` 는 바꿀 때만 보낸다 — 미전송/빈값이면 기존 `ha1` 이 유지된다. `ha1` 은
> (imsi, 서비스 domain/realm) 에 결박되므로 **`imsi` 나 `service_ref` 를 바꾸는 요청은 `passwd` 를
> 함께 보내야 한다**(400 `passwd required when imsi or service_ref changes (ha1 rebinding)`).
> 서비스의 `domain`/`auth_realm` 변경은 그 서비스 전 가입자의 `ha1` 을 무효화한다 — 전 가입자
> 비밀번호 재설정 없이는 바꾸지 않는다([sip_access_security.md §4.3](../design/features/sip_access_security.md)).
> `auth_scheme=aka` 로 바꾸는 PUT 은 보관된 키가 없으면 `k`/`opc`(또는 `op`)를 함께 보내야 한다(400).

**내부 AV API (CSP → CSC, Cx MAR/MAA 상당 — [sip_access_security.md §8.2](../design/features/sip_access_security.md))**

콘솔 경로가 아니다 — admin 서버(4421)에 붙지만 `/api/v1` 밖이라 OAM 게이트웨이가 프록시하지 않고, 관리자 JWT 가
아니라 모듈 간 공유 토큰(`csc.json InternalApi.Token` = `csp.json Setup.Csc.InternalToken`)으로 인증한다.

```
POST /internal/aka/av            Authorization: Bearer <InternalApi.Token>
{ "msisdn": "+82…", "service": "volte"|"ptt"|"", "rand": "<hex32>", "auts": "<hex28>" }   // rand/auts 는 AUTS 재동기 때만
200 { "scheme":"aka", "msisdn", "service", "resynced":bool, "av": { "rand","autn","xres","ck","ik" } }   // hex
401 토큰 불일치 · 404 unknown_subscriber · 409 scheme_mismatch|keys_not_provisioned · 422 auts_invalid ·
500 key_material(KEK 불일치) · 503 auc_disabled|schema_not_migrated
```

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4421/api/v1/users/1/call \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{"id":"+821012345678","passwd":"1234"}'
```

**성공 응답 (201):**
```json
{
  "id": "+821012345678",
  "auth_id": "+821012345678",
  "passwd": "1234",
  "dnd": false,
  "forward_id": "",
  "register_time": null,
  "logout_time": null
}
```

**실패 응답 (409 - 번호 중복):**
```json
{
  "error": "이미 등록된 번호입니다"
}
```

### 4.2 Call 번호 수정

```
PUT /api/v1/users/{pid}/call/{msisdn}
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:** (변경할 필드만)
```json
{
  "auth_id": "new_auth_id",
  "dnd": true,
  "forward_id": "+821099999999"
}
```

**curl 예시:**
```bash
curl -k -X PUT "https://192.168.0.2:4421/api/v1/users/1/call/%2B821012345678" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{"dnd":true,"forward_id":"+821099999999"}'
```

> 주의: URL의 `+` 문자는 `%2B`로 인코딩해야 합니다.

**성공 응답 (200):**
```json
{
  "id": "+821012345678",
  "auth_id": "new_auth_id",
  "passwd": "1234",
  "dnd": true,
  "forward_id": "+821099999999",
  "register_time": "2026-03-31T10:00:00",
  "logout_time": null
}
```

### 4.3 Call 번호 삭제

```
DELETE /api/v1/users/{pid}/call/{msisdn}
Authorization: Bearer <admin_token>
```

**curl 예시:**
```bash
curl -k -X DELETE "https://192.168.0.2:4421/api/v1/users/1/call/%2B821012345678" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**성공 응답 (200):**
```json
{
  "ok": true
}
```

---

## 5. PTT 구독 관리 (`/api/v1/users/{pid}/ptt`)

PTT 구독은 MCPTT 전화번호를 사용자에게 할당합니다. Call 구독과 동일한 구조이나, `auth_id`에 3GPP IMPI 형식을 사용합니다.

### 5.1 PTT 번호 추가

```
POST /api/v1/users/{pid}/ptt
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "id": "+82571900001",
  "auth_id": "4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org",
  "passwd": "123456",
  "dnd": false,
  "forward_id": ""
}
```

**auth_id (IMPI) 형식:**
```
<MCC><MNC><MSISDN_digits>@ptt.mnc<MNC>.mcc<MCC>.3gppnetwork.org
```
| 필드 | 예시 | 설명 |
|------|------|------|
| MCC | 450 | Mobile Country Code (한국) |
| MNC | 033 | Mobile Network Code |
| MSISDN | 82571900001 | 국가코드 포함 전화번호 (+ 제외) |

> 예시: `+82571900001` → auth_id: `4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org`
> 구성: MCC(450) + MNC(33) + MSISDN(82571900001) = `4503382571900001`

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4421/api/v1/users/1/ptt \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{
    "id": "+82571900001",
    "auth_id": "4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org",
    "passwd": "123456"
  }'
```

**성공 응답 (201):**
```json
{
  "id": "+82571900001",
  "auth_id": "4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org",
  "passwd": "123456",
  "dnd": false,
  "forward_id": "",
  "register_time": null,
  "logout_time": null
}
```

### 5.2 PTT 번호 수정

```
PUT /api/v1/users/{pid}/ptt/{msisdn}
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "passwd": "newpwd789",
  "dnd": true
}
```

**성공 응답 (200):**
```json
{
  "id": "+82571900001",
  "auth_id": "4503382571900001@ptt.mnc033.mcc450.3gppnetwork.org",
  "passwd": "newpwd789",
  "dnd": true,
  "forward_id": "",
  "register_time": "2026-03-31T12:00:00",
  "logout_time": null
}
```

### 5.3 PTT 번호 삭제

```
DELETE /api/v1/users/{pid}/ptt/{msisdn}
Authorization: Bearer <admin_token>
```

**성공 응답 (200):**
```json
{
  "ok": true
}
```

> 삭제 시 해당 번호가 소속된 PTT 그룹의 멤버에서도 제거됩니다.

---

## 6. PTT 그룹 관리 (`/api/v1/ptt/groups`)

### 6.1 그룹 목록 조회

```
GET /api/v1/ptt/groups
Authorization: Bearer <admin_token>
```

**curl 예시:**
```bash
curl -k -X GET https://192.168.0.2:4421/api/v1/ptt/groups \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**응답 (200):**
```json
[
  {
    "id": "+82571910001",
    "name": "Alpha그룹",
    "video_enabled": true,
    "member_count": 3,
    "members": [
      {"user_id": "+82571900001", "priority": 0},
      {"user_id": "+821030432632", "priority": 0},
      {"user_id": "+82571900002", "priority": 1}
    ]
  },
  {
    "id": "+82571910002",
    "name": "Bravo그룹",
    "video_enabled": false,
    "member_count": 2,
    "members": [
      {"user_id": "+82571900003", "priority": 0},
      {"user_id": "+82571900004", "priority": 1}
    ]
  }
]
```

### 6.2 그룹 생성

```
POST /api/v1/ptt/groups
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "id": "+82571910003",
  "name": "Charlie그룹",
  "video_enabled": false,
  "members": [
    {"user_id": "+82571900001", "priority": 0},
    {"user_id": "+82571900003", "priority": 0},
    {"user_id": "+82571900004", "priority": 1},
    {"user_id": "+82571900005", "priority": 2}
  ]
}
```

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `id` | string | Y | 그룹 MSISDN (E.164 형식) |
| `name` | string | Y | 그룹 표시 이름 |
| `video_enabled` | boolean | N | 영상 지원 여부 (기본: false) |
| `floor_policy` | string | N | 동시 발언 정책 `single`(기본)/`dual`/`multi` |
| `max_talkers` | integer | N | `multi` 의 동시 발언자 수 (2~8, CMP 슬롯 상한). `single`/`dual` 은 미해석 — 2 로 정규화 |
| `members` | array | N | 초기 멤버 목록 |
| `members[].user_id` | string | Y | PTT 구독 MSISDN |
| `members[].priority` | integer | Y | 우선순위 (0=최고, 숫자가 클수록 낮음) |

> `floor_policy`/`max_talkers` 는 CSP 가 `PTT_GROUP_ADD`/`_MODIFY` 로 CMP 에 발행한다
> ([mcptt_csp_cmp_roadmap_contract.md](../design/features/mcptt_csp_cmp_roadmap_contract.md) §B.1).
> `multi` 인데 정원이 범위를 벗어나면 **400 으로 거절**한다 — CMP 가 BAD_REQUEST 로 그룹 생성을
> 거부해 통화 불가가 되는 것을 저장 단계에서 막는다. 정책 변경은 다음 그룹콜 개설부터 적용되며,
> 통화 중 그룹은 CSP 의 설정 해시 변경 감지로 `PTT_GROUP_MODIFY` 가 나간다.
> ⚠️ `multi` 는 **단말이 동시 수신(SSRC 디먹스)을 지원해야** 실제로 겹쳐 들린다
> ([mcptt_ue_multitalker_media.md](../design/features/mcptt_ue_multitalker_media.md) U10).

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4421/api/v1/ptt/groups \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{
    "id": "+82571910003",
    "name": "Charlie그룹",
    "video_enabled": false,
    "members": [
      {"user_id": "+82571900001", "priority": 0},
      {"user_id": "+82571900003", "priority": 1}
    ]
  }'
```

**성공 응답 (201):**
```json
{
  "id": "+82571910003",
  "name": "Charlie그룹",
  "video_enabled": false,
  "member_count": 2,
  "members": [
    {"user_id": "+82571900001", "priority": 0},
    {"user_id": "+82571900003", "priority": 1}
  ]
}
```

**실패 응답 (409 - 그룹 ID 중복):**
```json
{
  "error": "이미 존재하는 그룹 ID입니다"
}
```

### 6.3 그룹 수정

```
PUT /api/v1/ptt/groups/{group_id}
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "name": "Charlie팀 그룹",
  "video_enabled": true
}
```

**성공 응답 (200):**
```json
{
  "id": "+82571910003",
  "name": "Charlie팀 그룹",
  "video_enabled": true,
  "member_count": 2,
  "members": [
    {"user_id": "+82571900001", "priority": 0},
    {"user_id": "+82571900003", "priority": 1}
  ]
}
```

### 6.4 그룹 삭제

```
DELETE /api/v1/ptt/groups/{group_id}
Authorization: Bearer <admin_token>
```

**curl 예시:**
```bash
curl -k -X DELETE "https://192.168.0.2:4421/api/v1/ptt/groups/%2B82571910003" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**성공 응답 (200):**
```json
{
  "ok": true
}
```

> 삭제 시 진행 중인 그룹 통화가 있으면 CMP에 `removegroup` 명령이 전송됩니다.

### 6.5 그룹 멤버 추가

```
POST /api/v1/ptt/groups/{group_id}/members
Authorization: Bearer <admin_token>
Content-Type: application/json
```

**요청:**
```json
{
  "user_id": "+82571900005",
  "priority": 2
}
```

**curl 예시:**
```bash
curl -k -X POST "https://192.168.0.2:4421/api/v1/ptt/groups/%2B82571910001/members" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..." \
  -H "Content-Type: application/json" \
  -d '{"user_id":"+82571900005","priority":2}'
```

**성공 응답 (201):**
```json
{
  "user_id": "+82571900005",
  "priority": 2
}
```

**실패 응답 (409 - 이미 멤버):**
```json
{
  "error": "이미 그룹에 소속된 멤버입니다"
}
```

**실패 응답 (404 - PTT 구독 없음):**
```json
{
  "error": "등록되지 않은 PTT 번호입니다"
}
```

### 6.6 그룹 멤버 삭제

```
DELETE /api/v1/ptt/groups/{group_id}/members/{user_id}
Authorization: Bearer <admin_token>
```

**curl 예시:**
```bash
curl -k -X DELETE "https://192.168.0.2:4421/api/v1/ptt/groups/%2B82571910001/members/%2B82571900005" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIs..."
```

**성공 응답 (200):**
```json
{
  "ok": true
}
```

---

## 7. DB 스키마

### users (사용자)

| 컬럼 | 타입 | Nullable | 기본값 | 제약조건 | 설명 |
|------|------|----------|--------|----------|------|
| id | INT | N | AUTO_INCREMENT | PK | 사용자 고유 ID |
| name | VARCHAR(128) | N | - | - | 표시 이름 |
| login_id | VARCHAR(255) | N | - | UNIQUE | 로그인 아이디 |
| password | VARCHAR(255) | N | - | - | SHA256 해시 비밀번호 |
| role | ENUM('admin','user') | N | 'user' | - | 역할 |
| org_id | VARCHAR(64) | Y | '' | - | 소속 조직 |
| details | TEXT | Y | NULL | - | 비고 |
| reject_id | TEXT | Y | NULL | - | 착신거부 번호 (JSON 배열 문자열) |
| create_time | DATETIME | N | CURRENT_TIMESTAMP | - | 생성일 |
| update_time | DATETIME | N | CURRENT_TIMESTAMP ON UPDATE | - | 수정일 |

### voip_subscriptions (Call 번호)

| 컬럼 | 타입 | Nullable | 기본값 | 제약조건 | 설명 |
|------|------|----------|--------|----------|------|
| id | VARCHAR(32) | N | - | PK | MSISDN (E.164 형식) |
| user_id | INT | N | - | FK → users.id ON DELETE CASCADE | 소유자 |
| auth_id | VARCHAR(128) | Y | id와 동일 | - | SIP 인증 ID (IMPI) |
| ha1 | CHAR(32) | N | '' | - | SIP Digest H(A1)=MD5(imsi@domain:realm:password) — 인증 자료 SoT |
| passwd | VARCHAR(128) | N | '' | - | 평문 (과도기 — ha1 이행 후 소거·DROP 예정) |
| sip_transport | ENUM('UDP','TCP','TLS') | Y | NULL | - | 채널 정책 (TLS=서버 집행 / UDP·TCP=힌트 / NULL=단말 선택) |
| auth_scheme | ENUM('digest','aka') | N | 'digest' | - | 인증 체계 — CSP 챌린지 체계 선택(aka=IMS AKA over TLS, TLS 채널 강제) — migrate_subscription_aka.sql |
| k_enc / opc_enc | VARCHAR(160) | N | '' | - | AKA K/OPc — CSC `AuC.Kek` 암호화 보관(`v1:<iv><ct><hmac>`), CSC 만 읽는다 |
| sqn | BIGINT UNSIGNED | N | 0 | - | AKA SQN_HE(48-bit) — CSC 단일 발급자만 갱신(AV 발급 +1, AUTS 재동기) |
| amf | CHAR(4) | N | '8000' | - | AKA AMF hex4 |
| dnd | TINYINT(1) | N | 0 | - | 방해금지 (0=off, 1=on) |
| forward_id | VARCHAR(32) | Y | '' | - | 착신전환 번호 (E.164) |
| register_time | DATETIME | Y | NULL | - | 최근 SIP REGISTER 시각 |
| logout_time | DATETIME | Y | NULL | - | 최근 등록해제 시각 |

### ptt_subscriptions (PTT 번호)

| 컬럼 | 타입 | Nullable | 기본값 | 제약조건 | 설명 |
|------|------|----------|--------|----------|------|
| id | VARCHAR(32) | N | - | PK | MSISDN (E.164 형식) |
| user_id | INT | N | - | FK → users.id ON DELETE CASCADE | 소유자 |
| auth_id | VARCHAR(128) | Y | - | - | IMPI (3GPP 형식) |
| ha1 | CHAR(32) | N | '' | - | SIP Digest H(A1) — 인증 자료 SoT |
| passwd | VARCHAR(128) | N | '' | - | 평문 (과도기 — 소거·DROP 예정) |
| sip_transport | ENUM('UDP','TCP','TLS') | Y | NULL | - | 채널 정책 (TLS=서버 집행) |
| auth_scheme | ENUM('digest','aka') | N | 'digest' | - | 인증 체계 — CSP 챌린지 체계 선택(aka=IMS AKA over TLS, TLS 채널 강제) — migrate_subscription_aka.sql |
| k_enc / opc_enc | VARCHAR(160) | N | '' | - | AKA K/OPc — CSC `AuC.Kek` 암호화 보관(`v1:<iv><ct><hmac>`), CSC 만 읽는다 |
| sqn | BIGINT UNSIGNED | N | 0 | - | AKA SQN_HE(48-bit) — CSC 단일 발급자만 갱신(AV 발급 +1, AUTS 재동기) |
| amf | CHAR(4) | N | '8000' | - | AKA AMF hex4 |
| dnd | TINYINT(1) | N | 0 | - | 방해금지 (0=off, 1=on) |
| forward_id | VARCHAR(32) | Y | '' | - | 착신전환 번호 |
| register_time | DATETIME | Y | NULL | - | 최근 SIP REGISTER 시각 |
| logout_time | DATETIME | Y | NULL | - | 최근 등록해제 시각 |

### ptt_groups (PTT 그룹)

| 컬럼 | 타입 | Nullable | 기본값 | 제약조건 | 설명 |
|------|------|----------|--------|----------|------|
| id | VARCHAR(32) | N | - | PK | 그룹 MSISDN (E.164) |
| name | VARCHAR(128) | N | - | - | 그룹 표시 이름 |
| video_enabled | TINYINT(1) | N | 0 | - | 영상 지원 여부 (0=off, 1=on) |

### ptt_group_members (그룹 멤버)

| 컬럼 | 타입 | Nullable | 기본값 | 제약조건 | 설명 |
|------|------|----------|--------|----------|------|
| group_id | VARCHAR(32) | N | - | PK (복합), FK → ptt_groups.id ON DELETE CASCADE | 그룹 ID |
| user_id | VARCHAR(32) | N | - | PK (복합), FK → ptt_subscriptions.id ON DELETE CASCADE | PTT 구독 MSISDN |
| priority | INT | N | 0 | CHECK (priority >= 0) | 우선순위 (0=최고) |

**ER 다이어그램:**
```
users (1) ──────── (N) voip_subscriptions
  │
  └──── (1) ──────── (N) ptt_subscriptions
                              │
                              └──── (N) ──── ptt_group_members ──── (N) ──── ptt_groups
```

---

## 8. 콘솔 화면 기능 설명

### 로그인 화면
- 아이디 / 비밀번호 입력
- 회원가입 가능 (role=user 고정)
- 비밀번호 변경 기능

### 가입자 관리 (관리자 전용)
- **검색**: 이름/아이디/조직/번호로 필터링
- **가입자 추가/편집**: 이름, 아이디, 조직, 세부사항, 착신거부 목록
- **Call 번호 관리**: 인라인 칩으로 표시, 클릭하여 편집, + 버튼으로 추가
- **PTT 번호 관리**: Call과 동일 구조
- 번호 표시: +82 제거 후 `XXX-XXXX-XXXX` 형태

### PTT 그룹 관리 (관리자 전용)
- 그룹 생성/편집/삭제
- 멤버 추가/삭제 (우선순위 설정)
- 영상 지원 여부 토글

### 통화현황
- 실시간 PTT 세션 상태
- 통화 로그 조회

---

## 9. 조직 관리 API (`/api/v1/organizations`)

### 9.1 조직 목록 조회

```
GET /api/v1/organizations
Authorization: Bearer <token>
```

**응답 200:**
```json
[
  {"id": 1, "name": "본부", "code": "HQ", "parent_id": null},
  {"id": 2, "name": "작전과", "code": "OPS", "parent_id": 1}
]
```

### 9.2 조직 생성

```
POST /api/v1/organizations
Content-Type: application/json
Authorization: Bearer <token>

{"name": "작전과", "code": "OPS", "parent_id": 1}
```

### 9.3 조직 수정/삭제

```
PUT /api/v1/organizations/{org_id}
DELETE /api/v1/organizations/{org_id}
```

---

## 10. 통계/상태 API (`/api/v1/stats`)

### 10.1 시스템 상태

```
GET /api/v1/stats/health
Authorization: Bearer <token>
```

**응답 200:**
```json
{
  "health": {"csp": "up", "cmp": "up", "db": "up"},
  "csp": {
    "registered_users": 42, "active_calls": 3, "db_connected": true,
    "roles": {"CSCF": true, "TAS": true, "PTT_AS": true, "IBCF": false},
    "subscribers_total": 5013, "volte_numbers": 5000, "volte_registered": 42,
    "ptt_numbers": 5000, "ptt_registered": 38, "ptt_groups_total": 12
  },
  "cmp": {
    "sessions": 3, "groups": 1,
    "rtp_ports":     {"total": 100, "used": 5, "free": 95},
    "rtp_ports_ptt": {"total": 50,  "used": 2, "free": 48}
  },
  "record_enable": false,
  "voip_calls": [ ... ], "ptt_groups": [ ... ]
}
```

- `csp.{subscribers_total, volte_numbers, volte_registered, ptt_numbers, ptt_registered, ptt_groups_total}` — 대시보드 KPI 용 **DB 카운트**(`_get_dashboard_counts`, 3s 캐시). 등록 = `register_time NOT NULL AND (logout_time NULL OR register_time>logout_time)`.
- `cmp.rtp_ports` = VoIP 풀, `cmp.rtp_ports_ptt` = PTT 전용 풀.
- csp/cmp probe 는 비블로킹(thread gather) + 3s 캐시. probe 대상 IP 는 `oam.json` 의 CspNotify/CmpIp (미지정 시 127.0.0.1 fallback → VIP/미디어 호스트 down 오탐).

### 10.2 가입자 통계

```
GET /api/v1/stats/subscribers
```

**응답 200:**
```json
{
  "total_users": 100,
  "voip_subscriptions": 80,
  "ptt_subscriptions": 60,
  "ptt_groups": 10,
  "registered_now": 42
}
```

### 10.3 서비스 통계 요약

```
GET /api/v1/stats/service/summary?date=2026-04-13
```

**응답 200:**
```json
{
  "date": "2026-04-13",
  "voip": {"total_calls": 150, "answered": 120, "missed": 30, "avg_duration": 45},
  "ptt": {"total_sessions": 20, "total_floor_grants": 85, "avg_members": 4}
}
```

### 10.4 외부 시스템 레지스트리 (`/api/v1/external-systems`)

CIMS agent/HA 밖의 외부 시스템(외부 DB / 모니터링 / 스토리지 / 인증 등)을 등록·감시. file_store 컬렉션(domain `external_systems`) 기반, 신규 DB 테이블 없음.

| Method | Path | 설명 |
|--------|------|------|
| GET | `/external-systems` | 목록 `{systems:[...]}` |
| POST | `/external-systems` | 생성 → `201 {id}` (name·endpoints 필수) |
| GET | `/external-systems/status` | enabled 전체 동시 probe `{items:[{id,status,latency_ms}]}` |
| GET | `/external-systems/{id}` | 1건 |
| PUT | `/external-systems/{id}` | 수정 |
| DELETE | `/external-systems/{id}` | 삭제 `{id,deleted}` |
| POST | `/external-systems/{id}/probe` | 즉시 probe `{id,status,latency_ms,checked_at}` |

**레코드:**
```json
{
  "id": 1, "name": "외부 가입자 DB", "type": "db",
  "endpoints": [{"host": "10.0.1.200", "port": 3306, "label": "primary"}],
  "probe": {"mode": "tcp", "host": "10.0.1.200", "port": 3306, "timeout": 2},
  "tags": [], "enabled": true, "description": ""
}
```
- `type`: `db` | `monitoring` | `storage` | `auth` | `other`
- `probe.mode`: `tcp`(구현, connect → up/down + latency_ms) | `http`·`icmp`(예약 → unknown) | `none`. host/port 미지정 시 `endpoints[0]` fallback.
- 대시보드 SystemTopologyWidget 에 **점선 노드**로 표시.

---

## 11. 통화 이력/Flow API (`/api/v1/call`, `/api/v1/flow`)

### 11.1 통화 이력 조회

```
GET /api/v1/call/logs?call_type=volte&date=2026-07-15&hour=20&limit=20&offset=0
Authorization: Bearer <token>
```

| 파라미터 | 설명 |
|---|---|
| `date` | YYYY-MM-DD (기본: 오늘) |
| `hour` | HH — 지정 시 목록을 해당 시간대로 한정. 내용 필터(msisdn/org/q) 미지정이면 스캔 자체를 해당 시간대 `.d` 로 좁힘 (콘솔 기본 진입 고속 경로) |
| `call_type` | `volte` (volte* 전체) / `ptt` 등 |
| `msisdn` / `org` / `q` | 번호 부분일치 / 부서(하위 포함) / 이름·번호 검색. 지정 시 하루 전체 스캔 (히트맵에 필터 반영) |
| `limit` / `offset` | 기본 200, 최대 1000 |

**응답 200:**
```json
{
  "total": 150,
  "limit": 20,
  "offset": 0,
  "hours": { "09": 12, "20": 3 },
  "logs": [
    {
      "call_id": "abc123",
      "call_type": "volte_audio",
      "initiator": "+821001",
      "callee": "+821002",
      "state": "ended",
      "invite_time": "2026-07-15T14:30:00",
      "answer_time": "2026-07-15T14:30:02",
      "end_time": "2026-07-15T14:31:15",
      "end_reason": "normal",
      "end_reason_ko": "정상종료",
      "dir_name": "volte/2026/07/15/14/+8210000/+821001/S20260715143000123.d",
      "participants": [],
      "has_recording": true
    }
  ]
}
```

- `hours` = 시간대별 호 수 히트맵. **항상 하루 전체 기준** — hour 스코프 조회 시에는
  `.d` 디렉터리 카운트(readdir 만)로 집계해 call.json 읽기 없이 유지된다.
- `participants`/`has_recording` 은 페이지 슬라이스에만 부착 (파일 I/O 최소화).

### 11.1.1 PTT 세션 이력 조회

PTT 는 축이 둘이다. **그룹**은 상시 편성 엔티티라 그룹 드릴다운(`/ptt/history`)이고,
**세션**은 그 자체가 기록 단위라 평면 목록이다. 후자는 11.1 과 같은 계약을 쓴다 —
1:1 private call·ad-hoc 은 그룹 문서가 없는 개별 호(TS 24.379 §11.1 / TS 22.179)라
그룹으로 묶을 자리가 애초에 없다.

```
GET /api/v1/ptt/sessions?date=2026-08-06&kind=private,adhoc&limit=50
```

| 파라미터 | 설명 |
|---|---|
| `date` | YYYY-MM-DD (기본: 오늘) |
| `from` / `to` | 기간 조회 (지정 시 `date` 무시, 최대 90일) |
| `days` | 최근 N일 (`from`/`to` 없을 때) |
| `kind` | `group`/`private`/`adhoc` 콤마 다중. 미지정 = 전체 |
| `group_key` | 녹취 저장 키 콤마 다중 (`ptt/{key}`) — 그룹 세션을 좁힌다 |
| `person` | 참여자 부분일치. 발언하지 않은 참가자도 포함 (`people[]` 기준) |
| `state` | `live`(진행중) / `ended`(종료). 진행중은 페이징 없이 전량 받는 용도 |
| `hour` | HH — **목록만** 그 시간대로 (`hours` 히스토그램은 유지) |
| `q` | 발신·화자·상대·그룹명·`call_id`·`sesid`·세션키 부분일치 |
| `sort` / `order` | `start`(기본)/`turns`/`speech`/`duration`/`speakers`, `desc`(기본)/`asc` |
| `limit` / `offset` | 기본 200, 최대 1000 |

**응답 200:** `{date, from, to, hour, state, sort, order, total, live_total, items[], hours{}}`

```json
{
  "date": "2026-08-06", "hour": "", "state": "", "sort": "start", "order": "desc",
  "total": 2, "live_total": 1, "hours": { "13": 2 },
  "items": [
    { "dir": "S20260806135230852083_1", "windows": ["2026080613"],
      "sesid": "g001::csp::20260806135230852083::1", "call_id": "WCS…",
      "initiator": "+82500000001", "people": ["+82500000001", "+82500000002"],
      "speakers": ["+82500000001", "+82500000002"], "peers": [],
      "start_time": "2026-08-06T13:52:30", "end_time": "2026-08-06T13:52:48",
      "state": "ended", "segment_count": 3, "turn_count": 3, "speaker_count": 2,
      "max_concurrent": 1, "total_speech_ms": 15231, "talk_ms": 15223,
      "floor_control": "on", "floor_policy": "single", "max_talkers": 1,
      "group_key": "1", "kind": "group", "mcptt_group_id": "g001",
      "group_name": "음성그룹1", "group_type": "prearranged" }
  ]
}
```

- `dir` = **세션키** = 녹취 세션 디렉터리 이름(`S{ts}_{n}`). 세션 디렉터리 도입 이전 녹취는
  `YYYYMMDDHH` 이며 그 버킷 하나가 세션 1건으로 읽힌다.
- `windows[]` = 세션이 걸친 시간버킷. 통화가 시간을 넘겨도 행은 하나다.
- `people[]` = 참여자(세션 스냅샷 멤버 ∪ 실제 화자 ∪ 개시자), `speakers[]` = 실제 발언자,
  `peers[]` = 1:1·임시에서 개시자를 뺀 상대.
- `hours` = 시간대별 세션 수. `hour` 를 뺀 나머지 필터는 반영한다 — "이 시간대를 고르면
  몇 건이 남나" 가 이동의 근거이기 때문.
- `live_total` = 필터 적용 후 진행중 세션 수 (`state` 필터 이전 값).
- 상세·floor·flow·녹취는 같은 세션키로 조회한다:
  `/api/v1/ptt/history/{group_key}/{dir}[/floor|/flow]`,
  녹취 id = `ptt/{group_key}/{YYYY}/{MM}/{DD}/{HH}/{dir}`
  (이어지는 버킷은 서버가 `seq` 로 찾아 붙인다 — 콘솔은 버킷을 몰라도 된다).

> **읽기 모델.** 이 엔드포인트의 출처는 세션 인덱스
> (`{ServiceLogDir}/ptt/index/YYYYMMDD.jsonl`, OAM `services/ptt_index`)다. 녹취
> 디렉터리가 정본이고 인덱스는 파생물이라 지우면 다시 만들어진다. `oam.json` 의
> `PttIndex.Enabled=false` 면 종전처럼 녹취를 직접 스캔한다(되돌리기 경로).
> 진행중 세션은 인덱스에 없다 — 종료돼야 확정되므로 `state/ptt/*.json` 에서 실시간 도출한다.

### 11.2 호별 메시지 Flow 조회

```
GET /api/v1/flow/{call_id}?date=YYYY-MM-DD&call_type=volte
```

**응답 200:** 노드 그룹별 FlowMessage 배열.
```json
{
  "call_id": "abc123",
  "date": "2026-07-15",
  "nodes": {
    "csp": [
      {
        "ts": "14:30:00.123456",
        "from": "ue_o", "to": "csp",
        "proto": "SIP", "label": "INVITE",
        "node": "csp", "nodeId": "csp_01",
        "mid": "1", "seq": 42, "iface": "sip",
        "sesid": "+821001::csp::20260715143000123::1", "subid": "abc123"
      }
    ],
    "cmp": [ ]
  }
}
```

- `nodeId` = **기록 주체 system_id** (flow 파일 소유자, 읽기 시 파일명에서 파생).
  같은 CSP↔CMP 제어 메시지가 CSP 기록분(TX)·CMP 기록분(RX) 두 줄로 나타나며,
  콘솔은 nodeId 로 모듈 컬럼·노드 필터·TX/RX 를 구분한다.
- `mid` = trans_id(JSON)/CSeq(SIP), `seq`+`iface` = 원문 역조회 키 (11.3).

### 11.3 메시지 원문(body) 조회

```
GET /api/v1/flow/body?date=YYYY-MM-DD&hour=HH&seq=42&iface=cmp&node=csp&ts=14:30:00.123456&sesid=...&mid=3052&dir=TX
```

- `seq` 줄을 읽되 `sesid` 로 검증하고, 불일치 시 같은 5분 버킷에서
  `sesid`+`mid`(trans_id)+`dir`(TX/RX)+`ts` 로 재검색해 복원한다
  ([flow_logging.md](../design/features/flow_logging.md) 원문 역조회 규칙).
- legacy: `ts`+`dir`(+`proto`) 만으로도 조회 가능.

**응답 200:** `{"body": "<원문 전체>"}`

### 11.4 사용자별 메시지 이력

```
GET /api/v1/flow/user?user=+821001&date=YYYY-MM-DD
```

해당 날짜에 사용자가 관여한 전 메시지 흐름 (REGISTER/SUBSCRIBE/호 처리/CSC HTTPS 포함).
응답 형식은 11.2 와 동일한 `nodes` 구조.

---

## 12. 녹취 API (`/api/v1/recordings`)

### 12.1 녹취 목록

```
GET /api/v1/recordings?call_type=voip&date_from=2026-04-13
Authorization: Bearer <token>
```

**응답 200:**
```json
{
  "items": [
    {
      "id": 1,
      "call_id": "abc123",
      "call_type": "voip",
      "caller": "+821001",
      "callee": "+821002",
      "status": "ready",
      "has_video": false,
      "created_at": "2026-04-13T14:30:00"
    }
  ]
}
```

### 12.2 녹취 파일 다운로드

```
GET /api/v1/recordings/{id}/audio   → audio/mpeg (MP3)
GET /api/v1/recordings/{id}/video   → video/mp4
```

---

## 13. 검증 API (`/api/v1/verification`)

6단계 (S1~S6) 파이프라인 실행/이력 엔드포인트. 콘솔 메뉴 `/release/verify` ·
`/release/verify-history` 가 사용. 자세한 절차는 `VERIFICATION_PROCESS.md`.

| Method | Path | 용도 |
|---|---|---|
| GET | `/verification/stages` | 6단계 트리 (RESET 제외) + 항목 정의 |
| GET | `/verification/stages/<N>/report` | stage N 의 최근 결과 |
| GET | `/verification/stages/<N>/reports` | stage N 의 회차 리스트 |
| POST | `/verification/stages/<N>` | stage N 실행. body `{async, target, inject_fail}` |
| POST | `/verification/run` | 임의 items / preset 실행. body `{items, preset, async, target, inject_fail, only_children}` |
| GET | `/verification/jobs/<id>` | 비동기 job 진행 상태 + items_progress + stage_gate |
| GET | `/verification/runs?days=&scope=&verdict=&limit=` | 회차 이력 |
| GET | `/verification/runs/<id>` | 회차 + 항목 결과 + manifest hash |
| GET | `/verification/runs/stats?days=N` | 종합 + scope 별 + 시계열 |
| GET | `/verification/active` | 현재 실행 중인 CLI/backend job (live_store) |
| GET | `/verification/env` | host / git_branch / git_sha / pkg_manifest_hash |
| GET | `/verification/items` | 항목 메타 (id/name/stage/depends_on) |
| GET | `/verification/presets` | 프리셋 (stage1-full ~ stage6-full / pipeline-full / post-deploy / prep-reset) |

회차 정리 / webhook / target prod 등은 `VERIFICATION_MANUAL.md` 부록 참조.

## 14. 빌드/패키징 API (`/api/v1/build`)

콘솔 메뉴 `/release/package` 의 backend. 자세한 워크플로우는 `design/features/build_and_packaging.md`.

| Method | Path | 용도 |
|---|---|---|
| POST | `/build/run` | `cims.sh build [-v X.Y.Z]` 비동기 실행 |
| POST | `/build/pkg` | `cims.sh pkg [-v X.Y.Z] [--no-bump] <m>...` 비동기 실행 |
| POST | `/build/release` | 한 job 으로 build && pkg --no-bump |
| POST | `/build/clean` | `packages/*.tar.gz` + `manifest.json` 삭제 |
| GET | `/build/jobs/<id>` | 진행 상태 + stdout tail |
| GET | `/build/manifest` | `packages/manifest.json` + `_self_sha256` (immutability gate) |
| GET | `/build/packages` | manifest.packages[] (없으면 디렉토리 스캔) |
| GET | `/build/packages/<m>` | tarball octet-stream 다운로드 |

지원 모듈 (`_VALID_MODULES`): cmp/pmp/imp/cmdp/csp/psp/isp + csc/oam/oam-svc/console/cspsim/agent
(scripts/package.sh 기본 타겟과 동기. console 은 명시 시만 단독, cwrtc/phone 은 재설계 예정 — 제외).

---

## 15. 배포 관리 API

콘솔 메뉴 `/deploy/servers` (primary — 시스템/인프라: 서버·HA 그룹 인라인 편집) ·
`/deploy/packages` (패키지) 가 사용.
**N.1 패키지 = 사용자가 업로드한 배포본 tarball** (`csc.json:Packages.Dir` 보관, `agents.py::_create_package`).
**14. 빌드 API 의 packages = `cims.sh pkg` 산출물** (`build/dist/packages/`). 두 디렉토리는 코드 레벨 분리.

### 15.1 패키지 (`/api/v1/packages`)

| Method | Path | 용도 |
|---|---|---|
| GET | `/packages` | 업로드된 패키지 목록 (`meta`, `config_template` 포함) |
| GET | `/packages/{id}` | 단일 조회 |
| POST | `/packages?force=true\|false` | 업로드 (raw tarball body, `Content-Type: application/octet-stream`, `X-Filename` 헤더) |
| PUT | `/packages/{id}` | `{config_template}` 업데이트 (콘솔 [템플릿] 편집) |
| DELETE | `/packages/{id}` | 삭제 (파일은 `packages_trash/` 로 이동) |

POST 는 tarball 의 `meta.json` 에서 name/version 자동 추출. 동일 (name, version) 이면 `force=true` 시 덮어쓰기.

### 15.2 서버 (`/api/v1/agents`)

| Method | Path | 용도 |
|---|---|---|
| GET | `/agents` | 서버 목록 |
| POST | `/agents` | 등록 (body `{name, note?}`) → enrollment_token + install_command 반환 |
| GET | `/agents/{id}` | 단일 조회 |
| PUT | `/agents/{id}` | 이름/메모/service_ip_rows 변경 |
| DELETE | `/agents/{id}` | 삭제 (관련 deployment 도 cascade) |
| POST | `/agents/{id}/approve` | pending → approved |
| POST | `/agents/{id}/revoke` | 세션 폐기 |
| POST | `/agents/{id}/upgrade` | agent 바이너리 업그레이드 job 큐잉 |
| POST | `/agents/{id}/apply-ip-config` | service_ip_rows[] 을 `apply_ip_config` job 으로 큐잉 (ServiceIpPanel [적용]) |
| GET | `/agents/{id}/metrics` | 최근 리소스 메트릭 |

응답 필드 (콘솔 시스템/인프라 용):
- `interfaces` — agent heartbeat 보고 `[{name, ip, mask}]` (None = 아직 보고 전)
- `service_ip_rows` — 운영자 설정 iface→slot 매핑 `[{iface, ip, mask, slot, status?}]`
- `mounts` — 이 노드에 적용된 마운트 `[{target, source, fstype, options, mounted}]`
  (그룹 공통 마운트 선언과 대조해 멤버별 적용 여부를 판정)
- `ha_group` — HA 그룹 ref `{id, name, mode, role}` (없으면 null = standalone)

### 15.3 배포 (`/api/v1/deployments`)

| Method | Path | 용도 |
|---|---|---|
| GET | `/deployments` | 배포 목록 |
| POST | `/deployments` | 생성 (body `{agent_id, package_id, process_name, service_functions[], config?}`). `config` 는 **템플릿 선언 키만** 저장 — 제외분은 응답 `pruned_keys[]` |
| GET | `/deployments/{id}` | 단일 조회 |
| PUT | `/deployments/{id}` | 필드 업데이트 (`note`, `process_name`, ...) |
| DELETE | `/deployments/{id}` | 제거 |
| POST | `/deployments/{id}/job` | job 큐잉 (`{job_type, extra?}`) |
| GET | `/deployments/{id}/config` | scalar 설정 + 템플릿. `type=password` 필드는 **마스킹**되어 반환 |
| PUT | `/deployments/{id}/config` | scalar 설정 저장 (`{config, queue_update?}`) — **변경분 병합**. overlay 는 `config_template` 선언 키만 담는다(스키마가 계약) — 저장되지 않은 키는 응답 `pruned_keys[]` 로 반환 |
| GET | `/deployments/{id}/collection/{name}` | jsonl 컬렉션 읽기 (Agent 프록시) |
| PUT | `/deployments/{id}/collection/{name}` | jsonl 컬렉션 저장 (`{records, signal?}`) |

관리평면 노드 합류 (2번째 OAM 노드 — [oam_ha.md](../design/features/oam_ha.md) §9):

| Method | Path | 용도 |
|---|---|---|
| POST | `/ha/join-token` | **1회용** 합류 토큰 발급 (admin, body `{ttl_sec?}` 기본 900). 평문은 응답에만 |
| GET | `/ha/join-token` | 발급/사용 이력 (admin) |
| POST | `/ha/join` | 토큰으로 **그룹 공통 신원** 수령 (body `{token, node_name?}`) — 인증=토큰 |

`/ha/join` 응답에는 JwtSecret·admin 계정·그룹 CA·mTLS CA(개인키 포함) + `node_name` 지정 시
**agent enrollment token** 이 들어간다. 서버 인증서는 넘기지 않는다(합류 노드가 같은 CA 로 자기
인증서를 발급 — 개인키는 노드를 떠나지 않는다). 토큰 재사용은 409, 만료·오류는 401 이며
발급·사용은 감사 로그로 남는다.

Agent OAM 주소 재지정 (이중화 전환: 노드 IP → VIP):

| Method | Path | 용도 |
|---|---|---|
| POST | `/agents/oam-url` | **전 agent** 재지정 (body `{url?, agent_ids?[]}` — `url` 생략 시 `Server.AgentOamUrl`) |
| POST | `/agents/{id}/oam-url` | 그 agent 만 재지정 |

각 agent 는 `set_oam_url` job 에서 **새 주소로 `/health` 를 찔러 도달 확인한 뒤에만** 적용한다
(도달 불가 시 주소 미변경 + job 실패) — VIP 가 아직 없을 때 fleet 이 OAM 과 단절되는 것을 막는다.

> **400 `bind_ip_is_vip`** — 배포 설정 `Server.Ip` 에 HA 그룹 VIP 를 지정하면 거부된다. VIP 를
> 보유하지 않은 노드에서 bind 가 실패해 기동 불능 루프가 되기 때문이며, `0.0.0.0` 이 정답이다
> (접속 주소는 설정값이 아니라 접속한 IP).

> **409 `upgrade_order_active_first`** — 관리평면(`oam`/`oam-svc`) 업그레이드는 **standby
> 먼저**다. 현재 Active 노드에서 `job_type:"upgrade"` 를 요청하면 거부되고 순서를 안내한다
> (실패 시 콘솔이 사라져 롤백을 지시할 통로가 없기 때문). 버전 롤백
> (`POST /deployments/{id}/rollback`)도 같은 제약을 받는다. `force:true` 로 우회 가능.

> 오류 응답의 `detail` 은 **사람이 읽을 설명**이고 `error` 는 분기용 코드다. 콘솔은 `detail` 을
> 화면에 띄우고 코드로 분기한다(가드 409 는 사유를 보여준 뒤 `force` 재시도를 묻는다).

> **`POST /api/v1/ha-groups/{id}/shared-store/migrate`** (admin) — 관리 store 를 공유
> 마운트로 이관. body `{mount_point}`. 그룹 `shared_store` 저장 + 멤버 oam/oam-svc 배포설정
> (`CimsRuntimeDir`/`CimsRuntimeMount`) 갱신 + store 보유 노드에 `migrate_oam_store` job
> (정지→복사→config→기동) 까지 한 번에 수행하고 **202** 를 반환한다. 진행 중 OAM 이
> 재기동되므로 콘솔이 잠깐 끊긴다. 복사는 멱등이고, 실패 시 구 설정으로 되돌려 기동한다.
> 시크릿·인증서는 이관 대상이 아니다(노드 로컬 유지). 대상 경로에 이전 데이터가 있으면
> **확인 없이 덮는다** — 이관의 source 는 지금 도는 OAM 의 store 이므로 정의상 정본이다.
> 기존 대상은 삭제하지 않고 `<target>.stale-<시각>` 으로 보관한다. `source == target` 이면
> 거부한다(이관이 무의미).
> 400: `invalid_mount_point` / `not_active_standby` / `no_oam_deployment`.
> 상세: [oam_ha.md](../design/features/oam_ha.md) §9.4

> **400 `not_a_mount_point`** — `shared_store.mount_point` 가 그룹 멤버의 **실제 마운트**가
> 아니면 저장·이관이 거부된다. 판정 근거는 agent heartbeat 의 `mount_targets`(실제 마운트
> 목록, cims-managed 아닌 것 포함)이고, 응답 `nodes[]` 에 노드별 실제 마운트 목록이 실린다.
> mount guard 는 `/proc/mounts` 와 **정확히 일치**하는 경로만 통과시키므로 하위 디렉터리는
> 마운트 지점이 될 수 없다. 마운트 보고가 아직 없는 노드는 판정하지 않는다.

> **409 `store_path_not_shared`** — `PUT /api/v1/ha-groups/{id}` 로 `shared_store` **경로만**
> 저장하려 할 때, 그룹 멤버의 oam/oam-svc 배포설정 `CimsRuntimeDir` 이 아직 그 마운트 하위가
> 아니면 거부된다. 그 상태로 두면 HA 편입은 되는데 데이터는 노드별 로컬에 남아 **절체 시 빈
> 콘솔**이 된다. 응답 `conflicts[]` 에 어긋난 배포가 실린다. 해결은 위 이관 엔드포인트
> (콘솔 `이 경로로 이관`). 신규 설치처럼 이미 경로가 맞으면 그대로 저장된다.

> **409 `url_unreachable`** — `POST /api/v1/agents/oam-url` 사전 확인. OAM 이 그 주소의
> `/health` 에 도달하지 못하면 job 을 큐잉하지 않고 거부한다. 각 agent 도 도달 확인 후에만
> 적용하므로 안전하지만, VIP 가 아직 없으면 **전 agent job 이 조용히 실패**하고 콘솔은
> "큐잉" 만 알려 성공으로 오해된다(실측). 그룹을 시작해 VIP 를 띄운 뒤 다시 실행한다.

> **409 `agents_not_on_vip`** — `POST /api/v1/ha-groups/{id}/failover` 사전 점검.
> **관리평면(`oam`)을 호스팅하는 그룹에만 적용된다** — agent 는 OAM 주소 하나만 보므로 다른
> 그룹의 VIP 와 비교하면 전원이 어긋남으로 잡혀 그 그룹 절체까지 막힌다. VIP 가 아닌
> 주소로 OAM 에 보고하는 agent 가 있으면 거부된다(응답 `agents[]`에 `{agent_id,name,oam_url}`).
> 그대로 절체하면 구 Active 주소가 죽어 fleet 전체가 단절되고, 콘솔에는 전 노드 offline·모듈
> 상태 고착으로 보인다(실측). 해결은 `POST /api/v1/agents/{id}/oam-url`(서버 1대) 또는
> `POST /api/v1/agents/oam-url`(전 agent) — 콘솔 `시스템/서버 구성 > 서버 > OAM 접속 주소`.
> `force: true` 로 우회 가능. 판정 근거는 agent heartbeat 의 `oam_url` 보고값이며,
> `GET /api/v1/agents` 의 `oam_url` 과 그룹 조회의 `agents_not_on_vip[]` 로도 노출된다.

> **409 `leader_lease_precondition`** — 안전 명세가 `requires_leader_lease` 인 모듈
> (`oam`/`oam-svc`)을 **공유 store 없는 A/S 그룹에서, 상대 노드가 이미 그 모듈을 돌리는 중에**
> `start`/`restart` 하려 하면 거부된다(관리 store 가 노드마다 독립이라 VIP 위치에 따라 콘솔이
> 다른 데이터를 보여준다). `force: true` 로 우회 가능하며, 상대 노드를 먼저 정지하거나 그룹에
> 공유 store 를 설정하면 해제된다.
>
> **설치(`POST /deployments`)는 차단하지 않는다** — 대신 201 응답에 `warning` +
> `warning_code: "leader_lease_precondition"` 이 실린다(설치만 된 standby 는 HA 편입에서
> 제외돼 무해하다). 편입 제외 사유는 `GET /api/v1/ha-groups` 응답의
> `ha_excluded`(`{모듈: 사유}`)와 ha.json 서비스 엔트리에도 실린다
> ([oam_ha.md](../design/features/oam_ha.md) §6.3).

> **409 `not_lease_owner`** — 관리 store 소유권(리스)이 없는 OAM 은 **read-only** 다. 모든
> 변경 API 가 이 응답을 낼 수 있고, 조회는 정상 동작한다. 상태는
> `GET /api/v1/gateway/health` 의 `lease`/`read_only` 로 확인한다
> ([oam_ha.md](../design/features/oam_ha.md) §4.4).

배포 설정 저장 규칙:
- **병합 저장** — `config` 는 변경분이며 기존 overlay 에 병합된다. 전체 교체가 아니므로 보내지
  않은 키는 보존된다. **명시 삭제는 값 `null`**.
- **시크릿 마스킹** — 조회 응답의 `type=password` 필드는 sentinel 로 가려진다. 저장 시 그 값이
  그대로 오면 "변경 없음" 으로 무시하므로 실제 값이 덮이지 않는다.
- 위 두 규칙은 짝이다: 화면에 빈칸/마스킹으로 보이는 값을 그대로 저장해도 시크릿·런타임 경로가
  overlay 에서 사라지지 않는다(사라지면 다음 `update_config` 에서 패키지 기본값으로 회귀해
  토큰 검증 불일치 = 전면 401).

Collection API 상세는 `api/collection_api.md`. Agent 프로토콜은 `api/agent_api.md`.

### 15.4 HA 그룹 (`/api/v1/ha-groups`)

| Method | Path | 용도 |
|---|---|---|
| GET | `/ha-groups` | HA 그룹 목록 (멤버 포함) |
| POST | `/ha-groups` | 생성 (body `{name, mode, vip?, vip_mask?, auth_pass, members?[], vip_bindings?[], volume?}`) |
| GET | `/ha-groups/{id}` | 단일 조회 |
| PUT | `/ha-groups/{id}` | `{name, vip, vip_mask, auth_pass, vip_bindings, members?, volume?}` 업데이트 (mode 변경 불가) |
| DELETE | `/ha-groups/{id}` | 삭제 (멤버 cascade) |
| GET | `/ha-groups/{id}/members` | 멤버 목록 |
| POST | `/ha-groups/{id}/members` | 멤버 추가 `{agent_id, role?, priority?}` (1 agent = 1 group UNIQUE) |
| DELETE | `/ha-groups/{id}/members/{agent_id}` | 멤버 제거 |
| POST | `/ha-groups/{id}/apply` | 데이터 변경 없이 멤버에 `update_ha` job 강제 큐잉 (VIP Bindings `[↻ 재적용]`) |
| POST | `/ha-groups/{id}/apply-mounts` | 그룹 공통 마운트 — 선언(`group.mounts`) 갱신 + 멤버 fan-out. body `{mounts:[{op:'add'\|'del', target, source?, fstype?, options?}]}` → `{group_id, mounts, applied, results[{agent_id, name, ok, rc?, error?}]}`. 오프라인·실패 멤버가 있어도 선언은 갱신(콘솔이 '미적용' 표시 → 재적용으로 따라잡음) |
| GET | `/ha-groups/{id}/packages/{pkg}/sync` | 그룹×패키지 공통 설정 **정합 판정 + 표시용 실효값**(읽기 전용, operator) — `{status, reason, auto_sync, active_agent_id, compared_to, drift[], deferred[], members[{…, values:{key:{v, src}}}]}`. 판정·표시 모두 서버 소유(자동 교정과 동일 규칙). `src` = `overlay`/`injected`/`default` |
| PUT | `/ha-groups/{id}/packages/{pkg}/config` | 그룹 공통(scope=service) 설정 저장 (operator) — body `{values, target_deployment_id?, queue_update?}`. 스위치 ON=전 멤버 / OFF=target 필수 |
| PUT | `/ha-groups/{id}/packages/{pkg}/auto-sync` | 자동 동기화 스위치 (operator) — body `{enabled}`. ON 전환 시 즉시 정합 1회 |

응답 필드:
- `mode` — `active_standby` | `all_active` (standalone 은 ha_groups 미배정 agent 로 표현)
- `vip` — 단일 VIP (nullable). `vip_bindings` 사용을 권장
- `vip_bindings` — VIP slot 별 binding `[{bid, slot, ip, mask?, status?, memberIfaces?}]`
  (`memberIfaces` 는 `{agent_id: iface_name}` 매핑)

그룹/멤버/`vip_bindings` 변경 시 `update_ha` job 자동 큐잉 (각 멤버에 ha.json 분배 — keepalived 재기동).
각 group 은 한 vrrp_instance, `vip_bindings` 의 IP 들은 `virtual_ipaddress` block 의
복수 entry 로 렌더 (모두 같은 VRID 공유, member 별 interface override 가능).
