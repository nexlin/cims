# 12. CSC (CIMS Service Controller) 모듈 상세 설계

## 1. 개요

CSC는 CIMS 시스템의 관리/MCPTT 서비스 서버로, REST API 기반 가입자/그룹 관리와 3GPP MCPTT 서비스(IdMS/GMS/CMS/KMS)를 제공한다.

### 1.1 핵심 기능

| 기능 | 설명 |
|------|------|
| Admin REST API | 가입자/그룹/구독 CRUD, 일괄 등록, 이력 조회 |
| MCPTT IdMS | OAuth 2.0 PKCE 기반 단말 인증 |
| MCPTT GMS | 그룹 관리 서비스 (XCAP) |
| MCPTT CMS | 사용자 설정 관리 서비스 (XCAP) |
| MCPTT KMS | 키 관리 서비스 (MIKEY-SAKKE) |
| CSP 실시간 연동 | UDP 알림으로 가입자/그룹 변경 즉시 반영 |
| 통화 이력/녹취 | 서비스 로그 조회, 녹취 파일 재생 |
| 통계/모니터링 | 시스템 상태, 서비스 통계 |

### 1.2 서버 구성

| 서버 | 포트 | 용도 |
|------|------|------|
| Admin API Server | 4420 (HTTPS) | 관리 콘솔 REST API |
| MCPTT Service Server | 4430 (HTTPS) | 단말용 IdMS/GMS/CMS/KMS |

### 1.3 프로세스 구성

```
python3 csc_app.py [--config csc.json]
```

---

## 2. 아키텍처

### 2.1 전체 구조

```
Console UI (React)
     │
     ▼ HTTPS (4420 운영 / 4445 검증 / 4419 TB)
Admin API Server ─────────────────────────────────┐
  ├─ auth.py            (인증/JWT)                  │
  ├─ admin.py           (가입자 CRUD + Call/PTT 구독 + PTT 그룹) │
  ├─ users.py           (/me — 본인 프로필/구독)    │
  ├─ org.py             (조직 관리)                 │
  ├─ stats.py           (통계/상태)                 │
  ├─ recording.py       (녹취 관리)                 │
  ├─ verification.py    (S1~S6 검증 파이프라인)     │
  ├─ build.py           (빌드/패키지/manifest)      │
  ├─ agents.py          (Agent/Package/Deployment)  │
  ├─ agent_api.py       (Agent ↔ CSC 콜백)         │
  ├─ modules.py         (deployment 의 collection)  │
  ├─ service_control.py (로컬 서비스 ▶/■/↻ — TB)    │
  ├─ csp_runtime.py     (csp jsonl 런타임 설정)     │
  └─ services/flow_logger.py (통화 이력 + Flow)     │
                                                    │
MCPTT UE (단말)                                     │
     │                                              │
     ▼ HTTPS (4430)                                 │
MCPTT Service Server                                │
  ├─ IdMS (인증)                                    │
  ├─ GMS  (그룹)                                    │
  ├─ CMS  (설정)                                    │
  └─ KMS  (키)                                      │
                                                    │
                              ┌──────────────────────┘
                              │ notify_csp() (UDP JSON)
                              ▼ CspNotify + PspNotify (USER/GROUP_CHANGED)
                         CSP / PSP (4421)
```

> 검증/시험 환경의 포트 매핑은 `VERIFICATION_PROCESS.md` 부록 B 참조.
> Admin API 의 backend handler 코드는 `csc/src/handlers/` 디렉토리.

### 2.2 핸들러 등록

각 handler 모듈이 `<NAME>_HANDLER_LIST` 를 export, `csc_app.py` 가 모두 합쳐서 Admin Server (4420/4445/4419) 에 등록:

