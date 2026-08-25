# 12. CSC (CIMS Service Controller) 모듈 상세 설계

> CSC 는 자족 독립 모듈이다([features/csc_standalone_module.md](../features/csc_standalone_module.md)). OAM(`ems/core/oam/src`)을 마운트하지 않으며, 결합은 계약(게이트웨이 HTTP + 공유 JwtSecret JWT verify + DB)만이다.
> - 통화 이력/Flow API(`services/flow_logger.py`)는 **oam-svc 소유**다. HA fan-out 인프라(sync_dispatch·sync_txn·drift_sweeper·service_registry·collection_schema·alert_log)도 oam 이 보유한다.
> - 현행 `csc/src/services/` = **mcptt · idms_storage · config_cache · file_store · ha_lookup · logger · admin_auth** (7개) + `__init__.py`.

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
| AuC (IMS AKA) | HSS/AuC 역할 — 가입자 K/OPc 암호화 보관(`AuC.Kek`), Milenage AV 발급·SQN·AUTS 재동기(`services/auc/`), CSP 내부 API `POST /internal/aka/av`(`InternalApi.Token`) — [sip_access_security.md §8.2](../features/sip_access_security.md) |
| CSP 실시간 연동 | UDP 알림으로 가입자/그룹 변경 즉시 반영 |
| 통화 이력/녹취 | 서비스 로그 조회, 녹취 파일 재생 |
| 통계/모니터링 | 시스템 상태, 서비스 통계 |

### 1.2 서버 구성

| 서버 | 포트 | 용도 |
|------|------|------|
| Admin API Server | 4421 (HTTPS) | 관리 콘솔 REST API + CSP 내부 AV API(`/internal/aka/av`, `/api/v1` 밖 — 게이트웨이 미프록시) |
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
     ▼ HTTPS (4421 운영 / 4445 검증 / 4419 TB)
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
  └─ service_control.py (로컬 서비스 ▶/■/↻ — TB)    │
                                                    │
  ※ 통화 이력 + Flow API 는 oam-svc 소유            │
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

