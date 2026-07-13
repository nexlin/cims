# OAM / CSC 분리 설계

본 문서는 OAM/CSC 분리의 방향·경계·인증·데이터 모델을 확정한다.

> ⚠️ **구현 메커니즘은 [features/csc_standalone_module.md](features/csc_standalone_module.md) 가 정본** —
> 본 문서의 **결정(경계·인증 모델·역할 범위·운영 토폴로지)은 유효**하나, 모듈 간 코드 공유 방식은
> "공유 라이브러리 sys.path 마운트 + handlers/services PEP420 namespace 병합" 이 아니라
> **계약 기반(게이트웨이 HTTP + 공유 JwtSecret JWT verify + DB 스키마) + 각 모듈 자체 인프라 vendoring** 이다. 현행:
> - csc 는 ems/core/oam/src 를 **마운트하지 않음**. base(oam)·oam-svc 도 csc/src 를 **마운트하지 않음**.
> - 각 모듈이 자기 services/httpsrv/util 을 자체 보유. csc = {mcptt, idms_storage, config_cache, file_store,
>   ha_lookup, logger, admin_auth} 7개. (csc 는 idms/config 스냅샷용으로 자체 file_store 사용)

## 배경

현재 `csc/` 단일 프로세스가 4개 책임을 통합:

1. **O&M (Operation & Management)** — Agent / HA / 배포 / 검증 / 통계 / 알림 — 운영팀 관심사
2. **가입자 관리** — VoLTE/PTT 가입자·조직 CRUD — 사업/단말팀 관심사
3. **CSC 본연 (MCPTT 신호처리)** — UE 와의 IdMS / GMS / CMS / KMS — 단말 통신
4. **공통 인프라** — Admin JWT 인증, 로그인 본인 정보

규모: `csc/src/handlers/` 13 파일 약 11,000 line, `services/mcptt.py` 1,200 line. Admin server(4421) 가 비대해진 상태. 운영팀/사업팀 배포 사이클 독립을 위해 분리 필요.

## 결정 (4 의문점)

### 1. O&M 범위

| handler | 분류 | 비고 |
|---|---|---|
| agents.py | **oam** | Agent 레지스트리 (2418 line) |
| agent_api.py | **oam** | Agent ↔ 서버 통신 |
| ha_groups.py | **oam** | HA 그룹 lifecycle |
| modules.py | **oam** | 모듈 overlay |
| build.py | **oam** | 빌드/패키지 |
| service_control.py | **oam** | 프로세스 제어 |
| verification.py | **oam** | S1~S6 검증 |
| alerts.py | **oam** | 알람 |
| stats.py | **oam** | 모니터링 통계 (agent 메트릭 포함) |
| recording.py | **oam** | 녹취 조회 (운영 도구) |
| admin.py | **csc** | 가입자(VoLTE/PTT) CRUD via MariaDB |
| org.py | **csc** | 조직 |
| services/mcptt.py | **csc** | IdMS / GMS / CMS / KMS — 단말 통신 |
| auth.py | **oam** | Admin JWT 발급 (관리자 콘솔 로그인) |
| users.py | **oam** | 로그인 본인 정보 |

**공유 라이브러리** (`services/admin_auth.py`):
- `verify_admin_jwt(token, secret) → claims` — oam, csc 모두 import (csc 는 검증만)
- `issue_admin_jwt(user_id, secret) → token` — oam 만 사용 (auth.py 의 /login)

### 2. UE 인증 — CSC 본연 그대로

분리와 무관하게 UE 인증 (3GPP TS 33.180 OAuth 2.0 PKCE) 은 CSC 그대로:

```
UE  ──OAuth PKCE──→  CSC IdMS (4430)  → access_token (scope: mcptt.user-profile 등)
UE  ──MCPTT API──→   CSC GMS/CMS/KMS (4430)
```

mcptt.py 의 `handle_auth_req` / `handle_token_req` / `handle_token_introspect` 그대로 유지. UE 가 발급받는 access_token 과 운영자 admin JWT 는 완전히 별개 도메인.

### 3. Admin 인증 — 모델 1 (oam SSO + 공유 비밀키)

```
관리자 → admin console (브라우저)
       → oam 의 /api/v1/auth/login (login_id+password)
       → JWT (HS256, 비밀키 K 로 서명, TTL 1d) 발급
       → admin console 이 JWT 보관 (localStorage)
       → oam API 호출 (Authorization: Bearer <JWT>) ← oam 이 K 로 검증
       → csc API 호출 (Authorization: Bearer <JWT>) ← csc 도 K 로 로컬 검증 (oam 호출 없음)
```