```python
HANDLER_LIST = (
    AUTH_HANDLER_LIST +              # /api/v1/auth/*
    CIMS_ADMIN_HANDLER_LIST +        # /api/v1/users/<pid>/{call,ptt}/* + /api/v1/ptt/groups/*
    CIMS_USERS_HANDLER_LIST +        # /api/v1/users/me*
    ORG_HANDLER_LIST +               # /api/v1/organizations/*
    STATS_HANDLER_LIST +             # /api/v1/stats/*
    RECORDING_HANDLER_LIST +         # /api/v1/recordings/*
    VERIFICATION_HANDLER_LIST +      # /api/v1/verification/* (S1~S6)
    CIMS_BUILD_HANDLER_LIST +        # /api/v1/build/*  (cims.sh build/pkg/release/clean + manifest)
    AGENTS_HANDLER_LIST +            # /api/v1/agents,packages,deployments/*
    CIMS_HA_GROUPS_HANDLER_LIST +    # /api/v1/ha-groups/* (HaServicesPage primary)
    AGENT_API_HANDLER_LIST +         # /api/agent/*  (Agent → CSC enroll/heartbeat/report)
    MODULES_HANDLER_LIST +           # /api/v1/deployments/<id>/collection/*
    SERVICE_CONTROL_HANDLER_LIST +   # /api/v1/services/<name>/<start|stop|restart> (TB-CSC 만)
    CSP_RUNTIME_HANDLER_LIST +       # CSP runtime 설정 (legacy 직접 path)
    FLOW_HANDLER_LIST                # /api/v1/flow/*, /api/v1/ptt/history/*/flow
)
```

---

## 3. Admin REST API 상세

### 3.1 인증 (handlers/auth.py)

**Base:** `/api/v1/auth`

| Method | Path | 설명 | 인증 |
|--------|------|------|------|
| POST | `/login` | 로그인 → JWT 발급 | 불필요 |
| POST | `/register` | 관리자 계정 등록 | 불필요 |
| GET | `/me` | 현재 사용자 정보 | JWT |
| PUT | `/password` | 비밀번호 변경 | JWT |

**JWT 토큰:**
- 알고리즘: HS256
- 유효기간: 24시간
- Payload: `{"user_id", "email", "role", "exp"}`
- Header: `Authorization: Bearer <token>`

**로그인 요청/응답:**

```
POST /api/v1/auth/login
Content-Type: application/json

{"email": "admin@example.com", "password": "secret"}
```

```json
{
  "token": "eyJ...",
  "user": {"id": 1, "email": "admin@example.com", "role": "admin"}
}
```

### 3.2 가입자 관리 (handlers/admin.py)

**Base:** `/api/v1/users`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/users` | 가입자 목록 (구독 정보 포함) |
| POST | `/users` | 가입자 생성 |
| GET | `/users/{user_id}` | 가입자 상세 |
| PUT | `/users/{user_id}` | 가입자 수정 |
| DELETE | `/users/{user_id}` | 가입자 삭제 |
| DELETE | `/users/batch` | 가입자 일괄 삭제 |
| GET | `/users/import/template` | Excel 템플릿 다운로드 |
| POST | `/users/import` | Excel 일괄 등록 |

**가입자 생성 요청:**

```json
POST /api/v1/users
{
  "name": "홍길동",
  "email": "hong@example.com",
  "org_id": 1,
  "details": "메모"
}
```

**가입자 조회 응답:**

```json
{
  "id": 1,
  "name": "홍길동",
  "email": "hong@example.com",
  "org_id": 1,
  "voip_subscriptions": [
    {
      "id": "+821001",
      "auth_id": "1001",
      "dnd": false,
      "forward_id": "",
      "reject_ids": ["1005"]
    }
  ],
  "ptt_subscriptions": [
    {
      "id": "+82571900001",
      "auth_id": "+82571900001@ptt.csp"
    }
  ]
}
```

### 3.3 VoIP 구독 관리

**Base:** `/api/v1/users/{user_id}/call`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/call` | VoIP 구독 목록 |
| POST | `/call` | VoIP 구독 추가 |
| PUT | `/call/{msisdn}` | VoIP 구독 수정 (DND, 전환, 거부) |
| DELETE | `/call/{msisdn}` | VoIP 구독 삭제 |

**VoIP 구독 추가:**

```json
POST /api/v1/users/1/call
{
  "id": "+821001",
  "auth_id": "1001",
  "passwd": "1234"
}
```

**VoIP 구독 수정 (DND, 착신전환):**

```json
PUT /api/v1/users/1/call/+821001
{
  "dnd": true,
  "forward_id": "+821002",
  "reject_ids": ["+821005"]
}
```

### 3.4 PTT 구독 관리

