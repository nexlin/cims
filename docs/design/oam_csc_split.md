# OAM / CSC 분리 설계

2026-05-28 사용자 합의. 본 문서는 분리 방향·경계·인증·데이터·진행 계획을 확정한다. 실제 코드 작업은 Phase 1 부터 단계 진행.

## 배경

현재 `csc/` 단일 프로세스가 4개 책임을 통합:

1. **O&M (Operation & Management)** — Agent / HA / 배포 / 검증 / 통계 / 알림 — 운영팀 관심사
2. **가입자 관리** — VoLTE/PTT 가입자·조직 CRUD — 사업/단말팀 관심사
3. **CSC 본연 (MCPTT 신호처리)** — UE 와의 IdMS / GMS / CMS / KMS — 단말 통신
4. **공통 인프라** — Admin JWT 인증, 로그인 본인 정보

규모: `csc/src/handlers/` 13 파일 약 11,000 line, `services/mcptt.py` 1,200 line. Admin server(4420) 가 비대해진 상태. 운영팀/사업팀 배포 사이클 독립을 위해 분리 필요.

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

- agent_api.py 가 oam 책임. agent 의 `csc_url` 설정은 `oam_url` 로 갱신 (Phase 3 에서).
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

**csc 는 file_store 미사용**. 가입자 데이터는 MariaDB 가 SoT.

⚠️ 단 — `mcptt.py` 의 `save_group_to_file` 같은 잔재 함수가 있다면 Phase 1 검토 시 정리 (대부분 MariaDB 로 통합되어 있어야 함).

## 패키지 구조 (Phase 2 이후)

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

## Phase 진행 계획

### Phase 1: 코드 구조 분리 (소프트, 같은 프로세스)
- **목표**: top-level `oam/` 디렉토리 신설. `csc/src/handlers/` 의 oam 책임 파일들을 `oam/src/handlers/` 로 이동.
- 같은 binary, 같은 systemd unit (`cims-csc`) — csc 가 oam 의 handler 도 import.
- 공유 라이브러리 `admin_auth.py` 추출.
- `services/mcptt.py` 의 잔재 함수 (`notify_csp`, `audit_config_change` 등) 정리.
- **검증**: csc 정상 시작 + 4서버 verdict=healthy + 가입자 CRUD/Agent 관리 모두 동작.
- 예상 작업: 1 세션.
- 위험도: 낮음 (코드 이동만, 동작 변경 없음).

### Phase 2: 패키지 분리
- **목표**: `oam/pkg.json` 신설, `cims.sh pkg oam` 으로 `oam-X.Y.tar.gz` 빌드.
- 아직 같은 프로세스 (`cims-csc` 가 oam tarball 의 코드를 통합 실행).
- agent 가 oam 패키지를 별도 배포 단위로 등록 (csc 와 무관하게 업데이트 가능).
- 예상 작업: 1~2 세션.
- 위험도: 중간 (의존 그래프 정리).

### Phase 3: 프로세스 분리
- **목표**: `cims-oam` systemd unit + 새 포트 (4419). `cims-csc` 는 4420 + 4430.
- agent 가 oam(4419) 으로 heartbeat 전환. install_command URL 변경.
- 공유 비밀키 K 를 oam/csc config 양쪽에 동기화.
- 예상 작업: 2~3 세션.
- 위험도: 큼 (LIVE 절체 — 4 agent 모두 새 URL 로 전환, 다운타임 관리).

### Phase 4: 호스트 분리 (선택)
- **목표**: oam 호스트 (운영망), csc 호스트 (서비스망).
- 네트워크 ACL / TLS / 인증 분리.
- 예상 작업: 운영 합의 + 인프라.
- 위험도: 운영 / 인프라 의존.

## 진행 시 주의사항

- **CSC → CSP UDP notify** (`notify_csp` 함수) — 현재 가입자 변경 시 CSP 에 UDP 알림. 분리 후에도 csc 가 호출자 (가입자 CRUD 측). 영향 없음.
- **CSP → CSC 의존** — CSP 가 DB 에서 가입자 데이터 읽음. csc 분리와 무관 (DB 직접 접근).
- **admin console (`cims-console`)** — 빌드 결과물 (정적 파일). oam 의 정적 자원으로 서빙 자연스러움. Phase 3 에서 결정.
- **agent 의 cert rotation** — Phase 3 에서 agent ↔ oam 만 통신하므로 cert 도 oam 발급. 현재 csc 가 발급하는 cert 갱신 흐름은 oam 으로 이관.

## 관련

- [01_overview.md](./01_overview.md) — 전체 아키텍처
- [02_deployment.md](./02_deployment.md) — 배포 모델
- [ha_design.md](./ha_design.md) — HA 그룹 설계
- [runtime_store_design.md](./runtime_store_design.md) — file_store 도메인