각 handler 모듈이 `<NAME>_HANDLER_LIST` 를 export, `csc_app.py` 가 모두 합쳐서 Admin Server (4421/4445/4419) 에 등록:

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
    CIMS_HA_GROUPS_HANDLER_LIST +    # /api/v1/ha-groups/* (ServersPage 그룹 인스펙터)
    AGENT_API_HANDLER_LIST +         # /api/agent/*  (Agent → CSC enroll/heartbeat/report)
    MODULES_HANDLER_LIST +           # /api/v1/deployments/<id>/collection/*
    SERVICE_CONTROL_HANDLER_LIST     # /api/v1/services/<name>/<start|stop|restart> (TB-CSC 만)
)
# 통화 이력 + Flow API(/api/v1/flow/*, /api/v1/ptt/history/*/flow)는 oam-svc 가 서빙
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
  "volte_subscriptions": [
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

### 3.7 통화 이력/Flow (oam-svc 의 services/flow_logger.py)

> 통화 이력 + Flow API 는 oam-svc 모듈이 서빙한다. 아래 명세는 해당 API 규격이다.

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
| GET | `/health` | 시스템 상태 (CSP/CMP/DB) + 대시보드 KPI 카운트 + RTP 풀(VoIP/PTT) |
| GET | `/subscribers` | 가입자 통계 |
| GET | `/service/summary` | 서비스 통계 요약 |
| GET | `/external-systems/*` | 외부 시스템 레지스트리 (admin_api.md §10.4) |

**Health 응답** (전체 스키마는 `docs/api/admin_api.md` §10.1):

```json
{
  "health": {"csp": "up", "cmp": "up", "db": "up"},
  "csp": {"registered_users": 42, "active_calls": 3,
          "subscribers_total": 5013, "volte_registered": 42, "ptt_groups_total": 12, "...": "..."},
  "cmp": {"sessions": 3, "groups": 1,
          "rtp_ports": {"total": 100, "used": 5, "free": 95},
          "rtp_ports_ptt": {"total": 50, "used": 2, "free": 48}}
}
```

- KPI 카운트(`subscribers_total`/`volte_registered`/...)는 DB 단일 쿼리(`_get_dashboard_counts`, 3s 캐시). RTP 풀은 VoIP/PTT 분리.

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
| GET | `/idms/authreq` | Authorization Request — ① `user_name`/`user_password` 동반(자체 단말 간이형) → 200 JSON code ② 자격 없음(규격 OIDC 요청) → 200 HTML 로그인 폼 |
| POST | `/idms/authreq` | 규격 로그인 폼 제출(form-urlencoded) → 302 `redirect_uri?code&state` / 실패 200 폼 재표시 |
| POST | `/idms/tokenreq` | Token Request (code_verifier) — JSON·form-urlencoded |
| GET | `/idms/introspect` | Token Introspection |

**인증 흐름 (두 말투 병행 — `handle_auth_req` 한 핸들러 안 분기, 검증·인증·코드 발급 공유):**

```
[자체 단말 간이형]                          [규격 단말 — TS 24.482 §6.3.1 / OIDC Core §3.1.2]
UE                    IdMS (CSC:4430)      UE                              IdMS (CSC:4430)
 │─ GET /idms/authreq ──→│                   │─ GET /idms/authreq ──────────→│ (client_id, redirect_uri,
 │  user_name,user_password│                  │   (자격 없음)                  │  code_challenge S256, state…)
 │  client_id,redirect_uri│                   │←─ 200 text/html 로그인 폼 ────│ (hidden 문맥 이월, 무상태)
 │  code_challenge S256   │                   │─ POST /idms/authreq ─────────→│ (form: username/password
 │←─ 200 JSON {code,state}│                   │                                │  + hidden 문맥)
 │                        │                   │←─ 302 Location: redirect_uri?code&state (실패=200 폼+오류)
 │─ POST /idms/tokenreq ─→│ (code, verifier)  │─ POST /idms/tokenreq (form-urlencoded) ─→│
 │←─ access/id/refresh ───│                   │←─ access/id/refresh ──────────│
```

폼 입력칸 이름은 `IdMs.FormLoginField`/`IdMs.FormPasswordField`(기본 `username`/`password`),
`redirect_uri` 허용목록은 `IdMs.RedirectUriAllow`(비면 전부 허용 — 상용 전 등록·활성). 규격 대비는
[mcptt_standard_conformance.md §3](../features/mcptt_standard_conformance.md).

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

**토큰 저장 (영속성 규칙):**

- access/id 토큰 = **JWT(서명 검증)** — 서버에 저장하지 않는다. CSC 재기동과 무관하게 만료까지 유효.
- **refresh 토큰 = file_store** (`{CimsRuntimeDir}/refresh_tokens/`, auth code 도 동일 루트) — 갱신 시
  회전(rotated_to)·회수 기록. `CimsRuntimeDir` 는 **버전 무관 영속 경로**여야 한다: 버전 디렉터리나
  개발 트리 경로를 주면 업그레이드마다 저장소가 갈려 단말 refresh 가 "not found" 로 실패하고
  **전 단말 재로그인**이 필요해진다(SIP 평면은 Digest 라 무관 — CSC 평면만 죽는다).
  미설정(빈 값)이면 csc_app 이 인증서(runtime/cert)와 같은 규칙으로 설치 트리의
  `modules/csc/runtime` 을 유도한다(oam/oam-svc 와 동일). 만료·회수 토큰은 기동 60초 후 + 6시간
  주기로 삭제한다.

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
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"
  xmlns:cims="urn:cims:groupinfo:1.0">
  <list-service uri="sip:group_1000@ptt.csp">
    <display-name>작전 1팀</display-name>
    <list>
      <entry uri="sip:+82571900001@ptt.csp">
        <display-name>사용자1</display-name>
        <mcpttgi:participant-type>chair</mcpttgi:participant-type>
        <mcpttgi:user-priority>10</mcpttgi:user-priority>
        <cims:user-title>팀장</cims:user-title>
      </entry>
      <entry uri="sip:+82571900002@ptt.csp">
        <display-name>사용자2</display-name>
      </entry>
    </list>
  </list-service>
</group>
```

멤버 `<entry>` 의 이름·직함은 `users` 테이블(name/title)에서 채운다. `<cims:user-title>`(직함) 은
3GPP 미정의 필드라 CIMS 전용 네임스페이스(`urn:cims:groupinfo:1.0`) 확장으로 전달한다 —
`<entry>` 는 `##other` lax 확장을 허용하므로(TS 24.481/RFC 4826 resource-lists) 규격 적합이며,
표준 단말은 무시한다. 직함이 빈 값이면 요소를 생략한다.

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
    for ip, port, label in _notify_targets(event):   # 4421
        _send_notify(ip, port, label, msg.encode())
```

전체 필드 규격은 [../features/flow_logging.md](./../features/flow_logging.md) § 7 (CSC → CSP) 참고.

**목적지 라우팅** — 모든 이벤트를 `CspNotify`(VoLTE 시그널링) + `PspNotify`(PTT 시그널링)
양쪽으로 broadcast 하고, 두 `(IP, Port)` 가 같으면 1회로 dedup 한다. PTT-AS 가 통합 CSP
(`Roles.PTT_AS`) 인지 분리 PSP 인지에 무관하게 PTT-AS 노드가 반드시 수신하도록 하기 위한
것이다. `PspNotify.Ip` 가 비어 있으면 PSP 미설정 = CSP 만 사용.

**목적지 IP 는 CSP 가 4421/UDP 를 bind 한 IP 와 같아야 한다.** CSP 의 `CCscInterface` 는
`Setup.Sip.LocalIp` 가 명시 IP 면 wildcard 가 아닌 **그 IP 로만** bind 한다 (한 호스트에
CSP/PSP/ISP 다중 인스턴스가 공존할 때 UDP 전달 대상을 결정적으로 만들기 위한 설계).
따라서 CSP 가 특정 IP 로 bind 한 구성에서 `CspNotify.Ip` 를 `127.0.0.1` 로 두면 notify 는
목적지에 도달하지 않는다.

**도달 실패 관측** — 목적지별로 **connect 된** UDP 소켓을 유지한다. 비연결 `sendto` 는
수신 프로세스가 없어도 항상 성공해 제어평면 단절이 무기한 침묵하지만, connected 소켓은
커널이 ICMP port-unreachable 을 큐잉하므로 `send`/`recv` 에서 `ECONNREFUSED` 로 관측된다.
발송 직후 짧은 창(50ms) 동안 ICMP 회신을 확인하고, 도달 실패 시 app 로그에
`Notify 미도달 → <label>(<ip>:<port>)` ERROR 를 남긴다. 도달한 경우만 `Notify Sent` INFO.

> 도달 실패는 조용한 기능 저하로 나타난다 — 관리자 편성 변경이 즉시 반영되지 않고
> CSP 의 주기 통지원(`SyncGroupsState`, 60초 해시 비교)으로만 뒤늦게 따라잡는다.

### 5.2 pi_http 미들웨어 (Admin API 로깅)

Admin/MCPTT API 요청을 서비스별 Flow 로그로 자동 기록하기 위해 `pi_http.http_server_controller.DynamicRouteProc` 에 pre/post hook 을 등록:

```python
DynamicRouteProc.set_request_hooks(pre=_pre_hook, post=_post_hook)
```

- `_pre_hook` : 요청 수신 시 JWT Bearer 토큰에서 caller 추출 (`handlers/auth.py::extract_token`), 요청 path prefix 를 service 로 매핑
- `_post_hook` : 핸들러 반환 후 `services/logger.py` 의 Flow 기록 호출 (body 는 `log_msg`)

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

> **DB 스키마 SoT 는 [db_schema.md](../db_schema.md)** — 테이블명·키 구조·DROP 이력은 그쪽이 단일 출처다. 여기서는 상세 DDL 을 중복 기재하지 않고 CSC 가 실제 사용하는 테이블만 나열한다.

DB 는 가입자(person/VoLTE/PTT) 도메인과 조직 트리 등 **관계형이 본질적으로 필요한 데이터에만 한정**한다. 통화/녹취 이력, IdMS 인증 코드·리프레시 토큰, CSP 런타임 설정, 검증 회차 등은 모두 파일 기반(file_store)이 SoT 이며 DB 테이블이 없다 ([db_schema.md](../db_schema.md) §3·§4, [runtime_store_design.md](../runtime_store_design.md)).

### 6.1 CSC 가 사용하는 테이블

| 테이블 | 키 | 용도 |
|--------|----|------|
| `users` | `id INT AI PK` | 가입자 개인정보(name/email/org_id/details) + 콘솔 인증(login_id/password/role) |
| `volte_subscriptions` | `id VARCHAR PK`(MSISDN) | VoLTE 회선: SIP 인증, dnd/forward. `user_id` → users(CASCADE) |
| `user_rejects` | `id INT AI PK` | VoLTE 착신거부 목록. `subscription_id` → volte_subscriptions(CASCADE) |
| `ptt_subscriptions` | `id VARCHAR PK`(MCPTT ID) | MCPTT 회선: IMPI 인증. `user_id` → users(CASCADE) |
| `ptt_groups` | **`id BIGINT AI PK`**(surrogate) | PTT 그룹. `mcptt_group_id` 는 UNIQUE 식별자(키 아님). group_type(prearranged/chat/broadcast)/priority/emergency/video_enabled/require_affiliation 등 |
| `ptt_group_members` | `id INT AI PK` | 멤버. `group_id` → **ptt_groups.id(surrogate BIGINT FK)**, role(chair/participant), mcptt_id |
| `ptt_affiliations` | (group_id, user_id, client_id) | MCPTT affiliation(TS 24.379 §9). `group_id` → ptt_groups.id(CASCADE) |
| `organizations` | `id INT AI PK` | code/name/parent_id 트리. users.org_id FK 대상 |

> 주의: 구 `voip_subscriptions` 는 `volte_subscriptions` 로 rename 되었고, `ptt_groups` 의 PK 는 옛 `VARCHAR id` 가 아니라 **surrogate `BIGINT AUTO_INCREMENT`** 이며 MCPTT 그룹 식별자는 별도 `mcptt_group_id` 컬럼이다. `voip_call_logs`/`volte_call_logs`/`ptt_call_logs` 등 통화 이력 계열과 `recordings`/`recording_segments` 는 DROP 되어 파일 기반으로 대체되었다. 정확한 현행명·마이그레이션 매핑은 [db_schema.md](../db_schema.md) 를 따른다.

---

## 7. Console UI

### 7.1 기술 스택

- **프레임워크:** React + TypeScript
- **빌드:** Vite
- **위치:** `/ems/core/console/`

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
    "Port": 4421
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
    "Ip": "121.161.164.45",
    "Port": 4421
  },
  "PspNotify": {
    "Ip": "121.161.164.45",
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

### 8.1 설정 로딩 우선순위 (overlay = primary, base = optional)

`csc_app.py:load_config()` 는 두 파일을 병합한다 — 배포 계약상 **deployment overlay
가 SoT 이고 base 는 선택**이다 (`lifecycle.sh:start_csc` 포트 탐지, CSP 의
`SipServerSetup._findDeploymentConfig` 와 동일한 모델).

| 우선순위 | 파일 | 누가 만드나 | 비고 |
|---|---|---|---|
| base | `csc/config/csc.json` | `configure.sh:apply_config_template` (configure 단계) | 상용 배포본은 `build→pkg`(configure 생략)이라 **부재가 정상**. 패키지엔 `config_template.json`(스키마)만 동봉 |
| overlay (primary) | `csc/config.json` (legacy: `../config.json`) | agent 가 install 시 렌더 기록 (`cims_agent.py:_write_config_file`) | flat dot-path 키(`"Server.Port": 4421`)를 base 위에 머지. 실제 운영 설정의 SoT |

base 가 없으면 빈 dict 에서 시작해 overlay 만으로 기동한다. 따라서 상용 배포본은
agent 가 쓴 `csc/config.json` 단독으로 정상 동작해야 하며, base 부재를 이유로
빈 설정(포트 4421 default·dummy user·JWT 미설정 → 401)으로 떨어지면 안 된다.

⚠️ **overlay 는 base 를 덮는다** — OAM 은 job 디스패치 시 deployment 의 sparse overlay
(사용자가 실제로 바꾼 값)를 `config_template` 의 `default` 위에 올려 완전한 설정으로
실체화한다 (`agents.py:_materialize_deploy_config`). 즉 **콘솔에서 한 번도 만지지 않은
필드도 template default 값으로 overlay 에 실려 base 를 덮는다.** 따라서 노드 topology 에
의존하는 필드(피어 주소 등)의 template default 는 `""` 로 두어 실체화 대상에서 빠지게
하고(`_template_defaults` 는 빈 default 를 제외), 운영값은 콘솔에서 명시 입력한다.
`deploy_value`(`@CSP_IP@` 등)는 `configure.sh` 경로에서만 치환되며 OAM 배포 경로는
치환하지 않는다.

### 8.2 런타임 리로드 (SIGUSR1)

agent 의 `job_update_config` 는 overlay 를 재기록한 뒤 모듈에 **SIGUSR1** 을 보낸다.
CSC 는 `csc_app.py` 에서 핸들러를 등록해 `load_config()` → `admin_auth.init()` →
`mcptt.apply_config()` 를 다시 수행한다. `config_template` 의 `restart:false` 필드
(`CspNotify`/`PspNotify`, IdMs TTL, `Provisioning` 등)가 재기동 없이 반영되는 경로다.

- `mcptt.apply_config(config)` = 스칼라 설정만 재적용 (가입자/그룹 데이터 로드는 미포함).
  기동 시엔 `load_shared_data()` 가 먼저 호출한다.
- bind 계열(`Server.Port`/`McpttServer.Port`)과 기동 시 캡처된 값은 재기동이 필요하다.
- 핸들러가 없으면 파이썬 기본 동작(프로세스 종료)이라 `update_config` 가 CSC 를 죽인다 —
  SIGUSR1 핸들러는 선택이 아니라 배포 규약이다.

---

## 9. 파일 구조

```
csc/
├── src/
│   ├── csc_app.py                  # 메인 엔트리포인트 (Admin 4421 + MCPTT 4430)
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
│   │   └── service_control.py      # 로컬 서비스 ▶/■/↻ (TB-CSC 전용)
│   ├── services/                   # 비-HTTP 서비스 (7개)
│   │   ├── mcptt.py                # MCPTT IdMS/GMS/CMS/KMS + notify_csp/_psp
│   │   ├── idms_storage.py         # IdMS auth_code/refresh_token 저장
│   │   ├── config_cache.py         # csc.json 부팅 시 1회 로드
│   │   ├── file_store.py           # 파일 기반 저장
│   │   ├── ha_lookup.py            # HA 멤버 조회
│   │   ├── logger.py               # 비동기 배치 로그 writer
│   │   └── admin_auth.py           # admin JWT verify
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