**Base:** `/api/v1/users/{user_id}/ptt`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/ptt` | PTT 구독 목록 |
| POST | `/ptt` | PTT 구독 추가 |
| PUT | `/ptt/{msisdn}` | PTT 구독 수정 |
| DELETE | `/ptt/{msisdn}` | PTT 구독 삭제 |

**PTT 구독 추가:**

```json
POST /api/v1/users/1/ptt
{
  "id": "+82571900001",
  "auth_id": "+82571900001@ptt.csp",
  "passwd": "secret123"
}
```

### 3.5 PTT 그룹 관리

**Base:** `/api/v1/ptt/groups`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/groups` | 그룹 목록 |
| POST | `/groups` | 그룹 생성 |
| GET | `/groups/{group_id}` | 그룹 상세 |
| PUT | `/groups/{group_id}` | 그룹 수정 |
| DELETE | `/groups/{group_id}` | 그룹 삭제 |
| GET | `/groups/{group_id}/members` | 멤버 목록 |
| POST | `/groups/{group_id}/members` | 멤버 추가 |
| DELETE | `/groups/{group_id}/members/{user_id}` | 멤버 제거 |

**그룹 생성:**

```json
POST /api/v1/ptt/groups
{
  "id": "group_1000",
  "name": "작전 1팀",
  "video_enabled": false,
  "priority": 5,
  "encryption": false,
  "emergency_call": true,
  "members": [
    {"user_id": "+82571900001", "priority": 1},
    {"user_id": "+82571900002", "priority": 2}
  ]
}
```

**그룹 변경 시 CSP 알림:**

```python
# 그룹 CRUD 완료 후 자동 호출
notify_csp("GROUP_CHANGED", uri=group_id, action="PUT", etag=new_etag)
```

### 3.6 조직 관리 (handlers/org.py)

**Base:** `/api/v1/organizations`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/organizations` | 조직 목록 |
| POST | `/organizations` | 조직 생성 |
| GET | `/organizations/{org_id}` | 조직 상세 |
| PUT | `/organizations/{org_id}` | 조직 수정 |
| DELETE | `/organizations/{org_id}` | 조직 삭제 |

### 3.7 통화 이력/Flow (services/flow_logger.py)

**Base:** `/api/v1`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/call/logs` | 통화 이력 목록 (필터: 날짜, 유형, 번호) |
| GET | `/call/logs/{call_id}` | 통화 상세 |
| GET | `/flow/list` | Flow 목록 |
| GET | `/flow/{session_id}` | SIP 메시지 Flow 재구성 |
| GET | `/ptt/history?group_id=X` | PTT 그룹 세션 목록 |
| GET | `/ptt/history/{gid}/{session}` | PTT 세션 이벤트 (has_recording 포함) |
| GET | `/ptt/history/{gid}/{session}/flow` | PTT 세션 SIP+CMP Flow (msg_log fallback) |
| GET | `/ptt/history/{gid}/{session}/audio` | PTT 세션 녹취 재생 (raw_audio.rtp → WAV) |
| GET | `/recordings` | 녹취 목록 (파일시스템 기반, .d 디렉토리 스캔) |
| GET | `/recordings/{call_id}/audio` | VoIP 녹취 오디오 (raw_a/b.rtp → WAV 믹싱) |
| GET | `/recordings/{call_id}/video` | VoIP 녹취 영상 (raw_va/vb.rtp → MP4) |

**PTT Flow 검색 로직:**
1. `{ServiceLogDir}/YYYY/MM/DD/HH/{system_id}/{system_id}_ptt_flow.jsonl`에서 SIP 메시지 검색
2. SIP 메시지가 없으면 `{MsgLogDir}/YYYY/MM/DD/HH/{system_id}/{system_id}_sip.jsonl`에서 group_id 기반 fallback 검색
3. 세션 디렉토리의 `cmp.jsonl`에서 Floor/JOIN/LEAVE 이벤트 병합

**PTT 녹취 트랜스코딩:**
- VoIP: `raw_a.rtp` + `raw_b.rtp` → PCM 변환 → amix → `recording_mixed.wav`
- PTT: `raw_audio.rtp` (단일 파일) → PCM 변환 → `recording_mixed.wav`
- 트랜스코딩은 on-demand (최초 재생 요청 시 변환, 이후 캐시)

**이력 조회 파라미터:**

| 파라미터 | 설명 |
|----------|------|
| date_from | 시작 날짜 (YYYY-MM-DD) |
| date_to | 종료 날짜 |
| type | voip / ptt |
| caller | 발신 번호 |
| callee | 착신 번호 |
| group_id | 그룹 ID |
| page | 페이지 번호 |
| limit | 페이지 크기 |