- 비밀키 K 는 oam/csc 양쪽 config 동일 값. 1회성 배포 동기화.
- 토큰 revoke 어려움 → TTL 짧게 (1일).
- 외부 IdP (keycloak 등) 의존 없음.
- 공유 라이브러리: `verify_admin_jwt(token, K)` 만 (작음).

### 4. Agent 통신 — B1 (agent → oam 만)

```
Agent  ──heartbeat/job pickup──→  oam (4419)
Agent  ──(없음)──→  csc                   ← 통신 안 함
```

- agent_api.py 가 oam 책임. agent 의 설정 키는 `oam_url`.
- install_command 의 URL 도 oam 으로 변경.
- csc 는 단말(UE) 통신만, agent 와 무관.

### 5. file_store — D1 (oam SoT)

`{CimsRuntimeDir}/` 의 도메인 owner:

| 도메인 | owner | 비고 |
|---|---|---|
| agents | oam | Agent 레지스트리 |
| instances | oam | 모듈 인스턴스 |
| packages | oam | 패키지 메타 + tarball 경로 |
| deployments | oam | 배포 레코드 |
| jobs | oam | job 큐 |
| metrics | oam | 시계열 메트릭 (JSONL) |
| ha_groups | oam | HA 그룹 |
| csp_config_audit | oam | CSP 설정 변경 audit JSONL |
| auth_codes | oam | OAuth PKCE 코드 (Admin JWT 흐름, IdMS 와 별개) |
| refresh_tokens | oam | Admin JWT refresh |
| sip_listener / sip_trunk / routing_rule / routing_access_list / sip_service | (retired) | csp_runtime 폐기 후 잔존 코드 |

가입자 데이터는 MariaDB 가 SoT. csc 는 idms/config 스냅샷용으로 자체 file_store 를 사용한다.

## 패키지 구조

```
cims/                                    cims/
├─ csp/                                  ├─ csp/        (변동 없음)
├─ cmp/                                  ├─ cmp/        (변동 없음)
├─ csc/   ─────  (분리 후)   ─────►      ├─ csc/        ← UE 통신만 (IdMS/GMS/CMS/KMS + 가입자 CRUD)
│   src/handlers/                        │   src/
│     agents.py                          │     handlers/
│     ha_groups.py        ►              │       admin.py        ← 가입자 CRUD
│     admin.py                           │       org.py
│     mcptt.py                           │     services/
│     ...                                │       mcptt.py        ← UE OAuth/GMS/CMS/KMS
                                         │       admin_auth.py   ← 공유 (verify only)
                                         │   pkg.json (csc-X.Y)
                                         │
                                         ├─ oam/        ← O&M (top-level, 신규)
                                         │   src/
                                         │     handlers/
                                         │       agents.py
                                         │       agent_api.py
                                         │       ha_groups.py
                                         │       modules.py
                                         │       build.py
                                         │       service_control.py
                                         │       verification.py
                                         │       alerts.py
                                         │       stats.py
                                         │       recording.py
                                         │       auth.py         ← Admin JWT 발급
                                         │       users.py        ← 본인 정보
                                         │     services/
                                         │       admin_auth.py   ← 공유 (verify + issue)
                                         │       file_store.py
                                         │       ...
                                         │   pkg.json (oam-X.Y)
```

**디렉토리 결정**: `csc/oam/` 같은 nested 가 아닌 **top-level `oam/`**. csp/cmp 와 동등한 모듈 위상.

## 운영 토폴로지

```
┌─────────────────────┐         ┌─────────────────────┐
│  oam host           │         │  csc host           │
│  cims-oam (4419)    │         │  cims-csc (4430)    │
│  ─────────────────  │  공유   │  ─────────────────  │
│   • Agent ↔ oam     │  비밀키 │   • UE OAuth (IdMS)  │
│   • Admin JWT 발급   │   K     │   • GMS/CMS/KMS      │
│   • file_store SoT  │ ◄─────► │   • 가입자 CRUD       │
│                     │         │   • DB SoT (가입자)   │
└─────────────────────┘         └─────────────────────┘
       ▲                                  ▲
       │ HTTPS (Admin JWT)                │ MCPTT (access_token)
       │                                  │
   admin console                       UE (단말)
   (운영팀)                            (가입자)
```

