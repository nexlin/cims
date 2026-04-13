# CIMS 가입자 관리 콘솔 API 명세 및 설명서

## 1. 개요

CIMS 관리 콘솔은 REST API 기반으로 사용자, 구독(Call/PTT 번호), PTT 그룹을 관리합니다.
API 서버는 CSC(csc_pihttp)가 HTTPS 포트 4420에서 제공합니다.

**기본 URL:** `https://<서버IP>:4420/api/v1`
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
curl -k -X POST https://192.168.0.2:4420/api/v1/auth/login \
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
curl -k -X POST https://192.168.0.2:4420/api/v1/auth/register \
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
curl -k -X GET https://192.168.0.2:4420/api/v1/auth/me \
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
curl -k -X GET https://192.168.0.2:4420/api/v1/users \
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
curl -k -X POST https://192.168.0.2:4420/api/v1/users \
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
curl -k -X PUT https://192.168.0.2:4420/api/v1/users/35 \
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
curl -k -X DELETE https://192.168.0.2:4420/api/v1/users/35 \
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
| `passwd` | string | Y | - | SIP Digest 비밀번호 |
| `dnd` | boolean | N | false | 방해금지 모드 |
| `forward_id` | string | N | "" | 착신전환 번호 (E.164 형식) |

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4420/api/v1/users/1/call \
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
curl -k -X PUT "https://192.168.0.2:4420/api/v1/users/1/call/%2B821012345678" \
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
curl -k -X DELETE "https://192.168.0.2:4420/api/v1/users/1/call/%2B821012345678" \
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
curl -k -X POST https://192.168.0.2:4420/api/v1/users/1/ptt \
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
curl -k -X GET https://192.168.0.2:4420/api/v1/ptt/groups \
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
| `members` | array | N | 초기 멤버 목록 |
| `members[].user_id` | string | Y | PTT 구독 MSISDN |
| `members[].priority` | integer | Y | 우선순위 (0=최고, 숫자가 클수록 낮음) |

**curl 예시:**
```bash
curl -k -X POST https://192.168.0.2:4420/api/v1/ptt/groups \
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
curl -k -X DELETE "https://192.168.0.2:4420/api/v1/ptt/groups/%2B82571910003" \
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
curl -k -X POST "https://192.168.0.2:4420/api/v1/ptt/groups/%2B82571910001/members" \
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
curl -k -X DELETE "https://192.168.0.2:4420/api/v1/ptt/groups/%2B82571910001/members/%2B82571900005" \
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
| passwd | VARCHAR(64) | N | - | - | SIP Digest 비밀번호 |
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
| passwd | VARCHAR(64) | N | - | - | SIP Digest 비밀번호 |
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
  "csp": {"status": "running", "registered_users": 42, "active_calls": 3},
  "cmp": {"status": "running", "sessions": 3, "groups": 1,
          "rtp_ports_total": 20, "rtp_ports_used": 5},
  "db": {"status": "connected"},
  "csc": {"status": "running", "uptime": 86400}
}
```

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

---

## 11. 통화 이력/Flow API (`/api/v1/call`, `/api/v1/flow`)

### 11.1 통화 이력 조회

```
GET /api/v1/call/logs?type=voip&date_from=2026-04-13&page=1&limit=20
Authorization: Bearer <token>
```

**응답 200:**
```json
{
  "total": 150,
  "page": 1,
  "items": [
    {
      "call_id": "abc123",
      "call_type": "voip",
      "caller": "+821001",
      "callee": "+821002",
      "state": "ended",
      "invite_time": "2026-04-13T14:30:00",
      "answer_time": "2026-04-13T14:30:02",
      "end_time": "2026-04-13T14:31:15",
      "duration": 73,
      "end_reason": "normal"
    }
  ]
}
```

### 11.2 SIP 메시지 Flow 조회

```
GET /api/v1/flow/{session_id}
```

**응답 200:**
```json
{
  "session_id": "S20260413143000123",
  "call_ids": ["abc123", "def456"],
  "messages": [
    {
      "ts": "14:30:00.123",
      "seq": 1,
      "from": "ue",
      "to": "csp",
      "proto": "SIP",
      "method": "INVITE",
      "call_id": "abc123",
      "from_uri": "sip:1001@csp",
      "to_uri": "sip:1002@csp"
    }
  ]
}
```

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

시스템 구성요소별 기능 검증 엔드포인트.

```
GET /api/v1/verification/run
Authorization: Bearer <token>
```

**응답 200:**
```json
{
  "results": [
    {"test": "db_connection", "status": "pass"},
    {"test": "csp_alive", "status": "pass"},
    {"test": "cmp_alive", "status": "pass"},
    {"test": "sip_register", "status": "pass"},
    {"test": "rtp_relay", "status": "pass"}
  ]
}
```