**Flow 조회 응답:**

```json
{
  "session_id": "S20260413143256789",
  "call_ids": ["abc123", "def456"],
  "messages": [
    {
      "ts": "14:32:56.123",
      "from": "ue",
      "to": "csp",
      "method": "INVITE",
      "call_id": "abc123",
      "body": "INVITE sip:1002@csp SIP/2.0\r\n..."
    }
  ]
}
```

### 3.8 통계 (handlers/stats.py)

**Base:** `/api/v1/stats`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/health` | 시스템 상태 (CSP/CMP/DB) |
| GET | `/subscribers` | 가입자 통계 |
| GET | `/service/summary` | 서비스 통계 요약 |

**Health 응답:**

```json
{
  "csp": {"status": "running", "registered_users": 42, "active_calls": 3},
  "cmp": {"status": "running", "sessions": 3, "groups": 1},
  "db": {"status": "connected"},
  "csc": {"status": "running", "uptime": 86400}
}
```

### 3.9 녹취 (handlers/recording.py)

**Base:** `/api/v1/recordings`

| Method | Path | 설명 |
|--------|------|------|
| GET | `/recordings` | 녹취 목록 |
| GET | `/recordings/{id}/audio` | 오디오 녹취 파일 |
| GET | `/recordings/{id}/video` | 비디오 녹취 파일 |

### 3.10 검증 (handlers/verification.py)

**Base:** `/api/v1/verification`

시스템 자체 검증 엔드포인트. CSP SIP 연결, CMP 통신, DB 쿼리 등 기능별 점검.

---

## 4. MCPTT 서비스 상세 (포트 4430)

### 4.1 IdMS (Identity Management Service)

OAuth 2.0 PKCE 기반 단말 인증.

**엔드포인트:**

| Method | Path | 설명 |
|--------|------|------|
| POST | `/idms/authreq` | Authorization Request (code_challenge) |
| POST | `/idms/tokenreq` | Token Request (code_verifier) |
| GET | `/idms/introspect` | Token Introspection |

**인증 흐름:**

```
UE                          IdMS (CSC:4430)
 │                            │
 │─── POST /idms/authreq ────→│  (client_id, redirect_uri,
 │    code_challenge,          │   code_challenge_method=S256)
 │    code_challenge_method    │
 │                            │
 │←── 302 redirect ──────────│  (auth_code)
 │                            │
 │─── POST /idms/tokenreq ──→│  (auth_code, code_verifier)
 │                            │
 │←── access_token, ─────────│  (JWT, expires_in, refresh_token)
 │    refresh_token            │
```

**토큰 구조 (JWT):**

```json
{
  "sub": "+82571900001",
  "iss": "cims-idms",
  "aud": "mcptt-client",
  "exp": 1713024000,
  "mcptt_id": "sip:+82571900001@ptt.csp",
  "org_id": "org_001"
}
```

### 4.2 GMS (Group Management Service)

XCAP 기반 그룹 관리.

**엔드포인트:**

| Method | Path | 설명 |
|--------|------|------|
| GET | `/org.openmobilealliance.groups/users/{mcptt_id}/...` | 그룹 목록/상세 |
| PUT | `/org.openmobilealliance.groups/users/{mcptt_id}/...` | 그룹 수정 |
| DELETE | `/org.openmobilealliance.groups/users/{mcptt_id}/...` | 그룹 삭제 |

**그룹 목록 응답 (JSON):**

```json
{
  "groups": [
    {
      "group_id": "group_1000",
      "name": "작전 1팀",
      "members": [
        {"mcptt_id": "sip:+82571900001@ptt.csp", "priority": 1},
        {"mcptt_id": "sip:+82571900002@ptt.csp", "priority": 2}
      ]
    }
  ]
}
```

**그룹 상세 응답 (OMA POC XML):**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service">
  <list-service uri="sip:group_1000@ptt.csp">
    <display-name>작전 1팀</display-name>
    <list>
      <entry uri="sip:+82571900001@ptt.csp">
        <display-name>사용자1</display-name>
      </entry>
      <entry uri="sip:+82571900002@ptt.csp">
        <display-name>사용자2</display-name>
      </entry>
    </list>
  </list-service>
</group>
```

### 4.3 CMS (Configuration Management Service)