소규모 환경에선 한 호스트에 둘 다 가능. 분리 시:
- oam 호스트: 인터넷 격리 가능 (내부 운영망만)
- csc 호스트: 단말 접근 허용 (서비스망)

## 현행 운영 모델

분리는 코드 구조 → 패키지 → 프로세스 순으로 진행되어, oam(O&M) 과 csc(가입자/MCPTT)
가 별도 패키지·별도 프로세스로 운영된다.

- **oam** (top-level `oam/`, `oam_app.py`) — admin server **4419**. Agent/HA/배포/검증/통계/알림.
  4서버 agent heartbeat 의 endpoint. ha_capability=active_standby.
- **csc** (`csc_app.py`) — admin server **4421** + mcptt server **4430**. 가입자/조직 CRUD + auth(관리자 로그인) + IdMS/GMS/CMS/KMS(UE 통신).
- **공유 비밀키 K** — oam/csc config 양쪽 동일 값. csc 는 admin JWT 를 로컬 검증만(발급은 oam).
- **agent URL** — agent 는 oam 만 호출(`--oam-url`, 구 `--csc-url` alias 호환). `Server.AgentOamUrl` config key 우선.
- private 환경 자족 — 각 모듈이 Python 의존성을 `vendor/` 에 동봉(pip 의존 0).

```
management host:
  ┌─────────────────────────┐    ┌──────────────────────────────┐
  │ cims-oam (oam_app.py)   │    │ cims-csc (csc_app.py)        │
  │  port 4419              │    │  port 4421 admin + 4430 mcptt│
  │  Agent/HA/배포/검증/통계   │    │  가입자/조직/auth/mcptt        │
  └─────────────────────────┘    └──────────────────────────────┘
            ▲                              ▲
            │ heartbeat (4419)              │ MCPTT (4430) — UE 통신
            │                              │
       각 노드 agent                     UE 단말 (PTT/VoLTE 가입자)
```

### NIC role 모델 + VIP slot 자동 매핑

- **Mgmt.Cidr** (oam.json) — oam 운영의 mgmt 대역 기준선. oam_app.py 시작 시 로드 + AgentOamUrl host IP 검증.
- **interface role** (agent.interfaces[].role) — mgmt 는 agent 의 `detect_mgmt_ip`(oam_url outgoing IP) + Mgmt.Cidr 검증으로 자동, service/internal 은 admin 명시(`PUT /api/v1/agents/<id>/interface-roles`). overrides 는 heartbeat 마다 정규화.
- **HA VIP slot ↔ role 자동 매핑** (`ha_groups.py::_render_ha_for_agent`) — vip_bindings.slot 이 mgmt/service/internal 이고 memberIfaces 미지정 시 agent.interfaces 의 role 매칭 NIC 자동 추론. `vips[].dev` 명시로 다중 망 multi-VIP 를 한 vrrp_instance 에서 dev 분리.

### deployment 정합

- **process_name 자동 추론** (`agents.py::_create_deployment`) — POST 시 process_name 누락 + package_name 있으면 `process_name = package_name` 자동 채움. (`cims-svc start` 의 default 'all' fallback 으로 인한 single-module install 실패 차단.)
- **agent safety net** (`cims_agent.py::job_process_control`) — svc 빈 경우 명시 에러(`process_name 누락 — deployment.process_name 필드 필수`).

### 호스트 분리 (선택)
- **목표**: oam 호스트 (운영망), csc 호스트 (서비스망).
- 네트워크 ACL / TLS / 인증 분리.
- 위험도: 운영 / 인프라 의존.

## 주의사항

- **CSC → CSP UDP notify** (`notify_csp` 함수) — 가입자 변경 시 CSP 에 UDP 알림. csc 가 호출자 (가입자 CRUD 측).
- **CSP → CSC 의존** — CSP 가 DB 에서 가입자 데이터 읽음. csc 분리와 무관 (DB 직접 접근).
- **admin console (`ems/core/console`)** — 빌드 결과물 (정적 파일). oam 의 정적 자원으로 서빙.
- **agent 의 cert rotation** — agent ↔ oam 만 통신하므로 cert 도 oam 발급.

## 관련

- [01_overview.md](./01_overview.md) — 전체 아키텍처
- [02_deployment.md](./02_deployment.md) — 배포 모델
- [ha_design.md](./ha_design.md) — HA 그룹 설계
- [runtime_store_design.md](./runtime_store_design.md) — file_store 도메인