XCAP 기반 사용자 프로파일/서비스 설정 관리.

**엔드포인트:**

| Method | Path | 설명 |
|--------|------|------|
| GET | `/org.3gpp.mcptt.user-profile/users/{mcptt_id}/...` | 사용자 프로파일 |
| GET | `/org.3gpp.mcptt.service-config/users/{mcptt_id}/...` | 서비스 설정 |

### 4.4 KMS (Key Management Service)

MIKEY-SAKKE 기반 키 관리 (SRTP 키 교환).

**엔드포인트:**

| Method | Path | 설명 |
|--------|------|------|
| POST | `/keymanagement/identity/v1/init` | KMS 초기화 |
| POST | `/keymanagement/identity/v1/keyprov` | 키 프로비저닝 |

---

## 5. CSP 연동 (notify_csp)

### 5.1 알림 메커니즘

CSC에서 가입자/그룹 변경 시 UDP JSON으로 CSP에 즉시 알림. 알림 메시지에는 **sesid/caller/service 상관 필드**가 함께 포함되어, CSP/CMP 의 Flow 로그와 동일 sesid 로 묶인다.

**전송:**

```python
def notify_csp(event, uri="", action="", etag="",
               sesid="", caller="", service=""):
    # service 미지정 시 event 타입 기준 자동 매핑
    #   USER_CHANGED / GROUP_CHANGED → "mcptt" (PTT 구독/그룹)
    #                                   "volte" (VoIP 구독)
    #   STATS_REQUEST / CSC_RESTART   → "system"
    #   Admin API 로그                → "console"
    if not service:
        service = _derive_service(event)
    if not sesid:
        sesid = flow_logger.issue_sesid(caller or uri, module="csc")
    msg = json.dumps({
        "event": event, "uri": uri, "action": action, "etag": etag,
        "sesid": sesid, "caller": caller, "service": service,
    })
    sock.sendto(msg.encode(), (CSP_IP, CSP_PORT))  # 4421
```

전체 필드 규격은 [../features/flow_logging.md](./../features/flow_logging.md) § 7 (CSC → CSP) 참고.

### 5.2 pi_http 미들웨어 (Admin API 로깅)

Admin/MCPTT API 요청을 서비스별 Flow 로그로 자동 기록하기 위해 `pi_http.http_server_controller.DynamicRouteProc` 에 pre/post hook 을 등록:

```python
DynamicRouteProc.set_request_hooks(pre=_pre_hook, post=_post_hook)
```

- `_pre_hook` : 요청 수신 시 JWT Bearer 토큰에서 caller 추출 (`handlers/auth.py::extract_token`), 요청 path prefix 를 service 로 매핑
- `_post_hook` : 핸들러 반환 후 `services/flow_logger.py::log_flow()` 호출 (body 는 `log_msg`)

| 경로 prefix | service |
|-------------|---------|
| `/api/v1/*` | `console` |
| `/idms/*` | `mcptt` |
| `/org.openmobilealliance*`, `/org.3gpp.mcptt*` | `mcptt` |
| `/keymanagement/*` | `mcptt` |

개별 핸들러는 별도의 `log_msg` 호출 없이도 자동으로 Flow/Msg 로그에 기록된다.

### 5.3 이벤트 타입

| 이벤트 | 트리거 | CSP 처리 |
|--------|--------|----------|
| `USER_CHANGED` | 가입자/구독 CUD | CspUserMap 캐시 갱신 |
| `GROUP_CHANGED` | 그룹/멤버 CUD | GroupMap reload + CMP 동기화 + GMS NOTIFY |
| `STATS_REQUEST` | 상태 조회 | CSP 통계 응답 반환 |
| `CSC_RESTART` | CSC 재시작 | DB 전체 재동기화 |

### 5.4 동기화 흐름

```
Console UI → REST API → DB 수정 → notify_csp()
                                      │
                                      ▼ (UDP 4421)
                                   CSP CCscInterface
                                      │
                    ┌─────────────────┼────────────────┐
                    │                 │                 │
              USER_CHANGED      GROUP_CHANGED     STATS_REQUEST
                    │                 │                 │
              CspUserMap         GroupMap         응답 반환
              캐시 갱신          reload              │
                                  │                   │
                           CMP 그룹 동기화            │
                           GMS NOTIFY 발송            │
```

---

## 6. 데이터베이스 스키마

### 6.1 핵심 테이블

```sql
-- 가입자 기본 정보
CREATE TABLE users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(200),
    org_id INT,
    details TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

-- VoIP 구독 (SIP 회선)
CREATE TABLE voip_subscriptions (
    id VARCHAR(50) PRIMARY KEY,      -- MSISDN (+821001)
    user_id INT NOT NULL,
    auth_id VARCHAR(100) NOT NULL,   -- SIP 인증 ID
    passwd VARCHAR(100) NOT NULL,    -- SIP 인증 비밀번호
    dnd BOOLEAN DEFAULT FALSE,
    forward_id VARCHAR(50),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- VoIP 착신거부 목록
CREATE TABLE user_rejects (
    id INT AUTO_INCREMENT PRIMARY KEY,
    subscription_id VARCHAR(50) NOT NULL,
    reject_id VARCHAR(50) NOT NULL,
    FOREIGN KEY (subscription_id) REFERENCES voip_subscriptions(id) ON DELETE CASCADE
);

-- PTT 구독
CREATE TABLE ptt_subscriptions (
    id VARCHAR(50) PRIMARY KEY,      -- MCPTT ID (+82571900001)
    user_id INT NOT NULL,
    auth_id VARCHAR(100) NOT NULL,   -- IMPI (id@domain)
    passwd VARCHAR(100) NOT NULL,
    dnd BOOLEAN DEFAULT FALSE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- PTT 그룹
CREATE TABLE ptt_groups (
    id VARCHAR(50) PRIMARY KEY,
    name VARCHAR(200) NOT NULL,
    video_enabled BOOLEAN DEFAULT FALSE,
    priority INT DEFAULT 5,
    encryption BOOLEAN DEFAULT FALSE,
    emergency_call BOOLEAN DEFAULT FALSE,
    org_code VARCHAR(50),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

-- PTT 그룹 멤버
CREATE TABLE ptt_group_members (
    id INT AUTO_INCREMENT PRIMARY KEY,
    group_id VARCHAR(50) NOT NULL,
    user_id VARCHAR(50) NOT NULL,    -- PTT subscription ID
    priority INT DEFAULT 5,
    FOREIGN KEY (group_id) REFERENCES ptt_groups(id) ON DELETE CASCADE,
    UNIQUE KEY (group_id, user_id)
);

-- 조직
CREATE TABLE organizations (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(200) NOT NULL,
    code VARCHAR(50) UNIQUE,
    parent_id INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 통화 이력은 DB 미적재. 파일 기반(SOT):
--   service_log/{volte|ptt}/YYYY/MM/DD/HH/.../<call_id>.d/call.json (VoLTE)
--   service_log/ptt/YYYY/MM/DD/HH/.../<call_id>.d/call.jsonl       (PTT, 세션 누적)
--   참여자: participants.jsonl
--   조회: GET /api/v1/call/logs (csc/src/services/flow_logger.py)
--   집계: GET /api/v1/stats/service/{volte|ptt|summary} (csc/src/handlers/stats.py)
-- 옛 voip_call_logs / ptt_call_logs 테이블은 v3(2026-04-22) DROP.
-- 전체 인벤토리는 docs/design/db_schema.md 참조.

-- 녹취 메타데이터
CREATE TABLE recordings (
    id INT AUTO_INCREMENT PRIMARY KEY,
    call_id VARCHAR(200),
    call_type VARCHAR(10),
    group_id VARCHAR(50),
    caller VARCHAR(100),
    callee VARCHAR(100),
    raw_dir VARCHAR(500),
    status VARCHAR(20) DEFAULT 'raw',  -- raw/transcoding/ready
    has_video BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- IdMS 인증 코드
CREATE TABLE auth_codes (
    code VARCHAR(200) PRIMARY KEY,
    client_id VARCHAR(200),
    user_id VARCHAR(200),
    redirect_uri VARCHAR(500),
    code_challenge VARCHAR(200),
    code_challenge_method VARCHAR(10),
    expires_at TIMESTAMP,
    used BOOLEAN DEFAULT FALSE
);

-- IdMS 리프레시 토큰
CREATE TABLE refresh_tokens (
    token VARCHAR(200) PRIMARY KEY,
    client_id VARCHAR(200),
    user_id VARCHAR(200),
    expires_at TIMESTAMP,
    revoked BOOLEAN DEFAULT FALSE
);
```

---

## 7. Console UI

### 7.1 기술 스택

- **프레임워크:** React + TypeScript
- **빌드:** Vite
- **위치:** `/cims-console/`

### 7.2 주요 페이지

| 페이지 | 경로 | 기능 |
|--------|------|------|
| 로그인 | `/login` | JWT 인증 |
| 대시보드 | `/dashboard` | 시스템 상태 개요 |
| 가입자 관리 | `/users` | CRUD + 구독 관리 |
| PTT 그룹 | `/ptt-groups` | 그룹/멤버 관리 |
| 통화 이력 | `/call-logs` | 이력 조회 + Flow 보기 |
| 녹취 관리 | `/recordings` | 녹취 목록 + 재생 |
| 서비스 상태 | `/service-status` | CSP/CMP/DB 상태 |
| 통계 | `/stats` | 서비스 통계 차트 |

---

## 8. 설정 (csc.json)

```json
{
  "Server": {
    "Ip": "0.0.0.0",
    "Port": 4420
  },
  "McpttServer": {
    "Ip": "0.0.0.0",
    "Port": 4430
  },
  "CimsDatabase": {
    "host": "127.0.0.1",
    "port": 3306,
    "user": "cims",
    "password": "cims",
    "database": "cims"
  },
  "IdMs": {
    "jwt_secret": "your-secret-key",
    "token_ttl": 3600,
    "refresh_ttl": 86400,
    "domain": "ptt.csp",
    "kms_uri": "https://kms.example.com"
  },
  "CimsAuth": {
    "jwt_secret": "admin-secret-key",
    "token_ttl": 86400
  },
  "CspNotify": {
    "Ip": "127.0.0.1",
    "Port": 4421
  },
  "MsgLogDir": "/data/msg_log",
  "ServiceLogDir": "/data/service_log",
  "Data": {
    "User": "User",
    "Group": "Group"
  }
}
```

---

## 9. 파일 구조

```
csc/
├── src/
│   ├── csc_app.py                  # 메인 엔트리포인트 (Admin 4420 + MCPTT 4430)
│   ├── handlers/                   # HTTP 라우트 핸들러
│   │   ├── auth.py                 # JWT 인증
│   │   ├── admin.py                # 가입자/Call/PTT 구독 + PTT 그룹 CRUD
│   │   ├── users.py                # /me 본인 프로필
│   │   ├── org.py                  # 조직 관리
│   │   ├── stats.py                # 통계/상태
│   │   ├── recording.py            # 녹취 목록/다운로드
│   │   ├── verification.py         # S1~S6 검증 파이프라인
│   │   ├── build.py                # cims.sh build/pkg/release/clean + manifest
│   │   ├── agents.py               # Agent/Package/Deployment CRUD (배포 메뉴)
│   │   ├── agent_api.py            # Agent → CSC enroll/heartbeat/report
│   │   ├── modules.py              # deployment 의 jsonl collection 프록시
│   │   ├── service_control.py      # 로컬 서비스 ▶/■/↻ (TB-CSC 전용)
│   │   └── csp_runtime.py          # CSP runtime 설정 (legacy 직접 path)
│   ├── services/                   # 비-HTTP 서비스
│   │   ├── flow_logger.py          # 통화 이력 + sesid 매칭 검색 + Flow API
│   │   ├── mcptt.py                # MCPTT IdMS/GMS/CMS/KMS + notify_csp/_psp
│   │   ├── config_cache.py         # csc.json 부팅 시 1회 로드
│   │   └── ...
│   ├── httpsrv/                    # HTTP 서버 프레임워크 (pre/post hook)
│   └── util/                       # 공통 유틸 (db, async, net, ...)
├── config/
│   ├── csc.json                    # 기본 설정
│   └── config_template.json        # 콘솔 설정 모달 스키마
└── packages/                       # 업로드된 배포본 tarball (Packages.Dir default)
```

---

## 10. 관련 문서

- [../features/flow_logging.md](../features/flow_logging.md) — Flow/Msg 로깅 공통 규격, sesid 발급/상속, CSC→CSP 인터페이스 필드
- [../../api/admin_api.md](../../api/admin_api.md) — REST API 명세 (가입자/검증/빌드/배포)
- [../../VERIFICATION_PROCESS.md](../../VERIFICATION_PROCESS.md) — 6단계 파이프라인 SSOT
